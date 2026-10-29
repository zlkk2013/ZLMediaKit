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
#include "Common/Parser.h"   // StrCaseCompare：别名表要不区分大小写
#include "Common/config.h"
#include <algorithm>
#include <atomic>

using namespace std;
using namespace toolkit;

namespace mediakit {

TranscodeSink::TranscodeSink(const TranscodeOption &option, std::function<size_t()> reader_count)
    : _option(option), _reader_count(std::move(reader_count)) {
}

TranscodeSink::~TranscodeSink() {
    cancelSilenceTimer();
    // 不能在持 _mtx 的情况下销毁 TrackContext：解码器析构会 flush 残留帧，flush 会
    // 同步回调进 onDecoded()，那里要拿 _mtx，而非递归锁 → 同一线程自死锁。
    // 实测：推流端断连、延时注销结束时释放 muxer 的输出源，poller 线程就卡在
    // `~FFmpegDecoder → FrameMerger::flush → onDecoded → mutex::lock`，
    // muxer 和它对外发布的源从此永不释放（重推永远 406）。
    // 先把 _contexts 换出来，在锁外停止线程并销毁。
    //
    // Never destroy TrackContext while holding _mtx: the decoder destructor flushes
    // its buffered frames, and that flush synchronously re-enters onDecoded(), which
    // takes _mtx — a non-recursive mutex, so the same thread deadlocks. Measured: the
    // poller thread hung in
    // `~FFmpegDecoder -> FrameMerger::flush -> onDecoded -> mutex::lock` when the
    // muxer's output sources were released after the pusher disconnected, and the
    // muxer plus its published sources were never freed (reconnect always 406).
    // Swap the contexts out first, then stop threads and destroy outside the lock.
    decltype(_contexts) contexts;
    {
        lock_guard<mutex> lck(_mtx);
        contexts.swap(_contexts);
    }
    for (auto &pr : contexts) {
        if (pr.second.decoder) {
            pr.second.decoder->stopThread(true);
        }
        if (pr.second.encoder) {
            pr.second.encoder->stopThread(true);
        }
    }
    contexts.clear();
}

void TranscodeSink::setOption(const TranscodeOption &option) {
    lock_guard<mutex> lck(_mtx);
    _option = option;
}

void TranscodeSink::setStreamLabel(std::string label) {
    lock_guard<mutex> lck(_mtx);
    _label = std::move(label);
}

std::string TranscodeSink::streamLabel() const {
    lock_guard<mutex> lck(_mtx);
    return _label;
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
    lock_guard<mutex> lck(_mtx);
    return _failed;
}

bool TranscodeSink::matchesOption(const TranscodeOption &option) const {
    lock_guard<mutex> lck(_mtx);
    if (!option.enable) {
        // 关掉转码：已有转码轨还在跑，说明配置与实况不符
        for (auto &pr : _contexts) {
            if (pr.second.transcoding) {
                return false;
            }
        }
        return true;
    }
    for (auto &pr : _contexts) {
        auto &ctx = pr.second;
        if (ctx.type == TrackAudio) {
            // 该转的已经在转同一个 codec，且采样率也对得上才算命中；
            // 不该转的轨不能在转
            if (!option.needAudioTranscode(ctx.source_codec)) {
                if (ctx.transcoding) {
                    return false;
                }
                continue;
            }
            if (!ctx.transcoding || !ctx.target_track) {
                return false;
            }
            if (ctx.target_track->getCodecId() != option.audio_codec) {
                return false;
            }
            auto audio = static_pointer_cast<AudioTrack>(ctx.target_track);
            if (option.sample_rate > 0 && audio->getAudioSampleRate() != option.sample_rate) {
                return false;
            }
        } else if (ctx.type == TrackVideo) {
            if (!option.needVideoTranscode(ctx.source_codec)) {
                if (ctx.transcoding) {
                    return false;
                }
                continue;
            }
            if (!ctx.transcoding || !ctx.target_track) {
                return false;
            }
            if (ctx.target_track->getCodecId() != option.video_codec) {
                return false;
            }
        }
    }
    return true;
}

std::vector<TranscodeSink::TrackStatus> TranscodeSink::trackStatus() const {
    lock_guard<mutex> lck(_mtx);
    std::vector<TrackStatus> ret;
    ret.reserve(_contexts.size());
    for (auto &pr : _contexts) {
        TrackStatus st;
        st.index = pr.first;
        st.type = pr.second.type;
        st.source_codec = pr.second.source_codec;
        // 未转码时目标轨就是源轨本身（下游看到的就是源 codec）
        st.target_codec = (pr.second.transcoding && pr.second.target_track)
                              ? pr.second.target_track->getCodecId()
                              : pr.second.source_codec;
        st.transcoding = pr.second.transcoding;
        st.frames = pr.second.frame_count;
        ret.emplace_back(st);
    }
    // 按 track index 排序，让接口输出稳定
    std::sort(ret.begin(), ret.end(), [](const TrackStatus &a, const TrackStatus &b) {
        return a.index < b.index;
    });
    return ret;
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
        int channels = audio->getAudioChannel();
        // 载荷规格写死的 codec（G711 8k/单声道、MP3 44.1k/双声道）必须按规格建目标轨：
        // 否则轨道声明的参数与 RTP payload 表对不上，RTSP 封装找不到 payload 类型，
        // 观众端报 "Decoder (codec none) not found"，音频整条丢失。实测 16k 源转 MP3
        // 就是这样：编码器按 44.1k 出包，轨却声明 16k，整轨被丢。
        // Codecs with a fixed RTP payload (G711 8k/mono, MP3 44.1k/stereo) must build the
        // target track at that spec, otherwise the RTSP muxer cannot map the payload type
        // and the whole audio track is dropped ("Decoder (codec none) not found").
        // Measured: 16kHz source -> MP3 declared 16k while packets were 44.1k.
        forcedAudioSpec(_option.audio_codec, sample_rate, channels);
        target = Factory::getTrackByCodecId(_option.audio_codec, sample_rate,
                                            channels, audio->getAudioSampleBit());
    } else {
        target = Factory::getTrackByCodecId(_option.video_codec);
        if (target) {
            // H265Track/H264Track 无参构造时宽高为 0，但会在 inputFrame 里解析 SPS 自动补齐。
            // 前提是传进去的帧带正确的 prefixSize（见 FFmpegEncoder::setNaluPrefixSize）。
            // H265Track/H264Track start with width/height 0 and fill them by parsing
            // SPS in inputFrame, provided the frames carry the right prefixSize
            // (see FFmpegEncoder::setNaluPrefixSize).
            target->setIndex(track->getIndex());
        }
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
    // 目标轨码率：视频优先用 video_bitrate，音频用通用 bitrate
    // Target track bitrate: video prefers video_bitrate, audio uses the shared one
    if (!is_audio && _option.video_bitrate > 0) {
        target->setBitRate(_option.video_bitrate);
    } else if (_option.bitrate > 0) {
        target->setBitRate(_option.bitrate);
    }

    try {
        ctx.decoder = std::make_shared<FFmpegDecoder>(track);
        // 帧率必须在构造 encoder 之前传给目标轨：FFmpegEncoder 的构造函数里就会
        // avcodec_open2，之后再 setVideoFps 已经无效。
        // The frame rate must be handed to the target track BEFORE constructing the
        // encoder: FFmpegEncoder opens the codec inside its constructor, so setting
        // the frame rate afterwards has no effect.
        float src_fps = 0;
        if (!is_audio) {
            // 帧率和宽高都从源轨读：H265Track 是空构造（宽高为 0，_fps 无 setter），
            // 而编码器必须在 open2 时就拿到尺寸和帧率。
            // Read frame rate and dimensions from the source track: H265Track is
            // default-constructed (width/height 0, no _fps setter) while the encoder
            // needs both at open2 time.
            auto video = static_pointer_cast<VideoTrack>(track);
            src_fps = video->getVideoFps();
            InfoL << "video encoder params: src_fps=" << src_fps
                  << ", src_size=" << video->getVideoWidth() << "x" << video->getVideoHeight()
                  << ", target_size=" << (_option.video_width > 0 ? _option.video_width : video->getVideoWidth())
                  << "x" << (_option.video_height > 0 ? _option.video_height : video->getVideoHeight())
                  << ", target_fps=" << (_option.video_fps > 0 ? _option.video_fps : src_fps)
                  << ", bitrate=" << _option.video_bitrate;
        }
        // thread_num 传 0：不开异步线程。onDecoded 已经是同步调用（解码器线程内），
        // 再叠加编码线程会让 warmup 与真实帧竞争同一个编码器实例，
        // 表现为只有首帧输出、后续帧全部丢失。视频解码本身就是逐帧同步的，
        // 编码留在同一线程更可控。
        // thread_num 0 disables the async encoder thread: onDecoded is already called
        // synchronously on the decoder thread, and layering another thread on top makes
        // warmup race the real frames on the same encoder instance, which showed up as
        // only the first frame being emitted and everything after it lost.
        VideoTargetParams video_params;
        video_params.width = _option.video_width;
        video_params.height = _option.video_height;
        // 分辨率倍数：只有在没给绝对宽高时才生效（显式尺寸比倍数更明确）。
        // YUV420 要求偶数宽高，这里统一向上取偶；源尺寸未知（还没解析到 SPS）时
        // 退回跟随源，等首帧到了再由编码器自己定尺寸。
        //
        // Resolution multiplier, only when no explicit width/height was given.
        // YUV420 needs even dimensions, so round up to even. If the source size is
        // still unknown (SPS not parsed yet), fall back to following the source.
        if (!is_audio && _option.video_scale > 0 && video_params.width <= 0 && video_params.height <= 0) {
            auto v = static_pointer_cast<VideoTrack>(track);
            int sw = v ? v->getVideoWidth() : 0;
            int sh = v ? v->getVideoHeight() : 0;
            if (sw > 0 && sh > 0) {
                auto even = [](double v) {
                    int r = (int) (v / 2.0 + 0.5) * 2;
                    return r < 2 ? 2 : r;
                };
                video_params.width = even(sw * (double) _option.video_scale);
                video_params.height = even(sh * (double) _option.video_scale);
                InfoL << "transcode scale=" << _option.video_scale << ", " << sw << "x" << sh
                      << " -> " << video_params.width << "x" << video_params.height;
            } else {
                WarnL << "transcode scale ignored: source size unknown (" << sw << "x" << sh << ")";
            }
        }
        video_params.fps = _option.video_fps;
        // video_bitrate 优先于通用的 bitrate：同一个进程里音频想保持 64k、
        // 视频想给 1M 是常见需求，共用一个字段做不到。
        // video_bitrate wins over the shared bitrate field: keeping audio at 64k while
        // giving video 1M is a normal requirement and one shared field cannot express it.
        video_params.bitrate = _option.video_bitrate > 0 ? _option.video_bitrate : _option.bitrate;
        ctx.encoder = std::make_shared<FFmpegEncoder>(target, 0, src_fps,
                                                      is_audio ? nullptr : track,
                                                      is_audio ? VideoTargetParams() : video_params);
        // 滤镜必须在喂第一帧前设置：图是按编码器 open2 之后的输出尺寸建的
        if (!is_audio && !_option.video_filter.empty()) {
            ctx.encoder->setVideoFilter(_option.video_filter);
            InfoL << "transcode video filter: " << _option.video_filter;
        }
        if (!is_audio && _option.adapt_fps) {
            ctx.encoder->setAdaptiveFps(true, _option.adapt_min_fps, _option.adapt_selftest_delay_ms);
            InfoL << "transcode adaptive fps: min=" << _option.adapt_min_fps
                  << ( _option.adapt_selftest_delay_ms ? " (selftest delay "
                        + std::to_string(_option.adapt_selftest_delay_ms) + "ms)" : "");
        }
        if (!is_audio && !_option.video_logo.empty()) {
            ctx.encoder->setVideoLogo(_option.video_logo, _option.video_logo_x,
                                      _option.video_logo_y, _option.video_logo_width);
            InfoL << "transcode video logo: " << _option.video_logo << " at "
                  << _option.video_logo_x << "," << _option.video_logo_y;
        }
        if (!is_audio) {
            // 关键：转码轨必须在 addTrack 之前 ready。
            // RtspMuxer/MP4Muxer 的 addTrack 见到 ready()==false 的轨会直接丢弃
            // （日志 "H265 unready!"），而那时代码还没喂过任何帧，SPS 还没解析到。
            // Critical: the transcoded track must be ready before addTrack.
            // RtspMuxer/MP4Muxer discard tracks whose ready() is false
            // ("H265 unready!"), and at this point no frame has been fed yet,
            // so the SPS has not been parsed.
            bool warmed = ctx.encoder->warmup(target);
            if (!warmed) {
                WarnL << "warmup failed, target track not ready, video will be dropped by muxers";
            }
            // 目标轨的宽高在 warmup 喂入 config NAL 后由 SPS 解析得到，
            // 这就是观众端能看到的尺寸，必须与配置核对。
            // The target track's size comes from the SPS parsed during warmup config
            // feeding — that is what viewers will see, so verify it against config.
            auto target_video = static_pointer_cast<VideoTrack>(target);
            InfoL << "transcoded video track ready=" << target_video->ready()
                  << ", size=" << target_video->getVideoWidth() << "x" << target_video->getVideoHeight()
                  << ", fps=" << target_video->getVideoFps();
        }
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
    ctx.audio = is_audio;
    // 一帧的时长：静音兜底靠它决定补帧节奏，与真实帧间隔保持一致，
    // 否则补出来的时间轴会过密（下游判重复）或过疏（播放器卡顿）
    // Frame duration: the silence fallback paces frames with it so the timeline
    // stays at the same density as real frames
    if (is_audio) {
        auto av_ctx = ctx.encoder->getContext();
        if (av_ctx && av_ctx->frame_size > 0 && av_ctx->sample_rate > 0) {
            ctx.frame_interval_ms = (int) (av_ctx->frame_size * 1000LL / av_ctx->sample_rate);
        } else if (av_ctx && av_ctx->sample_rate > 0) {
            // G711 这类变长帧编码器 frame_size 恒为 0，直接落回下面的 20ms 默认值就会
            // 与实际帧长脱节：8kHz 下 160 采样 = 20ms 恰好对，但 48kHz 下同一帧长是
            // 3.3ms，按 20ms 补帧会让静音只补到 1/6 的量。这里按编码器真实采样率算。
            // Variable-frame encoders like G711 always report frame_size 0; falling back
            // to the 20ms default decouples the pacing from the real frame length — 20ms
            // happens to be right for 160 samples at 8kHz, but at 48kHz that frame spans
            // 3.3ms, so padding on a 20ms beat only covers 1/6 of the gap.
            ctx.frame_interval_ms = (int) (ctx.encoder->getAudioFrameMs());
        } else {
            ctx.frame_interval_ms = 20;
        }
    } else {
        auto video = static_pointer_cast<VideoTrack>(track);
        // 用「输出」帧率算帧间隔（静音兜底不用视频，但日志与观测要一致）
        // Use the OUTPUT frame rate (video is never padded, but logs must agree)
        auto fps = _option.video_fps > 0 ? _option.video_fps : video->getVideoFps();
        ctx.frame_interval_ms = fps > 0.1 ? (int) (1000.0f / fps) : 40;
    }
    if (ctx.frame_interval_ms <= 0) {
        ctx.frame_interval_ms = 20;
    }
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
    auto src = track->getCodecId();
    auto type = track->getTrackType();

    lock_guard<mutex> lck(_mtx);
    if (!ok) {
        // 不需要转码的轨不算失败；需要转码但建不起来才算
        bool wanted = _option.needAudioTranscode(src) || _option.needVideoTranscode(src);
        if (wanted) {
            _failed += 1;
            WarnL << "transcode requested but unavailable, fallback to passthrough, stream="
                  << (_label.empty() ? "?" : _label) << ", index=" << index
                  << ", codec=" << getCodecName(src);
        }
        // 透传轨也登记，getTranscode 才能说明「这条轨没转」
        TrackContext passthrough;
        passthrough.transcoding = false;
        passthrough.type = type;
        passthrough.source_codec = src;
        _contexts[index] = std::move(passthrough);
        return track;
    }
    ctx.type = type;
    ctx.source_codec = src;
    _contexts[index] = std::move(ctx);
    InfoL << "transcode track registered, stream=" << (_label.empty() ? "?" : _label)
          << ", index=" << index
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
        // 透传与转码输出共用一把锁：两者可能在不同线程同时进入 muxer
        // Passthrough and transcoded output share the same lock: they can enter
        // the muxer from different threads at the same time
        if (_on_output) {
            lock_guard<mutex> lck(_output_mtx);
            _on_output(frame);
        }
        return true;
    }

    if (shouldSkip()) {
        // 按需：无人观看，不消耗 CPU  [AUTO-TRANSLATED:4f2a6c10]
        // On demand: nobody watching, spend no CPU
        TraceL << "transcode skip (no reader), index=" << index;
        // 关键：跳过转码不等于源端断流。必须刷新该轨的活跃时刻，
        // 否则静音兜底会把"按需跳过"误判成"上游断流"，在无人观看时
        // 白白补出大量静音帧（实测 10 秒补了 400 帧）。
        // Skipping for on-demand is NOT a stall: refresh the track's activity
        // time, otherwise the fallback mistakes on-demand skipping for a stalled
        // upstream and pads hundreds of useless silence frames.
        markTrackActive(index);
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
    bool was_silence = false;
    bool need_arm = false;
    {
        lock_guard<mutex> lck(_mtx);
        auto it = _contexts.find(index);
        if (it != _contexts.end()) {
            it->second.frame_count++;
            was_silence = it->second.in_silence_encode;
            if (was_silence) {
                it->second.silence_count++;
            } else {
                // 只在真实帧回来时刷新：静音帧自己不能把"断流"判据推迟，
                // 否则补帧会无限自我续期，源端恢复后也没法退出静音态
                // Refresh only on real frames: a silence frame must not postpone
                // the stall detection, otherwise the fallback renews itself forever
                it->second.last_output_ms = toolkit::getCurrentMillisecond();
                it->second.has_output = true;
                // 真实帧回来了就退出静音态，避免与源流重复补帧
                // Leave silence mode as soon as a real frame returns
                it->second.silence_mode = false;
                it->second.silence_entered = false;
                need_arm = !_silence_timer;
            }
            // 每 100 帧打一条：便于判断转码链路是否持续产出。
            // 带上流标识：多路并发时按流对账，别把别的流的计数当成这一路的。
            if (it->second.frame_count % 100 == 0) {
                InfoL << "transcoded " << it->second.frame_count << " frames, stream="
                      << (_label.empty() ? "?" : _label) << ", index=" << index
                      << ", dts=" << frame->dts() << ", size=" << frame->size()
                      << ", silence=" << it->second.silence_count;
            }
        }
    }
    if (need_arm) {
        // 首次真实输出之后再装定时器：此刻当前线程就是 muxer 的 poller 线程，
        // 定时器与输出同线程，补帧与真实帧天然串行
        // Arm after the first real output: we're on the muxer poller thread now,
        // so padding and real frames run serialized on the same thread
        ensureSilenceTimer();
    }
    if (_on_output) {
        // 关键：流 id 不变，只是这条轨的 codec 变了 → 透明转码
        // Key point: same stream id, only this track's codec changed
        //
        // 必须串行：音频与视频的 onEncoded 在不同线程，直接并发调用会把 muxer
        // 的协议缓存链表写坏（崩在 RtspMediaSource::onWrite，约 1/3 概率）。
        // 锁只包住回调本身，不包编码，避免与编码过程中的重入自锁。
        // Must serialize: onEncoded runs on different threads for audio and video,
        // and calling the muxer concurrently corrupts its packet lists (crash inside
        // RtspMediaSource::onWrite, roughly 1 in 3 runs). The lock wraps only the
        // callback, never the encode itself, which re-enters onEncoded.
        lock_guard<mutex> lck(_output_mtx);
        _on_output(frame);
    }
}

size_t TranscodeSink::silenceFrameCount() const {
    lock_guard<mutex> lck(_mtx);
    size_t total = 0;
    for (auto &pr : _contexts) {
        total += pr.second.silence_count;
    }
    return total;
}

void TranscodeSink::markTrackActive(int index) {
    lock_guard<mutex> lck(_mtx);
    auto it = _contexts.find(index);
    if (it == _contexts.end()) {
        return;
    }
    it->second.last_output_ms = toolkit::getCurrentMillisecond();
    it->second.has_output = true;
    it->second.silence_mode = false;
    it->second.silence_entered = false;
    it->second.silence_debt_ms = 0;
}

void TranscodeSink::ensureSilenceTimer() {
    if (_silence_timer || !_option.enable_silence) {
        return;
    }
    // 检查周期取超时的 1/4，最少 10ms：200ms 超时 → 50ms 检查一次，
    // 补帧粒度最差 50ms，观众端感知不到断点
    // Tick at 1/4 of the timeout, minimum 10ms: a 200ms timeout checks every 50ms
    auto tick = _option.silence_timeout_ms / 4;
    _silence_tick_ms = (uint64_t) (tick > 10 ? tick : 10);
    weak_ptr<TranscodeSink> weak_self = shared_from_this();
    _silence_timer = std::make_shared<Timer>((float) _silence_tick_ms / 1000.0f, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        strong_self->onSilenceTick();
        return true;
    }, EventPoller::getCurrentPoller());
    InfoL << "transcode silence fallback armed, timeout=" << _option.silence_timeout_ms
          << "ms, tick=" << _silence_tick_ms << "ms";
}

void TranscodeSink::cancelSilenceTimer() {
    _silence_timer = nullptr;
}

void TranscodeSink::onSilenceTick() {
    // 一个 tick 最多补的帧数：poller 被阻塞后恢复时，避免一次爆出大量静音帧
    // Upper bound of frames padded per tick, so a blocked poller cannot burst
    static const int kMaxPadPerTick = 8;

    // 收集本 tick 需要补帧的轨；真正的编码在锁外做，编码器内部有自己的状态
    // 且编码会回调 onEncoded，持锁调用会自锁
    // Collect the tracks to pad; encoding happens outside the lock because it
    // calls back into onEncoded and would deadlock otherwise
    struct PadTask {
        int index;
        std::shared_ptr<FFmpegEncoder> encoder;
        int frames;
    };
    std::vector<PadTask> pending;
    {
        lock_guard<mutex> lck(_mtx);
        if (!_option.enable_silence) {
            return;
        }
        auto timeout = (uint64_t) _option.silence_timeout_ms;
        // 没人看了就没必要继续补静音：补出来的帧只会被丢弃，白烧 CPU。
        // 用与转码同一个按需判据，保持两处语义一致。
        // No viewers means no point padding: the frames are dropped anyway and it
        // burns CPU. Reuse the same on-demand predicate to keep the semantics aligned.
        if (shouldSkip()) {
            for (auto &pr : _contexts) {
                pr.second.silence_mode = false;
                pr.second.silence_entered = false;
                pr.second.silence_debt_ms = 0;
            }
            return;
        }
        for (auto &pr : _contexts) {
            auto &ctx = pr.second;
            if (!ctx.transcoding || !ctx.audio || !ctx.encoder || !ctx.has_output) {
                continue;
            }
            if ((uint64_t) (toolkit::getCurrentMillisecond() - ctx.last_output_ms) < timeout) {
                // 有真实帧在流，清空补帧欠账，下次断流从零开始计时
                // Real frames are flowing: clear the pad debt so the next stall
                // starts counting from zero
                ctx.silence_mode = false;
                ctx.silence_entered = false;
                ctx.silence_debt_ms = 0;
                continue;
            }
            ctx.silence_mode = true;
            if (!ctx.silence_entered) {
                ctx.silence_entered = true;
                // 进入静音兜底时打一条：这是"源端断流、开始补帧"的取证点
                // Log on entry: this is the evidence point for "upstream stalled,
                // padding started"
                InfoL << "silence fallback engaged, index=" << pr.first
                      << ", idle=" << (toolkit::getCurrentMillisecond() - ctx.last_output_ms)
                      << "ms, frame_interval=" << ctx.frame_interval_ms
                      << "ms, real_frames=" << ctx.frame_count;
            }
            // 按真实时间节奏补帧：每 tick 累加 tick 时长，攒够一帧就补一帧。
            // 不是"每 tick 补一帧"，否则 tick 周期(50ms)与帧时长(20ms)不一致，
            // 补出来的时间轴会比真实时间慢 2.5 倍，观众端听到的是加速静音。
            // Pace padding by wall time: accumulate the tick duration and emit a
            // frame per frame_interval. Padding one frame per tick would drift
            // 2.5x slower than real time when tick(50ms) != frame(20ms).
            ctx.silence_debt_ms += (int) _silence_tick_ms;
            int frames = ctx.frame_interval_ms > 0 ? ctx.silence_debt_ms / ctx.frame_interval_ms : 1;
            if (frames > kMaxPadPerTick) {
                frames = kMaxPadPerTick;
            }
            ctx.silence_debt_ms -= frames * ctx.frame_interval_ms;
            if (frames <= 0) {
                continue;
            }
            pending.push_back(PadTask{pr.first, ctx.encoder, frames});
        }
    }
    for (auto &task : pending) {
        for (int i = 0; i < task.frames; ++i) {
            {
                lock_guard<mutex> lck(_mtx);
                auto it = _contexts.find(task.index);
                if (it == _contexts.end() || !it->second.silence_mode) {
                    break;
                }
                it->second.in_silence_encode = true;
            }
            auto ok = task.encoder->inputSilence();
            {
                lock_guard<mutex> lck(_mtx);
                auto it = _contexts.find(task.index);
                if (it != _contexts.end()) {
                    it->second.in_silence_encode = false;
                }
            }
            if (!ok) {
                TraceL << "silence encode failed, index=" << task.index;
                break;
            }
        }
    }
}

void TranscodeSink::teardown(int index) {
    // 不能在持 _mtx 的情况下 flush / 析构：解码器 flush 会同步回调 onDecoded()，
    // 那里要拿 _mtx，非递归锁 ⇒ 同一线程自死锁（与 ~TranscodeSink 完全同一个坑，
    // 那边已修）。这里先把 TrackContext 移出来，锁外再 flush/停线程。
    //
    // 现状：本函数目前没有调用方（resetTracks 走的是别的路径），所以这是潜在陷阱
    // 而不是活跃缺陷；仍按同样模式修掉，避免以后接上调用时踩同一个坑。
    //
    // Never flush or destroy while holding _mtx: a decoder flush re-enters
    // onDecoded() synchronously, which takes _mtx (non-recursive) and deadlocks the
    // calling thread — the exact bug already fixed in ~TranscodeSink. Move the
    // context out first, then flush and stop threads outside the lock.
    //
    // Note: this function currently has no callers (resetTracks takes another path),
    // so this is a latent trap rather than an active defect; fixed anyway so wiring
    // it up later cannot reintroduce the deadlock.
    TrackContext ctx;
    {
        lock_guard<mutex> lck(_mtx);
        auto it = _contexts.find(index);
        if (it == _contexts.end()) {
            return;
        }
        ctx = std::move(it->second);
        _contexts.erase(it);
    }
    if (ctx.decoder) {
        ctx.decoder->flush();
        ctx.decoder->stopThread(true);
    }
    if (ctx.encoder) {
        ctx.encoder->flush();
        ctx.encoder->stopThread(true);
    }
    InfoL << "transcode stopped, index=" << index << ", frames=" << ctx.frame_count;
}

void TranscodeSink::resetTracks() {
    cancelSilenceTimer();
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
        // 与 _contexts 同锁内清零，否则 reset 后接口可能读到「0 条轨但 failed=1」
        _failed = 0;
    }
}

}//namespace mediakit
#endif//ENABLE_FFMPEG
