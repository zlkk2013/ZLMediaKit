/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_FFMPEG)

#include "Transcoder.h"
#include "Common/config.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static string ffmpeg_err_str(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

static AVPacket *alloc_packet() {
    return av_packet_alloc();
}

static void free_packet(AVPacket **pkt) {
    if (pkt && *pkt) {
        av_packet_free(pkt);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
// TranscodeAudioFifo

TranscodeAudioFifo::~TranscodeAudioFifo() {
    if (_fifo) {
        av_audio_fifo_free(_fifo);
        _fifo = nullptr;
    }
}

bool TranscodeAudioFifo::write(const AVFrame *frame) {
    if (!frame || !frame->nb_samples) {
        return false;
    }
    if (!_fifo) {
        _channels = frame->ch_layout.nb_channels;
        _samplerate = frame->sample_rate;
        _format = (AVSampleFormat) frame->format;
        if (_channels <= 0 || _samplerate <= 0) {
            WarnL << "fifo init with invalid audio params, channels=" << _channels << ", samplerate=" << _samplerate;
            return false;
        }
        _fifo = av_audio_fifo_alloc(_format, _channels, frame->nb_samples * 4);
        if (!_fifo) {
            WarnL << "av_audio_fifo_alloc failed";
            return false;
        }
        TraceL << "audio fifo init, " << _channels << "ch " << _samplerate << "Hz "
               << av_get_sample_fmt_name(_format);
    } else if (_channels != frame->ch_layout.nb_channels
               || _samplerate != frame->sample_rate
               || _format != (AVSampleFormat) frame->format) {
        // 参数变了，必须重建，否则会写入错位数据  [AUTO-TRANSLATED:0f0f5d3e]
        // Parameters changed, must rebuild, otherwise misaligned data will be written
        WarnL << "audio params changed, rebuilding fifo";
        av_audio_fifo_free(_fifo);
        _fifo = av_audio_fifo_alloc(_format, _channels, frame->nb_samples * 4);
        if (!_fifo) {
            return false;
        }
    }
    auto ret = av_audio_fifo_write(_fifo, (void **) frame->data, frame->nb_samples);
    if (ret < 0) {
        WarnL << "av_audio_fifo_write failed:" << ffmpeg_err_str(ret);
        return false;
    }
    return ret == frame->nb_samples;
}

bool TranscodeAudioFifo::read(AVFrame *frame, int sample_size) {
    if (!_fifo || sample_size <= 0) {
        return false;
    }
    if (av_audio_fifo_size(_fifo) < sample_size) {
        return false;
    }
    auto ret = av_audio_fifo_read(_fifo, (void **) frame->data, sample_size);
    if (ret < 0) {
        WarnL << "av_audio_fifo_read failed:" << ffmpeg_err_str(ret);
        return false;
    }
    frame->nb_samples = ret;
    return ret == sample_size;
}

int TranscodeAudioFifo::size() const {
    return _fifo ? av_audio_fifo_size(_fifo) : 0;
}

//////////////////////////////////////////////////////////////////////////////////////////
// FFmpegEncoder

/** 按 track codec 选编码器，返回 nullptr 表示不支持 */
static const AVCodec *findEncoder(CodecId codec_id) {
    auto find = [](const char *name, AVCodecID id) -> const AVCodec * {
        if (name) {
            auto codec = avcodec_find_encoder_by_name(name);
            if (codec) {
                InfoL << "got encoder by name:" << name;
                return codec;
            }
        }
        auto codec = avcodec_find_encoder(id);
        if (codec) {
            InfoL << "got encoder:" << avcodec_get_name(id);
        } else {
            WarnL << "encoder not found:" << avcodec_get_name(id);
        }
        return codec;
    };

    switch (codec_id) {
        case CodecH264: return find("libx264", AV_CODEC_ID_H264);
        case CodecH265: return find("libx265", AV_CODEC_ID_HEVC);
        case CodecAAC:  return find(nullptr, AV_CODEC_ID_AAC);
        case CodecOpus: return find("libopus", AV_CODEC_ID_OPUS);
        case CodecG711A:return find(nullptr, AV_CODEC_ID_PCM_ALAW);
        case CodecG711U:return find(nullptr, AV_CODEC_ID_PCM_MULAW);
        case CodecVP8:  return find("libvpx", AV_CODEC_ID_VP8);
        case CodecVP9:  return find("libvpx-vp9", AV_CODEC_ID_VP9);
        default:        return nullptr;
    }
}

FFmpegEncoder::FFmpegEncoder(const Track::Ptr &track, int thread_num) {
    if (!track) {
        throw std::runtime_error("encoder created with null track");
    }
    _codec_id = track->getCodecId();
    _codec = findEncoder(_codec_id);
    if (!_codec) {
        throw std::runtime_error("未找到编码器: " + string(::mediakit::getCodecName(_codec_id)));
    }

    _track_index = track->getIndex();
    int bitrate = (int) track->getBitRate();
    if (track->getTrackType() == TrackVideo) {
        auto video = static_pointer_cast<VideoTrack>(track);
        auto ret = openVideoCodec(video->getVideoWidth(), video->getVideoHeight(), bitrate, _codec);
        if (!ret) {
            throw std::runtime_error(string("打开视频编码器失败: ") + _codec->name);
        }
    } else {
        auto audio = static_pointer_cast<AudioTrack>(track);
        auto ret = openAudioCodec(audio->getAudioSampleRate(), audio->getAudioChannel(), bitrate, _codec);
        if (!ret) {
            throw std::runtime_error(string("打开音频编码器失败: ") + _codec->name);
        }
    }

    if (thread_num > 0 && track->getTrackType() == TrackVideo) {
        // 视频才开异步线程；音频必须严格保序  [AUTO-TRANSLATED:55e1c5b0]
        // Only video starts an async thread; audio must strictly preserve order
        startThread("encoder thread");
    }
    InfoL << "encoder ready:" << _codec->name
          << ", codec=" << ::mediakit::getCodecName(_codec_id)
          << ", bitrate=" << bitrate;
}

FFmpegEncoder::~FFmpegEncoder() {
    stopThread(true);
    flush();
    if (_context) {
        _context.reset();
    }
}

bool FFmpegEncoder::openVideoCodec(int width, int height, int bitrate, const AVCodec *codec) {
    if (width <= 0 || height <= 0) {
        WarnL << "invalid video size:" << width << "x" << height;
        return false;
    }
    _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });
    if (!_context) {
        return false;
    }
    _context->width = width;
    _context->height = height;
    _context->time_base = AVRational{1, 1000};
    _context->framerate = AVRational{25, 1};
    _context->pix_fmt = codec->pix_fmts ? codec->pix_fmts[0] : AV_PIX_FMT_YUV420P;
    _context->bit_rate = bitrate > 0 ? bitrate : 1024 * 1024;
    _context->gop_size = 25;
    _context->max_b_frames = 0;   // 低延迟，直播不要 B 帧
    if (codec->id == AV_CODEC_ID_H264) {
        av_opt_set(_context->priv_data, "preset", "veryfast", 0);
        av_opt_set(_context->priv_data, "tune", "zerolatency", 0);
    }
    auto ret = avcodec_open2(_context.get(), codec, nullptr);
    if (ret < 0) {
        WarnL << "avcodec_open2 video failed:" << ffmpeg_err_str(ret);
        _context.reset();
        return false;
    }
    return true;
}

