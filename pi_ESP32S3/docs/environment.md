# 开发环境：WSL2 + ESP-IDF

## 主机环境

| 项 | 值 |
| --- | --- |
| 宿主 | Windows（DESKTOP-JPDUFHL），WSL2 |
| 发行版 | Ubuntu 22.04.5 LTS |
| 内核 | 6.18.33.2-microsoft-standard-WSL2 |
| systemd | 已启用（`/etc/wsl.conf` → `[boot] systemd=true`） |
| 网络模式 | **mirrored**（`C:\Users\Admin\.wslconfig` → `networkingMode=Mirrored`） |
| WSL IP | `192.168.31.126/24`，网关 `192.168.31.1` |
| 用户 | `zzp`（已在 `dialout` / `plugdev` 组），`sudo` 需要密码 |
| node / npm | v26.3.0 / 11.16.0 |

**mirrored 网络是关键优势**：WSL 直接持有局域网 IP，板子能反向连回 WSL。
做 ESP-IDF OTA（板子来 HTTP 拉取新固件）时**不需要** Windows 端口转发。

## 工具链现状

**已装好**（2026-09-17）：ESP-IDF v5.5.5 在 `~/esp/esp-idf`，
xtensa-esp-elf-gcc 14.2.0，Python 环境在 `~/.espressif/python_env/idf5.5_py3.10_env`。
每个新终端 `source scripts/env.sh` 即可。

### 安装过程中的四个坑（已写进脚本，但值得知道）

1. **tarball 不含 git submodule**（阻塞级）。GitHub 的 tarball 会漏掉 23 个子模块，
   包括 `mbedtls`、`lwip`、`esp_wifi/lib`、`esp_phy/lib`、`esp_coex/lib`、`heap/tlsf`，
   任何一个缺失都编译不了。`install-idf.sh` 会自动调
   `fix-idf-submodules.py` 用 codeload 按 SHA 补齐（~35 秒）。
2. **Python tarfile 的 `filter="data"` 会中断解压**。`esp-nimble` 里有个指向子模块外的
   符号链接，python 过滤器会直接抛异常，**留下半个目录但看起来非空**。
   所以用系统 `tar` 而不是 `tarfile`。
3. **仓库没有 commit → CMake 配置失败**。`project.cmake` 在 `PROJECT_VER` 未设时会调
   `git describe`；仓库没有 `HEAD` 就让 `grabRef.cmake` 报错中止。
   解法：至少做一次提交，或在工程里 `set(PROJECT_VER "0.1.0")`。
4. **忘记 `set-target` 会静默用 esp32**。直接 `idf.py build` 时 target 默认 `esp32`，
   编到 BSP 才报 `GPIO_NUM_45 undeclared`。解法：`sdkconfig.defaults` 里写
   `CONFIG_IDF_TARGET="esp32s3"`，并统一用 `env.sh` 里的 `idf-build`。

## 烧录与监控

```bash
source scripts/env.sh
idf-port                          # 确认 /dev/ttyACM0 在（不在就是 usbipd 掉了，重新 attach）

./scripts/backup-flash.sh         # 改分区表/刷固件前先整片备份（32MB，~3 分钟）
idf-build                         # = idf.py set-target esp32s3 build
idf-fm                            # = idf.py -p $PORT flash monitor
```

### 备份必须分块

USB-Serial-JTAG 上一次性读 32MB 会中途损坏：

```
A fatal error occurred: Corrupt data, expected 0x1000 bytes but received 0xd55 bytes
```

`scripts/backup-flash.sh` 用 1MB 分块 + 换速率重试 + 长度校验解决，32 块全过。

### 目标芯片速查（已实测）

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
USB mode: USB-Serial/JTAG
MAC: 80:45:6b:34:25:18
Detected flash size: 32MB
```

## 工具链安装历史记录

| 组件 | 状态 |
| --- | --- |
| `~/esp/esp-idf` | ✅ v5.5.5（tarball + 子模块补齐） |
| `idf.py` | ✅ 由 `scripts/env.sh` 注入 PATH |
| git / wget / curl / python3 | ✅ |
| libssl-dev / libusb-1.0-0 | ✅ |
| flex / bison / gperf | ✅ |
| cmake / ninja-build / ccache | ✅ |
| python3-pip / python3-venv | ✅ |
| dfu-util | ✅ |

### 一次性装 apt 依赖（需要 sudo 密码）

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
  python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
  libusb-1.0-0
```

装完跑 `./scripts/install-idf.sh`。

## USB：首次烧录的最后一块拼图

WSL2 默认看不到 USB 设备，`/dev/ttyACM*` 不存在。两条路：

### 路线 A：usbipd-win（一次性，之后全部在 WSL 里做）

