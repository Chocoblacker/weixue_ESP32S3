# ESP32-S3-Touch-AMOLED-1.75C 开发区

微雪 ESP32-S3 智能手表形态开发板的工程根目录。

## 目录约定

```
pi_ESP32S3/
├── docs/        我们的笔记：硬件、环境、网络、踩坑
├── upstream/    上游官方仓库副本（只读！不要在这里改代码）
├── projects/    我们自己的 ESP-IDF 工程，一个工程一个子目录
├── scripts/     env.sh / install-idf.sh / fetch-upstream.sh / net-info.sh
├── artifacts/   固件与构建产物（.bin/.zip 不进 git）
└── system/      pi 会话工作目录 / 草稿
```

工具链装在 `~/esp/`（ESP-IDF 官方约定位置），**不放在本仓库内**，避免 2GB+ 的
IDF 源码污染工作区。

## 快速开始

```bash
# 1. 一次性：装 ESP-IDF v5.5.5 到 ~/esp（需先装 apt 依赖，见 docs/environment.md）
#    会自动补齐 23 个 git submodule（tarball 装法的必需补丁）
./scripts/install-idf.sh

# 2. 每个新终端：加载工具链
source ./scripts/env.sh

# 3. 刷固件前先整片备份
./scripts/backup-flash.sh

# 4. 复制一个官方示例当起点；记得用板上分区布局，否则 NVS 会被冲
cp -r upstream/ESP32-S3-Touch-AMOLED-1.75C/examples/esp-idf/02_lvgl_demo_v9 \
      projects/my_app
cp projects/board-partitions.csv projects/my_app/partitions.csv
#    并在 sdkconfig.defaults 里钉死 CONFIG_IDF_TARGET="esp32s3"
cd projects/my_app && idf-build && idf-fm
```

可用命令（`env.sh` 提供）：`idf-build` / `idf-flash` / `idf-mon` / `idf-fm` /
`idf-erase` / `idf-port`

## 板子速览

| 项 | 值 |
| --- | --- |
| MCU | ESP32-S3R8，双核 LX7 @240MHz，8MB Octal PSRAM，32MB Flash |
| 屏 | 1.75" 466×466 QSPI AMOLED，CO5300 |
| 触摸 | CST9217，I2C |
| 电源 | AXP2101 |
| IMU | QMI8658（加速度 + 陀螺仪） |
| 音频 | ES7210 双麦 ADC + ES8311 codec + 板载扬声器 |
| 框架 | ESP-IDF ≥5.5（CI 验证 5.5.5 / 6.0.2），或 Arduino |
| BSP | 托管组件 `waveshare/esp32_s3_touch_amoled_1_75c` `^3.0.0` |
| 网络 | Wi-Fi `192.168.31.46`（MAC `80:45:6b:34:25:18`，Espressif） |

详细引脚表和器件版本见 [`docs/board.md`](docs/board.md)。

## 当前状态

- [x] 上游仓库副本落在 `upstream/`
- [x] 板子确认联网可达（MAC `80:45:6b:34:25:18`，OUI 确认为 Espressif）
- [x] USB 透传（usbipd-win，`/dev/ttyACM0` 可用）
- [x] ESP-IDF v5.5.5 工具链装好，子模块补齐
- [x] **build → flash → monitor 闭环打通**，板上跑着 `projects/lvgl_demo_v9`
- [x] 32MB 整片备份存档
- [ ] 给固件加 OTA，之后纯 Wi-Fi 迭代，不再需要 USB
- [ ] 定下自己的工程骨架（现在还用着官方示例副本）

第一次跑通的全过程、踩的坑、板上原有固件的清单，都在
👉 [`docs/bringup-log.md`](docs/bringup-log.md)

环境与网络细节见 [`docs/environment.md`](docs/environment.md) 与 [`docs/network.md`](docs/network.md)。

## 板上现状（重要）

`factory` 分区（0x110000）现在是我们的 `lvgl_demo_v9`。
`ota_0` 里原来的 **xiaozhi 2.1.0** 和 NVS（WiFi 凭据）**没被动过** ——
我们烧录时特意改用板上自带的分区表（`projects/board-partitions.csv`）而不是
示例自带的，后者会冲掉 NVS。

一键恢复接管前的原样：

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0 \
  artifacts/board-backup/flash-2000000-*.bin
```

## 上游参考

- 官方仓库：<https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C>
- 产品文档：<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75C>
- 原理图：`upstream/ESP32-S3-Touch-AMOLED-1.75C/Schematic/`
- 工厂固件：`artifacts/ESP32-S3-Touch-AMOLED-1.75C-FactoryOnly-260114.bin`（从 `0x0` 整片烧录）
