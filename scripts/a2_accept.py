#!/usr/bin/env python3
"""a2 验收：视频参数化（H264 输出 / 分辨率缩放 / 码率控制 / 降帧 / 1080p）。

所有结论来自真实拉流文件，不看服务端自述。
复现：env -u PYTHONPATH python3 scripts/a2_accept.py
"""
import json, os, signal, subprocess, sys, time, urllib.request

WT = "/Users/zhanglei/zlm-wt/a2"
BIN = f"{WT}/release/darwin/Release/MediaServer"
BASE_CFG = f"{WT}/config.av.ini"
CFG = f"{WT}/config.a2test.ini"
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
HTTP, RTMP, RTSP = 55080, 52900, 52554
LOG = "/tmp/a2_accept.log"
SRC = "/tmp/av_test.mp4"          # h264 320x240 15fps + aac
SRC720 = "/tmp/src_1280x720.mp4"
SRC1080 = "/tmp/src_1920x1080.mp4"

PASS, FAIL = [], []

def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'[PASS]' if ok else '[FAIL]'} {name}" + (f"  {detail}" if detail else ""))

def sh(cmd, timeout=60):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)

def api(path, **params):
    params.setdefault("secret", SECRET)
    import urllib.parse
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?{urllib.parse.urlencode(params)}"
    try:
        body = urllib.request.urlopen(url, timeout=8).read().decode("utf-8", "replace")
        return json.loads(body)
    except Exception as e:
        return {"_error": str(e)}

def listening_pid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.strip().split()
    return int(out[0]) if out else None

def make_cfg(enable=1, audio="opus", video="h265", **extra):
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i = [], 0
    while i < len(lines):
        if lines[i].strip().startswith("[transcode]"):
            out.append(lines[i])
            out += [f"enable={enable}", f"audio_codec={audio}", f"video_codec={video}",
                    "on_demand=0"]
            for k, v in extra.items():
                out.append(f"{k}={v}")
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                i += 1
            continue
        out.append(lines[i]); i += 1
    open(CFG, "w", encoding="utf-8").write("\n".join(out) + "\n")

def kill_all():
    for _ in range(3):
        sh("pkill -9 -f MediaServer; sleep 1.5")
        if all(listening_pid(p) is None for p in (RTMP, RTSP, HTTP)):
            return
    print("  !! 端口未释放"); sys.exit(1)

def start_server(tag, expect):
    kill_all()
    open(LOG, "w").close()
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=f"{WT}/release/darwin/Release",
                         stdout=open(LOG, "a"), stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        try:
            d = api("getServerConfig")
            if d.get("code") != 0:
                continue
            cfg = d["data"][0]
            got = {k: cfg.get(f"transcode.{k}") for k in expect}
            if all(str(got[k]) == str(v) for k, v in expect.items()):
                if listening_pid(RTMP) == p.pid:
                    print(f"  服务已起（{tag}），pid={p.pid}，配置已核对")
                    return p
                print("  !! 监听者不是本进程"); break
            print(f"  !! 配置未生效：期望 {expect} 实得 {got}"); break
        except Exception:
            pass
    print(open(LOG).read()[-1200:]); stop(p); sys.exit(1)

def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(1.2)

