/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/S3Storage.h"

#include "S3/S3Config.h"
#include "Util/logger.h"

namespace mediakit {

namespace {
/// 大于该阈值走分片上传（S3 单次 PUT 上限 5GB，但单片内存占用也要可控）
const size_t kMultipartThreshold = 8u << 20;
} // namespace

S3Storage &S3Storage::instance() {
    static S3Storage s_instance;
    return s_instance;
}

bool S3Storage::enabledForMp4() {
    auto opt = S3ConfigManager::instance().get();
    return opt.enable && opt.record_mp4;
}

bool S3Storage::enabledForHls() {
    auto opt = S3ConfigManager::instance().get();
    return opt.enable && opt.record_hls;
}

bool S3Storage::enabledForSnap() {
    auto opt = S3ConfigManager::instance().get();
    return opt.enable && opt.record_snap;
}

std::string S3Storage::makeKey(const std::string &app, const std::string &stream,
                               const std::string &date, const std::string &file_name) {
    auto opt = S3ConfigManager::instance().get();
    std::string key;
    if (!opt.root_prefix.empty()) {
        key = opt.root_prefix;
        if (key.back() != '/') {
            key.push_back('/');
        }
    }
    key += app + "/" + stream + "/" + date + "/" + file_name;
    return key;
}

bool S3Storage::upload(const std::string &key, const std::string &data, std::string &err) {
    auto cli = S3ConfigManager::instance().client();
    if (!cli) {
        err = "云存储未启用或配置不完整";
        return false;
    }
    if (data.empty()) {
        err = "数据为空";
        return false;
    }
    // 小对象一次 PUT；大对象分片，避免单请求体过大导致超时/重传代价高
    if (data.size() <= kMultipartThreshold) {
        return cli->putObject(key, data.data(), data.size(), err);
    }
    return cli->putObjectMultipart(key, data.data(), data.size(), 0, err);
}

bool S3Storage::uploadFile(const std::string &key, const std::string &file_path, std::string &err) {
    FILE *fp = fopen(file_path.data(), "rb");
    if (!fp) {
        err = "打不开本地文件: " + file_path;
        return false;
    }
    std::string data;
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        data.append(buf, n);
    }
    fclose(fp);
    if (data.empty()) {
        err = "本地文件为空: " + file_path;
        return false;
    }
    return upload(key, data, err);
}

bool S3Storage::exists(const std::string &key, uint64_t &size) {
    auto cli = S3ConfigManager::instance().client();
    if (!cli) {
        return false;
    }
    S3Object info;
    std::string err;
    if (!cli->headObject(key, info, err)) {
        return false;
    }
    size = info.size;
    return true;
}

} // namespace mediakit
