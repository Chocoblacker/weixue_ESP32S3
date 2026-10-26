# Muse 本地系统 + 千问实时语音翻译

面向 **Waveshare ESP32-S3-Touch-AMOLED-1.75C** 的独立翻译固件：复用 Muse 的板级驱动、圆屏 UI 基础、音频、输入、电源管理和设置，连接 Qwen Realtime 做中英互译。**不包含手卡、讲稿、应用选择器或手卡字体。**

> 这里的「Muse」指本地硬件/系统基础，不是 Muse 云端聊天与千问翻译双服务切换。翻译启动路径不启动 Muse 云端配对/账号会话；上游相关源码仍保留以减少非必要重构。本项目与 Meta、阿里云无隶属关系。

## 功能

- 按住上方按键录音，松开翻译；最长 12 秒。
- 录音时异步建立 `target=en`、`target=zh` 两路 WebSocket，边录边发送；依据原文选择输出方向并取消另一会话。不使用独立前置 ASR。
- 原文/译文聊天记录、中文字体、语音播放、静音/纯文字、录音打断。
- 左滑进入设置：Wi-Fi、音量、麦克风、亮度、睡眠、API 地址、Realtime URL、模型和掩码 Key。
- 有界录音/响应缓冲；TLS 证书验证保持开启。

双路请求可能产生两路计费；上游兼容服务不一定支持此模型、事件或输出格式。这里的实时翻译是按键分段录音/流式传输，不是常开同声传译。

## 硬件与构建

ESP32-S3R8 / 8 MB Octal PSRAM / 32 MB Flash；CO5300 466×466 AMOLED、CST9217、ES7210 双麦、ES8311、AXP2101。仅对 `s3`（1.75C）配置做本次编译验证，其他上游板型未验证。

使用 **ESP-IDF v6.0.1**，LVGL **9.5.0**（依赖锁在 `esp32/dependencies.lock`）。

```sh
. /path/to/esp-idf-v6.0.1/export.sh
cd pi_ESP32S3/projects/muse-qwen-translator/esp32
./tools/build-translator.sh
```

输出：`esp32/build-public/muse-gadget.bin`。脚本只构建，不烧录。

首次使用在设备设置页填写自己的 Key；仓库与公开构建均不包含 Key、Wi-Fi 密码或 Muse SDK Token。默认端点是此前实验使用的第三方兼容接口 `maas.qianwenaiapi.com`，**不是阿里云官方域名**：仅在信任运营方时使用，在输入 Key 前检查/修改 Realtime URL。REST base 是独立配置项，不会自动改写 WS 地址。

默认模型 `qwen3.8-livetranslate-flash-realtime`，可在设备设置修改；使用其他服务时须确认模型与 Realtime 协议兼容。

## 安全烧录

**先做设备备份并核对实际分区表与活动槽。不要对已有固件直接运行 `idf.py flash` 或擦除整片。** 上游 `flash` 会写引导、分区表和初始 OTA 数据；本项目不会自动改动设备。

- Muse 布局的应用槽为 `ota_0=0x20000`、`ota_1=0x420000`，各 4 MiB；NVS 为 `0x11000`、OTA 数据为 `0x1d000`。其他布局不适用。
- 若设备已使用该布局且确认启动 `ota_1`，备份当前应用/NVS/OTA 数据后，可仅替换应用：

```sh
python -m esptool --chip esp32s3 -p PORT -b 460800 \
  --after hard-reset write-flash 0x420000 build-public/muse-gadget.bin
```

- 首次迁移到 Muse 分区布局会覆盖原布局，必须另行规划；上面不是通用首次安装步骤。
- 不启用 Secure Boot、Flash Encryption 或不可逆 eFuse 操作；保留 USB 恢复。
- NVS 当前是明文，可能保存 Wi-Fi 和 API Key；备份必须保密。设备端清空 Key 不会清理历史备份。
- 上游 `esp32/dev_signing_key.pem` 是公开的共享开发签名键，**不能作为生产信任根**。

## 验证状态与来源

源自官方 Muse SDK 基线 `86cf33fb4092ba700b4dc33928966d1bcb31556d`，以及本地最新版翻译实现。此发布版是从翻译+手卡工程分离、删除手卡/导航入口并移除密钥种子的源码版本，**不是声称恢复了某个历史固件的逐字节源码**。

本次检查：

- ESP-IDF 6.0.1 / s3 全新构建通过，镜像 `0x2b1000`，4 MiB 槽剩余 33%。
- 上游主机测试：198 项，1 项跳过；另有本地发布约束测试（命令如下）。
- 没有为开源整理而烧录设备；当前整理版尚未重新完成实机语音、取消、设置持久化和连续运行验收。因此不宣称已证明「最稳定」。历史实机使用经验不能替代本次精简版硬件验收。
- 桌面模拟器保留的是上游版本，未适配此翻译 UI，不作为本项目 UI 的验证依据。

```sh
cd esp32
python3 -m unittest discover -s tests -p 'test_*.py'
```

## 关键代码

- `components/muse/translator.c/.h`：配置、NVS、聊天历史。
- `components/muse/muse_voice.c`：录音、双目标 Realtime、播放与取消。
- `components/muse/muse_ui.c`：双语聊天与录音进度。
- `components/muse/muse_settings_ui.c`：设备侧配置编辑。
- `main/app.c`：本地 Wi-Fi 保活入口，不启动云端配对。

上游通用文档在 `esp32/README.md` / `esp32/AGENTS.md`，其中云配对、头像和通用 flash 步骤不代表此翻译版本的使用方式，以本文为准。

## 许可与上游归属

SDK 代码保留上游 Apache-2.0 许可证及版权声明，见 [LICENSE](LICENSE)。第三方文件继续按原许可分发（minimp3 CC0、pixel_font BSD 等）。默认 Jollybot 头像有独立 Meta 版权声明，**不能据此将其宣称为 Apache-2.0**；字体许可见 `esp32/components/muse/fonts/README.md`，依赖组件按各自许可证。未将个人头像、私密讲稿、Flash/NVS 备份、构建产物或本地凭据纳入此发布目录。
