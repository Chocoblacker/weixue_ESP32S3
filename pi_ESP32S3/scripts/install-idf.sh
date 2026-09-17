#!/usr/bin/env bash
# 安装 ESP-IDF v5.5.5 到 ~/esp（tarball 方式，绕开本机不稳的 git-over-https）
# 用法: ./scripts/install-idf.sh [版本]   默认 v5.5.5
set -euo pipefail

IDF_VER="${1:-v5.5.5}"
IDF_ROOT="${IDF_ROOT:-$HOME/esp}"
IDF_DIR="$IDF_ROOT/esp-idf"
TARBALL="$IDF_ROOT/esp-idf-$IDF_VER.tar.gz"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 乐鑫的 GitHub 资源镜像。本机 objects.githubusercontent.com 不通，
# 不设这个变量 install.sh 下工具链会失败。
export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets"

echo "==> 目标: ESP-IDF $IDF_VER -> $IDF_DIR"

# ---- 1. apt 依赖检查 ----------------------------------------------------
MISSING=()
for p in git wget flex bison gperf python3 python3-pip python3-venv \
         cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0; do
  dpkg -s "$p" >/dev/null 2>&1 || MISSING+=("$p")
done
if [ "${#MISSING[@]}" -gt 0 ]; then
  echo "!! 缺少 apt 包: ${MISSING[*]}"
  echo "   先执行（需要 sudo 密码）："
  echo
  echo "   sudo apt-get update && sudo apt-get install -y ${MISSING[*]}"
  echo
  exit 1
fi
echo "==> apt 依赖 OK"

mkdir -p "$IDF_ROOT"

# ---- 2. 下载并解压 ------------------------------------------------------
if [ -d "$IDF_DIR" ]; then
  echo "==> $IDF_DIR 已存在，跳过下载解压（要重装就先删掉它）"
else
  if [ ! -f "$TARBALL" ]; then
    URL="https://codeload.github.com/espressif/esp-idf/tar.gz/refs/tags/$IDF_VER"
    echo "==> 下载 $URL"
    # 后台下载 + 循环打印进度：避免长命令因无输出被 agent 执行器掐断
    curl -sL --retry 3 -o "$TARBALL" "$URL" &
    CPID=$!
    while kill -0 "$CPID" 2>/dev/null; do
      sleep 5
      printf '    %s bytes\n' "$(stat -c%s "$TARBALL" 2>/dev/null || echo 0)"
    done
    wait "$CPID"
  fi
  echo "==> 解压"
  tar xzf "$TARBALL" -C "$IDF_ROOT"
  mv "$IDF_ROOT/esp-idf-${IDF_VER#v}" "$IDF_DIR"
fi

# ---- 3. 补齐 git submodule（tarball 不含）-------------------------------
# GitHub 的 tarball 不包含 submodule 内容，mbedtls / lwip / esp_wifi/lib /
esp_phy/lib / esp_coex/lib / heap/tlsf 等 23 个目录会是空的，任何工程都编译不了。
# fix-idf-submodules.py 用 codeload 按精确 SHA 逐个补回（本机实测 ~30 秒）。
echo "==> 补齐 submodule"
IDF_PATH="$IDF_DIR" python3 "$SCRIPT_DIR/fix-idf-submodules.py" --ref "$IDF_VER" || {
  echo "!! submodule 补齐失败，构建会报 'Missing esp-mqtt submodule' 之类的错误"
  exit 1
}

# ---- 4. 安装工具链 ------------------------------------------------------
echo "==> 安装 esp32s3 工具链（约 1-2GB，走 $IDF_GITHUB_ASSETS）"
cd "$IDF_DIR"
./install.sh esp32s3

cat <<EOF

==> 完成。每个新终端加载环境：

    source $SCRIPT_DIR/env.sh

或手动：

    . $IDF_DIR/export.sh
EOF
