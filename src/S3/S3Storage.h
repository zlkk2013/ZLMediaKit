/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_S3_STORAGE_H
#define ZLMEDIAKIT_S3_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mediakit {

/**
 * 录制/回放对云存储的门面：只暴露「按约定拼对象名」和「上传一块内存」两件事。
 *
 * 上层（MP4Recorder / HlsMakerImp）不应该关心分片大小、重试、签名等细节；
 * 这里统一决定：小对象单次 PUT、大对象自动走分片（8MB/片），失败记日志不抛异常
 * —— 录制线程不能被存储故障拖死（云不可用时录制继续，只是这一段没上传成功）。
 */
class S3Storage {
public:
    static S3Storage &instance();

    /// 是否启用云存储录制（s3.enable && 对应的 record_xxx）
    static bool enabledForMp4();
    static bool enabledForHls();
    static bool enabledForSnap();

    /**
     * 拼对象名：<root_prefix>/<app>/<stream>/<日期>/<文件名>
     * 与本地录制目录结构一致，现场换存储不用改回放口径。
     */
    static std::string makeKey(const std::string &app, const std::string &stream,
                               const std::string &date, const std::string &file_name);

    /**
     * 上传一块内存数据（**全程不落磁盘**）。
     * @return 成功与否；失败时 err 带原因
     */
    bool upload(const std::string &key, const std::string &data, std::string &err);

    /**
     * 把本地文件整块读进内存后上传（截图这类小文件用；大文件请用 upload + 自己的缓冲）
     * @return 成功与否；失败时 err 带原因
     */
    bool uploadFile(const std::string &key, const std::string &file_path, std::string &err);

    /// 对象是否已存在（避免重复上传同一段）
    bool exists(const std::string &key, uint64_t &size);

private:
    S3Storage() = default;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_S3_STORAGE_H
