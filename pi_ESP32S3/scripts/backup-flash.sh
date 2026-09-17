#!/usr/bin/env bash
# 整片备份板子的 flash —— 改分区表 / 刷固件之前必须做的事。
#
# 为什么要分块：USB-Serial-JTAG 上一次读 32MB 会中途坏掉
#   "Corrupt data, expected 0x1000 bytes but received 0xd55 bytes"
# 分块 + 每块重试 + 长度校验，坏块只影响 1MB，重试即可。
#
# 用法:
#   ./scripts/backup-flash.sh                     # 全片 32MB
#   ./scripts/backup-flash.sh --size 0x40000      # 只备 NVS 区
#   BAUDS="460800 921600 115200" ./scripts/backup-flash.sh
set -uo pipefail

PORT="${PORT:-/dev/ttyACM0}"
CHIP="${CHIP:-esp32s3}"
FLASH_TOTAL="${FLASH_TOTAL:-0x2000000}"   # 32MB
CHUNK="${CHUNK:-0x100000}"                # 每块 1MB
BAUDS="${BAUDS:-460800 921600 115200}"
TRIES="${TRIES:-3}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="$ROOT/artifacts/board-backup"
SIZE="$FLASH_TOTAL"
[ "${1:-}" = "--size" ] && SIZE="${2:?--size 需要一个值}"

mkdir -p "$DEST_DIR"
STAMP="$(date +%Y%m%d-%H%M)"
OUT="$DEST_DIR/flash-${SIZE#0x}-$STAMP.bin"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

python3 - "$SIZE" "$CHUNK" <<'PY'
import sys
size, chunk = int(sys.argv[1], 16), int(sys.argv[2], 16)
print(f"{size} {chunk} {(size + chunk - 1)//chunk}")
PY
read -r TOTAL_B CHUNK_B NCHUNK <<<"$(python3 -c "
size=$SIZE; chunk=$CHUNK
print(f'{size} {chunk} {(size+chunk-1)//chunk}')")"

echo "备份 $((TOTAL_B/1024/1024)) MiB，分成 $NCHUNK 块，每块 $((CHUNK_B/1024)) KiB"
echo "输出: $OUT"
echo

i=0
while [ "$i" -lt "$NCHUNK" ]; do
  off=$(python3 -c "print(hex($i*$CHUNK_B))")
  part="$TMPDIR/$(printf '%04d' "$i").bin"
  ok=0
  for baud in $BAUDS; do
    for t in $(seq 1 "$TRIES"); do
      rm -f "$part"
      esptool.py --chip "$CHIP" -p "$PORT" -b "$baud" read_flash "$off" "$CHUNK" "$part" \
        >/dev/null 2>&1
      got=$(stat -c%s "$part" 2>/dev/null || echo 0)
      if [ "$got" -eq "$CHUNK_B" ]; then ok=1; break 2; fi
      printf "\r  [%d/%d] off=%s baud=%s 第%s次: 只拿到 %s/%s 字节，重试 " \
        "$((i+1))" "$NCHUNK" "$off" "$baud" "$t" "$got" "$CHUNK_B"
    done
    printf "\r  [%d/%d] off=%s baud=%s 失败，换速率重试        " "$((i+1))" "$NCHUNK" "$off" "$baud"
  done
  if [ "$ok" -ne 1 ]; then
    printf "\n!! 块 %s (off=%s) 读取失败，终止\n" "$i" "$off"
    exit 1
  fi
  printf "\r  [%d/%d] %s ok (%s bytes)                    \n" "$((i+1))" "$NCHUNK" "$off" "$CHUNK_B"
  i=$((i+1))
done

cat "$TMPDIR"/*.bin > "$OUT"
rm -rf "$TMPDIR"

REAL=$(stat -c%s "$OUT")
echo
if [ "$REAL" -ne "$TOTAL_B" ]; then
  echo "!! 大小不对: $REAL != $TOTAL_B"
  exit 1
fi
echo "✅ 完成: $OUT"
echo "   size   : $REAL bytes"
echo "   sha256 : $(sha256sum "$OUT" | cut -d' ' -f1)"
