#!/usr/bin/env python3
"""分辨率倍数（video_scale / API scale）验收。

被测能力：第 3 项「基于 http api 的动态转码，支持分辨率倍数」。

判据一律看**拉流端实际解出的宽高**，不看服务端自述（服务端日志里的 target_size
只是我们算出来的期望值，不能自证）。

用例：
  1. 1280x720 源 + scale=0.5    -> 640x360
  2. 1280x720 源 + scale=1.5    -> 1920x1080
  3. 1280x720 源 + scale=0.25   -> 320x180
  4. 显式 width/height 与 scale 同时给 -> 显式优先
  5. scale=0                    -> 跟随源（1280x720）
  6. 非法值（abc / 0.05 / 5）  -> 接口必须报错，不能静默当成「跟随源」
  7. 配置文件 video_scale=0.5   -> 同样生效（走 ini 而不是 API）

用法：env -u PYTHONPATH python3 scripts/scale_param_test.py
端口：RTMP 56900 / RTSP 56554 / HTTP 57080
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

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.scale.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
SRC = "/tmp/src_1280x720.mp4"
STREAM = "scale_t"

PASS, FAIL = [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.split()
    return int(out[0]) if out else None


def api(path, **kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    try:
        with urllib.request.urlopen(url, timeout=8) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"code": -1, "_err": str(e)}


def size(stream=STREAM):
    """拉流端解出的实际宽高；取不到返回 None。"""
    r = sh(f"ffprobe -v error -select_streams v -show_entries stream=width,height "
           f"-of csv=p=0 rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=30)
    out = r.stdout.strip()
    if "," in out:
        w, h = out.split(",")[:2]
        return (int(w), int(h))
    return None


def make_cfg(scale=None):
    """生成配置；scale 不为 None 时写进 [transcode] 段（走 ini 路径）。"""
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=1" if scale is not None else "enable=0",
                    "audio_codec=", "video_codec=H265" if scale is not None else "video_codec=",
                    "on_demand=0", "sample_rate=0", "bitrate=0",
                    "video_width=0", "video_height=0", "video_fps=0", "video_bitrate=0"]
            if scale is not None:
                out.append(f"video_scale={scale}")
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
    txt = "\n".join(out) + "\n"
    txt = re.sub(r"(?m)^on_demand=\d", "on_demand=0", txt)
    open(CFG, "w", encoding="utf-8").write(txt)


def start():
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}")
            time.sleep(0.6)
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                         stdout=open("/tmp/scale.log", "w"), stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is None and lpid(RTSP) == p.pid:
            return p
    raise RuntimeError("MediaServer 没起来，看 /tmp/scale.log")


def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        pass
    time.sleep(1.0)


def push(src=SRC, stream=STREAM):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def kill_push(pr):
    try:
        os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
    except Exception:
        pass


_CASE_NO = [0]


def case_api(label, params, want, src=SRC, stream=None):
    """API 下发参数 -> 推流 -> 看实际尺寸。

    每例用独立流名：kill 推流端后 15 秒内重推会**接管旧流水线**并沿用旧的转码参数
    （宽限期设计如此）。同一个流名连跑多例会全部沿用第一例的 scale，
    表现为「只有第一条生效」——实测踩过。
    """
    if stream is None:
        _CASE_NO[0] += 1
        stream = f"{STREAM}{_CASE_NO[0]}"
    api("addTranscode", vhost="__defaultVhost__", app="live", stream=stream, reset=1)
    r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=stream, **params)
    if r.get("code") != 0:
        note(False, label, f"接口失败 {r}")
        return
    pr = push(src, stream)
    time.sleep(6)
    got = size(stream)
    note(got == want, label, f"实得 {got}（期望 {want}）")
    kill_push(pr)
    time.sleep(1.5)


def main():
    print("=" * 74)
    print("分辨率倍数验收（判据：拉流端实际宽高）")
    print("=" * 74)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}（1280x720），先生成")
        sh("ffmpeg -y -v error -f lavfi -i testsrc2=size=1280x720:rate=15:duration=20 "
           "-f lavfi -i sine=frequency=440:duration=20 -c:v libx264 -preset ultrafast "
           f"-b:v 800k -c:a aac -b:a 64k {SRC}", timeout=180)

    # 前 5 项走 API（ini 关掉转码，只有 API 覆盖生效）
    make_cfg(None)
    srv = start()
    try:
        case_api("scale=0.5 -> 640x360", {"video_codec": "H265", "scale": 0.5}, (640, 360))
        case_api("scale=1.5 -> 1920x1080", {"video_codec": "H265", "scale": 1.5}, (1920, 1080))
        case_api("scale=0.25 -> 320x180", {"video_codec": "H265", "scale": 0.25}, (320, 180))
        case_api("scale=0 -> 跟随源 1280x720", {"video_codec": "H265", "scale": 0}, (1280, 720))

        # 非法值必须报错（静默当成「跟随源」是最难查的坑）
        for bad in ("abc", "0.05", "5"):
            r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=STREAM,
                    video_codec="H265", scale=bad)
            note(r.get("code") != 0, f"非法 scale={bad} 被拒绝", f"code={r.get('code')}")

        # 显式宽高优先于倍数
        st = f"{STREAM}x"
        api("addTranscode", vhost="__defaultVhost__", app="live", stream=st, reset=1)
        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=st,
                video_codec="H265", scale=0.5, width=800, height=600)
        if r.get("code") == 0:
            pr = push(SRC, st)
            time.sleep(6)
            got = size(st)
            note(got == (800, 600), "显式 width/height 优先于 scale", f"实得 {got}")
            kill_push(pr)
        else:
            note(False, "显式 width/height 优先于 scale", f"接口失败 {r}")

        # 宽高必须成对给、且必须偶数（YUV420）
        for kw, why in (({"width": 640}, "只给 width"), ({"width": 641, "height": 361}, "奇数宽高")):
            r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=f"{STREAM}y",
                    video_codec="H265", **kw)
            note(r.get("code") != 0, f"{why} 被拒绝", f"code={r.get('code')}")
    finally:
        stop(srv)

    # 第 7 项：ini 里的 video_scale
    make_cfg(0.5)
    srv = start()
    try:
        pr = push()
        time.sleep(6)
        got = size()
        note(got == (640, 360), "配置文件 video_scale=0.5 -> 640x360", f"实得 {got}")
        kill_push(pr)
    finally:
        stop(srv)

    print()
    print("=" * 74)
    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
    for x in FAIL:
        print("  -", x)
    print("=" * 74)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
