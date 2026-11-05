/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/MP4FileS3.h"

#include <algorithm>
#include <cstring>

#include "Util/logger.h"

using namespace std;
using namespace toolkit;

#if defined(ENABLE_MP4)
namespace mediakit {

MP4FileS3::MP4FileS3(S3Client::Ptr cli, string key, uint64_t size)
        : _cli(std::move(cli)), _key(std::move(key)), _size(size) {}

int64_t MP4FileS3::onTell() {
    return (int64_t) _offset;
}

int MP4FileS3::onSeek(int64_t offset) {
    if (offset < 0) {
        // 与 MP4FileMemory 一致：负偏移表示从文件末尾算
        offset += (int64_t) _size;
        if (offset < 0) {
            return -1;
        }
    }
    if ((uint64_t) offset > _size) {
        return -1;
    }
    _offset = (uint64_t) offset;
    return 0;
}

bool MP4FileS3::loadBlock(uint64_t offset) {
    auto want = (size_t) min<uint64_t>(kBlock, _size - offset);
    if (want == 0) {
        return false;
    }
    string data, err;
    if (!_cli->getObjectRange(_key, offset, want, data, err)) {
        WarnL << "云点播读取失败: " << _key << " offset=" << offset << " len=" << want
              << " err=" << err;
        return false;
    }
    _cache = std::move(data);
    _cache_off = offset;
    _requests++;
    return true;
}

int MP4FileS3::onRead(void *data, size_t bytes) {
    if (_offset >= _size) {
        // EOF：与 MP4FileDisk/MP4FileMemory 的约定一致
        return -1;
    }
    auto out = (char *) data;
    size_t done = 0;
    while (done < bytes) {
        if (_cache.empty() || _offset < _cache_off || _offset >= _cache_off + _cache.size()) {
            if (!loadBlock(_offset)) {
                return -1;
            }
        }
        auto in_block = (size_t) (_offset - _cache_off);
        auto n = min(bytes - done, _cache.size() - in_block);
        memcpy(out + done, _cache.data() + in_block, n);
        done += n;
        _offset += n;
        if (_offset >= _size && done < bytes) {
            // 读到对象末尾但还没凑够：让调用方按 EOF 处理
            return -1;
        }
    }
    return 0;
}

int MP4FileS3::onWrite(const void *data, size_t bytes) {
    // 云点播是只读路径，写入属于调用方用法错误，明确失败而不是静默丢弃
    WarnL << "云点播对象是只读的，拒绝写入: " << _key << " bytes=" << bytes;
    return -1;
}

} // namespace mediakit
#endif // ENABLE_MP4
