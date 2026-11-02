# 文本剪贴板共享

剪贴板遵循 **Deskflow 客户端的所有权和切屏规则**，不采用 scrcpy 客户端
“按 Ctrl+V 才读取本机剪贴板”的规则。scrcpy 5.0.1 Android Server 只负责手机端读写：

```text
Deskflow 主端 ⇄ Deskflow 协议 ⇄ deskflow-otg ⇄ ADB / scrcpy 控制通道 ⇄ Android 剪贴板
```

因此 Deskflow server 可以在另一台电脑上，CLI 也不依赖 Qt、桌面剪贴板 API、SDL 或 FFmpeg。
键鼠仍走原来的 USB AOA / BLE；启用剪贴板才额外需要已授权的 ADB。

## 默认行为

- 每台设备独立启用，默认关闭；新增设备不继承开启状态或另一台手机的 ADB 序列号。
- 电脑复制后，Deskflow 在切入手机时提供最新内容，桥接写入手机剪贴板。
  Ctrl+V 和手机长按菜单都可以粘贴，不额外注入第二次粘贴。
- 手机处于活动屏幕时复制：立即向 Deskflow 通知所有权，通常在离开时上传正文。
- 手机不是活动屏幕时复制：立即通知所有权并上传正文；是否接受由 Deskflow 的序列和
  所有权规则决定。**后台手机复制也可能覆盖当前电脑/设备的剪贴板。**
- 手机 A → 手机 B 经 Deskflow server 中转；不在 GUI 内做设备间广播。
- 服从 server 下发的 `CLPS`、`CLSZ`。仅处理主剪贴板，不支持 X11 selection/中键剪贴板。
- 仅 UTF-8 纯文本，支持中文、多行、Emoji 和空文本；不共享图片、文件、HTML。
  每条文本最多 262130 字节，同时受 Deskflow 的大小上限限制。不在本地截断后粘贴。
- 不在首次连接时主动导出手机已有的旧剪贴板，不维护历史，不将正文写入日志或配置文件。

**隐私：**复制的密码、验证码等文本可能传给其他连接设备；只为可信设备开启。
本项目 Deskflow TCP 目前仍不支持 TLS，剪贴板同样走明文协议；跨电脑时应使用可信网络或安全隧道。

## 构建与 GUI

```bash
cmake -S . -B build-clipboard -DBUILD_GUI=ON -DBUILD_CLIPBOARD=ON
cmake --build build-clipboard --parallel
```

`BUILD_CLIPBOARD` 与 `BUILD_AUDIO` 独立。只开剪贴板不要求 ALSA/WASAPI 或音频播放设备。
Linux/Windows 可以叠加 `-DBUILD_AUDIO=ON`；macOS 只提供 POSIX 剪贴板会话，不提供音频。
跨平台构建列入 CI，但本地验证不等于 Windows/macOS 或 Android 真机验收。

GUI 在当前设备页勾选「通过 Deskflow 共享文本剪贴板（需要 ADB）」，并显示就绪、服务端禁用、
暂不可用等独立状态。程序随安装包携带匹配版本的 Android Server，不要求安装完整 scrcpy。
开发构建的 GUI 也需要按原有安装目录布局提供 server 和 `SCRCPY_VERSION.txt`。

音频与剪贴板共用「音频/剪贴板 ADB 序列号」和 ADB 可执行程序配置。USB 模式留空时使用
USB 序列号；Windows 可填写已连接的无线 ADB 地址，避免 ADB 与 libusb USB 驱动独占冲突。
BLE 模式必须显式填写**同一台手机**的 ADB 序列号，桥接不能仅凭 BLE MAC 自动推导 ADB 身份。
此时仍不进行 ADB/USB 设备扫描。

显式填写无线 `主机:端口` 时，会在推送 server 前检查目标是否在线，必要时自动执行
`adb connect` 并确认 `get-state` 为 `device`；Linux、Windows、macOS 行为一致。
需先完成手机要求的无线调试配对，填写连接端口而非配对端口。不会自动配对或改用其他设备，
会话退出不主动断开共享的无线 ADB 连接；USB 序列号保持原有流程。

Windows GUI 的保留 BLE provider 固定附加功能参数。修改剪贴板开关、ADB 目标/路径或 server
后，需先显式停止空闲蓝牙服务再连接；不会自动注销 GATT。停止服务可能影响订阅缓存连续性。

## CLI

```bash
./build-clipboard/deskflow-otg \
  --host 127.0.0.1 --port 24800 --name android \
  --serial PHONE_USB_SERIAL --width 1080 --height 1920 \
  --clipboard --adb /usr/bin/adb \
  --adb-serial 192.168.1.20:5555 \
  --scrcpy-server third_party/scrcpy/scrcpy-server-v5.0.1 \
  --scrcpy-version 5.0.1
```

可增加 `--audio`，仍只创建一个 Android Server 和一个 ADB forward。音频和控制使用同一
forward 的两条独立 socket；首 socket 的 dummy byte 在 server 接受第二条之前发出，
不能等待音频头才连接控制 socket，否则会死锁。不启用任何 ADB 功能时不启动 ADB/server。

BLE CLI 同样可加 `--clipboard --adb-serial ...`；Windows 仍使用默认适配器与 absolute 指针，
不改变既有单目标身份和队列限制。音频暂仍仅支持 USB 桥接会话。

## 时序与失败隔离

- 写入手机使用 `SET_CLIPBOARD(paste=false)`；其 ACK 只表示请求已处理，不保证 ROM 或应用一定接受。
- 等待写入 ACK 期间，最多保序暂存 64 条键盘事件，避免 Ctrl+V 抢在剪贴板之前到达。
  鼠标与 Deskflow keep-alive 不等待 ACK；持续输入或半包读取期间也驱动控制通道。
- 离开屏幕时取消尚未投递的键盘队列。ACK 超时（2 秒）、队列溢出、控制 socket EOF 等失败
  会释放键盘修饰键、取消队列并关闭本次剪贴板共享，不重放到下次切屏，也不改变鼠标拖拽状态。
- 剪贴板启动失败或运行失败保留键鼠，GUI 提示重新连接以重试；不会停止其他设备的会话。
  纯音频模式仍保留原来的启动失败处理。
- 同值回显去重；Deskflow 所有权变化撤销未发送的旧正文；断开清理本会话的控制缓冲、socket、
  server 子进程和 forward，不操作其他 ADB server 或已有蓝牙服务。

## 离线验证

```bash
ctest --test-dir build-clipboard --output-on-failure
```

`clipboard_protocol` 检查 UTF-8、Deskflow 分块/所有权/服务端选项和 scrcpy ACK/回显/EOF；
`clipboard_bridge` 用真实 main/bridge 回调和 fake AOA 检查粘贴屏障、松键、切出取消和失败隔离；
`clipboard_auxiliary` 用 fake ADB/server 检查独立及组合流、一次启动和清理，不访问手机或声卡。
Qt 测试检查每设备配置保存、参数传递、状态显示和剪贴板失败不关闭 HID 会话。

仍需真机确认不同 Android/厂商 ROM 的剪贴板权限、长按粘贴、后台复制、多设备切屏以及 ADB 断线。