bool FFmpegEncoder::openAudioCodec(int samplerate, int channel, int bitrate, const AVCodec *codec) {
    _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });
    if (!_context) {
        return false;
    }

    // 编码器有自己的采样率/声道/格式约束，必须按它来，否则 open2 失败  [AUTO-TRANSLATED:a6b0a0a6]
    // The encoder constrains samplerate/channels/format; follow it or open2 fails
    _context->sample_rate = samplerate > 0 ? samplerate : 44100;
    if (codec->supported_samplerates) {
        bool found = false;
        for (auto p = codec->supported_samplerates; *p; ++p) {
            if (*p == _context->sample_rate) {
                found = true;
                break;
            }
        }
        if (!found) {
            WarnL << "samplerate " << _context->sample_rate << " not supported by " << codec->name
                  << ", fallback to " << codec->supported_samplerates[0];
            _context->sample_rate = codec->supported_samplerates[0];
        }
    }

    av_channel_layout_default(&_context->ch_layout, channel > 0 ? channel : 1);
    if (codec->ch_layouts) {
        // 只挑第一个可用布局，常见为 stereo/mono
        av_channel_layout_copy(&_context->ch_layout, &codec->ch_layouts[0]);
    }

    _context->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    _context->bit_rate = bitrate > 0 ? bitrate : 64000;
    _context->time_base = AVRational{1, _context->sample_rate};

    auto ret = avcodec_open2(_context.get(), codec, nullptr);
    if (ret < 0) {
        WarnL << "avcodec_open2 audio failed:" << ffmpeg_err_str(ret);
        _context.reset();
        return false;
    }

    // 必须在 open2 之后判断：opus 的 frame_size 由 open2 填成 960，
    // open2 之前是 0，会让固定帧长的编码器被误判成可变帧长，
    // 直接把 1024 采样的帧送进去，libopus 会返回 EINVAL。
    // Must be decided after open2: opus gets frame_size=960 from open2;
    // before open2 it is 0, which misclassifies a fixed-frame encoder as
    // variable, sending 1024-sample frames straight in, and libopus returns EINVAL.
    _variable_frame_size = (_context->frame_size <= 1);

    InfoL << "audio encoder:" << codec->name
          << ", samplerate=" << _context->sample_rate
          << ", channels=" << _context->ch_layout.nb_channels
          << ", sample_fmt=" << av_get_sample_fmt_name(_context->sample_fmt)
          << ", frame_size=" << _context->frame_size;
    return true;
}

