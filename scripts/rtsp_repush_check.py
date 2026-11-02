#!/usr/bin/env python3
"""RTSP 推流断开重连验收（转码开/关都要过）。

现场最痛的一条：启用转码后，RTSP 推流端断开再重连（重新 ANNOUNCE）拿到 406
`Already publishing`，摄像机立刻退出、再也不上来。本脚本把这条路径写成可复现用例：

  推 #1 → DESCRIBE/ffprobe 验证编码 → 挂一个跨断点的观众 →
  SIGKILL 推流端 → 等 GAP 秒 → 推 #2 → **把推 #2 的 ffmpeg 实际输出与返回值贴出来** →
  再 DESCRIBE/ffprobe 验证转码依旧生效 → 收尾看跨断点观众有没有恢复。

判据（全部来自真机输出）：
  1. 推 #2 在 N 秒后仍存活（没被 406 踢掉），stderr 里没有 406 / Already publishing
  2. 重连后 SDP 仍是 muxer 的（`a=control:trackID=`），编码 = 目标编码
  3. 重连后 ffprobe 编码/分辨率 = 目标编码，解码 0 硬错误，包数随时间正常
  4. 跨断点观众：录像时长/帧数覆盖断点前后（或明确记录它断了）

端口独占：RTMP 62900 / RTSP 62554 / HTTP 63080。只按端口 kill 自己的进程，
绝不 `pkill -f MediaServer`（同机别的 agent 也在编译/跑 MediaServer）。

用法：
  python3 scripts/rtsp_repush_check.py --tag h265_to_h264 --src /tmp/src_h265.mp4 --video H264
  python3 scripts/rtsp_repush_check.py --tag base_h264 --src /tmp/src_h264.mp4 --enable 0
"""
import argparse, json, os, re, signal, socket, subprocess, sys, time

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE_CFG = f"{WT}/config.av.ini"
CFG = "/tmp/zlm_repush_check.ini"
HTTP, RTMP, RTSP = 63080, 62900, 62554
# 同机可能有别的 MediaServer 占着默认的 rtp_proxy/rtc/srt/onvif 端口，全部错开
PORT_MAP = {"[rtp_proxy]": 63100, "[rtc]": 8411, "[srt]": 63101, "[onvif]": 37020}
LOG = "/tmp/repush_check.log"

RESULT = []


def note(ok, name, detail=""):
    RESULT.append((ok, name, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""), flush=True)


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


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


def make_cfg(video, audio, enable):
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
    assert lpid(RTMP) is None, "62900 还被占着"
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
            print(f"  服务已起 pid={p.pid}（62900 确认是本进程）")
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


def spawn_push(src, stream, errfile):
    """返回 (Popen, stderr 文件路径)。stderr 落盘，用来贴 ffmpeg 的实际输出。"""
    f = open(errfile, "w")
    p = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy",
                          "-f", "rtsp", "-rtsp_transport", "tcp",
                          f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                         stdout=subprocess.DEVNULL, stderr=f,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    return p, errfile


def describe(stream):
    s = socket.create_connection(("127.0.0.1", RTSP), timeout=10)
    s.sendall(f"DESCRIBE rtsp://127.0.0.1:{RTSP}/live/{stream} RTSP/1.0\r\n"
              f"CSeq: 1\r\nAccept: application/sdp\r\n\r\n".encode())
    buf = b""
    s.settimeout(5)
    try:
        while b"m=video" not in buf and b"m=audio" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    s.close()
    return buf.decode("utf-8", "replace")


def sdp_vcodec(sdp):
    vcodec = None
    for ln in sdp.splitlines():
        if ln.startswith("a=rtpmap:") and "/90000" in ln and "audio" not in ln:
            name = ln.split()[1].split("/")[0]
            if name.upper() not in ("MPEG4-GENERIC", "OPUS", "PCMU", "PCMA", "MPEG4GENERIC"):
                vcodec = name
    return vcodec


def props(stream, sel="v"):
    r = sh(f"ffprobe -v error -select_streams {sel} -show_entries "
           f"stream=codec_name,width,height -of json "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=40)
    try:
        return json.loads(r.stdout)["streams"][0]
    except Exception:
        return {}


