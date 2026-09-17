# 硬件笔记：ESP32-S3-Touch-AMOLED-1.75C

## 核心器件

| 功能 | 器件 / 接口 | 备注 |
| --- | --- | --- |
| MCU | ESP32-S3R8 | Xtensa 双核 LX7 @240MHz，512KB SRAM + 384KB ROM |
| PSRAM | 8MB，Octal（OPI） | 叠封在 SoC 内 |
| Flash | 32MB NOR，QIO 模式 | 见下方"文档矛盾" |
| 显示 | CO5300，QSPI，466×466 | 1.75" 电容触摸 AMOLED，16.7M 色 |
| 触摸 | CST9217，I2C | 电容触摸控制器 |
| 电源管理 | AXP2101 | 充电 + 电池管理 + 多路电压输出 |
| IMU | QMI8658 | 6 轴：3 轴加速度 + 3 轴陀螺仪 |
| 音频采集 | ES7210 | 双麦克风阵列 ADC，带回声消除电路 |
| 音频输出 | ES8311 codec + 板载扬声器 | 扬声器焊盘在板载 |
| 按键 | PWR、BOOT | PWR 可软件自定义 |
| 电池 | MX1.25 2PIN，3.7V 锂电 | 支持充放电 |
| 接口 | Type-C | 直连 ESP32-S3 原生 USB（USB-Serial/JTAG） |

### ⚠️ 文档矛盾：Flash 容量

微雪产品页自相矛盾——"产品特性"写 **16MB**，"板载资源"写 **32MB**。
以仓库 `examples/esp-idf/*/sdkconfig.defaults` 为准：

```
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
```

`partitions.csv` 也按 32MB 布局（`factory` 8M + `storage` 7M）。**按 32MB 处理。**

## 引脚表

来源：上游仓库 `docs/components_ZH.md`（对原理图的只读交叉核对）+ Arduino 板级头文件。

### LCD（QSPI）

| 信号 | GPIO |
| --- | --- |
| D0–D3 | GPIO4 / GPIO5 / GPIO6 / GPIO7 |
| SCLK | GPIO38 |
| CS | GPIO12 |
| RST | GPIO1 |

### 触摸（I2C）

| 信号 | GPIO |
| --- | --- |
| SDA | GPIO15 |
| SCL | GPIO14 |
| INT | GPIO11 |
| RST | GPIO2 |

### 音频（I2S）

| 信号 | GPIO |
| --- | --- |
| MCLK | GPIO16 |
| BCLK | GPIO9 |
| LRCK | GPIO45 |
| DIN（ES7210 → SoC） | GPIO10 |
| DOUT（SoC → ES8311） | GPIO8 |
| PA_EN（功放使能） | GPIO46 |

> QMI8658 与 AXP2101 挂在同一条 I2C 总线上，但引脚未在本地头文件重复定义。
> **以原理图和托管 BSP 为准**，不要从二手文档推。

## 软件版本矩阵

| 项 | 版本 |
| --- | --- |
| ESP-IDF | ≥ 5.5（CI 验证 `v5.5.5` 与 `v6.0.2`） |
| BSP 组件 | `waveshare/esp32_s3_touch_amoled_1_75c` `^3.0.0`（注册表目前只有 3.0.0） |
| LVGL | `lvgl/lvgl` `9.5.0`（示例 02 用） |

ESP-IDF 与 BSP 组合的 `sdkconfig.defaults` 关键项：

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_FREERTOS_HZ=1000
CONFIG_LV_OS_FREERTOS=y
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
```

## 官方示例（`upstream/.../examples/esp-idf/`）

| 示例 | 内容 |
| --- | --- |
| `01_AXP2101` | 用移植版 XPowersLib 读 AXP2101：芯片温度、充放电状态、VBUS、电池电压、电量百分比。**不点屏** |
| `02_lvgl_demo_v9` | LVGL 9 官方 demo（benchmark / music / widgets），跑通屏幕的最佳起点 |
| `03_esp-brookesia` | 手机风格完整 UI：状态栏、导航栏、应用启动器、手势 |
| `04_Immersive_block` | QMI8658 加速度驱动 LVGL 图形跟随倾斜移动，含水平校准算法 |
| `05_Spec_Analyzer` | 实时音频频谱可视化，64 条对称彩色频谱条 + 峰值跟踪 |

Arduino 侧还有 8 个示例（`01_HelloWorld` ~ `08_*`），库在 `examples/arduino/libraries/`。

## 参考入口

- 原理图 PDF：`upstream/ESP32-S3-Touch-AMOLED-1.75C/Schematic/`
- 上游维护者文档：`upstream/ESP32-S3-Touch-AMOLED-1.75C/docs/`
  - `components_ZH.md` — 组件归属与硬件交叉核对结论
  - `firmware_ZH.md` — 工厂固件 vs CI 固件包的区别、烧录布局安全
  - `repository-structure_ZH.md` — 仓库布局约定
