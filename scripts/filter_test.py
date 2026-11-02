#!/usr/bin/env python3
"""视频滤镜（libavfilter 单输入链）验收。

被测能力：第 7 项「支持滤镜」的基础 —— OSD 文字这类**不改变尺寸**的滤镜。

判据用拉流端的 **luma 均值（signalstats YAVG）**，不做逐像素比对：
编码器输出本身不保证逐字节可复现，但亮度统计是稳定的量纲。
  - negate（反相）：过滤后的 YAVG ≈ 255 - 原始 YAVG
  - drawtext（OSD）：YAVG 与原始有可见差异（画白字/黑底框）
  - 非法滤镜名：流必须照常可用（滤镜被停用），尺寸不变
  - 改变尺寸的滤镜（crop）：init 阶段被拒绝，流水线不受影响

用法：env -u PYTHONPATH python3 scripts/filter_test.py
"""
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.parse
import urllib.request

WT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from _platshim import bin_path as _bin_path, bin_dir as _bin_dir
BIN = _bin_path()
BIN_DIR = _bin_dir()
BASE_CFG = f"{WT}/config.joint.base.ini"
CFG = f"{WT}/config.filter.ini"
HTTP, RTMP, RTSP = 57080, 56900, 56554
SECRET = "5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z"
SRC = "/tmp/src_1280x720.mp4"
LOG = "/tmp/filter.log"

PASS, FAIL = [], []


def note(ok, name, detail=""):
    (PASS if ok else FAIL).append(name)
    print(f"  {'PASS ' if ok else 'FAIL '} {name}" + (f"   {detail}" if detail else ""))


def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)


def lpid(port):
    out = sh(f"/usr/sbin/lsof -nP -tiTCP:{port} -sTCP:LISTEN").stdout.split()
    return int(out[0]) if out else None


def api(path, **kw):
    kw.setdefault("secret", SECRET)
    url = f"http://127.0.0.1:{HTTP}/index/api/{path}?" + urllib.parse.urlencode(kw)
    try:
        with urllib.request.urlopen(url, timeout=8) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"code": -1, "_err": str(e)}


def stats(stream):
    """拉 2 秒，返回 (宽, 高, luma 均值, 解码错误数)。"""
    url = f"rtsp://127.0.0.1:{RTSP}/live/{stream}"
    probe = sh(f"ffprobe -v error -select_streams v -show_entries stream=width,height "
               f"-of csv=p=0 {url}", timeout=30).stdout.strip()
    dims = None
    if "," in probe:
        w, h = probe.split(",")[:2]
        dims = (int(w), int(h))
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i {url} -t 2 "
           f"-vf signalstats,metadata=print:file=- -f null - 2>&1", timeout=60)
    vals = [float(m) for m in re.findall(r"signalstats\.YAVG=([0-9.]+)", r.stdout)]
    yavg = sum(vals) / len(vals) if vals else None
    # signalstats 的 metadata=print 会把每帧统计打到 stdout（lavfi.signalstats.* / frame:），
    # 这些不是错误行；漏排除会把 1300+ 行统计当成「硬错误」（实测踩过）。
    errs = [l for l in r.stdout.splitlines()
            if l.strip() and not l.startswith(("lavfi.signalstats", "frame:"))
            and "monoton" not in l and "Last message repeated" not in l]
    return dims, yavg, errs


def region_yavg(stream, w, h, x, y, secs=1):
    """只统计某个矩形区域的 luma 均值 —— logo 只覆盖画面一角，全画面均值会被稀释。"""
    url = f"rtsp://127.0.0.1:{RTSP}/live/{stream}"
    r = sh(f"ffmpeg -v error -rtsp_transport tcp -i {url} -t {secs} "
           f"-vf crop={w}:{h}:{x}:{y},signalstats,metadata=print:file=- -f null - 2>&1", timeout=60)
    vals = [float(m) for m in re.findall(r"signalstats\.YAVG=([0-9.]+)", r.stdout)]
    return sum(vals) / len(vals) if vals else None


