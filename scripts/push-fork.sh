#!/usr/bin/env bash
# 推自己的分支到 fork（origin）。带 GitHub 低带宽环境下的容错参数。
# 用法: ./push-fork.sh [branch]
set -euo pipefail

BRANCH="${1:-$(git rev-parse --abbrev-ref HEAD)}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

# 本机到 github.com 吞吐极低，默认 1000B/s / 60s 会误判为"远端不可达"
git -c http.version=HTTP/1.1 \
    -c http.lowSpeedLimit=0 \
    -c http.lowSpeedTime=999999 \
    push -u origin "$BRANCH"
