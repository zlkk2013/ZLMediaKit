#!/usr/bin/env python3
"""本机（macOS）跑：云点播（s3vod）端到端验证 —— 不需要真机或真 S3

用仓库里自带的「独立实现 SigV4 校验」的 mock S3 + 真实 MediaServer，
验的是同一套代码路径：对象 → MP4FileS3（Range 读 + seek）→ MP4Demuxer → 媒体源。

判据：
  1. rtsp://host/s3vod/<key> 出 h264，时长≈源文件
  2. rtmp://host/s3vod/<key> 出 h264
  3. http://host/s3vod/<key>.live.flv 拿到 >100KB 数据且含 h264
  4. -ss 5 取帧成功（seek + Range 读）
  5. 不存在的对象快速失败
  6. 本地从未落盘该 mp4
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = None
S3TEST = "/tmp/s3build/s3test"
WORK = "/tmp/s3vod-local"
HTTP_PORT, RTSP_PORT, RTMP_PORT = 57990, 57590, 19360
S3_PORT = 19100
AK, SK, BUCKET = "minioadmin", "minioadmin", "zlmtest"

results = []


def skip(name, detail=""):
    results.append((name, None, detail))
    print("  SKIP %-44s %s" % (name, detail))


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


def find_binary():
    for cand in ["build-ffmpeg/release/darwin/Release/MediaServer",
                 "build-ffmpeg/release/darwin/Debug/MediaServer",
                 "build/release/darwin/Release/MediaServer"]:
        p = os.path.join(REPO, cand)
        if os.path.exists(p):
            return p
    found = sh("find %s -name MediaServer -type f -perm +111 2>/dev/null | head -3" % REPO).stdout.split()
    return found[0] if found else None


def probe(url, extra="", timeout=40):
    t0 = time.time()
    p = sh("gtimeout %d ffprobe -v error %s -show_entries stream=codec_name,width,height:"
           "format=duration -of json %s" % (timeout, extra, url), timeout + 20)
    if p.returncode != 0 and "gtimeout" in p.stderr:
        # 本机没有 gtimeout 时退回不设超时（配合外层 time.time 检查）
        p = sh("ffprobe -v error %s -show_entries stream=codec_name,width,height:format=duration "
               "-of json %s" % (extra, url), timeout + 20)
    return p.stdout, p.stderr, time.time() - t0


def streams_of(js):
    try:
        info = json.loads(js or "{}")
    except Exception:
        return [], 0.0
    st = [(x.get("codec_name"), x.get("width")) for x in info.get("streams", [])]
    try:
        dur = float(info.get("format", {}).get("duration", 0) or 0)
    except Exception:
        dur = 0.0
    return st, dur


def main():
    global BIN
    BIN = find_binary()
    if not BIN:
        print("✗ 找不到 MediaServer（先 build-ffmpeg）")
        return 2
    if not os.path.exists(S3TEST):
        print("✗ 找不到 s3test（先按 README 编译 tools/s3/s3_selftest.cpp）")
        return 2
    print("MediaServer: %s" % BIN)
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)

    # ---------- mock S3 ----------
    mock_port = S3_PORT
    mock = subprocess.Popen([sys.executable, os.path.join(REPO, "tools/s3/s3_mock_server.py"),
                             "--port", str(mock_port), "--ak", AK, "--sk", SK],
                            stdout=open(os.path.join(WORK, "mock.log"), "w"),
                            stderr=subprocess.STDOUT)
    time.sleep(1.5)

    # ---------- 造一个 12s 的 mp4 并传到 mock ----------
    src = os.path.join(WORK, "src.mp4")
    sh("ffmpeg -v error -f lavfi -i testsrc2=size=640x360:rate=25:duration=12 "
       "-f lavfi -i sine=frequency=800:duration=12 -c:v libx264 -preset veryfast -pix_fmt yuv420p "
       "-c:a aac -shortest -y %s" % src, 180)
    assert os.path.exists(src), "造 mp4 失败"
    key = "record/live/vodlocal/2026-11-05/vod.mp4"
    p = sh("%s --endpoint http://127.0.0.1:%d --bucket %s --ak %s --sk %s --put %s --file %s"
           % (S3TEST, mock_port, BUCKET, AK, SK, key, src), 120)
    if p.returncode != 0:
        print("✗ 上传 mp4 失败:", p.stderr[-200:])
        return 1
    checksum = sh("shasum -a 256 %s | cut -d' ' -f1" % src).stdout.strip()
    print("对象: %s sha256=%s…" % (key, checksum[:12]))

    # ---------- MediaServer 配置 ----------
    cfg = os.path.join(WORK, "config.ini")
    # 先让服务自己生成一份默认配置（缺文件时会 dump）
    gen = subprocess.Popen([BIN, "-c", cfg, "-l", "0"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, cwd=WORK)
    for _ in range(30):
        if os.path.exists(cfg) and os.path.getsize(cfg) > 1000:
            break
        time.sleep(0.3)
    gen.terminate()
    time.sleep(1)
    text = open(cfg, encoding="utf-8", errors="replace").read()
    for k, v in [("http.port", str(HTTP_PORT)), ("rtsp.port", str(RTSP_PORT)),
                 ("rtmp.port", str(RTMP_PORT)),
                 ("s3.enable", "1"), ("s3.endpoint", "http://127.0.0.1:%d" % mock_port),
                 ("s3.bucket", BUCKET), ("s3.access_key", AK), ("s3.secret_key", SK),
                 ("s3.path_style", "1"), ("general.enableVhost", "0")]:
        text = setkey(text, k, v)
    open(cfg, "w").write(text)
    secret = secvals(text, "api").get("secret", "")

    srv = subprocess.Popen([BIN, "-c", cfg, "-l", "1"], stdout=open(os.path.join(WORK, "srv.log"), "w"),
                           stderr=subprocess.STDOUT, cwd=WORK)
    try:
        ready = False
        for _ in range(60):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/index/api/getApiList?secret=%s"
                                            % (HTTP_PORT, secret), timeout=2) as r:
                    if json.loads(r.read().decode()).get("code") == 0:
                        ready = True
                        break
            except Exception:
                pass
            time.sleep(0.5)
        if not ready:
            print("✗ MediaServer 没起来，日志尾部:")
            print(sh("tail -30 %s" % os.path.join(WORK, "srv.log")).stdout)
            return 1

        # ---------- 1) rtsp ----------
        url = "rtsp://127.0.0.1:%d/s3vod/%s" % (RTSP_PORT, key)
        js, err, cost = probe(url)
        st, dur = streams_of(js)
        check("rtsp 点播出 h264", any(x[0] in ("h264", "hevc") for x in st),
              "%s 用时 %.1fs %s" % (st, cost, err.strip()[:60]))
        check("rtsp 时长≈12s", 9 <= dur <= 15, "%.1fs" % dur)

        # ---------- 2) rtmp ----------
        url = "rtmp://127.0.0.1:%d/s3vod/%s" % (RTMP_PORT, key)
        js, err, cost = probe(url)
        st, dur = streams_of(js)
        check("rtmp 点播出 h264", any(x[0] in ("h264", "hevc") for x in st),
              "%s 用时 %.1fs" % (st, cost))

        # ---------- 3) http-flv ----------
        flv = os.path.join(WORK, "vod.flv")
        url = "http://127.0.0.1:%d/s3vod/%s.live.flv" % (HTTP_PORT, key)
        sh("ffmpeg -v error -i '%s' -t 3 -c copy -f flv -y %s" % (url, flv), 60)
        size = os.path.getsize(flv) if os.path.exists(flv) else 0
        js = sh("ffprobe -v error -show_entries stream=codec_name -of json %s" % flv, 30).stdout
        check("http-flv 拿到数据", size > 100 * 1024, "%d 字节" % size)
        check("http-flv 含 h264", "h264" in (js or ""), js.strip()[:60])

        # ---------- 4) seek ----------
        shot = os.path.join(WORK, "seek.jpg")
        url = "rtsp://127.0.0.1:%d/s3vod/%s" % (RTSP_PORT, key)
        sh("ffmpeg -v error -ss 5 -i '%s' -frames:v 1 -y %s" % (url, shot), 90)
        check("seek 到 5s 取帧成功", os.path.exists(shot) and os.path.getsize(shot) > 3 * 1024,
              "%d 字节" % (os.path.getsize(shot) if os.path.exists(shot) else 0))

        # ---------- 4b) webrtc 协商 ----------
        # 没有浏览器也能验一半：服务端必须能为 s3vod 源生成 SDP answer。
        # （真实 ICE/DTLS 联通仍需浏览器，这里只证明「webrtc 播放能打开这个源」）
        offer = (
            "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
            "a=group:BUNDLE 0\r\na=msid-semantic: WMS\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\n"
            "a=rtcp:9 IN IP4 0.0.0.0\r\na=ice-ufrag:abcd\r\n"
            "a=ice-pwd:abcdefghijklmnopqrstuvwx\r\na=ice-options:trickle\r\n"
            "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
            "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
            "a=setup:actpass\r\na=mid:0\r\na=sendonly\r\na=rtcp-mux\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n")
        api = ("http://127.0.0.1:%d/index/api/webrtc?app=s3vod&stream=%s&type=play"
               % (HTTP_PORT, key))
        try:
            req = urllib.request.Request(api, data=offer.encode(), method="POST")
            with urllib.request.urlopen(req, timeout=20) as r:
                ret = json.loads(r.read().decode())
        except Exception as e:
            ret = {"code": -1, "msg": str(e)[:80]}
        if "404" in (ret.get("msg") or ""):
            # 本机这份构建没注册 webrtc api（是否启用取决于编译选项与 rtc 配置），
            # 这不是云点播的问题，直接标 SKIP 而不是假装通过/失败。
            skip("webrtc 播放协商", "该构建未注册 /index/api/webrtc，需在启用 webrtc 的构建/浏览器上验")
        else:
            ok = ret.get("code") == 0 and "\r\nm=" in (ret.get("sdp") or "")
            check("webrtc 播放协商返回 SDP answer", ok,
                  "code=%s %s" % (ret.get("code"), (ret.get("msg") or "")[:50]))

        # ---------- 5) 不存在 ----------
        url = "rtsp://127.0.0.1:%d/s3vod/record/nope/missing.mp4" % RTSP_PORT
        t0 = time.time()
        p = sh("ffprobe -v error -show_entries format=duration -of json %s" % url, 30)
        cost = time.time() - t0
        # 服务端查到 404 是立刻的；播放器还要等 general.maxStreamWaitMS（默认 15s）才放弃，
        # 这与「播放不存在的直播流」完全一致，不是本功能引入的延迟。
        check("不存在的对象点播失败", p.returncode != 0 and cost < 25,
              "rc=%d 用时 %.1fs（含 maxStreamWaitMS 等待）" % (p.returncode, cost))

        # ---------- 6) 没落到本地磁盘 ----------
        local = sh("find %s -name 'vod.mp4' -o -name 'missing.mp4' 2>/dev/null" % WORK).stdout.strip()
        check("本地没有该 mp4 文件", not local, local[:80])

        print("\n服务日志里的云点播记录:")
        print(sh("grep 云点播 %s | tail -5" % os.path.join(WORK, "srv.log")).stdout.strip())
    finally:
        srv.terminate()
        mock.terminate()
        time.sleep(1)
        sh('pkill -f "s3_mock_server.py --port %d"' % mock_port)

    failed = [x for x in results if x[1] is False]
    passed = [x for x in results if x[1] is True]
    skipped = [x for x in results if x[1] is None]
    print("\n结果: %d/%d 通过%s" % (len(passed), len(results) - len(skipped),
                                 "（跳过 %d）" % len(skipped) if skipped else ""))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
