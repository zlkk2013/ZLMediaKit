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

#include "TranscodeSink.h"
#include "Extension/Factory.h"
#include "Util/util.h"
#include "Common/config.h"
#include <atomic>

using namespace std;
using namespace toolkit;

namespace mediakit {

TranscodeOption TranscodeOption::loadFromIni() {
    TranscodeOption option;
    auto &ini = mINI::Instance();
    // 未配置 [transcode] 段时 getValue 返回默认值，行为与上游完全一致
    // Without the [transcode] section, defaults are returned and behavior matches upstream
    option.enable = ini[kTranscodeEnable].as<bool>();
    if (!option.enable) {
        return option;
    }
    option.audio_codec = getCodecId(ini[kTranscodeAudioCodec].as<std::string>());
    option.video_codec = getCodecId(ini[kTranscodeVideoCodec].as<std::string>());
    option.bitrate = ini[kTranscodeBitrate].as<int>();
    option.sample_rate = ini[kTranscodeSampleRate].as<int>();
    option.on_demand = ini[kTranscodeOnDemand].as<bool>();
    if (option.audio_codec == CodecInvalid && option.video_codec == CodecInvalid) {
        WarnL << "transcode enabled but no target codec configured, disabled";
        option.enable = false;
    }
    return option;
}

TranscodeSink::TranscodeSink(const TranscodeOption &option, std::function<size_t()> reader_count)
    : _option(option), _reader_count(std::move(reader_count)) {
}

TranscodeSink::~TranscodeSink() {
    lock_guard<mutex> lck(_mtx);
    for (auto &pr : _contexts) {
        if (pr.second.decoder) {
            pr.second.decoder->stopThread(true);
        }
        if (pr.second.encoder) {
            pr.second.encoder->stopThread(true);
        }
    }
    _contexts.clear();
}

void TranscodeSink::setOption(const TranscodeOption &option) {
    lock_guard<mutex> lck(_mtx);
    _option = option;
}

size_t TranscodeSink::transcodeFrameCount() const {
    lock_guard<mutex> lck(_mtx);
    size_t total = 0;
    for (auto &pr : _contexts) {
        total += pr.second.frame_count;
    }
    return total;
}

bool TranscodeSink::isTranscoding() const {
    lock_guard<mutex> lck(_mtx);
    for (auto &pr : _contexts) {
        if (pr.second.transcoding) {
            return true;
        }
    }
    return false;
}

size_t TranscodeSink::failedTrackCount() const {
    return _failed;
}

bool TranscodeSink::shouldSkip() const {
    if (!_option.on_demand || !_reader_count) {
        return false;
    }
    auto readers = _reader_count();
    // 每 200 次打一条，避免刷日志；用于确认按需判断真的读到了观众数
    // Log every 200 calls to confirm the reader count is actually observed
    static std::atomic<uint64_t> calls{0};
    if (++calls % 200 == 0) {
        InfoL << "on-demand check: readers=" << readers;
    }
    return readers == 0;
}

bool TranscodeSink::setupTrack(const Track::Ptr &track, TrackContext &ctx) {
    auto src_codec = track->getCodecId();
    bool is_audio = (track->getTrackType() == TrackAudio);
    if (is_audio) {
        if (!_option.needAudioTranscode(src_codec)) {
            return false;
        }
    } else if (track->getTrackType() == TrackVideo) {
        if (!_option.needVideoTranscode(src_codec)) {
            return false;
        }
    } else {
        return false;
    }

    // 造目标轨：codec 变了，其余参数尽量沿用  [AUTO-TRANSLATED:2b7a1c02]
    // Build the target track: codec changes, other params carry over where possible
    Track::Ptr target;
    if (is_audio) {
        auto audio = static_pointer_cast<AudioTrack>(track);
        int sample_rate = _option.sample_rate > 0 ? _option.sample_rate : audio->getAudioSampleRate();
        target = Factory::getTrackByCodecId(_option.audio_codec, sample_rate,
                                            audio->getAudioChannel(), audio->getAudioSampleBit());
    } else {
        target = Factory::getTrackByCodecId(_option.video_codec);
    }
    if (!target) {
        WarnL << "cannot create target track, codec=" << getCodecName(
                is_audio ? _option.audio_codec : _option.video_codec);
        return false;
    }
    // 目标轨必须继承源轨的 index：下游 muxer 全部按 frame->getIndex() 分发，
    // 新建的 track index 默认 0，音频轨会与视频轨撞号，导致帧被丢
    // The target track must inherit the source index: downstream muxers dispatch
    // by frame->getIndex(), a fresh track defaults to 0 and collides with video
    target->setIndex(track->getIndex());
    if (_option.bitrate > 0) {
        target->setBitRate(_option.bitrate);
    }

    try {
        ctx.decoder = std::make_shared<FFmpegDecoder>(track);
        ctx.encoder = std::make_shared<FFmpegEncoder>(target);
    } catch (std::exception &ex) {
        WarnL << "transcode setup failed for " << getCodecName(src_codec) << " -> "
              << getCodecName(is_audio ? _option.audio_codec : _option.video_codec)
              << " : " << ex.what();
        ctx.decoder = nullptr;
        ctx.encoder = nullptr;
        return false;
    }

    int index = track->getIndex();
    ctx.encoder->setOnEncode([this, index](const Frame::Ptr &frame) {
        onEncoded(index, frame);
    });
    ctx.decoder->setOnDecode([this, index](const FFmpegFrame::Ptr &frame) {
        onDecoded(index, frame);
    });
    ctx.target_track = target;
    ctx.transcoding = true;
    return true;
}

Track::Ptr TranscodeSink::addTrack(const Track::Ptr &track) {
    if (!track) {
        return nullptr;
    }
    if (!_option.enable) {
        return track;
    }

    TrackContext ctx;
    bool ok = setupTrack(track, ctx);
    int index = track->getIndex();

    lock_guard<mutex> lck(_mtx);
    if (!ok) {
        // 不需要转码的轨不算失败；需要转码但建不起来才算
        auto src = track->getCodecId();
        bool wanted = _option.needAudioTranscode(src) || _option.needVideoTranscode(src);
        if (wanted) {
            _failed += 1;
            WarnL << "transcode requested but unavailable, fallback to passthrough, index="
                  << index << ", codec=" << getCodecName(src);
        }
        return track;
    }
    _contexts[index] = std::move(ctx);
    InfoL << "transcode track registered, index=" << index
          << ", " << getCodecName(track->getCodecId())
          << " -> " << getCodecName(_contexts[index].target_track->getCodecId());
    return _contexts[index].target_track;
}

bool TranscodeSink::isTrackTranscoding(int index) const {
    lock_guard<mutex> lck(_mtx);
    auto it = _contexts.find(index);
    return it != _contexts.end() && it->second.transcoding;
}

bool TranscodeSink::inputFrame(const Frame::Ptr &frame) {
    if (!frame) {
        return false;
    }
    int index = frame->getIndex();

    std::shared_ptr<FFmpegDecoder> decoder;
    {
        lock_guard<mutex> lck(_mtx);
        auto it = _contexts.find(index);
        if (it != _contexts.end() && it->second.transcoding) {
            decoder = it->second.decoder;
        }
    }

    if (!decoder) {
        // 不转码：直接透传出去  [AUTO-TRANSLATED:0e7b8d21]
        // Not transcoding: emit unchanged
        if (_on_output) {
            _on_output(frame);
        }
        return true;
    }

    if (shouldSkip()) {
        // 按需：无人观看，不消耗 CPU  [AUTO-TRANSLATED:4f2a6c10]
        // On demand: nobody watching, spend no CPU
        TraceL << "transcode skip (no reader), index=" << index;
        return true;
    }

    bool is_video = (frame->getTrackType() == TrackVideo);
    // 音频同步解码保序，视频异步  [AUTO-TRANSLATED:6d5d1b0e]
    return decoder->inputFrame(frame, true, is_video);
}

void TranscodeSink::onDecoded(int index, const FFmpegFrame::Ptr &frame) {
    std::shared_ptr<FFmpegEncoder> encoder;
    {
        lock_guard<mutex> lck(_mtx);
        auto it = _contexts.find(index);
        if (it == _contexts.end() || !it->second.encoder) {
            return;
        }
        encoder = it->second.encoder;
    }
    // 编码一律同步：保证输出帧序与输入一致  [AUTO-TRANSLATED:8a2f4c19]
    encoder->inputFrame(frame, false);
}

void TranscodeSink::onEncoded(int index, const Frame::Ptr &frame) {
    if (!frame) {
        return;
    }
    {
        lock_guard<mutex> lck(_mtx);
        auto it = _contexts.find(index);
        if (it != _contexts.end()) {
            it->second.frame_count++;
            // 每 100 帧打一条：便于判断转码链路是否持续产出
            // Log every 100 frames so we can tell the chain keeps producing
            if (it->second.frame_count % 100 == 0) {
                InfoL << "transcoded " << it->second.frame_count << " frames, index=" << index
                      << ", dts=" << frame->dts() << ", size=" << frame->size();
            }
        }
    }
    if (_on_output) {
        // 关键：流 id 不变，只是这条轨的 codec 变了 → 透明转码
        // Key point: same stream id, only this track's codec changed
        _on_output(frame);
    }
}

void TranscodeSink::teardown(int index) {
    lock_guard<mutex> lck(_mtx);
    auto it = _contexts.find(index);
    if (it == _contexts.end()) {
        return;
    }
    if (it->second.decoder) {
        it->second.decoder->flush();
        it->second.decoder->stopThread(true);
    }
    if (it->second.encoder) {
        it->second.encoder->flush();
        it->second.encoder->stopThread(true);
    }
    InfoL << "transcode stopped, index=" << index << ", frames=" << it->second.frame_count;
    _contexts.erase(it);
}

void TranscodeSink::resetTracks() {
    {
        lock_guard<mutex> lck(_mtx);
        for (auto &pr : _contexts) {
            if (pr.second.decoder) {
                pr.second.decoder->stopThread(true);
            }
            if (pr.second.encoder) {
                pr.second.encoder->stopThread(true);
            }
        }
        _contexts.clear();
    }
    _failed = 0;
}

}//namespace mediakit
#endif//ENABLE_FFMPEG
