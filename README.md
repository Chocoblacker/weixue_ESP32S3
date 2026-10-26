# weixue

微雪（Waveshare）开发板工作区。每个板子一个子目录。

| 目录 | 板子 | 状态 |
| --- | --- | --- |
| [`pi_ESP32S3/`](pi_ESP32S3/) | ESP32-S3-Touch-AMOLED-1.75C | ✅ 工具链打通，build→flash→monitor 闭环已验证 |

## 随身翻译（Muse 底层 + 千问翻译模型）

源码与构建说明：[`pi_ESP32S3/projects/muse-qwen-translator/`](pi_ESP32S3/projects/muse-qwen-translator/)。

面向 Waveshare 1.75C 的随身语音翻译器，以 Muse 提供硬件驱动、UI、音频与设置，使用千问 Realtime 翻译模型实现中英互译。ESP-IDF 6.0.1 构建及 202 项主机测试完成（1 项跳过），本次发布尚未重新完成实机验收，详见项目说明。

原工作区入口：[`pi_ESP32S3/README.md`](pi_ESP32S3/README.md)
