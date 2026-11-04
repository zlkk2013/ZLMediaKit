/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/S3Client.h"
#include "S3/S3Crypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef ENABLE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace mediakit {
namespace {

const char *kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const size_t kDefaultPartSize = 8u << 20;   // 8MB：S3 分片下限 5MB，8MB 对内存友好

std::string toLower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    return out;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// RFC3986：除 unreserved 之外全部 %XX（S3 签名要求，空格必须是 %20 不是 +）
std::string uriEncode(const std::string &s, bool keep_slash) {
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
            out.push_back((char) c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

void toAmzDate(std::string &amz_date, std::string &date_stamp) {
    time_t now = time(nullptr);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    amz_date = buf;
    date_stamp = amz_date.substr(0, 8);
}

/// 从 endpoint 里拆出 scheme / host / port / base path
struct Endpoint {
    std::string scheme = "http";
    std::string host;
    int port = 80;
    std::string base_path;   // 可能为空
};

bool parseEndpoint(const std::string &raw, Endpoint &out) {
    std::string s = raw;
    auto pos = s.find("://");
    if (pos != std::string::npos) {
        out.scheme = toLower(s.substr(0, pos));
        s = s.substr(pos + 3);
    }
    auto slash = s.find('/');
    if (slash != std::string::npos) {
        out.base_path = s.substr(slash);
        s = s.substr(0, slash);
    }
    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        out.host = s.substr(0, colon);
        out.port = atoi(s.substr(colon + 1).c_str());
    } else {
        out.host = s;
        out.port = (out.scheme == "https") ? 443 : 80;
    }
    if (out.host.empty()) {
        return false;
    }
    if (out.port <= 0) {
        out.port = (out.scheme == "https") ? 443 : 80;
    }
    return out.scheme == "http" || out.scheme == "https";
}

/// 极简 HTTP/1.1 连接（同步阻塞）：http 直连，https 走 OpenSSL
class HttpConn {
public:
    ~HttpConn() {
        close();
    }

    bool connect(const Endpoint &ep, int timeout_ms, std::string &err) {
        _timeout_ms = timeout_ms;
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        char port[16];
        snprintf(port, sizeof(port), "%d", ep.port);
        int rc = getaddrinfo(ep.host.c_str(), port, &hints, &res);
        if (rc != 0) {
            err = std::string("DNS 解析失败: ") + gai_strerror(rc);
            return false;
        }
        for (auto p = res; p; p = p->ai_next) {
            int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) {
                continue;
            }
            struct timeval tv;
            tv.tv_sec = _timeout_ms / 1000;
            tv.tv_usec = (_timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                _fd = fd;
                break;
            }
            ::close(fd);
        }
        freeaddrinfo(res);
        if (_fd < 0) {
            err = "连接失败: " + ep.host + ":" + port;
            return false;
        }
#ifdef ENABLE_OPENSSL
        if (ep.scheme == "https") {
            static bool ssl_inited = false;
            if (!ssl_inited) {
                SSL_library_init();
                SSL_load_error_strings();
                ssl_inited = true;
            }
            _ctx = SSL_CTX_new(TLS_client_method());
            if (!_ctx) {
                err = "SSL_CTX_new 失败";
                return false;
            }
            _ssl = SSL_new(_ctx);
            SSL_set_tlsext_host_name(_ssl, ep.host.c_str());   // SNI
            SSL_set_fd(_ssl, _fd);
            if (SSL_connect(_ssl) != 1) {
                err = "TLS 握手失败";
                return false;
            }
        }
#else
        if (ep.scheme == "https") {
            err = "本构建未启用 OpenSSL，无法访问 https 端点（可用 http 内网 MinIO，或重新编译时打开 ENABLE_OPENSSL）";
            return false;
        }
#endif
        return true;
    }

    bool sendAll(const void *data, size_t len) {
        auto p = (const char *) data;
        while (len) {
#ifdef ENABLE_OPENSSL
            ssize_t n = _ssl ? (ssize_t) SSL_write(_ssl, p, (int) len) : ::send(_fd, p, len, 0);
#else
            ssize_t n = ::send(_fd, p, len, 0);
#endif
            if (n <= 0) {
                return false;
            }
            p += n;
            len -= (size_t) n;
        }
        return true;
    }

    /// 读一行（不含 CRLF）
    bool readLine(std::string &line) {
        line.clear();
        char c;
        while (true) {
            if (_buf_pos >= _buf_len) {
                if (!fill()) {
                    return false;
                }
            }
            c = _buf[_buf_pos++];
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return true;
            }
            line.push_back(c);
            if (line.size() > 64 * 1024) {
                return false;
            }
        }
    }

