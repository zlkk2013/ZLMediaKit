/**
 * 单输入滤镜图实现，见 TranscodeFilter.h 的说明。
 */
#include "TranscodeFilter.h"

#include <cstring>

extern "C" {
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
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
}

bool TranscodeFilter::init(const std::string &desc, int width, int height, int fmt, int num, int den) {
    close();
    _desc = desc;
    _width = width;
    _height = height;
    _failed = false;

    if (desc.empty()) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        ErrorL << "transcode filter: invalid size " << width << "x" << height;
        return false;
    }
    if (num <= 0 || den <= 0) {
        num = 1;
        den = 1000;
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
