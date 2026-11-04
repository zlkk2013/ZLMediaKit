/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_S3_CRYPTO_H
#define ZLMEDIAKIT_S3_CRYPTO_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mediakit {

/**
 * S3 签名需要的几个密码学原语。
 *
 * 为什么不直接用 OpenSSL：S3 签名只用 SHA-256 / HMAC-SHA256 两个原语，
 * 而 OpenSSL 在部分信创/离线环境里版本不一致（或缺头文件）会让整个模块编不过。
 * 这里自带一份实现（约 150 行），无外部依赖，也不参与任何网络加密
 * —— 传输层的 TLS 仍然走 OpenSSL（见 S3Client 的 https 分支）。
 *
 * SHA-256 / HMAC-SHA256 used by AWS SigV4. Self-contained on purpose: OpenSSL is
 * optional in this project and its absence must not break S3 signing.
 */
namespace crypto {

/// SHA-256 摘要（32 字节）
void sha256(const void *data, size_t len, uint8_t out[32]);

/// HMAC-SHA256
void hmacSha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[32]);

/// 转十六进制小写
std::string hexEncode(const void *data, size_t len);

/// 便捷封装：SHA-256 → 十六进制
std::string sha256Hex(const std::string &data);

/// 便捷封装：HMAC-SHA256 → 原始 32 字节
std::string hmacSha256Raw(const std::string &key, const std::string &data);

/// 便捷封装：HMAC-SHA256 → 十六进制
std::string hmacSha256Hex(const std::string &key, const std::string &data);

} // namespace crypto
} // namespace mediakit

#endif // ZLMEDIAKIT_S3_CRYPTO_H
