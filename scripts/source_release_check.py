#!/usr/bin/env python3
"""媒体源释放检查：推流端断开后，本流发布的转协议源必须被摘掉。

为什么需要：`MultiMediaSourceMuxer` 存在引用环
`muxer → TranscodeSink → 编解码器 → 源 Track →(FrameDispatcher 存 shared_ptr)→ muxer`。
环一旦成立，推流端断开后 muxer 和它发布的 rtsp/rtmp/ts/fmp4/hls 源会永远留在媒体源池里：
  - 同名流重推时 `regist()` 报 media source already existed（摄像机再也上不来）
  - `getMediaList` 里一直挂着几条永远不更新的「在线」流
  - 长跑内存/句柄缓慢上涨

这个过程**不报错、不崩溃**，只能靠对比断开前后的媒体列表发现。

判据：
  - 推流中：该流有若干条转协议源（rtsp/rtmp/ts/hls/fmp4）
  - 断开 + 超过宽限期（默认 15s，这里等 20s）：列表必须为空
  - 带观众断开（观众正在看）也要一样清空

用法：env -u PYTHONPATH python3 scripts/source_release_check.py
端口：RTMP 56900 / RTSP 56554 / HTTP 57080
"""
import json, os, signal, subprocess, sys, time, urllib.parse, urllib.request

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from _platshim import bin_path as _bin_path, bin_dir as _bin_dir
BIN = _bin_path()
BIN_DIR = _bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.release.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
LOG = "/tmp/release_check.log"
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
GRACE_WAIT = 20

PASS, FAIL = [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))

def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)

def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None

def api(path, **kw):
    q = urllib.parse.urlencode({"secret": SECRET, **kw})
    try:
        return json.loads(urllib.request.urlopen(
            f"http://127.0.0.1:{HTTP}/index/api/{path}?{q}", timeout=5).read())
    except Exception as e:
        return {"_e": str(e)}

def sources(stream):
    d = api("getMediaList")
    return sorted({x.get("schema") for x in (d.get("data") or []) if x.get("stream") == stream})

def make_cfg(enable=1, video="H265"):
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += [f"enable={enable}", "audio_codec=", f"video_codec={video}",
                    "on_demand=0", "sample_rate=0", "bitrate=0",
                    "video_width=0", "video_height=0", "video_fps=0", "video_bitrate=0"]
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

def run_case(tag, with_viewers):
    print(f"--- {tag}（{'带观众' if with_viewers else '无观众'}断开）---")
    make_cfg(1, "H265")
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}"); time.sleep(0.6)
    srv = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=BIN_DIR,
                           stdout=open(LOG, "w"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if srv.poll() is None and lpid(RTMP) == srv.pid:
            break
    viewers = []
    try:
        pr = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", "/tmp/av_test.mp4",
                               "-c", "copy", "-f", "flv", f"rtmp://127.0.0.1:{RTMP}/live/{tag}"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              preexec_fn=os.setsid)
        time.sleep(6)
        if with_viewers:
            viewers = [subprocess.Popen(
                ["ffmpeg", "-y", "-v", "error", "-rtsp_transport", "tcp",
                 "-i", f"rtsp://127.0.0.1:{RTSP}/live/{tag}", "-t", "30",
                 "-c", "copy", f"/tmp/release_v{i}.mp4"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
                for i in range(2)]
            time.sleep(3)

        before = sources(tag)
        note(len(before) >= 3, "推流中已发布多条转协议源", f"{before}")

        try: os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
        except Exception: pass
        time.sleep(GRACE_WAIT)

        after = sources(tag)
        note(after == [], f"断开 {GRACE_WAIT}s 后源全部摘掉（不泄漏）", f"{after}")

        # 断开后同名流必须还能重推（泄漏的典型后果是 regist 冲突）
        pr2 = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", "/tmp/av_test.mp4",
                                "-c", "copy", "-f", "flv", f"rtmp://127.0.0.1:{RTMP}/live/{tag}"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               preexec_fn=os.setsid)
        time.sleep(5)
        note(pr2.poll() is None and sources(tag) != [], "断开后同名流能重推",
             f"存活={pr2.poll() is None} 源={sources(tag)}")
        try: os.killpg(os.getpgid(pr2.pid), signal.SIGKILL)
        except Exception: pass
    finally:
        for v in viewers:
            try: os.killpg(os.getpgid(v.pid), signal.SIGKILL)
            except Exception: pass
        try: os.killpg(os.getpgid(srv.pid), signal.SIGTERM)
        except Exception: pass
        time.sleep(1)

print("=" * 74)
print("媒体源释放检查（转码开启，RTMP 推流）")
print("=" * 74)
run_case("rel_nv", False)
run_case("rel_wv", True)

print()
print("=" * 74)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
for x in FAIL:
    print("  -", x)
print("=" * 74)
sys.exit(0 if not FAIL else 1)
