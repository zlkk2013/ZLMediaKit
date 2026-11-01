/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_TRANSCODE_CONFIG_H
#define ZLMEDIAKIT_TRANSCODE_CONFIG_H

#include "Common/macros.h"

#if defined(ENABLE_FFMPEG)

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Common/TranscodeSink.h"
#include "Record/Recorder.h"

namespace mediakit {

/**
 * 一条按流转码规则，来自 ini 的 [transcode] stream_rule / stream_rule_2 / ...
 *
 * 语法：`app/stream:key=value,key=value`，也可写全 `vhost/app/stream:...`
 * 认识的 key：enable、audio_codec、video_codec、bitrate、sample_rate、on_demand
 *
 * One per-stream rule, parsed from [transcode] stream_rule / stream_rule_2 / ...
 * Syntax: `app/stream:key=value,key=value`, or the full `vhost/app/stream:...`
 */
struct StreamRule {
    /** 为空表示匹配任意虚拟主机 / empty matches any vhost */
    std::string vhost;
    std::string app;
    std::string stream;
    /** 已 trim、key 已转小写的键值对 / trimmed items with lowercased keys */
    std::map<std::string, std::string> items;

    bool match(const MediaTuple &tuple) const;
    std::string targetKey() const;
};

/**
 * 转码参数的运行时状态。
 *
 * 三件事：
 *  1. 解析 ini 里的按流规则（只读 mINI，改配置 + reload 后新流即生效）
 *  2. 保存 HTTP API（addTranscode）设置的覆盖参数，优先级高于 ini
 *  3. 登记运行中的 TranscodeSink，供 API 查询状态、热更新参数
 *
 * Runtime state for transcoding: per-stream ini rules, API overrides (take
 * precedence over ini), and a registry of live TranscodeSink objects so the
 * HTTP API can read status and update parameters without a restart.
 *
 * **不落盘**：覆盖表只活在进程内存里，重启即回到 ini 的值。这是有意的
 * （无状态控制面，避免 API 与配置文件互相覆盖产生两套真相），HTTP 接口在
 * 返回里带 `persistent=false` 显式告知调用方。
 *
 * **Not persisted** on purpose: overrides live in process memory only and a
 * restart reverts to the ini file. The HTTP APIs report `persistent=false` so
 * callers are never misled into thinking a change survived a restart.
 *
 * 全部方法线程安全：ini 规则在推流 poller 线程读，覆盖参数从 HTTP 线程写。
 */
class TranscodeRuntime {
public:
    using Ptr = std::shared_ptr<TranscodeRuntime>;

    static TranscodeRuntime &instance();

    /** 解析一条规则文本，失败返回 false / parse one rule text, false when malformed */
    static bool parseRule(const std::string &text, StreamRule &rule);

    /**
     * 读取 ini 中所有非空 stream_rule* 并按后缀序号排序（stream_rule、stream_rule_2、...）
     * Load every non-empty stream_rule* key, ordered by numeric suffix.
     */
    static std::vector<StreamRule> loadRulesFromIni();

    /**
     * 计算某条流的最终转码参数：全局 [transcode] → 首条匹配的 stream_rule → API 覆盖
     * @param source 可选输出：实际生效来源（off/global/stream_rule/api），便于排障
     */
    TranscodeOption resolve(const MediaTuple &tuple, std::string *source = nullptr) const;

    /**
     * 同 resolve，但不做「没有目标 codec 就关掉」的归一化，也不会被旧的 API 覆盖遮蔽。
     * addTranscode 需要在这份原始合成结果上继续增量应用请求参数。
     */
    TranscodeOption resolveRaw(const MediaTuple &tuple, std::string *source = nullptr) const;

    /** API 设置的覆盖参数（整份替换，不做增量合并） */
    void setOverride(const MediaTuple &tuple, const TranscodeOption &option);
    bool getOverride(const MediaTuple &tuple, TranscodeOption *option = nullptr) const;
    bool clearOverride(const MediaTuple &tuple);

    /**
     * 登记运行中的转码节点；流关闭后 weak_ptr 自动失效。
     * 顺便把流标识打到 sink 上（`vhost/app/stream`），多路并发转码时日志才分得清。
     *
     * Register a live sink; the weak_ptr expires when the stream closes. Also
     * labels the sink with `vhost/app/stream` so concurrent transcodes are
     * distinguishable in logs.
     */
    void attachSink(const MediaTuple &tuple, const std::weak_ptr<TranscodeSink> &sink);

    /**
     * 为一条流挑硬件设备：指定了就尊重指定；否则在 hw_devices 里挑**当前在跑路数最少**的。
     * 没有配置设备列表返回 -1（表示不区分设备，交给编码器默认）。
     *
     * 分配依据是「正在转码的流数量」，不是 GPU 利用率：读利用率需要厂商 SDK，
     * 而路数已经能近似均衡（同样分辨率/码率下每路的负载接近）。
     *
     * Pick a device for a stream: honour the pinned device, otherwise the least-loaded
     * one from hw_devices. Returns -1 when no device list is configured. Balancing is by
     * active stream count rather than GPU utilization: reading utilization needs a
     * vendor SDK, while stream count is a good proxy for equal-cost streams.
     */
    int pickDevice(const std::string &stream_key, const std::string &devices);

    /** 记录某条流使用了哪个设备（流关闭时释放计数） */
    void bindDevice(const std::string &stream_key, int device);
    void unbindDevice(const std::string &stream_key);
    TranscodeSink::Ptr findSink(const MediaTuple &tuple) const;

    static std::string makeKey(const MediaTuple &tuple);

private:
    TranscodeRuntime() = default;

    mutable std::mutex _mtx;
    std::map<std::string, TranscodeOption> _overrides;
    /** findSink() 在 const 方法里清理失效条目，故为 mutable / pruned from a const method */
    mutable std::map<std::string, std::weak_ptr<TranscodeSink>> _sinks;
    /** 流 → 设备，以及设备 → 在跑路数 */
    std::map<std::string, int> _stream_device;
    std::map<int, int> _device_load;
};

/**
 * 把一组 key=value 增量应用到 option。未知 key 打警告并跳过，不抛异常。
 * @param items 键已转小写
 * @return false 表示有 key 认识但值非法（调用方据此返回参数错误）
 */
bool applyTranscodeItems(const std::map<std::string, std::string> &items,
                         TranscodeOption &option, std::string *err = nullptr);

/**
 * 编码名 → CodecId（含 aac/g711a/hevc 等常用别名，并校验音视频类型）
 * Codec name to CodecId, with the aliases operators actually type.
 */
CodecId parseTranscodeCodec(const std::string &name, TrackType expect, const char *what);

/**
 * 只读 [transcode] 全局段，不做「开了但没配 codec 就关掉」这类策略判断，
 * 以便按流规则在此基础上做增量覆盖。
 */
TranscodeOption loadGlobalTranscodeOption();

}//namespace mediakit
#endif//ENABLE_FFMPEG
#endif//ZLMEDIAKIT_TRANSCODE_CONFIG_H