Windows 上**当前未安装**（`Program Files\usbipd-win` 不存在，winget 也查不到）。
安装包已经下好并校验过：`C:\Users\Admin\Desktop\usbipd-win_5.3.0_x64.msi`
（4,501,504 字节，SHA256 `1C984914...6D938`，与官方 digest 一致）。双击装即可。

**必须先插板子，`usbipd list` 只能看见 Windows 当前枚举到的设备。**

严格按顺序来：

```powershell
# ① 装 MSI（UAC 提权）→ ② 用【数据线】把板子插到电脑
# ③ 管理员 PowerShell：
usbipd list                          # 找 VID:PID 列
usbipd bind --busid=<BUSID>          # 共享，持久，重启后仍有效，只需一次
usbipd attach --wsl --busid=<BUSID>  # 不需要管理员，但每次拔插/重启/设备重置后都要重做
```

```bash
# ④ 回 WSL 验证
ls -l /dev/ttyACM*
```

想还给 Windows：`usbipd detach --busid=<BUSID>`。

#### 踩坑清单

- **线材**：必须是数据线。充电线只有 VBUS 没有 D+/D-，插上什么都不会出现。最常见的坑。
- **设备可能根本不枚举**：板上现在跑的固件未必启用了 USB-Serial-JTAG 外设。
  如果设备管理器里一片空白 → **按住 BOOT，按一下 PWR（或拔插一次 USB），松开 BOOT**，
  强制进 ROM 下载模式。ROM 一定会拉起 USB-Serial-JTAG，此时必然出现 `303a:1001`。
  这个姿势烧录时也用得上（固件占着 USB 不放手时）。
- **不要死记 VID:PID**：`303a:1001`（USB JTAG/serial debug unit）是 ROM/USB-Serial-JTAG 的默认值。
  如果板上固件启用的是 TinyUSB CDC 之类的其他 USB 功能，VID:PID 会不一样。
  `usbipd list` 里**认设备名**比认 PID 稳。
- **attach 后设备从 Windows 消失**是正常的 —— 被"搬"进 WSL 了。
- 顺便 `wsl --update` 更新内核（README 也这么建议）。

### 路线 B：在 Windows 侧烧一次

同样要在 Windows 装 python + esptool，等于把工具链搞成两套，不推荐。

### 结论

**首次烧录绕不开 USB**，因为板子当前固件没有任何网络服务（见 `network.md`）。
但装完 usbipd-win 后，日常开发可以一直待在 WSL 里，Windows 侧不用再管。
烧上我们自己带 OTA 的固件之后，后续迭代可以纯 Wi-Fi，U 盘都不用插。

## 网络坑（已踩，别再踩）

### 1. `git clone https://github.com/...` 卡死 / GnuTLS 报错

现象：`GnuTLS recv error (-110): The TLS connection was non-properly terminated.`，
或 clone 到 120KB 就再也不动。

原因：这个 WSL 的 git 走 HTTP/2 + GnuTLS 组合有问题。

```bash
git config --global http.version HTTP/1.1    # 已设置
```

即使设了，大仓库 clone 仍不稳。**可靠做法**：用 codeload tarball + curl。

```bash
curl -sL -o repo.tar.gz \
  https://codeload.github.com/<owner>/<repo>/tar.gz/refs/heads/main
tar xzf repo.tar.gz -C dest --strip-components=1
```

实测 87MB 的仓库 tarball 12 秒下完。见 `scripts/fetch-upstream.sh`。

### 2. `objects.githubusercontent.com` 不通

GitHub Release 资源的实际域名，国内网络经常被墙：

```
https://github.com                          200  (0.78s)  ✅
https://objects.githubusercontent.com       000  (11.5s) ❌
https://dl.espressif.com                    302           ✅
https://dl.espressif.com/github_assets/     302           ✅
https://components.espressif.com            200           ✅
```

影响：ESP-IDF `install.sh` 默认从 GitHub Releases 下工具链会失败。
对策：用乐鑫的 GitHub 镜像。

```bash
export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets"
```

`scripts/install-idf.sh` 已内置这个设置。组件管理器走
`components.espressif.com`，本身可达，无需处理。

### 3. 长命令会被 pi 的执行器掐断

agent 的 `bash` 工具对长时间无输出的命令会 abort。下载/安装这类长任务用
"后台起 + 循环打印进度"的写法，让管道一直有输出：

```bash
curl -sL -o big.tar.gz URL &
CPID=$!
while kill -0 $CPID 2>/dev/null; do sleep 5; stat -c%s big.tar.gz; done
wait $CPID
```

单纯 `nohup ... &` 不管用：bash 调用返回时后台进程会被一起收掉。

## pip 加速（可选）

国内 PyPI 慢的话：

```bash
pip config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple
```
