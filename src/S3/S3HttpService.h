/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_S3_HTTP_SERVICE_H
#define ZLMEDIAKIT_S3_HTTP_SERVICE_H

#include <functional>
#include <string>

#include "Common/Parser.h"
#include "Http/HttpBody.h"

namespace mediakit {

/**
 * 云存储 HTTP 出口：把桶里的对象用 ZLM 自己的 HTTP 服务放出来。
 *
 *   GET /s3/<key>       下载/点播对象（支持 Range，可拖动、可断点续传）
 *   GET /s3/<prefix>/   按前缀列对象，生成菜单页（可点进子目录，可点播/下载）
 *
 * 为什么不让前端直连 S3：
 *   1) 密钥不下发到浏览器；预签名 URL 会过期，也没法按目录授权；
 *   2) 现场往往只有 ZLM 的端口对外开放，S3 常常在内网另一台机器；
 *   3) 录制与回放共用一套地址，前端/播放器不用配两个 endpoint。
 *
 * 传输策略（内存是硬约束，视频文件动辄几百 MB）：
 *   - 小对象（<= 2MB）整块读进内存直接回，省一次往返；
 *   - 大对象走 HttpS3RangeBody 分块拉（默认 1MB/块）：Content-Length 是已知的，
 *     keep-alive 不受影响；每块在后台线程取，既不阻塞 poller 也不会把整个文件读进内存。
 *
 * 安全：本模块沿用「静态文件目录同样公开」的口径 —— 只要 ZLM 的 HTTP 端口对谁开放，
 * /s3/ 就对谁开放。需要鉴权时用 http.secret 或前置反代，不在这一层加私有协议。
 */
class S3HttpService {
public:
    using Invoker = std::function<void(int code, const std::string &content_type,
                                      const StrCaseMap &responseHeader, const HttpBody::Ptr &body)>;

    /// 该请求是否属于云存储出口（/s3 或 /s3/...）
    static bool match(const Parser &parser);

    /// 处理请求（调用前需 match 为真）
    static void onRequest(Parser &parser, const Invoker &cb);
};

} // namespace mediakit

#endif // ZLMEDIAKIT_S3_HTTP_SERVICE_H
