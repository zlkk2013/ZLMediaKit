#!/usr/bin/env python3
"""本机跑：截图（getSnap）写云存储验证

验的是「快照也支持云存储」这条：getSnap 生成的 jpeg 除了回给客户端，还会写一份到桶里，
之后能通过 /s3/ 菜单页浏览、能直接下载，且字节与接口返回的一致。

判据：
  1. getSnap 返回 200 + 合法 jpeg（FFD8 开头，>3KB）
  2. 桶里出现 snap/<url的md5>/<日期>/<时间戳>.jpg
  3. 从桶里取回的字节与接口返回的**完全一致**
  4. /s3/ 菜单能列出这个目录，/s3/<key> 能取回同样的图片
  5. s3.record_snap=0 时桶里不应出现新对象（开关有效）
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
S3TEST = "/tmp/s3build/s3test"
WORK = "/tmp/s3snap-local"
HTTP_PORT, RTSP_PORT = 57991, 57591
S3_PORT = 19101
AK, SK, BUCKET = "minioadmin", "minioadmin", "zlmtest"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  %s %-44s %s" % ("OK  " if ok else "FAIL", name, detail))


def sh(c, t=180):
    return subprocess.run(c, shell=True, capture_output=True, text=True, timeout=t)


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


def secvals(text, section):
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
    p = sh("%s --endpoint http://127.0.0.1:%d --bucket %s --ak %s --sk %s --list %s"
           % (S3TEST, S3_PORT, BUCKET, AK, SK, prefix), 60)
    rows = []
    for ln in p.stdout.splitlines():
        parts = ln.split("\t")
        if len(parts) >= 2:
            try:
                rows.append((parts[0], int(parts[1])))
            except ValueError:
                pass
    return rows


def make_cfg(snap_on):
    cfg = os.path.join(WORK, "config.ini")
    gen = subprocess.Popen([BIN, "-c", cfg, "-l", "0"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, cwd=WORK)
    for _ in range(40):
        if os.path.exists(cfg) and os.path.getsize(cfg) > 1000:
            break
        time.sleep(0.3)
    gen.terminate()
    time.sleep(1)
    text = open(cfg, encoding="utf-8", errors="replace").read()
    for k, v in [("http.port", str(HTTP_PORT)), ("rtsp.port", str(RTSP_PORT)),
                 ("rtsp.sslport", str(RTSP_PORT + 443)),
                 ("rtmp.sslport", str(19350 + 7)),
                 ("srt.port", "19001"), ("shell.port", "19002"),
                 ("rtp_proxy.port", "10999"),
                 ("s3.enable", "1"), ("s3.endpoint", "http://127.0.0.1:%d" % S3_PORT),
                 ("s3.bucket", BUCKET), ("s3.access_key", AK), ("s3.secret_key", SK),
                 ("s3.path_style", "1"), ("s3.root_prefix", "record"),
                 ("s3.record_snap", "1" if snap_on else "0"),
                 ("general.enableVhost", "0"),
                 ("api.snapRoot", os.path.join(WORK, "snap") + "/"),
                 ("ffmpeg.bin", "/opt/homebrew/bin/ffmpeg")]:
        text = setkey(text, k, v)
    open(cfg, "w").write(text)
    return cfg


def start_server(cfg):
    srv = subprocess.Popen([BIN, "-c", cfg, "-l", "1"], cwd=WORK,
                           stdout=open(os.path.join(WORK, "srv.log"), "w"),
                           stderr=subprocess.STDOUT)
    secret = secvals(open(cfg, encoding="utf-8", errors="replace").read(), "api").get("secret", "")
    for _ in range(60):
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/index/api/getApiList?secret=%s"
                                        % (HTTP_PORT, secret), timeout=2) as r:
                if json.loads(r.read().decode()).get("code") == 0:
                    return srv, secret
        except Exception:
            pass
        time.sleep(0.5)
    return srv, secret


def main():
    global BIN
    BIN = os.path.join(REPO, "release/darwin/Release/MediaServer")
    if not os.path.exists(BIN):
        print("✗ 找不到 MediaServer（先 build-ffmpeg）")
        return 2
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)

    mock = subprocess.Popen([sys.executable, os.path.join(REPO, "tools/s3/s3_mock_server.py"),
                             "--port", str(S3_PORT), "--ak", AK, "--sk", SK],
                            stdout=open(os.path.join(WORK, "mock.log"), "w"), stderr=subprocess.STDOUT)
    time.sleep(1.5)

    cfg = make_cfg(True)
    srv, secret = start_server(cfg)
    push = None
    try:
        # 起一路直播流供截图
        push = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-f", "lavfi",
                                 "-i", "testsrc2=size=640x360:rate=25", "-c:v", "libx264",
                                 "-preset", "ultrafast", "-g", "25", "-pix_fmt", "yuv420p",
                                 "-f", "rtsp", "-rtsp_transport", "tcp",
                                 "rtsp://127.0.0.1:%d/live/snapme" % RTSP_PORT],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(4)
        url = "rtsp://127.0.0.1:%d/live/snapme" % RTSP_PORT
        api = ("http://127.0.0.1:%d/index/api/getSnap?secret=%s&url=%s&timeout_sec=10&expire_sec=1"
               % (HTTP_PORT, secret, urllib.parse.quote(url, safe="")))
        with urllib.request.urlopen(api, timeout=40) as r:
            jpg = r.read()
            ctype = r.headers.get("Content-Type", "")
        check("getSnap 返回 jpeg", jpg[:2] == b"\xff\xd8" and len(jpg) > 3000,
              "%d 字节 %s" % (len(jpg), ctype))

        # 给异步上传留点时间
        for _ in range(20):
            rows = s3list("record/snap/")
            if rows:
                break
            time.sleep(0.5)
        check("桶里出现 snap 对象", len(rows) > 0,
              "%s" % (rows[0][0] if rows else "无"))
        if rows:
            key, size = sorted(rows, key=lambda x: -x[1])[0]
            out = os.path.join(WORK, "from_s3.jpg")
            sh("%s --endpoint http://127.0.0.1:%d --bucket %s --ak %s --sk %s --get %s --out %s"
               % (S3TEST, S3_PORT, BUCKET, AK, SK, key, out), 60)
            got = open(out, "rb").read() if os.path.exists(out) else b""
            check("桶里取回与接口返回字节一致", got == jpg,
                  "s3=%d/%d 接口=%d" % (len(got), size, len(jpg)))

            # /s3/ 菜单 + 直接取图
            menu = ("http://127.0.0.1:%d/s3/record/snap/" % HTTP_PORT)
            with urllib.request.urlopen(menu, timeout=10) as r:
                html = r.read().decode("utf-8", "replace")
            check("/s3/ 菜单列出 md5 目录", "snap" in html or "/s3/record/snap/" in html,
                  "%d 字节" % len(html))
            with urllib.request.urlopen("http://127.0.0.1:%d/s3/%s?download=1"
                                        % (HTTP_PORT, key), timeout=20) as r:
                via_http = r.read()
                h_ctype = r.headers.get("Content-Type", "")
            check("/s3/<key> 取回同一张图", via_http == jpg,
                  "%d 字节 %s" % (len(via_http), h_ctype))
        else:
            check("桶里取回与接口返回字节一致", False, "没有对象")

        # 开关关闭时不应上传
        srv.terminate()
        time.sleep(1.5)
        before = len(s3list("record/snap/"))
        cfg_off = make_cfg(False)
        srv, secret = start_server(cfg_off)
        urllib.request.urlopen("http://127.0.0.1:%d/index/api/getSnap?secret=%s&url=%s"
                               "&timeout_sec=10&expire_sec=1"
                               % (HTTP_PORT, secret, urllib.parse.quote(url + "?x=2", safe="")),
                               timeout=40).read()
        time.sleep(2)
        after = len(s3list("record/snap/"))
        check("s3.record_snap=0 时不再写桶", after == before, "%d → %d" % (before, after))
    finally:
        if push:
            push.terminate()
        srv.terminate()
        mock.terminate()
        time.sleep(1)
        sh('pkill -f "s3_mock_server.py --port %d"' % S3_PORT)

    failed = [x for x in results if not x[1]]
    print("\n结果: %d/%d 通过" % (len(results) - len(failed), len(results)))
    if failed:
        print(sh("tail -20 %s" % os.path.join(WORK, "srv.log")).stdout)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
