/*
 * S3 客户端自测（命令行工具，不依赖 ZLM 运行时）
 *
 * 用法：
 *   ./s3_selftest --endpoint http://127.0.0.1:9000 --bucket zlmtest \
 *                 --ak minioadmin --sk minioadmin --region us-east-1
 *
 * 覆盖：小对象 PUT/GET/HEAD、范围读、LIST、分片上传（20MB 强制多片）、删除、预签名 URL。
 * 每一项都自己比对内容，失败即非 0 退出——不做「打印了就算过」的事。
 */
#include "S3/S3Client.h"
#include "S3/S3Crypto.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mediakit;

static int g_fail = 0;

static void check(const char *name, bool ok, const std::string &detail = "") {
    printf("  %s %-38s %s\n", ok ? "OK  " : "FAIL", name, detail.c_str());
    if (!ok) {
        g_fail++;
    }
}

int main(int argc, char **argv) {
    S3Config cfg;
    cfg.endpoint = "http://127.0.0.1:9000";
    cfg.bucket = "zlmtest";
    cfg.access_key = "minioadmin";
    cfg.secret_key = "minioadmin";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--endpoint") cfg.endpoint = v;
        else if (k == "--bucket") cfg.bucket = v;
        else if (k == "--ak") cfg.access_key = v;
        else if (k == "--sk") cfg.secret_key = v;
        else if (k == "--region") cfg.region = v;
        else if (k == "--token") cfg.session_token = v;
    }

    printf("S3 自测: endpoint=%s bucket=%s\n", cfg.endpoint.c_str(), cfg.bucket.c_str());
    S3Client client(cfg);
    std::string err;
    auto fresh = []() { return std::string(); };
    if (!client.valid(err)) {
        printf("配置无效: %s\n", err.c_str());
        return 2;
    }

    // 0) 建桶（已存在视为成功），保证第一次跑就能自举
    std::string cerr;
    bool cinfo = client.createBucket(cerr);
    check("createBucket（幂等）", cinfo, cerr);

    const std::string prefix = "selftest/";
    const std::string key = prefix + "small.txt";
    const std::string payload = "hello zlmediakit s3 " + std::to_string(time(nullptr));

    // 1) 小对象 PUT
    bool ok = client.putObject(key, payload.data(), payload.size(), err);
    check("putObject", ok, err);

    // 2) GET 回读并比对
    std::string got;
    ok = client.getObject(key, got, err);
    check("getObject 内容一致", ok && got == payload, ok ? "" : err);

    // 3) HEAD 拿大小
    S3Object info;
    ok = client.headObject(key, info, err);
    check("headObject 大小正确", ok && info.size == payload.size(),
          "size=" + std::to_string(info.size) + " 期望=" + std::to_string(payload.size()));

    // 4) 范围读
    std::string part;
    ok = client.getObjectRange(key, 6, 10, part, err);   // "zlmediakit"
    check("getObjectRange", ok && part == "zlmediakit", "got='" + part + "'");

    // 5) LIST
    std::vector<S3Object> objs;
    ok = client.listObjects(prefix, objs, 100, err);
    bool found = false;
    for (auto &o : objs) {
        if (o.key == key) found = true;
    }
    check("listObjects 含刚写的 key", ok && found,
          "共 " + std::to_string(objs.size()) + " 个对象");

    // 6) 分片上传：20MB 随机数据，强制走 3 片
    const size_t big_len = 20u << 20;
    std::string big;
    big.resize(big_len);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (char) ((i * 31 + (i >> 8)) & 0xFF);
    }
    const std::string big_key = prefix + "big.bin";
    auto t0 = std::chrono::steady_clock::now();
    ok = client.putObjectMultipart(big_key, big.data(), big.size(), 8u << 20, err);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
    check("putObjectMultipart 20MB/3 片", ok,
          ok ? std::to_string(ms) + "ms (" + std::to_string(big_len * 1000 / (ms ? ms : 1) / 1024 / 1024) + " MB/s)" : err);

    std::string big_back;
    t0 = std::chrono::steady_clock::now();
    ok = client.getObject(big_key, big_back, err);
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0).count();
    bool same = ok && big_back.size() == big.size() &&
                memcmp(big_back.data(), big.data(), big.size()) == 0;
    check("分片对象回读字节一致", same,
          "size=" + std::to_string(big_back.size()) + " 用时 " + std::to_string(ms) + "ms");

    // 7) 预签名 URL（由测试脚本用无凭据的 HTTP 客户端拉取验证）
    auto url = client.presignGet(key, 300);
    printf("PRESIGN_URL=%s\n", url.c_str());

    // 8) 删除
    ok = client.deleteObject(key, err);
    check("deleteObject", ok, err);
    bool missing = true;
    {
        S3Object tmp;
        std::string e2;
        missing = !client.headObject(key, tmp, e2);
    }
    check("删除后 HEAD 应失败", missing);

    // 清理大对象
    std::string e3;
    client.deleteObject(big_key, e3);

    printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
