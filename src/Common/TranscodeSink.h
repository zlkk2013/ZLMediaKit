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
#include "Extension/Track.h"
#include "Extension/Frame.h"
#include "Codec/Transcoder.h"

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
    /** 目标码率，0 表示沿用源 */
    int bitrate = 0;
    /** 目标采样率（音频），0 表示沿用源 */
    int sample_rate = 0;
    /** 是否按需转码：无人观看时跳过，省 CPU */
    bool on_demand = true;

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
class TranscodeSink {
public:
    using Ptr = std::shared_ptr<TranscodeSink>;
    using onOutput = std::function<void(const Frame::Ptr &)>;

    explicit TranscodeSink(const TranscodeOption &option = TranscodeOption(),
                           std::function<size_t()> reader_count = nullptr);
    ~TranscodeSink();

    void setOption(const TranscodeOption &option);
    const TranscodeOption &getOption() const { return _option; }

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

private:
    struct TrackContext {
        bool transcoding = false;
        Track::Ptr target_track;
        std::shared_ptr<FFmpegDecoder> decoder;
        std::shared_ptr<FFmpegEncoder> encoder;
        size_t frame_count = 0;
    };

    /** 建立该轨的 解码器 + 编码器；成功时 target_track 非空 */
    bool setupTrack(const Track::Ptr &track, TrackContext &ctx);
    void teardown(int index);
    bool shouldSkip() const;
    void onDecoded(int index, const FFmpegFrame::Ptr &frame);
    void onEncoded(int index, const Frame::Ptr &frame);

    mutable std::mutex _mtx;
    TranscodeOption _option;
    std::function<size_t()> _reader_count;
    onOutput _on_output;
    std::unordered_map<int, TrackContext> _contexts;
    size_t _failed = 0;
};

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODE_SINK_H