    bool readExact(void *dst, size_t len) {
        auto p = (char *) dst;
        while (len) {
            if (_buf_pos < _buf_len) {
                size_t take = std::min(len, _buf_len - _buf_pos);
                memcpy(p, _buf + _buf_pos, take);
                _buf_pos += take;
                p += take;
                len -= take;
                continue;
            }
            if (!fill()) {
                return false;
            }
        }
        return true;
    }

private:
    void close() {
#ifdef ENABLE_OPENSSL
        if (_ssl) {
            SSL_shutdown(_ssl);
            SSL_free(_ssl);
            _ssl = nullptr;
        }
        if (_ctx) {
            SSL_CTX_free(_ctx);
            _ctx = nullptr;
        }
#endif
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    bool fill() {
#ifdef ENABLE_OPENSSL
        ssize_t n = _ssl ? (ssize_t) SSL_read(_ssl, _buf, sizeof(_buf))
                         : ::recv(_fd, _buf, sizeof(_buf), 0);
#else
        ssize_t n = ::recv(_fd, _buf, sizeof(_buf), 0);
#endif
        if (n <= 0) {
            return false;
        }
        _buf_len = (size_t) n;
        _buf_pos = 0;
        return true;
    }

    int _fd = -1;
    int _timeout_ms = 10000;
    char _buf[16 * 1024];
    size_t _buf_len = 0;
    size_t _buf_pos = 0;
#ifdef ENABLE_OPENSSL
    SSL_CTX *_ctx = nullptr;
    SSL *_ssl = nullptr;
#else
    void *_ctx = nullptr;
    void *_ssl = nullptr;
#endif
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string raw_error;
};

/// 目录（key）→ 规范的 canonical URI 路径
std::string canonicalPath(const S3Config &cfg, const Endpoint &ep, const std::string &key) {
    std::string path;
    if (!ep.base_path.empty() && ep.base_path != "/") {
        path = ep.base_path;
    }
    if (cfg.path_style) {
        path += "/" + uriEncode(cfg.bucket, false);
        if (!key.empty()) {
            path += "/" + uriEncode(key, true);
        }
    } else {
        path += "/" + uriEncode(key, true);
    }
    if (path.empty()) {
        path = "/";
    }
    return path;
}

/// 把 query（已解码的 key/value）变成规范查询串：按 key/value 排序后 RFC3986 编码
std::string canonicalQuery(const std::vector<std::pair<std::string, std::string>> &q) {
    if (q.empty()) {
        return "";
    }
    auto v = q;
    std::sort(v.begin(), v.end(), [](const std::pair<std::string, std::string> &a,
                                     const std::pair<std::string, std::string> &b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second < b.second;
    });
    std::string out;
    for (auto &pr : v) {
        if (!out.empty()) {
            out += "&";
        }
        out += uriEncode(pr.first, false);
        out += "=";
        out += uriEncode(pr.second, false);
    }
    return out;
}

std::string hostHeader(const Endpoint &ep) {
    bool default_port = (ep.scheme == "http" && ep.port == 80) ||
                        (ep.scheme == "https" && ep.port == 443);
    if (default_port) {
        return ep.host;
    }
    return ep.host + ":" + std::to_string(ep.port);
}

} // namespace

struct S3Client::Impl {
    S3Config cfg;
    Endpoint ep;
    /// 最近一次请求的状态码/错误（排障用；多线程并发时只作参考值）
    mutable int last_status = 0;
    mutable std::string last_err;

