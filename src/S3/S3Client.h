/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_S3_CLIENT_H
#define ZLMEDIAKIT_S3_CLIENT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mediakit {

/**
 * S3/MinIO 客户端（同步、阻塞式、无外部 SDK 依赖）。
 *
 * 为什么自己实现而不是引 aws-sdk-cpp：
 *   1) aws-sdk-cpp 体积极大（编译产物数百 MB），离线/信创环境很难塞进交付包；
 *   2) 我们只需要 6 个操作（PUT/分片 PUT/GET 范围读/LIST/HEAD/DELETE），
 *      SigV4 签名 + HTTP/1.1 就够，约千行可控代码；
 *   3) 便于在 ZLM 的录制线程里同步调用，不引入额外事件循环。
 *
 * 线程安全：所有方法都是 const/无共享可变状态，可多线程并发调用（每个调用独立连接）。
 * 传输层：http 直连；https 在 ENABLE_OPENSSL 打开时支持（其余情况明确报错，不静默降级）。
 */
struct S3Config {
    /// 形如 http://127.0.0.1:9000 或 https://s3.cn-north-1.amazonaws.com.cn
    std::string endpoint;
    std::string region = "us-east-1";
    std::string bucket;
    std::string access_key;
    std::string secret_key;
    /// STS 临时凭据用；为空表示长期密钥
    std::string session_token;
    /// MinIO 必须 path-style（bucket 在路径里）；AWS 两种都可，默认 path 更通用
    bool path_style = true;
    int timeout_ms = 15000;
};

struct S3Object {
    std::string key;
    uint64_t size = 0;
    std::string etag;
    std::string last_modified;
};

class S3Client {
public:
    using Ptr = std::shared_ptr<S3Client>;

    explicit S3Client(const S3Config &cfg);
    ~S3Client();

    /// 配置自检（endpoint/bucket/密钥是否齐、endpoint 是否可解析）
    bool valid(std::string &err) const;

    const S3Config &config() const;

    /// 单次 PUT（整块内存）。小对象/小片段用这个最简单。
    bool putObject(const std::string &key, const void *data, size_t len, std::string &err);

    /**
     * 分片上传：**数据始终在内存里分片发送，不落磁盘**。
     * @param part_size 每片字节数（S3 要求 >= 5MB，最后一片可小于 5MB），传 0 用默认 8MB
     * 失败会尽力 AbortMultipartUpload，避免留下碎片计费。
     */
    bool putObjectMultipart(const std::string &key, const void *data, size_t len,
                            size_t part_size, std::string &err);

    /// 整对象读取（点播/下载时对象不大时用）
    bool getObject(const std::string &key, std::string &out, std::string &err);

    /// 范围读取（HTTP Range 下载/点播 seek 用）
    bool getObjectRange(const std::string &key, uint64_t offset, uint64_t len, std::string &out,
                        std::string &err);

    bool headObject(const std::string &key, S3Object &info, std::string &err);

    bool deleteObject(const std::string &key, std::string &err);

    /// 创建桶（已存在视为成功）。首次部署/自动化安装时用。
    bool createBucket(std::string &err);

    /// 列对象（自动翻页直到 max_keys 或列完）
    bool listObjects(const std::string &prefix, std::vector<S3Object> &out, int max_keys,
                     std::string &err);

    /// 预签名 GET URL（给浏览器直连；有效期秒级）
    std::string presignGet(const std::string &key, int expires_sec) const;

    /// 未签名直链（桶公开读或走内网时用）
    std::string objectUrl(const std::string &key) const;

    /// 最近一次请求的 HTTP 状态码与响应摘要（排障用）
    int lastStatus() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_S3_CLIENT_H
