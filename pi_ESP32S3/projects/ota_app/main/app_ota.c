#include "app_ota.h"
#include "app_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_ota";

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW(待验证)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY(待确认)";
    case ESP_OTA_IMG_VALID:          return "VALID(已确认)";
    case ESP_OTA_IMG_INVALID:        return "INVALID(已判无效)";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED(未确认被弃)";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "?";
    }
}

static void part_line(const char *what, const esp_partition_t *p, char *out, size_t n)
{
    if (!p) { snprintf(out, n, "%s=无", what); return; }
    snprintf(out, n, "%s=%s@0x%08lx(%luK)", what, p->label,
             (unsigned long)p->address, (unsigned long)(p->size / 1024));
}

void app_ota_state_str(char *buf, size_t n)
{
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    char a[64], b[64];
    part_line("running", run, a, sizeof(a));
    part_line("next", next, b, sizeof(b));
    const char *stn = "-";
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK) stn = state_name(st);
    snprintf(buf, n, "%s %s state=%s", a, b, stn);
}

void app_ota_log_state(void)
{
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *d    = esp_app_get_description();

    ESP_LOGI(TAG, "固件标识 : %s  (构建 %s)", APP_FW_TAG, APP_FW_BUILD);
    ESP_LOGI(TAG, "app desc : %s %s  IDF %s", d->project_name, d->version, d->idf_ver);
    if (run)  ESP_LOGI(TAG, "运行分区 : %s @0x%08lx  %lu KiB",
                       run->label, (unsigned long)run->address, (unsigned long)(run->size / 1024));
    if (boot) ESP_LOGI(TAG, "启动分区 : %s (%s)", boot->label,
                       (run == boot) ? "就是它" : "注意：不是正在跑的这个！");
    if (next) ESP_LOGI(TAG, "OTA 目标 : %s @0x%08lx  %lu KiB",
                       next->label, (unsigned long)next->address, (unsigned long)(next->size / 1024));
    else      ESP_LOGE(TAG, "OTA 目标 : 没有可写的 OTA 分区！");

    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK) {
        ESP_LOGI(TAG, "镜像状态 : %s", state_name(st));
    } else {
        ESP_LOGI(TAG, "镜像状态 : 非 OTA 分区（factory），无需确认");
    }
}

void app_ota_note_online(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return;

    esp_ota_img_states_t st;
    esp_err_t err = esp_ota_get_state_partition(run, &st);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "健康检查通过，但当前分区 %s 不是 OTA 分区，无需确认", run->label);
        return;
    }
    ESP_LOGI(TAG, "健康检查通过（已联网），当前镜像状态 = %s", state_name(st));

    if (st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) ESP_LOGI(TAG, "✅ 已确认镜像有效，回滚取消");
        else               ESP_LOGE(TAG, "确认失败: %s", esp_err_to_name(err));
    }
}

// ───────────────────────── HTTP 处理 ─────────────────────────

static void restart_soon(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));   // 给 HTTP 响应一点时间发出去
    ESP_LOGI(TAG, "重启进入新固件 …");
    esp_restart();
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);

    if (target == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "没有可写的 OTA 分区");
        return ESP_FAIL;
    }
    if (running == target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "目标分区 == 运行分区，拒绝");
        return ESP_FAIL;
    }

    const size_t total = req->content_len;
    if (total == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "需要 Content-Length（用 curl --data-binary）");
        return ESP_FAIL;
    }
    if (total > target->size) {
        char m[96];
        snprintf(m, sizeof(m), "固件 %u 字节 > 分区 %lu 字节", (unsigned)total,
                 (unsigned long)target->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, m);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "开始 OTA：%s -> %s，上传 %u 字节 (%u KiB)",
             running ? running->label : "?", target->label, (unsigned)total, (unsigned)(total / 1024));

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    char  *buf     = malloc(4096);
    size_t written = 0;
    if (!buf) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "malloc 失败");
        return ESP_FAIL;
    }

    while (written < total) {
        size_t want = total - written;
        if (want > 4096) want = 4096;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            ESP_LOGE(TAG, "接收中断（written=%u/%u, r=%d），放弃本次 OTA",
                     (unsigned)written, (unsigned)total, r);
            esp_ota_abort(handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "传输中断");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write 失败: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
        size_t before = written;
        written += r;
        if (before / (256 * 1024) != written / (256 * 1024)) {
            ESP_LOGI(TAG, "  … %u / %u KiB", (unsigned)(written / 1024), (unsigned)(total / 1024));
        }
    }
    free(buf);

    err = esp_ota_end(handle);          // 会校验镜像完整性/合法性
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 失败: %s（镜像可能损坏）", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置启动分区失败: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✅ OTA 写入完成并校验通过，%u 字节 → %s，即将重启", (unsigned)written, target->label);

    char msg[192];
    snprintf(msg, sizeof(msg), "OK: %u bytes written to %s, rebooting\n",
             (unsigned)written, target->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);

    xTaskCreate(restart_soon, "ota_restart", 2560, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t rollback_get_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other   = NULL;

    if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    } else {
        other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    }
    if (!other) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "找不到另一个 OTA 分区");
        return ESP_FAIL;
    }

    // esp_ota_set_boot_partition 会先校验镜像；坏镜像会被拒绝（这正是我们要的安全性）
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切到 %s 失败: %s（镜像可能无效）", other->label, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "手动切换启动分区 -> %s，即将重启", other->label);
    char msg[128];
    snprintf(msg, sizeof(msg), "OK: boot -> %s, rebooting\n", other->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    xTaskCreate(restart_soon, "rb_restart", 2560, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t app_ota_register_handlers(httpd_handle_t server)
{
    httpd_uri_t post_ota = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL,
    };
    httpd_uri_t get_rb = {
        .uri = "/rollback", .method = HTTP_GET, .handler = rollback_get_handler, .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post_ota));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_rb));
    ESP_LOGI(TAG, "已注册 POST /ota 与 GET /rollback");
    return ESP_OK;
}
