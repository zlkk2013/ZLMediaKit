#!/usr/bin/env python3
"""盒子上跑：HLS 录制直写云存储（真实 SeaweedFS S3）→ 用 S3 协议回读校验

与 MP4 版同一套判据，只是产物是多文件：
  1. startRecord(type=2) 返回 code=0
  2. --list 里能找到本次流的 .m3u8 与 .ts
  3. 取回 .m3u8 后，其中引用的切片在桶里都存在（播放列表自洽）
  4. 取回首个切片，ffprobe 认得出视频轨
  5. 本地 hls 目录文件数 = 0（全程不落盘）
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
CFG = "/root/zlmtest/cfg_s3hls.ini"
SRC_MP4 = "/tmp/src720.mp4"
S3TEST = "/root/s3t/s3test"
HTTP, RTSP = 57981, 57555
S3_ENDPOINT = "http://127.0.0.1:19020"
BUCKET, AK, SK = "zlmtest", "zlmediakit", "zlmsecret"
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = ("/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/lib64:"
                          "/usr/local/Ascend/driver/lib64/driver:" + ENV.get("LD_LIBRARY_PATH", ""))


def sh(c, t=120):
    return subprocess.run(c, shell=True, capture_output=True, text=True, timeout=t)


def get(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())


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
    p = sh("%s --endpoint %s --bucket %s --ak %s --sk %s --list %s" % (
        S3TEST, S3_ENDPOINT, BUCKET, AK, SK, prefix), 60)
    rows = []
    for ln in p.stdout.splitlines():
        parts = ln.split("\t")
        if len(parts) >= 2 and parts[0].startswith(prefix):
            try:
                rows.append((parts[0], int(parts[1]), parts[2] if len(parts) > 2 else ""))
            except ValueError:
                pass
    return rows, p


def s3get(key, out):
    return sh("%s --endpoint %s --bucket %s --ak %s --sk %s --get %s --out %s"
              % (S3TEST, S3_ENDPOINT, BUCKET, AK, SK, key, out), 120)


cfg = open(SRC_CFG, encoding="utf-8", errors="replace").read()
# HLS 实例换端口，避免和 MP4 实例撞（同一台盒子上可能并存）
for old, new in (("57980", str(HTTP)), ("57554", str(RTSP))):
    cfg = cfg.replace(old, new)
for k, v in [("api.apiDebug", "1"), ("s3.enable", "1"),
             ("s3.endpoint", S3_ENDPOINT), ("s3.bucket", BUCKET),
             ("s3.access_key", AK), ("s3.secret_key", SK),
             ("s3.record_hls", "1"), ("s3.root_prefix", "record"),
             ("hls.segDur", "2"), ("hls.segNum", "10")]:
    cfg = setkey(cfg, k, v)
open(CFG, "w").write(cfg)

hls_local = read_section(cfg, "hls").get("filePath", "www")
if not hls_local.startswith("/"):
    hls_local = os.path.join(REPO, hls_local)

sh('for p in $(pgrep -f "[c]fg_s3hls.ini"); do kill -9 $p; done')
time.sleep(1)
log = open("/root/zlmtest/s3_hls_box.log", "w")
srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=REPO, env=ENV, stdout=log,
                       stderr=subprocess.STDOUT)
push = None
rc = 0
before = set()
for root, _, files in os.walk(hls_local):
    for f in files:
        before.add(os.path.join(root, f))
try:
    secret = read_section(cfg, "api").get("secret", "")
    ready = False
    for _ in range(40):
        try:
            if get("http://127.0.0.1:%d/index/api/getApiList?secret=%s" % (HTTP, secret), 2).get("code") == 0:
                ready = True
                break
        except Exception:
            pass
        time.sleep(0.5)
    if not ready:
        print("✗ MediaServer 没起来，日志尾部:")
        print(sh("tail -20 /root/zlmtest/s3_hls_box.log").stdout)
        sys.exit(1)

    st = "boxhls%d" % int(time.time())
    push = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC_MP4,
                             "-c", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
                             "rtsp://127.0.0.1:%d/live/%s" % (RTSP, st)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(6)
    r = get("http://127.0.0.1:%d/index/api/startRecord?secret=%s&type=2&vhost=__defaultVhost__"
            "&app=live&stream=%s" % (HTTP, secret, st))
    print("startRecord(HLS): code=%s" % r.get("code"))
    if r.get("code") != 0:
        rc = 1
    time.sleep(14)
    get("http://127.0.0.1:%d/index/api/stopRecord?secret=%s&type=2&vhost=__defaultVhost__"
        "&app=live&stream=%s" % (HTTP, secret, st))
    time.sleep(4)

    rows, p = s3list("record/")
    mine = [x for x in rows if st in x[0]]
    m3u8 = [x for x in mine if x[0].endswith(".m3u8")]
    ts = [x for x in mine if x[0].endswith(".ts")]
    print("--list record/ → 共 %d 个对象，本次流 %d 个（m3u8 %d，ts %d）"
          % (len(rows), len(mine), len(m3u8), len(ts)))
    for k, size, _ in mine[:6]:
        print("   %s  %d 字节" % (k, size))
    if not m3u8:
        print("✗ 一个 m3u8 都没有；--list stderr: %s" % p.stderr.strip()[:200])
        rc = 1
    else:
        keys = {x[0] for x in rows}

        def verify_playlist(key):
            """取回一份播放列表 + 它引用的所有段，按原相对路径还原后整体 ffprobe。
            fMP4 的 moov 在 init 段里，单验一个切片没意义，必须整体验。"""
            nonlocal_rc = 0
            out = "/tmp/box_hls.m3u8"
            gp = s3get(key, out)
            if gp.returncode != 0 or not os.path.exists(out):
                print("  ✗ 取回 m3u8 失败: %s" % gp.stderr.strip()[:160])
                return 1
            text = open(out, encoding="utf-8", errors="replace").read()
            refs = [l.strip() for l in text.splitlines()
                    if l.strip() and not l.strip().startswith("#")]
            base = key.rsplit("/", 1)[0]
            missing = [r for r in refs if (base + "/" + r) not in keys]
            print("  播放列表 %s → 引用 %d 段，桶内缺失 %d"
                  % (key.rsplit("/", 1)[-1], len(refs), len(missing)))
            if missing:
                print("     ✗ 缺失:", missing[:5])
                nonlocal_rc = 1
            if not refs:
                print("     ✗ 播放列表里没有切片")
                return 1
            import shutil
            play = "/tmp/hlsplay_" + key.rsplit("/", 1)[-1].replace(".", "_")
            shutil.rmtree(play, ignore_errors=True)
            os.makedirs(play, exist_ok=True)
            maps = re.findall(r'EXT-X-MAP:URI="([^"]+)"', text)
            fetched, failed = 0, []
            for rel in sorted(set(refs + maps)):
                dst = os.path.join(play, rel)
                os.makedirs(os.path.dirname(dst) or play, exist_ok=True)
                if s3get(base + "/" + rel, dst).returncode == 0 and os.path.exists(dst):
                    fetched += 1
                else:
                    failed.append(rel)
            open(os.path.join(play, "index.m3u8"), "w").write(text)
            print("    取回 %d 个段/init（失败 %d）" % (fetched, len(failed)))
            if failed:
                print("     ✗ 取回失败:", failed[:5])
                nonlocal_rc = 1
            pr = sh("ffprobe -v error -show_entries stream=codec_name,width:format=duration "
                    "-of json %s" % os.path.join(play, "index.m3u8"), 120)
            info = json.loads(pr.stdout or "{}")
            streams = [(x.get("codec_name"), x.get("width")) for x in info.get("streams", [])]
            dur = float(info.get("format", {}).get("duration", 0) or 0)
            # TS 播放列表 ffprobe 常给 format.duration=0，用 EXTINF 之和兜底
            extinf = sum(float(x) for x in re.findall(r"#EXTINF:([\d.]+)", text))
            shown = dur if dur > 0.1 else extinf
            print("    ffprobe: 时长=%.1fs%s 流=%s"
                  % (shown, "" if dur > 0.1 else "（按 EXTINF 求和）", streams))
            if not any(s[0] in ("h264", "hevc") for s in streams):
                print("     ✗ 没有视频轨（stderr: %s）" % pr.stderr.strip()[:160])
                nonlocal_rc = 1
            # 滚动窗口下时长 = segNum*segDur，segDur=2 时 3 段约 6s，
            # 所以只要求「至少 2 段且总时长 >= 4s」
            if len(refs) < 2 or shown < 4:
                print("     ✗ 段数/时长不足（段 %d，时长 %.1fs）" % (len(refs), shown))
                nonlocal_rc = 1
            return nonlocal_rc

        for k, size, _ in sorted(m3u8):
            if verify_playlist(k):
                rc = 1

    after = set()
    for root, _, files in os.walk(hls_local):
        for f in files:
            after.add(os.path.join(root, f))
    new_files = after - before
    print("本地 hls 目录 %s 新增文件数: %d" % (hls_local, len(new_files)))
    if new_files:
        print("   ✗ 落盘了:", list(new_files)[:5])
        rc = 1
finally:
    if push:
        push.terminate()
    srv.terminate()
    time.sleep(2)
    sh('for p in $(pgrep -f "[c]fg_s3hls.ini"); do kill -9 $p; done')

print("结果:", "通过" if rc == 0 else "有失败")
sys.exit(rc)
