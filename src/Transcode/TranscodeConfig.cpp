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

#include "TranscodeConfig.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Extension/Frame.h"
#include "Util/mini.h"
#include "Util/util.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <set>

using namespace std;
using namespace toolkit;

// 转码配置键：定义在本文件而不是上游 config.h/config.cpp —— 全仓只有这里引用它们，
// 放在这儿能让上游文件的改动量最小，长期 rebase 冲突面更小。
// Transcode keys live here (referenced only from this file) to keep the upstream
// diff minimal.
namespace {
const std::string kTranscodeEnable = "transcode.enable";
const std::string kTranscodeAudioCodec = "transcode.audio_codec";
const std::string kTranscodeVideoCodec = "transcode.video_codec";
const std::string kTranscodeBitrate = "transcode.bitrate";
const std::string kTranscodeSampleRate = "transcode.sample_rate";
const std::string kTranscodeOnDemand = "transcode.on_demand";
const std::string kTranscodeEnableSilence = "transcode.enable_silence";
const std::string kTranscodeSilenceTimeoutMS = "transcode.silence_timeout_ms";
const std::string kTranscodeVideoWidth = "transcode.video_width";
const std::string kTranscodeVideoHeight = "transcode.video_height";
const std::string kTranscodeVideoFps = "transcode.video_fps";
const std::string kTranscodeVideoBitrate = "transcode.video_bitrate";
const std::string kTranscodeVideoScale = "transcode.video_scale";
const std::string kTranscodeVideoFilter = "transcode.video_filter";
const std::string kTranscodeVideoLogo = "transcode.video_logo";
const std::string kTranscodeVideoLogoX = "transcode.video_logo_x";
const std::string kTranscodeVideoLogoY = "transcode.video_logo_y";
const std::string kTranscodeVideoLogoWidth = "transcode.video_logo_width";
const std::string kTranscodeHwEncode = "transcode.hw_encode";
const std::string kTranscodeHwDecode = "transcode.hw_decode";
const std::string kTranscodeHwDevices = "transcode.hw_devices";
const std::string kTranscodeHwDevice = "transcode.hw_device";
const std::string kTranscodeAdaptFps = "transcode.adapt_fps";
const std::string kTranscodeAdaptMinFps = "transcode.adapt_min_fps";
const std::string kTranscodeAdaptSelftestDelay = "transcode.adapt_selftest_delay_ms";
}

