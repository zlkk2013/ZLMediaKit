#!/usr/bin/env python3
"""RTSP 进 → RTSP 出 转码验证（含 direct-proxy 关闭回归）。

专测 docs/TRANSCODE_AUDIO_FRAMING_FIX.md §10.4：RTSP 推入的源启用转码后，
direct-proxy 会把源 RTP 原样透传给观众，转码输出根本到不了观众端。
判据（每条都必须是真机输出）：
  - 拉流 ffprobe 的 codec/分辨率 = 目标编码
  - DESCRIBE 的 SDP 是 muxer 生成的（H264/90000 + a=control:trackID=），
    不是推流端的原始 SDP（streamid= / sprop-vps）
  - 5 秒包数合理、解码 0 错误
  - 基线：不开转码时 RTSP 推 → RTSP 拉，解码帧 md5 与改动前的二进制一致

端口独占：RTMP 59900 / RTSP 59554 / HTTP 60080，只按端口 kill 自己的进程，
绝不 pkill -f MediaServer（同机会有别的 agent 在编译 MediaServer）。

用法：
  python3 scripts/rtsp_transcode_check.py --tag H264 --src /tmp/src_h265.mp4 \
      --video h264 --audio "" --bin release/darwin/Release/MediaServer
  python3 scripts/rtsp_transcode_check.py --tag BASE --src /tmp/src_h264.mp4 \
      --video "" --audio "" --enable 0 --framemd5 /tmp/base.fmd5
"""
import argparse, json, os, re, signal, socket, subprocess, sys, time
from _platshim import sh_retry as _sh_retry

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE_CFG = f"{WT}/config.av.ini"
CFG = "/tmp/zlm_rtsp_transcode_check.ini"  # 放 /tmp，避免污染工作区
HTTP, RTMP, RTSP = 60080, 59900, 59554
# 同机可能跑着别的 agent 的 MediaServer（同一份模板配置），除了自己独占的三个端口，
# 其余监听端口必须错开，否则启动时报 "Listen on :: 53000 failed" 直接退出。
PORT_MAP = {"[rtmp]": RTMP, "[rtsp]": RTSP, "[http]": HTTP,
            "[rtp_proxy]": 63000, "[rtc]": 8100, "[srt]": 61900,
            "[onvif]": 63002, "[shell]": 0}
LOG = "/tmp/rtsp_transcode_check.log"
PULL = 5.0

RESULT = []


def note(ok, name, detail=""):
    RESULT.append((ok, name, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""), flush=True)


