#!/usr/bin/env python3
"""API 参数面覆盖验收：每个参数必须「真的被解析」，不能只回显。

背景：本项目踩过「配置能设、API 只回显不解析」的坑 —— 调用方传了参数、接口回
`code=0`、行为一点没变。所以这里逐个参数做三件事：

1. 传合法值 → 响应回显必须是传入的值（回显取自解析后的 option，能证明解析生效）
2. 传非法值 → 必须报 -300，不能静默回落到默认
3. 至少一个参数做**行为级**验证（fps 用拉流端实测帧率），证明不是只改了数字

用法：
  env -u PYTHONPATH python3 scripts/api_params_test.py
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
CFG = f"{WT}/config.apiparams.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
LOG = "/tmp/apiparams.log"
SRC = "/tmp/src_1280x720.mp4"
STREAM = "api_p1"

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
    kw.setdefault("vhost", "__defaultVhost__")
    kw.setdefault("app", "live")
    kw.setdefault("stream", STREAM)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    with urllib.request.urlopen(url, timeout=6) as r:
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
                    "hw_encode=0", "hw_decode=0", "adapt_fps=0"]
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
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                         stdout=open(LOG, "w"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is None and lpid(RTSP) == p.pid:
            return p
    raise RuntimeError("MediaServer 没起来，看 " + LOG)


def echo(case, **kw):
    """下发一次并返回完整响应；先 reset 掉旧覆盖，避免用例之间互相污染"""
    api("addTranscode", reset=1)
    return api("addTranscode", **case, **kw)


def main():
    print("=" * 76)
    print("API 参数面：合法值解析 / 非法值拒绝 / 行为级验证")
    print("=" * 76)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}")
        return 2
    make_cfg()
    srv = start()
    pusher = None
    try:
        # ---------- 1. 合法值必须被解析（不是只回显） ----------
        base = dict(video_codec="H265")
        cases = [
            ({"video_fps": "15"}, "video_fps", 15.0, "video_fps=15"),
            ({"fps": "20"}, "video_fps", 20.0, "fps=20（短名别名）"),
            ({"video_bitrate": "800000"}, "video_bitrate", 800000, "video_bitrate=800000"),
            ({"logo_x": "-20", "logo_y": "-30"}, "video_logo_x", -20, "logo_x=-20（负值靠右下）"),
            ({"logo_x": "-20", "logo_y": "-30"}, "video_logo_y", -30, "logo_y=-30"),
            ({"enable_silence": "0"}, "enable_silence", False, "enable_silence=0"),
            ({"silence_timeout_ms": "500"}, "silence_timeout_ms", 500, "silence_timeout_ms=500"),
            ({"scale": "0.5"}, "video_scale", 0.5, "scale=0.5"),
        ]
        for kw, key, want, label in cases:
            r = echo(base, **kw)
            got = r.get(key)
            ok = r.get("code") == 0 and got is not None and abs(float(got) - float(want)) < 1e-6
            note(ok, f"解析 {label}", f"{key}={got}")

        # ---------- 2. 非法值必须拒绝 ----------
        bad = [
            ({"video_fps": "abc"}, "video_fps=abc"),
            ({"video_fps": "200"}, "video_fps=200（>120）"),
            ({"video_bitrate": "-1"}, "video_bitrate=-1"),
            ({"logo_x": "abc"}, "logo_x=abc"),
            ({"silence_timeout_ms": "10"}, "silence_timeout_ms=10（<20）"),
            ({"scale": "5"}, "scale=5（>4）"),
            ({"width": "800"}, "只给 width"),
        ]
        for kw, label in bad:
            r = echo(base, **kw)
            note(r.get("code") == -300, f"拒绝 {label}", f"code={r.get('code')}")

        # ---------- 3. 行为级：fps=15 必须真的降帧（拉流端实测） ----------
        api("addTranscode", reset=1)
        r = api("addTranscode", video_codec="H265", video_fps="15")
        note(r.get("code") == 0, "下发 video_fps=15", f"code={r.get('code')}")

        pusher = subprocess.Popen(
            ["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC, "-c", "copy",
             "-f", "rtsp", "-rtsp_transport", "tcp", f"rtsp://127.0.0.1:{RTSP}/live/{STREAM}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        time.sleep(4)

        # 等视频轨真的开始转码，再拉流数帧
        started = False
        for _ in range(60):
            time.sleep(0.5)
            try:
                body = api("getTranscode")
            except Exception:
                continue
            for t in body.get("tracks") or []:
                if t.get("type") == "video" and t.get("frames", 0) > 3:
                    started = True
                    break
            if started:
                break
        note(started, "视频轨开始转码")

        # 注意：**不要用 `ffprobe -count_packets` 拉直播流** —— 没有 -t 限制它会一直读下去，
        # 直到被 120s 超时杀掉（实测踩过）。按包时间戳取窗口算帧率即可。
        # Do not run `ffprobe -count_packets` against a live stream without a limit: it never
        # terminates. Sample packet timestamps over a fixed window instead.
        # 取 6 秒窗口的视频包数算帧率（framecrc 是本项目验证过的线上取包口径）
        fc = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{STREAM} "
                f"-t 6 -map 0:v:0 -c copy -f framecrc -", timeout=90).stdout
        vpk = [l for l in fc.splitlines()
               if l and not l.startswith("#") and not l.startswith("stream") and "," in l]
        fps = len(vpk) / 6.0
        note(12 <= fps <= 18, "拉流端实测帧率 ≈15fps（行为级证据）",
             f"6 秒收到 {len(vpk)} 个视频包 = {fps:.1f}fps")
    finally:
        if pusher:
            try:
                os.killpg(os.getpgid(pusher.pid), signal.SIGKILL)
            except Exception:
                pass
        try:
            os.killpg(os.getpgid(srv.pid), signal.SIGKILL)
        except Exception:
            pass

    print()
    print("=" * 76)
    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
    for x in FAIL:
        print("  -", x)
    print("=" * 76)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
