#!/usr/bin/env python3
"""a3 验收：API 分离 + 按流规则 + 热更新如实回报。真机实测，不猜。"""
import json, os, signal, subprocess, sys, time, urllib.request, urllib.parse

WT = "/Users/zhanglei/zlm-wt/a3"
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.av.ini"
CFG = f"{WT}/config.a3test.ini"
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
HTTP = "56080"
RTMP = "53900"
RTSP = "53554"

def pull_url(stream):
    """转码要看 RTSP：RTMP/FLV 的容器与 codec 映射不支持 HEVC 和 Opus。"""
    return f"rtsp://127.0.0.1:{RTSP}/live/{stream}"
LOG = "/tmp/a3_accept.log"
SRC_AV = "/tmp/av_test.mp4"
SRC_A = "/tmp/aac_test.aac"

PASS, FAIL = [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'[PASS]' if ok else '[FAIL]'} {name}" + (f"  {detail}" if detail else ""))

def sh(cmd, timeout=30, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          timeout=timeout, **kw)

def api(path, **params):
    params["secret"] = params.get("secret", SECRET)
    q = urllib.parse.urlencode(params)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?{q}"
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            body = r.read().decode("utf-8", "replace")
    except Exception as e:
        return {"_error": str(e)}
    try:
        return json.loads(body)
    except json.JSONDecodeError:
        return {"_raw": body[:300]}

def make_cfg(enable=1, rules=(), audio="opus", video="h265"):
    txt = open(BASE_CFG, encoding="utf-8").read()
    lines = txt.splitlines()
    out, i, in_tc = [], 0, False
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("[transcode]"):
            in_tc = True
            out.append(ln)
            out.append(f"enable={enable}")
            out.append(f"audio_codec={audio}")
            out.append(f"video_codec={video}")
            # on_demand 必须显式写 0：不写就落到默认值 1，无观众时根本不建编码器，
            # 表现为 frames=0、拉流是源编码，看着像「配置没生效」
            out.append("on_demand=0")
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                i += 1
            for n, r in enumerate(rules):
                key = "stream_rule" if n == 0 else f"stream_rule_{n+1}"
                out.append(f"{key}={r}")
            continue
        out.append(ln)
        i += 1
    open(CFG, "w", encoding="utf-8").write("\n".join(out) + "\n")

def listening_pid(port):
    r = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN")
    out = r.stdout.strip().split()
    return int(out[0]) if out else None

def kill_all():
    """杀掉所有 MediaServer 并确认端口真的空了。
    只用 pkill -f 'MediaServer -c <path>' 会漏掉用相对路径起的旧进程，
    旧进程占着端口 -> 新进程绑定失败退出 -> 测试请求打到旧进程的配置上，
    表现为「改了配置完全没生效」的假绿。"""
    for _ in range(3):
        sh("pkill -9 -f MediaServer; sleep 1.5")
        if all(listening_pid(pt) is None for pt in (RTMP, 53554, int(HTTP))):
            return
    for pt in (RTMP, 53554, int(HTTP)):
        pid = listening_pid(pt)
        if pid:
            print(f"  !! 端口 {pt} 仍被 pid={pid} 占用")
    print("  !! 端口未释放，中止"); sys.exit(1)

def wait_expect(expect):
    """确认跑着的服务读的是本次配置：不匹配就当失败，绝不带病继续。"""
    for _ in range(60):
        time.sleep(0.25)
        try:
            d = api("getServerConfig")
            if d.get("code") != 0:
                continue
            cfg = d["data"][0]
            got = {k: cfg.get(f"transcode.{k}") for k in ("enable", "audio_codec", "video_codec")}
            if str(got["enable"]) == str(expect["enable"]) and \
               got["audio_codec"] == expect["audio"] and got["video_codec"] == expect["video"]:
                return True
            print(f"  !! 配置未生效：期望 {expect} 实得 {got}"); return False
        except Exception:
            continue
    return False

def start_server(tag, expect):
    kill_all()
    open(LOG, "w").close()
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
    if not wait_expect(expect):
        print(open(LOG).read()[-1500:]); stop(p); sys.exit(1)
    pid = listening_pid(RTMP)
    if pid != p.pid:
        print(f"  !! 监听 {RTMP} 的不是我起的进程（我={p.pid} 实际={pid}）"); stop(p); sys.exit(1)
    print(f"  服务已起（{tag}），pid={p.pid}，配置已核对")
    return p

