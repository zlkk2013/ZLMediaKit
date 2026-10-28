#!/usr/bin/env python3
"""AV1 + fmp4 稳定性测试。

背景：a5 分支曾观察到「AV1 套目标 + 默认开 fmp4」时偶发堆损坏（6 次 1 次崩）。
后续合入 a4 的「多轨输出串行化锁」与 a5 自己的 PacketCache 合写锁之后，本脚本用于
确认该问题是否还在 —— 这类偶发堆损坏只能靠多轮重复跑，单次通过没有意义。

判据：每轮「起服务→AV1 转码推流→观众拉流（触发 fmp4/hls 写入）」后进程存活、
日志无 crash/signal。

用法：env -u PYTHONPATH python3 scripts/av1_fmp4_stability.py [轮数，默认 8]
端口：RTMP 56900 / RTSP 56554 / HTTP 57080
"""
import json, os, signal, subprocess, sys, time

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.av1stab.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 8


def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None


def make_cfg():
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=1", "audio_codec=", "video_codec=AV1", "on_demand=0",
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


make_cfg()
crashes = 0
print(f"AV1 + fmp4 稳定性：{ROUNDS} 轮")
for n in range(ROUNDS):
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}"); time.sleep(0.4)
    log = f"/tmp/av1stab_{n}.log"
    srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                           stdout=open(log, "w"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    time.sleep(3.5)
    pr = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", "/tmp/av_test.mp4",
                           "-c", "copy", "-f", "flv", f"rtmp://127.0.0.1:{RTMP}/live/av1s"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          preexec_fn=os.setsid)
    time.sleep(4)
    # 观众拉流会触发 fmp4/hls 落盘，堆损坏就发生在这一步
    v = subprocess.Popen(["ffmpeg", "-y", "-v", "error", "-rtsp_transport", "tcp",
                          "-i", f"rtsp://127.0.0.1:{RTSP}/live/av1s", "-t", "3",
                          "-c", "copy", f"/tmp/av1stab_{n}.mp4"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         preexec_fn=os.setsid)
    try: v.wait(timeout=20)
    except Exception: pass
    time.sleep(1)
    dead = srv.poll() is not None or "signal:" in open(log).read()
    crashes += 1 if dead else 0
    print(f"  第{n+1}轮: {'崩溃/退出' if dead else '存活'}")
    for x in (pr, srv):
        try: os.killpg(os.getpgid(x.pid), signal.SIGKILL)
        except Exception: pass
    time.sleep(0.7)

print(f"\n崩溃 {crashes}/{ROUNDS}")
sys.exit(0 if crashes == 0 else 1)