def push(src, stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", src, "-c", "copy",
                             "-f", "flv", f"rtmp://127.0.0.1:{RTMP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)

def grab(stream, seconds=8):
    """拉 RTSP 存成文件，后续全部用它取证；返回文件路径。"""
    out = f"/tmp/a2_grab_{stream}.mp4"
    if os.path.exists(out):
        os.remove(out)
    sh(f"ffmpeg -y -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
       f"-t {seconds} -c copy {out}", timeout=seconds + 30)
    return out

def info(path):
    r = sh(f"ffprobe -v error -select_streams v -count_frames "
           f"-show_entries stream=codec_name,width,height,avg_frame_rate,nb_read_frames,bit_rate "
           f"-of json {path}")
    try:
        s = json.loads(r.stdout)["streams"][0]
    except Exception:
        return {}
    s["bytes"] = os.path.getsize(path) if os.path.exists(path) else 0
    return s

def decode_errors(stream, seconds=6):
    """完整解码一遍数错误行。

    硬错误（=没通）：Illegal temporal ID / Multi-layer HEVC / Could not find ref /
    decode_slice_header error 等，任何一条都算失败。

    启动期 dts 警告单独计：推流刚建立时 ffmpeg 的 null muxer 偶发报
    "non monotonically increasing dts"，但线上时间戳经 framecrc 取证零违规、
    音频解码时长精确（6.000s），属客户端启动瞬间的竞态。既不许它掩盖真错误，
    也不该因为它在不同轮次时有时无就让测试随机红。
    """
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:{RTSP}/live/{stream} "
           f"-t {seconds} -f null - 2>&1", timeout=seconds + 30)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    warn = [l for l in lines if "monoton" in l]
    hard = [l for l in lines if "monoton" not in l]
    return len(hard), hard[:3], len(warn)

def run(tag, push_src, push_stream, expect_cfg, checks):
    make_cfg(**expect_cfg["cfg"])
    p = start_server(tag, expect_cfg["probe"])
    try:
        pr = push(push_src, push_stream)
        time.sleep(6)
        f = grab(push_stream, 8)
        # a2 分支未含 P8 的 HTTP API，g 恒为 None（这些断言属于 a3 的验收）
        checks(None, info(f), f, push_stream)
        os.killpg(os.getpgid(pr.pid), signal.SIGTERM)
    finally:
        stop(p)

print("=" * 70)
print("T0  基线：视频参数全 0 -> 跟随源（320x240 15fps），行为与硬编码路径一致")
print("=" * 70)
def t0(g, i, f, s):
    note(i.get("codec_name") == "hevc", "输出仍是 hevc", f"{i.get('codec_name')}")
    note((i.get("width"), i.get("height")) == (320, 240), "尺寸跟随源 320x240",
         f"{i.get('width')}x{i.get('height')}")
    errs, sample, warn = decode_errors(s)
    note(errs == 0, "硬解码错误 0 条", f"{errs} 条 {sample}；启动期dts警告{warn}条")
run("T0-基线", SRC, "t0", {"cfg": dict(enable=1, video="h265"),
                          "probe": {"enable": 1, "video_codec": "h265"}}, t0)

print()
print("=" * 70)
print("T1  H264 输出（libx264，H264 没有 VPS，warmup 要适配）")
print("=" * 70)
def t1(g, i, f, s):
    note(i.get("codec_name") == "h264", "输出 h264", f"{i.get('codec_name')}")
    note((i.get("width"), i.get("height")) == (320, 240), "尺寸跟随源", f"{i.get('width')}x{i.get('height')}")
    errs, sample, warn = decode_errors(s)
    note(errs == 0, "硬解码错误 0 条（SPS/PPS 正确）", f"{errs} 条 {sample}；dts警告{warn}条")
    n = int(i.get("nb_read_frames") or 0)
    note(60 <= n <= 160, "8 秒 15fps 帧数合理", f"{n}")
run("T1-h264", SRC, "t1", {"cfg": dict(enable=1, video="h264"),
                          "probe": {"enable": 1, "video_codec": "h264"}}, t1)

print()
print("=" * 70)
print("T2  分辨率缩放：320x240 -> 640x480 与 160x120")
print("=" * 70)
def t2a(g, i, f, s):
    note((i.get("width"), i.get("height")) == (640, 480), "放大到 640x480",
         f"{i.get('width')}x{i.get('height')}")
    errs, sample, warn = decode_errors(s)
    note(errs == 0, "放大后硬解码错误 0 条", f"{errs} 条 {sample}；dts警告{warn}条")
    size = os.path.getsize(f) if os.path.exists(f) else 0
    note(size > 20000, "文件有实际画面数据", f"{size} 字节")
