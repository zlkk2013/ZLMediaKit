/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/S3Config.h"

#include <sstream>

#include "Common/config.h"
#include "Util/mini.h"

using namespace toolkit;

namespace mediakit {

namespace {
// 配置键集中在这里，避免各处硬编码字符串
const char *kEnable = "s3.enable";
const char *kEndpoint = "s3.endpoint";
const char *kRegion = "s3.region";
const char *kBucket = "s3.bucket";
const char *kAccessKey = "s3.access_key";
const char *kSecretKey = "s3.secret_key";
const char *kSessionToken = "s3.session_token";
const char *kPathStyle = "s3.path_style";
const char *kRootPrefix = "s3.root_prefix";
const char *kRecordMp4 = "s3.record_mp4";
const char *kRecordHls = "s3.record_hls";
const char *kRecordSnap = "s3.record_snap";
const char *kTimeoutMs = "s3.timeout_ms";

std::string jsonEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}
} // namespace

S3ConfigManager &S3ConfigManager::instance() {
    static S3ConfigManager s_instance;
    return s_instance;
}

void S3ConfigManager::reloadFromIni() {
    auto &ini = mINI::Instance();
    S3Options opt;
    opt.enable = (int) ini[kEnable] != 0;
    opt.endpoint = (std::string) ini[kEndpoint];
    if (!opt.endpoint.empty() && ini[kRegion].as<std::string>().empty()) {
        // 区域留空时给个默认，避免签名里的 scope 带空段
        opt.region = "us-east-1";
    } else {
        opt.region = (std::string) ini[kRegion];
        if (opt.region.empty()) {
            opt.region = "us-east-1";
        }
    }
    opt.bucket = (std::string) ini[kBucket];
    opt.access_key = (std::string) ini[kAccessKey];
    opt.secret_key = (std::string) ini[kSecretKey];
    opt.session_token = (std::string) ini[kSessionToken];
    opt.path_style = (int) ini[kPathStyle] != 0;
    opt.root_prefix = (std::string) ini[kRootPrefix];
    opt.record_mp4 = (int) ini[kRecordMp4] != 0;
    opt.record_hls = (int) ini[kRecordHls] != 0;
    opt.record_snap = (int) ini[kRecordSnap] != 0;
    opt.timeout_ms = (int) ini[kTimeoutMs] > 0 ? (int) ini[kTimeoutMs] : 15000;

    std::lock_guard<std::mutex> lck(_mtx);
    _opt = opt;
    _client.reset();   // 配置变了，客户端必须重建（签名密钥/端点都变了）
}

void S3ConfigManager::set(const S3Options &opt) {
    std::lock_guard<std::mutex> lck(_mtx);
    _opt = opt;
    _client.reset();
}

S3Options S3ConfigManager::get() {
    std::lock_guard<std::mutex> lck(_mtx);
    return _opt;
}

S3Client::Ptr S3ConfigManager::client() {
    std::lock_guard<std::mutex> lck(_mtx);
    if (!_opt.enable) {
        return nullptr;
    }
    if (!_client) {
        S3Config cfg;
        cfg.endpoint = _opt.endpoint;
        cfg.region = _opt.region;
        cfg.bucket = _opt.bucket;
        cfg.access_key = _opt.access_key;
        cfg.secret_key = _opt.secret_key;
        cfg.session_token = _opt.session_token;
        cfg.path_style = _opt.path_style;
        cfg.timeout_ms = _opt.timeout_ms;
        auto cli = std::make_shared<S3Client>(cfg);
        std::string err;
        if (!cli->valid(err)) {
            WarnL << "S3 配置无效: " << err;
            return nullptr;
        }
        _client = cli;
    }
    return _client;
}

std::string S3ConfigManager::statusJson() {
    auto opt = get();
    std::ostringstream oss;
    // 密钥只回显是否已配置，绝不回显明文
    oss << "{\"enable\":" << (opt.enable ? "true" : "false")
        << ",\"endpoint\":\"" << jsonEscape(opt.endpoint) << "\""
        << ",\"bucket\":\"" << jsonEscape(opt.bucket) << "\""
        << ",\"region\":\"" << jsonEscape(opt.region) << "\""
        << ",\"path_style\":" << (opt.path_style ? "true" : "false")
        << ",\"root_prefix\":\"" << jsonEscape(opt.root_prefix) << "\""
        << ",\"record_mp4\":" << (opt.record_mp4 ? "true" : "false")
        << ",\"record_hls\":" << (opt.record_hls ? "true" : "false")
        << ",\"access_key_set\":" << (opt.access_key.empty() ? "false" : "true")
        << ",\"secret_key_set\":" << (opt.secret_key.empty() ? "false" : "true")
        << ",\"session_token_set\":" << (opt.session_token.empty() ? "false" : "true")
        << "}";
    return oss.str();
}

} // namespace mediakit
