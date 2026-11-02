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

namespace mediakit {
// 滤镜图：只需要指针成员，定义在 TranscodeFilter.h（.cpp 里包含）
class TranscodeFilter;
}

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
 * 某些音频 codec 的 RTP 载荷规格是**写死**的（见 Rtsp.h 的 payload 表），
 * 编码器与目标轨都必须按这个规格来，否则 RTSP 封装找不到对应的 payload 类型，
 * 观众端直接报 "Decoder (codec none) not found"，音频整条丢失。
 *
 *   PCMA / PCMU : pt=8 / pt=0，8000Hz 单声道
 *   MP3         : pt=14，44100Hz 双声道
 *
 * 返回 true 表示该 codec 有强制规格，并回填 rate/channels。
 *
 * Some audio codecs have a FIXED RTP payload format (see the payload table in
 * Rtsp.h); both the encoder and the target track must use it, otherwise the RTSP
 * muxer cannot map the track and viewers get "Decoder (codec none) not found"
 * with no audio at all.
 */
static inline bool forcedAudioSpec(CodecId codec, int &rate, int &channels) {
    switch (codec) {
        case CodecG711A:
        case CodecG711U:
            rate = 8000;
            channels = 1;
            return true;
        case CodecMP3:
            rate = 44100;
            channels = 2;
            return true;
        case CodecG722:
            // pt=9 / 16000Hz / 单声道（见 Rtsp.h 的载荷表）
            rate = 16000;
            channels = 1;
            return true;
        default:
            return false;
    }
}

/**
 * 视频输出目标参数
 *
 * 全部为 0 时表示「跟随源」，行为与此前硬编码路径完全一致。
 * All-zero means "follow the source", identical to the previous hardcoded path.
 */
struct VideoTargetParams {
    /** 输出宽；0=跟随源。与 height 必须同时设置或同时为 0 */
    int width = 0;
    /** 输出高；0=跟随源 */
    int height = 0;
    /** 输出帧率；0=跟随源。只支持降帧（抽帧），不做插帧 */
    float fps = 0;
    /** 输出码率(bps)；0=按分辨率/帧率自动估算 */
    int bitrate = 0;
    /**
     * 优先使用硬件编码器（VideoToolbox / NVENC / VAAPI / QSV）。
     * 拿不到硬件编码器、或硬件编码器打开失败时**自动回落软件编码**，
     * 永远不让一条流因为硬件不可用而失败。
     *
     * Prefer a hardware encoder; fall back to software when it is unavailable or fails
     * to open, so a stream never dies just because the GPU path is missing.
     */
    bool hw_encode = false;
    /**
     * 硬件设备序号（-1 = 不指定）。目前只对 NVENC 生效（`gpu` 选项）：
     * VAAPI 走 render node 路径、VideoToolbox 没有设备索引，两者都会忽略并在状态里报 -1。
     *
     * Hardware device index (-1 = unspecified). Honoured by NVENC's `gpu` option only;
     * VAAPI uses render nodes and VideoToolbox has no device index.
     */
    int device = -1;
};

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
/**
 * 平台首选硬件解码器名（h264_videotoolbox / hevc_cuvid ...）；无对应硬件实现返回空串。
 * 供 FFmpegDecoder 的 codec_name 参数使用 —— 上游默认候选表把通用软件解码器排在硬件前面，
 * 不显式指定就永远走不到硬件解码。
 *
 * Preferred hardware decoder name per platform; empty when unsupported. Passed into
 * FFmpegDecoder's codec_name: upstream's default candidate list puts the generic
 * software decoder first, so hardware decode is otherwise never reached.
 */
std::string hwDecoderName(CodecId codec_id);

/**
 * 当前平台是否支持「按设备号选择 GPU」。
 *
 * 只有 NVENC（Windows/Linux）有 `gpu` 选项；macOS 的 VideoToolbox 没有设备索引，
 * VAAPI 走 render node 而非序号 —— 这两个平台上配置 hw_devices 不会报错，但要如实
 * 在状态里报 -1，绝不能假装分到了卡。
 *
 * Whether this platform supports selecting a GPU by index. Only NVENC does;
 * VideoToolbox/VAAPI have no index, so hw_devices is accepted but reported as -1.
 */
