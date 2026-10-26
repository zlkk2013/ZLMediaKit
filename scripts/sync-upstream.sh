#!/usr/bin/env bash
# 同步官方最新代码到本地 master，再把开发分支 rebase 上去。
# 用法: ./sync-upstream.sh [dev-branch]
set -euo pipefail

DEV_BRANCH="${1:-dev/transcode}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

echo "==> 仓库: $REPO_DIR"
echo "==> 拉官方更新 (upstream)"
git fetch upstream --tags --prune

echo "==> 更新本地 master"
git checkout master --quiet
git merge --ff-only upstream/master

echo "==> rebase $DEV_BRANCH 到 master"
git checkout "$DEV_BRANCH" --quiet
if ! git rebase master; then
    echo ""
    echo "!! rebase 有冲突，处理完执行:"
    echo "   git add -A && git rebase --continue"
    echo "   或放弃: git rebase --abort"
    exit 1
fi

echo "==> 完成。当前:"
git log --oneline -3
echo ""
echo "确认无误后推送: ./scripts/push-fork.sh $DEV_BRANCH"
