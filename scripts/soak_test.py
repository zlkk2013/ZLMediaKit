#!/usr/bin/env python3
"""长跑稳定性（soak）测试：连续转码 + 反复推拉，盯内存/句柄/线程/媒体源是否泄漏。

为什么需要：13 套功能验收每套只跑 1~10 分钟，抓不到「缓慢上涨」类问题。
引用环泄漏、句柄不释放、线程堆积都只在长时间运行后暴露（本项目就曾出现
muxer 引用环导致媒体源永不释放，短测完全看不出来）。

判据（全部满足才算通过）：
  1. **服务始终存活且可响应**：每轮调 getServerConfig，必须 <3 秒返回
     （自死锁不崩溃，只失去响应，ps 看着一切正常）
  2. **内存不持续上涨**：RSS 在预热后进入平台期，末段比平台期涨幅 < 15%
     （允许首轮有 JIT/缓存增长，所以先跑 20% 时间做预热再取基线）
  3. **句柄不泄漏**：文件描述符数量末段相对基线增长 < 50
  4. **线程数稳定**：末段线程数相对基线增长 <= 4
  5. **媒体源回到基线**：所有推流端停止并等过宽限期后，getMediaList 里
     本轮用到的流必须全部消失

用法：
  env -u PYTHONPATH python3 scripts/soak_test.py            # 默认 12 分钟
  env -u PYTHONPATH python3 scripts/soak_test.py 30          # 30 分钟
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
CFG = f"{WT}/config.soak.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
LOG = "/tmp/soak.log"
MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
# 两路流：一路软编（CPU 压力真实）、一路硬编（覆盖 GPU 路径）
STREAMS = [("soak_sw", {"video_codec": "H265", "hw": 0}),
           ("soak_hw", {"video_codec": "H265", "hw": 1})]
SRC = "/tmp/src_1280x720.mp4"

PASS, FAIL = [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.split()
    return int(out[0]) if out else None


def api(path, timeout=3, **kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    t0 = time.time()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = json.loads(r.read().decode())
        return body, time.time() - t0
    except Exception as e:
        return {"_err": str(e)}, time.time() - t0


def rss_kb(pid):
    out = sh(f"ps -o rss= -p {pid}").stdout.strip()
    return int(out) if out.isdigit() else None


def fd_count(pid):
    return int(sh(f"/usr/sbin/lsof -p {pid} 2>/dev/null | wc -l").stdout.strip() or 0)


def thread_count(pid):
    out = sh(f"ps -M -p {pid} 2>/dev/null | wc -l").stdout.strip()
    return int(out) if out.isdigit() else None


def our_sources():
    body, _ = api("getMediaList")
    names = set()
    for item in (body.get("data") or []):
        if item.get("stream", "").startswith("soak_"):
            names.add(item["stream"])
    return names


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
                         stdout=open(LOG, "w"), stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
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


def viewer(stream, secs=8):
    return subprocess.Popen(["ffmpeg", "-v", "error", "-rtsp_transport", "tcp",
                             "-i", f"rtsp://127.0.0.1:{RTSP}/live/{stream}",
                             "-t", str(secs), "-f", "null", "-"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def main():
    print("=" * 76)
    print(f"长跑稳定性：{MINUTES:.0f} 分钟，{'/'.join(s for s, _ in STREAMS)} 两路转码 + 反复推拉")
    print("=" * 76)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}，先跑 scripts/test_assets.py")
        return 2
    make_cfg()
    srv = start()
    pid = srv.pid
    samples = []
    t0 = time.time()
    end = t0 + MINUTES * 60
    round_no = 0
    pushers = []
    try:
        for stream, params in STREAMS:
            api("addTranscode", vhost="__defaultVhost__", app="live", stream=stream, reset=1)
            r, _ = api("addTranscode", vhost="__defaultVhost__", app="live", stream=stream, **params)
            if r.get("code") != 0:
                note(False, f"{stream} 参数下发", f"{r}")
            else:
                note(True, f"{stream} 参数下发")
            pushers.append(push(stream))

        while time.time() < end:
            round_no += 1
            # 每轮：挂两个观众、看几秒、断开（覆盖 reader 增删 + 编码并发）
            vs = [viewer(s, 6) for s, _ in STREAMS]
            time.sleep(7)
            for v in vs:
                try:
                    os.killpg(os.getpgid(v.pid), signal.SIGKILL)
                except Exception:
                    pass
            body, cost = api("getServerConfig")
            alive = body.get("code") == 0 and cost < 3.0
            samples.append({
                "t": round(time.time() - t0, 1),
                "rss": rss_kb(pid),
                "fd": fd_count(pid),
                "thr": thread_count(pid),
                "resp_ms": round(cost * 1000),
                "alive": alive,
                "sources": len(our_sources()),
            })
            s = samples[-1]
            print(f"  [{s['t']:6.1f}s] rss={s['rss']}KB fd={s['fd']} thread={s['thr']} "
                  f"resp={s['resp_ms']}ms alive={alive} sources={s['sources']}")
            if not alive:
                note(False, "服务在长跑中失去响应", f"t={s['t']}s")
                break
            time.sleep(8)

        # 判据 1：全程存活可响应
        note(all(s["alive"] for s in samples) and len(samples) > 3,
             "服务全程存活且可响应（无静默死锁）",
             f"{len(samples)} 轮，最慢响应 {max((s['resp_ms'] for s in samples), default=0)}ms")

        # 判据 2/3/4 都量「稳态漂移」，不是「从某个点开始的增长」：
        # 线程池/连接缓存在运行中途会有一次性扩张（实测 308s 时 fd 120->147、线程 29->37，
        # 之后一直平），把基线取在扩张之前会把它误判成泄漏。
        # 取中点做基线、末段取中位数，并允许小幅震荡（worker 会创建/销毁）。
        import statistics
        mid = max(1, len(samples) // 2)
        tail = samples[len(samples) * 3 // 4:]
        base = samples[mid]
        last = samples[-1]
        tail_fd = statistics.median(s["fd"] for s in tail)
        tail_thr = statistics.median(s["thr"] for s in tail)
        tail_rss = statistics.median(s["rss"] for s in tail)
        growth = (tail_rss - base["rss"]) / base["rss"] * 100
        note(growth < 15, "内存稳态不漂移（后半段中位数涨幅 <15%）",
             f"中点 {base['rss']}KB -> 末段中位 {int(tail_rss)}KB（{growth:+.1f}%）")

        # 判据 3：句柄（稳态漂移）
        fd_growth = tail_fd - base["fd"]
        note(fd_growth < 20, "文件句柄稳态不增长（<20）",
             f"中点 {base['fd']} -> 末段中位 {int(tail_fd)}（{fd_growth:+.0f}）")

        # 判据 4：线程（允许 ±2 的 worker 创建/销毁震荡）
        thr_growth = tail_thr - base["thr"]
        note(thr_growth <= 2, "线程数稳态稳定（中位数增长 <=2）",
             f"中点 {base['thr']} -> 末段中位 {int(tail_thr)}（{thr_growth:+.0f}）")

        # 判据 5：停掉推流端 + 等过宽限期，媒体源必须消失
        for p in pushers:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except Exception:
                pass
        pushers = []
        print("  停掉推流端，等 20s（宽限期 15s）后检查媒体源是否清空…")
        time.sleep(20)
        left = our_sources()
        note(not left, "推流端停止后媒体源全部释放（无泄漏）", f"残留 {left if left else '无'}")
    finally:
        for p in pushers:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except Exception:
                pass
        try:
            os.killpg(os.getpgid(srv.pid), signal.SIGKILL)
        except Exception:
            pass

    print()
    print("=" * 76)
    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}（{MINUTES:.0f} 分钟）")
    for x in FAIL:
        print("  -", x)
    print("=" * 76)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