def make_cfg():
    lines = open(BASE_CFG, encoding="utf-8").read().splitlines()
    out, i, sec = [], 0, ""
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("["):
            sec = ln.strip()
        if sec == "[transcode]" and ln.strip().startswith("["):
            out.append(ln)
            out += ["enable=0", "audio_codec=", "video_codec=", "on_demand=0", "sample_rate=0",
                    "bitrate=0", "video_width=0", "video_height=0", "video_fps=0",
                    "video_bitrate=0", "video_scale=0", "video_filter="]
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                i += 1
            sec = ""
            continue
        if ln.strip().startswith("port=") and sec in ("[rtmp]", "[rtsp]", "[http]"):
            out.append(f"port={ {'[rtmp]': RTMP, '[rtsp]': RTSP, '[http]': HTTP}[sec] }")
            i += 1
            continue
        out.append(ln)
        i += 1
    txt = re.sub(r"(?m)^on_demand=\d", "on_demand=0", "\n".join(out) + "\n")
    open(CFG, "w", encoding="utf-8").write(txt)


def start():
    for p in (RTMP, RTSP, HTTP):
        pid = lpid(p)
        if pid:
            sh(f"kill -9 {pid}")
            time.sleep(0.6)
    p = subprocess.Popen([BIN, "-c", CFG, "-l", "1"], cwd=BIN_DIR,
                         stdout=open(LOG, "w"), stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
    for _ in range(60):
        time.sleep(0.25)
        if p.poll() is None and lpid(RTSP) == p.pid:
            return p
    raise RuntimeError("MediaServer 没起来，看 " + LOG)


def stop(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception:
        pass
    time.sleep(1.0)


def push(stream):
    return subprocess.Popen(["ffmpeg", "-re", "-stream_loop", "-1", "-i", SRC, "-c", "copy",
                             "-f", "rtsp", "-rtsp_transport", "tcp",
                             f"rtsp://127.0.0.1:{RTSP}/live/{stream}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=os.setsid)


def run_case(name, params, region=None):
    """下发参数 -> 推流 -> 取统计。每例独立流名（宽限期内重推会接管旧参数）。

    region=(w,h,x,y) 时额外返回该矩形区域的 luma 均值 —— 必须在推流仍存活时取，
    用例结束后推流端被 kill，再回头采样只会拿到 None（实测踩过）。
    """
    api("addTranscode", vhost="__defaultVhost__", app="live", stream=name, reset=1)
    r = api("addTranscode", vhost="__defaultVhost__", app="live", stream=name, **params)
    if r.get("code") != 0:
        return None, None, [f"接口失败 {r}"], None
    pr = push(name)
    time.sleep(6)
    dims, yavg, errs = stats(name)
    ravg = region_yavg(name, *region) if region else None
    try:
        os.killpg(os.getpgid(pr.pid), signal.SIGKILL)
    except Exception:
        pass
    time.sleep(1.5)
    return dims, yavg, errs, ravg


def main():
    print("=" * 74)
    print("视频滤镜验收（判据：拉流端 luma 均值 + 尺寸 + 解码错误）")
    print("=" * 74)
    if not os.path.exists(SRC):
        print(f"缺素材 {SRC}")
        return 2
    make_cfg()
    srv = start()
    try:
        # 基线：H265 目标、无滤镜
        d0, y0, e0, _ = run_case("f_base", {"video_codec": "H265"})
        note(d0 == (1280, 720) and not e0, "基线（无滤镜）可解", f"{d0} YAVG={y0}")

        # negate：亮度应反相
        d1, y1, e1, _ = run_case("f_negate", {"video_codec": "H265", "filter": "negate"})
        note(d1 == (1280, 720), "negate 不改变尺寸", f"{d1}")
        ok = d1 == (1280, 720) and y0 is not None and y1 is not None and abs(y1 - (255 - y0)) < 12
        note(ok, "negate 生效（YAVG ≈ 255-原始）", f"原始 {y0:.1f} -> 反相 {y1:.1f}")
        note(not e1, "negate 后拉流 0 硬错误", f"{len(e1)} 条")

        # drawtext：OSD 文字，亮度分布应变化
        desc = ("drawtext=text='CAM-01 AEGIS':x=40:y=40:fontsize=64:fontcolor=white:"
                "box=1:boxcolor=black@0.8:boxborderw=8")
        d2, y2, e2, _ = run_case("f_osd", {"video_codec": "H265", "filter": desc})
        note(d2 == (1280, 720), "drawtext 不改变尺寸", f"{d2}")
        note(y2 is not None and y0 is not None and abs(y2 - y0) > 1.0,
             "drawtext 生效（YAVG 有可见变化）", f"{y0:.1f} -> {y2:.1f}")
        note(not e2, "drawtext 后拉流 0 硬错误", f"{len(e2)} 条")
        log = open(LOG, errors="replace").read()
        note("transcode filter ready" in log, "服务端确认滤镜图已建")

        # ---- logo 角标（overlay 双输入）----
        LOGO = "/tmp/logo_wm.png"
        TR = (240, 100, 1020, 20)     # 右上角：x=1020(距右 20) y=20
        # y=-120 表示「距底边 120」：logo 高 80，所以实际覆盖 y=520..600。
        # 区域必须落在 logo 身上，量到旁边就永远「没有变化」（实测踩过）。
        BL = (240, 90, 20, 520)

        # 基线：同一份源码、不开 logo，在推流存活时取区域亮度
        d0b, _, e0b, base_tr = run_case("f_base_tr", {"video_codec": "H265"}, region=TR)
        _, _, _, base_bl = run_case("f_base_bl", {"video_codec": "H265"}, region=BL)
        note(base_tr is not None and base_bl is not None, "取到区域基线",
             f"右上 {base_tr} / 左下 {base_bl}")

        d5, _, e5, tr5 = run_case("f_logo_tr", {"video_codec": "H265", "logo": LOGO,
                                                "logo_x": -20, "logo_y": 20,
                                                "logo_width": 240}, region=TR)
        note(d5 == (1280, 720), "logo 不改变尺寸", f"{d5}")
        note(tr5 is not None and base_tr is not None and abs(tr5 - base_tr) > 15,
             "logo 叠加到右上角（区域 YAVG 明显变化）",
             f"基线 {base_tr and round(base_tr, 1)} -> {tr5 and round(tr5, 1)}")
        note(not e5, "logo 后拉流 0 硬错误", f"{len(e5)} 条")
        note("transcode logo ready" in open(LOG, errors="replace").read(),
             "服务端确认 logo 图已建")

        # 负 y：贴底边
        d6, _, _, bl = run_case("f_logo_bl", {"video_codec": "H265", "logo": LOGO,
                                              "logo_x": 20, "logo_y": -120,
                                              "logo_width": 240}, region=BL)
        note(d6 == (1280, 720) and bl is not None and base_bl is not None
             and abs(bl - base_bl) > 15, "负坐标贴底边生效",
             f"基线 {base_bl and round(base_bl, 1)} -> {bl and round(bl, 1)}")

        # logo 文件不存在：降级为不叠加，流必须照常
        d7, _, e7, _ = run_case("f_logo_missing", {"video_codec": "H265",
                                                   "logo": "/tmp/no_such_logo.png"})
        note(d7 == (1280, 720), "logo 文件缺失时流照常", f"{d7}")

        # 非法滤镜名：不能把流搞死
        d3, y3, e3, _ = run_case("f_bad", {"video_codec": "H265", "filter": "no_such_filter=1"})
        note(d3 == (1280, 720), "非法滤镜不影响出流（尺寸不变）", f"{d3}")
        note(y3 is not None and abs(y3 - y0) < 3, "非法滤镜时按原样直通", f"YAVG={y3}")

        # 改变尺寸的滤镜必须被拒绝（否则编码器 EINVAL）
        d4, y4, e4, _ = run_case("f_crop", {"video_codec": "H265", "filter": "crop=iw:ih/2:0:0"})
        note(d4 == (1280, 720), "改变尺寸的滤镜被拒，输出仍是原尺寸", f"{d4}")
        note("不一致" in open(LOG, errors="replace").read(), "服务端给出了可读原因")
    finally:
        stop(srv)

    print()
    print("=" * 74)
    print(f"结果：PASS {len(PASS)} / FAIL {len(FAIL)}")
    for x in FAIL:
        print("  -", x)
    print("=" * 74)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
