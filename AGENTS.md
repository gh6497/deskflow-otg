# AGENTS.md

本文件为在此仓库工作的 AI 编码代理（以及未来的协作者）提供上下文、约定和关键实现细节。
先读 README.md 了解项目用途，再读本文件了解"怎么改、改哪里"。

## 项目是什么

`deskflow-otg` 是一个把 **Deskflow**（键盘鼠标共享）和 **scrcpy 的 OTG 模式**（USB AOA HID）
桥接起来的独立 C 程序。

数据流：

```
主机键盘鼠标 → deskflow server → (TCP, Deskflow 协议) → deskflow-otg → (USB AOA HID) → 安卓手机
```

手机在 Deskflow 里表现为一块普通的 secondary screen，鼠标滑进去即可用主机键鼠操控手机，
**不需要 adb、不需要 USB 调试**。

## 构建与运行

- CLI/GUI 版本统一来自根目录 `VERSION`，由 `cmake/Version.cmake` 配置生成 `build_version.h`。
  开发版追加 Git 提交与 tracked dirty 状态，正式版通过 `OTG_RELEASE_TAG` 校验标签与基础版本。
  修改提交/源码后重新配置 CMake 更新版本。安装 `BUILD_VERSION.txt` 供 DEB/Windows 打包默认读取，
  手动 `--version` 必须与已安装构建一致。CLI/GUI 均支持 `--version`，GUI 有双语「关于」。

```bash
# 依赖：gcc/clang (C11)、cmake、libusb-1.0（开发包）
sudo apt install build-essential cmake libusb-1.0-0-dev
# macOS（Intel / Apple Silicon）
# xcode-select --install
# brew install cmake libusb

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物 build/deskflow-otg

# 运行（USB 需 root 或 udev 规则）
sudo ./build/deskflow-otg --host 127.0.0.1 --port 24800 --name android \
    --width 1080 --height 1920 -s <手机序列号>
```

CMakeLists.txt 用 `find_path`/`find_library` 直接找 libusb，**不依赖 pkg-config**。
支持 Linux 和 macOS；macOS 默认查找 `/opt/homebrew`、`/usr/local` 及 libusb opt 前缀，
自定义路径可用 `-DCMAKE_PREFIX_PATH=...`。macOS 通常不需要 sudo，也不使用 udev。
共享程序源码在 `src/*.c`，Linux 专用源码与头文件在 `platform/linux/`，
Windows 后端在 `platform/windows/`，回归测试在 `tests/`，没有第三方 vendored 代码。

## 源码结构（每个文件干什么）

| 文件 | 职责 | 对应上游出处 |
|---|---|---|
| `src/main.c` | 入口、参数解析、把 deskflow 回调接到 bridge | — |
| `src/deskflow_client.c/.h` | 极简 Deskflow 客户端协议（TCP + 握手 + 消息循环） | deskflow `ProtocolTypes.h` + `PacketStreamFilter` |
| `src/aoa_hid.c/.h` | libusb 枚举/打开设备、注册 HID、发送报告 | scrcpy `usb/aoa_hid.c` + `usb/usb.c` |
| `src/bridge.c/.h` | 键鼠状态机 + HID 报告生成 | scrcpy `hid/hid_keyboard.c` + `hid/hid_mouse.c` |
| `src/keymap.c/.h` | deskflow KeyID → USB HID usage 映射 | deskflow `KeyTypes.h` |

## 关键实现细节（改代码前必读）

### 1. Deskflow 协议有 4 字节长度前缀

deskflow 的 `PacketStreamFilter` 给**每条消息**都加一个 4 字节大端长度前缀：

```
[uint32 length][payload]
```

- 发送：`send_packet(fd, payload, len)`（见 `deskflow_client.c`）。
- 接收：`recv_packet(fd, &out)` 先读长度再读 payload，一次读完整包。

**payload 内部**才是 `ProtocolTypes.h` 定义的格式：
整数大端 1/2/4 字节（`%i`/`%2i`/`%4i`），字符串是 4 字节长度前缀 + 数据（`%s`）。
这是**两层**长度前缀：外层是包帧，内层是字段里的字符串长度，别搞混。

### 2. 握手流程（客户端视角）