run("T2a-640x480", SRC, "t2a",
    {"cfg": dict(enable=1, video="h265", video_width=640, video_height=480),
     "probe": {"video_width": 640, "video_height": 480}}, t2a)

def t2b(g, i, f, s):
    note((i.get("width"), i.get("height")) == (160, 120), "缩小到 160x120",
         f"{i.get('width')}x{i.get('height')}")
    errs, sample, warn = decode_errors(s)
    note(errs == 0, "缩小后硬解码错误 0 条", f"{errs} 条 {sample}；dts警告{warn}条")
run("T2b-160x120", SRC, "t2b",
    {"cfg": dict(enable=1, video="h265", video_width=160, video_height=120),
     "probe": {"video_width": 160, "video_height": 120}}, t2b)

print()
print("=" * 70)
print("T3  码率控制：150k vs 1200k，比 8 秒输出字节数")
print("=" * 70)
sizes = {}
def mk_t3(key):
    def f(g, i, fi, s):
        sizes[key] = (os.path.getsize(fi) if os.path.exists(fi) else 0, i.get("bit_rate"))
        note(sizes[key][0] > 0, f"{key}: 拉流有数据", f"{sizes[key][0]} 字节 bit_rate={sizes[key][1]}")
        errs, sample, warn = decode_errors(s)
        note(errs == 0, f"{key}: 硬解码错误 0 条", f"{errs} 条 {sample}；dts警告{warn}条")
    return f
run("T3-low", SRC720, "t3low",
    {"cfg": dict(enable=1, video="h265", video_bitrate=150000),
     "probe": {"video_bitrate": 150000}}, mk_t3("150k"))
run("T3-high", SRC720, "t3high",
    {"cfg": dict(enable=1, video="h265", video_bitrate=1200000),
     "probe": {"video_bitrate": 1200000}}, mk_t3("1200k"))
if len(sizes) == 2:
    lo, hi = sizes["150k"][0], sizes["1200k"][0]
    note(hi > lo * 1.5, "码率设置真的影响输出体积（1200k 明显大于 150k）",
         f"{lo} -> {hi} 字节，倍数 {hi/max(lo,1):.2f}")

print()
print("=" * 70)
print("T4  降帧：源 15fps -> 输出 5fps")
print("=" * 70)
def t4(g, i, f, s):
    num, den = (i.get("avg_frame_rate") or "0/1").split("/")
    fps = float(num) / float(den or 1)
    note(4.0 <= fps <= 6.5, "输出帧率降到 ~5fps", f"{fps:.2f} fps ({i.get('avg_frame_rate')})")
    errs, sample, warn = decode_errors(s)
    note(errs == 0, "降帧后硬解码错误 0 条", f"{errs} 条 {sample}；dts警告{warn}条")
run("T4-fps5", SRC, "t4", {"cfg": dict(enable=1, video="h265", video_fps=5),
                           "probe": {"video_fps": 5}}, t4)

print()
print("=" * 70)
print("T5  1080p@30fps 源：不崩、帧数对、0 解码错误")
print("=" * 70)
def t5(g, i, f, s):
    note((i.get("width"), i.get("height")) == (1920, 1080), "尺寸跟随源 1920x1080",
         f"{i.get('width')}x{i.get('height')}")
    n = int(i.get("nb_read_frames") or 0)
    note(150 <= n <= 300, "8 秒 30fps 帧数合理（理论 240）", f"{n}")
    errs, sample, warn = decode_errors(s)
    note(errs == 0, "1080p 硬解码错误 0 条", f"{errs} 条 {sample}；dts警告{warn}条")
    cpu = sh("ps -A -o %cpu,comm | grep -i mediaserver | head -1").stdout.strip()
    print(f"  （参考）进程 CPU: {cpu}")
run("T5-1080p", SRC1080, "t5", {"cfg": dict(enable=1, video="h265"),
                                "probe": {"enable": 1, "video_codec": "h265"}}, t5)

print()
print("=" * 70)
print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
for x in FAIL:
    print("  -", x)
print("=" * 70)
sys.exit(0 if not FAIL else 1)
