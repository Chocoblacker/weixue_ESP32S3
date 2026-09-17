# 硬件入网 + 工具链打通实录（2026-09-17）

按时间顺序记录第一次把这块板子从零跑通的全过程，以及中间踩的坑。
新会话接手时看这一篇就能知道现状是怎么来的。

## 起点

- 只有一块板子，用户说"已经连上 WiFi 了，IP 192.168.31.46"
- WSL2 (Ubuntu 22.04)，系统里除了 git/curl/python 什么都没有
- Windows 侧没装 usbipd-win

## 第 1 步：确认板子身份

| 探测 | 结果 |
| --- | --- |
| `ping 192.168.31.46` | 通，RTT 5–70ms（抖动大 = Wi-Fi modem sleep） |
| ARP MAC | `80:45:6b:34:25:18` |
| OUI `80:45:6B` | **Espressif Inc.** ✅ |
| TCP 全端口扫描 1–65535 | **一个都没开** ❌ |

结论：板子在网，但固件不带任何网络服务 → **不可能 OTA，首次烧录必须走 USB**。
（ESP32-S3 的 ROM bootloader 只认 UART / USB-Serial-JTAG，没有网络引导。）

意外收获：`C:\Users\Admin\.wslconfig` 里是 `networkingMode=Mirrored`，
所以 **WSL 直接持有局域网 IP `192.168.31.126`** —— 板子能反向连回 WSL，
以后做 OTA 不需要 Windows 端口转发。

## 第 2 步：USB 透传

WSL2 看不到 USB，必须 Windows 侧装 usbipd-win（4.29 MiB，已放桌面并校验 SHA256）。

```powershell
usbipd list                          # 板子显示为 303a:1001 USB JTAG/serial debug unit (COM3)
usbipd bind --force --busid=1-1      # 必须 --force：火绒的 hrdevmon 过滤驱动会挡
usbipd attach --wsl --busid=1-1
```

之后 WSL 里出现 `/dev/ttyACM0`，`vhci_hcd` 自动加载。

## 第 3 步：认识板上现有的固件

`esptool` 识别结果：

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
USB mode: USB-Serial/JTAG
MAC: 80:45:6b:34:25:18        ← 和 Wi-Fi 上的 MAC 完全一致，确认同一块板
Detected flash size: 32MB     ← 硬件层面确认 32MB，微雪文档"16MB"是错的
```

**分区表不是微雪出厂布局**，而是一份带 OTA 的表（事后确认这是 xiaozhi-esp32 的 32MB 布局）：

```
nvsfactory 0x9000   200K      nvs        0x3b000  840K
otadata    0x10d000 8K        phy_init   0x10f000 4K
factory    0x110000 9M        ota_0      0xa10000 4032K
ota_1      0xe00000 4032K     assets     0x11f0000 9M
storage    0x1af0000 5M
```

读各分区里的 app 描述符，发现**板上有两个固件**：

| 分区 | project | version | 构建时间 | IDF |
| --- | --- | --- | --- | --- |
| `factory` @0x110000 | **esp-brookesia** | ac40993 | Jan 14 2026 09:28 | v5.5.2-249-gf56bea3d1f-dirty |
| `ota_0` @0xa10000 | **xiaozhi** | 2.1.0 | Jan 13 2026 14:48 | v5.5.2-249-gf56bea3d1f-dirty |
| `ota_1` | 空 | | | |

`otadata` 两个条目都是 `0xFFFFFFFF`（空）→ **bootloader 回落到 factory**，所以板子跑的是
Waveshare 的 brookesia 手机 Demo。串口日志验证：

```
I (1157) Main: [main.cpp:0064](app_main): Display ESP-Brookesia phone demo
```

顺带一个发现：brookesia 报告 `PSRAM: total 4043484`（≈3.8MB），
说明它编译时用的是 **QUAD 模式**，8MB octal PSRAM 只用了一半。

## 第 4 步：装 ESP-IDF（最费劲的一步）

apt 依赖 15 个包补齐后，用 `scripts/install-idf.sh` 装 v5.5.5 到 `~/esp/`。
因为本机 git-over-https 不稳（GnuTLS 报错/卡死），走的是 **codeload tarball**。

### 坑 1：tarball 不含 git submodule（阻塞级）

`idf.py build` 报：

```
CMake Error at esp-idf/components/mqtt/CMakeLists.txt:3 (message):
  Missing esp-mqtt submodule. Please run 'git submodule update --init --recursive'
```

一查 `.gitmodules`：**23 个子模块全是空目录**，包括 `mbedtls`、`lwip`、
`esp_wifi/lib`、`esp_phy/lib`、`esp_coex/lib`、`heap/tlsf`、`spiffs`、`cJSON`。
**任何一个缺失都编译不了**。

解法：`scripts/fix-idf-submodules.py`
1. 读 `.gitmodules` 拿 路径 → 仓库
2. 用 GitHub API 的**递归 tree** 一次拿到全部 gitlink SHA（23 个 1 次请求）
3. 用 codeload 按 SHA 下 tar.gz，strip-components=1 解压

实测 **~35 秒补齐 23 个**，比 git submodule 快得多。

### 坑 2：Python tarfile 的安全过滤器会中断解压

`esp-nimble` 里 `porting/npl/riot/include/npl_syscfg/npl_sycfg.h` 是个 18 字节符号链接
`-> ../syscfg/syscfg.h`，python 的 `filter="data"` 判定"链接到目标目录之外"直接抛异常，
**导致该子模块只解压了一半，却看起来"非空"**，`is_empty()` 检查漏过去了。

解法：改用系统 `tar --strip-components=1`（已验证解压结果与 git 内容一致）。
教训：以后凡是判断"子模块是否完整"，不能只看目录非空。

### 坑 3：仓库没有 commit → CMake 配置失败（隐蔽）

```
CMake Error at build/CMakeFiles/git-data/grabRef.cmake:48 (file):
  file failed to open for reading (No such file or directory):
  .../build/CMakeFiles/git-data/head-ref
