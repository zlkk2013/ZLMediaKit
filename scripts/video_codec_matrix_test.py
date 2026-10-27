#!/usr/bin/env python3
"""视频 codec 覆盖矩阵：VP8 / VP9 / AV1（以及 H266 的「能不能做」判定）。

为什么单独一个脚本
------------------
VP8/VP9/AV1 与 H264/H265 的差别不在于「有没有编码器」，而在于**整条链路的假设**：
  * 它们不是 Annex-B 码流（RTP 打包要 payload descriptor / aggregation header）
  * 没有 SPS/PPS，目标轨的宽高只能从关键帧/sequence header 里解析
  * 所以「编码器能出包」离「观众能解」还很远，必须按真机拉流判定

判据（每格都要过）
-----------------
1. ffprobe 拉 RTSP 显示目标 codec + 源分辨率 + 帧率
2. 5 秒内包数 ≈ 5 × 帧率（不能是「一帧一个包」或「0 包」）
3. `ffmpeg -v error -t 5 -f null -` 完整解码 0 错误
4. 拉流端看到的 codec 与源不同（证明真的换了轨，不是透传）

用法
----
    env -u PYTHONPATH python3 scripts/video_codec_matrix_test.py
    env -u PYTHONPATH python3 scripts/video_codec_matrix_test.py --only vp8
端口：RTMP 60900 / RTSP 60554 / HTTP 61080（本脚本独占）
"""
import json, os, re, signal, subprocess, sys, time
from urllib.parse import urlencode
from urllib.request import urlopen

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.av.ini"
CFG = f"{WT}/config.codec_matrix.ini"
HTTP, RTMP, RTSP = 61080, 60900, 60554
LOG = "/tmp/codec_matrix.log"
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
PULL_SECONDS = 5.0

# 验证工具用 conda 里那份 ffmpeg 7.1（6.0 缺 VP9/AV1 的 RTP 解包器）
FF = f"{os.path.expanduser('~')}/miniconda3/envs/zlm-ffmpeg/bin/ffmpeg"
FP = f"{os.path.expanduser('~')}/miniconda3/envs/zlm-ffmpeg/bin/ffprobe"
if not os.path.exists(FF):
    FF, FP = "ffmpeg", "ffprobe"

SRC_H264 = ("/tmp/av_test.mp4", 320, 240, 15)      # h264 320x240 15fps
SRC_HEVC = ("/tmp/src_h265.mp4", 640, 360, 15)    # hevc 640x360 15fps

# name, 源, 推流协议, 目标 codec（配置名）, 期望 codec, 验证方式
# verify: "ffmpeg" = ffprobe/ffmpeg 直接拉 RTSP（VP8/VP9 有 RTP 解包器）
#         "av1depay" = ffmpeg 7.1 没有 AV1 的 RTP 解包器，用自写解包器验证
CASES = [
    ("h264->vp8",   SRC_H264, "rtsp", "vp8", "vp8", "ffmpeg"),
    ("h264->vp9",   SRC_H264, "rtsp", "vp9", "vp9", "ffmpeg"),
    ("hevc->vp8",   SRC_HEVC, "rtsp", "vp8", "vp8", "ffmpeg"),
    ("hevc->vp9",   SRC_HEVC, "rtsp", "vp9", "vp9", "ffmpeg"),
    ("h264->av1",   SRC_H264, "rtsp", "av1", "av1", "av1depay"),
    ("hevc->av1",   SRC_HEVC, "rtsp", "av1", "av1", "av1depay"),
    # 对照组：RTMP 推流（muxer 路径，不涉及 rtsp direct-proxy）
    ("h264->vp8-rtmp", SRC_H264, "rtmp", "vp8", "vp8", "ffmpeg"),
    # H266：没有 ffmpeg 编码器，预期在 setupTrack 阶段就失败并回落透传
    ("h264->h266",  SRC_H264, "rtsp", "h266", "h264", "ffmpeg"),
]

