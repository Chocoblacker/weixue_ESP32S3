#include "app_display.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app_disp";

// 锁的超时语义坑：esp_lv_adapter_lock(timeout_ms) 里
//     ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
// 所以 **0 = 不等待（立即失败）**，-1 才是“等到底”。
// 而拿不到锁就去调 LVGL，会和 BSP 的 LVGL 渲染任务并发修改对象树 → 链表成环 → 死循环。
// 官方 BSP 示例用的是 -1。
#define DISP_LOCK_WAIT (-1)

static lv_display_t *s_disp;
static lv_obj_t     *s_l1;
static lv_obj_t     *s_l2;
static lv_obj_t     *s_l3;

static lv_obj_t *make_label(lv_obj_t *parent, int y, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 18, y);
    lv_label_set_text(l, "");
    return l;
}

esp_err_t app_display_start(void)
{
    // 注意：BSP v3 的签名是 lv_display_t *bsp_display_start(void)，不是 esp_err_t
    s_disp = bsp_display_start();
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start 返回 NULL，屏幕初始化失败");
        return ESP_FAIL;
    }

    esp_err_t err = bsp_display_lock(DISP_LOCK_WAIT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "取 LVGL 锁失败: %s", esp_err_to_name(err));
        return err;
    }

    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    if (scr == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "lv_display_get_screen_active 返回 NULL");
        return ESP_FAIL;
    }
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_l1 = make_label(scr, 30,  &lv_font_montserrat_26, lv_color_white());
    s_l2 = make_label(scr, 110, &lv_font_montserrat_16, lv_color_hex(0x9BE15D));
    s_l3 = make_label(scr, 160, &lv_font_montserrat_16, lv_color_hex(0x8AB4F8));
    bsp_display_unlock();

    ESP_LOGI(TAG, "屏幕已就绪");
    return ESP_OK;
}

void app_display_status(const char *l1, const char *l2, const char *l3)
{
    if (!s_l1) return;
    if (bsp_display_lock(DISP_LOCK_WAIT) != ESP_OK) return;
    if (l1) lv_label_set_text(s_l1, l1);
    if (l2) lv_label_set_text(s_l2, l2);
    if (l3) lv_label_set_text(s_l3, l3);
    bsp_display_unlock();
}
