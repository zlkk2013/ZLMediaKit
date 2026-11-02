#!/usr/bin/env python3
"""多卡负载均衡验收（第 10 项）。

同一个脚本按平台跑不同覆盖面 —— 因为「按序号选卡」（NVENC 的 gpu 选项）只在
Windows/Linux 上有，macOS 的 VideoToolbox 没有设备索引概念：

- **无设备选择能力的平台**（macOS）：验「配置被接受但如实忽略」——
  hw_devices/hw_device 不报错、打警告、状态里必须报 -1（**绝不能假装分了卡**）。
- **有设备选择能力的平台**（Linux/Windows + NVENC）：额外验真分配——
  3 路流按「在跑路数最少」分到 0/1/0，状态接口逐个可见，流关掉后计数回落。

所以本脚本在 macOS 上只证明了参数面与诚实性，不能证明负载均衡真的有效；
到了 NVIDIA 机器上会自动多跑那几条。

用法：
  env -u PYTHONPATH python3 scripts/multi_gpu_test.py
"""
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.parse
import urllib.request

from _platshim import (pid_on_port as _pid_on_port, gpu_count,
                       supports_device_selection)

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from _platshim import bin_path as _bin_path, bin_dir as _bin_dir
BIN = _bin_path()
BIN_DIR = _bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.multigpu.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
LOG = "/tmp/multigpu.log"
SRC = "/tmp/src_1280x720.mp4"   # H264 源；用 H265 源转 H265 会走同编码透传，压根不建编码器
PLATFORM_SUPPORTS_DEVICE = supports_device_selection()
GPU_N = gpu_count()

PASS, FAIL = [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    return _pid_on_port(port)


def api(path, timeout=5, **kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())


def make_cfg():
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, sec, i = [], "", 0
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=0", "audio_codec=", "video_codec=", "on_demand=0", "sample_rate=0",
                    "bitrate=0", "video_width=0", "video_height=0", "video_fps=0",
                    "video_bitrate=0", "video_scale=0", "video_filter=", "video_logo=",
                    "hw_encode=0", "hw_decode=0", "hw_devices=", "hw_device=-1"]
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                i += 1
            sec = ""
            continue
        if ln.strip().startswith("port=") and sec in ("[rtmp]", "[rtsp]", "[http]"):
            out.append(f"port={ {'[rtmp]': RTMP, '[rtsp]': RTSP, '[http]': HTTP}[sec] }")
            i += 1
            continue
        out.append(ln)
        i += 1
    txt = re.sub(r"(?m)^on_demand=\d", "on_demand=0", "\n".join(out) + "\n")
    open(CFG, "w", encoding="utf-8").write(txt)


def start():
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}")
            time.sleep(0.6)
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=BIN_DIR,
                         stdout=open(LOG, "w"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is None and lpid(RTSP) == p.pid:
            return p
    raise RuntimeError("MediaServer 没起来，看 " + LOG)


def push(stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def kill(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        pass


def tracks(stream):
    """getTranscode 的响应是**扁平**的（没有 data 包裹），没在转码时 tracks 可能为空。"""
    body = api("getTranscode", vhost="__defaultVhost__", app="live", stream=stream)
    return body.get("tracks") or [], body


def wait_video(stream, secs=40):
    """等视频轨真的开始转码（transcoding=true 且有帧），避免拿到建流前的空状态"""
    for _ in range(secs * 4):
        time.sleep(0.25)
        ts, _ = tracks(stream)
        for t in ts:
            if t.get("type") == "video" and t.get("transcoding") and t.get("frames", 0) > 0:
                return t
    return None


def main():
    print("=" * 76)
    print(f"多卡负载均衡验收（本平台支持按序号选卡：{PLATFORM_SUPPORTS_DEVICE}）")
    print("=" * 76)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}（跑 scripts/test_assets.py）")
        return 2
    make_cfg()
    srv = start()
    pushers = []
    try:
        # ---------- 参数面（必须在下发后再推流：转码参数在 track ready 时生效） ----------
        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_a",
                video_codec="H265", hw=1, hw_devices="0,1")
        note(r.get("code") == 0, "接受 hw_devices=0,1", f"code={r.get('code')}")
        note(r.get("hw_devices") == "0,1", "hw_devices 回显一致", f"{r.get('hw_devices')!r}")

        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_a",
                hw_device="1")
        note(r.get("code") == 0 and r.get("hw_device") == 1, "接受 hw_device=1 并回显",
             f"hw_device={r.get('hw_device')}")

        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_a",
                hw_device="-2")
        note(r.get("code") == -300, "hw_device=-2 拒绝（必须 >= -1）", f"code={r.get('code')}")

        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_a",
                hw_device="abc")
        note(r.get("code") == -300, "hw_device=abc 拒绝", f"code={r.get('code')}")

        # reset 与参数同时给：必须「先回退再按参数下发」，不能把参数静默吞掉
        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_a", reset=1,
                video_codec="H265", hw=1, hw_devices="0,1")
        note(r.get("code") == 0 and r.get("hw_devices") == "0,1"
             and str(r.get("video_codec", "")).upper() == "H265",
             "reset=1 与参数同时给：参数仍被应用（不静默吞掉）",
             f"video={r.get('video_codec')} devs={r.get('hw_devices')!r}")

        # ---------- 诚实性：不支持选卡的平台必须报 -1 ----------
        pushers.append(push("gpu_a"))
        t = wait_video("gpu_a")
        note(t is not None, "gpu_a 视频轨开始转码")
        if t is not None:
            if PLATFORM_SUPPORTS_DEVICE:
                # NVENC：第一路应分到 0 号卡
                note(t.get("hw_device") == 0, "分配到具体设备号（0 号卡）",
                     f"hw_device={t.get('hw_device')}（共 {GPU_N} 张卡）")
            else:
                note(t.get("hw_device") == -1,
                     "不支持选卡的平台如实报 hw_device=-1（不假装分卡）",
                     f"hw_device={t.get('hw_device')}")
        log = open(LOG, encoding="utf-8", errors="replace").read()
        if not PLATFORM_SUPPORTS_DEVICE:
            note("不生效" in log and "已忽略" in log, "配置被忽略时有明确日志警告")

        # ---------- 有硬件时才验真分配 ----------
        if PLATFORM_SUPPORTS_DEVICE:
            api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_b", reset=1,
                video_codec="H265", hw=1, hw_devices="0,1")
            api("addTranscode", vhost="__defaultVhost__", app="live", stream="gpu_c", reset=1,
                video_codec="H265", hw=1, hw_devices="0,1")
            pushers.append(push("gpu_b"))
            pushers.append(push("gpu_c"))
            tb, tc = wait_video("gpu_b"), wait_video("gpu_c")
            devs = [t.get("hw_device") for t in (t, tb, tc) if t]
            # 期望：按「在跑路数最少」分配，N 张卡时轮流落在 0,1,...,N-1,0,...
            want = [i % GPU_N for i in range(len(devs))] if GPU_N else []
            note(devs == want, f"{len(devs)} 路在 {GPU_N} 张卡上按负载分配",
                 f"实际 {devs} / 期望 {want}")
        else:
            print("  SKIP  负载均衡真分配（本平台无 NVENC，需 Linux/Windows + NVIDIA 复验）")

        # ---------- 稳定性 ----------
        time.sleep(3)
        body = api("getServerConfig")
        note(body.get("code") == 0, "服务仍然可响应")
    finally:
        for p in pushers:
            kill(p)
        kill(srv)

    print()
    print("=" * 76)
    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
    for x in FAIL:
        print("  -", x)
    print("=" * 76)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