1. server → hello 包：`协议名(7字节) + major(2) + minor(2)`，协议名是 `Barrier` 或 `Synergy`。
2. client → hello back 包：`协议名(7) + major(2) + minor(2) + 屏幕名(%s)`，回显 server 的协议名。
3. server → `QINF` → client 回 `DINF`（x,y,w,h,warp,mx,my 各 2 字节）。
4. server → `CIAK`。
5. server → `DSOP`（`%4I` = 4 字节 count + count×4 字节）。
6. 之后是事件流，client **对每条消息回 `CNOP`**，收到 `CALV` 要**原样回显 `CALV`**。

### 3. 未知消息必须忽略，不能断连

server 会发 `LSYN`（语言同步）、`SECN`（安全输入通知）、`DFTR`、`DDRG` 等，
这些对本程序无用，**忽略并回 CNOP 即可**。`handle_message` 的 default 分支已按此处理。
只有 `EICV`/`EBSY`/`EUNK`/`EBAD` 才是真错误，才 `return -1` 断连。

### 4. AOA HID 控制请求

AOAv2 HID（无 adb、无 accessory-mode 握手，直接对已打开设备发 vendor 控制请求）：

- `ACCESSORY_REGISTER_HID` = 54（wValue=HID id，wIndex=report desc 长度）
- `ACCESSORY_UNREGISTER_HID` = 55
- `ACCESSORY_SET_HID_REPORT_DESC` = 56
- `ACCESSORY_SEND_HID_EVENT` = 57

键盘 id=1，指针 id=2（`aoa_hid.h`）。键盘和相对鼠标描述符来自 scrcpy；默认绝对指针
使用 `bridge.c` 中的 `ABSOLUTE_MOUSE_REPORT_DESC`，不能按 scrcpy 的 5 字节相对报告解码。

### 5. 键盘模型

- 修饰键（0xE0–0xE7）进报告的 **modifier 字节**，不进按键数组。
- 非修饰键（0x00–0x65）进 `keys[0x66]` 数组，报告格式 `[mod][reserved=0][6 个 key]`。
- 修饰键状态 `hid_mods` 从 `CINN` 的 mask 初始化，之后靠显式的修饰键 key 事件更新。
- `keymap.c` 按 **US 布局**把 deskflow KeyID（ASCII 或 0xEFxx）映射到 HID usage。
- key repeat（`DKRP`）被忽略（HID 键盘会自动重复）。

### 6. 鼠标模型

**默认 `--mouse-mode absolute`**，通过 AOA 注册绝对坐标数位板，不是简单地把普通鼠标
的 `Input(Relative)` 改成 `Input(Absolute)`：Android 普通鼠标 mapper 只处理 REL_X/Y。

- Application 必须是 **Digitizer (0x0D/0x01)**，在 Linux 中产生 `INPUT_PROP_POINTER`。
  使用 Pen Application 会变成 `INPUT_PROP_DIRECT`，影响光标和滚轮行为。
- Stylus Physical Collection 中的 **Tip Switch + In Range** 产生 `BTN_TOUCH + BTN_TOOL_PEN`，
  让 Android `TouchInputMapper::dispatchPointerStylus()` 按绝对坐标定位并支持悬停。
  不能换成 Finger/Puck，否则又会进入相对触摸板/鼠标逻辑。
- 按钮置于嵌套的 **Pointer Physical Collection** 中，让 Linux 把 Button usages 映射为
  `BTN_LEFT/RIGHT/...`，而不是 `BTN_0/...`；按钮和坐标在同一个输入设备、同一条报告中。
- 8 字节报告：`[flags][buttons][X LE16][Y LE16][wheel i8][pan i8]`。
  flags bit0=Tip Switch，bit1=In Range。X/Y 逻辑范围 0..32767。
- Deskflow 的 `[0,width-1] / [0,height-1]` 分别映射到完整 HID 轴范围。按键、滚轮报告也
  携带当前位置，丢失一条移动报告不会让后续点击永久偏移。
- enter 只发悬停（不带按键）；leave/close 先松开按键，再结束 In Range。
  `DMRM` 累积并钳位到虚拟屏幕，下一条 `DMMV` 重新给出权威位置。