namespace mediakit {

// ─────────────────────────────────────────────────────────────────────────────
// 编码名解析 / Codec name parsing
// ─────────────────────────────────────────────────────────────────────────────

// getCodecId() 只认 CodecId 的标称名（AAC 的标称名是 mpeg4-generic、G711A 是 PCMA），
// 配置里写 aac/g711a 这类常见叫法必须另行映射，否则用户配了等于没配。
static const map<string, CodecId, StrCaseCompare> &codecAlias() {
    static const map<string, CodecId, StrCaseCompare> s_map = {
        {"aac",           CodecAAC},
        {"mpeg4-generic", CodecAAC},
        {"opus",          CodecOpus},
        {"g711a",         CodecG711A},
        {"pcma",          CodecG711A},
        {"g711u",         CodecG711U},
        {"pcmu",          CodecG711U},
        {"l16",           CodecL16},
        {"mp3",           CodecMP3},
        {"g722",          CodecG722},
        {"adpcm-g722",    CodecG722},
        {"h264",          CodecH264},
        {"avc",           CodecH264},
        {"h265",          CodecH265},
        {"hevc",          CodecH265},
        {"h266",          CodecH266},
        {"vp8",           CodecVP8},
        {"vp9",           CodecVP9},
        {"av1",           CodecAV1},
    };
    return s_map;
}

CodecId parseTranscodeCodec(const string &name, TrackType expect, const char *what) {
    auto lower = strToLower(trim(string(name)));
    // 空串 / none 表示「不转这一类」，不算错误
    if (lower.empty() || lower == "none" || lower == "invalid") {
        return CodecInvalid;
    }
    CodecId codec;
    auto it = codecAlias().find(lower);
    if (it != codecAlias().end()) {
        codec = it->second;
    } else {
        codec = getCodecId(lower);
    }
    if (codec == CodecInvalid) {
        // 用 Error 而不是 Warn：认不出 codec 会让这条轨静默不转码，
        // 表现为「配置写了却没生效」，是最难排查的一类问题。
        // Error rather than Warn: an unrecognised codec silently skips transcoding
        // for that track, which looks exactly like "the config had no effect".
        ErrorL << "unknown transcode " << what << " codec: '" << name
               << "'. Accepted: PCMA/g711a, PCMU/g711u, AAC/mpeg4-generic, MP3, opus, L16, "
                  "H264/avc, H265/hevc, H266, VP8, VP9, AV1; empty means don't transcode this kind";
        return CodecInvalid;
    }
    if (getTrackType(codec) != expect) {
        WarnL << "transcode " << what << " codec '" << name << "' is not a "
              << (expect == TrackAudio ? "audio" : "video") << " codec, ignored";
        return CodecInvalid;
    }
    return codec;
}

static int parseInt(const string &value, int fallback) {
    auto str = trim(string(value));
    if (str.empty()) {
        return fallback;
    }
    char *end = nullptr;
    long ret = ::strtol(str.data(), &end, 10);
    if (end == str.data() || ret < 0) {
        WarnL << "invalid transcode number: '" << value << "', keep " << fallback;
        return fallback;
    }
    return (int) ret;
}

/** 允许负数（logo 的 x/y 用负值表示「距右/下边缘」） */
static int parseSignedInt(const string &value, int fallback) {
    auto str = trim(string(value));
    if (str.empty()) {
        return fallback;
    }
    char *end = nullptr;
    long ret = ::strtol(str.data(), &end, 10);
    if (end == str.data() || *end != '\0') {
        WarnL << "invalid transcode number: '" << value << "', keep " << fallback;
        return fallback;
    }
    return (int) ret;
}

static float parseFloat(const string &value, float fallback) {
    auto str = trim(string(value));
    if (str.empty()) {
        return fallback;
    }
    char *end = nullptr;
    double ret = ::strtod(str.data(), &end);
    if (end == str.data() || ret < 0) {
        WarnL << "invalid transcode number: '" << value << "', keep " << fallback;
        return fallback;
    }
    return (float) ret;
}

static bool parseBool(const string &value, bool fallback) {
    auto str = strToLower(trim(string(value)));
    if (str.empty()) {
        return fallback;
    }
    if (str == "1" || str == "true" || str == "yes" || str == "on") {
        return true;
    }
    if (str == "0" || str == "false" || str == "no" || str == "off") {
        return false;
    }
    WarnL << "invalid transcode bool: '" << value << "', keep " << fallback;
    return fallback;
}

// ─────────────────────────────────────────────────────────────────────────────
// 全局段 / 按流规则
// ─────────────────────────────────────────────────────────────────────────────

bool applyTranscodeItems(const map<string, string> &items, TranscodeOption &option, string *err) {
    bool ok = true;
    auto fail = [&](const string &msg) {
        WarnL << msg;
        if (err && ok) {
            *err = msg;
        }
        ok = false;
    };

    for (auto &pr : items) {
        auto &key = pr.first;
        auto &value = pr.second;
        if (key == "enable") {
            option.enable = parseBool(value, option.enable);
        } else if (key == "audio_codec") {
            option.audio_codec = parseTranscodeCodec(value, TrackAudio, "audio");
        } else if (key == "video_codec") {
            option.video_codec = parseTranscodeCodec(value, TrackVideo, "video");
        } else if (key == "bitrate" || key == "sample_rate") {
            auto str = trim(string(value));
            char *end = nullptr;
            long num = str.empty() ? 0 : ::strtol(str.data(), &end, 10);
            if (!str.empty() && (end == str.data() || *end != '\0' || num < 0)) {
                fail("invalid transcode " + key + ": '" + value + "'");
                continue;
            }
            if (key == "bitrate") {
                option.bitrate = (int) num;
            } else {
                option.sample_rate = (int) num;
            }
        } else if (key == "on_demand") {
            option.on_demand = parseBool(value, option.on_demand);
        } else if (key == "video_scale") {
            auto str = trim(string(value));
            char *end = nullptr;
            double num = str.empty() ? 0 : ::strtod(str.data(), &end);
            if (!str.empty() && (end == str.data() || *end != '\0' || num < 0)) {
                fail("invalid transcode video_scale: '" + value + "'");
                continue;
            }
            option.video_scale = (float) num;
        } else {
            WarnL << "unknown transcode option '" << key << "', ignored";
        }
    }
    return ok;
}

TranscodeOption loadGlobalTranscodeOption() {
    TranscodeOption option;
    auto &ini = mINI::Instance();
    // 未配置 [transcode] 段时 getValue 返回默认值，行为与上游完全一致
    // Without the [transcode] section, defaults are returned and behavior matches upstream
    option.enable = ini[kTranscodeEnable].as<bool>();
    option.audio_codec = parseTranscodeCodec(ini[kTranscodeAudioCodec].as<string>(), TrackAudio, "audio");
    option.video_codec = parseTranscodeCodec(ini[kTranscodeVideoCodec].as<string>(), TrackVideo, "video");
    option.bitrate = parseInt(ini[kTranscodeBitrate].as<string>(), 0);
    option.sample_rate = parseInt(ini[kTranscodeSampleRate].as<string>(), 0);
    // 视频输出参数：全 0 表示跟随源，行为与未引入这些配置项时一致
    // Video output params: all-zero follows the source, matching earlier behavior
    option.video_width = parseInt(ini[kTranscodeVideoWidth].as<string>(), 0);
    option.video_height = parseInt(ini[kTranscodeVideoHeight].as<string>(), 0);
    option.video_fps = (float) parseInt(ini[kTranscodeVideoFps].as<string>(), 0);
    option.video_bitrate = parseInt(ini[kTranscodeVideoBitrate].as<string>(), 0);
    // 分辨率倍数：与 video_width/height 互斥，显式尺寸优先（见 TranscodeOption 注释）
    // Resolution multiplier; explicit width/height take precedence (see TranscodeOption)
    option.video_scale = parseFloat(ini[kTranscodeVideoScale].as<string>(), 0);
    // 滤镜串：原样保留（libavfilter 自己会校验语法），只有空串表示不启用
    option.video_filter = trim(ini[kTranscodeVideoFilter].as<string>());
    // logo 角标：路径可为空（不叠加）；x/y 允许负数（距右/下边缘）
    option.video_logo = trim(ini[kTranscodeVideoLogo].as<string>());
    option.video_logo_x = parseSignedInt(ini[kTranscodeVideoLogoX].as<string>(), 10);
    option.video_logo_y = parseSignedInt(ini[kTranscodeVideoLogoY].as<string>(), 10);
    option.video_logo_width = parseInt(ini[kTranscodeVideoLogoWidth].as<string>(), 0);
    // 负载自适应降帧：默认关闭，保持与未引入该特性时行为一致（基线回归靠这个）
    // 硬件编码：默认关闭，与旧行为一致；拿不到硬件编码器时自动回落软编
    option.hw_encode = parseBool(ini[kTranscodeHwEncode].as<string>(), false);
    option.hw_decode = parseBool(ini[kTranscodeHwDecode].as<string>(), false);
    // 多卡：设备列表 + 指定设备（-1 自动）
    option.hw_devices = trim(ini[kTranscodeHwDevices].as<string>());
    option.hw_device = parseSignedInt(ini[kTranscodeHwDevice].as<string>(), -1);
    option.adapt_fps = parseBool(ini[kTranscodeAdaptFps].as<string>(), false);
    option.adapt_min_fps = parseFloat(ini[kTranscodeAdaptMinFps].as<string>(), 5);
    option.adapt_selftest_delay_ms = parseInt(ini[kTranscodeAdaptSelftestDelay].as<string>(), 0);
    // 静音兜底的两项：P6 引入，重构到本文件时曾漏读，导致配置文件里改它们不生效
    // Silence fallback settings (from P6). The refactor into this file once dropped
    // these, so editing them in the ini had no effect at all.
    option.enable_silence = parseBool(ini[kTranscodeEnableSilence].as<string>(), true);
    option.silence_timeout_ms = parseInt(ini[kTranscodeSilenceTimeoutMS].as<string>(), 200);
    if (option.silence_timeout_ms <= 0) {
        option.silence_timeout_ms = 200;
    }
    option.on_demand = parseBool(ini[kTranscodeOnDemand].as<string>(), true);
    return option;
}

TranscodeOption TranscodeOption::loadFromIni() {
    auto option = loadGlobalTranscodeOption();
    if (!option.enable) {
        return option;
    }
    if (option.audio_codec == CodecInvalid && option.video_codec == CodecInvalid) {
        // 按流规则可能补上 codec，因此这里只提示不关闭；
        // resolve() 在合并完规则后才做最终判断。
        TraceL << "transcode enabled but global section has no target codec";
    }
    return option;
}

bool StreamRule::match(const MediaTuple &tuple) const {
    if (!vhost.empty() && vhost != tuple.vhost) {
        return false;
    }
    return app == tuple.app && (stream.empty() || stream == tuple.stream);
}

string StreamRule::targetKey() const {
    if (vhost.empty()) {
        return app + "/" + stream;
    }
    return vhost + "/" + app + "/" + stream;
}

bool TranscodeRuntime::parseRule(const string &text, StreamRule &rule) {
    auto body = trim(string(text));
    if (body.empty()) {
        return false;
    }
    auto pos = body.find(':');
    if (pos == string::npos) {
        WarnL << "invalid transcode stream_rule (missing ':'): '" << text << "'";
        return false;
    }
    auto target = trim(body.substr(0, pos));
    auto items_str = trim(body.substr(pos + 1));
    if (target.empty() || items_str.empty()) {
        WarnL << "invalid transcode stream_rule (empty target or items): '" << text << "'";
        return false;
    }

    // target 可以是 app/stream，也可以是 vhost/app/stream
    vector<string> parts;
    for (auto &part : split(target, "/")) {
        auto one = trim(string(part));
        if (!one.empty()) {
            parts.emplace_back(one);
        }
    }
    if (parts.size() == 2) {
        rule.app = parts[0];
        rule.stream = parts[1];
    } else if (parts.size() == 3) {
        rule.vhost = parts[0];
        rule.app = parts[1];
        rule.stream = parts[2];
    } else {
        WarnL << "invalid transcode stream_rule target (need app/stream or vhost/app/stream): '"
              << target << "'";
        return false;
    }

    static const set<string> kKnown = {
        "enable", "audio_codec", "video_codec", "bitrate", "sample_rate", "on_demand"};
    for (auto &item : split(items_str, ",")) {
        auto kv = trim(string(item));
        if (kv.empty()) {
            continue;
        }
        auto eq = kv.find('=');
        if (eq == string::npos) {
            WarnL << "invalid transcode stream_rule item (missing '='): '" << kv << "'";
            continue;
        }
        auto key = strToLower(trim(kv.substr(0, eq)));
        if (key.empty()) {
            continue;
        }
        if (kKnown.find(key) == kKnown.end()) {
            WarnL << "unknown transcode stream_rule key '" << key << "' in '" << text << "', ignored";
            continue;
        }
        rule.items[key] = trim(kv.substr(eq + 1));
    }
    return !rule.items.empty();
}

vector<StreamRule> TranscodeRuntime::loadRulesFromIni() {
    vector<StreamRule> rules;
    auto &ini = mINI::Instance();
    // [transcode] 段在 mINI 里是 "transcode.<key>" 前缀
    static const string kPrefix = "transcode.";
    static const string kRuleKey = "stream_rule";

    // 收集 stream_rule、stream_rule_2、stream_rule_3 ... 并按序号排序，
    // 保证「先匹配先生效」的顺序可预期（map 遍历是字典序，stream_rule_10 会跑到
    // stream_rule_2 前面）。
    vector<pair<long, string>> ordered;
    for (auto &pr : ini) {
        if (pr.first.compare(0, kPrefix.size(), kPrefix) != 0) {
            continue;
        }
        auto key = pr.first.substr(kPrefix.size());
        if (key.compare(0, kRuleKey.size(), kRuleKey) != 0) {
            continue;
        }
        auto suffix = key.substr(kRuleKey.size());
        long seq = 1;
        if (!suffix.empty()) {
            if (suffix[0] != '_') {
                continue;
            }
            suffix = suffix.substr(1);
            char *end = nullptr;
            long num = ::strtol(suffix.data(), &end, 10);
            if (suffix.empty() || end == suffix.data() || *end != '\0') {
                continue;
            }
            seq = num;
        }
        if (trim(string(pr.second)).empty()) {
            continue;
        }
        ordered.emplace_back(seq, pr.second);
    }
    sort(ordered.begin(), ordered.end(),
         [](const pair<long, string> &a, const pair<long, string> &b) { return a.first < b.first; });

    for (auto &pr : ordered) {
        StreamRule rule;
        if (parseRule(pr.second, rule)) {
            rules.emplace_back(std::move(rule));
        }
    }
    return rules;
}

// ─────────────────────────────────────────────────────────────────────────────
// 运行时状态 / Runtime state
// ─────────────────────────────────────────────────────────────────────────────

TranscodeRuntime &TranscodeRuntime::instance() {
    static TranscodeRuntime s_instance;
    return s_instance;
}

int TranscodeRuntime::pickDevice(const std::string &stream_key, const std::string &devices) {
    if (devices.empty()) {
        return -1;
    }
    lock_guard<mutex> lck(_mtx);
    // 已经绑定过（重推/接管）就沿用，避免同一条流在两个设备间跳来跳去
    auto it = _stream_device.find(stream_key);
    if (it != _stream_device.end()) {
        return it->second;
    }
    // 解析 "0,1" 这类列表，挑在跑路数最少的那个；同负载取序号小的，保证可预测
    int best = -1;
    int best_load = INT32_MAX;
    for (auto &part : split(devices, ",")) {
        auto str = trim(string(part));
        if (str.empty()) {
            continue;
        }
        char *end = nullptr;
        long id = ::strtol(str.data(), &end, 10);
        if (end == str.data() || id < 0) {
            continue;
        }
        int load = _device_load.count((int) id) ? _device_load[(int) id] : 0;
        if (load < best_load) {
            best_load = load;
            best = (int) id;
        }
    }
    return best;
}

void TranscodeRuntime::bindDevice(const std::string &stream_key, int device) {
    if (device < 0) {
        return;
    }
    lock_guard<mutex> lck(_mtx);
    auto it = _stream_device.find(stream_key);
    if (it != _stream_device.end()) {
        if (--_device_load[it->second] <= 0) {
            _device_load.erase(it->second);
        }
    }
    _stream_device[stream_key] = device;
    ++_device_load[device];
}

void TranscodeRuntime::unbindDevice(const std::string &stream_key) {
    lock_guard<mutex> lck(_mtx);
    auto it = _stream_device.find(stream_key);
    if (it == _stream_device.end()) {
        return;
    }
    if (--_device_load[it->second] <= 0) {
        _device_load.erase(it->second);
    }
    _stream_device.erase(it);
}

std::string TranscodeRuntime::makeKey(const MediaTuple &tuple) {
    return tuple.vhost + "/" + tuple.app + "/" + tuple.stream;
}

TranscodeOption TranscodeRuntime::resolveRaw(const MediaTuple &tuple, string *source) const {
    auto option = loadGlobalTranscodeOption();
    string from = option.enable ? "global" : "off";

    // 按流规则：只取第一条匹配的（顺序见 loadRulesFromIni）
    for (auto &rule : loadRulesFromIni()) {
        if (!rule.match(tuple)) {
            continue;
        }
        applyTranscodeItems(rule.items, option);
        // 规则显式写了 codec 就视为「要开转码」，不必再要求全局 enable=1
        if (!rule.items.count("enable") &&
            (rule.items.count("audio_codec") || rule.items.count("video_codec"))) {
            option.enable = true;
        }
        from = "stream_rule(" + rule.targetKey() + ")";
        break;
    }

    // API 覆盖优先级最高 / API override wins over everything
    TranscodeOption over;
    if (getOverride(tuple, &over)) {
        option = over;
        from = "api";
    }
    if (source) {
        *source = from;
    }
    return option;
}

TranscodeOption TranscodeRuntime::resolve(const MediaTuple &tuple, string *source) const {
    string from;
    auto option = resolveRaw(tuple, &from);
    if (option.audio_codec == CodecInvalid && option.video_codec == CodecInvalid) {
        // 开了但没有目标 codec 等于没开：要么全局没配，要么规则里只写了 bitrate
        if (option.enable) {
            TraceL << "transcode enabled but no target codec for " << makeKey(tuple) << ", skipped";
        }
        option.enable = false;
    }
    if (source) {
        *source = option.enable ? from : "off";
    }
    return option;
}

void TranscodeRuntime::setOverride(const MediaTuple &tuple, const TranscodeOption &option) {
    lock_guard<mutex> lck(_mtx);
    _overrides[makeKey(tuple)] = option;
}

bool TranscodeRuntime::getOverride(const MediaTuple &tuple, TranscodeOption *option) const {
    lock_guard<mutex> lck(_mtx);
    auto it = _overrides.find(makeKey(tuple));
    if (it == _overrides.end()) {
        return false;
    }
    if (option) {
        *option = it->second;
    }
    return true;
}

bool TranscodeRuntime::clearOverride(const MediaTuple &tuple) {
    lock_guard<mutex> lck(_mtx);
    return _overrides.erase(makeKey(tuple)) > 0;
}

void TranscodeRuntime::attachSink(const MediaTuple &tuple, const std::weak_ptr<TranscodeSink> &sink) {
    auto strong = sink.lock();
    if (strong) {
        // 流标签在这里统一打点：注册表本来就持有流标识，省得每个调用点各写一遍
        strong->setStreamLabel(tuple.shortUrl());
    }
    lock_guard<mutex> lck(_mtx);
    _sinks[makeKey(tuple)] = sink;
}

TranscodeSink::Ptr TranscodeRuntime::findSink(const MediaTuple &tuple) const {
    lock_guard<mutex> lck(_mtx);
    auto it = _sinks.find(makeKey(tuple));
    if (it == _sinks.end()) {
        return nullptr;
    }
    auto sink = it->second.lock();
    if (!sink) {
        // 流已关闭，清掉失效条目 / stream gone, drop the stale entry
        _sinks.erase(it);
    }
    return sink;
}

}//namespace mediakit
#endif//ENABLE_FFMPEG
