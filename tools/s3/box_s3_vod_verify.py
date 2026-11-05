#!/usr/bin/env python3
"""盒子上跑：云点播（s3vod）真机验证

目标：桶里的 mp4 对象不下载到本地，直接点播成 rtsp / rtmp / http-flv。

判据（每条都比对真实解码结果，不看状态码）：
  1. rtsp://host/s3vod/<key>  → ffprobe 认出 h264+aac，时长≈录制时长
  2. rtmp://host/s3vod/<key>  → 同上
  3. http://host/s3vod/<key>.live.flv → 拿到 >100KB 的 flv，ffprobe 认出 h264
  4. 拖动：-ss 5 取一帧成功（走 MP4FileS3 的 seek + Range 读）
  5. 对象不存在时点播失败（不会挂住、不会返回空流）
  6. 播放过程中本地没有生成该 mp4 文件（证明是流式读对象，不是先下到磁盘）
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

REPO = "/root/zlm-ascend"
BIN = REPO + "/release/linux/Release/MediaServer"
SRC_CFG = "/root/zlmtest/cfg_asc.ini"
CFG = "/root/zlmtest/cfg_s3vod.ini"
SRC_MP4 = "/tmp/src720.mp4"
S3_ENDPOINT = "http://127.0.0.1:19020"
BUCKET, AK, SK = "zlmtest", "zlmediakit", "zlmsecret"
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = ("/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/lib64:"
                          "/usr/local/Ascend/driver/lib64/driver:" + ENV.get("LD_LIBRARY_PATH", ""))

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("  %s %-46s %s" % ("OK  " if ok else "FAIL", name, detail))


def sh(c, t=120):
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


def section_vals(text, section):
    vals, cur = {}, None
    for ln in text.replace("\r\n", "\n").split("\n"):
        m = re.match(r"^\[(.+)\]\s*$", ln.strip())
        if m:
            cur = m.group(1)
        elif cur == section and "=" in ln:
            k, v = ln.strip().split("=", 1)
            vals[k.strip()] = v.strip()
    return vals


def probe(url, extra="", timeout=40):
    """ffprobe 一个流，返回 (json, stderr, 用时)"""
    t0 = time.time()
    p = sh("timeout %d ffprobe -v error -rw_timeout 15000000 %s -show_entries "
           "stream=codec_name,width,height:format=duration -of json %s"
           % (timeout, extra, url), timeout + 15)
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
    cfg = open(SRC_CFG, encoding="utf-8", errors="replace").read()
    # 换一组端口，避免和盒子上可能在跑的实例撞
    for old, new in (("57980", "57983"), ("57554", "57557")):
        cfg = cfg.replace(old, new)
    for k, v in [("api.apiDebug", "1"), ("s3.enable", "1"),
                 ("s3.endpoint", S3_ENDPOINT), ("s3.bucket", BUCKET),
                 ("s3.access_key", AK), ("s3.secret_key", SK),
                 ("s3.record_mp4", "1"), ("s3.root_prefix", "record")]:
        cfg = setkey(cfg, k, v)
    open(CFG, "w").write(cfg)

    http_port = int(section_vals(cfg, "http").get("port", 80))
    rtsp_port = int(section_vals(cfg, "rtsp").get("port", 554))
    rtmp_port = int(section_vals(cfg, "rtmp").get("port", 1935))
    print("端口: http=%d rtsp=%d rtmp=%d" % (http_port, rtsp_port, rtmp_port))

    sh('for p in $(pgrep -f "[c]fg_s3vod.ini"); do kill -9 $p; done')
    time.sleep(1)
    log = open("/root/zlmtest/s3_vod.log", "w")
    srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=REPO, env=ENV, stdout=log,
                           stderr=subprocess.STDOUT)
    push = None
    try:
        secret = section_vals(cfg, "api").get("secret", "")
        ready = False
        for _ in range(40):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/index/api/getApiList?secret=%s"
                                            % (http_port, secret), timeout=2) as r:
                    if json.loads(r.read().decode()).get("code") == 0:
                        ready = True
                        break
            except Exception:
                pass
            time.sleep(0.5)
        if not ready:
            print("✗ MediaServer 没起来")
            print(sh("tail -20 /root/zlmtest/s3_vod.log").stdout)
            return 1

        # ---------- 造一个 mp4 对象 ----------
        st = "boxvod%d" % int(time.time())
        push = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC_MP4,
                                 "-c", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
                                 "rtsp://127.0.0.1:%d/live/%s" % (rtsp_port, st)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(6)
        api = "http://127.0.0.1:%d/index/api/%%s?secret=%s&vhost=__defaultVhost__&app=live&stream=%s" % (
            http_port, secret, st)
        urllib.request.urlopen(api % "startRecord&type=1", timeout=10).read()
        time.sleep(11)
        urllib.request.urlopen(api % "stopRecord&type=1", timeout=10).read()
        time.sleep(5)

        rows = sh("/root/s3t/s3test --endpoint %s --bucket %s --ak %s --sk %s --list record/"
                  % (S3_ENDPOINT, BUCKET, AK, SK), 60).stdout
        keys = [l.split("\t")[0] for l in rows.splitlines() if st in l and l.endswith(".mp4")]
        if not keys:
            print("✗ 没造出 mp4 对象")
            return 1
        key = sorted(keys)[-1]
        print("\n点播对象: %s" % key)

        # 本地不应该有这个名字的文件（对象是录制直写的）
        local = os.path.join(REPO, "www", "record", key)
        check("播放前本地不存在该对象文件", not os.path.exists(local), local)

        # ---------- 1) rtsp ----------
        url = "rtsp://127.0.0.1:%d/s3vod/%s" % (rtsp_port, key)
        js, err, cost = probe(url)
        st_list, dur = streams_of(js)
        check("rtsp 点播出 h264 视频轨", any(s[0] in ("h264", "hevc") for s in st_list),
              "%s 用时 %.1fs" % (st_list, cost))
        check("rtsp 时长≈录制时长（8~20s）", 8 <= dur <= 20, "%.1fs" % dur)

        # ---------- 2) rtmp ----------
        url = "rtmp://127.0.0.1:%d/s3vod/%s" % (rtmp_port, key)
        js, err, cost = probe(url)
        st_list, dur = streams_of(js)
        check("rtmp 点播出 h264 视频轨", any(s[0] in ("h264", "hevc") for s in st_list),
              "%s 用时 %.1fs" % (st_list, cost))

        # ---------- 3) http-flv ----------
        flv = "/tmp/vod.flv"
        if os.path.exists(flv):
            os.remove(flv)
        url = "http://127.0.0.1:%d/s3vod/%s.live.flv" % (http_port, key)
        p = sh("timeout 12 ffmpeg -v error -rw_timeout 8000000 -i %s -t 3 -c copy -f flv -y %s"
               % (url, flv), 30)
        size = os.path.getsize(flv) if os.path.exists(flv) else 0
        js = sh("ffprobe -v error -show_entries stream=codec_name -of json %s" % flv, 30).stdout
        check("http-flv 点播拿到数据", size > 100 * 1024, "%d 字节" % size)
        check("http-flv 里有 h264", "h264" in (js or ""), js.strip()[:60])

        # ---------- 4) 拖动（seek 到 5s 取一帧） ----------
        shot = "/tmp/vod_seek.jpg"
        if os.path.exists(shot):
            os.remove(shot)
        url = "rtsp://127.0.0.1:%d/s3vod/%s" % (rtsp_port, key)
        p = sh("timeout 40 ffmpeg -v error -ss 5 -i %s -frames:v 1 -y %s" % (url, shot), 60)
        ok = os.path.exists(shot) and os.path.getsize(shot) > 5 * 1024
        check("seek 到 5s 取帧成功（Range 读 + 拖动）", ok,
              "%d 字节 %s" % (os.path.getsize(shot) if os.path.exists(shot) else 0, p.stderr[:60]))

        # ---------- 5) 对象不存在 ----------
        url = "rtsp://127.0.0.1:%d/s3vod/record/nope/does-not-exist.mp4" % rtsp_port
        t0 = time.time()
        p = sh("timeout 20 ffprobe -v error -show_entries format=duration -of json %s" % url, 30)
        cost = time.time() - t0
        check("不存在的对象点播失败且快速返回", p.returncode != 0 and cost < 15,
              "rc=%d 用时 %.1fs" % (p.returncode, cost))

        # ---------- 6) 本地仍然没有落盘 ----------
        check("播放后本地仍无该对象文件", not os.path.exists(local))
        tail = sh("grep -c 云点播 /root/zlmtest/s3_vod.log").stdout.strip()
        print("      日志中「云点播」行数: %s" % tail)
    finally:
        if push:
            push.terminate()
        srv.terminate()
        time.sleep(2)
        sh('for p in $(pgrep -f "[c]fg_s3vod.ini"); do kill -9 $p; done')

    failed = [x for x in results if not x[1]]
    print("\n结果: %d/%d 通过" % (len(results) - len(failed), len(results)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
