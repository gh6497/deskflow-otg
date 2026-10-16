# deskflow-otg

一个把 [Deskflow](https://github.com/deskflow/deskflow)（键盘鼠标共享）和
[scrcpy](https://github.com/Genymobile/scrcpy) 的 **OTG 模式**（USB AOA HID）连接起来的桥接程序。

在你的主机电脑上运行一个 Deskflow **server**，再运行本程序，手机通过 USB 线插在主机上，
当鼠标滑到"手机"这块屏幕时，就可以用主机的键盘鼠标直接操控安卓手机 ——
**不需要 adb、不需要开启 USB 调试**（和 `scrcpy --otg` 同理）。

提供命令行和可选的 **Qt 桌面 GUI**。GUI 可以通过 ADB 自动识别设备序列号、宽高和
方向，配置并启动已安装的 Deskflow，再一键连接手机。只有 ADB 读取设备与尺寸需要 USB 调试；
手动填写设备信息、已记住手机的 USB 自动连接均不需要。详见[桌面 GUI](#桌面-gui)。

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

Linux 另提供 **Bluetooth LE HID** 传输，可不接 USB 控制手机。CLI 与 GUI 使用同一
`deskflow-otg` 程序，常驻 GATT 服务和桥接会话是不同进程，不是额外的可执行程序。
USB 仍为默认方式；macOS/Windows 暂不提供蓝牙后端。

## 依赖

- Linux、macOS（Intel / Apple Silicon）或 Windows（x64）
- GCC / Clang / MSVC（C11）
- CMake ≥ 3.16
- libusb-1.0（开发包）

Linux BLE HID 后端还需要 BlueZ、GLib/GIO 开发包：

```bash
sudo apt install libusb-1.0-0-dev libglib2.0-dev bluez
```

Linux 默认编译 BLE 后端。仅需原有 USB CLI 时可用
`cmake -S . -B build -DBUILD_BLE=OFF`，不要求 GLib/GIO 或 pkg-config；非 Linux 默认不编译 BLE。

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libusb-1.0-0-dev

# macOS（先安装 Homebrew）
xcode-select --install
brew install cmake libusb

# Windows：Visual Studio C++ 工具链、CMake、vcpkg
vcpkg install libusb:x64-windows-static
```

命令行桥接不需要 Qt、不需要 FFmpeg、不需要 SDL —— 这就是本程序相对于"直接链接 deskflow/scrcpy
库"的主要优势（scrcpy 的 `common.h → compat.h` 会拖入 FFmpeg 头文件，deskflow 会拖入 Qt）。
可选 GUI 另需 C++17 编译器和 Qt 6.8+（Widgets、Network、LinguistTools，测试另需 Test 模块）。

## 桌面 GUI

支持 Windows x64、Linux x86_64、macOS Intel / Apple Silicon。GUI 发布包包含桥接程序、
Qt 运行库和 ADB 36.0.2；**不包含 Deskflow**，请自行安装。GUI 启动时优先发现自带 ADB，
也可以在界面指定外部 ADB。

GUI 支持中文和英文，可在窗口右上角切换，立即生效并自动保存。首次启动跟随系统语言
（中文系统使用中文，其他系统使用英文）。切换语言不会改变设备、连接参数或中断连接；
已有日志和 ADB / Deskflow / 桥接子进程的原始输出不重新翻译。
使用 Qt 官方 ID 式国际化，代码通过 `qtTrId("otg.action.connect")` 等稳定 ID 引用文案。
中英文分别维护在 `gui/translations/deskflow_otg_zh_CN.xml` 和 `deskflow_otg_en.xml`，
构建时使用 `lrelease -idbased -nounfinished` 编译并嵌入程序，无需额外复制翻译文件。
英文目录始终作为回退层，中文翻译缺失时显示英文，而不是直接显示 ID。

文件内容仍采用 Qt TS XML 格式，使用 `.xml` 后缀以避免 IDE 将其误识别为 TypeScript。
新增文案时，在两个 `.xml` 中添加相同的 `<message id="otg.…">`，`<source>` 统一使用英文，
`<translation>` 分别填写中英文；修改文案不需要修改 ID。目录直接维护或用 Qt Linguist 编辑，
不从代码运行 `lupdate` 重新生成（代码不包含源文案）。`gui_logic` 测试检查 ID 唯一性、
双语覆盖、代码引用、占位符和编译后实际查找结果。

### 使用流程

1. 启动 `bin/deskflow-otg-gui`（Windows 为 `.exe`）；macOS 打开 `deskflow-otg-gui.app`。
2. 手机通过 USB 连接，开启 USB 调试并授权，点击「刷新设备」。启动 GUI 时也会自动读取。
   列表显示型号、ADB 序列号和在线/授权状态，不选择无线 ADB 或模拟器。
3. 选择手机，自动读取 `wm size`，优先采用 Override size，并根据屏幕方向交换宽高。
   方向无法识别时会提示手动调整。识别结果作为「原始屏幕尺寸」，可设置「灵敏度倍率」
   （1～10×）等比例缩小虚拟尺寸，界面实时显示实际虚拟尺寸；
   连接期间转屏后需断开、刷新或交换宽高，再重新连接。
4. 勾选「自动配置并启动本机 Deskflow」，选择手机在电脑的左、右、上、下哪一侧。
   自动查找常见安装位置；未找到时指定 **deskflow-core / deskflow-server 可执行程序**，
   不是 Deskflow 的 GUI 程序。macOS 通常位于 `/Applications/Deskflow.app/Contents/MacOS/`。
5. 点击「连接」。USB HID 初始化且 Deskflow 接受屏幕信息后，状态才变为「已连接」。
6. 关闭窗口会隐藏到系统托盘，不中断连接。点击托盘图标或菜单「显示窗口」恢复窗口，
   也可以通过托盘菜单「断开」停止连接。
7. 将鼠标移回电脑后，选择托盘菜单「退出」，GUI 会先释放桥接键鼠，再停止自己启动的 server。
   如果桌面不支持系统托盘，关闭窗口仍会停止连接并退出。

只有一个设备时自动选中；多设备时手动选择。GUI 查询 ADB 设备路径，再将序列号交给
桥接核心与实际 USB 描述符精确匹配，不回退到任意设备。若厂商的 ADB 与 USB 标识不同，
根据桥接日志列出的候选 USB 序列号手动修正。未开启 USB 调试时可选择手动模式填写序列号和宽高。
Windows 原生 ADB USB 后端可能返回 `unknown` 路径，此时继续读取尺寸，并由桥接核心核对 USB 标识。

### 插入 USB 自动连接（无需 USB 调试）

在 GUI 勾选「USB 插入时自动连接已记住的手机」（默认关闭），再手动连接手机一次。
只有状态变为「已连接」后，程序才记住该 USB 序列号及其配置；旧版本仅保存过表单的手机
也需要重新成功连接一次。可不开启 USB 调试，直接填写 USB 序列号、原始屏幕宽高。
不知道序列号时，可运行 `deskflow-otg --usb-scan` 查看可读设备的 `serial` 字段。

- 程序优先使用 libusb 热插拔事件通知（运行时检测后端能力，Linux/macOS 通常支持），
  不支持的后端（如常见 Windows 后端）、监听启动失败或异常退出时，自动回退到每 2 秒轮询。
  当前检测方式会显示在日志中；事件模式空闲时不会周期性枚举 USB。
- 启动时会扫描已插入的设备；热插拔事件到达后合并短时间内的通知，并做有限次短暂重试，
  等待 USB 驱动/权限就绪。识别手机后恢复其屏幕尺寸、灵敏度、指针模式、Deskflow 地址/端口、
  屏幕名、位置及自动启动设置，然后自动连接。全过程不会调用 ADB。
- GUI 需保持运行，可以隐藏到托盘；启动 GUI 时已插着的已记住手机也会自动连接。
  这不是开机自启动功能，完全退出程序后不再检测。
- 手动断开或连接失败后，同一次 USB 插入不再自动重试；可手动点击「连接」，或拔插 USB。
  多台已记住的手机同时插入（或 USB 序列号重复）时不会自动选择，请手动连接或只保留一台。
- 配置按手机分别保存。无 ADB 时无法自动更新分辨率或方向；转屏后请断开，手动调整尺寸再连接。
- USB 权限、兼容的 Windows 驱动和可读的 USB 序列号仍是前提；序列号随 USB 模式改变时，
  需要用新序列号手动连接一次。`--usb-scan` 中序列号为空表示无法读取或设备没有序列号，
  不会尝试连接这些设备。轮询回退模式下，快速拔插若 USB 地址未改变且未被观察到，
  可能需要再拔插并稍等；事件模式会用拔出通知重置本次插入的连接记录。

### Deskflow 启动与版本兼容

- 根据 `--help` 探测实际启动接口，不通过版本号白名单限制协议兼容性。
- 支持新版 `deskflow-core server --settings`（含 1.26 接口），以及旧版
  `deskflow-server --no-daemon --name --config --address`；根据实际参数设置明文连接。
- 自动生成独立的临时设置与屏幕布局，不覆盖用户已有 Deskflow 配置。自动启动的 server
  只监听 `127.0.0.1`，关闭 TLS；端口被占用时明确报错。
- 取消「自动配置并启动本机 Deskflow」即可填写服务地址，连接已有 server。这时需要自己
  配置同名手机屏幕、位置并关闭 TLS；GUI 不会停止外部服务。
- 新版本如果保持协议、启动参数及设置格式兼容即可继续使用；未知启动接口会提示使用已有服务模式。
- 设备、尺寸、路径和连接参数自动保存。首次自动启动可能需要授予 Deskflow 键鼠捕获权限；
  macOS 需辅助功能权限，Wayland 可能需要桌面门户确认。Windows USB 驱动仍须同时满足 ADB
  与 libusb 的访问要求，不能仅以 ADB 可见判断 AOA 可用。

### 从源码构建 GUI

安装 Qt 6.8+，`CMAKE_PREFIX_PATH` 指向 Qt 安装前缀。保留 `BUILD_GUI=OFF`（默认值）可以
继续只构建纯 C 命令行程序。

```bash
cmake -S . -B build-gui -DBUILD_GUI=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=/path正式发布校验 Git 标签与版本一致。/to/Qt/6.8.3/gcc_64
cmake --build build-gui --config Release --parallel
ctest --test-dir build-gui -C Release --output-on-failure
```

Linux/Windows GUI 与桥接程序位于同一输出目录；macOS 构建产物是 `.app`，桥接复制到
包内 `Contents/MacOS`。Windows 仍需本 README 中的 vcpkg/libusb 工具链参数；GUI 使用
动态 Qt 和动态 MSVC 运行库，桥接保持静态 libusb 构建。

打包时下载固定版本并校验 SHA-256，保留 ADB 的许可证与必要 DLL：

```bash
python scripts/fetch_adb.py --output adb-bundle
cmake -S . -B build-gui -DBUILD_GUI=ON \
    -DADB_BUNDLE_DIR="$PWD/adb-bundle/platform-tools"
cmake --build build-gui --config Release --parallel
cmake --install build-gui --config Release --prefix "$PWD/stage"
python scripts/fetch_licenses.py --output stage/share/deskflow-otg/licenses
```

发布的目录包可解压运行；Windows 入口在 `stage/bin`，macOS 入口为 `stage/*.app`。
安装前缀需为绝对路径（Qt 生成 `qt.conf` 的要求）；PowerShell 可使用 `--prefix "$pwd/stage"`。
Linux 还需系统 libusb、桌面图形库，Wayland 桌面可通过 XWayland 运行 GUI。macOS 桌面包
部署桥接依赖的 libusb。第三方说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。

测试覆盖设备输出解析、分辨率覆盖与方向、布局生成、端口冲突、子进程连接/拒绝/取消/退出、
外部服务保留，以及完整窗口的模拟 ADB 识别。模拟测试不替代三平台手机 USB 真机验证。

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

### 发布预编译包

#### 统一版本管理

CLI 和 GUI 共用仓库根目录的 `VERSION`（当前 `0.1.4`），不单独维护版本。
`deskflow-otg --version`、`deskflow-otg-gui --version` 和 GUI 右上角「关于」显示版本及 Git 提交号。
开发构建使用 `0.1.4+git.<提交号>`，已跟踪文件有未提交修改时追加 `.dirty`；无 Git 信息时用 `unknown`。
版本信息在 CMake 配置时生成，切换提交或修改源码后应重新运行 `cmake -S . -B ...`。

发布时先修改并提交 `VERSION`，再创建相同版本的 `vX.Y.Z` 标签。CI 通过
`-DOTG_RELEASE_TAG=vX.Y.Z` 校验标签与版本文件一致且已跟踪文件干净；正式版使用纯 `X.Y.Z`。
本地正式构建也需传此参数；开发构建留空。安装目录的 `share/deskflow-otg/BUILD_VERSION.txt`
记录实际构建版本，DEB 和 Windows 打包脚本默认读取它，`--version` 仅用于一致性检查
（旧安装目录没有此文件时允许手动指定）。因此不会把新包版本贴到旧二进制上。

推送以 `v` 开头的版本标签后，GitHub Actions 会在四个平台构建、测试、打包，全部成功后
自动创建对应的 [Release](https://github.com/gh6497/deskflow-otg/releases)，并将 Linux x86_64、
macOS arm64 / x86_64 和 Windows x86_64 的包放在 **Assets** 中。例如：

```bash
git tag v0.1.0
git push origin v0.1.0
```

普通分支提交和 PR 构建 CLI、GUI、测试并检查桌面包部署，不会创建 Release。
标签发布同时提供 `deskflow-otg-*` 命令行包和 `deskflow-otg-gui-*` 桌面包。
Linux CLI 包运行时需要安装 libusb-1.0 运行库；macOS CLI 压缩包运行时需要先
`brew install libusb`。Windows 包使用静态链接的 libusb。

Windows GUI 发布版另提供 `deskflow-otg-gui-windows-x86_64-setup.exe` 安装包。
运行后按当前用户安装到 `%LOCALAPPDATA%\Programs\Deskflow OTG`，可通过开始菜单启动或在
「已安装的应用」卸载，无需管理员权限；同时保留原有的免安装 ZIP。安装包含桥接程序、
Qt、ADB 和依赖许可证，不包含 Deskflow。Windows USB 驱动仍需自行配置（见下文）。
安装包尚未签名，Windows 可能提示未知发布者；请核对下载来源。升级时安装程序会先卸载
旧版本。若 GUI 正在运行，请先退出再安装或卸载。本地制作安装包需在 Windows 上完成
上文的 GUI 构建、`cmake --install` 和许可证收集，并安装 NSIS（`makensis` 在 PATH 中）：

```powershell
python scripts/package_windows.py --stage stage --output dist/deskflow-otg-gui-windows-x86_64-setup.exe
```

macOS GUI 发布版另提供 `deskflow-otg-gui-macos-arm64.dmg` 和
`deskflow-otg-gui-macos-x86_64.dmg`。选择与 Mac 架构匹配的镜像，打开后将
`deskflow-otg-gui.app` 拖入「Applications」即可安装；App 内含桥接程序、Qt、libusb、
ADB 和依赖许可证，但不包含 Deskflow。无需为 GUI 安装 Homebrew libusb（单独的 CLI
压缩包仍需要）。当前镜像未签名、公证，首次打开可能被 macOS Gatekeeper 拦截；确认来源后
可在「系统设置 → 隐私与安全性」中选择「仍要打开」。本地制作镜像：

```bash
python3 scripts/package_dmg.py --stage stage \
    --output dist/deskflow-otg-gui-macos-arm64.dmg
```

先按上文以 `BUILD_GUI=ON` 构建、安装到 `stage`，下载 ADB 并收集许可证；输出文件名的
架构应与构建机器一致。镜像仅提供拖拽安装，不会修改系统设置或自动安装 Deskflow。

Linux GUI 发布版还提供 `.deb`（amd64），包含 GUI、GUI 必需的桥接程序及发布目录中的 Qt/ADB，
不单独打包或安装 CLI 命令。安装后可从应用菜单启动，或运行 `deskflow-otg-gui`；仍需自行安装
Deskflow，USB 访问仍需 root 或 udev 规则。下载 Release 中的 deb 后执行：

```bash
sudo apt install ./deskflow-otg-gui-<版本>-linux-amd64.deb
```

也可以从本地 GUI 安装目录制作 deb（需先按上文下载 ADB、以 `BUILD_GUI=ON`
构建 GUI 并收集许可证；`stage` 必须是 GUI 安装目录）：

```bash
cmake --install build-gui --prefix "$PWD/stage"
python3 scripts/package_deb.py --stage stage \
    --output dist/deskflow-otg-gui-0.1.0-$(dpkg --print-architecture).deb
```

deb 安装到 `/opt/deskflow-otg` 并在 `/usr/bin` 提供 GUI 入口；不能与手动安装在相同路径
的版本混用。GUI 打包需要有图形桌面运行时库；包内不包含 Deskflow。

Linux GUI 发布版同时提供无需安装的 `deskflow-otg-gui-linux-x86_64.AppImage`，
内含桥接程序、Qt、libusb 和 ADB（不含 Deskflow）。下载后执行：

```bash
chmod +x deskflow-otg-gui-linux-x86_64.AppImage
./deskflow-otg-gui-linux-x86_64.AppImage
```

系统需要图形桌面和 FUSE；缺少 FUSE 时可加 `--appimage-extract-and-run`。
USB 权限规则与 deb 相同。要从本地 GUI 安装目录生成 AppImage，先下载并校验
`appimagetool-x86_64.AppImage` 与 `runtime-x86_64`（CI 中固定了 SHA-256），然后运行：

```bash
python3 scripts/package_appimage.py --stage stage \
    --appimagetool appimagetool-x86_64.AppImage \
    --runtime runtime-x86_64 \
    --output dist/deskflow-otg-gui-linux-x86_64.AppImage
```

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

### 3. Linux BLE 操控手机

开发交接、已验证范围及下一轮真机检查顺序见 [Linux BLE 交接文档](docs/LINUX_BLE_HANDOVER.md)。

Linux 可将电脑作为 BLE HID peripheral 广播，手机作为 HID Host 连接。该模式不依赖 USB，
但需要手机完成配对并订阅键盘、指针两个 HID input characteristic；仅蓝牙连接或 Deskflow
握手成功都不会被当作输入通道就绪。程序运行期间会临时将适配器名称设置为「Deskflow OTG」，
退出时恢复原名称；不会自动配对、信任或删除配对。只有显式使用 `--ble-isolate` 才会临时
修改适配器模式，普通 `--transport ble` 不切换模式。

```bash
./build/deskflow-otg --transport ble --adapter hci0 --ble-peer AA:BB:CC:DD:EE:FF \
    --host 127.0.0.1 --port 24800 --name android \
    --width 1080 --height 2340
```

手机蓝牙设置中连接广播的「Deskflow OTG」，按系统提示确认配对后再将鼠标移入手机屏幕。
必须用 `--ble-peer` 指定目标手机在 BlueZ 中的身份地址（可查 `bluetoothctl devices`，
不是轮换的广播地址）。仅目标已配对、且适配器上只有这一个连接时才允许输入；
键鼠 Value 信号仅发送给 BlueZ，不在系统 D-Bus 广播。多个连接时失败关闭输入，不猜测目标。
这不是任意多客户端环境下的隔离保证，建议专用适配器；经典 HIDP 双模兼容及完整真机回归
仍需验证。指针纯移动报告默认约 30Hz。配合下述隔离启动器取得实际连接间隔后，
发送周期不短于该间隔、最高约 60Hz；按键和滚轮报告不做此节流。
断线时不保证已经断开的链路能够送达松键报告。
BLE 事件泵在持续 Deskflow 输入期间也会运行，空闲等待最长 5ms，不依赖原有 500ms
USB 空闲检查发送最后一个坐标。拖拽中仅合并坐标；按钮变化和退出悬停立即发送。

#### 原生仅 BLE 启动与自动恢复（无需 Python）

手机把双模电脑当作音频设备时，可以显式使用原生 `--ble-isolate`。需要 sudo、BlueZ 和
util-linux 的 `/usr/sbin/runuser`；不依赖 Python、独立验证目录或任何测试脚本。
启动时会临时断开该适配器的全部蓝牙连接，保存状态、禁用 BR/EDR。桥接子进程通过
`runuser` 降权，仍以发起 sudo 的普通用户运行；root 父进程只管理本次适配器隔离与恢复。
先停止旧桥接并等待恢复完成；不要同时运行多个隔离入口。

```bash
sudo ./build/deskflow-otg --transport ble --ble-isolate --adapter hci0 --ble-peer AA:BB:CC:DD:EE:FF \
  --host 127.0.0.1 --port 24800 --name android --width 1080 --height 2340
```

root 启动器通过专用 IPC 收到 HID 就绪通知后，只对该适配器上
唯一、已加密且本机为 peripheral 的已观测 LE 连接请求 7.5～15ms、latency=0 的参数。
不接受子进程指定的连接句柄或任意 HCI 命令；多连接时不猜测目标。每次重新连接重新判断。
HCI 监控不接受会话传来的任意句柄；输入侧另由 `--ble-peer` 绑定手机身份。

只有日志 `BLE actual interval=... (controller event)` 才代表观测到的实际参数；
`requesting interval` 只表示发出了请求。手机拒绝或 5 秒未确认时保留连接及已观测的参数，
不循环重试、不改永久蓝牙配置、不安装服务、不赋予桥接程序 HCI 权限。
不带 `--ble-isolate` 直接运行桥接时仍可连接，但不会自动请求低延迟参数。

**日常重连不要取消配对，也不要关闭程序后再重新启动隔离。** 手机临时断开时，程序保留
GATT 服务、广播及已有配对；关闭到 Deskflow 的 TCP 连接让电脑收回光标，丢弃旧按键和
坐标，等待原手机重新连接。BlueZ 为已配对设备保留 CCC 订阅时，不要求它再次触发
`StartNotify`。重连并确认实际链路与订阅就绪后，程序自动重新接入 Deskflow。
首次配对仍由系统代理确认，程序不自动接受陌生设备。GUI 的隔离选项须先确认影响范围，
再通过系统 pkexec 认证；不保存管理员密码。USB 自动连接与托盘行为不变。

GATT 服务固定使用 `0x0100..0x0111`，包含键盘 LED 输出和 HID Control Point，并支持
Report Map 的 offset 读取，避免每次重新注册时句柄漂移。该范围冲突会报错，不静默改成
新句柄。旧实验版的不同 GATT 布局、手机缓存或两端已有密钥不一致仍需单独诊断，不能把
这些代码修复当作已经完成真机免重配验收。

完全退出时，原生启动器核对恢复原 BR/EDR、LE、电源、安全、可发现/可配对及名称设置。
恢复成功后才能清理快照。异常退出或恢复失败会保留 root 专用的
`/run/deskflow-otg-ble-hci0.state`，再次启动拒绝覆盖原快照；用同一个二进制恢复：

```bash
sudo ./build/deskflow-otg --ble-restore --adapter hci0
```

快照绑定适配器地址；控制器重试上电后仍失败时明确报错，不伪称恢复成功，不重启
bluetoothd、不删除蓝牙数据。SIGKILL/断电无法自动清理，`/run` 快照不跨重启保存。
此命令不能读取旧 Python 实验脚本的 JSON 快照，两者不要同时运行。

#### 常驻 GATT 与可重启桥接会话（推荐）

Android 可能缓存 HID 描述符并在重连时不重写 CCC；BlueZ 则在应用注销时删除该服务的 CCC。
因此固定句柄并不足以保证 CLI 退出后免重配。推荐使用以下服务/会话分离路径：

1. 先正常结束旧 BLE 桥接，确认恢复成功、无 `.state` 遗留；发生 MGMT/HCI 超时先停止测试。
2. 在普通用户终端运行常驻服务（**不加 sudo**），保持该终端运行：

   ```bash
   ./build/deskflow-otg --ble-service --adapter hci0
   ```

3. 另开终端启动会话，仍由原生 root supervisor 隔离并在会话退出时恢复模式：

   ```bash
    sudo ./build/deskflow-otg --transport ble --ble-session --ble-isolate --adapter hci0 \
      --ble-peer AA:BB:CC:DD:EE:FF --host 127.0.0.1 --port 24800 --name android --width 1080 --height 2340
   ```

   系统已为 BLE 配置妥当时，去掉 `sudo` 与 `--ble-isolate` 即可，不切换模式。

会话经 `/run/user/UID/deskflow-otg/ble-hci0.sock` 的私有 Unix seqpacket 连接服务；目录 0700、
socket 0600，核对双方 UID、协议版本与包长度，每个适配器/用户仅允许一个活动会话。
CLI 的 `--ble-service` 以前台普通用户运行。首次会话注册 GATT，
以后会话复用它；必须使用相同 HID 描述符，目前只支持 absolute 指针，尺寸可按会话修改。

- 断开会话或桥接崩溃/EOF 时松键、结束指针范围、丢弃待发送坐标、停广告并恢复名称；
  5 秒无会话 IPC 也会释放输入。root supervisor 会先检查服务清理完成，再尝试恢复原模式；
  无法确认时不继续切换模式、不伪称恢复成功，保留快照供停止会话后显式恢复。
- 会话退出**不注销 GATT**。服务保留真实 StartNotify 状态，仍须原手机在线、已配对、
  两路已订阅且未挂起才就绪；不把“手机已连接”伪装成订阅成功。
- 停止常驻服务会注销 GATT，丢失这次 CCC 连续性；服务崩溃、BlueZ/主机重启后的恢复仍未解决。
  旧实验版已经损坏/丢失的 CCC 或 Android 缓存不会凭空补回；若仍无订阅，应抓包诊断，
  不要删除配对来冒充此方案通过验收。
- 本机 AX210/BlueZ 5.72 + Redmi K20 Pro（Android 14）已验证：原生隔离、桥接正常重启、
  两次手机断线重连、桥接子进程 SIGKILL 自动恢复及再次接入，期间无需重新配对；
  实际连接间隔由控制器确认降至 12.5～15ms。首次旧缓存迁移仍曾需要切手机蓝牙开关一次。
  这是生命周期的实验性修正，不是 AX210 HCI Reset 超时的修复，不代表完整输入、长时间、
  常驻服务/BlueZ/主机重启或其他设备已验收；不替代原有恢复快照机制。

#### GUI 与安装包

1. Linux BLE 构建的 GUI 在「连接方式」选择「低功耗蓝牙」，填写 `hci0` 等适配器名称、
   手机身份地址及屏幕尺寸。无需 ADB；USB 自动连接在蓝牙模式暂停。
2. 点击连接，GUI 检查同用户服务：空闲则复用，不存在则后台启动同一程序的 `--ble-service`。
   其他会话占用或旧版协议不兼容会报错，不会终止旧进程。首次配对/连接通过系统蓝牙设置完成。
3. 手机被当作音频设备时，可显式选中「临时使用仅 BLE 模式」。确认该适配器的所有连接将
   断开后进行管理员认证；需要系统提供 `/usr/bin/pkexec` 和桌面认证代理、有线键鼠或专用适配器。
4. 手机掉线后 GUI 显示等待重连，不退出会话、不删除配对。手动断开结束会话，等待监督器
   恢复模式。GUI 不强杀拥有恢复责任的特权进程，失败原因见日志。
5. 退出 GUI 也保留普通用户常驻服务（不继续广告或输入）。要更换目标手机、更新服务二进制
   或彻底停止时，先断开会话，再点击「停止空闲蓝牙服务」并确认会丢失订阅连续性。
   CLI 等价操作：

   ```bash
   deskflow-otg --ble-status --adapter hci0
   deskflow-otg --ble-stop-service --adapter hci0
   ```

服务在生命周期内固定绑定目标，不能用其他会话静默换手机。配置保存在 GUI 的 QSettings；
自动后台服务日志在 Qt AppLocalDataLocation 的 `ble-hci0.log`（通常为
`~/.local/share/deskflow-otg/deskflow-otg-gui/`，以实际应用标识为准）。首次从旧实验服务迁移
协议时，必须先显式结束旧会话/服务；GUI 不会擅自替换正在运行的服务。

DEB 自动携带 CLI 快捷入口及可选的 `deskflow-otg-ble@.service` 用户单元，按构建元数据为
BLE 包声明 BlueZ、GLib 和 pkexec 依赖；安装包**不自动启动/启用**服务、不改系统蓝牙配置。
希望由用户 systemd 托管时，在没有现存常驻服务的情况下显式运行
`systemctl --user start deskflow-otg-ble@hci0`，GUI 会复用该服务；不需要 root 常驻守护程序。
AppImage/解压分发仍依赖主机 BlueZ、GLib 和桌面认证代理；FUSE 挂载可能不允许 root 执行，
需要仅 BLE 隔离时优先安装 DEB 或解压到固定目录后运行，不以挂载中的 AppImage 承诺提权兼容。

本次 CLI/GUI 接入与打包回归不等同于新增硬件验收。服务/BlueZ/系统重启后的订阅恢复、
完整持键异常释放、锁屏和长时间稳定性仍需真机确认。

### 4. 操控手机

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
      --transport MODE usb（默认）或 ble（Linux）
      --adapter NAME   蓝牙适配器（默认 hci0）
      --ble-isolate    sudo 原生仅 BLE 启动，退出恢复，无需 Python
      --ble-restore    根据原生快照恢复适配器，不启动桥接
      --ble-peer MAC   目标手机 BlueZ 身份地址（BLE 桥接必填）
      --ble-service    普通用户前台常驻 GATT 服务
      --ble-session    通过常驻服务运行桥接会话（仅 absolute）
      --ble-status     查询服务 JSON 状态，不占用会话
      --ble-stop-service 停止空闲服务，会失去订阅连续性
      --width W         虚拟屏幕宽度，1..32767 (默认 1080)
      --height H        虚拟屏幕高度，1..32767 (默认 1920)
      --mouse-mode MODE absolute（默认）或 relative（兼容模式）
      --gui             GUI 子进程模式：stdout 输出 JSON 状态，stdin 字节/EOF 请求退出
      --usb-scan        输出 USB 设备 JSON 快照，不调用 ADB、不注册 HID
      --usb-watch       输出 USB 热插拔 JSON 事件；stdin 字节/EOF 停止，不支持时退出码为 3
  -v, --version         软件版本和 Git 提交号
  -h, --help            帮助
```

### 鼠标灵敏度偏低、上下移动费手腕

**GUI 操作**：确认「原始屏幕尺寸」后，将「灵敏度倍率」设为 `1.5×` 或 `2×`。
虚拟宽高按 **原始宽高 ÷ 倍率** 四舍五入计算（最小为 1），始终从原始尺寸计算，
反复调节不会累计缩放；`1×` 恢复原始尺寸。手动修改原始宽高、交换宽高或重新通过 ADB
识别尺寸时，会重新应用当前倍率。原始尺寸与倍率分别保存，修改后重新连接生效。
倍率仅用于绝对坐标模式；相对模式使用原始尺寸，并保留倍率设置供切回绝对模式使用。

如果指针能够到达全屏，但从顶部移到底部需要很大的手腕移动范围，不一定是卡顿或
延迟，也可能是虚拟屏幕尺寸较大导致灵敏度偏低。默认绝对模式会把虚拟坐标映射到
手机全屏，`--width/--height` 不必与手机实际分辨率一致：**等比例缩小虚拟尺寸可以
提高移动灵敏度，仍然覆盖手机全屏**。

例如，手机分辨率为 `1080 × 2340` 时，可先尝试 `720 × 1560`，移动灵敏度约为原来的
1.5 倍；如果仍觉得费手腕，可尝试 `540 × 1170`，约为原来的 2 倍：

```bash
sudo ./build/deskflow-otg --host 127.0.0.1 --port 24800 --name android \
    --width 720 --height 1560 -s <手机USB序列号>
```

宽高应等比例调整，避免横纵方向的移动比例失衡。修改参数前，先把鼠标移回电脑，
再按 Ctrl+C 停止旧进程并重新启动。这种调整针对默认 `--mouse-mode absolute`，不需要
切换到受 Android 鼠标加速影响的相对模式。

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
  usb_scan.c          USB 设备快照与 libusb 热插拔事件监听
  ble_hid.c           Linux GATT/HID、稳定句柄与已配对设备重连
  ble_supervisor.c    原生隔离、降权子进程、模式快照与异常恢复
  ble_link.c          root 父进程的 HCI 参数监控与低延迟请求
  bridge.c            键鼠状态机 + HID 报告生成 (对应 scrcpy 的 hid_*)
  keymap.c            deskflow KeyID → USB HID usage 映射
gui/
  main_window.cpp     Qt Widgets 界面、ADB 自动识别和设置保存
  session.cpp         Deskflow 配置/启动、桥接状态及进程生命周期
  device_info.cpp     设备/尺寸/方向解析、屏幕布局生成
  process_utils.cpp   异步命令执行、超时、可执行文件发现
  usb_auto_connect.cpp USB 热插拔通知、轮询回退与已记住手机的自动连接策略
scripts/
  fetch_adb.py        固定版本 ADB 下载和 SHA-256 校验
  fetch_licenses.py   发布包依赖许可证收集
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
