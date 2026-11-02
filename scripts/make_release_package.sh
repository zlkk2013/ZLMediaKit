#!/bin/bash
# 打交付包：二进制 + 配置 + 安装/自检脚本 + 文档 → tar.gz（含 SHA256SUMS）
#
# 在**目标平台上**执行（Linux 包就必须在 Linux 上打，二进制不能跨平台）。
# 用法：
#   ./scripts/make_release_package.sh                 # 用 release/linux/Release/MediaServer
#   ./scripts/make_release_package.sh --bin /path/to/MediaServer --out /tmp/pkg
#
# 产物：<out>/zlm-transcode-<日期>-<ffmpeg版本>.tar.gz
set -euo pipefail

WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$WT/release/linux/Release/MediaServer"
OUT="${TMPDIR:-/tmp}"
while [ $# -gt 0 ]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

[ -x "$BIN" ] || { echo "找不到二进制：$BIN（先构建）"; exit 1; }

# ffmpeg 版本（写进包名与清单，现场排障第一件事就是问这个）
FFVER=$(ldd "$BIN" 2>/dev/null | grep -o 'libavcodec\.so\.[0-9]*' | head -1 || echo "unknown")
FFVER=${FFVER:-unknown}
STAMP=$(date +%Y%m%d)
NAME="zlm-transcode-${STAMP}-${FFVER}"
STAGE="$OUT/$NAME"

echo "== 打包装到 $STAGE =="
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,config,systemd,docs}

install -m 0755 "$BIN" "$STAGE/bin/MediaServer"
install -m 0755 "$WT/deploy/linux/install.sh" "$STAGE/install.sh"
install -m 0755 "$WT/deploy/linux/check.sh"   "$STAGE/check.sh"
install -m 0644 "$WT/deploy/linux/systemd/zlmediakit-transcode.service" "$STAGE/systemd/"
install -m 0644 "$WT/conf/config.ini" "$STAGE/config/config.ini"

# 包根目录的 README = 交付包说明（装好前先看这个）
[ -f "$WT/docs/DELIVERY_PACKAGE_README.md" ] && install -m 0644 "$WT/docs/DELIVERY_PACKAGE_README.md" "$STAGE/README.md"
# 合规：ZLMediaKit 是 MIT-like，必须随包保留许可与版权声明
[ -f "$WT/LICENSE" ] && install -m 0644 "$WT/LICENSE" "$STAGE/LICENSE"

# 交付文档（缺哪个就跳过，不因为文档缺失打断打包）
for f in DELIVERY_CONFIG_REFERENCE.md DELIVERY_OPS_RUNBOOK.md DELIVERY_DEPLOY.md \
         DELIVERY_PACKAGE_README.md; do
    [ -f "$WT/docs/$f" ] && install -m 0644 "$WT/docs/$f" "$STAGE/docs/"
done

# 把 [transcode] 段以「可直接粘贴」的形式附在配置说明里
cat > "$STAGE/config/TRANSCODE_SNIPPET.ini" <<'EOF'
# 把下面这段粘到 config.ini 末尾（或已有的 [transcode] 段里），然后重启服务。
# 默认关闭：enable=0 时行为与原版完全一致，可作为基线对照。
[transcode]
enable=1                 ; 总开关
video_codec=h265         ; 目标视频编码：h264/h265/vp8/vp9/av1，留空=不转视频
audio_codec=opus         ; 目标音频编码：opus/aac/g711a/g711u/mp3/g722，留空=不转音频
on_demand=1              ; 没人看就不转码
; video_scale=0.5        ; 分辨率倍数（0.1~4.0），显式宽高优先
; video_fps=15           ; 目标帧率，降帧时会同步改时间轴
; video_bitrate=2000000  ; 视频码率（bps），0=按分辨率自动估
; video_filter=drawtext=text='CAM-07':x=20:y=20:fontsize=48:fontcolor=white
; video_logo=/opt/zlm/logo.png
; hw_encode=1            ; 优先硬件编码（NVENC/VideoToolbox），失败静默回落软件
; hw_decode=1            ; 优先硬件解码
; enable_silence=1       ; 源端断流时补静音帧（避免观众断流）
EOF

( cd "$STAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )

TAR="$OUT/$NAME.tar.gz"
tar -C "$OUT" -czf "$TAR" "$NAME"
echo "== 完成 =="
echo "包：$TAR"
echo "大小：$(du -h "$TAR" | cut -f1)"
echo
echo "解开后核对："
echo "  tar -xzf $(basename "$TAR") && cd $NAME && sha256sum -c SHA256SUMS"
