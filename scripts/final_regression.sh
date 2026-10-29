#!/usr/bin/env bash
# 转码改动的全量回归入口。
#
# 目的：不要每次改完代码手工记「该跑哪几个脚本」。这个脚本按固定顺序跑完、
# 输出一张汇总表，任何一项失败都以非 0 退出。
#
# 用法：
#   ./scripts/final_regression.sh            # 核心 9 套（约 20~25 分钟）
#   ./scripts/final_regression.sh --fast     # 跳过慢的 3 套（约 5 分钟，改代码时高频用）
#   ./scripts/final_regression.sh --all      # 核心 + 历史分支专用 8 套
#   ./scripts/final_regression.sh --list     # 只列出套件与端口
#   ./scripts/final_regression.sh camera_scenario_test joint_accept   # 只跑指定几个
#
# 纪律（踩过的坑）：
#   1) 先核对二进制比源码新 —— 编译成功不等于在跑新代码，旧进程/旧二进制会让
#      所有结论失真，而且不会报错。
#   2) 每个套件跑前清掉自己端口上的残留进程（只杀自己的端口段，不 pkill -f MediaServer，
#      否则多 worktree 并行会互相误杀）。
#   3) macOS 没有 GNU timeout，这里用「后台 sleep + kill」自己实现，避免某套卡死把整轮拖住。
#   4) 各套件用 env -u PYTHONPATH -u PYTHONHOME 运行：Hermes 注入的 PYTHONPATH 与本机
#      解释器冲突（pydantic_core 缺失一类）。

set -u
cd "$(dirname "$0")/.." || exit 1
WT=$(pwd)
BIN="$WT/release/darwin/Release/MediaServer"
LOGDIR="$WT/logs/regression/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOGDIR"

# 套件名 : 超时秒数 : 说明
CORE=(
  "camera_scenario_test:420:现场拓扑：RTSP 摄像机推流 -> 转码 -> 观众拉流"
  "rtsp_reconnect_test:480:断连续推：kill 推流端后重连不再 406，转码仍生效"
  "codec_reverse_test:420:反向方向：H265->H264、G711A/MP3 作源"
  "scale_param_test:420:分辨率倍数与 API 宽高（拉流端实际尺寸判据）"
  "vp8_source_timeline_test:420:VP8/VP9 作源的时间轴（防 2 倍速）"
  "joint_accept:600:联合验收：API/视频参数/静音兜底/多轨并发"
  "source_release_check:300:媒体源释放：断开后源必须清空（泄漏检查）"
  "teardown_liveness_test:420:存活性：反复重推热改不卡死（死锁检查）"
  "audio_matrix_test:900:音频矩阵：5 源规格 x 5 目标 codec = 125 项"
  "av1_fmp4_stability:600:AV1+fmp4 偶发崩溃：8 轮重复"
)
FAST_SKIP="joint_accept audio_matrix_test av1_fmp4_stability"

# 历史分支专用：端口各自独立，依赖对应 worktree 的构建，主线一般不跑
EXTRA=(
  "rtsp_repush_check:420:重推检查"
  "rtsp_transcode_check:420:RTSP 转码检查"
  "av1_rtp_verify:420:AV1 RTP 拉流"
  "codec_source_matrix_test:600:源编码矩阵"
  "video_codec_matrix_test:600:视频编码矩阵"
  "a1_accept:600:音频 codec 线（worktree 端口）"
  "a2_accept:600:视频参数线（worktree 端口）"
  "a3_accept:600:API/配置线（worktree 端口）"
)

MODE=core
SELECTED=()
for arg in "$@"; do
  case "$arg" in
    --fast) MODE=fast ;;
    --all)  MODE=all ;;
    --list) MODE=list ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) SELECTED+=("$arg") ;;
  esac
done

if [ "$MODE" = list ]; then
  echo "核心套件："
  for s in "${CORE[@]}"; do IFS=: read -r n t d <<<"$s"; printf "  %-26s %4ss  %s\n" "$n" "$t" "$d"; done
  echo "历史分支专用（--all 才跑）："
  for s in "${EXTRA[@]}"; do IFS=: read -r n t d <<<"$s"; printf "  %-26s %4ss  %s\n" "$n" "$t" "$d"; done
  exit 0
fi

# ---------- 前置检查：二进制是否比源码新 ----------
preflight() {
  echo "==================== 前置检查 ===================="
  if [ ! -x "$BIN" ]; then
    echo "❌ 找不到可执行文件：$BIN"
    return 1
  fi
  local newest_src
  newest_src=$(find "$WT/src" "$WT/server" -newer "$BIN" -type f \( -name '*.cpp' -o -name '*.h' \) 2>/dev/null | head -5)
  if [ -n "$newest_src" ]; then
    echo "❌ 有源码比二进制新，说明跑的是旧二进制（结论会全是假的）："
    echo "$newest_src" | sed 's/^/     /'
    echo "   先执行： cmake --build build-wt -j6 --target MediaServer"
    return 1
  fi
  echo "✅ 二进制比源码新：$(/usr/bin/stat -f '%Sm' "$BIN")"
  # 残留 MediaServer 必须清掉再开跑 —— 上一轮被打断（Ctrl-C / 工具超时）留下的孤儿进程
  # 会占着端口，导致后续套件「启动失败」，而症状看起来像产品坏了（Connection refused）。
  #
  # 只杀「命令行里就是本 worktree 这个二进制」的进程：这是精确匹配，不会误伤其它 worktree
  # 的实例（那些路径不同）。绝不用 pkill -f MediaServer 这种模糊匹配。
  local mine
  mine=$(ps -axo pid=,command= | grep -F "$BIN" | grep -v grep | awk '{print $1}')
  if [ -n "$mine" ]; then
    echo "🧹 清理本 worktree 的残留进程：$(echo $mine | tr '\n' ' ')"
    echo "$mine" | xargs -r kill -9 2>/dev/null
    sleep 1
  fi
  local busy=""
  for port in 56900 56554 57080 63554 64080; do
    p=$(/usr/sbin/lsof -nP -tiTCP:$port -sTCP:LISTEN 2>/dev/null | head -1)
    [ -n "$p" ] && busy="$busy $port(pid=$p)"
  done
  if [ -n "$busy" ]; then
    echo "⚠️  以下端口被别的进程占着（不是本 worktree 的 MediaServer）：$busy"
  else
    echo "✅ 相关端口空闲"
  fi
  return 0
}

