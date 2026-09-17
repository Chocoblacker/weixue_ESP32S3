# 硬件笔记：ESP32-S3-Touch-AMOLED-1.75C

## 核心器件

| 功能 | 器件 / 接口 | 备注 |
| --- | --- | --- |
| MCU | ESP32-S3R8 | Xtensa 双核 LX7 @240MHz，512KB SRAM + 384KB ROM |
| PSRAM | 8MB，Octal（OPI） | 叠封在 SoC 内 |
| Flash | 32MB NOR，QIO 模式 | **已硬件确认**，见下 | |
| 显示 | CO5300，QSPI，466×466 | 1.75" 电容触摸 AMOLED，16.7M 色 |
| 触摸 | CST9217，I2C | 电容触摸控制器 |
| 电源管理 | AXP2101 | 充电 + 电池管理 + 多路电压输出 |
| IMU | QMI8658 | 6 轴：3 轴加速度 + 3 轴陀螺仪 |
| 音频采集 | ES7210 | 双麦克风阵列 ADC，带回声消除电路 |
| 音频输出 | ES8311 codec + 板载扬声器 | 扬声器焊盘在板载 |
| 按键 | PWR、BOOT | PWR 可软件自定义 |
| 电池 | MX1.25 2PIN，3.7V 锂电 | 支持充放电 |
| 接口 | Type-C | 直连 ESP32-S3 原生 USB（USB-Serial/JTAG） |

### ⚠️ 微雪文档矛盾：Flash 容量 —— 已用硬件确认

微雪产品页自相矛盾——"产品特性"写 **16MB**，"板载资源"写 **32MB**。

`esptool flash_id` 实测：`Detected flash size: 32MB`（manufacturer `c8` = GigaDevice，
device `4019`）。仓库 `sdkconfig.defaults` 里也是 `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`，
`partitions.csv` 也按 32MB 布局。

**结论：32MB，"16MB"是错的。**

### PSRAM 注意：8MB 是 Octal，但能用多少取决于固件

芯片是 ESP32-S3R8，`esptool` 报 `Embedded PSRAM 8MB`，bootloader 报
`octal_psram: density 0x03 (64 Mbit)`。但 **8MB 需要固件开 Octal 模式才拿得到**：

| 固件 | 实测可用 PSRAM | 说明 |
| --- | --- | --- |
| 官方 BSP 示例（我们的构建） | **7360K** | `CONFIG_SPIRAM_MODE_OCT=y`，用满 |
| 板上原有的 esp-brookesia | 4043484 字节 (≈3.8MB) | 实际是 QUAD 模式，一半浪费 |

自己建工程时必须带上这一组（否则白丢 4MB）：

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
```

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

## 板子当前固件清单（2026-09-17 首次接管时）

板上不是微雪出厂固件，而是一份带 OTA 分区的布局，里面住着两个不同项目的固件：

| 分区 | 偏移 | project | version | IDF |
| --- | --- | --- | --- | --- |
| `factory` | 0x110000 | **esp-brookesia**（微雪手机 Demo） | ac40993 | v5.5.2-249-gf56bea3d1f-dirty |
| `ota_0` | 0xa10000 | **xiaozhi**（小智 AI 助手） | 2.1.0 | v5.5.2-249-gf56bea3d1f-dirty |
| `ota_1` | 0xe00000 | 空 | | |

`otadata` 两条记录均为空 → bootloader 回落 factory，所以接管时跑的是 brookesia。

**随后已被我们覆盖**：`factory` 现在是 `lvgl_demo_v9`（首次烧录验证），
`ota_0` 的 xiaozhi 与 NVS 保持原样未动。整片备份见下。

### 为什么保留了这份分区表

这份布局自带 `otadata` + `ota_0` + `ota_1`，**OTA 能力是现成的**。
我们没换成微雪示例自带的分区表，因为它会把 `nvs` 放在 `0x9000` 长 `0x6000`，
与板上的 `nvsfactory` 区重叠 —— **WiFi 凭据会被冲掉**。

## 备份（后悔药）

| 文件 | 内容 |
| --- | --- |
| `artifacts/board-backup/flash-2000000-*.bin` | **32MB 整片 flash**（33,554,432 字节，sha256 `2a452d9f…`） |
| `artifacts/board-backup/partitions.bin` | `0x8000` 起 4KB 分区表原始字节 |
| `artifacts/ESP32-S3-Touch-AMOLED-1.75C-FactoryOnly-260114.bin` | 微雪官方出厂固件 |
| `projects/board-partitions.csv` | 板上分区布局的 CSV 形式（新工程直接用它） |

恢复命令：

```bash
# 恢复整片（回到接管前的原样）
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0 artifacts/board-backup/flash-2000000-*.bin

