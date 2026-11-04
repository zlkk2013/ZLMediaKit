#!/usr/bin/env python3
"""S3 兼容模拟服务（仅供开发/CI 用，不是生产件）

存在的意义有两个：
  1. 离线/内网环境里没有 MinIO 可用时，能给 S3 客户端做端到端验证；
  2. **它独立实现一遍 SigV4 校验**（Python hashlib/hmac），所以能真正验证
     C++ 客户端的签名对不对 —— 我们自己的 mock 若能验签通过，
     说明签名算法与 AWS 文档一致，不是自说自话。

支持：建桶 / PUT / GET(含 Range) / HEAD / DELETE / LIST v2 / 分片上传(init/part/complete/abort)。

用法：
    python3 s3_mock_server.py --port 9000 --ak zlmediakit --sk zlmsecret
"""
import argparse
import hashlib
import hmac
import json
import re
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EMPTY_SHA = hashlib.sha256(b"").hexdigest()


class Store:
    def __init__(self):
        self.buckets = {}          # bucket -> {key: bytes}
        self.uploads = {}          # upload_id -> {"bucket","key","parts":{n: bytes}}
        self.lock = threading.Lock()
        self.seq = 0

    def next_id(self):
        with self.lock:
            self.seq += 1
            return "upload-%06d" % self.seq


STORE = Store()
AK = "zlmediakit"
SK = "zlmsecret"
REGION = "us-east-1"


