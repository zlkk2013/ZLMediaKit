#!/usr/bin/env python3
"""盒子上跑：云存储 HTTP 出口（/s3/ 路由）真机验证

验的是「浏览器/播放器只认 ZLM 的地址，也能看云上的录像」这条链路：
  1. /s3/                     → 菜单页（200，含对象名）
  2. /s3/<dir>/               → 子目录菜单页（200，含文件名）
  3. /s3/<key>                → 200，字节与 S3 里的对象完全一致，Content-Length 正确
  4. Range 任意区间            → 206，Content-Range 正确，取回的字节与本地同区间一致
  5. Range 后缀 bytes=-N       → 206，取末尾 N 字节
  6. Range 越界                → 416
  7. 大对象（>2MB，走分块流式） → 200，sha256 与源文件一致（证明分块拼接没错）
  8. HLS：取 m3u8 + 其中引用的切片（相对 URL）都能通过 /s3/ 取到

每项都比对真实字节，不做「返回 200 就算过」。
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = "/root/zlm-ascend"
BIN = REPO + "/release/linux/Release/MediaServer"
SRC_CFG = "/root/zlmtest/cfg_asc.ini"
CFG = "/root/zlmtest/cfg_s3http.ini"
SRC_MP4 = "/tmp/src720.mp4"
S3TEST = "/root/s3t/s3test"
HTTP, RTSP = 57982, 57556
S3_ENDPOINT = "http://127.0.0.1:19020"
BUCKET, AK, SK = "zlmtest", "zlmediakit", "zlmsecret"
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = ("/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/lib64:"
                          "/usr/local/Ascend/driver/lib64/driver:" + ENV.get("LD_LIBRARY_PATH", ""))

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  %s %-40s %s" % ("OK  " if ok else "FAIL", name, detail))


def sh(c, t=120):
    return subprocess.run(c, shell=True, capture_output=True, text=True, timeout=t)


def api(path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (HTTP, path), timeout=10) as r:
        return json.loads(r.read().decode())


class Resp:
    def __init__(self, code, headers, body):
        self.code, self.headers, self.body = code, headers, body


def http_get(path, headers=None, timeout=60):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (HTTP, path), headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return Resp(r.status, dict(r.headers), r.read())
    except urllib.error.HTTPError as e:
        return Resp(e.code, dict(e.headers), e.read())


def setkey(text, dotted, val):
    section, key = dotted.split(".", 1)
    lines = text.replace("\r\n", "\n").split("\n")
    out, cur, done, at = [], None, False, None
    for ln in lines:
        m = re.match(r"^\[(.+)\]\s*$", ln.strip())
        if m:
            cur = m.group(1)
            if cur == section and at is None:
                at = len(out) + 1
        if cur == section and re.match(r"^%s\s*=" % re.escape(key), ln.strip()):
            out.append("%s=%s" % (key, val))
            done = True
            continue
        out.append(ln)
    if not done:
        if at is None:
            out.extend(["", "[%s]" % section, "%s=%s" % (key, val)])
        else:
            out.insert(at, "%s=%s" % (key, val))
    return "\n".join(out)


def read_section(text, section):
    vals, cur = {}, None
    for ln in text.replace("\r\n", "\n").split("\n"):
        m = re.match(r"^\[(.+)\]\s*$", ln.strip())
        if m:
            cur = m.group(1)
        elif cur == section and "=" in ln:
            k, v = ln.strip().split("=", 1)
            vals[k.strip()] = v.strip()
    return vals


def s3list(prefix):
    p = sh("%s --endpoint %s --bucket %s --ak %s --sk %s --list %s"
           % (S3TEST, S3_ENDPOINT, BUCKET, AK, SK, prefix), 60)
    rows = []
    for ln in p.stdout.splitlines():
        parts = ln.split("\t")
        if len(parts) >= 2:
            try:
                rows.append((parts[0], int(parts[1])))
            except ValueError:
                pass
    return rows


def s3get(key, out):
    return sh("%s --endpoint %s --bucket %s --ak %s --sk %s --get %s --out %s"
              % (S3TEST, S3_ENDPOINT, BUCKET, AK, SK, key, out), 120)


def s3put(key, path):
    return sh("%s --endpoint %s --bucket %s --ak %s --sk %s --put %s --file %s"
              % (S3TEST, S3_ENDPOINT, BUCKET, AK, SK, key, path), 300)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    cfg = open(SRC_CFG, encoding="utf-8", errors="replace").read()
    for old, new in (("57980", str(HTTP)), ("57554", str(RTSP))):
        cfg = cfg.replace(old, new)
    for k, v in [("api.apiDebug", "1"), ("s3.enable", "1"),
                 ("s3.endpoint", S3_ENDPOINT), ("s3.bucket", BUCKET),
                 ("s3.access_key", AK), ("s3.secret_key", SK),
                 ("s3.record_mp4", "1"), ("s3.record_hls", "1"),
                 ("s3.root_prefix", "record"),
                 ("hls.segDur", "2"), ("hls.segNum", "6")]:
        cfg = setkey(cfg, k, v)
    open(CFG, "w").write(cfg)

    sh('for p in $(pgrep -f "[c]fg_s3http.ini"); do kill -9 $p; done')
    time.sleep(1)
    log = open("/root/zlmtest/s3_http.log", "w")
    srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=REPO, env=ENV, stdout=log,
                           stderr=subprocess.STDOUT)
    push = None
    try:
        secret = read_section(cfg, "api").get("secret", "")
        ready = False
        for _ in range(40):
            try:
                if api("/index/api/getApiList?secret=%s" % secret).get("code") == 0:
                    ready = True
                    break
            except Exception:
                pass
            time.sleep(0.5)
        if not ready:
            print("✗ MediaServer 没起来")
            print(sh("tail -20 /root/zlmtest/s3_http.log").stdout)
            return 1

        # ---------- 造一段 MP4 + 一段 HLS 录像 ----------
        st = "boxhttp%d" % int(time.time())
        push = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC_MP4,
                                 "-c", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
                                 "rtsp://127.0.0.1:%d/live/%s" % (RTSP, st)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(6)
        r = api("/index/api/startRecord?secret=%s&type=1&vhost=__defaultVhost__&app=live&stream=%s"
                % (secret, st))
        print("startRecord(mp4): code=%s" % r.get("code"))
        time.sleep(10)
        api("/index/api/stopRecord?secret=%s&type=1&vhost=__defaultVhost__&app=live&stream=%s" % (secret, st))
        r2 = api("/index/api/startRecord?secret=%s&type=2&vhost=__defaultVhost__&app=live&stream=%s"
                 % (secret, st))
        print("startRecord(hls): code=%s" % r2.get("code"))
        time.sleep(10)
        api("/index/api/stopRecord?secret=%s&type=2&vhost=__defaultVhost__&app=live&stream=%s" % (secret, st))
        time.sleep(6)

        rows = s3list("record/")
        mp4s = [x for x in rows if st in x[0] and x[0].endswith(".mp4")]
        m3u8s = [x for x in rows if st in x[0] and x[0].endswith(".m3u8")]
        if not mp4s or not m3u8s:
            print("✗ 没有造出录制品（mp4=%d m3u8=%d）" % (len(mp4s), len(m3u8s)))
            return 1
        mp4_key, mp4_size = sorted(mp4s, key=lambda x: -x[1])[0]
        day_prefix = mp4_key.rsplit("/", 1)[0].rsplit("/", 1)[0] + "/"
        print("\n测试对象: %s (%d 字节)" % (mp4_key, mp4_size))
        print("父前缀: %s\n" % day_prefix)

        # ---------- 1) 根菜单 ----------
        r = http_get("/s3/")
        body = r.body.decode("utf-8", "replace")
        check("/s3/ 返回菜单页", r.code == 200 and "云存储菜单" in body,
              "%d, %d 字节" % (r.code, len(r.body)))
        check("根菜单里能看到 record/ 目录", "record/" in body)

        # ---------- 2) 子目录菜单（逐级下钻） ----------
        # 流目录下面是日期目录，先验「日期目录被列出来」，再进日期目录验文件名
        r = http_get("/s3/" + day_prefix)
        body = r.body.decode("utf-8", "replace")
        date_dir = mp4_key[len(day_prefix):].split("/")[0]
        check("流目录菜单列出日期子目录", r.code == 200 and date_dir in body,
              "%d, %d 字节" % (r.code, len(r.body)))
        file_dir = mp4_key.rsplit("/", 1)[0] + "/"
        r = http_get("/s3/" + file_dir)
        body = r.body.decode("utf-8", "replace")
        check("日期目录菜单列出文件名", r.code == 200 and os.path.basename(mp4_key) in body,
              "%d, %d 字节" % (r.code, len(r.body)))

        # ---------- 3) 整对象下载，字节比对 ----------
        r = http_get("/s3/" + mp4_key)
        src = "/tmp/http_ref.mp4"
        s3get(mp4_key, src)
        ref = open(src, "rb").read()
        check("整对象 200 且字节一致", r.code == 200 and r.body == ref,
              "%d, %d 字节, sha=%s" % (r.code, len(r.body), sha256(r.body)[:12]))
        check("Content-Length 正确", r.headers.get("Content-Length") == str(len(ref)),
              r.headers.get("Content-Length", "-"))
        check("Content-Type 是 video/mp4", r.headers.get("Content-Type", "").startswith("video/mp4"),
              r.headers.get("Content-Type", "-"))
        check("带 Accept-Ranges", r.headers.get("Accept-Ranges") == "bytes")

        # ---------- 4) Range 任意区间 ----------
        r = http_get("/s3/" + mp4_key, {"Range": "bytes=100-199"})
        check("Range 100-199 → 206 且字节一致",
              r.code == 206 and r.body == ref[100:200],
              "%d, %d 字节" % (r.code, len(r.body)))
        check("Content-Range 正确",
              r.headers.get("Content-Range") == "bytes 100-199/%d" % len(ref),
              r.headers.get("Content-Range", "-"))

        # ---------- 5) 后缀 Range ----------
        r = http_get("/s3/" + mp4_key, {"Range": "bytes=-128"})
        check("Range -128 → 206 且等于末尾 128 字节",
              r.code == 206 and r.body == ref[-128:], "%d, %d 字节" % (r.code, len(r.body)))

        # 开源 Range：bytes=1000-
        r = http_get("/s3/" + mp4_key, {"Range": "bytes=1000-"})
        check("Range 1000- → 206 且等于尾部",
              r.code == 206 and r.body == ref[1000:], "%d, %d 字节" % (r.code, len(r.body)))

        # ---------- 6) 越界 ----------
        r = http_get("/s3/" + mp4_key, {"Range": "bytes=%d-" % (len(ref) + 10)})
        check("越界 Range → 416", r.code == 416, str(r.code))

        # ---------- 7) 大对象走分块流式 ----------
        big_local = "/tmp/http_big.bin"
        sh("dd if=/dev/urandom of=%s bs=1M count=6 2>/dev/null" % big_local)
        big_key = "httpcheck/big.bin"
        p = s3put(big_key, big_local)
        if p.returncode != 0:
            check("大对象上传（准备步骤）", False, p.stderr.strip()[:120])
        else:
            want = open(big_local, "rb").read()
            r = http_get("/s3/" + big_key, timeout=180)
            check("6MB 对象流式下载字节一致",
                  r.code == 200 and r.body == want,
                  "%d, %d 字节（sha=%s）" % (r.code, len(r.body), sha256(r.body)[:12]))
            # 大对象的中间区间（跨多个 1MB 块）
            r = http_get("/s3/" + big_key, {"Range": "bytes=1000000-3500000"}, timeout=180)
            check("6MB 对象中间区间一致",
                  r.code == 206 and r.body == want[1000000:3500001],
                  "%d, %d 字节" % (r.code, len(r.body)))
            sh("%s --endpoint %s --bucket %s --ak %s --sk %s --get %s --out /dev/null"
               % (S3TEST, S3_ENDPOINT, BUCKET, AK, SK, big_key))

        # ---------- 8) HLS 经 /s3/ 可播 ----------
        hls_key = sorted(m3u8s)[0][0]
        r = http_get("/s3/" + hls_key)
        text = r.body.decode("utf-8", "replace")
        refs = [l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("#")]
        check("m3u8 200 且非空", r.code == 200 and len(refs) > 0,
              "%d, 引用 %d 段" % (r.code, len(refs)))
        base_url = "/s3/" + hls_key.rsplit("/", 1)[0] + "/"
        ok_seg = 0
        for rel in refs[:3]:
            seg = http_get(base_url + rel)
            if seg.code == 200 and len(seg.body) > 1000:
                ok_seg += 1
        check("m3u8 引用的切片经 /s3/ 可取", ok_seg == min(3, len(refs)),
              "%d/%d 段" % (ok_seg, min(3, len(refs))))

        # ---------- 9) 不存在的对象 ----------
        r = http_get("/s3/nope/does-not-exist.mp4")
        check("不存在的对象 → 404", r.code == 404, str(r.code))
    finally:
        if push:
            push.terminate()
        srv.terminate()
        time.sleep(2)
        sh('for p in $(pgrep -f "[c]fg_s3http.ini"); do kill -9 $p; done')

    failed = [x for x in results if not x[1]]
    print("\n结果: %d/%d 通过" % (len(results) - len(failed), len(results)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
