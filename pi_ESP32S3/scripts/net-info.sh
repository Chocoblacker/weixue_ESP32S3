#!/usr/bin/env bash
# 快速探明板子在网络上的状态：可达性、MAC 归属、开放端口。
# 用法: ./scripts/net-info.sh [board-ip]   默认 192.168.31.46
set -uo pipefail

BOARD="${1:-192.168.31.46}"
IFACE="$(ip route show default | awk '{print $5; exit}')"
WSL_IP="$(ip -4 -o addr show "$IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"

echo "=== WSL 侧 ==="
echo "IP: ${WSL_IP:-未知}  (mirrored 模式下即局域网 IP)"
echo
echo "=== 板子 $BOARD ==="

if timeout 6 ping -c 3 -W 1 "$BOARD" >/dev/null 2>&1; then
  timeout 6 ping -c 3 -W 1 "$BOARD" | tail -2
else
  echo "ICMP: 不通"
fi

echo -n "MAC: "
ip neigh show "$BOARD" | awk '{print $5}' | head -1

MAC="$(ip neigh show "$BOARD" | awk '{print $5}' | head -1)"
if [ -n "$MAC" ]; then
  OUI="${MAC:0:8}"
  echo -n "OUI: $OUI -> "
  timeout 15 curl -s "https://api.macvendors.com/$OUI" || echo "(查询失败)"
  echo
fi

echo "=== 端口扫描 (1-10000) ==="
python3 - "$BOARD" <<'PY'
import socket, concurrent.futures, sys
host = sys.argv[1]
def probe(p):
    s = socket.socket(); s.settimeout(0.4)
    try:
        s.connect((host, p)); return p
    except Exception:
        return None
    finally:
        s.close()
found = []
with concurrent.futures.ThreadPoolExecutor(max_workers=400) as ex:
    for r in ex.map(probe, range(1, 10001)):
        if r: found.append(r)
print("开放端口:", found or "无（全关）")
PY

echo
echo "提示: 无开放端口 => 板子当前固件不支持 OTA，首次烧录必须走 USB"
echo "      详见 docs/network.md"
