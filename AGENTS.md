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

仓库没有测试目录，本地验证用 `/tmp/opencode/mouse-regression/` 中的临时文件（未入库）：

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
- ✅ 已复现相对模式加速漂移，并加入默认绝对数位板指针；主机仿真与构建通过。
- ⏳ **待真机实测**：绝对模式在用户手机上的悬停、全屏到达、拖拽、滚轮与切屏兼容性。
- 潜在后续项：游戏手柄支持、多键盘布局、TLS 支持、udev 规则/systemd 示例。

## 代码风格约定

- 纯 C11，无 C++、无 Qt/FFmpeg/SDL 依赖。
- 日志用 `LOG_ERR/LOG_WARN/LOG_INFO/LOG_DBG` 宏（各文件自行 `#define`，输出到 stderr）。
- 每个 `.c` 顶部注明其对应的 scrcpy/deskflow 上游出处。
- License：Apache-2.0（HID/AOA 逻辑移植自 scrcpy，同为 Apache-2.0）。
