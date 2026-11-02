#!/bin/bash
# 转码版 MediaServer 安装脚本（Linux x86_64）
#
# 用法：
#   sudo ./install.sh                 # 默认装到 /opt/zlm，配置到 /etc/zlmediakit
#   sudo ./install.sh --prefix /data/zlm --config-dir /data/zlm/conf
#   sudo ./install.sh --no-service    # 只铺文件，不装 systemd 服务
#
# 做的事：铺二进制与配置 → 建运行目录 → 装 systemd 单元（可选）→ 打印验收命令。
# 幂等：重复执行会覆盖二进制与配置模板，但**不覆盖已有 config.ini**（避免冲掉现场参数）。
set -euo pipefail

PREFIX=/opt/zlm
CONFIG_DIR=/etc/zlmediakit
WITH_SERVICE=1
PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --config-dir) CONFIG_DIR="$2"; shift 2 ;;
        --no-service) WITH_SERVICE=0; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

[ "$(id -u)" = "0" ] || { echo "需要 root（或 sudo）"; exit 1; }
[ -x "$PKG_DIR/bin/MediaServer" ] || { echo "包不完整：缺 bin/MediaServer"; exit 1; }

echo "== 1/5 检查运行依赖 =="
missing=0
for so in libavcodec.so libavformat.so libavfilter.so libswscale.so libswresample.so; do
    if ! ldconfig -p 2>/dev/null | grep -q "$so"; then
        echo "  ✗ 缺 $so —— 装一下：apt install libavcodec-dev libavformat-dev libavfilter-dev libswscale-dev libswresample-dev"
        missing=1
    else
        echo "  ✓ $so"
    fi
done
[ "$missing" = "0" ] || { echo "依赖不全，先装依赖再跑本脚本"; exit 1; }

echo "== 2/5 铺文件到 $PREFIX =="
mkdir -p "$PREFIX/bin" "$PREFIX/www" "$PREFIX/log" "$CONFIG_DIR"
install -m 0755 "$PKG_DIR/bin/MediaServer" "$PREFIX/bin/MediaServer"
echo "  ✓ 二进制：$PREFIX/bin/MediaServer"

if [ -f "$CONFIG_DIR/config.ini" ]; then
    echo "  ! 已存在 $CONFIG_DIR/config.ini，保留不动（新模板见 $CONFIG_DIR/config.ini.new）"
    install -m 0644 "$PKG_DIR/config/config.ini" "$CONFIG_DIR/config.ini.new"
else
    install -m 0644 "$PKG_DIR/config/config.ini" "$CONFIG_DIR/config.ini"
    echo "  ✓ 配置：$CONFIG_DIR/config.ini"
fi

# ZLM 首次启动时若发现 secret 仍是打包自带的默认值，会把它改写成随机值并写回配置
# （实测踩过：启动前读到的 secret 启动后就失效了）。这里安装时直接给一个稳定的随机值，
# 一方面避免这个「首次启动悄悄换 secret」的坑，一方面把值打印出来，装完即可用。
DEF_SECRET=035c73f7-bb6b-4889-a715-d9eb2d1925cc
if grep -q "^secret=$DEF_SECRET" "$CONFIG_DIR/config.ini" 2>/dev/null; then
    NEW_SECRET=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
    sed -i.bak "s/^secret=$DEF_SECRET\r\?$/secret=$NEW_SECRET\r/" "$CONFIG_DIR/config.ini"
    echo "  ✓ 已生成随机 secret（服务首次启动会改写默认值，提前定下来更可控）"
    echo "     API secret: $NEW_SECRET"
fi

echo "== 3/5 库依赖自检 =="
if ldd "$PREFIX/bin/MediaServer" | grep -q "not found"; then
    echo "  ✗ 有库找不到："
    ldd "$PREFIX/bin/MediaServer" | grep "not found"
    exit 1
fi
echo "  ✓ 依赖库齐全"

echo "== 4/5 装 systemd 服务 =="
if [ "$WITH_SERVICE" = "1" ]; then
    sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@CONFIG@|$CONFIG_DIR/config.ini|g" \
        "$PKG_DIR/systemd/zlmediakit-transcode.service" > /etc/systemd/system/zlmediakit-transcode.service
    systemctl daemon-reload
    systemctl enable zlmediakit-transcode >/dev/null 2>&1 || true
    echo "  ✓ 已装：/etc/systemd/system/zlmediakit-transcode.service"
    echo "    启动：systemctl start zlmediakit-transcode"
else
    echo "  - 跳过（--no-service）"
fi

echo "== 5/5 完成 =="
cat <<EOF

下一步：
  1) 编辑配置，打开转码并填目标 codec：
       $CONFIG_DIR/config.ini   →  [transcode]  enable=1 / video_codec=h265 ...
  2) 启动：
       systemctl start zlmediakit-transcode      # 或 --no-service 时手动：
       cd $PREFIX && ./bin/MediaServer -c $CONFIG_DIR/config.ini -l 1
  3) 自检（服务起来后跑）：
       ./check.sh --secret <你的secret>

默认端口（config.ini 里可改）：RTSP 554 / RTMP 1935 / HTTP 80
EOF
