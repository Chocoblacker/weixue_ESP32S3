// WiFi STA 连接 —— 用 NVS 里已经存好的配置自动连。
//
// 为什么不用硬编码 SSID/密码：板上 NVS 的 `nvs.net80211` 命名空间里已经有
// ESP-IDF WiFi 驱动自己存下的 AP 配置（brookesia / xiaozhi 连过网留下的），
// 驱动默认 WIFI_STORAGE_FLASH 会把它读回来，所以直接 connect 就能连上。
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define APP_WIFI_CONNECTED_BIT BIT0

esp_err_t app_wifi_start(EventGroupHandle_t evt);

// 由调用方创建 event group 后传进来也可以；方便时用下面这个
bool        app_wifi_is_connected(void);
const char *app_wifi_ip(void);      // 未连上返回 "0.0.0.0"
const char *app_wifi_ssid(void);    // NVS 里恢复出的 SSID
int         app_wifi_rssi(void);    // 未连上返回 0
