// OTA：HTTP 上传式升级 + 回滚保护
//
// 设计取舍（纯网络 OTA 的保命机制）：
//   * 只往"下一个" OTA 分区写，永远不碰正在运行的分区，也不碰 factory（小智住那儿）。
//   * 开启 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE。新固件启动后处于 PENDING_VERIFY，
//     必须主动"确认"才算数。
//   * **确认条件 = 连上 WiFi 并拿到 IP**。为什么不是"启动成功就确认"：
//     如果新固件能启动但连不上网，我们就再也 OTA 不进去了（只能插 USB）。
//     用"能连上网"当健康检查，连不上就自动回滚，板子自己救回来。
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

// 注册 POST /ota（上传固件）和 GET /rollback（切回另一个槽并重启）
esp_err_t app_ota_register_handlers(httpd_handle_t server);

// 开机时打一份分区/镜像状态到日志
void app_ota_log_state(void);

// 把状态格式化成一行，给 HTTP 状态页用
void app_ota_state_str(char *buf, size_t n);

// 拿到 IP 之后由 main 调用一次：满足健康检查 → 确认镜像，取消回滚
void app_ota_note_online(void);