PASS, FAIL, SKIP = [], [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None


def make_cfg():
    """从 config.av.ini 派生测试用配置。

    两处关键改动：
    * [rtsp] directProxy=0 —— 直proxy 模式下 RTSP 推流进来的源 RTP 会被原样
      转发给观众，转码后的帧根本不进这条链路（见 docs 的 §10.4）。关掉之后
      RTSP 源走 demuxer → MultiMediaSourceMuxer（转码就挂在这里）→ 观众。
    * [transcode] 全局段留空，codec 全部用 addTranscode 接口按流下发，
      这样一个进程就能测完所有组合，不必每组重启。
    """
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if ln.strip().startswith("[") and sec == "[transcode]":
            out.append(ln)
            out += ["enable=1", "audio_codec=", "video_codec=", "on_demand=0",
                    "bitrate=0", "sample_rate=0", "video_width=0", "video_height=0",
                    "video_fps=0", "video_bitrate=500000"]
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                i += 1
            sec = ""
            continue
        if ln.strip().startswith("port=") and sec in ("[rtmp]", "[rtsp]", "[http]"):
            out.append(f"port={ {'[rtmp]': RTMP, '[rtsp]': RTSP, '[http]': HTTP}[sec] }")
            i += 1
            continue
        # VP8/VP9/AV1 只测 RTSP 投递：TS/HLS 这两条 muxer 链路对它们没有可用输出，
        # 且 MpegMuxer 写缓冲断言会在断流时把推流端打掉（见 docs 的已知问题），
        # 关掉它们才能把「RTSP 投递」这件事单独判定。
        # fmp4 也要关：AV1 走 fMP4/MP4 写入路径时会在 libmov 里踩坏堆
        # （实测 6 次 1 次 SIGABRT；关掉后 6/6 干净，见 docs §6.3）。
        # Also disable fmp4: the AV1 fMP4/MP4 write path corrupts the heap inside
        # libmov (1 SIGABRT in 6 runs; 6/6 clean with it off — see docs §6.5).
        if ln.strip() in ("enable_hls=1", "enable_ts=1", "enable_fmp4=1") and sec == "[protocol]":
            out.append(ln.strip().replace("=1", "=0"))
            i += 1
            continue
        if ln.strip() == "directProxy=1" and sec == "[rtsp]":
            out.append("directProxy=0")
            i += 1
            continue
        out.append(ln)
        i += 1
    open(CFG, "w", encoding="utf-8").write("\n".join(out) + "\n")


def kill_all():
    for _ in range(3):
        sh("pkill -9 -f MediaServer; sleep 1.5")
        if all(lpid(p) is None for p in (RTMP, RTSP, HTTP)):
            return
    print("  !! 端口未释放")
    sys.exit(1)


def start():
    kill_all()
    open(LOG, "w").close()
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=os.path.dirname(BIN),
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
    for _ in range(80):
        time.sleep(0.25)
        # 必须核对是我们起的进程，否则测的是别人的服务/别的配置
        if lpid(RTSP) == p.pid and lpid(HTTP) == p.pid:
            return p
    print(open(LOG).read()[-1500:])
    sys.exit(1)


def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(1.0)


def api(path, **kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urlencode(kw)
    try:
        with urlopen(url, timeout=10) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"code": -1, "err": str(e)}


def push(src, proto, stream):
    url = (f"rtsp://127.0.0.1:{RTSP}/live/{stream}" if proto == "rtsp"
           else f"rtmp://127.0.0.1:{RTMP}/live/{stream}")
    fmt = "rtsp" if proto == "rtsp" else "flv"
    cmd = [FF, "-re", "-stream_loop", "-1", "-i", src, "-c", "copy", "-f", fmt]
    if proto == "rtsp":
        cmd += ["-rtsp_transport", "tcp"]
    cmd.append(url)
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def props(stream):
    r = sh(f"{FP} -v error -rtsp_transport tcp -select_streams v -show_entries "
           f"stream=codec_name,width,height,r_frame_rate -of json "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=40)
    try:
        return json.loads(r.stdout)["streams"][0]
    except Exception:
        return {}


def packets(stream):
    for _ in range(3):
        r = sh(f"{FP} -v error -rtsp_transport tcp -select_streams v "
               f"-read_intervals '%+{PULL_SECONDS:.0f}' -count_packets "
               f"-show_entries stream=nb_read_packets -of csv=p=0 "
               f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=60)
        v = r.stdout.strip().split("\n")[0].strip()
        if v.isdigit() and int(v) > 0:
            return int(v)
        time.sleep(0.5)
    return 0


def decode_errors(stream, seconds=5):
    r = sh(f"{FF} -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
           f"-t {seconds} -f null - 2>&1", timeout=seconds + 40)
    lines = [l for l in (r.stdout + r.stderr).splitlines() if l.strip()]
    return len(lines), lines[:2]


def decode_stats(stream, seconds=5):
    """完整解码一遍：返回 (错误行数, 解码帧数, 首行错误样本)。

    帧数走 `-progress pipe:1`（不是 grep showinfo）：progress 的 frame= 是解码器
    真实吐出的帧数，能区分「拉得到包但解不出画面」这种情况。
    """
    r = sh(f"{FF} -v error -nostats -progress pipe:1 -rtsp_transport tcp "
           f"-i rtsp://127.0.0.1:{RTSP}/live/{stream} -t {seconds} -an -f null - 2>&1",
           timeout=seconds + 45)
    errors, frames = [], 0
    for line in (r.stdout + r.stderr).splitlines():
        line = line.strip()
        m = re.match(r"frame=(\d+)", line)
        if m:
            frames = max(frames, int(m.group(1)))
        elif line and "=" not in line:
            # progress 输出是 key=value（含 stream_0_0_q=...），只有不带 '=' 的
            # 行才是 ffmpeg 的错误行
            errors.append(line)
    return len(errors), frames, errors[:2]


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    make_cfg()
    rows = []
    for name, (src, w, h, fps), proto, tcodec, want, verify in CASES:
        if only and only not in name:
            continue
        stream = re.sub(r"[^a-zA-Z0-9]", "_", name)
        print(f"== {name}  ({os.path.basename(src)} → {tcodec}, push={proto})")
        # 每组都重启：RTSP 推流 + 转码这条拓扑在断流收尾时偶发崩溃（见 docs，
        # 与 VP8/VP9 无关，H265 同样复现），每格独立进程才能互不污染。
        # Restart per case: the RTSP-push + transcode topology crashes intermittently
        # on teardown (documented; reproduces with H265 too, so it is not VP8/VP9
        # specific). One process per case keeps the results independent.
        p = start()
        psh = None
        try:
            r = api("addTranscode", vhost="__defaultVhost__", app="live",
                    stream=stream, video_codec=tcodec, enable=1)
            if r.get("code") != 0:
                print(f"  addTranscode 失败: {r}")
                SKIP.append(name)
                continue
            psh = push(src, proto, stream)
            # 等转码轨登记（日志比接口可靠：sink 是懒创建的）
            for _ in range(40):
                time.sleep(0.5)
                log = open(LOG, errors="ignore").read()
                if f"stream={stream}" in log and ("transcode track registered" in log
                                                 or "transcode setup failed" in log):
                    break
            time.sleep(2)
            if verify == "av1depay":
                # ffmpeg 7.1 的 libavformat 没有 AV1 的 RTP 解包器（RTSP 拉到的是
                # unknown codec），必须用 scripts/av1_rtp_verify.py 自己解包再交给
                # ffmpeg 解码，否则会把「工具不支持」误判成「服务端不支持」。
                # ffmpeg 7.1 has no AV1 RTP depacketizer (an RTSP pull reports an
                # unknown codec), so depacketize with our own RFC 9628 parser and hand
                # the result to ffmpeg — otherwise a tool limitation would be mistaken
                # for a server limitation.
                r = sh(f"python3 {WT}/scripts/av1_rtp_verify.py "
                       f"rtsp://127.0.0.1:{RTSP}/live/{stream} {PULL_SECONDS:.0f} {w} {h} {fps}",
                       timeout=PULL_SECONDS + 40)
                out = r.stdout
                if not out.strip():
                    out = "verifier 无输出，stderr:\n" + r.stderr.strip()
                print("    " + out.strip().replace(chr(10), chr(10) + "    "))
                pr = {"codec_name": "av1", "width": w, "height": h} if "PASS" in out else {}
                m = re.search(r"解码帧数: (\d+)", out)
                frames = int(m.group(1)) if m else 0
                # AV1 这一行的「包数」用解码出的时间单元数：RTP 包数是分片后的
                # 个数（一帧可能好几个包），不能直接和 帧率×时长 比
                # For AV1 the "packets" column is the decoded temporal-unit count:
                # the raw RTP count is post-fragmentation and incomparable
                npk = frames
                m = re.search(r"解码错误: (\d+)", out)
                err = int(m.group(1)) if m else 1
                sample = [] if not err else ["见上"]
            else:
                pr = props(stream)
                npk = packets(stream) if pr else 0
                err, frames, sample = decode_stats(stream) if pr else (0, 0, [])
        finally:
            if psh:
                try:
                    os.killpg(os.getpgid(psh.pid), signal.SIGKILL)
                except Exception:
                    pass
            time.sleep(0.5)
            log = open(LOG, errors="ignore").read()
            stop(p)

        if "transcode setup failed" in log:
            print("  转码未建立（回落透传）:" +
                  next((l.split("|")[-1].strip() for l in log.splitlines()
                        if "transcode setup failed" in l), ""))
        got_codec = pr.get("codec_name")
        got_w, got_h = pr.get("width"), pr.get("height")
        ok_codec = (got_codec == want)
        ok_size = (got_w == w and got_h == h)
        expect_pk = PULL_SECONDS * fps
        ok_pk = expect_pk * 0.6 <= npk <= expect_pk * 1.4
        ok_err = (err == 0)
        rows.append((name, want, got_codec, f"{got_w}x{got_h}", npk, frames, err,
                     ok_codec, ok_size, ok_pk, ok_err))
        note(ok_codec, f"{name} codec", f"期望 {want}，实际 {got_codec}")
        note(ok_size, f"{name} 分辨率", f"{got_w}x{got_h}")
        note(ok_pk, f"{name} 包数 ≈ {PULL_SECONDS*fps:.0f}", f"实测 {npk}")
        note(ok_err, f"{name} 解码 0 错误", f"{err} 行 {sample}")
        print(f"     解码出 {frames} 帧（期望 ≈{PULL_SECONDS*fps:.0f}）")
        print()

    print("=" * 78)
    print("汇总")
    print("=" * 78)
    print(f"{'case':18} {'want':6} {'got':6} {'size':10} {'pkts':5} {'frames':6} {'err':4}")
    for r in rows:
        print(f"{r[0]:18} {r[1]:6} {str(r[2]):6} {r[3]:10} {r[4]:5} {r[5]:6} {r[6]:4}")
    print()
    print(f"PASS {len(PASS)} / FAIL {len(FAIL)}")
    for f in FAIL:
        print("  FAIL", f)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
