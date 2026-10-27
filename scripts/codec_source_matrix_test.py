#!/usr/bin/env python3
"""反向矩阵：VP8/VP9 作为**源**（RTSP 推入）能否被转码成 H264。

为什么单独测
-----------
「能作为目标」和「能作为源」是两条链：源方向要 RTSP 解包（VP8RtpDecoder 等）
+ ffmpeg 解码 + 目标轨；目标方向要编码器 + RTP 打包。任何一个环节缺都会失败。
ffmpeg 能造 VP8/VP9 的 RTSP 源（有 RTP 打包器），所以这两项可以真机验证；
AV1 没有 RTP 打包器（ffmpeg 侧），造不出源，只能记录为未验证。

用法：env -u PYTHONPATH python3 scripts/codec_source_matrix_test.py
"""
import json, os, re, signal, subprocess, sys, time
from urllib.parse import urlencode
from urllib.request import urlopen

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
CFG = f"{WT}/config.codec_matrix.ini"
HTTP, RTMP, RTSP = 61080, 60900, 60554
LOG = "/tmp/codec_source.log"
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
FF = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffmpeg")
FP = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffprobe")

# 源 codec → (编码参数, 期望解出来的源 codec)
SOURCES = [
    ("vp8", ["-c:v", "libvpx", "-b:v", "500k", "-g", "15"], "vp8"),
    # ffmpeg 的 VP9 RTP 打包器还标着 experimental（草案），必须显式开
    # ffmpeg still marks its VP9 RTP payloader experimental (draft spec)
    ("vp9", ["-c:v", "libvpx-vp9", "-b:v", "500k", "-g", "15", "-deadline", "realtime",
             "-cpu-used", "8", "-lag-in-frames", "0", "-auto-alt-ref", "0",
             "-strict", "experimental"], "vp9"),
]


def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None


def api(**kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/addTranscode?" + urlencode(kw)
    with urlopen(url, timeout=10) as r:
        return json.loads(r.read().decode())


def main():
    ok_all = True
    for src_codec, enc_args, want_src in SOURCES:
        stream = f"src_{src_codec}"
        print(f"== {src_codec} 作为源 → 目标 h264")
        subprocess.run("pkill -9 -f MediaServer; sleep 1.5", shell=True)
        open(LOG, "w").close()
        p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=os.path.dirname(BIN),
                             stdout=open(LOG, "a"), stderr=subprocess.STDOUT,
                             preexec_fn=os.setsid)
        for _ in range(80):
            time.sleep(0.25)
            if lpid(RTSP) == p.pid:
                break
        r = api(vhost="__defaultVhost__", app="live", stream=stream,
                video_codec="h264", enable=1)
        print("  addTranscode:", r.get("code"), r.get("applied"))
        psh = subprocess.Popen([FF, "-re", "-stream_loop", "-1", "-i", "/tmp/av_test.mp4",
                                "-an"] + enc_args +
                               ["-f", "rtsp", "-rtsp_transport", "tcp",
                                f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              preexec_fn=os.setsid)
        time.sleep(7)
        # 源侧：直接看推流器是否被接受（ZLM 的 RTSP 解包器认不认这个 codec）
        r = sh(f"{FP} -v error -rtsp_transport tcp -select_streams v "
               f"-show_entries stream=codec_name,width,height -of csv=p=0 "
               f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=40)
        got = r.stdout.strip()
        print("  拉流端看到:", got or r.stderr.strip()[:120])
        r = sh(f"{FF} -v error -nostats -progress pipe:1 -rtsp_transport tcp "
               f"-i rtsp://127.0.0.1:{RTSP}/live/{stream} -t 5 -an -f null - 2>&1", timeout=60)
        frames = 0
        for line in r.stdout.splitlines():
            if line.startswith("frame="):
                try:
                    frames = max(frames, int(line.split("=")[1]))
                except ValueError:
                    pass
        errors = [l for l in r.stderr.splitlines() if l.strip() and "=" not in l]
        ok = got.startswith("h264") and frames > 40 and not errors
        ok_all = ok_all and ok
        print(f"  结果: codec={got.split(',')[0] if got else None}, 解码帧数={frames}, "
              f"错误={len(errors)} -> {'PASS' if ok else 'FAIL'}")
        try:
            os.killpg(os.getpgid(psh.pid), signal.SIGKILL)
        except Exception:
            pass
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGTERM)
        except Exception:
            pass
        time.sleep(1)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
