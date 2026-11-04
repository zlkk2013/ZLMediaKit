/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_TRANSCODE_SINK_H
#define ZLMEDIAKIT_TRANSCODE_SINK_H

#if defined(ENABLE_FFMPEG)

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Extension/Track.h"
#include "Extension/Frame.h"
#include "Codec/Transcoder.h"
#include "Poller/Timer.h"
#ifdef ENABLE_ASCEND_VENC
// 必须放在 namespace 之外：AscendVdec.h 自己开 mediakit 命名空间，
// 放在里面会变成 mediakit::mediakit::AscendVdec
#include "Transcode/AscendVdec.h"
#endif

namespace mediakit {

/**
 * 转码参数
 * Transcode parameters.
 */
struct TranscodeOption {
    /** 是否启用 */
    bool enable = false;
    /** 目标音频 codec；CodecInvalid 表示不转音频 */
    CodecId audio_codec = CodecInvalid;
    /** 目标视频 codec；CodecInvalid 表示不转视频 */
    CodecId video_codec = CodecInvalid;
    /** 目标码率，0 表示沿用源（音视频共用；视频想单独指定用 video_bitrate） */
    int bitrate = 0;
    /** 目标采样率（音频），0 表示沿用源 */
    int sample_rate = 0;
    /** 视频输出宽，0=跟随源；必须与 video_height 成对 */
    int video_width = 0;
    /** 视频输出高，0=跟随源 */
    int video_height = 0;
    /** 视频输出帧率，0=跟随源。只支持降帧（抽帧） */
    float video_fps = 0;
    /** 视频专用码率(bps)，0=沿用 bitrate/源。优先于 bitrate */
    int video_bitrate = 0;
    /**
     * 分辨率倍数：按源宽高等比缩放，如 0.5 表示一半、2.0 表示两倍。
     * 0 = 不缩放（默认）。显式给了 video_width / video_height 时以显式值为准
     * （绝对尺寸比倍数更明确，避免两者同时配置时语义含糊）。
     *
     * Resolution multiplier relative to the source (0.5 = half, 2.0 = double).
     * 0 disables it. Explicit video_width/video_height win when both are given.
     */
    float video_scale = 0;
    /**
     * 视频滤镜描述串（libavfilter 单输入），如 `drawtext=text='CAM-01':x=10:y=10:fontsize=28:fontcolor=white`。
     * 空串表示不启用。滤镜**不能改变尺寸/像素格式**（要改尺寸用 video_scale/video_width）。
     * 多输入滤镜（overlay 叠加 logo 图片）暂不支持。
     *
     * Single-input libavfilter chain (e.g. drawtext OSD). Empty disables it. The filter
     * must not change size/pixel format; multi-input filters (overlay with a logo image)
     * are not supported yet.
     */
    std::string video_filter;
    /**
     * logo 角标：图片路径（png/jpg）。空串表示不叠加。
     * x/y 为叠加位置，负数表示距右边缘/下边缘的距离；video_logo_width=0 用原始宽度。
     * Logo watermark image (png/jpg); empty disables it. Negative x/y mean distance
     * from the right/bottom edge; 0 width keeps the original size.
     */
    /**
     * 负载自适应降帧：编码跟不上时主动降帧（避免卡顿），有余量时自动恢复。
     * adapt_min_fps 是下限，默认 5。
     * adapt_selftest_delay_ms 仅用于自测（人为放大测得耗时以复现过载），生产必须 0。
     */
    /** 优先使用硬件编码器（拿不到或打不开时自动回落软件编码） */
    bool hw_encode = false;
    /**
     * 可用硬件设备列表（如 "0,1"）与指定设备（-1 = 自动按负载分配）。
     * 只在支持设备选择的编码器上生效（NVENC 的 gpu 选项、VAAPI 的 render node）；
     * macOS 的 VideoToolbox 没有设备索引概念，这里会被忽略并在状态里报 -1。
     *
     * Hardware device list ("0,1") and pinned device (-1 = auto, balanced by load).
     * Only meaningful where the encoder supports it (NVENC gpu option, VAAPI render node);
     * VideoToolbox has no device index, so it reports -1.
     */
    std::string hw_devices;
    int hw_device = -1;
    /** 优先使用硬件解码器（拿不到时自动回落软件解码） */
    bool hw_decode = false;

    bool adapt_fps = false;
    float adapt_min_fps = 5;
    int adapt_selftest_delay_ms = 0;

