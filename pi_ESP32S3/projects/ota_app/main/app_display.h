// 屏幕上显示一行状态 —— 升级的时候能直接看到进度，不用看串口。
#pragma once

#include "esp_err.h"

esp_err_t app_display_start(void);
void      app_display_status(const char *l1, const char *l2, const char *l3);