**`--mouse-mode relative` 兼容模式**保留 scrcpy 的 5 字节鼠标报告：进入时
`mouse_warp()` 先撞左上边界再移动到目标；`mouse_travel()` 将超过 ±127 的位移拆分。
这只能在“相同显示尺寸、无加速的 1:1 位移”模型中保持同步，**不能保证真机准确**。
Android `CursorInputMapper` 会做速度缩放/加速；即使起点校准、尺寸一致，依然可能出现
server 到达 y=0 而手机指针还在屏幕中间的情况。AOA 本身并不限制只能相对定位。

- 滚轮 delta 以 120/格 为单位（`WHEEL_DELTA`），除以 120 后累加、凑整再发。
- 按钮映射（deskflow ButtonID → HID bit）：1=左(bit0) 2=中(bit2) 3=右(bit1) 4=bit3 5=bit4。
- `active` 标志（enter→leave 之间为真）用来丢弃窗口外的 move/wheel/button 事件。
- `--width/--height` 范围 1..32767（协议坐标按 int16 解析）；建议与手机当前方向分辨率
  一致以获得自然移动比例。绝对模式不依赖设备实际像素数相等。

## 如何验证（无需真机/真 server）

`tests/test_socket.c` 验证协议包帧、EOF、断连写入不会触发 SIGPIPE；Darwin 使用
`SO_NOSIGPIPE`，Linux 使用 `MSG_NOSIGNAL`。执行 `ctest --test-dir build --output-on-failure`。
`.github/workflows/build.yml` 覆盖 Linux、Intel macOS 和 Apple Silicon macOS 的构建与测试。

鼠标本地验证用 `/tmp/opencode/mouse-regression/` 中的临时文件（未入库）：

1. `test_mouse.c` stub AOA 层，解析实际注册的 HID 描述符，再按字段回放报告。
   相对模式施加随位移变化的速度增益；绝对模式按 Android 数位板轴范围缩放。
2. 覆盖四角、大跳跃、连续拖拽、悬停、按钮/滚轮、离开后事件、重入、不同设备分辨率、
   丢包后点击恢复、DMRM 边界反向、极限尺寸与相对兼容模式。ASan/UBSan 检查通过。
3. 修复前版本的相同加速模型复现：server y=0，手机指针 y=1050，无法继续向上。

```bash
sh /tmp/opencode/mouse-regression/run.sh
# 期望: "PASS: 2729 pointer samples ..." + 旧版 "REPRODUCED ... y=1050"
# 修复前基线固定为 0e6a97e。
```

这些是主机仿真，不代表 Android 真机兼容性已验证。绝对模式 enter 只需一条指针报告。

所有源文件应保持 `-Wall -Wextra -Werror` 干净：

```bash
for f in src/*.c; do gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only -Isrc -Ibuild/generated "$f"; done
# Linux BLE 后端（需要 GLib/GIO 开发包）
for f in platform/linux/*.c; do gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only -Isrc $(pkg-config --cflags gio-2.0) "$f"; done
```

## 约束与已知限制

