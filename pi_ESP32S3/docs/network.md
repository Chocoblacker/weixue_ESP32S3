# 板子网络现状与 OTA 计划

## 已确认的事实（2025-09 探测）

| 项 | 值 | 依据 |
| --- | --- | --- |
| 板子 IP | `192.168.31.46` | 用户告知 + 本机探测 |
| MAC | `80:45:6b:34:25:18` | `ip neigh` → OUI `80:45:6B` = **Espressif Inc.**，确认是这块板子 |
| ICMP | 通 | `ping` 3/3，RTT 23–99ms（抖动大，典型的 Wi-Fi modem sleep） |
| TCP 端口 | **1–65535 全关** | 全端口异步扫描，无任何监听 |
| WSL 侧 IP | `192.168.31.126` | mirrored 网络模式 |

结论：**板子当前固件不带任何网络服务，无法 OTA / 无法远程烧录。**

## 这对开发流程意味着什么

```
现在：  固件在跑，连了 Wi-Fi，但不监听任何端口 → 只能 USB 烧录
第一步：USB 烧一个带 OTA 的固件（需要 usbipd-win，见 environment.md）
之后：  idf.py build → OTA 推送 → 看日志，全程 Wi-Fi，不用插线
```

### 为什么第一步必须 USB

ESP32-S3 的 ROM bootloader 只支持 UART / USB-Serial-JTAG 下载，没有网络引导。
要在网络上换固件，**必须**板子上已经跑着一个支持 OTA 的固件。当前固件不支持，
所以鸡生蛋的问题只能靠一次 USB 解决。

## 后续 OTA 方案（待实现）

板子已被授予 OTA 能力后，两条主流路线：

1. **ESP-IDF 官方 `esp_https_ota` / `esp_ota_ops`**——我们的固件起一个 HTTP(S) client
   从 WSL 的临时 HTTP 服务器拉 `app.bin`。因为 WSL 有真实局域网 IP
   （mirrored 模式），板子能直接连 `192.168.31.126:8000`，**不需要 Windows 端口转发**。
2. **反向 OTA（板子起 HTTP server 接收上传）**——方便但要在板子侧实现上传接口。

推荐路线 1，配一个 `scripts/ota.sh`：构建 → 起 `python3 -m http.server` →
用板子上的命令（HTTP 请求 / 串口触发 / 按键触发）拉起升级。

## 待办

- [ ] 给板子配 DHCP 静态租约（或固件里写死 IP），避免 IP 漂移
- [ ] 确认当前跑的是哪个固件（首次接 USB 后用 `esptool.py flash_id` +
      `esptool.py read_mac` 与 `80:45:6b:34:25:18` 对一下）
- [ ] 首个自研固件就跑通 OTA 通道，别等到要迭代时才补
