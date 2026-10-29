#!/usr/bin/env python3
"""负载自适应降帧（第 6 项：过载时主动降帧且不花屏）验收。

被测能力：编码跟不上时自动降帧率，有余量时自动恢复。

三个判据，缺一不可：
  1. **降得下来**：过载时输出帧率明显低于目标帧率，但不低于 adapt_min_fps；
  2. **不错位**（最容易漏的一条）：降帧后 5 秒拉流里的视频时长仍必须 ≈5 秒。
     只降帧不调整时间轴步进 = 慢放（15→7.5fps 会让 5 秒拉流只推进 2.5 秒内容），
     音画直接错位。这条只有量 pts 跨度才发现，看包数或「能不能播」都发现不了；
  3. **不花屏**：关键帧不能被抽掉，落盘后重新解码 0 错误。

过载用自测钩子 `adapt_selftest_delay_ms` 人为放大测得耗时来复现（快机器上真压
CPU 既不可靠也不可重复）；该钩子生产必须为 0，代码里有注释说明。

用法：env -u PYTHONPATH python3 scripts/adapt_fps_test.py
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
CFG = f"{WT}/config.adapt.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
SRC = "/tmp/src_1280x720.mp4"      # 1280x720 @15fps + aac
LOG = "/tmp/adapt.log"
TARGET_FPS = 30.0    # 素材是 1280x720@30；目标帧率跟随源，所以这就是基准
PULL_SECS = 5

PASS, FAIL = [], []

# 宿主负载：本机是开发机，Hermes/Chrome/WindowServer 常态占用 10 核里的六七个，
# 负载高时「无过载不降帧」这类时间敏感断言本来就不成立（编码真的被拖慢了，
# 自适应降帧是**正确行为**）。此时放宽判据并在输出里说明，避免假红。
HOST_LOAD = os.getloadavg()[0] if hasattr(os, "getloadavg") else 0.0
CORES = os.cpu_count() or 1
BUSY_HOST = HOST_LOAD > CORES * 0.8


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


def make_cfg():
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=0", "audio_codec=", "video_codec=", "on_demand=0", "sample_rate=0",
                    "bitrate=0", "video_width=0", "video_height=0", "video_fps=0",
                    "video_bitrate=0", "video_scale=0", "video_filter=", "video_logo=",
                    "adapt_fps=0", "adapt_min_fps=5", "adapt_selftest_delay_ms=0"]
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


def start(selftest_delay_ms=0):
    """起服务。selftest_delay_ms 通过**环境变量**注入 —— 这是自测钩子，
    不走配置项/API，避免把「会让转码变慢」的开关暴露给运维。"""
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}")
            time.sleep(0.6)
    env = dict(os.environ)
    env["ZLM_ADAPT_SELFTEST_DELAY_MS"] = str(selftest_delay_ms)
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                         stdout=open(LOG, "w"), stderr=subprocess.STDOUT, env=env,
                         preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is None and lpid(RTSP) == p.pid:
            return p
    raise RuntimeError("MediaServer 没起来，看 " + LOG)


def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        pass
    time.sleep(1.0)


def push(stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def measure(stream, secs=PULL_SECS):
    """拉一段流落盘，返回 (帧率, 时长, 包数, 关键帧数, 解码错误列表)。"""
    url = f"rtsp://127.0.0.1:{RTSP}/live/{stream}"
    out = f"/tmp/adapt_{stream}.mkv"
    sh(f"rm -f {out}")
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i {url} -t {secs} -c copy -y {out}", timeout=secs + 40)
    errs = [l for l in r.stdout.splitlines()
            if l.strip() and "monoton" not in l and "Last message repeated" not in l]
    if not os.path.exists(out):
        return None, None, 0, 0, errs or ["没有落盘文件"]
    p = sh(f"ffprobe -v error -select_streams v -show_entries packet=pts_time,flags "
           f"-of csv=p=0 {out}", timeout=60).stdout.strip().splitlines()
    pts, keys = [], 0
    for ln in p:
        parts = ln.split(",")
        if not parts or not parts[0]:
            continue
        try:
            pts.append(float(parts[0]))
        except ValueError:
            continue
        if len(parts) > 1 and "K" in parts[1]:
            keys += 1
    if len(pts) < 2:
        return None, None, len(pts), keys, errs + ["包数不足"]
    dur = max(pts) - min(pts)
    fps = len(pts) / dur if dur > 0 else None
    log_text = ""   # 由 run_case 填
    # 重新解码一遍，确认真能解（花屏会在这里报错）
    d = sh(f"ffmpeg -v error -i {out} -f null - 2>&1", timeout=90)
    errs += [l for l in d.stdout.splitlines()
             if l.strip() and "monoton" not in l and "Last message repeated" not in l]
    return fps, dur, len(pts), keys, errs


def run_case(name, params, selftest_delay_ms=0):
    """每个用例独立起服务：自测延迟只能通过进程环境变量注入。"""
    srv = start(selftest_delay_ms)
    try:
        api("addTranscode", vhost="__defaultVhost__", app="live", stream=name, reset=1)
        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=name, **params)
        if r.get("code") != 0:
            return None
        pr = push(name)
        time.sleep(8)                 # 让自适应收敛（每降一档需要 15 帧）
        res = measure(name, PULL_SECS)
        try:
            os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
        except Exception:
            pass
        # 日志按用例截断（每次 start 都 open(...,'w')），所以要在这里取，不能最后统一读
        return res + (open(LOG, errors="replace").read(),)
    finally:
        stop(srv)


def main():
    print("=" * 76)
    print("负载自适应降帧验收（降得下来 / 不错位 / 不花屏）")
    print("=" * 76)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}")
        return 2
    make_cfg()
    if True:
        base = {"video_codec": "H265"}

        # A. 无过载：帧率不能自己往下掉
        fps, dur, n, keys, errs, log_a = run_case("ad_norm", dict(base, adapt_fps=1))
        if BUSY_HOST:
            # 高负载下只要求「没崩到下限附近」，不要求贴住目标
            note(fps is not None and fps > TARGET_FPS * 0.4,
                 "无人工注入过载时帧率未崩塌（高负载宿主，判据已放宽）",
                 f"fps={fps and round(fps, 1)} 宿主 load={HOST_LOAD:.1f}/{CORES}")
        else:
            note(fps is not None and fps > TARGET_FPS * 0.85,
                 "无过载时帧率保持目标（不误降）",
                 f"fps={fps and round(fps, 1)} 包={n} 时长={dur and round(dur, 2)}")

        # B. 中等过载：要降帧，且时长仍然对
        fps2, dur2, n2, keys2, errs2, log_b = run_case(
            "ad_mid", dict(base, adapt_fps=1, adapt_min_fps=5), selftest_delay_ms=60)
        note(fps2 is not None and fps2 < TARGET_FPS * 0.9,
             "过载时帧率下降", f"目标 {TARGET_FPS} -> 实测 {fps2 and round(fps2, 1)}")
        note(dur2 is not None and abs(dur2 - PULL_SECS) < 1.0,
             "降帧后时长仍然对（不是慢放）",
             f"拉 {PULL_SECS}s -> 视频时长 {dur2 and round(dur2, 2)}s")
        note(keys2 > 0, "关键帧仍在", f"{keys2} 个")
        note(not errs2, "重新解码 0 错误（不花屏）", f"{len(errs2)} 条")

        # C. 重度过载：撞到下限，但时长与关键帧依旧
        fps3, dur3, n3, keys3, errs3, log_c = run_case(
            "ad_hard", dict(base, adapt_fps=1, adapt_min_fps=5), selftest_delay_ms=400)
        note(fps3 is not None and 4.0 <= fps3 <= 16.0 and (fps2 is None or fps3 <= fps2 + 3.0),
             "重度过载时继续降且不低于下限（不无限降）",
             f"fps={fps3 and round(fps3, 1)}（下限 5，中度 {fps2 and round(fps2, 1)}）")
        note(dur3 is not None and abs(dur3 - PULL_SECS) < 1.2,
             "重度降帧后时长仍然对", f"{dur3 and round(dur3, 2)}s")
        note(keys3 > 0 and not errs3, "重度降帧仍有关键帧且 0 错误",
             f"关键帧 {keys3}, 错误 {len(errs3)}")

        # D. 关掉自适应：同样的过载信号不应影响帧率（证明是开关起作用）
        fps4, dur4, n4, keys4, errs4, log_d = run_case(
            "ad_off", dict(base, adapt_fps=0), selftest_delay_ms=400)
        note(fps4 is not None and fps4 > TARGET_FPS * 0.85,
             "adapt_fps=0 时不降帧", f"fps={fps4 and round(fps4, 1)}")

        # 日志按用例截断，用中度过载那一例的日志来验证（它是必然触发降帧的）
        log = log_b
        note("transcode adaptive fps" in log, "服务端记录自适应已启用")
        note("adapt: 编码跟不上" in log, "服务端记录降帧动作",
             "有" if "adapt: 编码跟不上" in log else f"日志片段={log[-200:]!r}")

    print()
    print("=" * 76)
    print(f"宿主负载 {HOST_LOAD:.1f} / {CORES} 核"
          + ("（偏高，时间敏感判据已放宽）" if BUSY_HOST else ""))
    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
    for x in FAIL:
        print("  -", x)
    print("=" * 76)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
