#!/bin/bash
# 静音兜底实测（raw PCM 判据）
# Silence fallback test, raw-PCM evidence.
#
# 为什么用裸 PCM 而不是 wav/ogg/mkv：
#   - wav/mp4 的头里要写总长度，进程被信号打断就不落盘（实测 wav 0 字节但 ffmpeg
#     报已写 241kB），"文件大小"这个判据直接失真；
#   - ogg/mkv 的写入路径会因源时间戳抖动打 "Non-monotonous DTS"，那个告警与本次
#     功能无关（enable_silence=0 时同样有 194 次），会污染证据；
#   - s16le 裸流按采样顺序顺序追加字节，没有头、没有容器时间戳，文件大小
#     就等于"收到多少音频"，是判断"断流后还在不在收音频"最干净的信号。
# Why raw PCM rather than wav/ogg/mkv:
#   - wav/mp4 write the total length into the header, so a signalled process leaves
#     a 0-byte file even after ffmpeg reported 241kB written — the size criterion
#     becomes meaningless;
#   - ogg/mkv emit "Non-monotonous DTS" on source timestamp jitter, unrelated to this
#     feature (194 occurrences with enable_silence=0 too), polluting the evidence;
#   - raw s16le appends bytes in sample order with no header and no container
#     timestamps, so size == "audio received" — the cleanest stall signal.
#
# 判据 / Criteria:
#   1. 观众端进程在 pusher 被杀后仍存活到 -t 到期
#   2. kill 之后 PCM 字节持续增长（每 2s 采样一次）
#   3. kill 之后段落 mean_volume << -60dB；直播段约 -20dB
#
# 用法 / Usage: p6_silence_test.sh <name> <pull_seconds> <kill_at_seconds>
set -u
NAME="${1:-sil}"
DUR_PULL="${2:-22}"
KILL_AT="${3:-8}"
RTMP=11900
RTSP=10554
SECRET=5j9gfMmxP1l7Ox1lrhUm1l3SsWA5dz4z
OUT=/tmp/${NAME}_pull.s16
PLOG=/tmp/pull_${NAME}.log
ULOG=/tmp/push_${NAME}.log
RESULT=/tmp/${NAME}_result.txt

rm -f "$OUT" "$PLOG" "$ULOG" "$RESULT"
exec > >(tee -a "$RESULT") 2>&1

ffmpeg -re -stream_loop 40 -i /tmp/av_test.mp4 -c copy -f flv \
    "rtmp://127.0.0.1:${RTMP}/live/${NAME}" > "$ULOG" 2>&1 &
PUSH_PID=$!
disown $PUSH_PID 2>/dev/null

for i in $(seq 1 80); do
    curl -s "http://127.0.0.1:14080/index/api/getMediaList?secret=${SECRET}&stream=${NAME}" \
        | grep -q '"codec_id"' && break
    sleep 0.25
done

# 16kHz 单声道 s16le：48k/2ch 的 1/6 大小，音量判据不受影响
ffmpeg -rtsp_transport tcp -i "rtsp://127.0.0.1:${RTSP}/live/${NAME}" \
    -vn -ac 1 -ar 16000 -c:a pcm_s16le -f s16le - > "$OUT" 2> "$PLOG" &
PULL_PID=$!

# 裸 PCM 没有头，靠脚本自己限时收尾（-t 对 raw muxer 无效，实测输出 0 字节）
# Raw PCM has no header; the script enforces the duration itself
# (-t is a no-op for the raw muxer, measured as a 0-byte output)
( sleep $((DUR_PULL)); kill -TERM $PULL_PID 2>/dev/null; sleep 3; kill -9 $PULL_PID 2>/dev/null; echo "STOP_AT_DURATION" >> "$PLOG" ) &
WATCH_PID=$!

sleep "$KILL_AT"
KILL_EPOCH=$(date +%s)
echo "KILL_AT=${KILL_AT}s pusher_pid=${PUSH_PID}"
kill -9 $PUSH_PID 2>/dev/null

echo "--- PCM size every 2s after kill (16000Hz mono s16 = 32000 B/s) ---"
for i in 1 2 3 4 5 6; do
    sleep 2
    SZ=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
    ALIVE=$(kill -0 $PULL_PID 2>/dev/null && echo yes || echo no)
    echo "t+$((i*2))s size=${SZ} viewer_alive=${ALIVE}"
done

kill -0 $PULL_PID 2>/dev/null && echo "VIEWER_ALIVE_AT_END=yes" || echo "VIEWER_ALIVE_AT_END=no"
wait $PULL_PID 2>/dev/null
echo "PULL_EXIT=$?"
kill -9 $WATCH_PID 2>/dev/null

echo "--- viewer ffmpeg notable lines ---"
grep -E "error|Error|failed|WATCHDOG|teardown|Connection refused|END" "$PLOG" | tail -5
FINAL_SZ=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
echo "FINAL_PCM_BYTES=${FINAL_SZ}"
echo "FINAL_PCM_SECONDS=$((FINAL_SZ / 32000))"
echo "--- loudness AFTER kill (expect << -60dB) ---"
ffmpeg -v info -ss "$KILL_AT" -f s16le -ar 16000 -ac 1 -i "$OUT" -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume|Error"
echo "--- loudness BEFORE kill (live, expect ~ -20dB) ---"
ffmpeg -v info -t 5 -f s16le -ar 16000 -ac 1 -i "$OUT" -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume|Error"
echo "DONE"