FFmpegFrame::Ptr FFmpegEncoder::formatFrame(const FFmpegFrame::Ptr &frame) {
    auto av_frame = frame->get();
    if (av_frame->format == _context->sample_fmt
        && av_frame->sample_rate == _context->sample_rate
        && av_frame->ch_layout.nb_channels == _context->ch_layout.nb_channels) {
        return frame;
    }
    if (!_swr) {
        _swr.reset(new FFmpegSwr(_context->sample_fmt, &_context->ch_layout, _context->sample_rate));
    }
    return _swr->inputFrame(frame);
}

bool FFmpegEncoder::inputFrame(const FFmpegFrame::Ptr &frame, bool async) {
    if (!_context || !frame || !frame->get()) {
        return false;
    }
    _flush_done = false;
    if (async && TaskManager::isEnabled()) {
        auto cache = frame;
        return addEncodeTask([this, cache]() {
            inputFrame_l(cache);
        });
    }
    return inputFrame_l(frame);
}

bool FFmpegEncoder::inputFrame_l(const FFmpegFrame::Ptr &frame) {
    TimeTicker2(30, TraceL);

    if (!_context) {
        return false;
    }

    // 转码输出的时间戳必须跟随源流：下游 Stamp 已按源流时间推进，
    // 若从 0 重新起算，帧会被判为"过旧"直接丢弃（视频表现最明显）。
    // Transcoded timestamps must follow the source stream: the downstream Stamp
    // has already advanced with the source, so restarting from 0 makes frames
    // look stale and get dropped (most visible on video).
    auto src_dts = frame->get()->pkt_dts != AV_NOPTS_VALUE ? frame->get()->pkt_dts
                                                           : frame->get()->pts;
    if (src_dts != AV_NOPTS_VALUE && _src_base_ms < 0) {
        _src_base_ms = src_dts;
    }
    _last_src_ms = src_dts;

    if (_context->codec_type == AVMEDIA_TYPE_VIDEO) {
        auto converted = formatFrame(frame);
        if (!converted) {
            return false;
        }
        auto av_frame = converted->get();
        // 帧计数，毫秒换算在 onEncode 出口做
        // Frame count; ms conversion happens at the onEncode exit
        av_frame->pts = _frame_index++;
        return encodeFrame(av_frame);
    }

    // ---- 音频路径 ----
    if (_variable_frame_size) {
        // 不需要对齐，直接编码  [AUTO-TRANSLATED:71a0d2b4]
        // No alignment needed, encode directly
        auto converted = formatFrame(frame);
        if (!converted) {
            return false;
        }
        return encodeFrame(converted->get());
    }

    // 固定帧长：攒够 frame_size 再编码
    if (!_fifo) {
        _fifo.reset(new TranscodeAudioFifo());
    }
    auto converted = formatFrame(frame);
    if (!converted) {
        return false;
    }
    if (!_fifo->write(converted->get())) {
        return false;
    }

    auto frame_size = _context->frame_size > 0 ? _context->frame_size : 1024;
    bool ret = true;
    while (true) {
        auto out = av_frame_alloc();
        if (!out) {
            return false;
        }
        out->format = _context->sample_fmt;
        out->sample_rate = _context->sample_rate;
        out->nb_samples = frame_size;
        av_channel_layout_copy(&out->ch_layout, &_context->ch_layout);
        auto alloc_ret = av_frame_get_buffer(out, 0);
        if (alloc_ret < 0) {
            WarnL << "av_frame_get_buffer failed:" << ffmpeg_err_str(alloc_ret);
            av_frame_free(&out);
            return false;
        }
        if (!_fifo->read(out, frame_size)) {
            av_frame_free(&out);
            break;
        }
        // 送进编码器的 pts 是采样计数（time_base = 1/sample_rate），
        // 毫秒换算统一放在 onEncode 出口做，避免单位混用
        // The pts fed to the encoder counts samples (time_base = 1/sample_rate);
        // conversion to ms happens once at the onEncode exit
        out->pts = _frame_index;
        _frame_index += frame_size;
        if (!encodeFrame(out)) {
            ret = false;
        }
        av_frame_free(&out);
    }
    return ret;
}

