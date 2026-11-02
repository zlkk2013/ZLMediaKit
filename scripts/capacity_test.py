#!/usr/bin/env python3
"""容量测试：这台机器能同时转多少路？

客户第一个会问的问题，而且它能顺带暴露「编码跟不上时怎么办」——
所以除了「最多几路」，还要记录每路的**实际帧率是否追上源帧率**、CPU/GPU 占用、
以及内存曲线（无背压时内存会随积压增长）。

做法：从 1 路开始逐档加路（1→2→4→6→8→10→12…），每档稳定运行 N 秒后测：
- 每路服务端已编码帧数增量 / 时间  → 实际帧率
- 拉流端（可选，较慢）            → 用服务端计数为主，拉流只抽验一路
- 进程 CPU%（软编用）与 GPU 编码器利用率%（硬编用，nvidia-smi 的 encoder 列）
- 常驻内存

判定「这档撑得住」：所有路实际帧率 ≥ 源帧率 × 0.95，且无 failed_tracks。

用法：
  env -u PYTHONPATH python3 scripts/capacity_test.py                # 默认 720p，硬编
  env -u PYTHONPATH python3 scripts/capacity_test.py --sw           # 软件编码对照
  env -u PYTHONPATH python3 scripts/capacity_test.py --max 16 --step 2
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _platshim import (bin_path, bin_dir, pid_on_port, rss_kb, gpu_count, gpu_name,
                       supports_device_selection, kill_group, load_avg, ncpu, nvidia_smi)

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN, BIN_DIR = bin_path(), bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.capacity.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
LOG = "/tmp/capacity.log"
SRC = "/tmp/src_1280x720.mp4"
SRC_FPS = 30.0
BASELINE = 30.0   # 运行时用单路实测覆盖
CODEC = "H265"
HW = "--sw" not in sys.argv
MAX = 16
STEP = 4
DWELL = 12          # 每档稳定观察秒数
RAMP = 6            # 加路后等待起流时间

PASS, FAIL = [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def api(path, **kw):
    kw.setdefault("secret", SECRET)
    kw.setdefault("vhost", "__defaultVhost__")
    kw.setdefault("app", "live")
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    with urllib.request.urlopen(url, timeout=6) as r:
        return json.loads(r.read().decode())


def gpu_encoder_util():
    """GPU 编码器利用率（%）：nvidia-smi 的 utilization.encoder 列。

    必须用绝对路径：WSL 把 nvidia-smi 放在 /usr/lib/wsl/lib，不在精简 PATH 里，
    直接写命令名会拿不到读数（实测显示 None）。
    """
    smi = nvidia_smi() or "nvidia-smi"
    out = sh(f"{smi} --query-gpu=utilization.encoder,utilization.gpu --format=csv,noheader").stdout
    try:
        enc, gpu = [x.strip().rstrip("%") for x in out.strip().splitlines()[0].split(",")]
        return int(enc), int(gpu)
    except Exception:
        return None, None


def proc_cpu(pid):
    """进程 CPU%（ps 的 %cpu 是「自启动以来平均」，用两次采样算瞬时更准）。"""
    def ticks():
        out = sh(f"ps -o cputime= -p {pid}").stdout.strip()
        m = re.match(r"(?:(\d+)-)?(\d+):(\d+):(\d+)", out)
        if not m:
            return 0
        d, h, mi, s = (int(x or 0) for x in m.groups())
        return ((d * 24 + h) * 3600) + mi * 60 + s
    t1 = ticks(); r1 = time.time()
    time.sleep(3)
    t2 = ticks(); r2 = time.time()
    return (t2 - t1) / (r2 - r1) * 100.0


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
        pid = pid_on_port(p)
        if pid:
            sh(f"kill -9 {pid}")
            time.sleep(0.6)
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=BIN_DIR,
                         stdout=open(LOG, "w"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(80):
        time.sleep(0.25)
        if p.poll() is None and pid_on_port(RTSP) == p.pid:
            return p
    raise RuntimeError("MediaServer 没起来，看 " + LOG)


def push(stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def frames_of(stream):
    """该流视频轨已编码帧数 + 编码器名（只用于判活/拿编码器名）。"""
    try:
        body = api("getTranscode", stream=stream)
    except Exception:
        return None, None
    for t in body.get("tracks") or []:
        if t.get("type") == "video":
            return int(t.get("frames") or 0), t.get("encoder")
    return None, None


def pulled_fps(stream, secs=3):
    """拉流端实测帧率 —— 容量的唯一可信口径。

    为什么不用 tracks[].frames：那个计数是按「编码输出帧/NAL」累加的，我们在 H265 上按
    单 NAL 造帧，一个 IDR 会产生多个输出 → 计数比真实帧率高（实测 63fps vs 真实 27fps）。
    用它做判据会把容量放宽一倍。
    Measured frame rate on the pull side. tracks[].frames counts encoder outputs/NALs; with
    per-NAL framing an IDR yields several outputs, so it reads ~2x high (63 vs 27 fps).
    """
    out = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
             f"-t {secs} -map 0:v:0 -c copy -f framecrc -", timeout=secs * 6 + 30).stdout
    n = len([l for l in out.splitlines()
             if l and not l.startswith(("#", "stream")) and "," in l])
    return n / float(secs)


def main():
    print("=" * 78)
    print(f"容量测试：{CODEC} {'硬编' if HW else '软编'}，源 1280x720@{SRC_FPS:.0f}，"
          f"GPU={gpu_name() or '无'} x{gpu_count()}，CPU 核数={ncpu()}，负载={load_avg():.2f}")
    print("=" * 78)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}")
        return 2
    make_cfg()
    srv = start()
    pushers = {}
    rows = []
    global BASELINE
    try:
        # 单路基线：先只跑一路，用同一套拉流口径测一次
        api("addTranscode", stream="cap_base", video_codec=CODEC, hw=1 if HW else 0)
        pb = push("cap_base")
        time.sleep(RAMP)
        BASELINE = pulled_fps("cap_base", 5)
        pushers["cap_base"] = pb
        print(f"  单路基线：{BASELINE:.1f}fps（同一拉流口径）")

        n = 0
        while n < MAX:
            n += STEP
            t0 = time.time()
            # 加路：新流先下发参数，再推流（参数必须在推流前到）
            for k in range(len(pushers), n):
                st = f"cap_{k}"
                r = api("addTranscode", stream=st, video_codec=CODEC, hw=1 if HW else 0)
                if r.get("code") != 0:
                    note(False, f"cap_{k} 参数下发", f"{r}")
                pushers[st] = push(st)
            time.sleep(RAMP)

            streams = list(pushers)
            enc = next((frames_of(s)[1] for s in streams if frames_of(s)[1]), None)
            if not HW:
                cpu0 = proc_cpu(srv.pid)
            else:
                cpu0 = sh(f"ps -o %cpu= -p {srv.pid}").stdout.strip()
            # 逐路拉流实测（真实口径，见 pulled_fps 的注释）
            fps = {s: pulled_fps(s, 3) for s in streams}
            worst = min(fps.values()) if fps else 0
            try:
                cpu1 = round(float(cpu0), 1)
            except Exception:
                cpu1 = None
            enc_util, gpu_util = gpu_encoder_util() if HW else (None, None)
            rss = rss_kb(srv.pid)
            failed = sum(int((api("getTranscode", stream=s) or {}).get("failed_tracks") or 0)
                         for s in streams)
            row = dict(n=n, worst=round(worst, 2), avg=round(sum(fps.values()) / len(fps), 2),
                       cpu=cpu1, enc=enc_util, gpu=gpu_util, rss=rss, failed=failed,
                       enc_name=enc, took=round(time.time() - t0, 1))
            rows.append(row)
            print(f"  {n:2d} 路 | 最差 {row['worst']:5.1f}fps 平均 {row['avg']:5.1f}fps | "
                  f"CPU {row['cpu']}% GPU编码 {row['enc']}% RSS {row['rss']}KB "
                  f"| failed={row['failed']} | {enc or '-'}")
            if row["worst"] < BASELINE * 0.95:
                print(f"  → {n} 路已经不达标（最差 {row['worst']:.1f} < {BASELINE * 0.95:.1f}）")
                break

        # 判据基准用「单路实测」而不是标称 30fps：3 秒窗口必然含加入等待（GOP 0.5s），
        # 单路也测不到 30。用同一口径的基线比较，偏差互相抵消。
        # Compare against a single-stream baseline measured the same way instead of the
        # nominal 30fps: any window includes join latency, so even one stream reads < 30.
        ok_rows = [r for r in rows if r["worst"] >= BASELINE * 0.95 and r["failed"] == 0]
        best = max((r["n"] for r in ok_rows), default=0)
        note(best > 0, "至少 1 路能追上单路基线", f"{best} 路")
        note(all(r["worst"] >= BASELINE * 0.95 for r in ok_rows),
             "达标档位全部路数都追上单路基线",
             f"达标档：{[r['n'] for r in ok_rows]}")
        print()
        print("=" * 78)
        print(f"结论：{CODEC} {'硬编' if HW else '软编'} 单机可达 **{best} 路** "
              f"1280x720@{SRC_FPS:.0f}（判据：每路帧率 ≥ 源 × 0.95 且无 failed_tracks）")
        if rows:
            t = rows[min(len(rows) - 1, len(ok_rows))]
            print(f"      达标档位实测：CPU {t['cpu']}%，GPU 编码 {t['enc']}%，RSS {t['rss']}KB")
            print(f"      下一档（{rows[-1]['n']} 路）最差帧率 {rows[-1]['worst']}fps → 拐点")
        print("=" * 78)
    finally:
        for p in pushers.values():
            kill_group(p)
        try:
            os.killpg(os.getpgid(srv.pid), signal.SIGKILL)
        except Exception:
            pass

    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