bool hwDeviceSelectionSupported();

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
     * @param video_params 输出分辨率/帧率/码率；全 0 表示跟随源。
     *                     Output size/fps/bitrate; all-zero follows the source.
     * @throw std::runtime_error 找不到编码器或打开失败时抛出
     */
    FFmpegEncoder(const Track::Ptr &track, int thread_num = 2, float src_fps = 0,
                  const Track::Ptr &src_video = nullptr,
                  const VideoTargetParams &video_params = VideoTargetParams());
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
     * 设置视频滤镜（libavfilter 单输入描述串，如 OSD 文字 drawtext=...）。
     *
     * 必须在构造后、喂第一帧前调用：滤镜图要按编码器的输出尺寸/像素格式建，
     * 而那是 open2 之后才确定的。空串表示不启用。
     * 滤镜**不允许改变尺寸/像素格式**，需要改尺寸请用 video_scale / video_width。
     *
     * Set a single-input libavfilter chain (e.g. drawtext OSD). Must be called before
     * the first frame; the graph is built from the encoder's output size/pix_fmt.
     * Empty string disables it. The filter must not change size or pixel format —
     * use video_scale / video_width for resizing.
     */
    /** 实际选中的编码器名（如 libx265 / hevc_videotoolbox），未初始化时为空 */
    std::string encoderName() const { return _codec && _codec->name ? _codec->name : ""; }
    /** 是否走硬件编码器 */
    bool isHardwareEncoder() const;

    /**
     * 已编码的**帧**数（一个 AVPacket 算一帧，音频同理）。
     *
     * 不要用 NAL 级回调计数：视频在 NAL 拆分后才逐 NAL 回调，x265 每帧带 AUD+VCL、
     * IDR 再加 VPS/SPS/PPS ⇒ NAL 数约为帧数的 2.2 倍（实测服务端 63fps vs 真实 27fps）。
     *
     * Encoded frame count (one AVPacket = one frame). Counting at the NAL level
     * over-reports ~2.2x because x265 emits AUD+VCL per frame and VPS/SPS/PPS per IDR.
     */
    int64_t encodedFrames() const { return _encoded_frames; }
    /** 负载自适应：当前生效帧率（0 表示未启用/未初始化）与已丢弃帧数 */
    float adaptFpsNow() const { return _adapt_fps ? _adapt_fps_now : 0; }
    int64_t adaptDropped() const { return _adapt_dropped; }

    void setVideoFilter(const std::string &desc) { _filter_desc = desc; }

    /**
     * 设置 logo 角标（同样必须在喂第一帧前）。x/y 为负表示距右/下边缘的距离。
     * Logo watermark, also before the first frame. Negative x/y means distance from
     * the right/bottom edge.
     */
    /**
     * 负载自适应降帧（过载时主动降帧率，避免编码跟不上导致卡顿/丢帧）。
     *
     * 判据用「单帧编码耗时 / 帧间隔」的负载比，不是 CPU 占用：
     * 后者受其它进程影响，且多核机器上占用率与「这条流来不来得及」不是一回事。
     * 降帧靠抽帧实现（编码前丢弃，解码端不会花屏），且**永不放掉关键帧**。
     *
     * @param enable 是否启用
     * @param min_fps 降帧下限，0 表示用 5
     * @param selftest_delay_ms 仅用于自测：给测得的编码耗时人为加上这么多毫秒，
     *                          用来在快机器上确定性地复现「过载」。生产必须为 0。
     *
     * Load-adaptive frame dropping. The metric is encode cost / frame interval, not CPU
     * usage (which other processes distort). Dropping happens BEFORE encoding so the
     * decoder never sees a broken reference chain, and keyframes are never dropped.
     * selftest_delay_ms is a test hook to deterministically simulate overload; must be 0
     * in production.
     */
    void setAdaptiveFps(bool enable, float min_fps = 5, int selftest_delay_ms = 0) {
        _adapt_fps = enable;
        _adapt_min_fps = min_fps > 0.5f ? min_fps : 5.0f;
        _adapt_selftest_delay_ms = selftest_delay_ms;
    }

    void setVideoLogo(const std::string &path, int x, int y, int width) {
        _logo_path = path;
        _logo_x = x;
        _logo_y = y;
        _logo_width = width;
    }

    /**
     * 预热：编码一帧纯色画面并把得到的 config NAL（H265 的 VPS/SPS/PPS、
     * H264 的 SPS/PPS）喂给目标轨，使其 ready() 变 true。
     * 必要性：RtspMuxer/MP4Muxer 的 addTrack 直接丢弃 ready()==false 的轨
     * （日志 "H265 unready!"），而 Track 只有在 inputFrame 里解析到 config NAL 后才 ready。
     * 转码轨必须在 addTrack 之前就 ready，否则整条视频轨被丢掉、观众端没有画面。
     * Warm up: encode one solid-color frame and feed the resulting config NALs
     * (VPS/SPS/PPS for H265, SPS/PPS for H264 — H264 has no VPS) into the target
     * track so ready() becomes true. RtspMuxer/MP4Muxer drop tracks whose ready()
     * is false ("H265 unready!") and a Track only becomes ready after parsing a
     * config NAL in inputFrame. A transcoded track must be ready before addTrack,
     * otherwise the whole video track is discarded and viewers see nothing.
     * @param target 目标轨（会调用其 inputFrame 喂 config 帧）
     * @return 目标轨是否已 ready
     */
    bool warmup(const Track::Ptr &target);

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

    /**
     * 一帧音频的毫秒时长：固定帧长取 frame_size/rate，变长帧（G711 frame_size=0）
     * 按 20ms 计。静音兜底的补帧节奏必须用它，否则会与实际帧长脱节。
     *
     * Per-frame audio duration in ms: frame_size/rate for fixed-frame encoders, 20ms for
     * variable-frame ones (G711 reports frame_size 0). The silence fallback must pace on
     * this value or it drifts from the real frame length.
     */
    int getAudioFrameMs() const { return (int) _audio_frame_ms; }

    /** 编码器是否可用（构造成功即为 true，供上层做能力判定） */
    bool available() const { return _context && _codec; }

