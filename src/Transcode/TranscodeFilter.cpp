/**
 * 单输入滤镜图实现，见 TranscodeFilter.h 的说明。
 */
#include "TranscodeFilter.h"

#include <cstring>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixfmt.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
}

#include "Util/logger.h"

using namespace toolkit;

namespace mediakit {

TranscodeFilter::~TranscodeFilter() {
    close();
}

void TranscodeFilter::close() {
    if (_graph) {
        avfilter_graph_free(&_graph);
    }
    _graph = nullptr;
    _src = nullptr;
    _sink = nullptr;
    _logo_src = nullptr;
    if (_logo_frame) {
        av_frame_free(&_logo_frame);
    }
}

/**
 * 读一张图片并转换成与主链一致的像素格式（可选缩放到指定宽度）。
 * 失败返回 nullptr 并打 ErrorL（调用方据此停用 logo，但主链仍可继续）。
 */
AVFrame *TranscodeFilter::loadLogo(int width, int height, int fmt) {
    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, _logo_path.data(), nullptr, nullptr) < 0 || !fmt_ctx) {
        ErrorL << "transcode logo: 打不开图片 " << _logo_path;
        return nullptr;
    }
    AVFrame *ret = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *raw = nullptr;
    SwsContext *sws = nullptr;
    auto cleanup = [&]() {
        if (sws) sws_freeContext(sws);
        av_frame_free(&raw);
        av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        av_frame_free(&ret);
    };
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        ErrorL << "transcode logo: 解析图片失败 " << _logo_path;
        cleanup();
        return nullptr;
    }
    int idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx < 0) {
        ErrorL << "transcode logo: 图里没有视频帧 " << _logo_path;
        cleanup();
        return nullptr;
    }
    auto par = fmt_ctx->streams[idx]->codecpar;
    auto codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        ErrorL << "transcode logo: 没有可用解码器 " << _logo_path;
        cleanup();
        return nullptr;
    }
    dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    if (!dec || avcodec_open2(dec, codec, nullptr) < 0) {
        ErrorL << "transcode logo: 解码器打开失败 " << _logo_path;
        cleanup();
        return nullptr;
    }
    pkt = av_packet_alloc();
    raw = av_frame_alloc();
    ret = av_frame_alloc();
    if (!pkt || !raw || !ret) {
        cleanup();
        return nullptr;
    }
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != idx) {
            av_packet_unref(pkt);
            continue;
        }
        int sent = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (sent < 0) {
            break;
        }
        if (avcodec_receive_frame(dec, raw) >= 0) {
            break;
        }
    }
    if (!raw->data[0]) {
        ErrorL << "transcode logo: 图片解码不出帧 " << _logo_path;
        cleanup();
        return nullptr;
    }

    // logo 尺寸：指定宽度就等比缩放，否则用原图；高度上限不超过画面的 1/3
    int lw = _logo_width > 0 ? _logo_width : raw->width;
    int lh = raw->height;
    if (_logo_width > 0 && raw->width > 0) {
        lh = (int) ((double) raw->height * lw / raw->width);
    }
    if (lh > height / 3) {
        lh = height / 3;
        lw = (int) ((double) raw->width * lh / raw->height);
    }
    if (lw % 2) ++lw;
    if (lh % 2) ++lh;
    if (lw <= 0 || lh <= 0) {
        cleanup();
        return nullptr;
    }

    ret->format = fmt;
    ret->width = lw;
    ret->height = lh;
    ret->pts = 0;
    if (av_frame_get_buffer(ret, 32) < 0) {
        cleanup();
        return nullptr;
    }
    sws = sws_getContext(raw->width, raw->height, (enum AVPixelFormat) raw->format,
                         lw, lh, (enum AVPixelFormat) fmt,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        ErrorL << "transcode logo: sws 初始化失败";
        cleanup();
        return nullptr;
    }
    sws_scale(sws, raw->data, raw->linesize, 0, raw->height, ret->data, ret->linesize);
    InfoL << "transcode logo loaded: " << _logo_path << " " << raw->width << "x" << raw->height
          << " -> " << lw << "x" << lh << " (" << width << "x" << height << " canvas)";
    // 成功路径：与 cleanup() 相同的释放，但不释放 ret 本身
    if (sws) sws_freeContext(sws);
    av_frame_free(&raw);
    av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    return ret;
}

