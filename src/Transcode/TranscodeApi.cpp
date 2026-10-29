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

#include "TranscodeApi.h"
#include "TranscodeConfig.h"
#include "Common/MediaSource.h"
#include "Common/TranscodeSink.h"
#include "Extension/Frame.h"
#include "Util/logger.h"
#include "Util/util.h"
#include <cstdlib>

using namespace std;
using namespace toolkit;

namespace mediakit {

// 与 server/WebApi.h 里 API::ApiErr 的取值保持一致（本文件不依赖 server 层类型）
// Kept in sync with API::ApiErr in server/WebApi.h, without depending on it.
static const int kApiSuccess = 0;
static const int kApiInvalidArgs = -300;
static const int kApiNotFound = -500;

namespace {

/** 取参数值（缺省返回空串，不区分大小写——TranscodeApiArgs 的 key 比较器已处理） */
string argValue(const TranscodeApiArgs &args, const char *key) {
    auto it = args.find(key);
    return it == args.end() ? string() : it->second;
}

/**
 * 组装失败返回。约定：成功时只有 `code=0`（与上游接口一致，不加 msg），
 * 失败时 `code` + `msg` 同 responseApi() 的输出形状。
 *
 * Failure body: `code` + `msg`, matching what server/WebApi.cpp's responseApi()
 * produces. Success carries only `code=0`.
 */
Json::Value makeError(int code, const string &msg) {
    Json::Value ret;
    ret["code"] = code;
    ret["msg"] = msg;
    return ret;
}

/** 解析必填的 vhost/app/stream；vhost 缺省 __defaultVhost__ */
bool buildTuple(const TranscodeApiArgs &args, MediaTuple &tuple, string &err) {
    tuple.vhost = trim(argValue(args, "vhost"));
    if (tuple.vhost.empty()) {
        tuple.vhost = DEFAULT_VHOST;
    }
    tuple.app = trim(argValue(args, "app"));
    tuple.stream = trim(argValue(args, "stream"));
    if (tuple.app.empty() || tuple.stream.empty()) {
        err = "Required parameter missed: app, stream";
        return false;
    }
    return true;
}

/**
 * 解析可选的非负整数参数。
 * @return 1 已设置；0 参数缺省（保持原值）；-1 非法
 *
 * 非数字必须报错而不是静默当成 0：`bitrate=abc` 悄悄变成「沿用源码率」是
 * 最难排查的一类问题。
 */
int parseNonNegative(const TranscodeApiArgs &args, const char *key, int *out) {
    auto raw = trim(argValue(args, key));
    if (raw.empty()) {
        return 0;
    }
    char *end = nullptr;
    long num = ::strtol(raw.data(), &end, 10);
    if (end == raw.data() || *end != '\0' || num < 0) {
        return -1;
    }
    *out = (int) num;
    return 1;
}

/**
 * 解析可选的正浮点参数（分辨率倍数这类）。
 * @return 1 已设置；0 参数缺省；-1 非法
 *
 * 与 parseNonNegative 同样的道理：`scale=abc` 必须报错，
 * 静默当成 0（= 跟随源）会让调用方以为缩放生效了。
 */
int parseNonNegativeFloat(const TranscodeApiArgs &args, const char *key, float *out) {
    auto raw = trim(argValue(args, key));
    if (raw.empty()) {
        return 0;
    }
    char *end = nullptr;
    double num = ::strtod(raw.data(), &end);
    if (end == raw.data() || *end != '\0' || num < 0) {
        return -1;
    }
    *out = (float) num;
    return 1;
}

/** 解析可选的布尔参数；@return 1 已设置；0 缺省；-1 非法 */
int parseBoolArg(const TranscodeApiArgs &args, const char *key, bool *out) {
    auto raw = strToLower(trim(argValue(args, key)));
    if (raw.empty()) {
        return 0;
    }
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") {
        *out = true;
        return 1;
    }
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") {
        *out = false;
        return 1;
    }
    return -1;
}