private:
    void onEncode(AVPacket *packet);

    bool encodeFrame(AVFrame *frame);
    bool encodeAudio_l(AVFrame *converted);
    /** 音频切帧长度（采样数），编码与静音补帧共用；见实现处注释 */
    int audioChunkSamples() const;
    bool inputFrame_l(const FFmpegFrame::Ptr &frame);
    bool drainEncoder();

    bool openVideoCodec(int width, int height, int bitrate, const AVCodec *codec);
    /**
     * @param target 目标轨（音频）。用于回填 extradata 让 AACTrack::ready() 变真，
     *               否则 RtspMuxer/MP4Muxer 会把未就绪的转码轨直接丢弃。
     *               Target audio track. Used to backfill extradata so AACTrack::ready()
     *               becomes true; otherwise RtspMuxer/MP4Muxer drop the unready track.
     */
    bool openAudioCodec(int samplerate, int channel, int bitrate, const AVCodec *codec,
                        const Track::Ptr &target = nullptr);

    /**
     * 估算自动码率（bitrate 传 0 时使用）
     * bpp 按编码器效率取值：H265 比 H264 省约 30% 码率
     * Estimate the automatic bitrate used when bitrate == 0. The bits-per-pixel
     * factor is per-codec: H265 needs roughly 30% less than H264.
     */
    int estimateBitrate(int width, int height, int fps) const;

    /** 按目标帧率决定这一源帧是否需要编码（降帧），返回 false 表示丢弃 */
    bool shouldKeepFrame(const AVFrame *frame);

    /** 把 FFmpegFrame 转成编码器要求的像素格式/采样格式 */
    FFmpegFrame::Ptr formatFrame(const FFmpegFrame::Ptr &frame);

    /**
     * 用解码帧时间戳间隔实测源帧率（仅裸帧 codec）
     *
     * VP8/VP9/AV1 的码流、SDP、RTMP metadata 都不带帧率，源轨的 getVideoFps()
     * 只是 VideoTrackImp 的默认值 30 —— 15fps 的源会被当成 30fps 打时间戳，
     * 输出时间轴只剩一半长度（拉 5 秒内容要 10 秒）。这里对最近若干帧的
     * 源时间戳间隔做「去极值平均」得到真实帧间隔：丢帧造成的 2 倍间隔与瞬间
     * 抖动都被剔除，CFR 源的误差在 1% 以内。
     *
     * Measure the source frame rate from decoded-frame timestamp deltas. VP8/VP9/
     * AV1 carry no frame rate anywhere (bitstream, SDP, RTMP metadata), so the
     * source track's getVideoFps() is just VideoTrackImp's default 30 — a 15fps
     * source gets 30fps timestamps and the output timeline ends up half as long
     * (pulling 5s of content took 10s). Take a trimmed mean over recent source
     * timestamp deltas: dropped frames (2x interval) and jitter are trimmed off,
     * CFR error stays under 1%.
     */
    void updateSrcFpsEstimate(int64_t src_ms);

    /**
     * 当前视频帧的时间步进（毫秒，带小数）
     *
     * 优先级：显式配置的输出帧率 > 实测/声明的源帧率 > 40ms 兜底。
     * Priority: configured output rate > measured/declared source rate > 40ms.
     */
    double videoStepMs() const;
    /** 负载自适应降帧的控制律，见 .cpp 里的说明 */
    void updateAdaptiveFps(double cost_ms);

    /**
     * 取下一个视频时间戳（毫秒）并推进时钟
     *
     * 用累加而不是「序号 × 步进」：步进会在实测帧率收敛时变化，序号乘法
     * 会让时间轴在那一刻整段跳变。累加保持时间轴连续。
     * Accumulate instead of "index * step": the step changes while the measured
     * rate converges, and index multiplication would jump the whole timeline at
     * that moment. Accumulation keeps it continuous.
     *
     * @param acc 该时间轴的累加器；输入帧 pts 与输出包 dts 各用一个，
     *            否则两条路径都推进同一个累加器，时间轴会以两倍速前进。
     *            The accumulator for this timeline: input pts and output dts use
     *            two separate ones, otherwise both paths advance the same
     *            accumulator and the timeline runs at double speed.
     */
    int64_t nextVideoStamp(double &acc);

    onEnc _cb;
    CodecId _codec_id;
    const AVCodec *_codec = nullptr;
    std::shared_ptr<AVCodecContext> _context;

    std::unique_ptr<FFmpegSws> _sws;
    /** sws 当前的输出目标（尺寸/格式）；与编码器不一致时必须重建 */
    int _sws_width = 0;
    int _sws_height = 0;
    AVPixelFormat _sws_format = AV_PIX_FMT_NONE;
    std::unique_ptr<FFmpegSwr> _swr;
    std::unique_ptr<TranscodeAudioFifo> _fifo;

    /** OPUS 等允许变长帧的编码器，不需要 FIFO 对齐 */
    bool _variable_frame_size = false;
    /** 一帧的采样点数：固定帧长编码器取 frame_size，变长帧（G711）取 rate/50 */
    int _silence_samples = 0;
    /** 一帧的毫秒时长，静音补帧节奏与 pts 推进共用 */
    int64_t _audio_frame_ms = 20;
    /** 输出帧的 track index，由所属 track 决定 */
    int _track_index = -1;
    /** 输出时间戳（毫秒），自维护严格单调 */
    int64_t _next_pts_ms = 0;
    /** 每帧时间步进（毫秒），0 表示按编码器 frame_size/fps 推算 */
    int64_t _frame_interval_ms = 0;
    /** 已编码帧数（一个包一帧） */
    int64_t _encoded_frames = 0;
    /** 输出帧的 Annex-B 起始码长度，0 表示未知（由 onEncode 探测） */
    size_t _nalu_prefix_size = 0;
    /** 视频帧率，0 表示未设置（回退到编码器默认） */
    float _video_fps = 0;
    /** 源轨帧率（用于降帧抽帧判断），0 表示未知 */
    float _src_fps = 0;
    /** 输出目标参数（分辨率/帧率/码率），全 0 表示跟随源 */
    VideoTargetParams _video_params;
    /** 输出的宽高（缩放目标）；源与目标相同时等于源宽高 */
    int _out_width = 0;
    int _out_height = 0;
    /** 降帧抽帧用的累计计数（按源帧序号 × 目标/源帧率比取整判断） */
    int64_t _drop_index = 0;
    int64_t _keep_index = 0;
    /**
     * 源帧率需要实测：裸帧 codec（VP8/VP9/AV1）没有任何地方带帧率，
     * 源轨给的 30 只是 VideoTrackImp 的默认值。
     * The source frame rate must be measured: raw-packet codecs (VP8/VP9/AV1)
     * carry no frame rate anywhere and the track's 30 is just VideoTrackImp's
     * default.
     */
    bool _src_fps_measure = false;
    /** 实测源帧间隔的统计窗口长度（帧） */
    static constexpr int kSrcFpsWindow = 16;
    int64_t _src_fps_deltas[kSrcFpsWindow] = {0};
    int _src_fps_delta_count = 0;
    int _src_fps_delta_pos = 0;
    /** 上一帧的源时间戳（毫秒），仅用于帧率实测 */
    int64_t _src_fps_last_ms = AV_NOPTS_VALUE;
    /** 实测得到的源帧率，0 表示还没收敛 */
    float _src_fps_measured = 0;
    /** 视频时间戳累加器（毫秒，保留小数避免长时间累积漂移）：输入 pts 用 _video_in_ms */
    double _video_in_ms = 0;
    /** 视频输出包 dts 的累加器 */
    double _video_out_ms = 0;
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
    /** 视频滤镜描述串与图对象（懒建：等编码器 open2 确定尺寸后再建图） */
    std::string _filter_desc;
    std::shared_ptr<TranscodeFilter> _filter;
    // ---- 负载自适应降帧 ----
    bool _adapt_fps = false;
    float _adapt_min_fps = 5;
    /** 当前生效的帧率上限，0 表示未初始化（跟随源/目标） */
    float _adapt_fps_now = 0;
    /** 编码耗时的指数滑动平均（毫秒） */
    double _enc_cost_ewma_ms = 0;
    int _overload_frames = 0;
    int _idle_frames = 0;
    int _adapt_selftest_delay_ms = 0;
    int64_t _adapt_dropped = 0;
    int64_t _adapt_last_log_ms = 0;

    std::string _logo_path;
    int _logo_x = 10;
    int _logo_y = 10;
    int _logo_width = 0;
    /** 建图失败过就不再重试（避免每帧都打一条错误日志刷爆） */
    bool _filter_failed = false;
    bool _flush_done = false;
    int64_t _frame_index = 0;
};

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODER_H
