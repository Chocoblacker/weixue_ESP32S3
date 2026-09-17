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
# 1. 一次性：装 ESP-IDF v5.5.5 到 ~/esp（需要先装 apt 依赖，见 docs/environment.md）
./scripts/install-idf.sh

# 2. 每个新终端：加载工具链
source ./scripts/env.sh

# 3. 复制一个官方示例当起点
cp -r upstream/ESP32-S3-Touch-AMOLED-1.75C/examples/esp-idf/02_lvgl_demo_v9 \
      projects/lvgl_demo_v9
cd projects/lvgl_demo_v9 && idf.py set-target esp32s3 && idf.py build
```

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
- [x] 板子确认联网可达（`ping`/ARP 通，MAC 确认为 Espressif）
- [ ] ESP-IDF 工具链安装（缺 apt 依赖，需要 sudo 密码）
- [ ] USB 透传（WSL2 需 Windows 侧 usbipd-win）——**首次烧录的前置条件**
- [ ] 首次烧录官方固件，跑通 build → flash → monitor 闭环
- [ ] 给自己的固件加 OTA，之后纯 Wi-Fi 迭代，不再需要 USB

详见 [`docs/environment.md`](docs/environment.md) 与 [`docs/network.md`](docs/network.md)。

## 上游参考

- 官方仓库：<https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C>
- 产品文档：<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75C>
- 原理图：`upstream/ESP32-S3-Touch-AMOLED-1.75C/Schematic/`
- 工厂固件：`artifacts/ESP32-S3-Touch-AMOLED-1.75C-FactoryOnly-260114.bin`（从 `0x0` 整片烧录）