    /// 一次已签名的请求：负责建连、发请求、读响应
    bool doRequest(const std::string &method, const std::string &key,
                   const std::vector<std::pair<std::string, std::string>> &query,
                   const std::string &payload_hash, const std::string &body,
                   const std::vector<std::pair<std::string, std::string>> &extra_headers,
                   HttpResponse &resp, std::string &err) const {
        std::string amz_date, date_stamp;
        toAmzDate(amz_date, date_stamp);

        auto canon_uri = canonicalPath(cfg, ep, key);
        auto canon_query = canonicalQuery(query);

        // 参与签名的头必须小写、去空白、按键排序
        std::map<std::string, std::string> headers;
        headers["host"] = hostHeader(ep);
        headers["x-amz-content-sha256"] = payload_hash;
        headers["x-amz-date"] = amz_date;
        if (!cfg.session_token.empty()) {
            headers["x-amz-security-token"] = cfg.session_token;
        }
        for (auto &pr : extra_headers) {
            headers[toLower(pr.first)] = trim(pr.second);
        }

        std::string canon_headers, signed_headers;
        for (auto &pr : headers) {
            canon_headers += pr.first + ":" + pr.second + "\n";
            if (!signed_headers.empty()) {
                signed_headers += ";";
            }
            signed_headers += pr.first;
        }

        std::string canon_request = method + "\n" + canon_uri + "\n" + canon_query + "\n" +
                                    canon_headers + "\n" + signed_headers + "\n" + payload_hash;
        std::string scope = date_stamp + "/" + cfg.region + "/s3/aws4_request";
        std::string string_to_sign = "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
                                     crypto::sha256Hex(canon_request);

        auto k_date = crypto::hmacSha256Raw("AWS4" + cfg.secret_key, date_stamp);
        auto k_region = crypto::hmacSha256Raw(k_date, cfg.region);
        auto k_service = crypto::hmacSha256Raw(k_region, "s3");
        auto k_signing = crypto::hmacSha256Raw(k_service, "aws4_request");
        auto signature = crypto::hmacSha256Hex(k_signing, string_to_sign);

        std::string authorization = "AWS4-HMAC-SHA256 Credential=" + cfg.access_key + "/" + scope +
                                    ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

        // 组装请求
        std::string req = method + " " + canon_uri;
        if (!canon_query.empty()) {
            req += "?" + canon_query;
        }
        req += " HTTP/1.1\r\n";
        req += "Host: " + headers["host"] + "\r\n";
        req += "x-amz-content-sha256: " + payload_hash + "\r\n";
        req += "x-amz-date: " + amz_date + "\r\n";
        if (!cfg.session_token.empty()) {
            req += "x-amz-security-token: " + cfg.session_token + "\r\n";
        }
        for (auto &pr : extra_headers) {
            req += pr.first + ": " + pr.second + "\r\n";
        }
        req += "Authorization: " + authorization + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        req += "User-Agent: zlmediakit-s3\r\n\r\n";

        HttpConn conn;
        last_err.clear();
        if (!conn.connect(ep, cfg.timeout_ms, err)) {
            last_err = err;
            return false;
        }
        if (!conn.sendAll(req.data(), req.size()) ||
            (!body.empty() && !conn.sendAll(body.data(), body.size()))) {
            err = "发送请求失败";
            return false;
        }

        std::string line;
        if (!conn.readLine(line)) {
            err = "读取响应状态行失败";
            return false;
        }
        // HTTP/1.1 200 OK
        auto sp = line.find(' ');
        resp.status = (sp == std::string::npos) ? 0 : atoi(line.substr(sp + 1).c_str());
        last_status = resp.status;

        while (conn.readLine(line)) {
            if (line.empty()) {
                break;
            }
            auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            resp.headers[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }

        bool chunked = resp.headers.count("transfer-encoding") &&
                       toLower(resp.headers["transfer-encoding"]).find("chunked") != std::string::npos;
        if (chunked) {
            while (true) {
                if (!conn.readLine(line)) {
                    break;
                }
                size_t chunk = strtoul(line.c_str(), nullptr, 16);
                if (chunk == 0) {
                    conn.readLine(line);   // 结尾空行
                    break;
                }
                size_t old = resp.body.size();
                resp.body.resize(old + chunk);
                if (!conn.readExact(&resp.body[old], chunk)) {
                    break;
                }
                conn.readLine(line);       // 每块尾部的 CRLF
            }
        } else if (method != "HEAD" && resp.headers.count("content-length")) {
            size_t n = (size_t) strtoull(resp.headers["content-length"].c_str(), nullptr, 10);
            if (n) {
                resp.body.resize(n);
                if (!conn.readExact(&resp.body[0], n)) {
                    err = "读取响应体失败";
                    return false;
                }
            }
        }
        if (resp.status >= 400) {
            err = "HTTP " + std::to_string(resp.status) + ": " + resp.body.substr(0, 512);
            last_err = err;
            return false;
        }
        return true;
    }
};

S3Client::S3Client(const S3Config &cfg) : _impl(new Impl) {
    _impl->cfg = cfg;
}

S3Client::~S3Client() = default;

const S3Config &S3Client::config() const {
    return _impl->cfg;
}

bool S3Client::valid(std::string &err) const {
    if (_impl->cfg.endpoint.empty() || _impl->cfg.bucket.empty()) {
        err = "endpoint/bucket 不能为空";
        return false;
    }
    if (!parseEndpoint(_impl->cfg.endpoint, _impl->ep)) {
        err = "endpoint 格式不对，应形如 http://host:9000";
        return false;
    }
    if (_impl->cfg.access_key.empty() || _impl->cfg.secret_key.empty()) {
        err = "access_key/secret_key 不能为空";
        return false;
    }
    return true;
}

bool S3Client::putObject(const std::string &key, const void *data, size_t len, std::string &err) {
    auto payload_hash = crypto::sha256Hex(std::string((const char *) data, len));
    HttpResponse resp;
    std::vector<std::pair<std::string, std::string>> extra = {
            {"Content-Type", "application/octet-stream"}};
    return _impl->doRequest("PUT", key, {}, payload_hash,
                            std::string((const char *) data, len), extra, resp, err);
}

bool S3Client::putObjectMultipart(const std::string &key, const void *data, size_t len,
                                  size_t part_size, std::string &err) {
    if (part_size == 0) {
        part_size = kDefaultPartSize;
    }
    if (part_size < (5u << 20)) {
        part_size = 5u << 20;   // S3 硬性要求
    }

    // 1) 初始化分片上传
    HttpResponse resp;
    if (!_impl->doRequest("POST", key, {{"uploads", ""}}, kEmptySha256, "", {}, resp, err)) {
        return false;
    }
    auto pos = resp.body.find("<UploadId>");
    auto end = (pos == std::string::npos) ? pos : resp.body.find("</UploadId>", pos);
    if (pos == std::string::npos || end == std::string::npos) {
        err = "InitiateMultipartUpload 未返回 UploadId: " + resp.body.substr(0, 256);
        return false;
    }
    std::string upload_id = resp.body.substr(pos + 10, end - pos - 10);

    // 2) 逐片上传（每片都从内存里签自己的 hash，全程不落盘）
    std::string complete_xml = "<CompleteMultipartUpload>";
    auto p = (const char *) data;
    size_t offset = 0;
    int part_no = 1;
    while (offset < len) {
        size_t take = std::min(part_size, len - offset);
        std::string chunk(p + offset, take);
        auto payload_hash = crypto::sha256Hex(chunk);
        HttpResponse part_resp;
        std::vector<std::pair<std::string, std::string>> q = {
                {"partNumber", std::to_string(part_no)},
                {"uploadId", upload_id},
        };
        if (!_impl->doRequest("PUT", key, q, payload_hash, chunk, {}, part_resp, err)) {
            // 失败必须收敛：否则会在桶里留下永不完成的碎片
            std::string abort_err;
            _impl->doRequest("DELETE", key, {{"uploadId", upload_id}}, kEmptySha256, "", {}, resp,
                             abort_err);
            err = "分片 " + std::to_string(part_no) + " 上传失败: " + err;
            return false;
        }
        auto etag = part_resp.headers.count("etag") ? part_resp.headers["etag"] : "";
        complete_xml += "<Part><PartNumber>" + std::to_string(part_no) + "</PartNumber><ETag>" +
                        etag + "</ETag></Part>";
        offset += take;
        part_no++;
    }
    complete_xml += "</CompleteMultipartUpload>";

    // 3) 完成
    auto body_hash = crypto::sha256Hex(complete_xml);
    HttpResponse done;
    std::vector<std::pair<std::string, std::string>> extra = {
            {"Content-Type", "application/xml"}};
    if (!_impl->doRequest("POST", key, {{"uploadId", upload_id}}, body_hash, complete_xml, extra,
                          done, err)) {
        return false;
    }
    return true;
}

bool S3Client::getObject(const std::string &key, std::string &out, std::string &err) {
    HttpResponse resp;
    if (!_impl->doRequest("GET", key, {}, kEmptySha256, "", {}, resp, err)) {
        return false;
    }
    out.swap(resp.body);
    return true;
}

bool S3Client::getObjectRange(const std::string &key, uint64_t offset, uint64_t len,
                              std::string &out, std::string &err) {
    HttpResponse resp;
    std::vector<std::pair<std::string, std::string>> extra = {
            {"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + len - 1)}};
    if (!_impl->doRequest("GET", key, {}, kEmptySha256, "", extra, resp, err)) {
        return false;
    }
    out.swap(resp.body);
    return true;
}

bool S3Client::headObject(const std::string &key, S3Object &info, std::string &err) {
    HttpResponse resp;
    if (!_impl->doRequest("HEAD", key, {}, kEmptySha256, "", {}, resp, err)) {
        return false;
    }
    info.key = key;
    info.size = resp.headers.count("content-length")
                        ? strtoull(resp.headers["content-length"].c_str(), nullptr, 10)
                        : 0;
    info.etag = resp.headers.count("etag") ? resp.headers["etag"] : "";
    info.last_modified = resp.headers.count("last-modified") ? resp.headers["last-modified"] : "";
    return true;
}

bool S3Client::createBucket(std::string &err) {
    HttpResponse resp;
    // path-style 下建桶就是 PUT /<bucket>（key 为空）
    if (_impl->doRequest("PUT", "", {}, kEmptySha256, "", {}, resp, err)) {
        return true;
    }
    // 409 BucketAlreadyOwnedByYou / 200 都算成功
    if (resp.status == 409 || err.find("BucketAlreadyOwnedByYou") != std::string::npos) {
        err.clear();
        return true;
    }
    return false;
}

bool S3Client::deleteObject(const std::string &key, std::string &err) {
    HttpResponse resp;
    return _impl->doRequest("DELETE", key, {}, kEmptySha256, "", {}, resp, err);
}

namespace {
/// 取 <tag>...</tag> 之间的内容
bool extractTag(const std::string &xml, const std::string &tag, size_t from,
                std::string &out, size_t &next) {
    auto t1 = "<" + tag + ">";
    auto t2 = "</" + tag + ">";
    auto p = xml.find(t1, from);
    if (p == std::string::npos) {
        return false;
    }
    auto q = xml.find(t2, p);
    if (q == std::string::npos) {
        return false;
    }
    out = xml.substr(p + t1.size(), q - p - t1.size());
    next = q + t2.size();
    return true;
}
} // namespace

bool S3Client::listObjects(const std::string &prefix, std::vector<S3Object> &out, int max_keys,
                           std::string &err) {
    out.clear();
    std::string token;
    while ((int) out.size() < max_keys) {
        std::vector<std::pair<std::string, std::string>> q = {
                {"list-type", "2"},
                {"max-keys", std::to_string(std::min(1000, max_keys - (int) out.size()))},
        };
        if (!prefix.empty()) {
            q.emplace_back("prefix", prefix);
        }
        if (!token.empty()) {
            q.emplace_back("continuation-token", token);
        }
        HttpResponse resp;
        if (!_impl->doRequest("GET", "", q, kEmptySha256, "", {}, resp, err)) {
            return false;
        }
        size_t pos = 0;
        while (true) {
            auto p = resp.body.find("<Contents>", pos);
            if (p == std::string::npos) {
                break;
            }
            auto e = resp.body.find("</Contents>", p);
            if (e == std::string::npos) {
                break;
            }
            std::string item = resp.body.substr(p, e - p);
            S3Object obj;
            size_t next = 0;
            extractTag(item, "Key", 0, obj.key, next);
            std::string sz;
            extractTag(item, "Size", 0, sz, next);
            obj.size = sz.empty() ? 0 : strtoull(sz.c_str(), nullptr, 10);
            extractTag(item, "ETag", 0, obj.etag, next);
            extractTag(item, "LastModified", 0, obj.last_modified, next);
            out.push_back(obj);
            pos = e + 11;
            if ((int) out.size() >= max_keys) {
                break;
            }
        }
        token.clear();
        std::string truncated;
        size_t next = 0;
        extractTag(resp.body, "IsTruncated", 0, truncated, next);
        if (toLower(truncated) != "true") {
            break;
        }
        std::string next_token;
        if (!extractTag(resp.body, "NextContinuationToken", 0, next_token, next)) {
            break;
        }
        token = next_token;
    }
    return true;
}

std::string S3Client::objectUrl(const std::string &key) const {
    auto &ep = _impl->ep;
    std::string url = ep.scheme + "://" + hostHeader(ep);
    if (ep.base_path != "/") {
        url += ep.base_path;
    }
    if (_impl->cfg.path_style) {
        url += "/" + uriEncode(_impl->cfg.bucket, false) + "/" + uriEncode(key, true);
    } else {
        url = ep.scheme + "://" + uriEncode(_impl->cfg.bucket, false) + "." + hostHeader(ep) + "/" +
              uriEncode(key, true);
    }
    return url;
}

std::string S3Client::presignGet(const std::string &key, int expires_sec) const {
    // 预签名 v4：不签 content-sha256（用 UNSIGNED-PAYLOAD），其余同普通签名，
    // 但把签名放进 query 而不是 Authorization 头
    std::string amz_date, date_stamp;
    toAmzDate(amz_date, date_stamp);
    auto &ep = _impl->ep;
    std::string scope = date_stamp + "/" + _impl->cfg.region + "/s3/aws4_request";
    std::string cred = _impl->cfg.access_key + "/" + scope;

    std::vector<std::pair<std::string, std::string>> q = {
            {"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
            {"X-Amz-Credential", cred},
            {"X-Amz-Date", amz_date},
            {"X-Amz-Expires", std::to_string(expires_sec)},
            {"X-Amz-SignedHeaders", "host"},
    };
    if (!_impl->cfg.session_token.empty()) {
        q.emplace_back("X-Amz-Security-Token", _impl->cfg.session_token);
    }
    auto canon_uri = canonicalPath(_impl->cfg, ep, key);
    auto canon_query = canonicalQuery(q);
    std::string canon_headers = "host:" + hostHeader(ep) + "\n";
    std::string canon_request =
            "GET\n" + canon_uri + "\n" + canon_query + "\n" + canon_headers + "\n" + "host\n" +
            "UNSIGNED-PAYLOAD";
    std::string string_to_sign = "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
                                 crypto::sha256Hex(canon_request);
    auto k_date = crypto::hmacSha256Raw("AWS4" + _impl->cfg.secret_key, date_stamp);
    auto k_region = crypto::hmacSha256Raw(k_date, _impl->cfg.region);
    auto k_service = crypto::hmacSha256Raw(k_region, "s3");
    auto k_signing = crypto::hmacSha256Raw(k_service, "aws4_request");
    auto signature = crypto::hmacSha256Hex(k_signing, string_to_sign);

    std::string url = ep.scheme + "://" + hostHeader(ep) + canon_uri + "?" + canon_query +
                      "&X-Amz-Signature=" + signature;
    return url;
}

int S3Client::lastStatus() const {
    return _impl->last_status;
}

std::string S3Client::lastError() const {
    return _impl->last_err;
}

} // namespace mediakit