def packets(stream, secs=5, sel="v"):
    r = sh(f"ffprobe -v error -select_streams {sel} -read_intervals '%+{secs}' -count_packets "
           f"-show_entries stream=nb_read_packets -of csv=p=0 "
           f"rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=secs + 60)
    v = r.stdout.strip().splitlines()[0].strip() if r.stdout.strip() else ""
    return int(v) if v.isdigit() else 0


def decode_errors(stream, secs=4):
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
           f"-t {secs} -f null - 2>&1", timeout=secs + 60)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    return [l for l in lines if "monoton" not in l]


def err_tail(path, n=12):
    try:
        lines = [l for l in open(path, encoding="utf-8", errors="replace").read().splitlines() if l.strip()]
    except FileNotFoundError:
        return ["(no stderr)"]
    return lines[-n:]


WANT_NAME = {"h265": "hevc", "h264": "h264", "vp8": "vp8", "vp9": "vp9", "av1": "av1"}


def check_pipeline(a, stream, when):
    """DESCRIBE + ffprobe + 包数 + 解码错误，全部贴实测值。"""
    sdp = describe(stream)
    codec = sdp_vcodec(sdp)
    want_sdp = WANT_NAME.get((a.video or "").lower(), (a.video or "").lower()) or None
    want_sdp = {"hevc": "H265", "h264": "H264", "vp8": "VP8", "vp9": "VP9", "av1": "AV1"}.get(want_sdp, want_sdp)
    print(f"  [{when}] SDP rtpmap video = {codec}, control = "
          f"{[l for l in sdp.splitlines() if 'a=control' in l][:2]}")
    if a.enable and a.video:
        note(bool(codec) and codec.upper() == (want_sdp or "").upper(),
             f"{when}: SDP 声明 {want_sdp}", f"实得 {codec}")
        note("a=control:trackID=" in sdp, f"{when}: SDP 是 muxer 生成的（trackID=）",
             [l for l in sdp.splitlines() if "a=control" in l][:2])

    inf = props(stream, "v")
    print(f"  [{when}] ffprobe: {json.dumps(inf, ensure_ascii=False)}")
    if a.enable and a.video:
        want_name = WANT_NAME.get(a.video.lower(), a.video.lower())
        note(inf.get("codec_name") == want_name, f"{when}: 拉流编码 = {a.video}",
             f"实得 {inf.get('codec_name')}")
        note((inf.get("width"), inf.get("height")) == (640, 360),
             f"{when}: 分辨率 640x360", f"实得 {inf.get('width')}x{inf.get('height')}")
    else:
        src_name = sh(f"ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 {a.src}").stdout.strip()
        note(inf.get("codec_name") == src_name, f"{when}: 拉流编码 = 源编码（未转码）",
             f"实得 {inf.get('codec_name')} / 源 {src_name}")

    npk = packets(stream, 5)
    note(40 <= npk <= 200, f"{when}: 5 秒包数合理（15fps≈75）", f"{npk} 包")
    errs = decode_errors(stream, 4)
    note(not errs, f"{when}: 解码 0 硬错误", errs[:2])
    return inf