- Windows 原生 BLE 为显式开启的实验选项 `-DBUILD_BLE_WIN=ON`，不等同 Linux
  `BUILD_BLE`；同时开启 `BUILD_GUI` 时提供 Windows GUI 连接选项。`platform/windows/ble_win.cpp` 使用 C++20/WinRT；
  `platform/windows/ble_win.h` 暴露 C ABI，桥接核心保持 C11。专用线程管理 GATT/身份核对和单条
  在途通知，64 条有界队列仅合并相邻纯移动，按钮/范围/滚轮/键盘保持屏障。
  必须 `--transport ble --ble-peer MAC`、默认适配器和 absolute；不支持 Linux 服务、
  隔离选项。GUI 以 `--gui --gui-ble-control --transport ble --ble-peer MAC` 管理前台桥接，不传 Linux
  `--adapter/--ble-session`，不与 `--background` 合用；隐藏托盘保留进程，退出停止
  自己的 GATT。断开使用有界 stdin `pause`，停止 TCP、丢弃待发动作，等待在途通知与释放报告
  处理完才回 `ble-idle`；空闲保留原 provider/订阅，下次 `resume` 更新 host/port/name/尺寸。
  `platform/windows/ble_win_control.c` 校验协议/状态，目标手机与描述符固定；换目标前显式停止空闲服务。
  USB 会话可并存一个空闲 provider；GUI 退出/EOF 才停止，8 秒释放超时失败关闭并提示连续性丢失。
  不保留独立常驻服务、不触碰其他 CLI 实例。`test_ble_win_control` 测试解析，GUI 用真实
  main/bridge + fake BLE/Deskflow 验证创建次数、重复连接、更新参数、释放确认、超时及 EOF，均不访问硬件。
  身份读取在线 BluetoothLEDevice 地址及已配对状态，通知定向发送；
  失联/订阅变化清队列并断开 Deskflow，恢复先释放输入；退出释放为尽力而为。
  WinRT 等待按会话 epoch 取消并记录操作名；断线取消不算致命超时。Cancel 不等于完成，
  旧通知真正完成/取消前不得恢复 ready 或发送新通知；5 秒仍未结束则失败退出。
  当前 Windows 排障版所有通知统一30ms间隔（非实测interval），降低切屏状态突发；
  致命通知超时先关闭ready再释放/注销，日志含发送计数/队列/在线及session状态。
  nova 9 的独立原型键盘/悬停/点击/拖拽/滚轮已过，不能等同连续 Deskflow 输入、
  重连/休眠/进程重启已验收。Redmi Android14 ROM 的 LHDC 白名单超91字节导致
  配对时蓝牙崩溃，未修改手机配置。用法与限制见 `docs/WINDOWS_BLE.md`。

- Linux BLE 实验后端通过 `sudo deskflow-otg --transport ble --ble-isolate` 原生启动，
  不依赖 Python。`platform/linux/ble_supervisor.c` 使用 Linux MGMT API 快照/隔离/核对恢复，
  通过 `/usr/sbin/runuser` 降权运行自己的桥接子进程；父进程用 `platform/linux/ble_link.c` 监控 HCI。
  桥接通过继承的 Unix datagram FD（`OTG_BLE_LATENCY_FD`）传递订阅就绪
  与实测连接间隔。只对唯一、已加密、本机为 peripheral 的已观测 LE 连接请求 7.5～15ms；
  HCI Command Status 成功不等于参数已更新，必须等待 LE Connection Update Complete。
  失败/超时保留连接；重连后重新判断，不接受子进程指定 HCI 命令或句柄。
  `/run/deskflow-otg-ble-hciN.state` 保存 root 专用、绑定适配器地址的原始快照；失败不覆盖，
  `--ble-restore --adapter hciN` 恢复，只有核对成功才删除；不要与历史 Python 隔离脚本并行。
  普通 `--transport ble` 不切换模式。手机掉线不注销 GATT：BlueZ 5.72 对 bonded peer 可能
  保留 CCC 而不调用 StopNotify，因此就绪还须 Device1 Connected/Paired。断线断开 Deskflow
  TCP、清空输入，原手机回来后重新握手；不能以重新删除配对作为正常重连方案。
  GATT 固定 `0x0100..0x0111`（characteristic Handle 为 value handle），含 LED 输出、
  Control Point；长读处理 offset。`test_ble_gatt.c` 使用私有 D-Bus，不访问系统蓝牙；
  常驻会话的 REPORT/POLL 为非阻塞、单请求在途（仍兼容 IPC v2），状态查询间隔 5ms。
  最多排队 64 条报告，只替换相邻纯移动；按下位置、松键、滚轮、键盘及范围变化必须保序。
  溢出/回复超时关闭 IPC 触发释放，断线清空待发输入；退出先收完在途回复再请求 DETACH。
  服务等待 socket 可读，活动期间用最多 1ms 超时驱动 BlueZ/末条移动，不得恢复每请求固定休眠。
  指针默认 15ms pacing，收到实测连接间隔后直接跟随，不再另加 60Hz 上限。
  `test_ble_supervisor.c` mock MGMT 验证恢复，`test_ble_link.c` 验证实际 HCI 确认，
  `test_ble_pointer.c` 验证坐标/状态屏障与已配对 CCC 保留重连。历史 Python 仅作回归参考。
  构建/模拟不等于真机免重配或硬件恢复验证完成，仍需用户实测确认。
  新实验路径：普通用户前台 `--ble-service --adapter hciN` 保持 GATT 注册，
  桥接通过 `--transport ble --ble-session` 接入，可叠加 `--ble-isolate`。
  `platform/linux/ble_session.c` 使用私有 Unix seqpacket、双方 UID/版本/包长核对和独占会话；
  断开/EOF/5 秒无 IPC 释放输入、停广告、恢复名称，不注销 GATT。父进程在模式恢复前
  通过只读状态检查空闲；失败保留快照。仅支持 absolute、固定描述符，禁止伪造 CCC。
  服务终止/BlueZ 或主机重启后不保证 CCC 连续性，不能声称任意重启已免重配。
  `test_ble_session.c` 用真实 socket/fork + fake HID 测试会话；`test_ble_gatt.c` 用私有
  D-Bus 模拟 BlueZ 管理器测试停广告但保留注册、会话复用和失败清理，不访问系统蓝牙。
  当前单组真机（AX210/BlueZ 5.72、Redmi K20 Pro Android 14）已过原生隔离、会话正常重启、
  两次手机断线重连、普通桥接子进程 SIGKILL 后自动恢复及再次接入；实际 interval 12.5～15ms。
  首次缓存迁移仍需切过一次手机蓝牙；常驻服务/BlueZ/主机重启和持键强杀未验收。
  详细证据和仍运行的服务/会话见 `docs/LINUX_BLE_HANDOVER.md` 第 7 节，勿擅自停止用户连接。

