# Windows BLE CLI / GUI（实验性）

Windows 原生 GATT Server 将电脑作为 BLE HID 键盘 + 绝对指针外设，手机作为主机。
不需要手机 App、ADB 或 USB 调试。此后端接入现有 Deskflow 协议与 bridge 状态机，
不是把终端输入字符转发到手机；支持 GUI 管理的桥接进程，但不支持 Linux 常驻服务或蓝牙隔离选项。

独立原型的 nova 9 手动测试已通过，见
[`experiments/windows-ble-hid/README.md`](../experiments/windows-ble-hid/README.md)。
正式桥接路径的连续输入、延迟、重连和持键退出仍需另行真机验收。

## 构建

需要 VS2022 C++ Build Tools、Windows SDK（C++/WinRT）、CMake 和静态 libusb。
只有 WinRT 适配器使用 C++20 协程，桥接核心仍为 C11，GUI 仍为 C++17。
默认不编译此实验后端；现有 USB 构建不受影响。

```powershell
vcpkg install libusb:x64-windows-static
cmake -S . -B build-win-ble -A x64 -DBUILD_GUI=OFF -DBUILD_BLE_WIN=ON `
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-win-ble --config Release --parallel
ctest --test-dir build-win-ble -C Release --output-on-failure
```

产物：`build-win-ble/Release/deskflow-otg.exe`。
Windows CI 也编译此 CLI 后端并执行不访问蓝牙的回归测试。
`BUILD_FEATURES.json` 的 `windowsBleCli` 为兼容保留的特性标记，也适用于 GUI 的 Windows 后端；
原有 `ble` 仍指 Linux 常驻后端。

### GUI 构建与使用

安装 Qt 6.8+ 后，在上述配置中改用 `-DBUILD_GUI=ON`，并通过 `-DCMAKE_PREFIX_PATH`
指定 Qt 前缀。启动 `build-win-ble/Release/deskflow-otg-gui.exe`，选择
「低功耗蓝牙（Windows，实验性）」，填写手机地址与屏幕尺寸即可连接。GUI 可自动配置并
启动自己管理的 Deskflow server，也可使用已有的明文 server；蓝牙模式不调用 ADB、不自动连接 USB。

GUI 使用 `--gui --gui-ble-control --transport ble --ble-peer MAC` 前台子进程，不传 `--adapter`、
`--ble-session`、隔离/提权或 `--background` 参数。等待配对无固定超时，可随时点「断开」取消；
身份/订阅就绪并完成 Deskflow DSOP 后才显示已连接，手机掉线时显示等待重连。
「断开」通过 stdin 发 `pause`：先关闭 Deskflow TCP，再丢弃未发动作，把键盘、按钮与范围
释放排在已有在途通知后面。确认队列/在途通知处理完（或手机已失联）才回 `ble-idle`，GUI 才恢复空闲。
同一 GATT provider、特征和订阅一直保留；空闲时不重连 Deskflow、不接受新输入。
下次「连接」发 `resume<TAB>host<TAB>port<TAB>screen<TAB>width<TAB>height`（换行结束），
复用该进程，并可更新服务器/屏幕名/尺寸，不重新注册 HID。协议有长度、状态与数值校验，
只通过 GUI 私有的子进程 stdin 使用，不能改变目标手机或 HID 描述符。普通 `--gui` 仍为任意字节停止。
释放/控制超时（8 秒）会失败关闭自己的 provider，日志明确提示 GATT 连续性丢失，不假报成功保留。
释放是尽力而为，手机失联或驱动错误时不能保证送达。

关闭窗口隐藏到托盘不停止桥接（无托盘时关闭仍退出）。「停止空闲蓝牙服务」或「退出」才
停止 GUI 自己的 provider 并注销 GATT，stdin EOF/父 GUI 崩溃也会退出，避免孤儿进程。
GUI 退出后不保留 Linux 风格的独立常驻服务，也不停止其他 CLI/原型进程。
目标身份在 provider 生命周期中固定，换手机或桥接程序须先显式停止旧服务，不能静默重建。
临时切到 USB 不停止空闲的 Windows provider，但退出 GUI 时会一并停止。
进程重启可能触发 Android 缓存恢复，不能承诺免重配；开始前请显式停止其他 BLE 原型或后台实例。
GUI 参数、状态和停止流程有离线模拟测试；真实 `main.c` + bridge + fake BLE/Deskflow
回归验证重复会话只创建一次 provider、释放确认、配置更新、USB 切换、显式停止、超时关闭与
活动/空闲/等待配对时 EOF。它不访问真实蓝牙，不能代替手机连续输入与重连验收。

## 配置 Deskflow

1. 使用已有 Deskflow GUI，将手机配置为名为 `android` 的 secondary screen，放在电脑一侧。
2. 关闭 server 的 TLS/加密；桥接目前只支持明文协议。仅本机使用时监听回环地址。
3. 确保 server 已运行，地址/端口与下方参数一致。CLI 不会启动、修改或关闭已有 Deskflow。
4. 停止独立 BLE 原型后再启动桥接，不要让两套 HID 服务同时运行。

## 运行

目标地址必须显式指定。使用原型 `status` 的 `DeviceId` 中**最后一段手机地址**，
不是前面的电脑适配器地址；也可从 Windows 显示的该已配对 BLE 设备信息确认。
例如 `BluetoothLE#BluetoothLE<电脑地址>-<手机地址>` 中，取 `<手机地址>`。
实现还会读取在线 `BluetoothLEDevice.BluetoothAddress`、配对状态和订阅者核对，
不会根据设备名称授权，也不会回退到任意手机。隐私地址/身份地址解析差异尚未普遍验证。

