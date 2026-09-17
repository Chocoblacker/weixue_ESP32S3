#!/usr/bin/env bash
# 拉取/刷新上游参考仓库到 upstream/
# 用 codeload tarball 而不是 git clone —— 本机 git-over-https 会 GnuTLS 报错或卡死。
# 用法: ./scripts/fetch-upstream.sh [branch]
set -euo pipefail

OWNER_REPO="waveshareteam/ESP32-S3-Touch-AMOLED-1.75C"
BRANCH="${1:-main}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/upstream/ESP32-S3-Touch-AMOLED-1.75C"
TARBALL="/tmp/${OWNER_REPO##*/}-$BRANCH.tar.gz"

echo "==> 下载 $OWNER_REPO@$BRANCH"
rm -f "$TARBALL"
curl -sL --retry 3 -o "$TARBALL" \
  "https://codeload.github.com/$OWNER_REPO/tar.gz/refs/heads/$BRANCH" &
CPID=$!
while kill -0 "$CPID" 2>/dev/null; do
  sleep 5
  printf '    %s bytes\n' "$(stat -c%s "$TARBALL" 2>/dev/null || echo 0)"
done
wait "$CPID"

echo "==> 解压到 $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"
tar xzf "$TARBALL" -C "$DEST" --strip-components=1
echo "==> 完成: $(du -sh "$DEST" | cut -f1)"
echo "    HEAD 内容快照（非 git 仓库，没有 commit 信息）"
