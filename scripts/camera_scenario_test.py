#!/usr/bin/env python3
"""现场场景验收：RTSP 摄像机推流 -> 转码 -> 观众拉流。

这是真实部署形态（摄像机 RTSP 推、平台转码、业务端 RTSP 拉），
和 `codec_reverse_test.py` 的区别是：源一律走 **RTSP** 推，而不是 RTMP。
H264 经 RTMP 推再转 H265 一直是通的，会掩盖「RTSP 源 + direct-proxy」这条路的缺陷。

判据：
  1. DESCRIBE 的 SDP 必须是**转码后**的编码，且 control 是 muxer 风格（trackID=）
  2. 拉流 ffprobe 编码/分辨率正确
  3. 完整解码 0 硬错误（按 hevc/h264 各自解，解不了会直接报错）
  4. 两个观众同时拉，互不影响

用法：env -u PYTHONPATH python3 scripts/camera_scenario_test.py
端口：RTMP 56900 / RTSP 56554 / HTTP 57080（与 joint_accept 一致）
"""
import json, os, re, signal, socket, subprocess, sys, time

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from _platshim import bin_path as _bin_path, bin_dir as _bin_dir
BIN = _bin_path()
BIN_DIR = _bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.camera.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
LOG = "/tmp/camera.log"

PASS, FAIL = [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))

def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)

def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None

def make_cfg(audio="", video="", extra=""):
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=1", f"audio_codec={audio}", f"video_codec={video}",
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

def kill_all():
    for _ in range(3):
        sh("pkill -9 -f MediaServer; sleep 1.5")
        if all(lpid(p) is None for p in (RTMP, RTSP, HTTP)):
            return
    print("  !! 端口未释放"); sys.exit(1)

def start(tag):
    kill_all()
    open(LOG, "w").close()
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=BIN_DIR,
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if lpid(RTMP) == p.pid:
            print(f"  服务已起（{tag}），pid={p.pid}")
            return p
    print(open(LOG).read()[-800:]); sys.exit(1)

def stop(p):
    try: os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception: pass
    time.sleep(1.0)

def push_rtsp(src, stream):
    """摄像机形态：RTSP 推流。"""
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)

def describe(stream):
    s = socket.create_connection(("127.0.0.1", RTSP), timeout=10)
    s.sendall(f"DESCRIBE rtsp://127.0.0.1:{RTSP}/live/{stream} RTSP/1.0\r\n"
              f"CSeq: 1\r\nAccept: application/sdp\r\n\r\n".encode())
    time.sleep(1.2)
    d = s.recv(65535).decode("utf-8", "replace")
    s.close()
    return d

def sdp_codecs(sdp):
    """从 SDP 里取 (video codec 名, 是否 muxer 风格的 control)。"""
    vcodec, muxer_style = None, False
    for ln in sdp.splitlines():
        if ln.startswith("a=rtpmap:") and re.search(r"/90000", ln):
            vcodec = ln.split()[1].split("/")[0]
        if ln.startswith("a=control:trackID="):
            muxer_style = True
    return vcodec, muxer_style

def props(stream, sel="v"):
    r = sh(f"ffprobe -v error -select_streams {sel} -show_entries "
           f"stream=codec_name,width,height -of json rtsp://127.0.0.1:{RTSP}/live/{stream}",
           timeout=30)
    try: return json.loads(r.stdout)["streams"][0]
    except Exception: return {}

def packets(stream, secs=5):
    for _ in range(3):
        r = sh(f"ffprobe -v error -select_streams v -read_intervals '%+{secs}' -count_packets "
               f"-show_entries stream=nb_read_packets -of csv=p=0 "
               f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=60)
        v = r.stdout.strip().split("\n")[0].strip()
        if v.isdigit() and int(v) > 0:
            return int(v)
        time.sleep(0.6)
    return 0

def hard_errors(stream, secs=4):
    """拉流解码并数硬错误。宿主机被压满时拉流会饿死到超时 —— 重试一次，
    仍超时则返回一条可读的失败而不是抛 traceback（实测踩过：整轮回归因此判红，
    日志里只有 TimeoutExpired，看不出是哪条断言）。

    Pull and count hard errors. Retry once on timeout; if it still times out report a
    readable failure instead of raising — a traceback hides which assertion failed.
    """
    r = None
    for attempt in range(2):
        try:
            r = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
                   f"-t {secs} -f null - 2>&1", timeout=secs + 40)
            break
        except subprocess.TimeoutExpired:
            print(f"    (拉流超时，重试 {attempt + 1}/2，负载 {os.getloadavg()[0]:.2f})")
    if r is None:
        return 1, [f"拉流 {secs}s 两次都超时（负载 {os.getloadavg()[0]:.2f}）"]
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    hard = [l for l in lines if "monoton" not in l]
    return len(hard), hard[:2]

def case(tag, src, want_codec, want_wh, audio="", video=""):
    print(f"--- {tag} ---")
    make_cfg(audio=audio, video=video)
    p = start(tag)
    try:
        pr = push_rtsp(src, tag)
        time.sleep(7)

        sdp = describe(tag)
        sc, muxer_style = sdp_codecs(sdp)
        # SDP 里用的是 rtpmap 名（H264/H265），ffprobe 用 h264/hevc，不能直接比
        want_sdp = {"hevc": "h265", "h264": "h264"}.get(want_codec, want_codec)
        note(sc is not None and sc.lower() == want_sdp,
             f"SDP 声明的视频编码 = {sc}", f"期望 rtpmap {want_sdp}")

        inf = props(tag, "v")
        ok_wh = (inf.get("width"), inf.get("height")) == want_wh
        note(inf.get("codec_name") == want_codec and ok_wh,
             f"拉流 = {want_codec} {want_wh[0]}x{want_wh[1]}",
             f"实得 {inf.get('codec_name')} {inf.get('width')}x{inf.get('height')}")

        hard, sample = hard_errors(tag)
        note(hard == 0, "完整解码 0 硬错误", f"{hard} {sample}")

        n1 = packets(tag)
        note(40 <= n1 <= 200, "5 秒视频包数合理（15fps≈75）", f"{n1}")

        # 两个观众同时拉
        viewers = [subprocess.Popen(
            ["ffmpeg", "-y", "-v", "error", "-rtsp_transport", "tcp",
             "-i", f"rtsp://127.0.0.1:{RTSP}/live/{tag}", "-t", "4",
             "-an", "-c", "copy", f"/tmp/cam_{tag}_{i}.mp4"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
            for i in range(2)]
        for v in viewers:
            try: v.wait(timeout=25)
            except Exception: pass
        sizes = [os.path.getsize(f"/tmp/cam_{tag}_{i}.mp4")
                 if os.path.exists(f"/tmp/cam_{tag}_{i}.mp4") else 0 for i in range(2)]
        note(all(s > 10000 for s in sizes), "两个观众同时拉都有数据", f"{sizes}")
        os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
    finally:
        stop(p)

print("=" * 74)
print("现场场景：RTSP 摄像机推流 -> 转码 -> 观众 RTSP 拉流")
print("=" * 74)
case("cam_h265_to_h264", "/tmp/src_h265.mp4", "h264", (640, 360), video="H264")
case("cam_h264_to_h265", "/tmp/av_test.mp4", "hevc", (320, 240), video="H265")
case("cam_h265_to_h265_passthrough", "/tmp/src_h265.mp4", "hevc", (640, 360), video="")

print()
print("=" * 74)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
for x in FAIL:
    print("  -", x)
print("=" * 74)
sys.exit(0 if not FAIL else 1)
