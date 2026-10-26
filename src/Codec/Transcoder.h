/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_TRANSCODER_H
#define ZLMEDIAKIT_TRANSCODER_H

#if defined(ENABLE_FFMPEG)

#include <memory>
#include "Codec/Transcode.h"

// 注意：ffmpeg 头必须放在 extern "C" 里，Transcode.h 已包裹大部分；
// 但 opt.h 未在其中，漏掉会导致 av_opt_set 被当 C++ 符号而链接失败
// NOTE: ffmpeg headers must be wrapped in extern "C"; Transcode.h covers most of them,
// but opt.h is not included there and must be wrapped here or av_opt_set fails to link
#ifdef __cplusplus
extern "C" {
#endif
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#ifdef __cplusplus
}
#endif

namespace mediakit {

/**
 * 音频重采样 FIFO
 *
 * AAC 编码器要求固定 1024 采样点一帧，而解码出来的 PCM 长度不齐，
 * 必须攒够再送。少这一步，编码器会返回 EINVAL 或者产出时长错误的流，
 * 表现为播放器音画不同步 / 声音断续。
 *
 * Audio resample FIFO.
 * The AAC encoder requires exactly 1024 samples per frame, but decoded PCM
 * comes in arbitrary lengths, so samples must be accumulated. Skipping this
 * makes the encoder return EINVAL or produce frames with wrong duration,
 * which shows up as A/V desync or stuttering audio in players.
 */
class TranscodeAudioFifo {
public:
    using Ptr = std::shared_ptr<TranscodeAudioFifo>;

    TranscodeAudioFifo() = default;
    ~TranscodeAudioFifo();

    /**
     * 写入一帧 PCM
     * 返回 false 表示参数与已初始化状态不一致（声道/采样率/格式变了）
     */
    bool write(const AVFrame *frame);

    /**
     * 读出一帧，长度不足 sample_size 时返回 false（数据留在 FIFO 里）
     * 写出的 frame 由调用方 av_frame_alloc 好，这里只填 buffer
     */
    bool read(AVFrame *frame, int sample_size);

    /** FIFO 里现有采样点数（单声道计） */
    int size() const;

    /** 已初始化则返回 true */
    bool inited() const { return _fifo != nullptr; }

private:
    int _channels = 0;
    int _samplerate = 0;
    AVSampleFormat _format = AV_SAMPLE_FMT_NONE;
    AVAudioFifo *_fifo = nullptr;
};

/**
 * FFmpeg 编码器
 *
 * 与 FFmpegDecoder 对称：解码器吃压缩帧吐 AVFrame，编码器吃 AVFrame 吐压缩帧。
 * 按 track 的 codec 类型自动选编码器，音频走 FIFO 补齐帧长。
 *
 * FFmpeg encoder, symmetric to FFmpegDecoder: the decoder consumes compressed
 * frames and emits AVFrames; this consumes AVFrames and emits compressed frames.
 * The encoder is chosen from the track codec, and audio runs through a FIFO so
 * frames are sample-aligned.
 */
class FFmpegEncoder : public TaskManager, public CodecInfo {
public:
    using Ptr = std::shared_ptr<FFmpegEncoder>;
    using onEnc = std::function<void(const Frame::Ptr &)>;

    /**
     * @param track 目标轨（转换后的 codec/分辨率/采样率，不是源轨）
     * @param thread_num 编码线程数；仅视频会真正开线程，音频同步编码
     * @throw std::runtime_error 找不到编码器或打开失败时抛出
     */
    FFmpegEncoder(const Track::Ptr &track, int thread_num = 2);
    ~FFmpegEncoder() override;

    /**
     * 输入一帧解码后的数据
     * @param async 是否异步编码（仅视频有意义；音频必须同步以保证帧序）
     */
    bool inputFrame(const FFmpegFrame::Ptr &frame, bool async);

    /** 冲刷编码器内部缓冲，流结束时必须调用，否则尾部若干帧丢失 */
    void flush();

    void setOnEncode(onEnc cb) { _cb = std::move(cb); }

    /** 设置输出帧的 track index（必须与源轨一致，下游按 index 分发） */
    void setTrackIndex(int index) { _track_index = index; }

    CodecId getCodecId() const override { return _codec_id; }
    const AVCodecContext *getContext() const { return _context.get(); }

    /** 编码器是否可用（构造成功即为 true，供上层做能力判定） */
    bool available() const { return _context && _codec; }

private:
    void onEncode(AVPacket *packet);
    bool encodeFrame(AVFrame *frame);
    bool inputFrame_l(const FFmpegFrame::Ptr &frame);
    bool drainEncoder();

    bool openVideoCodec(int width, int height, int bitrate, const AVCodec *codec);
    bool openAudioCodec(int samplerate, int channel, int bitrate, const AVCodec *codec);

    /** 把 FFmpegFrame 转成编码器要求的像素格式/采样格式 */
    FFmpegFrame::Ptr formatFrame(const FFmpegFrame::Ptr &frame);

private:
    onEnc _cb;
    CodecId _codec_id;
    const AVCodec *_codec = nullptr;
    std::shared_ptr<AVCodecContext> _context;

    std::unique_ptr<FFmpegSws> _sws;
    std::unique_ptr<FFmpegSwr> _swr;
    std::unique_ptr<TranscodeAudioFifo> _fifo;

    /** OPUS 等允许变长帧的编码器，不需要 FIFO 对齐 */
    bool _variable_frame_size = false;
    /** 输出帧的 track index，由所属 track 决定 */
    int _track_index = -1;
    /** 输出时间戳（毫秒），自维护严格单调 */
    int64_t _next_pts_ms = 0;
    /** 每帧时间步进（毫秒），0 表示按编码器 frame_size/fps 推算 */
    int64_t _frame_interval_ms = 0;
    /** 首个源帧时间戳（毫秒），用于把输出对齐到源时间轴 */
    int64_t _src_base_ms = -1;
    /** 最近一次输入帧的源时间戳 */
    int64_t _last_src_ms = AV_NOPTS_VALUE;
    /** 最近一次已输出帧的时间戳，保证严格单调 */
    int64_t _emitted_ms = -1;
    bool _flush_done = false;
    int64_t _frame_index = 0;
};

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODER_H
