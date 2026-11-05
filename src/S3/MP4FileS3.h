/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MP4_FILE_S3_H
#define ZLMEDIAKIT_MP4_FILE_S3_H

#include <cstdint>
#include <string>

#include "S3/S3Client.h"

#if defined(ENABLE_MP4)
#include "Record/MP4.h"
#endif

namespace mediakit {

#if defined(ENABLE_MP4)

/**
 * 把对象存储里的一个 mp4 当成「可 seek 的只读文件」给 mp4 解复用器用。
 *
 * 只实现 read/seek/tell（写入直接失败），解复用器不关心字节从哪来。
 * 关键在**读放大**：mov_reader 会大量小读（每次几十字节读 moov/索引），
 * 一次一请求会把 S3 打穿、点播延迟飙到秒级 —— 所以按块（默认 512KB）预读缓存，
 * 命中就直接内存拷贝。块大小是「额外流量」与「请求数」的折中：
 * 512KB 时顺序播放几乎全部命中，随机拖动最多多读半兆。
 */
class MP4FileS3 : public MP4FileIO {
public:
    using Ptr = std::shared_ptr<MP4FileS3>;

    /// @param size 对象大小（调用方已 HEAD 过，避免每块都探一次）
    MP4FileS3(S3Client::Ptr cli, std::string key, uint64_t size);

    uint64_t fileSize() const { return _size; }
    const std::string &key() const { return _key; }

    int64_t onTell() override;
    int onSeek(int64_t offset) override;
    int onRead(void *data, size_t bytes) override;
    int onWrite(const void *data, size_t bytes) override;

    /// 统计（排障/日志用）：实际发起的对象请求数、读到的字节数
    uint64_t requestCount() const { return _requests; }

private:
    /// 把 [offset, offset+kBlock) 拉进缓存；失败返回 false
    bool loadBlock(uint64_t offset);

    static const size_t kBlock = 512 * 1024;

    S3Client::Ptr _cli;
    std::string _key;
    uint64_t _size;
    uint64_t _offset = 0;
    uint64_t _requests = 0;

    std::string _cache;
    uint64_t _cache_off = 0;
};

#endif // ENABLE_MP4

} // namespace mediakit

#endif // ZLMEDIAKIT_MP4_FILE_S3_H