# 带超时执行：macOS 无 GNU timeout，「后台 sleep + kill」自救
run_with_timeout() {
  local tmo=$1; shift
  local log=$1; shift
  ( env -u PYTHONPATH -u PYTHONHOME python3 "$@" >"$log" 2>&1 ) &
  local pid=$!
  ( sleep "$tmo"; kill -9 "$pid" 2>/dev/null ) &
  local watcher=$!
  wait "$pid" 2>/dev/null
  local rc=$?
  kill -9 "$watcher" 2>/dev/null
  wait "$watcher" 2>/dev/null
  return $rc
}

suite_line() {   # $1=name -> 从 CORE/EXTRA 里取超时和说明
  local name=$1
  for s in "${CORE[@]}" "${EXTRA[@]}"; do
    IFS=: read -r n t d <<<"$s"
    [ "$n" = "$name" ] && { echo "$t:$d"; return 0; }
  done
  echo "600:未登记的套件"
}

if ! preflight; then
  echo
  echo "前置检查未通过，已中止（避免产出假结论）。"
  exit 2
fi

# ---------- 组装本轮要跑的列表 ----------
TO_RUN=()
if [ ${#SELECTED[@]} -gt 0 ]; then
  TO_RUN=("${SELECTED[@]}")
else
  for s in "${CORE[@]}"; do
    IFS=: read -r n t d <<<"$s"
    if [ "$MODE" = fast ] && [[ " $FAST_SKIP " == *" $n "* ]]; then continue; fi
    TO_RUN+=("$n")
  done
  if [ "$MODE" = all ]; then
    for s in "${EXTRA[@]}"; do IFS=: read -r n t d <<<"$s"; TO_RUN+=("$n"); done
  fi
fi

echo
echo "==================== 开始回归 ===================="
echo "套件数：${#TO_RUN[@]}    日志目录：$LOGDIR"
echo

RESULT_NAMES=(); RESULT_STATUS=(); RESULT_SUM=(); RESULT_SEC=()
FAILED=0
START_ALL=$(date +%s)

for name in "${TO_RUN[@]}"; do
  script="$WT/scripts/$name.py"
  if [ ! -f "$script" ]; then
    echo "❌ $name：脚本不存在，跳过"
    RESULT_NAMES+=("$name"); RESULT_STATUS+=("MISSING"); RESULT_SUM+=("-"); RESULT_SEC+=("0")
    FAILED=$((FAILED + 1)); continue
  fi
  meta=$(suite_line "$name"); IFS=: read -r tmo desc <<<"$meta"
  log="$LOGDIR/$name.log"
  printf "▶ %-26s %s\n" "$name" "$desc"
  t0=$(date +%s)
  run_with_timeout "$tmo" "$log" "scripts/$name.py"
  rc=$?
  dt=$(( $(date +%s) - t0 ))
  sum=$(grep -oE '结果：[^=]*' "$log" | tail -1 | sed 's/结果：//;s/[[:space:]]*$//')
  [ -z "$sum" ] && sum=$(grep -oE '通过 [0-9]+ / 失败 [0-9]+' "$log" | tail -1)
  [ -z "$sum" ] && sum="-"
  if [ $rc -eq 0 ]; then
    st="PASS"; printf "  ✅ %-6s %5ss  %s\n" "$st" "$dt" "$sum"
  else
    st="FAIL"; FAILED=$((FAILED + 1))
    if [ $rc -eq 137 ]; then st="TIMEOUT"; fi
    printf "  ❌ %-6s %5ss  %s   (rc=%d)\n" "$st" "$dt" "$sum" "$rc"
    echo "     失败详情（最后 8 行）："
    tail -8 "$log" | sed 's/^/       /'
  fi
  RESULT_NAMES+=("$name"); RESULT_STATUS+=("$st"); RESULT_SUM+=("$sum"); RESULT_SEC+=("$dt")
  # 给下个套件留出端口释放时间
  sleep 2
done

TOTAL=$(( $(date +%s) - START_ALL ))

echo
echo "==================== 汇总 ===================="
printf "%-28s %-8s %6s  %s\n" "套件" "结果" "耗时" "通过/失败"
printf "%-28s %-8s %6s  %s\n" "----------------------------" "--------" "------" "----------------"
for i in "${!RESULT_NAMES[@]}"; do
  printf "%-28s %-8s %5ss  %s\n" "${RESULT_NAMES[$i]}" "${RESULT_STATUS[$i]}" "${RESULT_SEC[$i]}" "${RESULT_SUM[$i]}"
done
echo "---------------------------------------------------------------"
printf "共 %d 套，失败 %d 套，总耗时 %dm%02ds\n" "${#RESULT_NAMES[@]}" "$FAILED" $((TOTAL / 60)) $((TOTAL % 60))
echo "日志：$LOGDIR"

if [ "$FAILED" -gt 0 ]; then
  echo
  echo "❗ 有失败项。先看对应日志确认是产品问题还是测试脚本的判据问题。"
  exit 1
fi
echo
echo "✅ 全部通过"
exit 0
