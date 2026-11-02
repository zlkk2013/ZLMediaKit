#!/bin/bash
# 装完自检：确认服务在跑、转码能力可用、状态接口能读到运行态。
#
# 用法：
#   ./check.sh                          # 用默认端口与 secret
#   ./check.sh --port 8080 --secret abc
#   ./check.sh --push /path/to/test.mp4 # 额外做一次真实转码冒烟（需要 ffmpeg 命令行）
#   ./check.sh --config /etc/zlmediakit/config.ini   # 从配置文件自动取 secret（推荐）
#
# 判据（每条都会打印结论，失败即非 0 退出）：
#   1. HTTP API 可响应（自死锁不崩溃、只失响应，所以要量耗时）
#   2. 本机 ffmpeg 具备所需编码器（软编 libx264/libx265/libopus）
#   3. 若给了 --push：推流 → 转码 → 拉流，codec 必须真的变了
set -uo pipefail

HOST=127.0.0.1
PORT=57080
SECRET=""
PUSH_SRC=""
RTSP_OVERRIDE=""
CONFIG_FILE=""
PREFIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --secret) SECRET="$2"; shift 2 ;;
        --push) PUSH_SRC="$2"; shift 2 ;;
        --rtsp-port) RTSP_OVERRIDE="$2"; shift 2 ;;
        --config) CONFIG_FILE="$2"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

# 没给 --secret 时从配置文件读（推荐：ZLM 可能改写 secret，读文件最可靠）
if [ -z "$SECRET" ] && [ -n "$CONFIG_FILE" ] && [ -f "$CONFIG_FILE" ]; then
    SECRET=$(grep -m1 '^secret=' "$CONFIG_FILE" | cut -d= -f2-)
fi

# secret 可能从 Windows 编辑过的 config.ini 里带来 CRLF —— 不洗掉会一直「Please login first」
SECRET=$(printf '%s' "$SECRET" | tr -d '\r\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

FAILED=0
ok()   { echo "  ✓ $1"; }
bad()  { echo "  ✗ $1"; FAILED=1; }
note() { echo "  - $1"; }

echo "== 1. HTTP API =="
t0=$(date +%s%N)
RESP=$(curl -s -m 5 "http://$HOST:$PORT/index/api/getServerConfig?secret=$SECRET" || true)
t1=$(date +%s%N)
MS=$(( (t1 - t0) / 1000000 ))
if echo "$RESP" | grep -q '"code"[[:space:]]*:[[:space:]]*0'; then
    ok "getServerConfig 返回 code=0，耗时 ${MS}ms"
    [ "$MS" -gt 3000 ] && bad "响应超过 3 秒（可能已卡住）"
else
    bad "getServerConfig 无响应或返回异常：$(echo "$RESP" | head -c 200)"
    if echo "$RESP" | grep -q "login first"; then
        note "这是 secret 不对（不是服务没起来）。从 config.ini 的 [api] 段取 secret，注意 Windows 编辑会带入 CRLF"
    else
        note "检查：服务是否启动 / 端口是否被占 / secret 是否正确"
    fi
fi

echo "== 2. 本机编码器 =="
if command -v ffmpeg >/dev/null 2>&1; then
    # 先把清单抓成变量再匹配：`ffmpeg ... | grep -q` 在 set -o pipefail 下会被 SIGPIPE(141)
    # 判成失败，导致「明明有编码器却报缺失」（实测踩过）。
    ENCODERS=$(ffmpeg -hide_banner -encoders 2>/dev/null || true)
    for enc in libx264 libx265 libopus; do
        if grep -qw -- "$enc" <<< "$ENCODERS"; then
            ok "有 $enc"
        else
            bad "缺 $enc（软编目标 codec 会不可用）"
        fi
    done
    if grep -q nvenc <<< "$ENCODERS"; then
        note "检测到 NVENC（可开 hw_encode=1）"
    fi
else
    note "命令行 ffmpeg 不存在（只影响推流测试；服务端转码用的是它自己链接的 ffmpeg 库）"
fi

echo "== 3. 转码冒烟 =="
if [ -n "$PUSH_SRC" ]; then
    [ -f "$PUSH_SRC" ] || { bad "素材不存在：$PUSH_SRC"; exit 1; }
    # getServerConfig 返回的 JSON 带缩进与 tab，直接正则匹配容易落空 —— 压掉空白再取。
    # 取不到就退回 554，并把实际解析结果打印出来，免得悄悄用错端口。
    COMPACT=$(printf '%s' "$RESP" | tr -d '[:space:]')
    RTSP_PORT=$(printf '%s' "$COMPACT" | tr ',' '\n' | awk -F: '/"rtsp\.port"/{gsub(/[^0-9]/,"",$2); print $2; exit}')
    [ -n "$RTSP_OVERRIDE" ] && RTSP_PORT="$RTSP_OVERRIDE"
    RTSP_PORT=${RTSP_PORT:-554}
    note "推流/拉流用 RTSP 端口 $RTSP_PORT（自动解析失败时用 --rtsp-port 指定）"
    STREAM="check_$(date +%s)"
    # 参数必须在推流前下发（在线流只写覆盖、下次推流生效）
    curl -s "http://$HOST:$PORT/index/api/addTranscode?secret=$SECRET&vhost=__defaultVhost__&app=live&stream=$STREAM&video_codec=H265" >/dev/null
    ffmpeg -re -i "$PUSH_SRC" -c copy -f rtsp -rtsp_transport tcp "rtsp://$HOST:$RTSP_PORT/live/$STREAM" >/dev/null 2>&1 &
    PUSH_PID=$!
    sleep 8
    GOT=$(ffprobe -v error -rtsp_transport tcp -select_streams v -show_entries stream=codec_name -of csv=p=0 \
          "rtsp://$HOST:$RTSP_PORT/live/$STREAM" 2>/dev/null | head -1)
    STATUS=$(curl -s -m 5 "http://$HOST:$PORT/index/api/getTranscode?secret=$SECRET&vhost=__defaultVhost__&app=live&stream=$STREAM")
    kill "$PUSH_PID" 2>/dev/null
    if [ "$GOT" = "hevc" ]; then
        ok "拉流 codec = hevc（转码生效）"
    else
        bad "拉流 codec = ${GOT:-空}（期望 hevc）"
        note "排查顺序：目标 codec 与源是否相同（同 codec 走透传）/ 是否用 RTSP 拉（RTMP 装不下 HEVC）"
    fi
    echo "$STATUS" | grep -q '"transcoding"[[:space:]]*:[[:space:]]*true' \
        && ok "状态接口 transcoding=true" || bad "状态接口未报告在转码"
else
    note "跳过（加 --push /path/to.mp4 做真实转码冒烟）"
fi

echo
if [ "$FAILED" = "0" ]; then
    echo "结论：自检通过"
    exit 0
else
    echo "结论：有失败项，见上面 ✗"
    exit 1
fi
