/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */
#include "S3/S3HttpService.h"
#include "S3/S3Client.h"
#include "S3/S3Config.h"
#include "S3/S3Vod.h"

#include "Thread/WorkThreadPool.h"
#include "Util/util.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

using namespace std;
using namespace toolkit;

namespace mediakit {

namespace {

/// 2MB 以下整块读：一次往返就好，省掉分块调度的开销
const size_t kInlineMax = 2u << 20;
/// 流式传输每块的字节数（内存占用上限就靠这个控制）
const size_t kChunkSize = 1u << 20;

const char *mimeOfKey(const string &key) {
    auto dot = key.rfind('.');
    string ext = dot == string::npos ? "" : key.substr(dot);
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const map<string, const char *> kMime = {
            {".mp4", "video/mp4"},
            {".m4s", "video/iso.segment"},
            {".ts", "video/mp2t"},
            {".flv", "video/x-flv"},
            {".m3u8", "application/vnd.apple.mpegurl"},
            {".mpd", "application/dash+xml"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".png", "image/png"},
            {".gif", "image/gif"},
            {".mp3", "audio/mpeg"},
            {".aac", "audio/aac"},
            {".wav", "audio/wav"},
            {".json", "application/json"},
            {".html", "text/html"},
            {".txt", "text/plain"},
            {".log", "text/plain"},
    };
    auto it = kMime.find(ext);
    return it == kMime.end() ? "application/octet-stream" : it->second;
}

string htmlEscape(const string &s) {
    string out;
    out.reserve(s.size() + 16);
    for (auto c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

string humanSize(uint64_t n) {
    static const char *unit[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    double v = (double) n;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    char buf[64];
    if (i == 0) {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long) n);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", v, unit[i]);
    }
    return buf;
}

/// 把 /s3/xxx 里的 xxx 取出来。
/// 注意：HttpSession 在分发前已经对 parser.url() 做过 UrlDecodePath，
/// 这里**不能**再解一次（否则文件名里的 % 会被解开两次）。
string keyFromUrl(const string &url) {
    string key = url;
    auto pos = key.find('?');
    if (pos != string::npos) {
        key = key.substr(0, pos);
    }
    if (key.compare(0, 3, "/s3") != 0) {
        return "";
    }
    key = key.substr(3);
    if (!key.empty() && key[0] == '/') {
        key = key.substr(1);
    }
    return key;
}

void sendString(int code, const string &content_type, const string &body, const S3HttpService::Invoker &cb,
                const StrCaseMap &header = StrCaseMap()) {
    cb(code, content_type, header, std::make_shared<HttpStringBody>(body));
}

void sendError(int code, const string &msg, const S3HttpService::Invoker &cb) {
    sendString(code, "text/plain", msg + "\n", cb);
}

/**
 * 大对象的分块读取体。
 *
 * 关键点：remainSize() 返回的是「还没发的字节数」，所以 ZLM 能设出正确的
 * Content-Length（keep-alive 保持、下载进度条准确）；真正的取数在后台线程做。
 */
class HttpS3RangeBody : public HttpBody {
public:
    HttpS3RangeBody(S3Client::Ptr cli, string key, uint64_t offset, uint64_t len)
            : _cli(std::move(cli)), _key(std::move(key)), _offset(offset), _remain((int64_t) len) {}

    int64_t remainSize() override { return _remain.load(); }

    Buffer::Ptr readData(size_t size) override {
        if (_remain.load() <= 0) {
            return nullptr;
        }
        size_t want = (size_t) min<uint64_t>((uint64_t) _remain.load(), size ? size : kChunkSize);
        string data, err;
        if (!_cli->getObjectRange(_key, _offset, want, data, err)) {
            WarnL << "云存储读取失败: " << _key << " offset=" << _offset << " err=" << err;
            return nullptr;
        }
        _offset += data.size();
        _remain -= (int64_t) data.size();
        return std::make_shared<BufferString>(std::move(data));
    }

    void readDataAsync(size_t size, const std::function<void(const Buffer::Ptr &buf)> &cb) override {
        if (_remain.load() <= 0) {
            cb(nullptr);
            return;
        }
        size_t want = (size_t) min<uint64_t>((uint64_t) _remain.load(), size ? size : kChunkSize);
        auto offset = _offset.load();
        auto cli = _cli;
        auto key = _key;
        // S3 客户端是阻塞 IO，放到工作线程池：poller 线程不能被一次网络抖动卡住
        auto self = std::static_pointer_cast<HttpS3RangeBody>(shared_from_this());
        WorkThreadPool::Instance().getExecutor()->async([self, cli, key, offset, want, cb]() {
            string data, err;
            if (!cli->getObjectRange(key, offset, want, data, err)) {
                WarnL << "云存储读取失败: " << key << " offset=" << offset << " err=" << err;
                cb(nullptr);
                return;
            }
            self->_offset += data.size();
            self->_remain -= (int64_t) data.size();
            cb(std::make_shared<BufferString>(std::move(data)));
        });
    }

private:
    S3Client::Ptr _cli;
    string _key;
    // 取数发生在工作线程，回收发生在 poller 线程：计数用原子量，避免数据竞争
    std::atomic<uint64_t> _offset;
    std::atomic<int64_t> _remain;
};

/// 解析单区间 Range。多区间下载视频没人用，S3 本身也只支持单区间。
bool parseRange(const string &range, uint64_t total, uint64_t &start, uint64_t &end) {
    if (total == 0 || range.compare(0, 6, "bytes=") != 0) {
        return false;
    }
    auto spec = range.substr(6);
    auto dash = spec.find('-');
    if (dash == string::npos) {
        return false;
    }
    auto first = spec.substr(0, dash);
    auto second = spec.substr(dash + 1);
    if (first.empty()) {
        // bytes=-N：最后 N 个字节
        if (second.empty()) {
            return false;
        }
        uint64_t n = strtoull(second.c_str(), nullptr, 10);
        if (n == 0) {
            return false;
        }
        start = n >= total ? 0 : total - n;
        end = total - 1;
        return true;
    }
    start = strtoull(first.c_str(), nullptr, 10);
    if (second.empty()) {
        end = total - 1;
    } else {
        end = strtoull(second.c_str(), nullptr, 10);
        if (end >= total) {
            end = total - 1;
        }
    }
    return start < total && start <= end;
}

string urlEncodeKey(const string &key) {
    static const char *hex = "0123456789ABCDEF";
    string out;
    out.reserve(key.size());
    for (unsigned char c : key) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char) c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

/// 对象下载/点播：先 HEAD 拿大小，再按 Range 决定 200/206 与传输方式
void sendObject(S3Client::Ptr cli, Parser &parser, const string &key, const S3HttpService::Invoker &cb) {
    S3Object info;
    string err;
    if (!cli->headObject(key, info, err)) {
        sendError(404, "对象不存在: " + key + (err.empty() ? "" : "（" + err + "）"), cb);
        return;
    }

    StrCaseMap header;
    header["Accept-Ranges"] = "bytes";
    header["Content-Disposition"] =
            (parser.getUrlArgs().count("download") ? "attachment" : "inline") + string("; filename=\"") +
            key.substr(key.rfind('/') == string::npos ? 0 : key.rfind('/') + 1) + "\"";
    // 录制产物按名字就不会变（同一天同一时段），允许浏览器/CDN 短时缓存，拖动时不至于回源多次
    header["Cache-Control"] = "public, max-age=60";

    uint64_t start = 0, end = info.size ? info.size - 1 : 0;
    bool ranged = false;
    const string &range = parser["Range"];
    if (!range.empty()) {
        if (!parseRange(range, info.size, start, end)) {
            header["Content-Range"] = "bytes */" + to_string(info.size);
            sendString(416, "text/plain", "Range 不可满足\n", cb, header);
            return;
        }
        ranged = true;
        header["Content-Range"] = "bytes " + to_string(start) + "-" + to_string(end) + "/" + to_string(info.size);
    }

    uint64_t len = info.size ? (end - start + 1) : 0;
    int code = ranged ? 206 : 200;
    const char *mime = mimeOfKey(key);

    if (len == 0) {
        sendString(code, mime, "", cb, header);
        return;
    }
    if (len <= kInlineMax) {
        // 小对象：整块取回来直接发（一次往返，比两次快）
        string data;
        if (!cli->getObjectRange(key, start, len, data, err)) {
            sendError(502, "读取对象失败: " + err, cb);
            return;
        }
        sendString(code, mime, data, cb, header);
        return;
    }
    cb(code, mime, header, std::make_shared<HttpS3RangeBody>(cli, key, start, len));
}

/// 菜单页：按前缀列对象，文件可点播/下载，子目录可下钻
string buildMenu(S3Client::Ptr cli, const string &prefix, const string &host, string &err) {
    vector<S3Object> objs;
    const int kMaxKeys = 1000;
    if (!cli->listObjects(prefix, objs, kMaxKeys, err)) {
        return "";
    }

    struct DirEntry {
        uint64_t count = 0;
        uint64_t bytes = 0;
    };
    map<string, DirEntry> dirs;
    vector<const S3Object *> files;

    for (auto &o : objs) {
        if (o.key.size() <= prefix.size()) {
            continue;
        }
        auto rest = o.key.substr(prefix.size());
        auto slash = rest.find('/');
        if (slash == string::npos) {
            if (!rest.empty()) {
                files.push_back(&o);
            }
        } else {
            auto name = rest.substr(0, slash);
            if (name.empty()) {
                continue;
            }
            auto &d = dirs[name];
            d.count++;
            d.bytes += o.size;
        }
    }

    ostringstream html;
    html << "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         << "<title>云存储 · /" << htmlEscape(prefix) << "</title><style>"
         << "body{font:14px/1.6 -apple-system,'Segoe UI','Microsoft YaHei',sans-serif;"
         << "margin:0;padding:24px 32px;color:#222;background:#fff}"
         << "h1{font-size:18px;font-weight:600;margin:0 0 4px}"
         << ".sub{color:#888;font-size:12px;margin-bottom:18px;word-break:break-all}"
         << "table{border-collapse:collapse;width:100%;max-width:1100px}"
         << "th,td{text-align:left;padding:7px 10px;border-bottom:1px solid #ececec;font-size:13px}"
         << "th{color:#666;font-weight:600;background:#fafafa;border-top:1px solid #ececec}"
         << "td.n{text-align:right;color:#666;font-variant-numeric:tabular-nums;white-space:nowrap}"
         << "a{color:#1668dc;text-decoration:none}a:hover{text-decoration:underline}"
         << ".dir{font-weight:600}.tag{color:#999;font-size:12px;margin-left:6px}"
         << ".empty{color:#999;padding:24px 0}"
         << ".tip{color:#666;font-size:12px;margin:-8px 0 14px}"
         << "code{background:#f5f5f5;padding:1px 5px;border-radius:3px;font-size:12px}</style></head><body>";

    html << "<h1>云存储菜单</h1><div class=\"sub\">前缀：/<b>" << htmlEscape(prefix)
         << "</b> · 对象 " << objs.size() << " 个";
    if ((int) objs.size() >= kMaxKeys) {
        html << "（已达上限 " << kMaxKeys << "，请进子目录查看）";
    }
    html << "</div><div class=\"tip\">云点播：<code>rtsp://" << htmlEscape(host)
         << "/" << S3Vod::APP_NAME << "/&lt;对象名&gt;</code>（rtmp / http-flv 把协议和前缀换掉即可）</div>"
         << "<table><tr><th>名称</th><th class=\"n\">大小</th><th class=\"n\">时间</th><th>操作</th></tr>";

    if (!prefix.empty()) {
        auto parent = prefix;
        if (!parent.empty() && parent.back() == '/') {
            parent.pop_back();
        }
        auto slash = parent.rfind('/');
        parent = slash == string::npos ? "" : parent.substr(0, slash + 1);
        html << "<tr><td colspan=\"4\"><a href=\"/s3/" << urlEncodeKey(parent) << "\">↑ 上一级</a></td></tr>";
    }

    for (auto &kv : dirs) {
        auto path = prefix + kv.first + "/";
        html << "<tr><td class=\"dir\"><a href=\"/s3/" << urlEncodeKey(path) << "\">" << htmlEscape(kv.first)
             << "/</a><span class=\"tag\">" << kv.second.count << " 个 · " << humanSize(kv.second.bytes)
             << "</span></td><td class=\"n\">—</td><td class=\"n\">—</td><td>—</td></tr>";
    }

    for (auto *o : files) {
        auto name = o->key.substr(prefix.size());
        auto path = "/s3/" + urlEncodeKey(o->key);
        string name_lower = name;
        transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        bool playable = name_lower.size() > 4 &&
                        (name_lower.rfind(".mp4") == name_lower.size() - 4 ||
                         name_lower.rfind(".m3u8") == name_lower.size() - 5);
        html << "<tr><td><a href=\"" << path << "\">" << htmlEscape(name) << "</a></td><td class=\"n\">"
             << humanSize(o->size) << "</td><td class=\"n\">" << htmlEscape(o->last_modified) << "</td><td>";
        if (playable) {
            html << "<a href=\"" << path << "\" target=\"_blank\">播放</a> · ";
        }
        html << "<a href=\"" << path << "?download=1\">下载</a></td></tr>";
    }

    if (dirs.empty() && files.empty()) {
        html << "<tr><td colspan=\"4\" class=\"empty\">该前缀下没有对象</td></tr>";
    }
    html << "</table></body></html>";
    return html.str();
}

} // namespace

bool S3HttpService::match(const Parser &parser) {
    const auto &url = parser.url();
    if (url.compare(0, 3, "/s3") != 0) {
        return false;
    }
    // 只认 /s3 与 /s3/...，别把 /s3abc 也吃进来
    return url.size() == 3 || url[3] == '/' || url[3] == '?';
}

void S3HttpService::onRequest(Parser &parser, const S3HttpService::Invoker &cb) {
    auto cli = S3ConfigManager::instance().client();
    if (!cli) {
        sendError(503, "云存储未启用（s3.enable=0 或配置不全），请先配置 s3.* 并下发", cb);
        return;
    }

    auto key = keyFromUrl(parser.url());
    // 目录 / 根：给菜单页
    if (key.empty() || key.back() == '/') {
        string err;
        auto html = buildMenu(cli, key, parser["Host"], err);
        if (html.empty()) {
            sendError(502, "列对象失败: " + err, cb);
            return;
        }
        sendString(200, "text/html", html, cb);
        return;
    }

    sendObject(cli, parser, key, cb);
}

} // namespace mediakit