```powershell
.\build-win-ble\Release\deskflow-otg.exe --transport ble `
  --ble-peer AA:BB:CC:DD:EE:FF `
  --host 127.0.0.1 --port 24800 --name android --width 1080 --height 2340
```

如果希望关闭 PowerShell/命令行后仍保持同一个 GATT provider（避免 Android 下次连接时
丢失 HID 订阅），使用 `--background`：

```powershell
.\build-win-ble\Release\deskflow-otg.exe --transport ble `
  --ble-peer AA:BB:CC:DD:EE:FF `
  --host 127.0.0.1 --port 24800 --name android --width 1080 --height 2340 `
  --background
```

父命令会立即返回，后台子进程继续持有 GATT 服务并在 Deskflow/手机暂时断开后重试连接。
同一用户只允许一个后台实例；停止时使用：

```powershell
.\build-win-ble\Release\deskflow-otg.exe --background-stop
```

`--background-stop` 会请求后台进程优雅释放 HID 输入、停止广播并退出。后台模式不接入 GUI，
也不改变配对或删除手机缓存。

把示例 MAC 替换为自己的手机地址；宽高按手机当前方向填写。
只支持默认适配器和默认 absolute 指针；不接受 `--adapter`、`--serial`、
`--mouse-mode relative`、`--ble-service`、`--ble-session` 或隔离/恢复选项。
`--gui` 供桌面端使用，stdin 任意字节或 EOF 请求优雅退出，stdout 输出 JSON 状态；
它不能与 CLI 的 `--background` 合用。

1. 在手机蓝牙设置连接/配对电脑，确认配对提示。首次从原型迁移时 Windows 的 GATT
   动态句柄可能改变；若手机保留旧缓存而无法订阅，才单独解除这对实验配对后重新配对。
   **不要自动删除配对，也不要在 Redmi ROM 已崩溃的环境反复配对。**
2. 日志出现 `target paired, connected and subscribed; input ready` 后，才开始 Deskflow TCP 连接。
   不需要 `arm`/`key` 命令：`--ble-peer` 是显式选择目标的授权。
3. 把鼠标从 Deskflow 配置的边缘移到手机，正常使用键鼠。
4. 光标移回电脑后，Ctrl+C 正常退出。程序尽力按序释放输入，再停止广播/注销服务。
   强杀/关终端、手机失联时不能保证释放送达。

## 传输与失联行为

- WinRT 服务、设备身份检查、通知完成等待均在专用工作线程，Deskflow 回调只入队。
- 队列最多 64 条；仅相邻且同按钮/范围状态的纯移动可合并。首次位置、按下/松开、
  键盘、滚轮及其清零、In Range 变化均作为顺序屏障保留。
- 默认通知间隔为 30ms（包括键盘和范围/按钮屏障），用于 Windows 10 或无法读取链路参数
  的驱动；顺序仍保留，不丢弃状态屏障。Windows 11 会在可用时改用实测协商间隔。
- Windows 11 如果系统暴露已协商的 LE connection interval，通知节流会跟随该间隔，并在
  `ConnectionParametersChanged` 时更新；Windows 10 或驱动不支持时回退到 30ms。状态屏障
  仍与指针移动共享同一顺序队列，不会因自适应节流而丢失。
- HID 服务补充 `Protocol Mode`（当前固定 Report Protocol；Boot Protocol 会明确拒绝）、
  `HID Information` 的 `RemoteWake | NormallyConnectable` flags，以及只读 Battery
  Service（100%）。这些是兼容性补全，不改变当前绝对 Digitizer 报告格式。
- 同时需要键盘和指针各一个订阅者，必须是同一个在线、已配对、匹配目标地址的手机。
  多订阅者/身份不匹配时关闭输入，通知只定向发给已验证目标。
- 订阅变化、断线、挂起会撤销 ready、清空待发事件；主循环断开 Deskflow TCP、重置
  bridge 输入，避免手机失联后继续占据 secondary screen。同进程保留 GATT，等待手机
  回来并重新握手；恢复 ready 时先发送全松开报告，再接受新输入。
- 已观测到断线/订阅代次变化时，取消旧会话的等待并保留 GATT，不把这类取消当作致命超时。
  Cancel 只是请求，必须等旧通知真正完成/取消后才能恢复 ready 和发送新通知；超过 5 秒仍
  未结束则退出，避免新旧输入交错。恢复 ready 时仍先发送释放报告。
- 队列溢出或没有会话变化的 WinRT 操作超时视为传输失败，尽力释放后退出，而不是丢弃松键并继续。
  正常退出也丢弃尚未发出的动作，等待在途通知后发送最终释放；异步操作及释放等待设有超时，
  不以强杀线程代替清理。
- 蓝牙系统、进程重启、Windows 休眠和 Android GATT 缓存行为尚未验收，不保证免重配。
- 不改电脑蓝牙名称、适配器模式、其他设备配对、手机设置或系统持久属性。

## 真机验收清单

- Deskflow enter/leave、连续快速移动、点击/拖拽与滚轮位置正确。
- 连续文字、Shift/Ctrl/Alt、组合键、长按和松开，无卡键。
- 手机主动断开时电脑取回光标；同一进程内重连后输入恢复，无旧输入回放。
- 持键/拖拽时正常 Ctrl+C、手机断开、进程被强杀后的状态（后三者不能承诺全部释放）。
- 程序重启、电脑休眠恢复及手机重启后是否需要重新配对。

先在安全的空白文本/绘图页面测试；日志和手机实际现象都通过后才能声称验收完成。

## 当前排障记录

用户已报告在 nova 9 上通过身份检查、完成远端 Deskflow `Barrier 1.8` 握手，并收到
540×1170 虚拟屏幕的 enter/move 事件；随后 Windows 报告手机断线，旧实现的未区分
会话代次的等待又超时退出。尚未证明最初断线的原因，也不能据此标记连续输入已通过。

新日志含相对时间和操作名（如 `notify pointer` / `refresh pairing state`）；
地址不匹配会打印 expected/actual/DeviceId，解析失败另行显示。
若仍掉线，请保留从启动到掉线的完整日志；需要手机侧日志时连接的是 nova 9，
不要误采之前 Redmi 的崩溃记录。未自动修改配对或任何手机设置。

2026-10-28 新日志没有先出现手机断线，而是连续 enter/leave 后 `notify pointer`
超时，释放也失败。此现象不能再直接归因为手机断链。排障版增加通知超时时的
成功发送计数、队列长度、在线与 GATT session 状态，统一30ms通知节流；致命超时
在释放/注销前立即关闭 ready，避免清理期间继续接收新输入。根因和真机改善尚待确认。

同日 nova 9 的手机侧实时日志补充：11:10:22.031 按既有信息注册键盘/指针通知
（handles 99/103），.032 HID OPEN 成功并复用164字节描述符，.060 收到 Service Changed
indication，范围 `0x005D..0x006E`；后续服务发现确认这个范围就是 HID Service 0x1812。
同一条 indication 被多个 GATT client 回调打印，不能把多行日志当作多次空口通知。
这是缓存恢复与 Windows 动态 GATT 生命周期交错的具体证据，但不足以证明通知超时的根因。
服务发现的 ATT 0x0A（Attribute Not Found）可用于正常结束枚举，不能单独视为故障。

11:10:33 手机设置发起断开，11:10:34 调用 removeBond；用户明确这是输入已经失效后
的手动恢复行为，不是初始故障原因。待核对相同测试的电脑日志及 Windows 实际服务/
特征句柄；优先调查保持 GATT 生命周期及缓存重发现后的订阅，不再把反复删除配对当作修复。