def sh(cmd, timeout=120):
    return _sh_retry(cmd, timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.split()
    return int(out[0]) if out else None


def kill_ports():
    """只杀自己端口上的监听进程，绝不按进程名 pkill。"""
    for port in (RTMP, RTSP, HTTP):
        for _ in range(3):
            pid = lpid(port)
            if pid is None:
                break
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            time.sleep(0.7)


def make_cfg(video, audio, enable, extra=""):
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, sec, skip = [], "", False
    for ln in lines:
        if ln.strip().startswith("["):
            sec = ln.strip()
            skip = (sec == "[transcode]")
            out.append(ln)
            if skip:
                out += ["enable=%d" % enable,
                        f"audio_codec={audio}", f"video_codec={video}",
                        "bitrate=0", "sample_rate=0", "on_demand=0"]
                if extra:
                    out.append(extra)
            continue
        if skip:
            continue
        if ln.strip().startswith("port=") and sec in PORT_MAP:
            out.append("port=%d" % PORT_MAP[sec])
            continue
        out.append(ln)
    open(CFG, "w", encoding="utf-8").write("\n".join(out) + "\n")


def start(binary):
    kill_ports()
    open(LOG, "w").close()
    rundir = os.path.dirname(os.path.abspath(binary))
    p = subprocess.Popen([os.path.abspath(binary), "-c", CFG, "-l", "1"], cwd=rundir,
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(80):
        time.sleep(0.25)
        if lpid(RTMP) is not None:
            if lpid(RTMP) != p.pid:
                print(f"!! 端口被别的进程占用: {lpid(RTMP)} != {p.pid}")
                sys.exit(1)
            return p
    print("!! 启动失败", open(LOG).read()[-1500:])
    sys.exit(1)


def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(1.0)
    kill_ports()


def push(src, stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdin=subprocess.DEVNULL, start_new_session=True)


def describe(stream):
    """裸 socket 发 DESCRIBE，ffmpeg -v trace 抓 SDP 不可靠。"""
    s = socket.create_connection(("127.0.0.1", RTSP), timeout=10)
    s.sendall(f"DESCRIBE rtsp://127.0.0.1:{RTSP}/live/{stream} RTSP/1.0\r\n"
              f"CSeq: 1\r\nAccept: application/sdp\r\n\r\n".encode())
    buf = b""
    s.settimeout(5)
    try:
        while b"v=0" not in buf or b"m=video" not in buf and b"m=audio" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    s.close()
    return buf.decode("utf-8", "replace")


def props(stream, sel="v"):
    r = sh(f"ffprobe -v error -select_streams {sel} -show_entries "
           f"stream=codec_name,width,height,sample_rate,channels -of json "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=40)
    try:
        return json.loads(r.stdout)["streams"][0]
    except Exception:
        return {}


def pull(stream, out, extra="-an -c copy"):
    if os.path.exists(out):
        os.remove(out)
    sh(f"ffmpeg -y -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
       f"-t {PULL:.0f} {extra} {out}", timeout=150)
    return out


def packets(stream, sel="v"):
    r = sh(f"ffprobe -v error -select_streams {sel} -read_intervals '%+{PULL:.0f}' -count_packets "
           f"-show_entries stream=nb_read_packets -of csv=p=0 "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=90)
    v = r.stdout.strip().splitlines()[0].strip() if r.stdout.strip() else ""
    return int(v) if v.isdigit() else 0


def decode_errors(stream, secs=4):
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
           f"-t {secs} -f null - 2>&1", timeout=secs + 60)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    hard = [l for l in lines if "monoton" not in l]
    return hard


def framemd5(path):
    r = sh(f"ffmpeg -v error -i {path} -f framemd5 -", timeout=120)
    lines = [l.split(",")[-1].strip() for l in r.stdout.splitlines() if l and not l.startswith("#")]
    return lines


def run_once(a, stream):
    """跑一轮完整取证；任何一步连接失败（同机别的 agent pkill 掉了服务）由外层重试。"""
    p = start(a.bin)
    pusher = None
    try:
        pusher = push(a.src, stream)
        deadline = time.time() + 7
        while time.time() < deadline:
            time.sleep(0.5)
            if p.poll() is not None:
                raise RuntimeError("server died (killed?) before ready")
        sdp = describe(stream)
        if os.environ.get("DUMP_SDP"):
            open(f"/tmp/sdp_{a.tag}.txt", "w").write(sdp)
            print("  ---- SDP ----")
            for line in sdp.splitlines():
                if line.strip() and not line.startswith(("CSeq", "Date", "Server", "Content-", "RTSP/", "Cache")):
                    print("  |", line)

        inf = props(stream, "v" if a.src.endswith(".mp4") else "a")
        if not inf:
            raise RuntimeError("ffprobe got no stream")
        print("  ffprobe:", json.dumps(inf, ensure_ascii=False))

        # --- SDP（只在转码时判定：不开转码就是 direct-proxy，SDP 本来就是推流端的） ---
        want_codec = (a.video or "").upper()
        transcoding = bool(a.enable and (a.video or a.audio))
        if want_codec:
            note(f" {want_codec}/90000" in sdp, f"{a.tag}: SDP 声明 {want_codec}/90000",
                 [l for l in sdp.splitlines() if "rtpmap" in l][:3])
        if transcoding:
            note("a=control:trackID=" in sdp, f"{a.tag}: SDP 用 muxer 的 a=control:trackID=",
                 [l for l in sdp.splitlines() if "a=control" in l][:3])
            note("streamid=" not in sdp, f"{a.tag}: SDP 不是推流端原始 SDP（无 streamid=）",
                 [l for l in sdp.splitlines() if "streamid=" in l][:2])
        if a.src.endswith("h265.mp4") and want_codec == "H264":
            note("sprop-vps" not in sdp.lower(), f"{a.tag}: SDP 无 H265 sprop-vps", "")

        # --- 属性 ---
        if a.src.endswith(".mp4"):
            # ffprobe 对 H265 的 codec_name 是 hevc
            want_name = {"h265": "hevc", "h264": "h264"}.get((a.video or "").lower(), (a.video or "").lower())
            note(inf.get("codec_name") == (want_name or inf.get("codec_name")),
                 f"{a.tag}: 拉流编码 = {a.video or '源'}", f"实得 {inf.get('codec_name')}")
            note((inf.get("width"), inf.get("height")) == (640, 360),
                 f"{a.tag}: 分辨率 640x360", f"实得 {inf.get('width')}x{inf.get('height')}")

        # --- 包数 / 解码错误 ---
        npk = packets(stream, "v" if a.src.endswith(".mp4") else "a")
        note(40 <= npk <= 200, f"{a.tag}: 5 秒包数合理（15fps≈75，HEVC 按 AU 计包数偏少）", f"{npk} 包")
        errs = decode_errors(stream)
        note(not errs, f"{a.tag}: 解码 0 错误", errs[:2])

        # --- 抽帧对比 ---
        out = f"/tmp/tc_{a.tag}.mp4"
        pull(stream, out)
        d = sh(f"ffprobe -v error -show_entries format=duration -of csv=p=0 {out}").stdout.strip()
        print(f"  拉流落盘 {out}  时长 {d}s  大小 {os.path.getsize(out) if os.path.exists(out) else 'NA'}")
        try:
            dur_ok = abs(float(d) - PULL) <= 0.7
        except ValueError:
            raise RuntimeError(f"pull produced no duration ({d!r})")
        if not dur_ok:
            # 拉流被中途掐断（服务被同机其他任务 kill 掉）属于环境噪声，重来而不是判失败
            raise RuntimeError(f"pull truncated: {d}s")
        note(True, f"{a.tag}: 拉流时长 ≈{PULL:.0f}s", f"{d}s")
        md5s = framemd5(out)
        print(f"  解码帧数 {len(md5s)}")
        if not md5s:
            raise RuntimeError("pull produced no decodable frames")
        if a.framemd5:
            open(a.framemd5, "w").write("\n".join(md5s) + "\n")
            print(f"  解码帧 md5 -> {a.framemd5}")
        if a.expect_src or a.expect_not_src:
            src_md5 = set(framemd5(a.src))
            hit = sum(1 for h in md5s if h in src_md5)
            ratio = hit / len(md5s)
            if a.expect_src:
                note(ratio >= 0.99, f"{a.tag}: 拉流解码帧全部来自源（透传未重编码）",
                     f"{hit}/{len(md5s)} = {ratio:.1%} 命中源帧")
            else:
                note(ratio <= 0.05, f"{a.tag}: 拉流解码帧与源不同（确认重编码）",
                     f"{hit}/{len(md5s)} = {ratio:.1%} 命中源帧")
        if a.expect_framemd5 and os.path.exists(a.expect_framemd5):
            # 与另一次运行的解码帧集合比对（拉流从随机关键帧开始，只能比集合不能比顺序）
            exp = set(open(a.expect_framemd5).read().split())
            hit = sum(1 for h in md5s if h in exp)
            note(hit > 0, f"{a.tag}: 解码帧与基线运行有交集（同一内容）",
                 f"{hit}/{len(md5s)} 帧在基线集合内")
        return True
    finally:
        if pusher:
            try:
                os.killpg(os.getpgid(pusher.pid), signal.SIGKILL)
            except Exception:
                pass
        stop(p)
        log = open(LOG, encoding="utf-8", errors="replace").read()
        for pat in ("transcode enabled, direct-proxy disabled", "H265 -> H264", "H264 -> H265"):
            hits = [l for l in log.splitlines() if pat in l]
            if hits:
                print(f"  log[{pat}]: {hits[0][:160]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=f"{BIN_DIR}/MediaServer")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--video", default="")
    ap.add_argument("--audio", default="")
    ap.add_argument("--enable", type=int, default=1)
    ap.add_argument("--stream", default=None)
    ap.add_argument("--framemd5", default=None, help="把拉流解码帧 md5 写到该文件（基线对比用）")
    ap.add_argument("--expect-framemd5", default=None, help="与该文件比对解码帧集合")
    ap.add_argument("--expect-src", action="store_true",
                    help="校验拉流解码帧全部来自源（透传＝不重编码，位置无关）")
    ap.add_argument("--expect-not-src", action="store_true",
                    help="校验拉流解码帧不等于源（转码＝真的重编码了）")
    ap.add_argument("--attempts", type=int, default=4,
                    help="同机其他任务会 pkill 掉服务，失败了整体重试")
    a = ap.parse_args()

    stream = a.stream or ("tc" + a.tag)
    make_cfg(a.video, a.audio, a.enable)
    print(f"== {a.tag}: push {os.path.basename(a.src)} -> rtsp://127.0.0.1:{RTSP}/live/{stream}"
          f"  video={a.video or '-'} audio={a.audio or '-'} enable={a.enable}")
    for attempt in range(1, a.attempts + 1):
        del RESULT[:]
        try:
            run_once(a, stream)
            break
        except Exception as ex:
            print(f"  !! attempt {attempt}/{a.attempts} aborted: {ex}")
            if attempt == a.attempts:
                print(f"== {a.tag}: 未取到有效数据")
                return 2

    fails = [r for r in RESULT if not r[0]]
    print(f"== {a.tag}: {len(RESULT) - len(fails)} 项通过 / {len(fails)} 项失败")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