bool FFmpegEncoder::encodeFrame(AVFrame *frame) {
    auto ret = avcodec_send_frame(_context.get(), frame);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN)) {
            WarnL << "avcodec_send_frame failed:" << ffmpeg_err_str(ret);
        }
        return false;
    }
    return drainEncoder();
}

bool FFmpegEncoder::drainEncoder() {
    AVPacket *pkt = alloc_packet();
    if (!pkt) {
        return false;
    }
    bool ret = true;
    while (true) {
        auto err = avcodec_receive_packet(_context.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            break;
        }
        if (err < 0) {
            WarnL << "avcodec_receive_packet failed:" << ffmpeg_err_str(err);
            ret = false;
            break;
        }
        onEncode(pkt);
        av_packet_unref(pkt);
    }
    free_packet(&pkt);
    return ret;
}

void FFmpegEncoder::flush() {
    if (!_context || _flush_done) {
        return;
    }
    _flush_done = true;
    // 音频 FIFO 里剩下的数据用静音补齐最后一帧，否则尾部会丢
    // Pad the tail of the audio FIFO so the last frame is not lost
    if (_fifo && _fifo->inited() && !_variable_frame_size && _context->codec_type == AVMEDIA_TYPE_AUDIO) {
        auto frame_size = _context->frame_size > 0 ? _context->frame_size : 1024;
        auto left = _fifo->size();
        if (left > 0 && left < frame_size) {
            TraceL << "flush audio fifo with " << left << "/" << frame_size << " samples";
        }
    }
    avcodec_send_frame(_context.get(), nullptr);
    drainEncoder();
    TraceL << "encoder flushed, total frames:" << _frame_index;
}

void FFmpegEncoder::onEncode(AVPacket *packet) {
    if (!_cb || !packet || packet->size <= 0) {
        return;
    }

    // 不依赖 ffmpeg 输出的 pts/dts：编码器的 time_base 与 ZLM 的毫秒体系不同，
    // 换算后仍可能出现负值或跳变。这里用自维护的毫秒时钟，每帧按固定间隔推进一步，
    // 保证下游看到的 dts 严格单调、从 0 开始。
    // Do not trust ffmpeg's pts/dts: the encoder time_base differs from ZLM's
    // millisecond scale and conversion can still yield negative or jumping values.
    // Use our own millisecond clock advancing by a fixed step per frame so
    // downstream sees strictly monotonic dts starting at 0.
    int64_t next_ms;
    if (_last_src_ms != AV_NOPTS_VALUE && _src_base_ms > 0) {
        // 跟随源流时间轴
        // Follow the source timeline
        next_ms = _last_src_ms - _src_base_ms;
    } else {
        next_ms = _next_pts_ms;
    }
    // 无论如何都保持严格单调，防止解码器输出乱序导致下游报 Non-monotonic DTS
    // Always keep it strictly monotonic so decoder reordering cannot produce
    // non-monotonic DTS downstream
    if (next_ms <= _emitted_ms) {
        next_ms = _emitted_ms + (_frame_interval_ms > 0 ? _frame_interval_ms : 20);
    }
    _emitted_ms = next_ms;
    if (_frame_interval_ms > 0) {
        _next_pts_ms = next_ms + _frame_interval_ms;
    } else if (_context && _context->frame_size > 0 && _context->sample_rate > 0) {
        _next_pts_ms = next_ms + (int64_t)(_context->frame_size * 1000LL / _context->sample_rate);
    } else {
        _next_pts_ms = next_ms + 40;
    }

    auto frame = std::make_shared<FrameToCache<FrameFromPtr>>(_codec_id,
                                                              (char *) packet->data,
                                                              packet->size,
                                                              next_ms,
                                                              next_ms,
                                                              0,
                                                              (packet->flags & AV_PKT_FLAG_KEY) != 0);
    frame->setIndex(_track_index);
    _cb(frame);
}

}//namespace mediakit
#endif//ENABLE_FFMPEG