# 只恢复微雪出厂布局 + brookesia Demo
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0 artifacts/ESP32-S3-Touch-AMOLED-1.75C-FactoryOnly-260114.bin
```

## xiaozhi 是什么（挖到底了）

**小智 AI**，开源项目 [`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32)。
不是玩具 demo，是国内非常流行的嵌入式 AI 语音助手框架：

- 离线唤醒词（ESP-SR，默认“你好小智”）
- WebSocket 或 MQTT+UDP 传输，OPUS 编解码，流式 ASR + LLM + TTS
- 声纹识别、屏幕表情、多语言
- **MCP 协议**做设备控制（音量/灯/电机/GPIO）
- 接 Qwen / DeepSeek 等大模型，官方云 `api.tenclass.net` + [xiaozhi.me](https://xiaozhi.me/) 控制台

### 板上这一份：第三方定制版，不是干净的上游发布

| 证据 | 含义 |
| --- | --- |
| `idf_ver = v5.5.2-249-gf56bea3d1f-dirty` | `-dirty` = 用**改过的** IDF 树编译的（和 factory 里的 brookesia 共用同一套） |
| 固件里有 `pcn7cs20v8cr.feishu.cn/wiki/...` | 烧进了**飞书文档链接**，说明来自某个中文教程/店铺的定制 fork |
| 板型串 `WaveshareEsp32s3TouchAMOLED1.75C` / `waveshare-s3-touch-amoled-1.75c` | 对该板有**原生支持**（上游是后来才合入的，来自 fork PR） |
| `api.tenclass.net/xiaozhi/ota/` | 连的是**官方云**，不是自建服务器 |

### 实测行为（otatool 切到 ota_0 后抓的日志）

```
I (2610) MCP: Add tool: self.reboot / self.upgrade_firmware
I (2620) MCP: Add tool: self.screen.snapshot / self.screen.preview_image
I (2630) Assets: The partition size is 9216 KB
E (2730) WifiStation: Failed to open NVS: 4354
W (2740) SsidManager: NVS namespace wifi doesn't exist
I (4250) StateMachine: State: starting -> wifi_configuring
I (4300) WifiConfigurationAp: Access Point started with SSID Xiaozhi-2519
I (4310) esp_netif_lwip: DHCP server started on IP: 192.168.4.1
W (4330) Application: 配网模式: 手机连接热点 Xiaozhi-2519，浏览器访问 http://192.168.4.1
```

### ⚠️ 关键发现：板子上那个 WiFi 不是 xiaozhi 配的

`NVS namespace wifi doesn't exist` —— xiaozhi 自己的 NVS 里**没有** WiFi 凭据，
所以它一启动就进了 AP 配网模式。

**接管时板子连着 `192.168.31.46`，那套凭据属于 `factory` 里的 brookesia**，
不是 xiaozhi 的。两个固件各用各的 NVS 命名空间。

### 想真正玩一下 xiaozhi 的步骤

1. 手机连热点 **`Xiaozhi-2519`**（数字是 MAC 后两字节）
2. 浏览器开 **http://192.168.4.1** → 填你家 WiFi 的 SSID/密码
3. 板子连上网后向 `api.tenclass.net` 要**激活码**，显示在屏上
4. 去 [xiaozhi.me](https://xiaozhi.me/) 注册（有免费 Qwen 实时模型额度）→ 控制台添加设备、填激活码
5. 对着板子说“你好小智”

### 自带的自升级能力（对我们的影响）

xiaozhi 有 `self.upgrade_firmware` 这个 MCP 工具，**它自己能 OTA**，
而且用的是同一套轮转算法 → 如果用它，它可能占掉 `ota_0` / `ota_1`。
按“xiaozhi 不丢就行”的决定，这不是问题：文件在手，直接收回槽位。

## 切换启动分区（不刷固件，只改 otadata）

```bash
source scripts/env.sh
OT=$IDF_PATH/components/app_update/otatool.py

# 启动 xiaozhi（ota_0）
python3 $OT --port /dev/ttyACM0 switch_ota_partition --slot 0

# 启动我们的 lvgl_demo_v9（factory，= 擦空 otadata）
python3 $OT --port /dev/ttyACM0 erase_otadata

# 看当前启动项
python3 $OT --port /dev/ttyACM0 read_otadata
```

> 注意：`otatool` **不接受 `--chip`**（它的 `--esptool-args` 会把 `--chip` 当成自己的值而报错）。
> 不带就行，esptool 自动探测。

## 参考入口

- 原理图 PDF：`upstream/ESP32-S3-Touch-AMOLED-1.75C/Schematic/`
- 上游维护者文档：`upstream/ESP32-S3-Touch-AMOLED-1.75C/docs/`
  - `components_ZH.md` — 组件归属与硬件交叉核对结论
  - `firmware_ZH.md` — 工厂固件 vs CI 固件包的区别、烧录布局安全
  - `repository-structure_ZH.md` — 仓库布局约定
