#!/usr/bin/env python3
"""RTSP 断连续推验收（现场摄像机重连场景）。

为什么单独测：启用转码后，对外源变成 muxer 的 RtspMediaSource，推流端断开重连时
`handleReq_ANNOUNCE` 可能因为源池里还有同名源而回 406 —— 摄像机就再也上不来。
这条只在「断开 + 重连」时暴露，常规推流测试完全看不出来。

判据：
  1. 首推成功，转码生效（拉流编码 = 目标 codec）
  2. kill 推流端后，新推流能成功（不是 406）
  3. 重连后转码仍生效
  4. 不开转码时同样能重连（基线不能退）

用法：env -u PYTHONPATH python3 scripts/rtsp_reconnect_test.py
端口：RTMP 56900 / RTSP 56554 / HTTP 57080
"""
import json, os, re, signal, socket, subprocess, sys, time, urllib.request

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.reconnect.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
LOG = "/tmp/reconnect.log"
SRC_H265 = "/tmp/src_h265.mp4"
SRC_H264 = "/tmp/av_test.mp4"
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"   # 本机测试用，仅内网

PASS, FAIL = [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))

def sh(cmd, timeout=90):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)

def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None

def make_cfg(enable=1, video=""):
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
    txt = "\n".join(out) + "\n"
    # [rtsp]/[rtmp] 的 on_demand 默认是 1（按需）：上一个观众断开后源立刻下架，
    # 紧接着第二次拉流就拿到 `DESCRIBE failed: 404 Stream Not Found`。
    # 这曾被误当成「重连后解码有硬错误」（实测踩过），这里一律置 0。
    txt = re.sub(r"(?m)^on_demand=.*$", "on_demand=0", txt)
    open(CFG, "w", encoding="utf-8").write(txt)

def kill_mine():
    """只杀监听本脚本端口段的 MediaServer，避免误杀本机其他 worktree 的实例。"""
    pid = lpid(RTMP) or lpid(RTSP) or lpid(HTTP)
    if pid:
        sh(f"kill -9 {pid}")
        time.sleep(1.5)
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}")
    time.sleep(1.0)

def start(tag):
    kill_mine()
    open(LOG, "w").close()
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is None and lpid(RTMP) == p.pid:
            return p
    print(open(LOG).read()[-800:]); sys.exit(1)

def stop(p):
    try: os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception: pass
    for _ in range(40):
        if p.poll() is not None:
            break
        time.sleep(0.1)
    time.sleep(0.3)

def push(src, stream):
    """返回 (进程, 推流日志路径)。RTSP 推流要抓 stderr 判断是否 406。"""
    log = f"/tmp/push_{stream}.log"
    f = open(log, "w")
    p = subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy",
                          "-f", "rtsp", "-rtsp_transport", "tcp",
                          f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                         stdout=f, stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    return p, log

def push_ok(log, proc, wait=3.0):
    """推流是否真的建立了：进程还活着 + 日志里没有 406/Already publishing。"""
    time.sleep(wait)
    txt = open(log).read() if os.path.exists(log) else ""
    bad = ("406" in txt) or ("Already publishing" in txt) or ("method ANNOUNCE failed" in txt)
    return (proc.poll() is None) and not bad, txt[-300:].replace("\n", " ")

def vcodec(stream):
    r = sh(f"ffprobe -v error -select_streams v -show_entries stream=codec_name "
           f"-of csv=p=0 rtsp://127.0.0.1:{RTSP}/live/{stream}", timeout=30)
    return r.stdout.strip()

def hard_errors(stream, secs=4):
    """数硬解码错误。

    必须排除两类：
    1. `non monotonically increasing dts` —— 重连后时间轴从 0 重算，客户端会告警，
       属已知良性（线上时间戳经 framecrc 取证是单调的）；
    2. `Last message repeated N times` —— ffmpeg 把重复告警折叠成这一行摘要，
       它本身不含 "monoton"，只过滤关键字会把它误判成硬错误（实测踩过）。
    """
    # 注意：不要用 `-ss 2` 来「跳过加入时的 GOP 中间段」——直播流上 seek 并不等于
    # 跳到关键帧，实测反而让每条转码流都报 `Could not find ref`（比不 seek 更糟）。
    # 加入时机造成的引用缺失改为按行排除（见下）。
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
           f"-t {secs} -f null - 2>&1", timeout=secs + 50)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    hard = [l for l in lines
            if "monoton" not in l and "Last message repeated" not in l]
    return hard


_TL = []          # 采样到的媒体源存在性时间轴
_TL_ON = [False]

_HTTP_PID = [None]

