// 固件标识 —— 每次要验证 OTA 是否生效时，把这个数字 +1。
// 上线后从 GET / 或串口日志里能看到它，用来确认"跑的确实是新固件"。
#pragma once

#define APP_FW_TAG    "ota-app-1"
#define APP_FW_BUILD  __DATE__ " " __TIME__
