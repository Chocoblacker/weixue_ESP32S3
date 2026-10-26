# weixue

微雪（Waveshare）开发板工作区。每个板子一个子目录。

| 目录 | 板子 | 状态 |
| --- | --- | --- |
| [`pi_ESP32S3/`](pi_ESP32S3/) | ESP32-S3-Touch-AMOLED-1.75C | ✅ 工具链打通，build→flash→monitor 闭环已验证 |

## Muse + 千问实时翻译（无手卡版）

源码与构建说明：[`pi_ESP32S3/projects/muse-qwen-translator/`](pi_ESP32S3/projects/muse-qwen-translator/)。

面向 Waveshare 1.75C，保留 Muse 本地硬件/UI/音频/设置，使用 Qwen Realtime 中英互译；不含手卡或个人凭据。ESP-IDF 6.0.1 构建及 202 项主机测试完成（1 项跳过），本次精简发布未重新烧录验收，详见目录说明。

原工作区入口：[`pi_ESP32S3/README.md`](pi_ESP32S3/README.md)
