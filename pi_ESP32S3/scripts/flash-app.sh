#!/usr/bin/env bash
# 安全烧录脚本：把工程烧进指定的 OTA 槽，并且**绝不碰 factory**（小智住那儿）。
#
# 为什么不能用 `idf.py flash`：
#   idf.py flash 把 app 写到分区表里第一个 app 分区 = factory(0x110000)，
#   会直接覆盖小智的固件。而且它还会写 ota_data_initial（擦空 otadata），
#   导致烧完启动项指向 factory。
#
# 用法:
#   ./scripts/flash-app.sh [工程目录] [槽号 0|1]
#   ./scripts/flash-app.sh                       # 默认 projects/ota_app 烧到 slot 0
#   ./scripts/flash-app.sh projects/ota_app 1    # 烧到 ota_1
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="${1:-$ROOT/projects/ota_app}"
SLOT="${2:-0}"
PORT="${PORT:-/dev/ttyACM0}"
CHIP="${CHIP:-esp32s3}"

# 板上固定布局（来源 projects/board-partitions.csv，从板子读出来的）
case "$SLOT" in
  0) APP_OFF=0xa10000; SLOT_NAME=ota_0 ;;
  1) APP_OFF=0xe00000; SLOT_NAME=ota_1 ;;
  *) echo "槽号只能是 0 或 1" >&2; exit 2 ;;
esac

PROJ="$(cd "$PROJ" && pwd)"

# 工程名可能和目录名不同，从 CMakeLists 里取真正的 project(...)
PRJ_NAME="$(sed -n 's/^[[:space:]]*project(\([A-Za-z0-9_-]*\).*/\1/p' "$PROJ/CMakeLists.txt" | head -1)"
APP_BIN="$PROJ/build/${PRJ_NAME}.bin"

echo "工程      : $PROJ  (project=$PRJ_NAME)"
echo "目标槽    : $SLOT_NAME @ $APP_OFF   ← 不是 factory，安全"
echo "端口      : $PORT"
echo

for f in "$PROJ/build/bootloader/bootloader.bin" \
         "$PROJ/build/partition_table/partition-table.bin" \
         "$APP_BIN"; do
  [ -f "$f" ] || { echo "缺少 $f —— 先 idf.py build" >&2; exit 1; }
done

if [ ! -e "$PORT" ]; then
  echo "!! $PORT 不存在 —— usbipd attach 掉了吗？（见 docs/environment.md）" >&2
  exit 1
fi

SIZE=$(stat -c%s "$APP_BIN")
echo "app 大小  : $SIZE 字节 ($((SIZE/1024)) KiB)"
echo

echo "==> 1/3 写 bootloader(0x0) + 分区表(0x8000) + app($APP_OFF)"
esptool.py --chip "$CHIP" -p "$PORT" write_flash \
  0x0      "$PROJ/build/bootloader/bootloader.bin" \
  0x8000   "$PROJ/build/partition_table/partition-table.bin" \
  "$APP_OFF" "$APP_BIN"

echo
echo "==> 2/3 把启动项指到 $SLOT_NAME"
python3 "$IDF_PATH/components/app_update/otatool.py" --port "$PORT" switch_ota_partition --slot "$SLOT" >/dev/null

echo "==> 3/3 确认 otadata"
python3 "$IDF_PATH/components/app_update/otatool.py" --port "$PORT" read_otadata 2>/dev/null | tail -1

echo
echo "✅ 完成。用 ./scripts/watch-serial.py 30 看启动日志（被动，不复位）"
echo "   想回小智: python3 \$IDF_PATH/components/app_update/otatool.py --port $PORT erase_otadata"
