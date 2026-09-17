#!/usr/bin/env python3
"""
被动监听串口 —— 不复位芯片、不打扰正在跑的固件。

为什么不用 esptool：`esptool.detect_chip()` 会把芯片复位进 ROM 下载模式，
那些"看门狗/复位原因"的现场就被我们自己破坏了。调试按键、复位原因、
偶发崩溃时，必须被动地看。

用法:
    python3 scripts/watch-serial.py              # 默认看 60 秒
    python3 scripts/watch-serial.py 120          # 看 120 秒
    python3 scripts/watch-serial.py 120 --raw    # 原样输出（默认会加时间戳）
    PORT=/dev/ttyACM0 BAUD=115200 python3 scripts/watch-serial.py

关心复位原因的对照表（启动横幅里的 rst:0x..）:
    0x1  POWERON          真的掉电后重新上电（PMIC 断电 / 拔插 USB）
    0x3  RTC_SW_SYS_RST   RTC 域软件复位
    0xc  SW_CPU_RESET     软件重启（esp_restart / 看门狗前的软复位）
    0xf  BROWN_OUT        电压跌落
    0x10 RTCWDT_RTC_RESET RTC 看门狗
    0x15 USB_UART_CHIP_RESET  通过 USB-Serial-JTAG 复位（esptool / idf.py monitor）
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial  # pyserial，在 ESP-IDF 的 python 环境里
except ImportError:
    sys.exit("缺 pyserial。先 `source scripts/env.sh` 用 IDF 的 python 环境。")

RESET_REASONS = {
    "0x1": "POWERON            掉电后上电",
    "0x3": "RTC_SW_SYS_RST     RTC 域软复位",
    "0xc": "SW_CPU_RESET       软件重启",
    "0xf": "BROWN_OUT          电压跌落",
    "0x10": "RTCWDT_RTC_RESET  RTC 看门狗",
    "0x15": "USB_UART_CHIP_RESET  被 USB-Serial-JTAG 复位",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=60.0)
    ap.add_argument("--port", default=os.environ.get("PORT", "/dev/ttyACM0"))
    ap.add_argument("--baud", type=int, default=int(os.environ.get("BAUD", "115200")))
    ap.add_argument("--raw", action="store_true", help="不加时间戳")
    ap.add_argument("--save", metavar="FILE", help="同时原样存一份日志")
    args = ap.parse_args()

    if not Path(args.port).exists():
        sys.exit(f"{args.port} 不存在 —— usbipd attach 掉了吗？试试 scripts/env.sh 的 idf-port")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    # (True,True) -> (False,False) 不是 USB-JTAG 的复位时序，(False,False) 是空闲态
    ser.dtr = False
    ser.rts = False

    sink = open(args.save, "wb") if args.save else None
    print(f"# 监听 {args.port} @ {args.baud} 共 {args.seconds:g} 秒（被动，不复位）", flush=True)
    t0 = time.time()
    n = 0
    try:
        while time.time() - t0 < args.seconds:
            data = ser.read(4096)
            if not data:
                continue
            n += len(data)
            if sink:
                sink.write(data)
                sink.flush()
            if args.raw:
                sys.stdout.buffer.write(data)
            else:
                for line in data.decode("utf8", "replace").splitlines():
                    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    print(f"[{ts}] {line}", flush=True)
                    for code, desc in RESET_REASONS.items():
                        if f"rst:{code}" in line:
                            print(f"          ★★ 复位原因 {code} = {desc}", flush=True)
    except KeyboardInterrupt:
        print("\n# 手动中断")
    finally:
        ser.close()
        if sink:
            sink.close()
            print(f"# 已存日志: {args.save}")
    print(f"# 结束，共收到 {n} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
