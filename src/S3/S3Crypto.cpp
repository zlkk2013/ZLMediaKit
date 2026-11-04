/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/S3Crypto.h"

#include <cstring>

namespace mediakit {
namespace crypto {
namespace {

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

const uint32_t kK[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
};

/// 一个分组（64 字节）的压缩函数
void transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i * 4] << 24) | ((uint32_t) block[i * 4 + 1] << 16) |
               ((uint32_t) block[i * 4 + 2] << 8) | ((uint32_t) block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/// SHA-256 增量上下文：sha256() 与 HMAC 共用同一份实现，避免两份代码走偏
struct Sha256Ctx {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t buf_len = 0;
    uint64_t total = 0;

    void update(const void *data, size_t len) {
        auto q = (const uint8_t *) data;
        total += len;
        while (len) {
            size_t take = 64 - buf_len;
            if (take > len) {
                take = len;
            }
            memcpy(buf + buf_len, q, take);
            buf_len += take;
            q += take;
            len -= take;
            if (buf_len == 64) {
                transform(state, buf);
                buf_len = 0;
            }
        }
    }

    void finish(uint8_t out[32]) {
        uint64_t bits = total * 8;
        uint8_t pad[128];
        size_t pad_len = (buf_len < 56) ? (56 - buf_len) : (120 - buf_len);
        memset(pad, 0, sizeof(pad));
        pad[0] = 0x80;
        update(pad, pad_len);
        uint8_t len_be[8];
        for (int i = 0; i < 8; i++) {
            len_be[7 - i] = (uint8_t) (bits >> (8 * i));
        }
        update(len_be, 8);
        for (int i = 0; i < 8; i++) {
            out[i * 4] = (uint8_t) (state[i] >> 24);
            out[i * 4 + 1] = (uint8_t) (state[i] >> 16);
            out[i * 4 + 2] = (uint8_t) (state[i] >> 8);
            out[i * 4 + 3] = (uint8_t) state[i];
        }
    }
};

} // namespace

void sha256(const void *data, size_t len, uint8_t out[32]) {
    Sha256Ctx ctx;
    ctx.update(data, len);
    ctx.finish(out);
}

void hmacSha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha256(key, key_len, k);   // 长 key 先摘要到 32 字节
    } else if (key_len) {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t) (k[i] ^ 0x36);
        opad[i] = (uint8_t) (k[i] ^ 0x5c);
    }

    uint8_t inner[32];
    {
        Sha256Ctx ctx;
        ctx.update(ipad, 64);
        ctx.update(data, len);
        ctx.finish(inner);
    }
    Sha256Ctx ctx;
    ctx.update(opad, 64);
    ctx.update(inner, 32);
    ctx.finish(out);
}

std::string hexEncode(const void *data, size_t len) {
    static const char *kHex = "0123456789abcdef";
    auto p = (const uint8_t *) data;
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0x0F]);
    }
    return out;
}

std::string sha256Hex(const std::string &data) {
    uint8_t digest[32];
    sha256(data.data(), data.size(), digest);
    return hexEncode(digest, 32);
}

std::string hmacSha256Raw(const std::string &key, const std::string &data) {
    uint8_t out[32];
    hmacSha256(key.data(), key.size(), data.data(), data.size(), out);
    return std::string((const char *) out, 32);
}

std::string hmacSha256Hex(const std::string &key, const std::string &data) {
    uint8_t out[32];
    hmacSha256(key.data(), key.size(), data.data(), data.size(), out);
    return hexEncode(out, 32);
}

} // namespace crypto
} // namespace mediakit
