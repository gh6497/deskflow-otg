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

```bash
# 依赖：gcc/clang (C11)、cmake、libusb-1.0（开发包）
sudo apt install build-essential cmake libusb-1.0-0-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物 build/deskflow-otg

# 运行（USB 需 root 或 udev 规则）
sudo ./build/deskflow-otg --host 127.0.0.1 --port 24800 --name android \
    --width 1080 --height 1920 -s <手机序列号>
```

CMakeLists.txt 用 `find_path`/`find_library` 直接找 libusb，**不依赖 pkg-config**。
只有 `src/*.c` 是源码，没有第三方 vendored 代码。

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

键盘 id=1，鼠标 id=2（`aoa_hid.h`）。HID 报告描述符在 `bridge.c` 里是逐字节从 scrcpy 复制的。

### 5. 键盘模型

- 修饰键（0xE0–0xE7）进报告的 **modifier 字节**，不进按键数组。
- 非修饰键（0x00–0x65）进 `keys[0x66]` 数组，报告格式 `[mod][reserved=0][6 个 key]`。
- 修饰键状态 `hid_mods` 从 `CINN` 的 mask 初始化，之后靠显式的修饰键 key 事件更新。
- `keymap.c` 按 **US 布局**把 deskflow KeyID（ASCII 或 0xEFxx）映射到 HID usage。
- key repeat（`DKRP`）被忽略（HID 键盘会自动重复）。

### 6. 鼠标模型

AOA 鼠标是**相对模式**，而 deskflow 的光标是**绝对坐标**并被 server 钳制在手机屏幕矩形内。
两者要 1:1 对应，必须满足两件事，`bridge.c` 里都做了：

1. **进入时必须把设备指针"warp"到 enter 坐标**（`mouse_warp()`）。
   真正的 deskflow 客户端在 `Client::enter()` 里会调 `Screen::mouseMove()`（X11 下就是
   `XWarpPointer`），把本机光标放到 enter 坐标再开始算增量。AOA HID 鼠标只有相对模式，
   没有绝对寻址，所以用两步模拟：先朝**最小边**走一整个屏幕宽/高（必然被钳到 0，位置就确定了），
   再走精确的剩余位移到目标。**warp 期间按键强制松开**，避免进手机时把一次点击/拖拽抹在屏幕上。
   不做这一步：设备指针停在上次离开的位置，和 server 光标差一个未知常量偏移 →
   屏幕一侧永远够不到，另一侧指针先撞到 Android 边缘卡死，server 光标还在继续走。
   这就是"部分区域无法到达 + 到某个位置就到底"的成因。

2. **超过 ±127 的位移必须拆成多个报告**（`mouse_travel()`）。
   一个 HID 报告每轴最多带 127，而两个 `DMMV` 之间 server 光标可以跳几百上千像素
   （高回报率鼠标的一次快速甩动）。只发一个钳到 127 的报告会**静默丢掉剩下的位移**，
   设备指针越落越远，最后卡在边缘。

- 滚轮 delta 以 120/格 为单位（`WHEEL_DELTA`），除以 120 后累加、凑整再发。
- 按钮映射（deskflow ButtonID → HID bit）：1=左(bit0) 2=中(bit2) 3=右(bit1) 4=bit3 5=bit4。
- `active` 标志（enter→leave 之间为真）用来丢弃窗口外的 move/wheel/button 事件。
- `--width/--height` **必须等于手机真实分辨率**（也要和 server 布局里该屏幕的
  `halfwidths/halfheights` 一致）：server 把光标钳到 `--width/--height` 矩形，
  Android 把设备指针钳到真实显示矩形，两者相等时增量才不会被截断。

## 如何验证（无需真机/真 server）

仓库没有测试目录，本地验证用临时文件（在 `/tmp/opencode/`，未入库）：

1. `stub libusb`：`/tmp/opencode/libusbstub/`（`libusb-1.0/libusb.h` + `stub.c`），
   返回一个假设备并**记录每次 `SEND_HID_EVENT` 的字节**。
2. `testsim.c` + `run.sh`：一个内含 mock deskflow server（fork + 127.0.0.1 socket）的
   端到端仿真。它按脚本发 `CINN`（5 种进入位置/5 种上次停留位置）、连续 `DMMV` 扫过
   四个角、`COUT`，最后再做一次 20px 步进的连续拖拽；把录到的 HID 报告回放进一个
   "模拟 Android 相对指针"（按屏幕矩形钳位），逐步断言**设备指针坐标 == server 光标坐标**。

```bash
/tmp/opencode/run.sh
# 期望: "136 samples, 0 failures" + "REACHABILITY: ok" + "DRAG: ok" + "PASS"
# 回归对照：把 HEAD 的 bridge.c/h 拿来跑同一个 testsim，会看到
#   "131 failures" + "DRAG: FAIL (pointer is up to 832px away ...)"
```

warp 的开销是每次 enter 18~33 个 HID 报告（几十毫秒的 USB 控制传输），只在切屏时发生一次。

所有源文件应保持 `-Wall -Wextra -Werror` 干净：

```bash
for f in src/*.c; do gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only -Isrc "$f"; done
```

## 约束与已知限制

- **只支持明文协议**，不支持 deskflow 的 TLS（deskflow 1.26 默认开 TLS，需在 server 侧关闭）。
- 键盘 **US 布局**（其它布局符号键可能错位）。
- 只支持键盘 + 鼠标，不支持游戏手柄、剪贴板、文件拖放、视频。
- 需要 root 或 udev 规则才能访问 USB。

## 当前状态 / 待办

- ✅ 协议握手 + 消息流 + HID 报告，mock 验证通过。
- ✅ 真机环境下已连上 deskflow server（`connected ... as "android"`）。
- ⏳ **待真机实测**：鼠标滑入手机屏幕后是否能真正操控（用户待验证，vid 0x0a9d 待确认是否为手机）。
- 潜在后续项：游戏手柄支持、多键盘布局、TLS 支持、udev 规则/systemd 示例。

## 代码风格约定

- 纯 C11，无 C++、无 Qt/FFmpeg/SDL 依赖。
- 日志用 `LOG_ERR/LOG_WARN/LOG_INFO/LOG_DBG` 宏（各文件自行 `#define`，输出到 stderr）。
- 每个 `.c` 顶部注明其对应的 scrcpy/deskflow 上游出处。
- License：Apache-2.0（HID/AOA 逻辑移植自 scrcpy，同为 Apache-2.0）。