def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(1.5)

def push(src, stream, dur=12):
    return subprocess.Popen(
        ["ffmpeg", "-re", "-stream_loop", "-1", "-i", src,
         "-c", "copy", "-f", "flv", f"rtmp://127.0.0.1:{RTMP}/live/{stream}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)

def stream_packets(url, sel, seconds=6):
    """只数某一路（sel='a' 或 'v'）。
    多流一起数时 ffprobe 的 nb_read_packets 常给第二路 0（已知抖动），
    分开单独起进程数才稳。"""
    for _ in range(3):
        try:
            r = sh(f"ffprobe -v error -select_streams {sel} -read_intervals '%+{seconds}' "
                   f"-count_packets -show_entries stream=nb_read_packets -of csv=p=0 {url}",
                   timeout=seconds + 15)
            v = r.stdout.strip().split("\n")[0].strip()
            if v.isdigit() and int(v) > 0:
                return int(v)
        except subprocess.TimeoutExpired:
            pass
        time.sleep(0.6)
    return 0

def probe_tracks(url, timeout=12):
    """返回 (tracks列表, 每轨包数) ；tracks = [(codec, type, w/h 或 sr/ch)]"""
    try:
        r = sh(f"ffprobe -v error -show_entries stream=index,codec_name,codec_type,width,height,"
               f"sample_rate,channels -of csv=p=0 {url}", timeout=timeout)
    except subprocess.TimeoutExpired:
        return [], {}
    tracks = []
    for ln in r.stdout.strip().splitlines():
        f = ln.split(",")
        if len(f) >= 3:
            tracks.append((f[1], f[2], ",".join(f[3:])))
    return tracks, {}

def count_packets(url, seconds=6):
    """数包。ffprobe 走 RTSP 偶发拿到 0（会话刚建立/上一个 probe 未断），
    此时静默返回 0 会让测试假绿或假红，所以重试，取第一次非零结果。"""
    def once():
        try:
            r = sh(f"ffprobe -v error -read_intervals '%+{seconds}' -count_packets "
                   f"-show_entries stream=index,codec_type,nb_read_packets -of csv=p=0 {url}",
                   timeout=seconds + 15)
        except subprocess.TimeoutExpired:
            return {}
        out = {}
        for ln in r.stdout.strip().splitlines():
            f = ln.split(",")
            if len(f) >= 3 and f[2].isdigit():
                out[f[1]] = int(f[2])
        return out
    best = {}
    for _ in range(3):
        cur = once()
        if sum(cur.values()) > 0:
            return cur
        best = best or cur
        time.sleep(0.8)
    return best

def transcode_loglines(pattern="transcod"):
    return sh(f"grep -c -i '{pattern}' {LOG}").stdout.strip()

print("=" * 68)
print("T1  错误处理（服务需先起）")
print("=" * 68)
make_cfg(enable=1, rules=(), audio="opus", video="h265")
p = start_server("T1", {"enable": 1, "audio": "opus", "video": "h265"})
try:
    # 约定：查离线流不算错误，返回「若上线会用什么配置」+ online=false；
    # 只有缺参数/非法值才是错误码。这样运维能在推流前预检配置。
    r = api("getTranscode", stream="nope", app="live")
    note(r.get("code") == 0 and r.get("online") is False,
         "查离线流 -> code=0 且 online=false（可预检配置）",
         f"code={r.get('code')} online={r.get('online')}")
    note(not (r.get("tracks") or []), "离线流无 tracks 明细（不编造）", f"{r.get('tracks')}")

    r = api("addTranscode", secret="wrong", app="live", stream="cam1", audio_codec="opus")
    note(r.get("code") != 0 or "secret" in str(r).lower(), "错 secret -> 被拒", str(r)[:120])

    r = api("addTranscode", app="live", stream="")
    note(r.get("code") != 0, "缺 stream 参数 -> 非 0 code", str(r)[:120])

    r = api("addTranscode", app="live", stream="cam1", audio_codec="nosuchcodec")
    note(r.get("code") != 0, "非法 codec -> 非 0 code", str(r)[:140])

    r = api("addTranscode", app="live", stream="cam1", audio_codec="opus", bitrate="abc")
    note(r.get("code") != 0, "非法 bitrate -> 非 0 code", str(r)[:140])
finally:
    stop(p)

print()
print("=" * 68)
print("T2  基线回归：enable=0 时零转码日志、编码与源一致")
print("=" * 68)
make_cfg(enable=0, rules=(), audio="opus", video="h265")
p = start_server("T2-enable0", {"enable": 0, "audio": "opus", "video": "h265"})
try:
    pr = push(SRC_AV, "base")
    time.sleep(5)
    tracks, _ = probe_tracks(pull_url("base"))
    codecs = sorted(t[0] for t in tracks)
    note(codecs == ["aac", "h264"], "enable=0 拉流编码 == 源(h264/aac)", f"{codecs}")
    n = transcode_loglines()
    note(n in ("0", ""), "enable=0 零转码日志", f"grep 计数={n}")
    os.killpg(os.getpgid(pr.pid), signal.SIGTERM)
finally:
    stop(p)

print()
print("=" * 68)
print("T3  按流规则：cam1=opus / cam2=PCMA，两路同时推互不影响")
print("=" * 68)
make_cfg(enable=0, rules=("live/cam1:enable=1,audio_codec=opus,sample_rate=48000",
                          "live/cam2:enable=1,audio_codec=g711a,sample_rate=8000"),
         audio="opus", video="")
p = start_server("T3-rules", {"enable": 0, "audio": "opus", "video": ""})
try:
    p1 = push(SRC_AV, "cam1")
    p2 = push(SRC_AV, "cam2")
    time.sleep(7)
    t1, _ = probe_tracks(pull_url("cam1"))
    t2, _ = probe_tracks(pull_url("cam2"))
    a1 = [t for t in t1 if t[1] == "audio"]
    a2 = [t for t in t2 if t[1] == "audio"]
    v1 = [t for t in t1 if t[1] == "video"]
    note(bool(a1) and a1[0][0] == "opus", "cam1 音频 -> opus", f"{a1}")
    note(bool(a2) and a2[0][0] in ("pcm_alaw", "pcm_mulaw"), "cam2 音频 -> PCMA", f"{a2}")
    note(bool(v1) and v1[0][0] == "h264", "全局 video_codec 为空 -> cam1 视频保持 h264 不转", f"{v1}")

    g1 = api("getTranscode", app="live", stream="cam1")
    g2 = api("getTranscode", app="live", stream="cam2")
    note("stream_rule" in str(g1.get("config_source", "")), "cam1 config_source 命中按流规则", str(g1.get("config_source")))
    note("stream_rule" in str(g2.get("config_source", "")), "cam2 config_source 命中按流规则", str(g2.get("config_source")))
    note(g1.get("failed_tracks") == 0 and g2.get("failed_tracks") == 0,
         "两路 failed_tracks 均为 0", f"cam1={g1.get('failed_tracks')} cam2={g2.get('failed_tracks')}")

    pk1 = count_packets(pull_url("cam1"), 6)
    note(pk1.get("audio", 0) > 0, "cam1 拉流有音频包", f"{pk1}")
    os.killpg(os.getpgid(p1.pid), signal.SIGTERM)
    os.killpg(os.getpgid(p2.pid), signal.SIGTERM)
finally:
    stop(p)

print()
print("=" * 68)
print("T4  API 覆盖 ini（离线预设，推流前设置 -> 首帧即刻生效）")
print("=" * 68)
make_cfg(enable=0, rules=("live/ovr:enable=1,audio_codec=opus,sample_rate=48000",),
         audio="opus", video="")
p = start_server("T4-override", {"enable": 0, "audio": "opus", "video": ""})
try:
    r = api("addTranscode", app="live", stream="ovr", audio_codec="g711u", sample_rate="8000")
    note(r.get("code") == 0, "addTranscode 离线预设成功", str(r)[:160])
    # 离线预设的诚实答案是 waiting：流没上线，谈不上「已生效」，上线时才会用上
    note(r.get("applied") == "waiting", "离线预设如实回报 waiting（不谎报已生效）", str(r.get("applied")))
    note(r.get("persistent") is False, "回报 persistent=false", str(r.get("persistent")))

    pr = push(SRC_AV, "ovr")
    time.sleep(6)
    tr, _ = probe_tracks(pull_url("ovr"))
    au = [t for t in tr if t[1] == "audio"]
    note(bool(au) and au[0][0] in ("pcm_mulaw",), "API 覆盖生效：拉流是 g711u 而非规则里的 opus", f"{au}")
    g = api("getTranscode", app="live", stream="ovr")
    note("api" in str(g.get("config_source", "")), "config_source 显示 api 覆盖", str(g.get("config_source")))
    os.killpg(os.getpgid(pr.pid), signal.SIGTERM)
finally:
    stop(p)

print()
print("=" * 68)
print("T5  热更新：运行中改 codec -> 如实报 pending，此刻拉流仍是旧 codec")
print("=" * 68)
make_cfg(enable=1, rules=(), audio="opus", video="h265")
p = start_server("T5-hot", {"enable": 1, "audio": "opus", "video": "h265"})
try:
    pr = push(SRC_AV, "hot")
    time.sleep(6)
    before, _ = probe_tracks(pull_url("hot"))
    ab = [t[0] for t in before if t[1] == "audio"]
    note("opus" in ab, "初始音频 opus", f"{ab}")

    r = api("addTranscode", app="live", stream="hot", audio_codec="g711a")
    note(r.get("code") == 0, "热改被接受", str(r)[:100])
    note(r.get("applied") == "pending", "如实回报 pending（不谎报立即生效）", str(r.get("applied")))

    after, _ = probe_tracks(pull_url("hot"))
    aa = [t[0] for t in after if t[1] == "audio"]
    note("opus" in aa, "热改后立刻拉流仍是 opus（未谎报）", f"{aa}")
    os.killpg(os.getpgid(pr.pid), signal.SIGTERM)
finally:
    stop(p)

print()
print("=" * 68)
print("T6  音视频同时转码：getTranscode 的 tracks 明细")
print("=" * 68)
make_cfg(enable=1, rules=(), audio="opus", video="h265")
p = start_server("T6-avboth", {"enable": 1, "audio": "opus", "video": "h265"})
try:
    pr = push(SRC_AV, "av")
    time.sleep(7)
    g = api("getTranscode", app="live", stream="av")
    print("  getTranscode =", json.dumps(g, ensure_ascii=False)[:600])
    note(g.get("failed_tracks") == 0, "failed_tracks=0", str(g.get("failed_tracks")))
    note(bool(g.get("transcoding")) is True, "transcoding=true（有轨真在转）", str(g.get("transcoding")))
    tr = g.get("tracks") or []
    note(len(tr) >= 2, "tracks 明细 >= 2 条", f"{len(tr)} 条")
    kinds = {t.get("type") or t.get("codec_type"): t for t in tr if isinstance(t, dict)}
    if "video" in kinds:
        v = kinds["video"]
        note("265" in str(v).lower() or "hevc" in str(v).lower(), "视频轨明细指向 H265", json.dumps(v, ensure_ascii=False)[:200])
    if "audio" in kinds:
        a = kinds["audio"]
        note("opus" in str(a).lower(), "音频轨明细指向 opus", json.dumps(a, ensure_ascii=False)[:200])

    st, _ = probe_tracks(pull_url("av"))
    note(any(t[0] == "hevc" for t in st), "拉流视频 == hevc", str([t[0] for t in st]))
    note(any(t[0] == "opus" for t in st), "拉流音频 == opus", str([t[0] for t in st]))
    vn = stream_packets(pull_url("av"), "v", 6)
    an = stream_packets(pull_url("av"), "a", 6)
    note(60 <= vn <= 130, "视频包数合理（6s@15fps 理论 90）", f"{vn}")
    note(an > 100, "音频包数合理（6s@20ms 理论 300）", f"{an}")
    os.killpg(os.getpgid(pr.pid), signal.SIGTERM)
finally:
    stop(p)

print()
print("=" * 68)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
if FAIL:
    print("失败项：")
    for f in FAIL:
        print("  -", f)
print("=" * 68)
sys.exit(0 if not FAIL else 1)
