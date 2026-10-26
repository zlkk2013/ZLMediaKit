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
     * @param src_fps 源轨帧率；<=0 表示未知。视频必须传，否则按 25fps 硬编码，
     *                15fps 的源会变成 1.67 倍速且音画不同步。
     *                Source frame rate; <=0 means unknown. Required for video,
     *                otherwise a hardcoded 25fps makes a 15fps source play 1.67x
     *                too fast and desyncs audio.
     * @param src_video 源视频轨，用于取宽高。目标轨（如 H265Track）是空构造，
     *                  宽高为 0，要等 inputFrame 解析 SPS 才补齐；但编码器必须在
     *                  open2 时就知道尺寸，所以必须从源轨取。
     *                  Source video track used for width/height. The target track
     *                  (e.g. H265Track) is default-constructed with width/height 0
     *                  and only fills them when inputFrame parses the SPS, but the
     *                  encoder needs the size at open2 time, so take it from the source.
     * @throw std::runtime_error 找不到编码器或打开失败时抛出
     */
    FFmpegEncoder(const Track::Ptr &track, int thread_num = 2, float src_fps = 0,
                  const Track::Ptr &src_video = nullptr);
    ~FFmpegEncoder() override;

    /**
     * 输入一帧解码后的数据
     * @param async 是否异步编码（仅视频有意义；音频必须同步以保证帧序）
     */
    bool inputFrame(const FFmpegFrame::Ptr &frame, bool async);

    /**
     * 编码一帧静音（全零 PCM），供转码轨在源端断流时兜底
     *
     * 静音负载必须由编码器真实产出，不能硬编码字节数组：Opus 的 TOC 字节由
     * 带宽/立体声/帧长共同决定，写死的负载与真实编码器配置不一致，解码端会
     * 报错或丢掉这一帧。时间戳留空(AV_NOPTS_VALUE)，由编码器自维护的毫秒
     * 时钟从上一帧继续推进，保证与前面已输出的帧严格连续、单调。
     *
     * Encode one frame of digital silence (all-zero PCM) so a transcoded track can
     * keep producing frames after the upstream stalls. The payload must come from
     * the real encoder: the Opus TOC byte depends on bandwidth/stereo/frame size,
     * so a hard-coded blob would not match and decoders would reject the frame.
     * The timestamp is left unset (AV_NOPTS_VALUE) so the encoder's own millisecond
     * clock continues from the previous frame and stays strictly monotonic.
     *
     * @return false 表示编码器未就绪或不是音频编码器
     */
    bool inputSilence();

    /** 冲刷编码器内部缓冲，流结束时必须调用，否则尾部若干帧丢失 */
    void flush();

    /**
     * 预热：编码一帧纯色画面并把得到的 VPS/SPS/PPS 喂给目标轨，使其 ready() 变 true。
     * 必要性：RtspMuxer/MP4Muxer 的 addTrack 直接丢弃 ready()==false 的轨
     * （日志 "H265 unready!"），而 H265Track 只有在 inputFrame 里解析到 SPS 后才 ready。
     * 转码轨必须在 addTrack 之前就 ready，否则整条视频轨被丢掉、观众端没有画面。
     * Warm up: encode one solid-color frame and feed the resulting VPS/SPS/PPS into the
     * target track so ready() becomes true. RtspMuxer/MP4Muxer drop tracks whose
     * ready() is false ("H265 unready!") and H265Track only becomes ready after
     * parsing an SPS in inputFrame. A transcoded track must be ready before
     * addTrack, otherwise the whole video track is discarded and viewers see nothing.
     * @param target 目标轨（会调用其 inputFrame 喂 config 帧）
     * @param width 画面宽
     * @param height 画面高
     * @return 目标轨是否已 ready
     */
    bool warmup(const Track::Ptr &target, int width, int height);

    void setOnEncode(onEnc cb) { _cb = std::move(cb); }

    /** 设置输出帧的 track index（必须与源轨一致，下游按 index 分发） */
    void setTrackIndex(int index) { _track_index = index; }

    /**
     * 设置输出帧的 Annex-B 起始码长度（3 或 4）
     * H264/H265 的 Track::inputFrame 依赖 frame->prefixSize() 跳过起始码读 NAL 头，
     * 传 0 会让 H265Track 把起始码字节当成 NAL 类型，永远解析不到 SPS（宽高恒为 0）。
     * Set the Annex-B start code length (3 or 4) of output frames.
     * Track::inputFrame relies on frame->prefixSize() to skip the start code
     * before reading the NAL header; passing 0 makes H265Track treat start-code
     * bytes as the NAL type, so SPS is never parsed and width/height stay 0.
     */
    void setNaluPrefixSize(size_t size) { _nalu_prefix_size = size; }

    /** 设置视频帧率（从源轨取，不要硬编码 25fps，否则输出时间戳与音画同步都会错） */
    void setVideoFps(float fps) { _video_fps = fps; }

    CodecId getCodecId() const override { return _codec_id; }
    const AVCodecContext *getContext() const { return _context.get(); }

    /** 编码器是否可用（构造成功即为 true，供上层做能力判定） */
    bool available() const { return _context && _codec; }

private:
    void onEncode(AVPacket *packet);
    bool encodeFrame(AVFrame *frame);
    bool encodeAudio_l(AVFrame *converted);
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
    /** 输出帧的 Annex-B 起始码长度，0 表示未知（由 onEncode 探测） */
    size_t _nalu_prefix_size = 0;
    /** 视频帧率，0 表示未设置（回退到编码器默认） */
    float _video_fps = 0;
    int64_t _video_frame_index = 0;
    int64_t _video_pkt_index = 0;
    /** 首个源帧时间戳（毫秒），用于把输出对齐到源时间轴 */
    int64_t _src_base_ms = -1;
    /** 最近一次输入帧的源时间戳 */
    int64_t _last_src_ms = AV_NOPTS_VALUE;
    /** 最近一次已输出帧的时间戳，保证严格单调 */
    int64_t _emitted_ms = -1;
    /**
     * 本次编码的是静音兜底帧：强制走 _emitted_ms 单调时钟，忽略源时间轴。
     *
     * 静音帧的"源时间轴"是我们自己虚拟推进的，与真实源 dts 的步长不一致
     * （AAC 一帧 23.2ms，Opus 一帧 20ms），混用会让 dts 偶发倒退，
     * 实测观众端报 "Non-monotonous DTS"。静音帧只保证"紧跟上一帧、严格递增"，
     * 源端恢复后真实帧再靠 _emitted_ms 守卫接回去，不会出现时间回退。
     *
     * True while encoding a silence padding frame: forces the monotonic
     * _emitted_ms clock and ignores the source timeline. The virtual source clock
     * we advance for silence does not match real source steps (AAC 23.2ms vs Opus
     * 20ms per frame), and mixing them made dts occasionally go backwards
     * (observers reported "Non-monotonous DTS"). Silence frames only guarantee
     * "follow the previous frame, strictly increasing"; real frames rejoin later
     * through the _emitted_ms guard.
     */
    bool _silence_encoding = false;
    bool _flush_done = false;
    int64_t _frame_index = 0;
};

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODER_H
