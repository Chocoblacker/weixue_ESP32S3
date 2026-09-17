#!/usr/bin/env python3
"""
真正被动的串口监听 —— 不复位芯片、不打扰正在跑的固件。

═══ 为什么不能用 pyserial ═══
pyserial 的 Serial() 一打开端口就会把 DTR/RTS 拉高，而 ESP32-S3 的
USB-Serial-JTAG 把 DTR/RTS 的电平变化**解释成复位请求**。实测：
用 pyserial 打开端口 3/3 次都出现 "ESP-ROM:esp32s3" 启动横幅 —— 板子被我复位了。
（`cat /dev/ttyACM0` 不复位但收不到数据。）

所以本脚本用 os.open + termios 直接读 tty，全程不设置任何 modem 控制线。

═══ 什么时候必须用它 ═══
- 观察按键事件、复位原因、偶发崩溃（esptool 会把复位原因的现场破坏掉）
- 正在跑对话/OTA 时想看日志，但不想打断它（打开串口=复位=打断）
- 区分"真断电"和"软复位"（见下表）

═══ 判据 ═══
| 现象                                   | 结论                        |
|----------------------------------------|-----------------------------|
| 端口消失（本脚本会报 ★★★）             | 真断电 / 硬件复位           |
| 端口在，日志出现启动横幅 rst:0x..       | 软复位（USB 不重新枚举）    |
| 端口在，无横幅，只有业务日志            | 纯软件行为                  |

复位原因码:
    0x1  POWERON            掉电后上电（拔插 USB / PMIC 断电）
    0x3  RTC_SW_SYS_RST     RTC 域软复位
    0xc  SW_CPU_RESET       软件重启
    0xf  BROWN_OUT          电压跌落
    0x10 RTCWDT_RTC_RESET   RTC 看门狗
    0x15 USB_UART_CHIP_RESET 被 USB-Serial-JTAG 复位（esptool / idf.py monitor）

用法:
    python3 scripts/watch-serial.py                 # 默认看 60 秒
    python3 scripts/watch-serial.py 120             # 看 120 秒
    python3 scripts/watch-serial.py 120 --raw       # 原样输出（默认加时间戳）
    python3 scripts/watch-serial.py 120 --save log.txt
    PORT=/dev/ttyACM0 python3 scripts/watch-serial.py 60
"""
from __future__ import annotations

import argparse
import errno
import os
import sys
import termios
import time
from datetime import datetime
from pathlib import Path

RESET_REASONS = {
    "0x1": "POWERON —— 掉电后重新上电（拔插 USB / PMIC 断电）",
    "0x3": "RTC_SW_SYS_RST —— RTC 域软复位",
    "0xc": "SW_CPU_RESET —— 软件重启 esp_restart",
    "0xf": "BROWN_OUT —— 电压跌落",
    "0x10": "RTCWDT_RTC_RESET —— RTC 看门狗",
    "0x15": "USB_UART_CHIP_RESET —— 被 USB-Serial-JTAG 复位（esptool/monitor）",
}
BOOT_MARKERS = ("ESP-ROM:esp32s3", "boot: ESP-IDF", "Project name:")


def open_passive(path: str) -> int:
    """打开 tty 但不碰 DTR/RTS —— 这是不复位芯片的关键。"""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                    # iflag
    attrs[1] = 0                                    # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                    # lflag
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=60.0)
    ap.add_argument("--port", default=os.environ.get("PORT", "/dev/ttyACM0"))
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--save", metavar="FILE")
    ap.add_argument("--quiet-heartbeat", action="store_true")
    ap.add_argument("--flush", type=float, default=2.0, metavar="SEC",
                    help="开头的预热时长：丢弃最初 SEC 秒内的数据（清掉驱动缓冲区里的陈旧日志）")
    args = ap.parse_args()

    if not Path(args.port).exists():
        sys.exit(f"{args.port} 不存在 —— usbipd attach 掉了吗？用 scripts/env.sh 的 idf-port 看")

    fd = open_passive(args.port)
    sink = open(args.save, "wb") if args.save else None
    print(f"# 被动监听 {args.port}，{args.seconds:g} 秒（不复位，不碰 DTR/RTS）", flush=True)

    # 预热：先把驱动缓冲里堆积的旧日志吐掉。
    # 不这么做会看到“过去的” uptime（实测同一次读数里出现相差 73 秒的两个运行时长）。
    if args.flush > 0:
        print(f"# 预热 {args.flush:g} 秒，丢弃陈旧缓冲 ...", flush=True)
        f0 = time.time()
        dropped = 0
        while time.time() - f0 < args.flush:
            try:
                d = os.read(fd, 65536)
                dropped += len(d)
            except BlockingIOError:
                pass
            except OSError:
                pass
            time.sleep(0.05)
        print(f"# 预热完成，丢弃 {dropped} 字节陈旧数据\n", flush=True)

    t0 = time.time()
    n = 0
    present = True
    next_beat = 10.0
    boot_seen = False
    try:
        while time.time() - t0 < args.seconds:
            elapsed = time.time() - t0

            now_present = Path(args.port).exists()
            if present and not now_present:
                print(f"\n[{elapsed:6.1f}s] ★★★ {args.port} 消失了"
                      f" => 设备从 USB 掉线 = 真断电或硬件复位", flush=True)
            elif not present and now_present:
                print(f"\n[{elapsed:6.1f}s] ★★★ {args.port} 回来了 => 断电又上电", flush=True)
                try:
                    os.close(fd)
                except OSError:
                    pass
                time.sleep(0.5)
                try:
                    fd = open_passive(args.port)
                except OSError as e:
                    print(f"          重新打开失败: {e}", flush=True)
            present = now_present

            if not present:
                time.sleep(0.3)
                continue

            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                data = b""
            except OSError as e:
                if e.errno in (errno.ENODEV, errno.EIO, errno.EAGAIN):
                    data = b""
                    time.sleep(0.2)
                else:
                    raise

            if data:
                n += len(data)
                if sink:
                    sink.write(data)
                    sink.flush()
                text = data.decode("utf8", "replace")
                if not boot_seen and any(m in text for m in BOOT_MARKERS):
                    boot_seen = True
                    print(f"\n[{elapsed:6.1f}s] ※※ 检测到启动横幅 ⇒ 芯片刚复位过", flush=True)
                if args.raw:
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
                else:
                    for line in text.splitlines():
                        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        print(f"[{ts}] {line}", flush=True)
                        for code, desc in RESET_REASONS.items():
                            if f"rst:{code}" in line:
                                print(f"          ★★ 复位原因 {code} = {desc}", flush=True)
            else:
                time.sleep(0.05)

            if not args.quiet_heartbeat and elapsed >= next_beat:
                print(f"[{elapsed:6.1f}s] ...心跳 port={'在' if present else '掉线'} "
                      f"累计 {n} 字节", flush=True)
                next_beat += 15.0
    except KeyboardInterrupt:
        print("\n# 手动中断", flush=True)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        if sink:
            sink.close()
            print(f"# 已存日志: {args.save}", flush=True)

    print(f"# 结束：{n} 字节，{'检测到复位横幅' if boot_seen else '全程未见复位横幅'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
