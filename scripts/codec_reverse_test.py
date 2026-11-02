#!/usr/bin/env python3
"""反向方向测试：非默认源。

音频矩阵默认用 AAC 源；视频只验了 H264→H265。这个脚本补另一半：
  - 视频：H265 → H264（反向）
  - 音频：G711A 源 → opus/aac、MP3 源 → opus/G711A

判据与 audio_matrix_test.py 一致：属性 + 时长不变量 + 包数自洽 + 0 硬解码错误。

用法：env -u PYTHONPATH python3 scripts/codec_reverse_test.py
端口：RTMP 56900 / RTSP 56554 / HTTP 57080
"""
import json, os, re, signal, subprocess, sys, time

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from _platshim import bin_path as _bin_path, bin_dir as _bin_dir
BIN = _bin_path()
BIN_DIR = _bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.reverse.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
LOG = "/tmp/reverse.log"
PULL = 5.0

PASS, FAIL, KNOWN = [], [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))

def knote(name, detail=""):
    KNOWN.append(name)
    print(f"  KNOWN {name}   {detail}")

def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)

def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None

def make_cfg(audio="", video="", bitrate=0):
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=1", f"audio_codec={audio}", f"video_codec={video}",
                    "on_demand=0", "sample_rate=0", f"bitrate={bitrate}",
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
            return p
    print(open(LOG).read()[-800:]); sys.exit(1)

def stop(p):
    try: os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception: pass
    time.sleep(1.0)

def push(src, stream, via_rtsp=False):
    """H265 源必须走 RTSP 推：FLV 容器承载不了 HEVC，直接推会 404/无流。"""
    if via_rtsp:
        fmt = ["-f", "rtsp", "-rtsp_transport", "tcp",
               f"rtsp://127.0.0.1:{RTSP}/live/{stream}"]
    else:
        fmt = ["-f", "flv", f"rtmp://127.0.0.1:{RTMP}/live/{stream}"]
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy"] + fmt,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)

def props(stream, sel):
    r = sh(f"ffprobe -v error -select_streams {sel} -show_entries "
           f"stream=codec_name,sample_rate,channels,width,height -of json "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=30)
    try: return json.loads(r.stdout)["streams"][0]
    except Exception: return {}

def pull(stream, wav=True):
    out = f"/tmp/rev_{stream}_{'a.wav' if wav else 'v.mp4'}"
    if os.path.exists(out): os.remove(out)
    extra = "-vn -c:a pcm_s16le" if wav else "-an -c copy"
    t0 = time.time()
    sh(f"ffmpeg -y -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
       f"-t {PULL:.0f} {extra} {out}", timeout=120)
    return out, time.time() - t0

def dur(path):
    try: return float(sh(f"ffprobe -v error -show_entries format=duration -of csv=p=0 {path}").stdout.strip())
    except Exception: return 0.0

def packets(stream, sel):
    r = sh(f"ffprobe -v error -select_streams {sel} -read_intervals '%+{PULL:.0f}' -count_packets "
           f"-show_entries stream=nb_read_packets -of csv=p=0 "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=60)
    v = r.stdout.strip().split("\n")[0].strip()
    return int(v) if v.isdigit() else 0

def hard_errors(stream, secs=4):
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
           f"-t {secs} -f null - 2>&1", timeout=secs + 40)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    hard = [l for l in lines if "monoton" not in l]
    return len(hard), hard[:2]

def audio_case(tag, src, want_codec, want_rate, want_ch):
    make_cfg(audio=tag.upper() if tag.isalpha() else tag)
    p = start(tag)
    try:
        pr = push(src, tag)
        time.sleep(5)
        inf = props(tag, "a")
        wav, wall = pull(tag)
        d = dur(wav)
        npk = packets(tag, "a")
        hard, sample = hard_errors(tag)
        note(inf.get("codec_name") == want_codec
             and int(inf.get("sample_rate") or 0) == want_rate
             and int(inf.get("channels") or 0) == want_ch,
             f"{tag}: 输出 {want_codec} {want_rate}Hz {want_ch}ch",
             f"实得 {inf.get('codec_name')} {inf.get('sample_rate')}Hz {inf.get('channels')}ch")
        note(abs(d - PULL) <= 0.5, f"{tag}: 时长 ≈{PULL:.0f}s", f"{d:.3f}s（墙钟 {wall:.2f}s）")
        note(hard == 0, f"{tag}: 硬解码错误 0", f"{hard} {sample}")
        if npk and d:
            note(10 <= d / npk * 1000 <= 140, f"{tag}: 包数与时长自洽",
                 f"{npk} 包 / {d:.2f}s = {d/npk*1000:.1f} ms/包")
        os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
    finally:
        stop(p)

def video_case(tag, src, want_codec, want_wh):
    make_cfg(video=tag.split("_")[0].upper())
    p = start(tag)
    try:
        pr = push(src, tag, via_rtsp=True)
        time.sleep(6)
        inf = props(tag, "v")
        f, wall = pull(tag, wav=False)
        npk = packets(tag, "v")
        hard, sample = hard_errors(tag)
        note(inf.get("codec_name") == want_codec
             and (inf.get("width"), inf.get("height")) == want_wh,
             f"{tag}: 输出 {want_codec} {want_wh[0]}x{want_wh[1]}",
             f"实得 {inf.get('codec_name')} {inf.get('width')}x{inf.get('height')}")
        note(60 <= npk <= 160, f"{tag}: 5 秒包数合理（15fps≈75）", f"{npk} 包")
        note(hard == 0, f"{tag}: 硬解码错误 0", f"{hard} {sample}")
        os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
    finally:
        stop(p)

print("=" * 74)
print("音频反向：非 AAC 源（G711A / MP3）转其它 codec")
print("=" * 74)
audio_case("PCMA", "/tmp/src_g711a.flv", "pcm_alaw", 8000, 1)   # 同类源（对照）
print("  --- G711A 源 -> opus ---")
audio_case("opus", "/tmp/src_g711a.flv", "opus", 48000, 2)
print("  --- G711A 源 -> aac ---")
# FLV 头里没有 G711A 的采样率，ZLM 建的轨按 44100 声明；AAC 目标跟随源轨，
# 所以这里期望 44100/1（要 8000 就得在配置里显式 sample_rate=8000）
audio_case("AAC", "/tmp/src_g711a.flv", "aac", 44100, 1)
print("  --- MP3 源 -> opus ---")
audio_case("opus", "/tmp/src_mp3.flv", "opus", 48000, 2)
print("  --- MP3 源 -> G711A ---")
audio_case("PCMA", "/tmp/src_mp3.flv", "pcm_alaw", 8000, 1)

print()
print("=" * 74)
print("视频反向：H265 源 -> H264")
print("=" * 74)
video_case("H264", "/tmp/src_h265.mp4", "h264", (640, 360))

print()
print("=" * 74)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)} / KNOWN {len(KNOWN)}")
for x in FAIL:
    print("  -", x)
for x in KNOWN:
    print("  [已知缺陷]", x)
print("=" * 74)
sys.exit(0 if not FAIL else 1)
