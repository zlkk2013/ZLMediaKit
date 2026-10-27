#!/usr/bin/env python3
"""AV1 over RTSP 的独立验证器（因为 ffmpeg 的 RTSP 客户端没有 AV1 解包器）。

背景
----
服务端把 AV1 按 RFC 9628（AV1 RTP Payload Format）打成聚合头 + OBU 元素。但
`ffprobe -i rtsp://...` 对 `a=rtpmap:96 AV1/90000` 直接判定为 unknown ——
ffmpeg 7.1 的 libavformat 只注册了 H264/H265/VP8/VP9 的 RTP 解包器（实测
`strings libavformat | grep -x AV1` 无命中，VP8/VP9 有）。所以「ffmpeg 拉不到」
并不能证明服务端错，必须自己解包。

本脚本做的事
-----------
1. 裸 socket 走 RTSP：DESCRIBE → SETUP(interleaved TCP) → PLAY
2. 按 RFC 9628 解析聚合头（Z/Y/W/N）与 OBU 元素（除最后一个外都有 leb128 长度）
3. 跨包重组被切开的 OBU，按 RTP 时间戳聚成一个 temporal unit
4. 每个 temporal unit 写成一个 IVF 帧，交给 ffmpeg 解码，统计帧数与错误

判据：解出的帧数 ≈ 时长 × 帧率，且解码 0 错误。

用法
----
    python3 scripts/av1_rtp_verify.py rtsp://127.0.0.1:60554/live/xxx [秒数] [宽] [高]
"""
import socket
import struct
import subprocess
import sys
import tempfile
import os

FF = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffmpeg")
FP = os.path.expanduser("~/miniconda3/envs/zlm-ffmpeg/bin/ffprobe")


def rtsp(url, seconds=5.0):
    """返回 (sdp, rtp_payloads)。rtp_payloads 是 [(rtp_stamp, payload_bytes), ...]"""
    # 拆 url -> host:port/path
    assert url.startswith("rtsp://")
    rest = url[len("rtsp://"):]
    hostport, _, path = rest.partition("/")
    host, _, port = hostport.partition(":")
    port = int(port or 554)
    path = "/" + path.rstrip("/")

    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(3)

    def recv_response():
        data = b""
        while b"\r\n\r\n" not in data:
            b = sock.recv(65536)
            if not b:
                break
            data += b
        head, _, tail = data.partition(b"\r\n\r\n")
        head_txt = head.decode(errors="replace")
        clen = 0
        for line in head_txt.splitlines():
            if line.lower().startswith("content-length:"):
                clen = int(line.split(":", 1)[1].strip())
        body = tail
        while len(body) < clen:
            b = sock.recv(65536)
            if not b:
                break
            body += b
        # interleaved 数据可能已经跟着来了，先丢掉（DESCRIBE 阶段不应该有）
        return head_txt, body[:clen]

    def send(msg):
        sock.sendall(msg.encode())

    send(f"DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n")
    head, sdp = recv_response()
    assert "200 OK" in head, head

    control = None
    for line in sdp.decode(errors="replace").splitlines():
        if line.startswith("a=control:trackID=") and control is None:
            control = line.split(":", 1)[1]  # trackID=0
    assert control, "no video track in SDP"

    track_url = f"{url}/{control}"
    send(f"SETUP {track_url} RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n")
    head, _ = recv_response()
    assert "200 OK" in head, head
    session = ""
    for line in head.splitlines():
        if line.lower().startswith("session:"):
            session = line.split(":", 1)[1].strip().split(";")[0]

    send(f"PLAY {url} RTSP/1.0\r\nCSeq: 3\r\nSession: {session}\r\nRange: npt=0.000-\r\n\r\n")
    head, _ = recv_response()
    assert "200 OK" in head, head

    # 读 interleaved 帧
    payloads = []
    buf = b""
    deadline = None
    import time
    start = time.time()
    while time.time() - start < seconds:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        while True:
            if len(buf) < 4:
                break
            if buf[0:1] != b"$":
                # 不是 interleaved，丢弃一个字节重新同步
                idx = buf.find(b"$")
                buf = buf[idx:] if idx >= 0 else b""
                if len(buf) < 4:
                    break
            ch = buf[1]
            length = (buf[2] << 8) | buf[3]
            if len(buf) < 4 + length:
                break
            pkt = buf[4:4 + length]
            buf = buf[4 + length:]
            if ch != 0:
                continue  # RTCP
            if len(pkt) < 12:
                continue
            csrc = pkt[0] & 0x0F
            off = 12 + 4 * csrc
            if pkt[0] & 0x10:  # extension
                if len(pkt) < off + 4:
                    continue
                ext_len = struct.unpack("!H", pkt[off + 2:off + 4])[0]
                off += 4 + 4 * ext_len
            stamp = struct.unpack("!I", pkt[4:8])[0]
            payloads.append((stamp, pkt[off:]))
    try:
        send(f"TEARDOWN {url} RTSP/1.0\r\nCSeq: 4\r\nSession: {session}\r\n\r\n")
    except Exception:
        pass
    sock.close()
    return sdp.decode(errors="replace"), payloads


def leb128_read(buf, i):
    val = 0
    shift = 0
    while i < len(buf):
        b = buf[i]
        i += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return val, i
    return None, i


