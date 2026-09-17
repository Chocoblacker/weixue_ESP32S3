#!/usr/bin/env bash
# 把板子的 USB 重新透传进 WSL —— 一条命令搞定。
#
# 为什么需要这个：usbipd 的 attach 是**非持久**的。板子每次重新枚举
# （断电重启 / 拔插 / 硬复位 / esptool 硬复位）都会从 WSL 里消失，必须重新 attach。
# bind 才是持久的（我们已用 --force bind 过，状态是 Shared (forced)，重启也在）。
#
# usbipd 5.3.0 **不支持自动 attach**（policy 只能 AutoBind），所以做成这个脚本。
#
# 用法:
#   ./scripts/attach-usb.sh
#   PORT=/dev/ttyACM0 ./scripts/attach-usb.sh
set -uo pipefail

PORT="${PORT:-/dev/ttyACM0}"
USBIPD_WIN='C:\Program Files\usbipd-win\usbipd.exe'
USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"

[ -x "$USBIPD" ] || { echo "!! 找不到 $USBIPD —— Windows 侧装了 usbipd-win 吗？" >&2; exit 1; }

if [ -e "$PORT" ]; then
    echo "✅ $PORT 已经在，不需要 attach"
    ls -l "$PORT"
    exit 0
fi

echo "==> 问 Windows：板子接在本机吗"
STATE="$(timeout 90 "$USBIPD" state 2>/dev/null)"
BUSID="$(printf '%s' "$STATE" | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
i = raw.find("{")
if i < 0: sys.exit(0)
try:
    d = json.loads(raw[i:])
except Exception:
    sys.exit(0)
for x in d.get("Devices", []):
    if "VID_303A" in str(x.get("InstanceId", "")).upper():
        b = x.get("BusId")
        if b:
            print(b); break
' 2>/dev/null)"

if [ -z "$BUSID" ]; then
    # 退路：文本输出里找 303a:1001 那一行的第一个字段
    BUSID="$(printf '%s' "$STATE" | awk 'tolower($0) ~ /303a:1001/ && $1 ~ /^[0-9]+-/ {print $1; exit}')"
fi

if [ -z "$BUSID" ]; then
    cat >&2 <<'MSG'
!! 板子没有接入本机 USB。

   区分两种情况：
     * "只接了电源适配器/USB 充电头" —— 那根线只有电，没有数据，Windows 看不到设备。
       要用 USB 必须把数据线插到**电脑**上。
     * "线插在电脑上但没反应" —— 检查是不是充电线（没有 D+/D-）；
       或按住 BOOT 键再按一下 PWR 强制进 ROM 下载模式（ROM 一定会枚举 303a:1001）。

   板子在 Windows 上的持久绑定还在（PersistedGuid e7bc75d8...），
   所以插上后**不需要重新 bind**，只要 attach。
MSG
    exit 1
fi

echo "==> 找到板子 BUSID=$BUSID，attach 进 WSL"
timeout 90 "$USBIPD" attach --wsl --busid="$BUSID" 2>&1 | tail -4

for i in 1 2 3 4 5; do
    sleep 2
    if [ -e "$PORT" ]; then
        echo "✅ $PORT 就位"
        ls -l "$PORT"
        exit 0
    fi
done

echo "!! attach 报成功但 $PORT 还是没出现。试试：" >&2
echo "   1) 临时退出火绒（hrdevmon 过滤驱动会干扰，usbipd 一直在警告它）" >&2
echo "   2) 按住 BOOT 再按 PWR 强制进 ROM 下载模式后重跑本脚本" >&2
exit 1
