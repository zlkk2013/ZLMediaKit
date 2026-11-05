#!/usr/bin/env python3
"""盒子上跑：MP4 录制直写云存储（真实 SeaweedFS S3）→ 用 S3 协议回读校验

与上一版的区别：**不再翻 SeaweedFS 的数据目录猜文件**（对象存在 volume 的 .dat 里，
文件系统上看不到 .mp4），改成用 s3test --list / --get 走 S3 协议列对象、取对象，
再用 ffprobe 验内容。这样验的是「客户端写进去的、S3 服务端认可的」对象。

判据（全部满足才算通过）：
  1. startRecord 返回 code=0
  2. --list record/ 至少 1 个 .mp4 对象，且大小 > 200KB
  3. --get 取回该对象后 ffprobe 认得出，时长 8~20s，含 h264/hevc 视频轨
  4. 本地录制目录文件数 = 0（全程不落盘）
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
CFG = "/root/zlmtest/cfg_s3.ini"
SRC_MP4 = "/tmp/src720.mp4"
S3TEST = "/root/s3t/s3test"
HTTP, RTSP = 57980, 57554
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


cfg_src = open(SRC_CFG, encoding="utf-8", errors="replace").read()
cfg = cfg_src
for k, v in [("api.apiDebug", "1"), ("s3.enable", "1"),
             ("s3.endpoint", S3_ENDPOINT), ("s3.bucket", BUCKET),
             ("s3.access_key", AK), ("s3.secret_key", SK),
             ("s3.record_mp4", "1"), ("s3.root_prefix", "record")]:
    cfg = setkey(cfg, k, v)
open(CFG, "w").write(cfg)

rec_local = read_section(cfg, "record").get("filePath", "www/record")
if not rec_local.startswith("/"):
    rec_local = os.path.join(REPO, rec_local)

sh('for p in $(pgrep -f "[c]fg_s3.ini"); do kill -9 $p; done')
time.sleep(1)
log = open("/root/zlmtest/s3_box.log", "w")
srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=REPO, env=ENV, stdout=log,
                       stderr=subprocess.STDOUT)
push = None
rc = 0
before = set(os.listdir(rec_local)) if os.path.isdir(rec_local) else set()
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
        print(sh("tail -20 /root/zlmtest/s3_box.log").stdout)
        sys.exit(1)

    st = "boxs3%d" % int(time.time())
    push = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC_MP4,
                             "-c", "copy", "-f", "rtsp", "-rtsp_transport", "tcp",
                             "rtsp://127.0.0.1:%d/live/%s" % (RTSP, st)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(6)
    r = get("http://127.0.0.1:%d/index/api/startRecord?secret=%s&type=1&vhost=__defaultVhost__"
            "&app=live&stream=%s" % (HTTP, secret, st))
    print("startRecord: code=%s" % r.get("code"))
    if r.get("code") != 0:
        rc = 1
    time.sleep(12)
    get("http://127.0.0.1:%d/index/api/stopRecord?secret=%s&type=1&vhost=__defaultVhost__"
        "&app=live&stream=%s" % (HTTP, secret, st))
    # 录制结束 → 关闭文件 → 后台上传，等它传完
    for i in range(20):
        rows, _ = s3list("record/")
        mp4 = [x for x in rows if x[0].endswith(".mp4") and st in x[0]]
        if mp4:
            break
        time.sleep(1)
    time.sleep(2)

    rows, p = s3list("record/")
    mp4s = [x for x in rows if x[0].endswith(".mp4") and st in x[0]]
    print("--list record/ → 共 %d 个对象，本次流 %d 个" % (len(rows), len(mp4s)))
    for k, size, _ in rows[:8]:
        print("   %s  %d 字节" % (k, size))
    if not mp4s:
        print("✗ 没有本次流（%s）的 mp4 对象；--list stderr: %s" % (st, p.stderr.strip()[:200]))
        rc = 1
    else:
        key, size, _ = sorted(mp4s, key=lambda x: -x[1])[0]
        if size < 200 * 1024:
            print("✗ 对象太小: %d 字节" % size)
            rc = 1
        out = "/tmp/box_rec.mp4"
        gp = sh("%s --endpoint %s --bucket %s --ak %s --sk %s --get %s --out %s"
                % (S3TEST, S3_ENDPOINT, BUCKET, AK, SK, key, out), 120)
        if gp.returncode != 0 or not os.path.exists(out):
            print("✗ 取回失败: %s" % gp.stderr.strip()[:200])
            rc = 1
        else:
            got = os.path.getsize(out)
            print("取回: %s → %d 字节（对象声明 %d）" % (key, got, size))
            if got != size:
                print("✗ 取回大小与列出的不一致")
                rc = 1
            pr = sh("ffprobe -v error -show_entries format=duration:stream=codec_name,width,height "
                    "-of json %s" % out, 60)
            info = json.loads(pr.stdout or "{}")
            dur = float(info.get("format", {}).get("duration", 0) or 0)
            streams = [(x.get("codec_name"), x.get("width")) for x in info.get("streams", [])]
            print("ffprobe: 时长=%.1fs 流=%s" % (dur, streams))
            if not (8 <= dur <= 20):
                print("✗ 时长异常（期望 8~20s）")
                rc = 1
            if not any(s[0] in ("h264", "hevc") for s in streams):
                print("✗ 没有视频轨")
                rc = 1

    after = set(os.listdir(rec_local)) if os.path.isdir(rec_local) else set()
    new_files = after - before
    print("本地录制目录 %s 新增文件数: %d" % (rec_local, len(new_files)))
    if new_files:
        print("   ✗ 落盘了:", list(new_files)[:5])
        rc = 1
finally:
    if push:
        push.terminate()
    srv.terminate()
    time.sleep(2)
    sh('for p in $(pgrep -f "[c]fg_s3.ini"); do kill -9 $p; done')

print("结果:", "通过" if rc == 0 else "有失败")
sys.exit(rc)
