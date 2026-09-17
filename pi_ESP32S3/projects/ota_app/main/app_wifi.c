#include "app_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_wifi";

static EventGroupHandle_t s_evt;
static char               s_ip[16]   = "0.0.0.0";
static char               s_ssid[33] = "";
static volatile bool      s_up;
static volatile bool      s_need_reconnect;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 已启动，开始连接");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "掉线 (reason=%d)，交给看门狗任务重连", d->reason);
        s_up = false;
        strcpy(s_ip, "0.0.0.0");
        s_need_reconnect = true;                 // 不在事件回调里 delay，避免阻塞事件循环
        if (s_evt) xEventGroupClearBits(s_evt, APP_WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_up = true;
        ESP_LOGI(TAG, "拿到 IP: %s", s_ip);
        if (s_evt) xEventGroupSetBits(s_evt, APP_WIFI_CONNECTED_BIT);
    }
}

// 重连看门狗：断开后每 3 秒重试一次
static void wifi_supervisor(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (s_need_reconnect && !s_up) {
            s_need_reconnect = false;
            ESP_LOGI(TAG, "重连 …");
            esp_wifi_connect();
        }
    }
}

esp_err_t app_wifi_start(EventGroupHandle_t evt)
{
    s_evt = evt;

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 先把驱动从 NVS 恢复出来的配置打出来 —— 连不上的时候第一眼就看这里
    wifi_config_t wc = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
        memcpy(s_ssid, wc.sta.ssid, sizeof(s_ssid) - 1);
        ESP_LOGI(TAG, "NVS 恢复出的配置: SSID=\"%s\" 密码=%s 信道=%u",
                 s_ssid, wc.sta.password[0] ? "有" : "无(空)", wc.sta.channel);
        if (s_ssid[0] == '\0') {
            ESP_LOGE(TAG, "NVS 里没有 WiFi 配置！需要先在 sdkconfig 里写死，或用配网流程");
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(wifi_supervisor, "wifi_sup", 3072, NULL, 4, NULL);
    return ESP_OK;
}

bool app_wifi_is_connected(void) { return s_up; }
const char *app_wifi_ip(void)    { return s_ip; }
const char *app_wifi_ssid(void)  { return s_ssid; }

int app_wifi_rssi(void)
{
    if (!s_up) return 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}