void writeOption(Json::Value &val, const TranscodeOption &option) {
    val["audio_codec"] = ::mediakit::getCodecName(option.audio_codec);
    val["video_codec"] = ::mediakit::getCodecName(option.video_codec);
    val["bitrate"] = option.bitrate;
    val["sample_rate"] = option.sample_rate;
    val["on_demand"] = option.on_demand;
    val["enable"] = option.enable;
    // 视频侧参数一并回显：只回 codec 会让调用方不知道分辨率/帧率/码率到底生效了没有
    val["video_width"] = option.video_width;
    val["video_height"] = option.video_height;
    val["video_fps"] = option.video_fps;
    val["video_bitrate"] = option.video_bitrate;
    val["video_scale"] = option.video_scale;
}

}//namespace

Json::Value addTranscodeApi(const TranscodeApiArgs &args) {
    MediaTuple tuple;
    string err;
    if (!buildTuple(args, tuple, err)) {
        return makeError(kApiInvalidArgs, err);
    }

    // 在「全局段 + 首条规则 + 已有覆盖」的基础上增量应用请求参数，
    // 这样只传 bitrate 也能继承 ini 里配的 codec
    auto option = TranscodeRuntime::instance().resolveRaw(tuple);

    // 只清覆盖不动 ini：控制面有写就得有撤销手段，否则只能重启才能回到配置值
    bool reset = false;
    auto reset_ret = parseBoolArg(args, "reset", &reset);
    if (reset_ret < 0) {
        return makeError(kApiInvalidArgs, "invalid reset");
    }
    if (reset) {
        bool cleared = TranscodeRuntime::instance().clearOverride(tuple);
        bool online = (bool) MediaSource::find(tuple.vhost, tuple.app, tuple.stream);
        Json::Value val;
        val["code"] = kApiSuccess;
        val["vhost"] = tuple.vhost;
        val["app"] = tuple.app;
        val["stream"] = tuple.stream;
        val["online"] = online;
        val["persistent"] = false;
        val["reset"] = cleared;
        if (!cleared) {
            val["applied"] = "noop";
            val["note"] = "there was no API override for this stream";
        } else if (online) {
            // 正在跑的 sink 参数不可回退（编码器已建好），必须说清楚
            // "stream online but no live sink" is still pending: tracks are already fixed downstream
            val["applied"] = "pending";
            val["note"] = "override removed; the running sink keeps its current encoders until the publisher "
                          "reconnects, so the ini values apply from the next publish";
        } else {
            val["applied"] = "waiting";
            val["note"] = "override removed; the ini values apply when the stream starts publishing";
        }
        InfoL << "addTranscode reset " << tuple.shortUrl() << ", cleared=" << cleared
              << ", applied=" << val["applied"].asString();
        return val;
    }

    struct {
        const char *name;
        TrackType type;
        const char *what;
    } const kCodecArgs[] = {
        {"audio_codec", TrackAudio, "audio"},
        {"video_codec", TrackVideo, "video"},
    };
    bool touched_codec = false;
    for (auto &arg : kCodecArgs) {
        auto value = trim(argValue(args, arg.name));
        if (value.empty()) {
            continue;
        }
        auto codec = parseTranscodeCodec(value, arg.type, arg.what);
        if (codec == CodecInvalid) {
            // 空串/none 已经在上面被跳过，走到这里就是名字不认识或类型不符
            return makeError(kApiInvalidArgs, StrPrinter << "invalid " << arg.name << ": " << value);
        }
        if (arg.type == TrackAudio) {
            option.audio_codec = codec;
        } else {
            option.video_codec = codec;
        }
        touched_codec = true;
    }

    // 数值与布尔参数：三个返回值分别表示「已设置 / 缺省 / 非法」
    bool bitrate_given = false;
    {
        int v = option.bitrate;
        auto ret = parseNonNegative(args, "bitrate", &v);
        if (ret < 0) {
            return makeError(kApiInvalidArgs, "invalid bitrate, must be a non-negative integer");
        }
        bitrate_given = (ret == 1);
        option.bitrate = v;
    }
    bool sample_rate_given = false;
    {
        int v = option.sample_rate;
        auto ret = parseNonNegative(args, "sample_rate", &v);
        if (ret < 0) {
            return makeError(kApiInvalidArgs, "invalid sample_rate, must be a non-negative integer");
        }
        sample_rate_given = (ret == 1);
        option.sample_rate = v;
    }

    // enable 是否显式给出，决定「只给 codec 自动开」与「显式关」的先后
    bool enable_value = option.enable;
    auto enable_ret = parseBoolArg(args, "enable", &enable_value);
    if (enable_ret < 0) {
        return makeError(kApiInvalidArgs, "invalid enable");
    }
    bool enable_given = (enable_ret == 1);
    if (enable_given) {
        option.enable = enable_value;
    }

    // 显式宽高：0 = 跟随源。与 scale 同时给时显式值优先（绝对尺寸更明确）
    // Explicit width/height, 0 = follow source. Wins over scale when both are given.
    bool size_given = false;
    {
        int w = option.video_width, h = option.video_height;
        auto rw = parseNonNegative(args, "width", &w);
        auto rh = parseNonNegative(args, "height", &h);
        if (rw < 0 || rh < 0) {
            return makeError(kApiInvalidArgs, "invalid width/height, must be a non-negative integer");
        }
        // 只给一个会得到「宽变了高没变」的怪尺寸，直接拒绝
        if ((rw == 1) != (rh == 1)) {
            return makeError(kApiInvalidArgs, "width and height must be given together");
        }
        if (rw == 1 && w > 0 && h > 0 && (w % 2 || h % 2)) {
            return makeError(kApiInvalidArgs, "width/height must be even (YUV420)");
        }
        size_given = (rw == 1);
        option.video_width = w;
        option.video_height = h;
    }

    // 分辨率倍数：0 表示取消缩放（回到跟随源），0.1~4.0 有效
    bool scale_given = false;
    {
        float v = option.video_scale;
        auto ret = parseNonNegativeFloat(args, "scale", &v);
        if (ret < 0 || v > 4.0f) {
            return makeError(kApiInvalidArgs, "invalid scale, must be 0 or 0.1~4.0");
        }
        if (ret == 1 && v > 0 && v < 0.1f) {
            return makeError(kApiInvalidArgs, "invalid scale, must be 0 or 0.1~4.0");
        }
        scale_given = (ret == 1);
        option.video_scale = v;
    }

    bool on_demand_given = false;
    {
        bool v = option.on_demand;
        auto ret = parseBoolArg(args, "on_demand", &v);
        if (ret < 0) {
            return makeError(kApiInvalidArgs, "invalid on_demand");
        }
        on_demand_given = (ret == 1);
        option.on_demand = v;
    }

    // 显式给了 codec 就视为要开；显式 enable=0 优先（允许只传 enable=0 关掉某路）
    if (touched_codec && !enable_given) {
        option.enable = true;
    }

    // 一次都没给可执行参数 → 报错，别静默写一条什么也不做的覆盖
    if (!touched_codec && !enable_given && !on_demand_given && !bitrate_given && !sample_rate_given
        && !scale_given && !size_given) {
        return makeError(kApiInvalidArgs,
                         "need at least one of audio_codec/video_codec/bitrate/sample_rate/on_demand/enable/scale/width");
    }
    // 没显式 enable=0，但全局段/规则/本次请求都凑不出目标 codec：
    // 这条覆盖永远转不了任何东西，报错比留一条空覆盖好排查
    if (option.enable && option.audio_codec == CodecInvalid && option.video_codec == CodecInvalid) {
        return makeError(kApiInvalidArgs, "need at least one of audio_codec/video_codec to enable transcoding");
    }
    if (!enable_given && option.audio_codec == CodecInvalid && option.video_codec == CodecInvalid) {
        return makeError(kApiInvalidArgs,
                         "no target codec configured for this stream; pass audio_codec and/or video_codec");
    }

    bool online = (bool) MediaSource::find(tuple.vhost, tuple.app, tuple.stream);
    auto sink = TranscodeRuntime::instance().findSink(tuple);

    // 覆盖表先写：即使运行中的流无法热切换，下次推流也一定按新参数走
    TranscodeRuntime::instance().setOverride(tuple, option);

    // 已建好的编码器参数不可变：只有已登记的轨本来就符合新参数时才算「立即生效」
    // （on_demand / bitrate 这类不重建编码器的项能热切换，改 codec 不能）
    bool immediate = false;
    if (sink) {
        immediate = sink->matchesOption(option);
        sink->setOption(option);
    }

    Json::Value val;
    val["code"] = kApiSuccess;
    val["vhost"] = tuple.vhost;
    val["app"] = tuple.app;
    val["stream"] = tuple.stream;
    val["online"] = online;
    // 覆盖参数只活在内存里，重启即回到 ini。显式告知，避免调用方以为已持久化。
    val["persistent"] = false;
    writeOption(val, option);
    val["config_source"] = "api";
    if (sink && immediate) {
        val["applied"] = "immediate";
        val["note"] = "codec/sample_rate already match the running sink; other parameters "
                      "(on_demand/bitrate) were pushed to it and apply now. Not persisted: "
                      "a restart reverts to the ini file";
    } else if (sink) {
        val["applied"] = "pending";
        val["note"] = "the running transcode sink keeps its current codec/sample_rate until the stream is "
                      "republished; on_demand/bitrate changes apply immediately. Not persisted: a restart "
                      "reverts to the ini file";
    } else if (online) {
        val["applied"] = "pending";
        val["note"] = "stream is online but was not transcoding when its tracks became ready; "
                      "takes effect after the publisher reconnects. Not persisted: a restart reverts to "
                      "the ini file";
    } else {
        val["applied"] = "waiting";
        val["note"] = "stream is offline; takes effect when it starts publishing. Not persisted: a "
                      "restart reverts to the ini file";
    }

    InfoL << "addTranscode " << tuple.shortUrl() << ", applied=" << val["applied"].asString()
          << ", audio=" << val["audio_codec"].asString() << ", video=" << val["video_codec"].asString()
          << ", bitrate=" << option.bitrate << ", on_demand=" << option.on_demand
          << ", enable=" << option.enable;
    return val;
}