    std::string video_logo;
    int video_logo_x = 10;
    int video_logo_y = 10;
    int video_logo_width = 0;
    /** 是否按需转码：无人观看时跳过，省 CPU */
    bool on_demand = true;
    /** 转码轨静音兜底：源端断流后继续按目标 codec 补静音帧，观众端不断流 */
    bool enable_silence = true;
    /** 静音兜底触发阈值（毫秒）：超过该时长没有新帧输出即开始补静音 */
    int silence_timeout_ms = 200;

    // 必须同时校验编码类型，否则传入视频 codec 时会被音频规则误判
    // Must check the track type too, otherwise a video codec passed to the audio
    // rule gets misclassified as "needs audio transcoding"
    bool needAudioTranscode(CodecId src) const {
        return enable && audio_codec != CodecInvalid && audio_codec != src
               && mediakit::getTrackType(src) == TrackAudio;
    }
    bool needVideoTranscode(CodecId src) const {
        return enable && video_codec != CodecInvalid && video_codec != src
               && mediakit::getTrackType(src) == TrackVideo;
    }

    /**
     * 从配置文件 [transcode] 段读取参数。
     * 每次调用都重新读，所以改 config.ini + reload 后新流即生效，无需重启。
     *
     * Read from the [transcode] section. Re-read on every call, so editing
     * config.ini and reloading takes effect for new streams without a restart.
     */
    static TranscodeOption loadFromIni();
};

/**
 * 转码节点（与 MediaSink 解耦，便于单测）
 *
 * 用法（三个调用点）：
 *  1. track 就绪时：`auto downstream = sink->addTrack(track);` 把 downstream 交给下游 muxer。
 *     需要转码时返回的是"目标轨"（codec 已变），否则原样返回输入轨。
 *  2. 收到帧时：`sink->inputFrame(frame);`，内部决定转码或直接透传。
 *  3. 构造函数里 `setOnOutput` 注册输出回调，转码后的帧和透传的帧都从这里出去。
 *
 * 设计依据（读上游作者公开范式得出，代码独立实现）：
 *  - **换轨不换流**：下游只看到 codec 变了的 track，流 id 不变 → "透明转码"。
 *  - **解码→编码用回调串联**，无中间队列。
 *  - **输出回灌下游 inputFrame**，所以 rtsp/rtmp/hls/webrtc 全部自动具备。
 *  - **按需**：reader 数为 0 时跳过，不解码不编码。
 *
 * Decoupled from MediaSink so it can be unit tested. Three call sites:
 * addTrack at track-ready time, inputFrame per frame, and an output callback.
 */
class TranscodeSink : public std::enable_shared_from_this<TranscodeSink> {
public:
    using Ptr = std::shared_ptr<TranscodeSink>;
    using onOutput = std::function<void(const Frame::Ptr &)>;

    explicit TranscodeSink(const TranscodeOption &option = TranscodeOption(),
                           std::function<size_t()> reader_count = nullptr);
    ~TranscodeSink();

    void setOption(const TranscodeOption &option);
    const TranscodeOption &getOption() const { return _option; }

    /**
     * 给日志/可观测性用的流标识（如 `__defaultVhost__/live/cam1`）。
     * 多路并发转码时，帧数计数日志必须能区分是哪一路，否则
     * `transcoded N frames, index=1` 无法与单路接口返回值对账。
     *
     * Stream label for logs (e.g. `__defaultVhost__/live/cam1`). With several
     * streams transcoding at once an untagged frame count cannot be reconciled
     * per stream. Set by TranscodeRuntime::attachSink().
     */
    void setStreamLabel(std::string label);
    std::string streamLabel() const;

    /** 指定 track 是否正在转码；未转码的轨走原路径，不经本节点 */
    bool isTrackTranscoding(int index) const;

    /** 输出回调：转码后的帧 + 未转码轨的原始帧都从这里出去 */
    void setOnOutput(onOutput cb) { _on_output = std::move(cb); }

    /**
     * 登记一条轨
     * @return 应该交给下游 muxer 的轨：需要转码时是目标轨，否则是入参本身
     */
    Track::Ptr addTrack(const Track::Ptr &track);

    /**
     * 输入一帧
     * @return true 表示该帧已被消费（转码或透传），调用方不应再往下发
     */
    bool inputFrame(const Frame::Ptr &frame);

    /** 声道/尺寸/轨道重置时调用，释放编解码器 */
    void resetTracks();

    /** 已转码帧数，用于观测"确实转了" */
    size_t transcodeFrameCount() const;

    /** 是否至少有一条轨在转码 */
    bool isTranscoding() const;

