#!/usr/bin/env bash
# 加载 ESP-IDF 环境 + 本板常用别名
# 用法: source ./scripts/env.sh
#
# 注意：必须 source 而不是执行，否则环境变量不会留在当前 shell。

IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf}"
PORT="${PORT:-/dev/ttyACM0}"

if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "找不到 $IDF_DIR/export.sh —— 先跑 ./scripts/install-idf.sh" >&2
  return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1090
. "$IDF_DIR/export.sh"

export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets"

# --- 快捷函数 -----------------------------------------------------------
idf-build()   { idf.py set-target esp32s3 build "$@"; }
idf-flash()   { idf.py -p "$PORT" flash "$@"; }
idf-mon()     { idf.py -p "$PORT" monitor "$@"; }
idf-fm()      { idf.py -p "$PORT" flash monitor "$@"; }
idf-erase()   { idf.py -p "$PORT" erase-flash "$@"; }

# 确认板子的 USB 是否已经透传进 WSL
idf-port() {
  if [ -e "$PORT" ]; then
    echo "OK: $PORT 存在"
    ls -l "$PORT"
  else
    echo "!! 没有 $PORT —— Windows 侧需要 usbipd attach（见 docs/environment.md）"
    ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "   /dev 下没有任何 USB 串口设备"
    return 1
  fi
}

echo "ESP-IDF $(idf.py --version 2>/dev/null | head -1) 已加载"
echo "PORT=$PORT  (用 PORT=/dev/ttyUSB0 source ./scripts/env.sh 可改)"
echo "可用: idf-build / idf-flash / idf-mon / idf-fm / idf-erase / idf-port"