Json::Value getTranscodeApi(const TranscodeApiArgs &args) {
    MediaTuple tuple;
    string err;
    if (!buildTuple(args, tuple, err)) {
        return makeError(kApiInvalidArgs, err);
    }

    string source;
    auto option = TranscodeRuntime::instance().resolve(tuple, &source);
    bool online = (bool) MediaSource::find(tuple.vhost, tuple.app, tuple.stream);
    auto sink = TranscodeRuntime::instance().findSink(tuple);
    TranscodeOption over;
    bool has_override = TranscodeRuntime::instance().getOverride(tuple, &over);

    if (!online && !sink && !has_override && source == "off") {
        // 流不在线、没有 sink、ini 也没有任何规则命中：这条流对我们完全不存在
        return makeError(kApiNotFound, "can not find the stream");
    }

    Json::Value val;
    val["code"] = kApiSuccess;
    val["vhost"] = tuple.vhost;
    val["app"] = tuple.app;
    val["stream"] = tuple.stream;
    val["online"] = online;
    // `persistent=false` 表示 API 覆盖不落盘，重启回到 ini。恒为 false：我们不落盘。
    val["persistent"] = false;
    val["has_override"] = has_override;
    val["config_source"] = source;
    // 配置目标 = 下次推流应达到的目标；此刻真正在转什么看 tracks[].target_codec
    writeOption(val, option);

    val["transcoding"] = sink ? sink->isTranscoding() : false;
    val["frames"] = sink ? (Json::UInt64) sink->transcodeFrameCount() : 0;
    val["failed_tracks"] = sink ? (Json::UInt64) sink->failedTrackCount() : 0;
    val["silence_frames"] = sink ? (Json::UInt64) sink->silenceFrameCount() : 0;

    if (sink) {
        for (auto &st : sink->trackStatus()) {
            Json::Value track;
            track["index"] = st.index;
            track["type"] = getTrackString(st.type);
            track["transcoding"] = st.transcoding;
            track["source_codec"] = ::mediakit::getCodecName(st.source_codec);
            track["target_codec"] = ::mediakit::getCodecName(st.target_codec);
            track["frames"] = (Json::UInt64) st.frames;
            val["tracks"].append(track);
        }
    } else if (online) {
        val["msg"] = "stream is online but transcode is not enabled for it";
    } else {
        // 离线：可能是有覆盖但在等推流，也可能是覆盖被 enable=0 关掉了
        val["msg"] = option.enable
                     ? "stream is offline; the parameters above apply when it starts publishing"
                     : "an override exists but transcode is disabled for this stream";
    }
    return val;
}

}//namespace mediakit
#endif//ENABLE_FFMPEG
