# deskflow-otg

一个把 [Deskflow](https://github.com/deskflow/deskflow)（键盘鼠标共享）和
[scrcpy](https://github.com/Genymobile/scrcpy) 的 **OTG 模式**（USB AOA HID）连接起来的桥接程序。

在你的主机电脑上运行一个 Deskflow **server**，再运行本程序，手机通过 USB 线插在主机上，
当鼠标滑到"手机"这块屏幕时，就可以用主机的键盘鼠标直接操控安卓手机 ——
**不需要 adb、不需要开启 USB 调试**（和 `scrcpy --otg` 同理）。

```
┌─────────────────────────────── 主机电脑 ───────────────────────────────┐
│                                                                        │
│   deskflow server（持有真实键盘鼠标）                                      │
│        │                                                               │
│        │  Deskflow 协议 (TCP :24800)                                    │
│        ▼                                                               │
│   deskflow-otg  ──(USB AOA HID)──▶ 安卓手机                              │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

手机在 Deskflow 里表现为一块普通的"屏幕"，放在主机屏幕的左/右/上/下即可。

## 工作原理

- **deskflow 侧**：本程序实现了一个极简的 Deskflow *客户端*（secondary screen），
  用 C 语言直接讲 Deskflow / Synergy / Barrier 的 TCP 二进制协议（握手、屏幕信息交换、
  键鼠事件流、keep-alive、CNOP 应答）。协议格式参考 deskflow 的
  `src/lib/deskflow/ProtocolTypes.h`。
- **scrcpy/OTG 侧**：本程序通过 libusb 直接向手机发送 **AOA 2.0 HID** 控制请求
  （`ACCESSORY_REGISTER_HID`=54 / `SET_HID_REPORT_DESC`=56 / `SEND_HID_EVENT`=57），
  把键鼠事件编码成标准 USB HID 键盘/鼠标报告注入手机。HID 报告描述符和编码逻辑
  与 scrcpy 的 `app/src/usb/aoa_hid.c`、`app/src/hid/hid_keyboard.c`、
  `app/src/hid/hid_mouse.c` 逐字节一致。

两者串起来就是：`主机键盘鼠标 → deskflow → 本程序 → AOA HID → 手机`。

## 依赖

- Linux（本项目目标平台）
- GCC / Clang（C11）
- CMake ≥ 3.16
- libusb-1.0（开发包）

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libusb-1.0-0-dev
```

不需要 Qt、不需要 FFmpeg、不需要 SDL —— 这就是本程序相对于"直接链接 deskflow/scrcpy
库"的主要优势（scrcpy 的 `common.h → compat.h` 会拖入 FFmpeg 头文件，deskflow 会拖入 Qt）。

## 构建

```bash
git clone https://your-host/deskflow-otg.git
cd deskflow-otg
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物: build/deskflow-otg
```

## 使用

### 1. 配置 Deskflow server

在 Deskflow 主机上，把"手机"配置成一块屏幕（名字要和下面的 `--name` 一致），
放在主机屏幕旁边。例如：

- 屏幕名：`android`
- 位置：主机屏幕的 **右侧**

### 2. 连接手机并运行本程序

用 USB 线把手机插到主机上，然后：

```bash
sudo ./build/deskflow-otg \
    --host 127.0.0.1 \
    --port 24800 \
    --name android \
    --width 1080 --height 1920
```

> 需要 root 或 udev 规则才能访问 USB 设备（和 `scrcpy --otg` 一样）。不想要 `sudo`
> 的话，配置一条 udev 规则即可（见下文）。

### 3. 操控手机

把鼠标移出主机屏幕边缘（滑向"手机"那块屏幕），此时键盘鼠标就会注入到手机。
把鼠标再滑回主机屏幕边缘，即可回到主机。

> **退出程序**：当鼠标停留在"手机"屏幕上时，键盘（包括 Ctrl+C）都被转发给了手机，
> 不会到达终端。要退出，先把鼠标滑回主机屏幕，再按 Ctrl+C；或者直接拔掉手机 ——
> 本程序会检测到 USB 断开、自动退出，并让鼠标回到主机。

## 命令行参数

```
  -H, --host HOST       Deskflow server 地址 (默认 127.0.0.1)
  -p, --port PORT       Deskflow server 端口 (默认 24800)
  -n, --name NAME       上报给 server 的屏幕名 (默认 android)
  -s, --serial SERIAL   安卓设备的 USB 序列号 (不指定则自动检测)
      --width W         虚拟屏幕宽度，用手机分辨率 (默认 1080)
      --height H        虚拟屏幕高度 (默认 1920)
  -h, --help            帮助
```

## 免 root 访问 USB（udev 规则）

`/etc/udev/rules.d/51-android.rules`：

```
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0666", GROUP="plugdev"
```

（`18d1` 是 Google 的 USB vendor id，其它厂商的安卓设备需要相应补上。）
然后 `sudo udevadm control --reload-rules && sudo udevadm trigger`。

## 目录结构

```
src/
  main.c              入口、参数解析、回调接线
  deskflow_client.c   极简 Deskflow 客户端协议 (TCP 握手 + 消息循环)
  aoa_hid.c           USB AOA HID (libusb 枚举/打开/注册/发送报告)
  bridge.c            键鼠状态机 + HID 报告生成 (对应 scrcpy 的 hid_*)
  keymap.c            deskflow KeyID → USB HID usage 映射
```

## 与 scrcpy / deskflow 的关系（出处说明）

| 本仓库文件 | 对应来源 |
|---|---|
| `src/aoa_hid.c` | scrcpy `app/src/usb/aoa_hid.c` + `app/src/usb/usb.c` |
| `src/bridge.c`（HID 报告描述符与编码） | scrcpy `app/src/hid/hid_keyboard.c` + `app/src/hid/hid_mouse.c` |
| `src/keymap.c`（KeyID 常量） | deskflow `src/lib/deskflow/KeyTypes.h` |
| `src/deskflow_client.c`（协议格式） | deskflow `src/lib/deskflow/ProtocolTypes.h` |

scrcpy 的相关代码采用 Apache-2.0，本仓库亦采用 Apache-2.0（见 `LICENSE`）。

## TLS 说明

deskflow 1.26+ 默认开启 TLS（`security/tlsEnabled`）。本桥接程序目前**只支持明文协议**，
如果 server 开了 TLS，连接会卡住或报错。需要在 Deskflow 设置里把 **TLS 关掉**（Security → TLS，
或者配置文件里 `security/tlsEnabled=false`）再使用。

## 当前限制

- 只支持**明文协议**，不支持 deskflow 的 TLS（见上文）。
- 键盘映射按 **US 布局**（其它布局下部分符号键可能错位）。
- 不支持剪贴板同步、文件拖放、屏幕截图（这些与"操控手机"无关）。
- 只支持键盘 + 鼠标，不支持游戏手柄（AOA gamepad 未实现）。
- 鼠标用**相对位移**（AOA 鼠标是相对 HID），绝对 warp 会被换算成增量。
- 需要先能访问 USB 设备（root 或 udev 规则）。
