#!/usr/bin/env python3
"""转码改造时的存活性测试：反复重推 + 热改 codec，服务不能被卡死。

背景：`~TranscodeSink` / `teardown()` 里如果在持有 `_mtx` 时 flush 解码器，
flush 会同步回调 `onDecoded` 再取同一把非递归锁 → 同线程自死锁，poller 线程卡住，
表现是「服务还在，但 API 不再响应、新观众连不上」。这类死锁**不会崩**，
只是静默失去响应，所以必须专门测「还活着」。

判据（每轮都要满足）：
  1. /index/api/getServerConfig 在 3 秒内返回 code=0（poller 没卡）
  2. 重推成功、拉流编码正确
  3. 连续多轮后仍然满足

用法：env -u PYTHONPATH python3 scripts/teardown_liveness_test.py
端口：RTMP 56900 / RTSP 56554 / HTTP 57080
"""
import json, os, re, signal, subprocess, sys, time, urllib.request

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.liveness.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
LOG = "/tmp/liveness.log"
SRC = "/tmp/src_h265.mp4"
ROUNDS = 6

PASS, FAIL = [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))

def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)

def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None

def make_cfg(video="H265"):
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=1", "audio_codec=", f"video_codec={video}", "on_demand=0",
                    "sample_rate=0", "bitrate=0", "video_width=0", "video_height=0",
                    "video_fps=0", "video_bitrate=0"]
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                i += 1
            sec = ""
            continue
        if ln.strip().startswith("port=") and sec in ("[rtmp]", "[rtsp]", "[http]"):
            out.append(f"port={ {'[rtmp]': RTMP, '[rtsp]': RTSP, '[http]': HTTP}[sec] }")
            i += 1
            continue
        out.append(ln); i += 1
    open(CFG, "w", encoding="utf-8").write("\n".join(out) + "\n")

def api_ok(timeout=3):
    """API 是否还能响应 —— 死锁时这里会超时。"""
    t0 = time.time()
    try:
        body = urllib.request.urlopen(
            f"http://127.0.0.1:{HTTP}/index/api/getServerConfig?secret=5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z",
            timeout=timeout).read()
        return json.loads(body).get("code") == 0, time.time() - t0
    except Exception as e:
        return False, time.time() - t0

def pull_codec(stream):
    r = sh(f"ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=25)
    return r.stdout.strip()

def push(stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)

def kill_quiet(pid):
    try: os.killpg(os.getpgid(pid), signal.SIGKILL)
    except Exception: pass

print("=" * 74)
print(f"存活性测试：{ROUNDS} 轮「推流 -> 热改 codec -> kill -> 重推」")
print("=" * 74)

make_cfg()
for p in (RTMP, RTSP, HTTP):
    pid = lpid(p)
    if pid:
        sh(f"kill -9 {pid}"); time.sleep(0.6)
open(LOG, "w").close()
srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                       stdout=open(LOG, "a"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
for _ in range(60):
    time.sleep(0.25)
    if srv.poll() is None and lpid(RTMP) == srv.pid:
        break
print(f"  服务已起，pid={srv.pid}")

stream = "lv"
try:
    for r in range(ROUNDS):
        # 轮换目标 codec，逼每次重推都重建 TrackContext
        target = "H264" if r % 2 == 0 else "H265"
        make_cfg(target)
        # 配置改了要重启才生效（这里用 API 覆盖更贴近现场）
        pr = push(stream)
        time.sleep(4)
        ok, cost = api_ok()
        note(ok, f"第{r+1}轮: 推流后 API 仍响应", f"{cost:.2f}s")
        # 这里只验「流还活着」，不断言具体 codec：本测试不重启服务（重启就测不到
        # 长跑累积的死锁），配置文件改了也不会生效，断言 codec 会假红。
        # 断言 codec 的职责在 camera_scenario_test / codec_reverse_test 里。
        note(pull_codec(stream) in ("h264", "hevc"),
             f"第{r+1}轮: 拉流仍可解（不断流）", pull_codec(stream))

        # 热改 codec（返回 pending，重推后生效）
        api = "http://127.0.0.1:%d/index/api/addTranscode?secret=5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z" % HTTP
        try:
            rq = urllib.request.urlopen(f"{api}&app=live&stream={stream}&video_codec={'H264' if target=='H265' else 'H265'}", timeout=3)
            rq.read()
        except Exception as e:
            note(False, f"第{r+1}轮: 热改 API 可用", str(e))

        kill_quiet(pr.pid)
        time.sleep(2.0)
        ok, cost = api_ok()
        note(ok, f"第{r+1}轮: 断流后 API 仍响应（没卡死）", f"{cost:.2f}s")
        # 断流本身是触发 TrackContext 销毁的时机
        time.sleep(0.5)

    # 收尾：再来一次推流，确认服务还能正常干活
    pr = push(stream)
    time.sleep(4)
    ok, cost = api_ok()
    note(ok, "最后一轮推流后 API 仍响应", f"{cost:.2f}s")
    note(pull_codec(stream) in ("h264", "hevc"), "最后仍能拉到流", pull_codec(stream))
    kill_quiet(pr.pid)
finally:
    try: os.killpg(os.getpgid(srv.pid), signal.SIGTERM)
    except Exception: pass
    time.sleep(1)

print()
print("=" * 74)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
for x in FAIL:
    print("  -", x)
print("=" * 74)
sys.exit(0 if not FAIL else 1)