    /** 转码失败的轨数（配置不当/编码器缺失时非 0） */
    size_t failedTrackCount() const;

    /**
     * 当前已登记的轨是否已经符合给定参数（也就是 setOption 之后能否真正生效）。
     * 已建好的编码器参数不可变，此时改 codec 只能等重推，调用方据此如实回报
     * applied=immediate / pending，不能一律说成功了。
     *
     * Whether the tracks registered so far already match the given option.
     * Created encoders cannot change parameters, so a codec switch needs a
     * republish; the caller uses this to report immediate vs pending honestly.
     */
    bool matchesOption(const TranscodeOption &option) const;

    /**
     * 每条已登记轨的转码状态，供 getTranscode 接口返回明细。
     * 未转码的轨也会出现在列表里（transcoding=false），否则运维看不出「为什么没转」。
     *
     * Per-track status for the getTranscode API. Passthrough tracks are listed
     * too (transcoding=false), otherwise an operator cannot tell why nothing
     * happened.
     */
    struct TrackStatus {
        int index = 0;
        TrackType type = TrackInvalid;
        /** 源轨 codec */
        CodecId source_codec = CodecInvalid;
        /** 目标轨 codec；未转码时等于 source_codec */
        CodecId target_codec = CodecInvalid;
        bool transcoding = false;
        size_t frames = 0;
        /** 实际编码器名（如 libx265 / hevc_videotoolbox）；未转码时为空 */
        std::string encoder;
        /** 是否硬件编码器 —— 运维要能确认「到底用没用 GPU」 */
        bool hardware = false;
        /** 负载自适应当前帧率（0 = 未启用） */
        float adapt_fps = 0;
        /** 因过载被丢弃的帧数 */
        int64_t adapt_dropped = 0;
        /** 解码后送进编码器的输入帧数：与 frames 一起看能区分「输入不够」与「编码跟不上」 */
        size_t input_frames = 0;
        /** 已补出的静音帧数（仅音频） */
        size_t silence = 0;
        /** 解码是否真的走了硬件（依据：有过硬件帧下载），以及该类帧计数 */
        bool hw_decode = false;
        int64_t hw_decode_frames = 0;
        /** 分配到的硬件设备号（-1 = 未指定/该平台不支持设备选择） */
        int hw_device = -1;
        /** 解码后端：ascend(DVPP VDEC) / ffmpeg / none（未转码）—— 现场判断「有没有真上 NPU」 */
        std::string decode_backend;
        /** DVPP VDEC 统计：in=送进去的帧数，out=解出来的帧数，dropped=取到句柄但无有效图像 */
        int64_t vdec_in = 0;
        int64_t vdec_out = 0;
        int64_t vdec_dropped = 0;
        /** DVPP 通道号（-1 = 未占用）；运维排查 16 路通道上限要用 */
        int vdec_channel = -1;
        /** DVPP VENC 统计：sent=送进编码器，out=回调出的包，retries=驱动忙重投次数 */
        int64_t venc_sent = 0;
        int64_t venc_out = 0;
        int64_t venc_retries = 0;
        int venc_channel = -1;
        /** 零拷贝是否启用（VDEC 设备地址直接喂 VENC，不经主机往返） */
        bool zero_copy = false;
        /** 直通成功编码的帧数；>0 才说明真的走了设备直通 */
        int64_t zero_copy_frames = 0;
        /** 直通失败回落的次数（正常应恒为 0） */
        int64_t zero_copy_fallbacks = 0;
    };
    std::vector<TrackStatus> trackStatus() const;