- **只支持明文协议**，不支持 deskflow 的 TLS（deskflow 1.26 默认开 TLS，需在 server 侧关闭）。
- 键盘 **US 布局**（其它布局符号键可能错位）。
- 只支持键盘 + 鼠标，不支持游戏手柄、剪贴板、文件拖放、视频。
- Linux 需要 root 或 udev 规则才能访问 USB；macOS 需允许配件连接，Deskflow 需辅助功能权限。

## 当前状态 / 待办

- ✅ 协议握手 + 消息流 + HID 报告，mock 验证通过。
- ✅ 真机环境下已连上 deskflow server（`connected ... as "android"`）。
- ✅ 已复现相对模式加速漂移，并加入默认绝对数位板指针；主机仿真与构建通过。
- ⏳ **待真机实测**：绝对模式在用户手机上的悬停、全屏到达、拖拽、滚轮与切屏兼容性。
- 潜在后续项：游戏手柄支持、多键盘布局、TLS 支持、udev 规则/systemd 示例。

## 代码风格约定

- 桥接核心 `src/` 保持纯 C11，无 Qt/FFmpeg/SDL 依赖。可选 `gui/` 使用 C++17、Qt 6.8+。
- 日志用 `LOG_ERR/LOG_WARN/LOG_INFO/LOG_DBG` 宏（各文件自行 `#define`，输出到 stderr）。
- 每个 `.c` 顶部注明其对应的 scrcpy/deskflow 上游出处。
- License：Apache-2.0（HID/AOA 逻辑移植自 scrcpy，同为 Apache-2.0）。

## 可选桌面 GUI

- Linux BLE 正式接入在同一 `deskflow-otg` 中分为 `--ble-service` 常驻进程与
  `--transport ble --ble-session --ble-peer MAC` 桥接会话；BLE 桥接必须指定 BlueZ 身份地址。
  本地 IPC 协议现为 v2，服务生命周期内固定目标，不兼容旧实验常驻进程；迁移必须显式停止
  旧服务，不得擅自杀掉正在使用的连接。`--ble-status` 只读，`--ble-stop-service` 仅同 UID
  且空闲/清理成功时停止（会注销 GATT，丢失 CCC 连续性）。GATT 方法仅允许 BlueZ 调用，
  键鼠 Value 信号定向发送给 BlueZ；目标需已配对且适配器仅这一个连接，多连接关闭输入。
  GUI 蓝牙模式不扫描 ADB/USB，不使用 USB 自动连接；后台启动/复用普通用户服务，退出保留服务。
  仅 BLE 隔离默认关闭，须确认全适配器断连影响，再用 `/usr/bin/pkexec` 认证；监督器识别
  PKEXEC_UID/SUDO_UID，继承桥接 stdin，实现取消/EOF 优雅恢复；GUI 不强杀特权恢复进程。
  DEB 根据 BUILD_FEATURES.json 添加运行依赖和可选用户 systemd 单元，但安装不自动启动服务。
  GUI/新身份门控的离线通过不代表提权、缓存迁移、系统重启或完整真机输入验收完成。
  身份比较必须读取在线 Device1 的 `Address`，不能把身份 MAC 拼成固定 D-Bus 路径：
  BlueZ 解析隐私地址后可能保留原 `dev_随机地址` 路径。授权仍要求该在线对象本身 Paired；
  另一个同 Address、离线且已配对的对象不能替它提供授权。GATT device 选项核对当前已认证
  在线对象路径，多连接继续关闭输入。服务日志的 `BLE waiting:` 会区分身份、配对和订阅等待。