```

根因链条：`project.cmake:731` 只在 **`PROJECT_VER` 未设置**时才调 `git_describe`；
`weixue/` 虽然 `git init` 过但**一个 commit 都没有** → 没有 `HEAD` → `git describe` 失败
→ `grabRef.cmake` 读不到 `head-ref` → CMake 报错、配置中止。

解法：做第一次提交（无论如何都该做）。或者在自己的工程里 `set(PROJECT_VER "0.1.0")`。

### 坑 4：忘了 set-target 会静默用 esp32

只跑 `idf.py build`（不带 `set-target`）时，sdkconfig 里的 target 是 `esp32`，
编译到 BSP 才炸：

```
esp32_s3_touch_amoled_1_75c.h:38: error: 'GPIO_NUM_45' undeclared
```

（esp32 只有 GPIO0–39。）日志里的征兆是链接脚本变成了 `esp32/ld/esp32.rom.ld`。

解法：在 `sdkconfig.defaults` 里钉死 `CONFIG_IDF_TARGET="esp32s3"`，
并且用 `env.sh` 提供的 `idf-build`（内部就是 `set-target esp32s3 build`）。

## 第 5 步：整片备份

改分区表/刷固件之前必须做。**32MB 一次性整读会坏**：

```
A fatal error occurred: Corrupt data, expected 0x1000 bytes but received 0xd55 bytes
```

解法：`scripts/backup-flash.sh` —— 1MB 分块 + 每块换速率重试 + 长度校验 + 拼接。
实测 32 块全部一次通过，约 3 分钟。

产物：`artifacts/board-backup/flash-2000000-20260917-2352.bin`
（33,554,432 bytes，sha256 `2a452d9f311ae103c10b5f4346fa280eea4365826a62952683802b7dca3b1e7b`）
**这就是板子被你弄坏时的后悔药。**

## 第 6 步：首次烧录（保留别人的东西）

关键决策：**不用示例自带的分区表**，改用从板上读出来的那份。
理由：
- 示例的 `partitions.csv` 会把分区表整张换掉 → `ota_0` 里的 xiaozhi、
  `assets`/`storage` 全丢
- 而且它把 `nvs` 放在 `0x9000` 长 `0x6000`，和板上的 `nvsfactory` 区重叠 →
  **WiFi 凭据会被冲掉**
- 用板上布局，我们的 app 落在 `factory` @0x110000（9MB），其他分区一个不碰，
  还白得一套 OTA 分区

```bash
cp projects/board-partitions.csv projects/lvgl_demo_v9/partitions.csv
idf.py -p /dev/ttyACM0 flash
```

结果：

```
Wrote 1030864 bytes (575335 compressed) at 0x00110000 in 6.9 seconds
Hash of data verified.
```

（app 只占 9MB 分区的 10.9%。`idf.py flash` 会顺手把 `otadata` 擦成空 —— 它本来也是空的，
无副作用。）

## 第 7 步：验证

bootloader 日志：

```
I (99)  boot:  6 ota_1            OTA app     00 11 00e00000 003f0000
I (122) boot: Defaulting to factory image
I (323) boot: Loaded app from partition at offset 0x110000
I (333) octal_psram: density : 0x03 (64 Mbit)     ← 8MB octal 识别正确
```

app 日志：

```
I (706) app_init: Project name:     lvgl_demo_v9
I (711) app_init: App version:      c5b092f-dirty
I (724) app_init: ESP-IDF:          v5.5.5
I (762) esp_psram: Adding pool of 7360K of PSRAM memory   ← 7.36MB，比 brookesia 多一倍
I (824) co5300: version: 2.2.0
I (2276) CST9217: Resolution X: 466, Y: 466
I (2317) esp_lvgl:adapter: LVGL task started successfully
```

**build → flash → monitor 闭环打通，形态是 lv_demo_benchmark 在屏上跑。**

## 现状小结

| 项 | 状态 |
| --- | --- |
| 工具链 | ESP-IDF v5.5.5 @ `~/esp/esp-idf`（tarball + 23 个子模块补齐） |
| 编译器 | `xtensa-esp-elf-gcc 14.2.0` |
| 串口 | `/dev/ttyACM0`（usbipd 转发，重新拔插后需重新 attach） |
| 板上固件 | `factory` = **我们的 lvgl_demo_v9**；`ota_0` = xiaozhi 2.1.0（未动） |
| NVS | 未动（WiFi 凭据还在） |
| 备份 | `artifacts/board-backup/flash-2000000-*.bin`（32MB 整片） |
| 待办 | OTA 通道；自己的工程骨架 |

## 还在的小警告（都无害）

```
W (2195) i2c.master: Please check pull-up resistances ...   ← Waveshare 板常见
W (992)  co5300_spi: The 3Ah command has been used ...      ← 驱动内部覆盖提示
W (2278) esp_lvgl:touch: LV_USE_GESTURE_RECOGNITION is disabled
```
