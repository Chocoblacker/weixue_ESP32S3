# OTA 开发流程（从此不用插 USB）

## 一句话

```bash
source scripts/env.sh
cd projects/ota_app && idf.py build
curl --data-binary @build/ota_app.bin http://192.168.31.46/ota
```

板子自己写进另一个槽、校验、重启。**11 秒推 1.29MB**（约 113 KB/s）。

## 分区与启动项规则

```
factory    0x110000  9M     🟢 小智 2.1.0      ← 永久保底，OTA 永不碰
ota_0      0xa10000  4032K  🔵 我们的 app  ┐
ota_1      0xe00000  4032K  🔵 我们的 app  ┘  A/B 轮转
assets/storage/nvs          ← 小智的资源与 WiFi 凭据，不动
otadata                     ← 指向当前该启动哪个槽
```

| otadata | 启动 |
| --- | --- |
| 空（全 0xFF） | `factory` = 小智 |
| `seq=1` | `ota_0` |
| `seq=2` | `ota_1` |

```bash
OT=$IDF_PATH/components/app_update/otatool.py
python3 $OT --port /dev/ttyACM0 erase_otadata          # 回小智
python3 $OT --port /dev/ttyACM0 switch_ota_partition --slot 0   # 跑 ota_0
python3 $OT --port /dev/ttyACM0 read_otadata           # 看当前
```

## 接口

| 请求 | 作用 |
| --- | --- |
| `GET /` | 状态 JSON：固件标识、分区、镜像状态、WiFi、内存、uptime |
| `POST /ota` | 上传固件（body 就是 .bin），写完自动重启 |
| `GET /rollback` | 切到另一个槽并重启（`esp_ota_set_boot_partition` 会先校验镜像，坏镜像会被拒） |
| `GET /reboot` | 重启 |

```bash
curl http://192.168.31.46/                        # 看状态
curl http://192.168.31.46/rollback                # 一键退回上一个槽
curl --data-binary @build/ota_app.bin http://192.168.31.46/ota
```

## 设计取舍（为什么这么做）

### 1. 只写"下一个"槽，永不碰 factory

`esp_ota_get_next_update_partition(NULL)` 自动选非运行的那个 OTA 槽。
`factory` 是保底（小智住那儿），我们的代码一行都不碰它。

顺带：这也是为什么不能信 `idf.py flash` —— 它把 app 写到分区表里**第一个** app
分区即 `factory`，会直接覆盖小智。用 `scripts/flash-app.sh`。

### 2. 确认条件 = 连上 WiFi，而不是"启动成功"

开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。新镜像启动后处于
`PENDING_VERIFY`，必须在下次重启前调用
`esp_ota_mark_app_valid_cancel_rollback()` 才算数，否则 bootloader 回滚。

```c
// app_ota.c: app_ota_note_online()  —— 由 main 在拿到 IP 之后调用
if (st == ESP_OTA_IMG_PENDING_VERIFY) esp_ota_mark_app_valid_cancel_rollback();
```

**为什么不用"启动成功就确认"**：如果新固件能启动但连不上网，我们就再也
OTA 不进去了（只能拆机插 USB）。用"能连上网"当健康检查，连不上就自动回滚，
**板子自己救自己**。

验证过的完整状态链：
```
写入 ota_1 → 重启 → PENDING_VERIFY → 连上网 → VALID(已确认)
                            ↑ 若这一步连不上网，下次重启就回滚回 ota_0
```

### 3. WiFi 凭据不硬编码

板上 NVS 的 `nvs.net80211` 命名空间里已经有 ESP-IDF WiFi 驱动存下的 AP 配置
（brookesia / xiaozhi 连过网留下的），驱动默认 `WIFI_STORAGE_FLASH` 会读回来，
所以 `esp_wifi_connect()` 直接就连上了。启动日志里会打印恢复出的 SSID，方便排查。

## 排错

| 症状 | 原因 / 处理 |
| --- | --- |
| `curl` 连不上 | 串口看 `[状态] ip=`；没有 IP 就看启动日志里"NVS 恢复出的 SSID"对不对 |
| OTA 返回 400 `content_len` 为空 | 必须用 `--data-binary`，且 curl 要带 Content-Length |
| 上传中途断开 | 板子会 `esp_ota_abort`，**不会**破坏运行中的固件，直接重传 |
| 新固件起不来 | 等它自己回滚（连不上网 → 不确认 → 回滚），或 `curl .../rollback` |
| 烧完启动的是小智 | `idf.py flash` 擦了 otadata。用 `scripts/flash-app.sh`，或手动 `switch_ota_partition --slot N` |
| 板子完全没反应 | 看 Windows 是否还有 `VID_303A`；没有就是断电/USB 没插。`usbipd attach` 只解决"在线但没透传" |

## 已知坑

### `bsp_display_lock(0)` 是"不等待"，不是"永久等待"

```c
// espressif__esp_lvgl_adapter/src/adapter/esp_lv_adapter.c
TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
```

所以 `0` → 立即返回（拿不到就失败），**`-1` 才是等到底**。官方 BSP 示例用的就是 `-1`。

踩过的后果：锁没拿到却照样调 LVGL → 与 BSP 的 LVGL 渲染任务并发改对象树 →
链表成环 → **task watchdog 每 5 秒报 `CPU 0: main` 死循环**，
backtrace 停在 `lv_obj_set_style_bg_color`。**永远检查 `bsp_display_lock` 的返回值。**

### `otatool` 写第二个 otadata 条目的偏移和 bootloader 不一致

otatool 用 `spi_flash_sec_size >> 1` = 2048 当条目间距，而 bootloader 按
`SPI_FLASH_SEC_SIZE` = 4096 读两个条目。结果 `read_otadata` 会显示两条一模一样的
`seq`（看着像"两个槽都有效"），其实 2048 那处是 bootloader 不看的垃圾。
判启动槽以 **偏移 0** 的那条为准。

### `idf.py flash` 的三个副作用

1. app 写到 `factory` → 覆盖小智
2. 写 `ota_data_initial.bin` → 擦空 otadata → 启动项丢失
3. 所以本项目一律用 `scripts/flash-app.sh <项目> <槽号>`

## 下一步可以做的

- [ ] 双槽自动交替的回归测试（连推两次，确认 ota_0 → ota_1 → ota_0）
- [ ] 故意推一个"能启动但连不上网"的镜像，验证自动回滚真的生效
- [ ] `POST /ota` 加个 token，避免同网段别人乱刷
- [ ] HTTPS 拉取式 OTA（`esp_https_ota`）+ 版本比较，做成"检查更新"
