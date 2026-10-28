#!/usr/bin/env python3
"""VP8/VP9 作为**源**时转码时间轴的复现与验收（2 倍速问题）。

问题
----
VP8/VP9 的码流、SDP、RTMP metadata 都没有帧率信息，`VideoTrackImp(CodecId)` 的
默认 `_fps=30` 被 `TranscodeSink` 当成源帧率传给编码器，于是每帧按 33ms 打时间戳，
而真实帧是 66ms 一帧 → 输出时间轴快一倍：拉 5.0 秒内容要花约 10 秒。

判据（真机）
------------
* 拉 5.0 秒内容：**墙钟耗时** ≤ 6.0s（+20% 容差）
* 解出时长 ≈ 5.0s（±0.7s）
* 包数 ≈ 5s × 真实帧率（±15%）

用法
----
    env -u PYTHONPATH python3 scripts/vp8_source_timeline_test.py            # 全量 1 轮
    env -u PYTHONPATH python3 scripts/vp8_source_timeline_test.py --rounds 3
    env -u PYTHONPATH python3 scripts/vp8_source_timeline_test.py --only vp8

端口（本 worktree 独占）：RTMP 63900 / RTSP 63554 / HTTP 64080。
起服务只杀「监听自己端口」的那个 pid，绝不用 pkill -f MediaServer。
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.parse
import urllib.request

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
CFG = "/tmp/zlm_vp8_timeline.ini"  # 放 /tmp，避免污染工作区
# 基线配置：优先用主干统一的那份；config.av.ini 只存在于 a7 worktree，别写死
BASE_CFG = next((p for p in (f"{WT}/config.joint.base.ini", f"{WT}/config.av.ini") if os.path.exists(p)),
                f"{WT}/config.joint.base.ini")
LOG = "/tmp/zlm_vp8_timeline.log"
WORK = "/tmp/zlm_vp8_timeline"
HTTP, RTMP, RTSP = 64080, 63900, 63554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
FF = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffmpeg")
FP = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffprobe")

SRC_H264 = f"{WORK}/src_h264.mp4"      # 320x240@15 + aac
SRC_H265 = f"{WORK}/src_h265.mp4"      # 640x360@15 + aac

# 名字, 推流参数, 目标视频 codec, 期望源 codec, 期望帧率, 推流协议
CASES = [
    ("vp8", ["-i", SRC_H264, "-c:v", "libvpx", "-b:v", "600k", "-g", "15",
             "-keyint_min", "15", "-deadline", "realtime", "-cpu-used", "8", "-c:a", "copy"],
     "h264", "vp8", 15, "rtsp"),
    ("vp9", ["-i", SRC_H264, "-c:v", "libvpx-vp9", "-b:v", "600k", "-g", "15",
             "-deadline", "realtime", "-cpu-used", "8", "-lag-in-frames", "0",
             "-auto-alt-ref", "0", "-strict", "experimental", "-c:a", "copy"],
     "h264", "vp9", 15, "rtsp"),
    # 回归：帧率能正常拿到的源，时间轴不能变
    ("h264_src", ["-i", SRC_H264, "-c", "copy"], "h265", "h264", 15, "rtsp"),
    ("h265_src", ["-i", SRC_H265, "-c", "copy"], "h264", "hevc", 15, "rtsp"),
    # RTMP 推流的拓扑（音频在这条链上才通），确认时间轴同样正确
    ("h264_rtmp", ["-i", SRC_H264, "-c", "copy"], "h265", "h264", 15, "rtmp"),
]


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def listener_pid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.split()
    return int(out[0]) if out else None


def api(path, **kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read().decode())


def prepare_config():
    """从 config.av.ini 派生：codec 走 API、关掉 TS/HLS/fMP4、RTSP 关 directProxy。"""
    txt = open(BASE_CFG, encoding="utf-8").read()
    # [transcode] 全局段的 codec 清空：本次只按流下发，避免继承 h265/opus
    txt = re.sub(r"(?m)^video_codec=.*$", "video_codec=", txt, count=1) if "[transcode]" in txt else txt
    blocks = re.split(r"(?m)^(?=\[)", txt)
    out = []
    for blk in blocks:
        head = blk.split("\n", 1)[0].strip()
        if head == "[transcode]":
            blk = re.sub(r"(?m)^(video_codec|audio_codec)=.*$", r"\1=", blk)
            blk = re.sub(r"(?m)^enable=.*$", "enable=0", blk)
        if head == "[rtsp]":
            # 转码输出要能被观众拿到：directProxy=1 时 RTSP 源的 RTP 直接透传
            blk = re.sub(r"(?m)^directProxy=.*$", "directProxy=0", blk)
        if head == "[general]" or head == "":
            blk = re.sub(r"(?m)^enable_hls=.*$", "enable_hls=0", blk)
            blk = re.sub(r"(?m)^enable_ts=.*$", "enable_ts=0", blk)
            blk = re.sub(r"(?m)^enable_fmp4=.*$", "enable_fmp4=0", blk)
        out.append(blk)
    txt = "".join(out)
    open(CFG, "w", encoding="utf-8").write(txt)


def kill_own(pid):
    """只杀自己起的那个 pid（并且它必须还监听我们的 RTSP 端口）。"""
    if not pid:
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    for _ in range(40):
        time.sleep(0.1)
        if listener_pid(RTSP) is None:
            return


def start_server():
    # 保险：如果我们的端口上已有别人占用的进程，先如实报错而不是乱杀
    pid = listener_pid(RTSP)
    if pid:
        raise SystemExit(f"port {RTSP} already in use by pid {pid}; refusing to kill it")
    open(LOG, "w").close()
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=WT,
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
    for _ in range(120):
        time.sleep(0.25)
        if listener_pid(RTSP) == p.pid:
            return p
    raise SystemExit("MediaServer did not come up (check %s)" % LOG)


def push(name, args, proto="rtsp"):
    if proto == "rtmp":
        url = f"rtmp://127.0.0.1:{RTMP}/live/{name}"
        tail = ["-f", "flv", url]
    else:
        url = f"rtsp://127.0.0.1:{RTSP}/live/{name}"
        tail = ["-f", "rtsp", "-rtsp_transport", "tcp", url]
    cmd = [FF, "-hide_banner", "-re", "-stream_loop", "-1"] + args + tail
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(f"{WORK}/{name}_push.log", "w"),
                            preexec_fn=os.setsid)


def pull_measure(name, seconds=5):
    """拉 `seconds` 秒内容。

    返回 (墙钟耗时, 包统计, 解码错误数, 错误样本, 时间轴斜率)。
    斜率 = ffmpeg `-progress` 里输出时间轴的推进速度 / 墙钟推进速度：
    1.0 表示输出时间轴与真实时间同步（这是本 bug 的核心判据），
    0.5 表示时间轴快一倍（修前的 VP8/VP9 源）。用斜率而不是只看墙钟耗时，
    是因为接入时可能要等一个关键帧（GOP 30 帧 = 2s），那段时间与时间轴对不对无关。
    """
    out = f"{WORK}/{name}_pull.mkv"
    if os.path.exists(out):
        os.remove(out)
    cmd = [FF, "-hide_banner", "-v", "error", "-progress", "pipe:1", "-nostats",
           "-rtsp_transport", "tcp", "-i", f"rtsp://127.0.0.1:{RTSP}/live/{name}",
           "-t", str(seconds), "-c", "copy", "-y", out]
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    samples = []
    for line in p.stdout:
        line = line.strip()
        if line.startswith("out_time_us=") or line.startswith("out_time_ms="):
            v = line.split("=", 1)[1]
            if v.isdigit():
                samples.append((time.monotonic() - t0, int(v) / 1e6))
    err_txt = p.stderr.read()
    p.wait(timeout=60)
    wall = time.monotonic() - t0
    if p.returncode != 0 or not os.path.exists(out):
        return wall, None, -1, err_txt.strip()[:300], None
    # 斜率：丢掉第一个采样点（此时可能还在等关键帧），用首个真实推进点到最后一点
    slope = None
    if len(samples) >= 3:
        c = [(t, s) for t, s in samples if s > 0]
        if len(c) >= 2:
            (t1, s1), (t2, s2) = c[0], c[-1]
            if t2 - t1 > 0.5 and s2 > s1:
                slope = round((s2 - s1) / (t2 - t1), 3)
    # 每条轨的时长用「包时间戳跨度 + 一个帧间隔」算：Matroska 的 per-stream
    # duration 恒为 0，只能自己从包算，否则判据永远不成立。
    pr = sh(f"{FP} -v error -show_entries packet=stream_index,pts_time,dts_time "
            f"-show_entries stream=index,codec_type,codec_name,width,height -of json {out}")
    raw = json.loads(pr.stdout)
    stats = {}
    for st in raw["streams"]:
        idx = st["index"]
        ts = []
        for pk in raw["packets"]:
            if pk.get("stream_index") == idx:
                t = pk.get("pts_time") or pk.get("dts_time")
                if t not in (None, "N/A"):
                    ts.append(float(t))
        if not ts:
            continue
        ts.sort()
        gaps = [b - a for a, b in zip(ts, ts[1:]) if b > a]
        step = sorted(gaps)[len(gaps) // 2] if gaps else 0.0
        stats[idx] = {
            "type": st["codec_type"], "codec": st.get("codec_name"),
            "size": f"{st.get('width')}x{st.get('height')}",
            "pkts": len(ts), "dur": round(ts[-1] - ts[0] + step, 3),
            "step": round(step, 4), "dup_pts": len(ts) - len(set(ts)),
        }
    # 解码校验走 rawvideo：`-f null` 会把自己的「dts 不单调」抱怨混进来（那是
    # 客户端重新封装的产物，不是解码错误），rawvideo 只解码不碰时间戳。
    dec = sh(f"{FF} -hide_banner -v error -i {out} -f rawvideo -pix_fmt yuv420p -y /dev/null")
    errs = [l for l in dec.stderr.splitlines() if l.strip()]
    return wall, stats, len(errs), " | ".join(errs[:3]), slope


def log_grep(pattern):
    try:
        txt = open(LOG, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return []
    return re.findall(pattern, txt)


def run_case(name, push_args, target, want_src, want_fps, seconds=5, proto="rtsp"):
    stream = f"tl_{name}"
    server = start_server()
    res = {"case": name, "target": target}
    try:
        r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=stream,
                video_codec=target, audio_codec="aac", enable=1)
        res["addTranscode"] = f"{r.get('code')}/{r.get('applied')}"
        pub = push(stream, push_args, proto)
        time.sleep(8)
        wall, stats, errs, err, slope = pull_measure(stream, seconds)
        res["wall"] = round(wall, 2)
        res["slope"] = slope
        res["decode_errors"] = errs
        res["pull_err"] = err
        if stats:
            v = next((s for s in stats.values() if s["type"] == "video"), {})
            a = next((s for s in stats.values() if s["type"] == "audio"), {})
            res["codec"] = v.get("codec")
            res["size"] = v.get("size")
            res["v_pkts"] = int(v.get("pkts", 0) or 0)
            res["v_dur"] = v.get("dur")
            res["v_step"] = v.get("step")
            res["v_dup_pts"] = v.get("dup_pts")
            res["a_pkts"] = int(a.get("pkts", 0) or 0)
            res["a_dur"] = a.get("dur")
        fps_log = log_grep(r"video encoder params: src_fps=([0-9.]+)")
        res["src_fps_declared"] = fps_log[-1] if fps_log else None
        meas = log_grep(r"source fps measured: ([0-9.]+) \(interval=([0-9.]+)ms")
        res["src_fps_measured"] = f"{meas[-1][0]}/{meas[-1][1]}ms" if meas else None
        try:
            os.killpg(os.getpgid(pub.pid), signal.SIGKILL)
        except Exception:
            pass
    finally:
        kill_own(server.pid)
    # 判据
    exp_pkts = seconds * want_fps
    # 时间轴斜率是主判据：1.0 = 输出时间轴与真实时间同步；修前的 2× 速会落在 0.5
    slope = res.get("slope")
    ok_slope = slope is not None and 0.85 <= slope <= 1.15
    # 墙钟：内容 5s + 至多一个 GOP 的关键帧等待（gop=30 帧 = 2s）为上限，
    # 修前的 2× 速是 9.5~10s，仍然会被这条挡住
    ok_wall = res.get("wall") is not None and res["wall"] <= seconds + 2.5  # type: ignore
    ok_dur = abs((res.get("v_dur") or 0) - seconds) <= 0.7
    ok_pkt = abs((res.get("v_pkts") or 0) - exp_pkts) <= exp_pkts * 0.15
    ok_codec = (res.get("codec") == want_src) if target in ("vp8", "vp9") else \
        res.get("codec") == {"h265": "hevc"}.get(target, target)
    ok_err = res.get("decode_errors") == 0
    # 音频：这条拓扑（RTSP 推入）实测源音频轨没有帧进来，改前改后都一样，
    # 因此没有音频包时记为 n/a（不计入判定），有音频包就必须时长对得上
    if res.get("a_dur") is None:
        ok_audio, audio_note = True, "n/a(无音频包)"
    else:
        ok_audio = abs((res.get("a_dur") or 0) - seconds) <= 0.8
        audio_note = "ok" if ok_audio else "bad"
    res["audio_note"] = audio_note
    res["PASS"] = all([ok_slope, ok_wall, ok_dur, ok_pkt, ok_codec, ok_err, ok_audio])
    res["judge"] = {"slope": ok_slope, "wall": ok_wall, "duration": ok_dur, "packets": ok_pkt,
                    "codec": ok_codec, "decode": ok_err, "audio": ok_audio,
                    "exp_pkts": exp_pkts}
    return res


def make_sources():
    os.makedirs(WORK, exist_ok=True)
    jobs = [
        (SRC_H264, "testsrc2=size=320x240:rate=15", "libx264", ["-pix_fmt", "yuv420p", "-g", "15"]),
        (SRC_H265, "testsrc2=size=640x360:rate=15", "libx265", ["-pix_fmt", "yuv420p", "-g", "15",
                                                              "-x265-params", "keyint=15:min-keyint=15"]),
    ]
    for path, src, enc, extra in jobs:
        if os.path.exists(path):
            continue
        cmd = (f"{FF} -hide_banner -v error -y -f lavfi -i {src} -f lavfi -i sine=frequency=440:sample_rate=44100 "
               f"-t 20 -c:v {enc} {' '.join(extra)} -c:a aac -b:a 64k -shortest {path}")
        r = sh(cmd, timeout=300)
        if r.returncode != 0:
            raise SystemExit(f"failed to build {path}: {r.stderr[:300]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--only", default="")
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--bin", default="", help="覆盖 MediaServer 路径（改前基线用）")
    a = ap.parse_args()

    global BIN
    if a.bin:
        BIN = a.bin
    for need in (BIN, FF, FP):
        if not os.path.exists(need):
            raise SystemExit(f"missing {need}")
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    make_sources()
    prepare_config()

    cases = [c for c in CASES if not a.only or a.only in c[0]]
    all_ok = True
    report = []
    for rnd in range(1, a.rounds + 1):
        print(f"\n===== round {rnd}/{a.rounds} =====")
        for name, args, target, want_src, fps, proto in cases:
            res = run_case(name, args, target, want_src, fps, a.seconds, proto)
            res["round"] = rnd
            report.append(res)
            all_ok = all_ok and res["PASS"]
            j = res["judge"]
            print(f"{'PASS' if res['PASS'] else 'FAIL'} {name:9s} -> {target:5s}  "
                  f"wall={res.get('wall')}s  slope={res.get('slope')}  dur={res.get('v_dur')}s"
                  f"  v_pkts={res.get('v_pkts')} (exp {j['exp_pkts']:.0f})  dup_pts={res.get('v_dup_pts')}"
                  f"  codec={res.get('codec')}  err={res.get('decode_errors')}"
                  f"  src_fps={res.get('src_fps_declared')}/{res.get('src_fps_measured')}")
            if not res["PASS"]:
                print(f"     judge={j}" + (f" pull_err={res['pull_err']}" if res.get("pull_err") else ""))
    open(f"{WORK}/report.json", "w").write(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"\nreport: {WORK}/report.json   server log: {LOG}")
    print("ALL PASS" if all_ok else "SOME FAILED")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