def leb128_write(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def depacketize(payloads):
    """按 RFC 9628 解包，返回 [(rtp_stamp, obu_stream_bytes), ...]"""
    units = []          # 每个 temporal unit 一段 OBU 流
    pending = b""       # 跨包未完成的 OBU
    continuing = False  # 上一个包最后一个元素还没结束（Y=1）
    cur_stamp = None
    cur = bytearray()

    def flush_obu(obu):
        if not obu:
            return
        hdr = obu[0]
        body = obu[1:]
        if hdr & 0x04 and body:      # obu_extension_flag：扩展字节属于头
            ext = body[0:1]
            body = body[1:]
            # 头里保留 extension 位，扩展字节跟在头后面
            cur.extend(bytes([hdr | 0x02]) + ext + leb128_write(len(body)) + body)
        else:
            cur.extend(bytes([hdr | 0x02]) + leb128_write(len(body)) + body)

    for stamp, pl in payloads:
        if not pl:
            continue
        if stamp != cur_stamp:
            if pending:
                flush_obu(pending)
                pending = b""
            if cur_stamp is not None and cur:
                units.append((cur_stamp, bytes(cur)))
                cur = bytearray()
            cur_stamp = stamp
        agg = pl[0]
        z = bool(agg & 0x80)
        y = bool(agg & 0x40)
        w = (agg & 0x30) >> 4
        rest = pl[1:]
        elements = []
        i = 0
        if w == 0:
            # W=0 表示元素个数 >3 或未知：除最后一个外都带长度
            while i < len(rest):
                size, j = leb128_read(rest, i)
                if size is None or j + size >= len(rest):
                    elements.append(rest[i:])
                    break
                elements.append(rest[j:j + size])
                i = j + size
        else:
            for k in range(w):
                if k == w - 1:
                    elements.append(rest[i:])
                else:
                    size, j = leb128_read(rest, i)
                    if size is None or j + size > len(rest):
                        elements.append(rest[i:])
                        i = len(rest)
                        break
                    elements.append(rest[j:j + size])
                    i = j + size
        for k, el in enumerate(elements):
            if k == 0 and z and continuing:
                pending += el
            else:
                if pending:
                    flush_obu(pending)
                pending = el
            continuing = (k == len(elements) - 1) and y
            if not continuing:
                flush_obu(pending)
                pending = b""
        if not continuing and pending:
            flush_obu(pending)
            pending = b""
    if pending:
        flush_obu(pending)
    if cur:
        units.append((cur_stamp, bytes(cur)))
    return units


def write_ivf(path, units, width, height, fps):
    with open(path, "wb") as f:
        f.write(b"DKIF")
        f.write(struct.pack("<HH", 0, 32))
        f.write(b"AV01")
        f.write(struct.pack("<HH", width, height))
        f.write(struct.pack("<II", fps, 1))
        f.write(struct.pack("<I", len(units)))
        f.write(struct.pack("<I", 0))
        for idx, (_, data) in enumerate(units):
            f.write(struct.pack("<IQ", len(data), idx))
            f.write(data)


def main():
    url = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 320
    height = int(sys.argv[4]) if len(sys.argv) > 4 else 240
    fps = int(sys.argv[5]) if len(sys.argv) > 5 else 15

    sdp, payloads = rtsp(url, seconds)
    rtpmap = [l for l in sdp.splitlines() if l.startswith("a=rtpmap:")]
    print(f"RTP 包数: {len(payloads)}, SDP rtpmap: {rtpmap}")
    if not payloads:
        print("FAIL: 没收到 RTP 包")
        return 1
    units = depacketize(payloads)
    print(f"temporal unit 数: {len(units)}（期望 ≈ {seconds * fps:.0f}）")

    # 中途接入时第一段是半个 GOP（没有 sequence header），播放器也要等到关键帧
    # 才能起解 —— 这里同样只从第一个带 sequence header 的时间单元开始算。
    # Joining mid-stream means the first units are half a GOP with no sequence header;
    # a player also waits for a keyframe. Start from the first unit that carries one.
    def first_obu_type(buf):
        return (buf[0] >> 3) & 0xF if buf else -1

    skip = 0
    for i, (_, data) in enumerate(units):
        if first_obu_type(data) == 1:
            skip = i
            break
    if skip:
        print(f"跳过 {skip} 个中途接入的残缺时间单元（无 sequence header）")
        units = units[skip:]
    if not units:
        print("FAIL: 整个拉流窗口内没等到关键帧（sequence header）")
        return 1

    ivf = tempfile.mktemp(suffix=".ivf")
    write_ivf(ivf, units, width, height, fps)

    r = subprocess.run([FP, "-v", "error", "-show_entries",
                        "stream=codec_name,width,height", "-of", "csv=p=0", ivf],
                       capture_output=True, text=True)
    print("重组后 ffprobe:", r.stdout.strip() or r.stderr.strip())

    r = subprocess.run([FF, "-v", "error", "-nostats", "-progress", "pipe:1",
                        "-i", ivf, "-f", "null", "-"], capture_output=True, text=True)
    frames = 0
    for line in r.stdout.splitlines():
        if line.startswith("frame="):
            try:
                frames = max(frames, int(line.split("=")[1]))
            except ValueError:
                pass
    errors = [l for l in r.stderr.splitlines() if l.strip()]
    print(f"解码帧数: {frames}, 解码错误: {len(errors)} {errors[:2]}")
    ok = frames >= seconds * fps * 0.8 and not errors
    print("PASS" if ok else "FAIL")
    if ok:
        os.unlink(ivf)
    else:
        print("保留重组文件:", ivf)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
