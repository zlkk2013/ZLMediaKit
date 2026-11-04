/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_S3_CONFIG_H
#define ZLMEDIAKIT_S3_CONFIG_H

#include <mutex>
#include <string>

#include "S3/S3Client.h"

namespace mediakit {

/**
 * 云存储（S3/MinIO）配置。
 *
 * 配置来源优先级：运行时 API 下发（setS3Config）> 配置文件 ini。
 * 之所以要运行时下发：现场换桶/换密钥不该重启服务（录制中断），
 * 而配置文件是启动时读一次的快照。
 */
struct S3Options {
    bool enable = false;          ///< s3.enable：总开关，关掉则所有录制仍落本地
    std::string endpoint;         ///< s3.endpoint  例如 http://127.0.0.1:9000
    std::string region = "us-east-1";
    std::string bucket;
    std::string access_key;
    std::string secret_key;
    std::string session_token;    ///< STS 临时凭据（可空）
    bool path_style = true;       ///< MinIO 必须 true
    std::string root_prefix;      ///< 对象名前缀，例如 "zlm/record"
    bool record_mp4 = false;      ///< 录制 mp4 直接写云存储（不落盘）
    bool record_hls = false;      ///< 录制 hls 直接写云存储（不落盘）
    int timeout_ms = 15000;
};

class S3ConfigManager {
public:
    static S3ConfigManager &instance();

    /// 当前生效的配置（ini + 运行时覆盖）
    S3Options get();

    /// 运行时下发（线程安全；配置变化后 client() 会自动重建）
    void set(const S3Options &opt);

    /// 从 ini 重新加载（服务启动时调用一次；也会在 set() 后作为基线）
    void reloadFromIni();

    /// 取客户端（未启用或配置不全时返回 nullptr）
    S3Client::Ptr client();

    /// 状态快照（给 HTTP API 用；**不含密钥明文**）
    std::string statusJson();

private:
    S3ConfigManager() = default;

    std::mutex _mtx;
    S3Options _opt;
    S3Client::Ptr _client;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_S3_CONFIG_H