- `-DBUILD_GUI=ON` 启用 Qt Widgets 桌面端；默认 OFF 保留纯 C 构建。GUI 支持 Linux、macOS、Windows。
- `gui/main_window.cpp` 管理界面、ADB 异步查询和 QSettings；`device_info.cpp` 解析设备/尺寸/方向并生成布局。
- GUI 的可选 USB 自动连接默认关闭；成功 Connected 后按 USB 序列号保存独立配置。
  `usb_auto_connect.cpp` 优先启动桥接 `--usb-watch`（`src/usb_scan.c`），运行时检查 libusb
  hotplug 能力并注册回调；不支持、启动超时、监听错误/退出时回退到每 2 秒异步 `--usb-scan`。
  事件模式只在启动、热插拔、就绪/配置变化时扫描，合并通知并有限次重试权限就绪，不做周期枚举。
  不依赖 ADB、不注册 HID。按 bus/address 记录本次插入是否已尝试，失败/手动断开不重试，拔插后重置；
  拔出事件立即重置该地址并丢弃旧扫描结果，防止快速同地址重插与扫描竞态。
  不可读序列号不等于拔出，多台已知设备/重复序列号不自动选择。桥接 `usb-ready` 携带
  `usb-id`，避免独占 USB 驱动下手动断开立即重连；退出禁用检测并清理扫描子进程。
  `test_usb_auto_connect.cpp` 和 `test_window.cpp` 覆盖策略、无 ADB、托盘自动连接及配置恢复。
- GUI 提供系统托盘：关闭窗口只隐藏并保留连接，菜单可恢复窗口、断开或退出；无托盘时关闭仍退出。
  退出沿用 Session 异步停止流程，Idle 后发出 `MainWindow::exitReady`，由入口退出应用；
  `quitOnLastWindowClosed` 关闭以支持后台运行。托盘菜单与状态提示支持实时语言切换。
  `test_window.cpp` 模拟托盘可用/不可用，覆盖隐藏、恢复、断开及连接中优雅退出。
  托盘与窗口统一使用嵌入 Qt 资源的 `packaging/deskflow-otg.svg`，DEB 与 AppImage 桌面入口也使用此图。
- `gui/session.cpp` 只管理自己创建的 Deskflow 和桥接进程。自动启动生成独立配置、监听回环地址并关闭 TLS；
  外部服务模式不更改或终止已有 Deskflow。根据 `--help` 区分现代 `deskflow-core server --settings` 与传统 server 参数。
- `src/main.c --gui` 在 stdout 输出 JSON 行 `usb-ready` / `connected` / `disconnected`；日志仍走 stderr。
  stdin 任意字节或 EOF 请求优雅退出，Windows 使用 PeekNamedPipe，Unix 使用 poll。
- `connected` 由首次完整 DSOP 接受触发，而不是 TCP connect/hello 返回；取消检查覆盖消息循环和部分包读取。
- `scripts/fetch_adb.py` 下载固定 ADB 36.0.2，核验 SHA-256，只保留 adb、Windows DLL 与 notices。
  `ADB_BUNDLE_DIR` 是提取后的 platform-tools 目录；不提供时开发版使用系统 ADB。
- 发布包提供 Qt 动态库、桥接与 ADB，不包含 Deskflow。`qt_deploy_runtime_dependencies` 同时检查桥接依赖。
- Qt 测试：`tests/test_gui.cpp` + `test_peer.cpp` 测试模拟子进程生命周期；`test_window.cpp` 测试模拟 ADB 完整识别。
  `OTG_TEST_SCREENSHOT=/path/image.png ctest ...` 可保存窗口截图（测试设备为模拟数据）。
