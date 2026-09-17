// ota_app —— 第一个"能自己升级自己"的固件
//
// 跑在 ota_0 / ota_1 双槽里（factory 留着给小智当代底）。
// 提供：
//   GET  /           状态 JSON（版本、分区、镜像状态、WiFi、内存）
//   POST /ota        上传固件（curl --data-binary @build/ota_app.bin）
//   GET  /rollback   切回另一个槽并重启
//   GET  /reboot     重启
//
// 屏幕会显示 IP / 版本 / 分区，升级完看一眼就知道跑的是不是新固件。
#include <stdio.h>
#include <string.h>

#include "app_display.h"
#include "app_ota.h"
#include "app_version.h"
#include "app_wifi.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

static EventGroupHandle_t s_evt;
static httpd_handle_t     s_httpd;
static char               s_status_line[160];

static void restart_soon(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char ota[192];
    app_ota_state_str(ota, sizeof(ota));

    char *json = malloc(1024);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    snprintf(json, 1024,
             "{\n"
             "  \"fw\": \"%s\",\n"
             "  \"build\": \"%s\",\n"
             "  \"project\": \"%s\",\n"
             "  \"app_version\": \"%s\",\n"
             "  \"idf\": \"%s\",\n"
             "  \"uptime_ms\": %lld,\n"
             "  \"wifi\": {\"ssid\": \"%s\", \"ip\": \"%s\", \"rssi\": %d, \"connected\": %s},\n"
             "  \"heap\": {\"internal_free\": %u, \"psram_free\": %u},\n"
             "  \"ota\": \"%s\",\n"
             "  \"hint\": \"POST /ota 上传固件；GET /rollback 切槽；GET /reboot 重启\"\n"
             "}\n",
             APP_FW_TAG, APP_FW_BUILD,
             d->project_name, d->version, d->idf_ver,
             (long long)(esp_timer_get_time() / 1000),
             app_wifi_ssid(), app_wifi_ip(), app_wifi_rssi(),
             app_wifi_is_connected() ? "true" : "false",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             ota);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t reboot_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK: rebooting\n");
    xTaskCreate(restart_soon, "wb_reboot", 2560, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;   // OTA 处理里要 malloc + 打日志，栈给小了会炸
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t get_status = {.uri = "/",         .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL};
    httpd_uri_t get_reboot = {.uri = "/reboot",   .method = HTTP_GET, .handler = reboot_get_handler, .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &get_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &get_reboot));
    ESP_ERROR_CHECK(app_ota_register_handlers(s_httpd));

    ESP_LOGI(TAG, "HTTP 服务已启动，端口 %d", cfg.server_port);
    return ESP_OK;
}

// 每秒钟刷新屏幕，每 10 秒往串口打一次状态
static void status_task(void *arg)
{
    (void)arg;
    int tick = 0;
    while (1) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        char l2[64], l3[64];

        if (app_wifi_is_connected()) {
            snprintf(s_status_line, sizeof(s_status_line), "%s", app_wifi_ip());
            snprintf(l2, sizeof(l2), "wifi ok  rssi %d", app_wifi_rssi());
        } else {
            snprintf(s_status_line, sizeof(s_status_line), "no network");
            snprintf(l2, sizeof(l2), "wifi: %s", app_wifi_ssid()[0] ? "connecting" : "no creds");
        }
        snprintf(l3, sizeof(l3), "%s @ %s", APP_FW_TAG, run ? run->label : "?");

        app_display_status(s_status_line, l2, l3);

        if (tick % 10 == 0) {
            char ota[192];
            app_ota_state_str(ota, sizeof(ota));
            ESP_LOGI(TAG, "[状态] ip=%s rssi=%d heap=%u/%u | %s",
                     app_wifi_ip(), app_wifi_rssi(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), ota);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "================ ota_app 启动 ================");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除重建");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_ota_log_state();
    app_display_start();
    app_display_status("booting", "", APP_FW_TAG);

    s_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(app_wifi_start(s_evt));

    app_display_status("wifi connecting", app_wifi_ssid(), APP_FW_TAG);
    ESP_LOGI(TAG, "等 IP（最多 30 秒）…");
    EventBits_t bits = xEventGroupWaitBits(s_evt, APP_WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    if (bits & APP_WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi 就绪: %s", app_wifi_ip());
        app_ota_note_online();          // 健康检查通过 → 确认镜像，取消回滚
        ESP_ERROR_CHECK(start_httpd());
        ESP_LOGI(TAG, "★ 升级命令: curl --data-binary @build/ota_app.bin http://%s/ota", app_wifi_ip());
        ESP_LOGI(TAG, "★ 状态查询: curl http://%s/", app_wifi_ip());
    } else {
        ESP_LOGE(TAG, "30 秒没拿到 IP。HTTP 服务照常启动（可插网线/U 盘后重试），"
                      "但没网就没法 OTA —— 看看上面 NVS 恢复出的 SSID 对不对");
        ESP_ERROR_CHECK(start_httpd());
    }

    xTaskCreate(status_task, "status", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "================ 启动完成 ================");
}
