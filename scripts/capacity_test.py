#!/usr/bin/env python3
"""容量测试：这台机器能同时转多少路 1280x720@30？

## 为什么不能用服务端计数

`getTranscode` 的 `tracks[].frames` 是按「编码输出帧/NAL」累加的：H265 按单 NAL 造帧时，
一个 IDR 会产生多个输出。实测同一窗口内服务端 248 帧 / 3.9s（63fps），
拉流端 135 包 / 5s（27fps），而源是 30fps —— 计数比真实帧率高约一倍。
用它判容量会把容量放大一倍（第一版就得出「12 路跑满」的错误结论）。

## 为什么不用「窗口内发包数 / 墙钟」

3 秒窗口必然含「加入等待」（握手 + 等首个关键帧），单路也只有 27/30，会系统性低估。

## 走过的三条弯路（都实测过，别再回头）

1. 用 `tracks[].frames` 算帧率 → 该计数按 NAL 累加，虚高 2.2 倍（63fps vs 真实 27fps）；
2. 改成拉流发包数 / 墙钟 → 3 秒窗口含加入等待，单路也只有 27/30，系统性低估；
3. 改成并发拉流 + 媒体时长 → 干净了，但要起 N 个 ffmpeg，在 WSL 上测量自身成为干扰
   （对照流瞬间只有 6.5fps）。

## 现在的口径：每路自己跟自己比

服务端暴露两个**单调累加**的计数（都在 `getTranscode` 的 `tracks[]` 里）：

- `frames`：编码器输出的**包数 = 帧数**（修过：曾按 NAL 计数虚高 2.2 倍）
- `input_frames`：解码后送进编码器的帧数

取一个窗口量两侧增量：

- **输入帧率**（fps_in）反映源有没有喂满；
- **输出帧率**（fps_out）反映编码跟不跟得上；
- 判据：`fps_in ≥ 源 × 0.95`（否则是推流端受限，该档不作容量结论）
  **且** `fps_out / fps_in ≥ 97%`（编码不是瓶颈）且无 `failed_tracks`。

优点：零外部进程（不污染被测对象）、无窗口偏置（两侧同一时钟）、
且天然把「源慢」和「编码慢」分开 —— 不需要对照组。

用法：
  env -u PYTHONPATH python3 scripts/capacity_test.py               # 硬编，4/8/12/16 路
  env -u PYTHONPATH python3 scripts/capacity_test.py --sw          # 软编对照
  env -u PYTHONPATH python3 scripts/capacity_test.py --max 24 --step 6
  env -u PYTHONPATH python3 scripts/capacity_test.py --window 10   # 拉流窗口（媒体秒）
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
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _platshim import (bin_path, bin_dir, pid_on_port, rss_kb, gpu_count, gpu_name,
                       kill_group, load_avg, ncpu, nvidia_smi, sh_retry)

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN, BIN_DIR = bin_path(), bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.capacity.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
LOG = "/tmp/capacity.log"
SRC = "/tmp/src_1280x720.mp4"
SRC_FPS = 30.0
CODEC = "H265"
HW = "--sw" not in sys.argv


def _arg(name, default):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return int(sys.argv[i + 1])
    return default


MAX = _arg("--max", 16)
STEP = _arg("--step", 4)
WINDOW = _arg("--window", 8)      # 每档拉流窗口（媒体秒）
RAMP = 8                           # 加路后等待起流

PASS, FAIL = [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=180):
    return sh_retry(cmd, timeout)


def api(path, **kw):
    kw.setdefault("secret", SECRET)
    kw.setdefault("vhost", "__defaultVhost__")
    kw.setdefault("app", "live")
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    with urllib.request.urlopen(url, timeout=8) as r:
        return json.loads(r.read().decode())


def gpu_encoder_util():
    """GPU 编码器利用率（%）。WSL 的 nvidia-smi 在 /usr/lib/wsl/lib，必须绝对路径。"""
    for _ in range(5):
        smi = nvidia_smi()
        if not smi:
            return None
        out = sh(f"{smi} --query-gpu=utilization.encoder --format=csv,noheader",
                 timeout=20).stdout
        m = re.search(r"(\d+)\s*%", out)
        return int(m.group(1)) if m else None
    return None


def counters(stream):
    """返回 (输出帧数, 输入帧数)。两者都由服务端单调累加，不受测量进程干扰。

    输出用编码器的「包=帧」计数（修过：NAL 级计数虚高 2.2 倍），
    输入是解码后送进编码器的帧数 —— 每路自己跟自己比，就能区分
    「源没喂满」与「编码跟不上」，不需要额外的对照组或拉流进程。
    """
    try:
        body = api("getTranscode", stream=stream) or {}
    except Exception:
        return 0, 0
    for t in body.get("tracks") or []:
        if t.get("type") == "video":
            return int(t.get("frames") or 0), int(t.get("input_frames") or 0)
    return 0, 0


def measure_all(streams, secs=WINDOW):
    """采样一个窗口，返回 {stream: (fps_out, fps_in)}。"""
    before = {s: counters(s) for s in streams}
    time.sleep(secs)
    after = {s: counters(s) for s in streams}
    res = {}
    for s in streams:
        o1, i1 = before[s]
        o2, i2 = after[s]
        res[s] = ((o2 - o1) / float(secs), (i2 - i1) / float(secs))
    return res


def proc_cpu(pid):
    """进程瞬时 CPU%（ps 的 %cpu 是自启动以来平均，短测不可用）。"""
    def ticks():
        out = sh(f"ps -o cputime= -p {pid}", timeout=20).stdout.strip()
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
            sh(f"kill -9 {pid}", timeout=20)
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


def enc_name(stream):
    try:
        for t in (api("getTranscode", stream=stream) or {}).get("tracks") or []:
            if t.get("type") == "video" and t.get("encoder"):
                return t["encoder"]
    except Exception:
        pass
    return None


def failed_tracks(stream):
    try:
        return int((api("getTranscode", stream=stream) or {}).get("failed_tracks") or 0)
    except Exception:
        return 0


def main():
    print("=" * 78)
    print(f"容量测试：{CODEC} {'硬编' if HW else '软编'}，源 1280x720@{SRC_FPS:.0f}，"
          f"GPU={gpu_name() or '无'} x{gpu_count()}，CPU={ncpu()} 核，负载={load_avg():.2f}")
    print("口径：每路自己跟自己比 —— 输出帧率是否追平输入帧率（服务端包=帧计数）")
    print("=" * 78)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}（跑 scripts/test_assets.py）")
        return 2
    make_cfg()
    srv = start()
    pushers = {}
    rows = []

    try:
        n = 0
        while n < MAX:
            n += STEP
            for k in range(len(pushers), n):
                st = f"cap_{k}"
                r = api("addTranscode", stream=st, video_codec=CODEC, hw=1 if HW else 0)
                if r.get("code") != 0:
                    note(False, f"{st} 参数下发", f"{r}")
                pushers[st] = push(st)
            time.sleep(RAMP)

            streams = list(pushers)
            enc = next((enc_name(s) for s in streams if enc_name(s)), None)
            enc_util = gpu_encoder_util() if HW else None
            measured = measure_all(streams)
            outp = {s: v[0] for s, v in measured.items()}
            inp = {s: v[1] for s, v in measured.items()}
            worst_out = min(outp.values()) if outp else 0.0
            worst_in = min(inp.values()) if inp else 0.0
            # 每路自己跟自己比：输出是否追得上输入。追得上 = 编码不是瓶颈。
            ratio = (worst_out / worst_in) if worst_in > 0 else 0.0
            cpu = round(proc_cpu(srv.pid), 1) if not HW else None
            if HW and cpu is None:
                cpu = sh(f"ps -o %cpu= -p {srv.pid}", timeout=20).stdout.strip()
                try:
                    cpu = round(float(cpu), 1)
                except Exception:
                    pass
            rss = rss_kb(srv.pid)
            failed = sum(failed_tracks(s) for s in streams)
            rows.append(dict(n=n, out=round(worst_out, 1), inp=round(worst_in, 1),
                             ratio=round(ratio, 3), cpu=cpu, enc=enc_util, rss=rss,
                             failed=failed, enc_name=enc))
            print(f"  {n:2d} 路 | 输入 {worst_in:5.1f}fps 输出 {worst_out:5.1f}fps "
                  f"(追平率 {ratio * 100:5.1f}%) | CPU {cpu}% GPU编码 {enc_util}% "
                  f"RSS {rss}KB | failed={failed} | {enc or '-'}")
            # 判据：输出追得上输入（≥97%），且输入本身是满的（≥源×0.95，否则是源没喂满）
            if worst_in < SRC_FPS * 0.95:
                zero = [s for s, v in inp.items() if v <= 0.5]
                print(f"  → {n} 路时输入只有 {worst_in:.1f}fps（源没喂满），"
                      f"该档不作为转码容量结论")
                if zero:
                    print(f"     输入为 0 的流：{zero}")
                    for s in zero[:3]:
                        try:
                            body = api("getTranscode", stream=s) or {}
                            print(f"     {s}: online={body.get('online')} "
                                  f"transcoding={body.get('transcoding')} msg={body.get('msg')}")
                        except Exception as e:
                            print(f"     {s}: 查询失败 {e}")
                break
            if ratio < 0.97:
                print(f"  → {n} 路时编码跟不上：输出 {worst_out:.1f} vs 输入 {worst_in:.1f}"
                      f"（{ratio * 100:.1f}%）")
                break

        ok = [r for r in rows
              if r["ratio"] >= 0.97 and r["inp"] >= SRC_FPS * 0.95 and r["failed"] == 0]
        best = max((r["n"] for r in ok), default=0)
        note(best > 0, "至少 1 档：输入满且输出追得上", f"{best} 路")
        note(all(r["ratio"] >= 0.97 for r in ok),
             "达标档位每路输出都追平输入（≥97%）", f"达标档：{[r['n'] for r in ok]}")
        print()
        print("=" * 78)
        print(f"结论：{CODEC} {'硬编' if HW else '软编'} 单机 **{best} 路** "
              f"1280x720@30（判据：输入满 30fps 且输出追平输入 ≥97%，无 failed_tracks）")
        if ok:
            t = ok[-1]
            print(f"      该档实测：输入 {t['inp']}fps / 输出 {t['out']}fps"
                  f"（{t['ratio'] * 100:.1f}%），CPU {t['cpu']}%，"
                  f"GPU 编码 {t['enc']}%，RSS {t['rss']}KB")
        if rows and rows[-1] not in ok:
            t = rows[-1]
            print(f"      拐点：{t['n']} 路时输入 {t['inp']}fps / 输出 {t['out']}fps")
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