    /** 静音兜底已补出的帧数（判据用：源端断流后该值应持续增长） */
    size_t silenceFrameCount() const;

private:
    struct TrackContext {
        bool transcoding = false;
        TrackType type = TrackInvalid;
        CodecId source_codec = CodecInvalid;
        bool audio = false;
        Track::Ptr target_track;
        std::shared_ptr<FFmpegDecoder> decoder;
        std::shared_ptr<FFmpegEncoder> encoder;
        size_t frame_count = 0;
        /** 解码后送进编码器的输入帧数（据此判断「编码是否跟得上输入」） */
        size_t in_frames = 0;
        /** 已补出的静音帧数 */
        size_t silence_count = 0;
        /** 分配到的硬件设备号（-1 未指定） */
        int hw_device = -1;
        /** 最近一次输出真实帧的时刻(ms)，用于判断源端是否断流 */
        int64_t last_output_ms = 0;
        /** 真实帧是否已产出过；未产出前不补静音，避免抢先于首帧 */
        bool has_output = false;
        /** 已进入静音兜底状态：每个 tick 续补一帧，直到真实帧回来 */
        bool silence_mode = false;
        /** 静音态的"进入沿"，只为打一条进入日志取证用 */
        bool silence_entered = false;
        /** 已补出的静音帧累计覆盖的时长（毫秒），用于按真实时间节奏补帧 */
        int silence_debt_ms = 0;
        /** 正在由兜底路径编码（用于在 onEncoded 里区分真实帧与静音帧） */
        bool in_silence_encode = false;
        /** 该轨一帧的时长（毫秒），决定静音补帧的节奏 */
        int frame_interval_ms = 20;
        /** 零拷贝：解码帧不落主机，直接交给编码器（仅昇腾硬解+硬编、无滤镜/缩放时启用） */
        bool zero_copy = false;
        /** 直通成功编码的帧数（运维判据：>0 说明真走了设备直通） */
        size_t zero_copy_frames = 0;
        /** 直通失败回落的次数（应为 0；持续增长说明有帧在走主机路径） */
        size_t zero_copy_fallbacks = 0;
    };

    /** 建立该轨的 解码器 + 编码器；成功时 target_track 非空 */
    bool setupTrack(const Track::Ptr &track, TrackContext &ctx);
    void teardown(int index);
    bool shouldSkip() const;
    void onDecoded(int index, const FFmpegFrame::Ptr &frame);
#ifdef ENABLE_ASCEND_VENC
    /** 设备帧直通：返回 true 表示已在设备侧编码完（解码器随后释放该帧） */
    bool onDecodedDevice(int index, const AscendVdec::DevFrame &frame);
#endif
    void onEncoded(int index, const Frame::Ptr &frame);

    /**
     * 懒创建静音兜底定时器。
     *
     * 只在首次输出帧时创建，用当前线程的 EventPoller：所有转码输出本来就跑在
     * muxer 的 poller 线程上，定时器挂同一个 poller 才能保证补出来的静音帧与
     * 真实帧串行进入下游，不需要额外加锁。
     *
     * Lazily create the silence timer on the calling (muxer) poller. All transcode
     * output already runs on that thread, so silence frames and real frames reach
     * downstream strictly serialized without extra locking.
     */
    void ensureSilenceTimer();
    void cancelSilenceTimer();

    /** 定时器回调：给断流的转码轨补静音帧 */
    void onSilenceTick();

    /**
     * 标记该轨"上游仍在产出"。
     *
     * 除真实输出帧外，按需跳过（无人观看）也必须调用：没人看时我们有意不转码，
     * 这跟上游断流是两件事，混为一谈会在无人观看时凭空补出大量静音帧。
     *
     * Mark the track as "upstream still producing". Besides real output frames,
     * on-demand skipping must call this too: declining to transcode when nobody
     * watches is not the same as a stalled upstream, and conflating them makes
     * the fallback emit hundreds of pointless silence frames.
     */
    void markTrackActive(int index);

    mutable std::mutex _mtx;
    TranscodeOption _option;
    /** 日志用流标识，见 setStreamLabel() / stream label used in logs */
    std::string _label;
    std::function<size_t()> _reader_count;
    onOutput _on_output;

    /**
     * 输出串行锁。音频与视频的编码完成回调落在不同线程（音频在推流 poller 线程、
     * 视频在 decoder 线程），两者同时写同一个 muxer 的协议缓存会把内部链表写坏，
     * 表现为低概率 SIGSEGV（崩在 RtspMediaSource::onWrite 的链表插入处）。
     *
     * **不能复用 _mtx**：编码过程本身会回调 onEncoded 再取 _mtx，持锁调用会自锁。
     *
     * Serializes output. Audio and video encode callbacks land on different threads
     * (audio on the publish poller thread, video on the decoder thread); writing the
     * same muxer concurrently corrupts its internal packet lists, which showed up as
     * an intermittent SIGSEGV inside RtspMediaSource::onWrite. Must NOT reuse _mtx:
     * encoding re-enters onEncoded and takes _mtx, so holding it would self-deadlock.
     */
    mutable std::mutex _output_mtx;
    std::unordered_map<int, TrackContext> _contexts;
    size_t _failed = 0;
    /** 静音兜底定时器；只有存在转码音频轨时才创建 */
    toolkit::Timer::Ptr _silence_timer;
    /** 定时器检查周期（毫秒），取 silence_timeout_ms 的 1/4，保证补帧足够密 */
    uint64_t _silence_tick_ms = 50;
};

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODE_SINK_H
