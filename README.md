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
  把键鼠事件编码成标准 USB HID 报告注入手机。USB 传输、键盘和相对鼠标参考 scrcpy 的
  `app/src/usb/aoa_hid.c`、`app/src/hid/hid_keyboard.c`、`app/src/hid/hid_mouse.c`。
  默认指针使用支持悬停的 **HID 数位板绝对坐标**，直接把 Deskflow 坐标映射到手机全屏，
  避免 Android 鼠标加速造成坐标漂移。

两者串起来就是：`主机键盘鼠标 → deskflow → 本程序 → AOA HID → 手机`。

## 依赖

- Linux、macOS（Intel / Apple Silicon）或 Windows（x64）
- GCC / Clang / MSVC（C11）
- CMake ≥ 3.16
- libusb-1.0（开发包）

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libusb-1.0-0-dev

# macOS（先安装 Homebrew）
xcode-select --install
brew install cmake libusb

# Windows：Visual Studio C++ 工具链、CMake、vcpkg
vcpkg install libusb:x64-windows-static
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

macOS 构建命令相同，默认会查找 Apple Silicon 的 `/opt/homebrew` 和 Intel 的
`/usr/local` 下的 libusb。使用自定义 Homebrew 安装路径、或提示找不到 libusb 时：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix libusb)"
cmake --build build
```

编译器与 libusb 必须使用相同架构；不要混用 Rosetta 下的 Intel Homebrew 与原生
Apple Silicon 编译器。构建和 socket 回归测试可用 `ctest --test-dir build --output-on-failure`
验证；CI 覆盖 Linux、Intel macOS、Apple Silicon macOS 和 Windows，不包含手机 USB 真机测试。

Windows（PowerShell，使用 Visual Studio x64 工具链；`VCPKG_ROOT` 指向 vcpkg 目录）：

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/Release/deskflow-otg.exe --help
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

> **Linux**：需要 root 或 udev 规则才能访问 USB 设备。不想要 `sudo` 的话，配置一条
> udev 规则即可（见下文）。**macOS**：先不带 `sudo` 运行；不使用 udev，见下面的说明。

**Windows**：在 PowerShell 中运行 `./build/Release/deskflow-otg.exe --host 127.0.0.1
--name android --width 1080 --height 1920 -s <手机USB序列号>`。需安装可供 libusb 使用的
USB 驱动（例如通过 Zadig 将手机对应 USB 接口绑定 WinUSB）；更换驱动可能影响 MTP/ADB，
请确认选中的设备/接口并备份原驱动。Deskflow 的 TLS 仍需关闭。Windows USB 真机行为尚未验证。

### macOS 运行与权限

```bash
./build/deskflow-otg --host 127.0.0.1 --name android \
    --width 1080 --height 1920 -s <手机USB序列号>
```

- 在“系统设置 → 隐私与安全性”中允许 **Deskflow** 的辅助功能权限；如果系统提示需要
  输入监控权限，也应允许。这些权限用于 Deskflow 捕获主机键鼠，本程序只接收 TCP 事件。
- 使用支持数据传输的 USB 线；Mac 提示是否允许配件连接时，解锁 Mac 并选择允许。
  不需要开启手机 USB 调试。
- Mac 的其它 USB 设备也可能带序列号，建议通过 `-s` 指定手机；未指定时程序会列出
  候选设备，多个候选时不会自动选择。
- 遇到 USB 打开失败或 `ACCESS/BUSY` 错误时，先关闭其它占用手机的程序
  （例如 scrcpy、Android 文件传输工具），重新插拔后重试。`sudo` 不能解决所有设备
  占用或 macOS 配件授权问题，不需要关闭 SIP 或卸载系统驱动。
- macOS 的构建与运行路径已适配，但具体手机的 AOA HID、拔线检测和绝对指针兼容性
  仍需真机验证。

### 3. 操控手机

把鼠标移出主机屏幕边缘（滑向"手机"那块屏幕），此时键盘鼠标就会注入到手机。
把鼠标再滑回主机屏幕边缘，即可回到主机。

默认 `--mouse-mode absolute`：移动时悬停，按住左键拖动时连续按下，离开手机屏幕时松开。
建议把 `--width/--height` 设为手机当前方向的分辨率，以获得自然的移动比例；绝对模式会
把虚拟屏幕完整映射到手机显示区域，尺寸不一致也不会累积相对位移误差。

> **退出程序**：当鼠标停留在"手机"屏幕上时，键盘（包括 Ctrl+C）都被转发给了手机，
> 不会到达终端。要退出，先把鼠标滑回主机屏幕，再按 Ctrl+C；或者直接拔掉手机 ——
> 本程序会检测到 USB 断开、自动退出，并让鼠标回到主机。

## 命令行参数

```
  -H, --host HOST       Deskflow server 地址 (默认 127.0.0.1)
  -p, --port PORT       Deskflow server 端口 (默认 24800)
  -n, --name NAME       上报给 server 的屏幕名 (默认 android)
  -s, --serial SERIAL   安卓设备的 USB 序列号 (不指定则自动检测)
      --width W         虚拟屏幕宽度，1..32767 (默认 1080)
      --height H        虚拟屏幕高度，1..32767 (默认 1920)
      --mouse-mode MODE absolute（默认）或 relative（兼容模式）
  -h, --help            帮助
```

### 鼠标提前碰到“看不见的边界”

旧版把 Deskflow 绝对坐标之差直接作为相对 HID 位移。但 Android 会按鼠标速度、加速设置
改变实际移动距离，导致 server 认为已经到达边界，手机指针却还在屏幕中间。仅在进入时
归零、拆分大位移或调整分辨率，不能保证消除这种偏差。

新版默认的绝对坐标模式使用数位板的 `Tip Switch + In Range + ABS X/Y`，每个报告同时
包含当前位置和按键状态，不依赖上一次指针位置。它仍通过 AOA 工作，无需 adb。
Android 会把它识别为带鼠标按键的数位板/笔，个别应用对笔和鼠标的响应可能不同。
如果设备不支持该模式，可加 `--mouse-mode relative` 使用原有相对鼠标；该模式仍受
Android 加速影响，不能保证指针与 Deskflow 边界一致。

## Linux 免 root 访问 USB（udev 规则）

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
| `src/bridge.c`（键盘、相对鼠标） | scrcpy `app/src/hid/hid_keyboard.c` + `app/src/hid/hid_mouse.c` |
| `src/bridge.c`（绝对指针） | HID Digitizer 规范；Android `TouchInputMapper::dispatchPointerStylus` |
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
- 默认绝对指针依赖 Android 的 HID 数位板支持，具体设备兼容性需真机验证。
- `--mouse-mode relative` 为兼容模式，Android 鼠标加速可能造成漂移、提前碰边。
- 需要先能访问 USB 设备（Linux 使用 root 或 udev 规则；macOS 需允许 USB 配件连接；Windows 需兼容 libusb 的驱动）。