bool TranscodeFilter::initLogoGraph(int width, int height, int fmt, int num, int den) {
    AVFrame *logo = loadLogo(width, height, fmt);
    if (!logo) {
        return false;
    }
    _graph = avfilter_graph_alloc();
    if (!_graph) {
        av_frame_free(&logo);
        return false;
    }
    char args[256];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             width, height, fmt, num, den);
    if (avfilter_graph_create_filter(&_src, avfilter_get_by_name("buffer"), "in", args, nullptr, _graph) < 0) {
        ErrorL << "transcode logo: create main buffersrc failed";
        av_frame_free(&logo);
        close();
        return false;
    }
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             logo->width, logo->height, fmt, num, den);
    if (avfilter_graph_create_filter(&_logo_src, avfilter_get_by_name("buffer"), "logo", args, nullptr, _graph) < 0) {
        ErrorL << "transcode logo: create logo buffersrc failed";
        av_frame_free(&logo);
        close();
        return false;
    }

    // 叠加位置：负数表示「距右边/下边 |x| 像素」，用表达式让滤镜自己按画面尺寸算
    auto pos = [](int v, const char *size, const char *edge) -> std::string {
        char buf[64];
        if (v < 0) {
            snprintf(buf, sizeof(buf), "%s-%s-%d", size, edge, -v);
        } else {
            snprintf(buf, sizeof(buf), "%d", v);
        }
        return buf;
    };
    std::string graph = _desc.empty() ? std::string() : (_desc + ",");
    // 表达式里 W/H 是主画面尺寸，w/h 是 logo 尺寸：距右边缘 = W-w-|x|
    graph += "overlay=x=" + pos(_logo_x, "W", "w") + ":y=" + pos(_logo_y, "H", "h") + ":eof_action=repeat";

    AVFilterInOut *open_in = nullptr;
    AVFilterInOut *open_out = nullptr;
    int ret = avfilter_graph_parse2(_graph, graph.data(), &open_in, &open_out);
    if (ret < 0 || !open_in || !open_in->next || !open_out) {
        ErrorL << "transcode logo: 解析滤镜串失败（" << graph << "）: " << ret;
        avfilter_inout_free(&open_in);
        avfilter_inout_free(&open_out);
        av_frame_free(&logo);
        close();
        return false;
    }
    // open_in 的第一个是主链入口，第二个是 overlay 的第二路输入
    avfilter_link(_src, 0, open_in->filter_ctx, open_in->pad_idx);
    avfilter_link(_logo_src, 0, open_in->next->filter_ctx, open_in->next->pad_idx);
    if (avfilter_graph_create_filter(&_sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, _graph) < 0) {
        ErrorL << "transcode logo: create buffersink failed";
        avfilter_inout_free(&open_in);
        avfilter_inout_free(&open_out);
        av_frame_free(&logo);
        close();
        return false;
    }
    avfilter_link(open_out->filter_ctx, open_out->pad_idx, _sink, 0);
    avfilter_inout_free(&open_in);
    avfilter_inout_free(&open_out);

    if (avfilter_graph_config(_graph, nullptr) < 0) {
        ErrorL << "transcode logo: avfilter_graph_config failed";
        av_frame_free(&logo);
        close();
        return false;
    }
    // 保留 logo 帧，之后**每个主帧喂一次**。
    // 不能在建图后立刻发 EOF 指望 eof_action=repeat 复用它：实测那样 overlay 在第一帧
    // 到达前就结束，输出与输入完全一样（看起来像「logo 没生效」）。
    // Keep the logo frame and push it once per main frame. Sending EOF right after init
    // and relying on eof_action=repeat makes overlay finish before the first main frame
    // arrives, so the output is identical to the input.
    _logo_frame = logo;

    auto out_link = _sink->inputs[0];
    if (out_link->w != width || out_link->h != height || out_link->format != fmt) {
        ErrorL << "transcode logo: 输出 " << out_link->w << "x" << out_link->h
               << " fmt=" << out_link->format << " 与编码器 " << width << "x" << height
               << " fmt=" << fmt << " 不一致";
        close();
        return false;
    }
    InfoL << "transcode logo ready: " << _logo_path << " at x=" << _logo_x << " y=" << _logo_y
          << " (" << graph << ")";
    return true;
}