def sign(key, msg):
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def canonical_query(qs):
    """按 key/value 排序并用 %20 风格编码（与客户端/AWS 一致）"""
    items = []
    for k, v in urllib.parse.parse_qsl(qs, keep_blank_values=True):
        items.append((urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~")))
    items.sort()
    return "&".join("%s=%s" % (k, v) for k, v in items)


def verify_sigv4(handler, body, err):
    """独立实现一遍 SigV4 校验；返回 (ok, 诊断信息)"""
    auth = handler.headers.get("Authorization", "")
    m = re.match(r"AWS4-HMAC-SHA256 Credential=([^/]+)/(\d{8})/([^/]+)/s3/aws4_request, "
                 r"SignedHeaders=([^,]+), Signature=([0-9a-f]{64})", auth)
    if not m:
        err.append("Authorization 头格式不对: %r" % auth[:120])
        return False
    ak, date, region, signed_headers, client_sig = m.groups()
    if ak != AK:
        err.append("access key 不匹配: %s" % ak)
        return False
    amz_date = handler.headers.get("x-amz-date", "")
    if not amz_date.startswith(date):
        err.append("x-amz-date 与 scope 日期不一致")
        return False

    parsed = urllib.parse.urlsplit(handler.path)
    canon_uri = urllib.parse.quote(urllib.parse.unquote(parsed.path), safe="/-_.~")
    canon_qs = canonical_query(parsed.query)

    hdrs = []
    for name in signed_headers.split(";"):
        val = handler.headers.get(name, "")
        hdrs.append("%s:%s\n" % (name, " ".join(val.split())))
    canon_headers = "".join(hdrs)

    payload_hash = handler.headers.get("x-amz-content-sha256", EMPTY_SHA)
    canon_request = "\n".join([handler.command, canon_uri, canon_qs, canon_headers,
                               signed_headers, payload_hash])
    scope = "%s/%s/s3/aws4_request" % (date, region)
    sts = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope,
                     hashlib.sha256(canon_request.encode()).hexdigest()])
    k = sign(("AWS4" + SK).encode(), date)
    k = sign(k, region)
    k = sign(k, "s3")
    k = sign(k, "aws4_request")
    expect = hmac.new(k, sts.encode(), hashlib.sha256).hexdigest()
    if expect != client_sig:
        err.append("签名不匹配: client=%s expect=%s" % (client_sig[:16], expect[:16]))
        return False
    # 载荷完整性：客户端既然声明了 hash，就核对一遍（分片上传/普通 PUT 都适用）
    if payload_hash != EMPTY_SHA:
        actual = hashlib.sha256(body).hexdigest()
        if actual != payload_hash:
            err.append("payload hash 不匹配")
            return False
    return True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if "/healthz" in (args[0] if args else ""):
            return
        sys.stderr.write("[s3mock] %s - %s\n" % (self.address_string(), fmt % args))

    # ---------- 工具 ----------

    def _read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def _send(self, code, body=b"", headers=None, head_only=False):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body and not head_only:
            self.wfile.write(body)

    def _xml(self, body):
        return body.encode("utf-8")

    def _parse(self):
        parsed = urllib.parse.urlsplit(self.path)
        qs = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        path = urllib.parse.unquote(parsed.path).lstrip("/")
        bucket, _, key = path.partition("/")
        return bucket, key, qs

    def _deny(self, err):
        self._send(403, self._xml("<Error><Code>SignatureDoesNotMatch</Code><Message>%s</Message>"
                                  "</Error>" % "; ".join(err)))

    # ---------- 路由 ----------

    def do_PUT(self):
        body = self._read_body()
        err = []
        if not verify_sigv4(self, body, err):
            return self._deny(err)
        bucket, key, qs = self._parse()
        if not bucket:
            return self._send(400, self._xml("<Error><Code>InvalidBucketName</Code></Error>"))

        # 分片上传的一个分片
        if "partNumber" in qs and "uploadId" in qs:
            uid = qs["uploadId"][0]
            part = int(qs["partNumber"][0])
            with STORE.lock:
                up = STORE.uploads.get(uid)
                if not up:
                    return self._send(404, self._xml("<Error><Code>NoSuchUpload</Code></Error>"))
                up["parts"][part] = body
            etag = '"%s"' % hashlib.md5(body).hexdigest()
            return self._send(200, b"", {"ETag": etag})

        if not key:   # 建桶
            with STORE.lock:
                if bucket in STORE.buckets:
                    return self._send(409, self._xml(
                        "<Error><Code>BucketAlreadyOwnedByYou</Code></Error>"))
                STORE.buckets[bucket] = {}
            return self._send(200)

        with STORE.lock:
            STORE.buckets.setdefault(bucket, {})[key] = body
        return self._send(200, b"", {"ETag": '"%s"' % hashlib.md5(body).hexdigest()})

    def do_POST(self):
        body = self._read_body()
        err = []
        if not verify_sigv4(self, body, err):
            return self._deny(err)
        bucket, key, qs = self._parse()
        if "uploads" in qs:   # 初始化
            uid = STORE.next_id()
            with STORE.lock:
                STORE.uploads[uid] = {"bucket": bucket, "key": key, "parts": {}}
            xml = ("<InitiateMultipartUploadResult><Bucket>%s</Bucket><Key>%s</Key>"
                   "<UploadId>%s</UploadId></InitiateMultipartUploadResult>" % (bucket, key, uid))
            return self._send(200, self._xml(xml))

        if "uploadId" in qs:  # 完成
            uid = qs["uploadId"][0]
            with STORE.lock:
                up = STORE.uploads.get(uid)
                if not up:
                    return self._send(404, self._xml("<Error><Code>NoSuchUpload</Code></Error>"))
                parts = [up["parts"][n] for n in sorted(up["parts"])]
                data = b"".join(parts)
                STORE.buckets.setdefault(up["bucket"], {})[up["key"]] = data
                STORE.uploads.pop(uid, None)
            xml = ("<CompleteMultipartUploadResult><Bucket>%s</Bucket><Key>%s</Key>"
                   "<ETag>\"%s\"</ETag></CompleteMultipartUploadResult>"
                   % (bucket, key, hashlib.md5(data).hexdigest()))
            return self._send(200, self._xml(xml))

        return self._send(400, self._xml("<Error><Code>BadRequest</Code></Error>"))

    def do_DELETE(self):
        err = []
        if not verify_sigv4(self, b"", err):
            return self._deny(err)
        bucket, key, qs = self._parse()
        if "uploadId" in qs:
            with STORE.lock:
                STORE.uploads.pop(qs["uploadId"][0], None)
            return self._send(204)
        with STORE.lock:
            if bucket in STORE.buckets:
                STORE.buckets[bucket].pop(key, None)
        return self._send(204)

    def do_HEAD(self):
        err = []
        if not verify_sigv4(self, b"", err):
            return self._deny(err)
        bucket, key, _ = self._parse()
        with STORE.lock:
            data = STORE.buckets.get(bucket, {}).get(key)
        if data is None:
            return self._send(404, b"", {"Content-Length": "0"}, head_only=True)
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("ETag", '"%s"' % hashlib.md5(data).hexdigest())
        self.send_header("Last-Modified", "Thu, 01 Jan 2026 00:00:00 GMT")
        self.end_headers()

    def do_GET(self):
        err = []
        if not verify_sigv4(self, b"", err):
            return self._deny(err)
        bucket, key, qs = self._parse()

        if not key:   # LIST v2
            prefix = qs.get("prefix", [""])[0]
            max_keys = int(qs.get("max-keys", ["1000"])[0])
            with STORE.lock:
                items = sorted((k, v) for k, v in STORE.buckets.get(bucket, {}).items()
                               if k.startswith(prefix))[:max_keys]
            body = ["<ListBucketResult><Name>%s</Name><Prefix>%s</Prefix>"
                    "<KeyCount>%d</KeyCount><IsTruncated>false</IsTruncated>"
                    % (bucket, prefix, len(items))]
            for k, v in items:
                body.append("<Contents><Key>%s</Key><Size>%d</Size>"
                            "<LastModified>2026-01-01T00:00:00.000Z</LastModified>"
                            "<ETag>\"%s\"</ETag></Contents>" % (k, len(v), hashlib.md5(v).hexdigest()))
            body.append("</ListBucketResult>")
            return self._send(200, self._xml("".join(body)))

        with STORE.lock:
            data = STORE.buckets.get(bucket, {}).get(key)
        if data is None:
            return self._send(404, self._xml("<Error><Code>NoSuchKey</Code></Error>"))

        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d+)-(\d+)", rng)
            if m:
                a, b = int(m.group(1)), int(m.group(2))
                chunk = data[a:min(b + 1, len(data))]
                return self._send(206, chunk, {
                    "Content-Range": "bytes %d-%d/%d" % (a, a + len(chunk) - 1, len(data)),
                    "Accept-Ranges": "bytes"})
        return self._send(200, data, {"Accept-Ranges": "bytes"})


def main():
    global AK, SK
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--ak", default="zlmediakit")
    ap.add_argument("--sk", default="zlmsecret")
    a = ap.parse_args()
    AK, SK = a.ak, a.sk
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    print("[s3mock] listening on :%d (ak=%s)" % (a.port, AK), file=sys.stderr)
    srv.serve_forever()


if __name__ == "__main__":
    main()