def _sampler(stream, stop):
    """每 200ms 记一次该流的协议源列表，用来把「源在不在」和 404 的时刻对齐。
    之前只能看到『404 空窗 16s』这个结论，却不知道键是什么时候没的 —— 就是因为没有时间轴。"""
    import threading
    def run():
        t0 = time.time()
        while not stop.is_set():
            try:
                body = urllib.request.urlopen(
                    f"http://127.0.0.1:{HTTP}/index/api/getMediaList?secret={SECRET}", timeout=2).read()
                data = json.loads(body).get("data") or []
                # 记完整 tuple：池里的 vhost 若与 DESCRIBE 用的 __defaultVhost__ 不一致，
                # 就会出现「getMediaList 有、findAsync 找不到」的自相矛盾
                names = sorted({f'{d.get("vhost")}/{d.get("app")}/{d.get("schema")}'
                                for d in data if d.get("stream") == stream})
            except Exception:
                names = ["<api-fail>"]
            # 每次采样都取端口归属（不能缓存！上一版只在首次取样，那时服务还没起来，
            # 缓存成了 '?'，白跑一轮）。若 HTTP 与 RTSP 不是同一个 pid，
            # 说明 getMediaList 与 DESCRIBE 根本不在同一个进程里 —— 那 404 就是双实例。
            if len(_TL) % 5 == 0:      # 每 ~1 秒取一次，别把采样拖慢
                hp = sh(f"/usr/sbin/lsof -nP -tiTCP:{HTTP} -sTCP:LISTEN").stdout.strip().split()
                rp = sh(f"/usr/sbin/lsof -nP -tiTCP:{RTSP} -sTCP:LISTEN").stdout.strip().split()
                _HTTP_PID[0] = f"pid http={hp[0] if hp else '?'} rtsp={rp[0] if rp else '?'}"
            _TL.append((round(time.time() - t0, 2), names + ([_HTTP_PID[0]] if _HTTP_PID[0] else [])))
            time.sleep(0.2)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t

def timeline_dump(stream):
    """把时间轴压缩成「状态变化」几行打出来。"""
    if not _TL:
        return
    print(f"     ↳ 媒体源时间轴（{stream}）：")
    prev = None
    for ts, names in _TL:
        if tuple(names) != prev:
            print(f"        {ts:6.2f}s  {names}")
            prev = tuple(names)


def check_errors(label, stream, retries=4):
    """断言拉流无硬错误。

    两类「错误」必须分开看，否则会互相冒充：
    - `DESCRIBE failed: 404 Stream Not Found` = 媒体源尚未注册（连接失败，不是解码错误）。
      实测重连转码时存在注册空窗：旧源先释放、新源后注册，中间 DESCRIBE 会 404。
      这里重试并**把空窗时长打出来**，不掩盖。
    - 其余行 = 真·解码/打包错误，直接判失败并打印。
    """
    gap = 0
    hard = []
    for attempt in range(retries):
        hard = hard_errors(stream)
        not_found = [h for h in hard if "404" in h or "Stream Not Found" in h]
        real = [h for h in hard if not ("404" in h or "Stream Not Found" in h)]
        if not not_found:
            break
        gap += 4   # hard_errors 每次尝试 4 秒
    else:
        real = hard
    extra = f"，其中首次拉流 404，注册空窗约 {gap}s 后恢复" if gap else ""
    ok = len(real) == 0
    note(ok, label, (f"{len(real)} 条硬错误" if real else "0 条") + extra)
    for line in real[:3]:
        print(f"        ↳ {line[:150]}")

def case(tag, src, want_codec, video, enable=1):
    print(f"--- {tag} ---")
    _TL.clear()
    fail_snapshot = list(FAIL)
    stop_ev = __import__("threading").Event()
    _sampler(tag, stop_ev)
    make_cfg(enable=enable, video=video)
    p = start(tag)
    try:
        # 1) 首推
        pr, plog = push(src, tag)
        ok, tail = push_ok(plog, pr)
        note(ok, "首推成功", tail)
        got = vcodec(tag) if ok else ""
        note(got == want_codec, f"首推后拉流 = {want_codec}", f"实得 {got}")
        check_errors("首推后解码 0 硬错误", tag)

        # 2) 断开
        try: os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
        except Exception: pass
        time.sleep(2.5)

        # 3) 重连
        pr2, plog2 = push(src, tag + "_re")
        ok2, tail2 = push_ok(plog2, pr2)
        note(ok2, "断开后能重新推流（不是 406）", tail2)
        got2 = vcodec(tag) if ok2 else ""
        note(got2 == want_codec, f"重连后拉流仍 = {want_codec}", f"实得 {got2}")
        if ok2:
            check_errors("重连后解码 0 硬错误", tag)
        else:
            note(False, "重连后解码 0 硬错误", "重连失败，无从判定")
        try: os.killpg(os.getpgid(pr2.pid), signal.SIGKILL)
        except Exception: pass
    finally:
        stop(p)
        stop_ev.set()
        time.sleep(0.3)
        if FAIL and len(FAIL) > len(fail_snapshot):
            timeline_dump(tag)
            # 服务端 404 的两个分支：找不到源 / SDP 里没有有效 track。
            # 这两句能一次定性，别再靠猜（实测被误判过两轮）。
            try:
                hits = [l for l in open(LOG, errors="replace").read().splitlines()
                        if "无有效track" in l or "no such stream" in l or "404" in l
                        or "DIAG pool" in l]
                print(f"     ↳ 服务端 404 相关日志 {len(hits)} 行：")
                for l in hits[-4:]:
                    # 不要截断：时间戳前缀是判断「404 与源存在是否同一时刻」的唯一依据，
                    # 上一版截到末 160 字符正好把它砍掉了，导致七轮假设都没法对齐时间
                    print("        " + l)
            except Exception as e:
                print(f"     ↳ 读服务端日志失败：{e}")

print("=" * 76)
print("RTSP 断连续推验收（模拟摄像机重连）")
print("=" * 76)

print()
print("[转码开启] H265 源 -> H264")
case("rc_h265_h264", SRC_H265, "h264", "H264", enable=1)

print()
print("[转码开启] H264 源 -> H265")
case("rc_h264_h265", SRC_H264, "hevc", "H265", enable=1)

print()
print("[基线：不开转码] H265 源，重连必须也正常")
case("rc_base", SRC_H265, "hevc", "", enable=0)

print()
print("=" * 76)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
for x in FAIL:
    print("  -", x)
print("=" * 76)
sys.exit(0 if not FAIL else 1)