bool TranscodeFilter::init(const std::string &desc, int width, int height, int fmt, int num, int den) {
    close();
    _desc = desc;
    _width = width;
    _height = height;
    _failed = false;

    // 注意顺序：只有 logo、没有滤镜串是合法配置（纯角标）。
    // 如果先把空 desc 当非法返回，纯 logo 场景会被静默关掉（实测踩过：
    // 日志里是 "filter init failed, disabled: " 后面跟个空串，很费解）。
    // Logo-only (no filter string) is a valid configuration, so check that BEFORE
    // rejecting an empty description.
    if (width <= 0 || height <= 0) {
        ErrorL << "transcode filter: invalid size " << width << "x" << height;
        return false;
    }
    if (num <= 0 || den <= 0) {
        num = 1;
        den = 1000;
    }

    if (!_logo_path.empty()) {
        return initLogoGraph(width, height, fmt, num, den);
    }
    if (desc.empty()) {
        return false;
    }

    _graph = avfilter_graph_alloc();
    if (!_graph) {
        ErrorL << "transcode filter: avfilter_graph_alloc failed";
        return false;
    }

    // ---- 输入 ----
    char args[256];
    const char *pix = av_get_pix_fmt_name((enum AVPixelFormat) fmt);
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             width, height, fmt, num, den);
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    if (avfilter_graph_create_filter(&_src, buffersrc, "in", args, nullptr, _graph) < 0) {
        ErrorL << "transcode filter: create buffersrc failed, " << width << "x" << height
               << " pix_fmt=" << (pix ? pix : "unknown");
        close();
        return false;
    }

    // ---- 输出 ----
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    if (avfilter_graph_create_filter(&_sink, buffersink, "out", nullptr, nullptr, _graph) < 0) {
        ErrorL << "transcode filter: create buffersink failed";
        close();
        return false;
    }

    // ---- 解析描述串 ----
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    auto cleanup_io = [&]() {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
    };
    if (!outputs || !inputs) {
        cleanup_io();
        close();
        return false;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = _src;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = _sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    int ret = avfilter_graph_parse_ptr(_graph, desc.data(), &inputs, &outputs, nullptr);
    cleanup_io();
    if (ret < 0) {
        ErrorL << "transcode filter: 解析滤镜串失败（" << desc << "）: " << ret;
        close();
        return false;
    }
    ret = avfilter_graph_config(_graph, nullptr);
    if (ret < 0) {
        ErrorL << "transcode filter: avfilter_graph_config failed: " << ret
               << "（滤镜串 " << desc << "）";
        close();
        return false;
    }

    // 尺寸/格式必须与输入一致：编码器是按输入尺寸配置的，滤镜改了尺寸会在
    // avcodec_send_frame 处报 EINVAL，报错信息完全看不出是滤镜引起的。
    // The output size/format must match the input, otherwise the encoder (configured
    // with the input size) rejects frames with EINVAL and the message points nowhere.
    auto out_link = _sink->inputs[0];
    if (out_link->w != width || out_link->h != height) {
        ErrorL << "transcode filter: 输出尺寸 " << out_link->w << "x" << out_link->h
               << " 与编码器 " << width << "x" << height
               << " 不一致，请改用 video_scale/video_width 来改尺寸";
        close();
        return false;
    }
    if (out_link->format != fmt) {
        ErrorL << "transcode filter: 输出像素格式 " << out_link->format
               << " 与编码器 " << fmt << " 不一致";
        close();
        return false;
    }

    InfoL << "transcode filter ready: " << desc << " @ " << width << "x" << height;
    return true;
}

int TranscodeFilter::filter(AVFrame *in, AVFrame **out) {
    *out = nullptr;
    if (!_graph || _failed) {
        return -1;
    }
    if (_logo_src && _logo_frame) {
        AVFrame *dup = av_frame_clone(_logo_frame);
        if (dup) {
            dup->pts = in->pts;
            av_buffersrc_add_frame_flags(_logo_src, dup, 0);
            av_frame_free(&dup);
        }
    }
    int ret = av_buffersrc_add_frame_flags(_src, in, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        ErrorL << "transcode filter: buffersrc_add_frame failed: " << ret;
        _failed = true;
        return -1;
    }
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return -1;
    }
    ret = av_buffersink_get_frame(_sink, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free(&frame);
        return 0;
    }
    if (ret < 0) {
        ErrorL << "transcode filter: buffersink_get_frame failed: " << ret;
        av_frame_free(&frame);
        _failed = true;
        return -1;
    }
    *out = frame;
    return 1;
}

int TranscodeFilter::flush(AVFrame **out) {
    *out = nullptr;
    if (!_graph || _failed) {
        return 0;
    }
    int ret = av_buffersrc_add_frame_flags(_src, nullptr, 0);
    if (ret < 0) {
        return 0;
    }
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return -1;
    }
    ret = av_buffersink_get_frame(_sink, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free(&frame);
        return 0;
    }
    if (ret < 0) {
        av_frame_free(&frame);
        return -1;
    }
    *out = frame;
    return 1;
}

} // namespace mediakit