def run_once(a, stream):
    p = start(a.bin)
    push1 = push2 = viewer = None
    try:
        # ---------- 推 #1 ----------
        push1, e1 = spawn_push(a.src, stream, "/tmp/push1.err")
        time.sleep(6)
        alive = push1.poll() is None
        note(alive, "推 #1 存活", f"rc={push1.poll()}")
        if not alive:
            print("  push1 stderr:", err_tail(e1))
        check_pipeline(a, stream, "推#1")

        # ---------- 跨断点观众（在断点前 2 秒挂上，验证观众侧不断流/自动恢复） ----------
        span_secs = 2 + a.gap + 10
        # 容器要跟着目标编码走：mp4 装不了 VP8/VP9，观众端会直接报错退出，
        # 那不是服务端断流，别记成修复失败
        # The container must follow the target codec: mp4 cannot hold VP8/VP9 and the
        # client would bail for its own reason, which is not a server-side drop.
        bare = (a.video or "").lower()
        vout = "/tmp/repush_span.ivf" if bare in ("vp8", "vp9", "av1") else "/tmp/repush_span.mp4"
        if os.path.exists(vout):
            os.remove(vout)
        viewer = subprocess.Popen(["ffmpeg", "-y", "-v", "error", "-rtsp_transport", "tcp",
                                   "-i", f"rtsp://127.0.0.1:{RTSP}/live/{stream}",
                                   "-t", str(span_secs), "-an", "-c", "copy", vout],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  stdin=subprocess.DEVNULL, start_new_session=True)
        time.sleep(2)

        # ---------- 断开 ----------
        os.killpg(os.getpgid(push1.pid), signal.SIGKILL)
        push1.wait()
        print(f"  -- 推流端已 kill（SIGKILL），等 {a.gap}s 后重推 --")
        time.sleep(a.gap)

        # ---------- 推 #2 ----------
        push2, e2 = spawn_push(a.src, stream, "/tmp/push2.err")
        time.sleep(6)
        rc = push2.poll()
        tail = err_tail(e2)
        print(f"  == 重推 ffmpeg 返回码: {'仍在运行 (None)' if rc is None else rc}")
        print("  == 重推 ffmpeg 实际输出（尾部）:")
        for l in tail:
            print("     |", l)
        note(rc is None, "重推 #2 存活（没被 406 踢掉）", f"rc={rc}")
        txt = "\n".join(tail)
        note(not re.search(r"\b406\b|Already publishing|method ANNOUNCE failed", txt),
             "重推 #2 stderr 无 406/Already publishing")

        log = open(LOG, encoding="utf-8", errors="replace").read()
        note("ANNOUNCE: Already publishing" not in log, "服务端日志无 Already publishing",
             [l for l in log.splitlines() if "Already publishing" in l][:2])

        if rc is None:
            check_pipeline(a, stream, "推#2")

        # ---------- 观众侧 ----------
        try:
            viewer.wait(timeout=span_secs + 25)
        except Exception:
            pass
        if os.path.exists(vout) and os.path.getsize(vout) > 0:
            d = sh(f"ffprobe -v error -show_entries format=duration -of csv=p=0 {vout}").stdout.strip()
            n = sh(f"ffprobe -v error -select_streams v -count_packets -show_entries "
                   f"stream=nb_read_packets -of csv=p=0 {vout}").stdout.strip()
            print(f"  == 跨断点观众录像: {d}s / {n} 包 / {os.path.getsize(vout)} B")
            need = 2 + a.gap + 4
            ok_span = float(d or 0) >= need * 0.8
            if not ok_span and a.allow_viewer_drop:
                print(f"  == 跨断点观众只录到 {d}s（--allow-viewer-drop：基线 direct-proxy 就会断开）")
                note(True, "跨断点观众（允许断开，基线行为）", f"{d}s")
            else:
                note(ok_span, "跨断点观众恢复（录像覆盖断点前后）", f"{d}s ≥ {need * 0.8:.1f}s")
            errs = sh(f"ffmpeg -v error -i {vout} -f null - 2>&1").stdout
            hard = [l for l in errs.splitlines() if l.strip() and "monoton" not in l]
            note(len(hard) <= 2, "跨断点观众录像可正常解码", hard[:2])
        elif a.allow_viewer_drop:
            print("  == 跨断点观众没有覆盖断点（--allow-viewer-drop：不开转码时这是基线行为）")
            note(True, "跨断点观众（允许断开，基线行为）", "断开")
        else:
            print("  == 跨断点观众没有产出录像（观众被断开且没自动恢复）")
            note(False, "跨断点观众恢复（录像覆盖断点前后）", "无录像")
        return True
    finally:
        for pr in (push1, push2, viewer):
            if pr and pr.poll() is None:
                try:
                    os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
                except Exception:
                    pass
        stop(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=f"{BIN_DIR}/MediaServer")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--video", default="")
    ap.add_argument("--audio", default="")
    ap.add_argument("--enable", type=int, default=1)
    ap.add_argument("--stream", default=None)
    ap.add_argument("--gap", type=float, default=2.0, help="断开后等多久再重推（秒）")
    ap.add_argument("--allow-viewer-drop", action="store_true",
                    help="允许跨断点观众被断开：不开转码（direct-proxy）时这是既有行为，"
                         "环形缓冲读者在断流超过读超时后会被关闭，不是本次修复的回归项")
    ap.add_argument("--attempts", type=int, default=3)
    a = ap.parse_args()

    stream = a.stream or ("rp" + re.sub(r"[^0-9a-zA-Z]", "", a.tag))
    make_cfg(a.video, a.audio, a.enable)
    print(f"== {a.tag}: {os.path.basename(a.src)} -> rtsp://127.0.0.1:{RTSP}/live/{stream} "
          f"video={a.video or '-'} audio={a.audio or '-'} enable={a.enable} gap={a.gap}s")
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
    for r in fails:
        print("   -", r[1], r[2])
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
