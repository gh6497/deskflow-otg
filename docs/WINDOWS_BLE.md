# Windows BLE CLI（实验性）

Windows 原生 GATT Server 将电脑作为 BLE HID 键盘 + 绝对指针外设，手机作为主机。
不需要手机 App、ADB 或 USB 调试。此后端接入现有 Deskflow 协议与 bridge 状态机，
不是把终端输入字符转发到手机；暂不支持 GUI、常驻服务或 Linux 蓝牙隔离选项。

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
`BUILD_FEATURES.json` 的 `windowsBleCli` 表示此特性；原有 `ble` 仍指 Linux 常驻/GUI 后端。

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

把示例 MAC 替换为自己的手机地址；宽高按手机当前方向填写。
只支持默认适配器和默认 absolute 指针；不接受 `--adapter`、`--serial`、`--gui`、
`--mouse-mode relative`、`--ble-service`、`--ble-session` 或隔离/恢复选项。

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
- 当前排障版所有通知统一至少间隔 30ms（包括键盘和范围/按钮屏障），降低切屏时的突发压力；
  顺序仍保留，不丢弃状态屏障。此值是保守实验参数，不是实测连接间隔，可能增加输入延迟。
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
