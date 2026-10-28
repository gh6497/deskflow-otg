# Windows BLE HID 最小原型

这是独立的可行性实验，**尚未接入 Deskflow 或 GUI，也不是已验证的 Windows 蓝牙后端**。
验证 Windows 能否作为 BLE HID 外设，让 Android 无 App、无 USB 调试地接收键盘及绝对指针。
不改电脑蓝牙名称、不关闭其他蓝牙连接、不安装驱动或系统服务。

## 构建

需要 Windows 10/11、Visual Studio 2022 的「使用 C++ 的桌面开发」、Windows SDK
（含 C++/WinRT 头文件）和 CMake。这个独立项目不依赖 Qt、libusb 或 vcpkg。
在仓库根目录执行：

```powershell
cmake -S experiments/windows-ble-hid -B build-win-ble-probe -A x64
cmake --build build-win-ble-probe --config Release --parallel
ctest --test-dir build-win-ble-probe -C Release --output-on-failure
.\experiments\windows-ble-hid\test-descriptors.ps1
```

产物：`build-win-ble-probe/Release/deskflow-otg-ble-probe.exe`。
普通用户运行即可；管理员权限不能解决适配器不支持外设角色的问题。
也提供独立的 GitHub Actions 工作流 **Windows BLE HID prototype**；推送相关文件后或手动
运行，可从该次运行的 Artifacts 下载程序、本文档及源码快照。CI 只编译/检查报文，不访问蓝牙。

## 真机测试

### 1. 只检查硬件能力（不广播）

```powershell
.\build-win-ble-probe\Release\deskflow-otg-ble-probe.exe --probe
```

需要 `LE supported=1 peripheral supported=1`。为 0 时，先检查当前默认适配器及厂商驱动。
原型只使用默认适配器，不支持在多适配器间选择。

### 2. 先测键盘

```powershell
.\build-win-ble-probe\Release\deskflow-otg-ble-probe.exe --keyboard-only |
  Tee-Object -FilePath windows-ble-keyboard.log
```

1. 等待广播状态日志。`Advertising requested` 只表示提交请求，不能证明广播已成功。
   `ADVERTISEMENT` 会打印 Windows 的状态与 `BluetoothError` 数值；失败时请保留日志。
2. 在**手机蓝牙设置**搜索这台电脑的名称并配对，确认系统配对提示。不是在电脑上扫描手机。
   HID 服务要求加密；本程序不绕过系统配对。Windows 使用系统 GAP/设备信息，原型不尝试
   注册被系统保留的 Device Information 服务，也不能保证手机将其显示为键盘图标。
3. 终端输入 `status` 并回车。需要键盘 Report 有一个订阅者，且显示的是你的手机，
   `paired=1 connected=1`。若没有订阅，请先把这一阶段的日志发回，不要盲目继续。
4. 手机打开一个空白文本输入框。终端输入 `arm` 并回车，明确授权当前唯一订阅者。
5. 输入 `key` 并回车。程序发送 HID `a` 按下、松开；英文输入法下应出现一个 `a`。
   其他输入法/布局下可能显示不同字符。
6. 输入 `quit` 正常退出。

安全限制：未 `arm` 不发送输入；只向选定的已配对、在线订阅者定向通知，
不会向全部订阅者广播。订阅发生变化后必须重新 `arm`；多订阅者拒绝发送。
这只是显式授权的本地实验工具，不是完成安全审计的后台产品。

### 3. 再测键盘 + 绝对指针

这一步改变 Report Map，Android 可能继续使用旧键盘缓存。请先正常退出程序，
**仅删除这对实验设备的旧配对**，再运行不带 `--keyboard-only` 的版本，重新配对。
不要删除无关蓝牙设备。普通断线重连不应该通过反复删除配对来处理。

```powershell
.\build-win-ble-probe\Release\deskflow-otg-ble-probe.exe |
  Tee-Object -FilePath windows-ble-pointer.log
```

输入 `status`，确认键盘和指针各有一个订阅者，且属于同一手机，然后输入 `arm`。
在安全的测试页面逐个输入命令，每个命令按回车：

- `key`：同上，发送一次 `a`。
- `center`：悬停在屏幕中心，不点击。
- `corners`：左上、右上、右下、左下、中心，各停留约 0.7 秒，不点击。
- `click`：在屏幕中心按下并松开左键/笔尖。**会真的点击，请避开付款、删除等按钮。**
- `wheel`：在屏幕中心发送一格滚轮，再清零滚轮。
- `drag`：从横向 1/4、纵向 1/2，拖动到横向 3/4、纵向 1/2。会真的拖动。
- `release`：尽力松开所有键/按钮并结束悬停。
- `disarm`：释放输入并撤销授权。
- `quit`：尽力释放输入、停止广播并退出。EOF 也走退出清理。

测试结束请使用 `quit`，不要直接关终端、Ctrl+C 或强杀；后者不保证执行释放。
掉线时无法保证松开报告送达手机，需观察 Android 是否自行清理按键状态。

### 4. 断线重连

保持同一模式和进程运行，手机主动断开后重新连接，运行 `status`，必要时重新 `arm`，
再测 `key`、`center`。随后可另测程序正常退出并重开后的行为，但**不承诺免重配**：
Windows 动态 GATT handle、系统服务和 Android 缓存兼容性都是本次实验需要验证的内容。

## 发回哪些信息

- Windows 版本、蓝牙适配器型号/驱动版本、手机型号/Android 版本。
- `windows-ble-keyboard.log`、`windows-ble-pointer.log`（可遮盖设备 ID/地址）。
- 手机是否搜到电脑、配对是否成功、两个 Report 是否订阅。
- 键盘字符、指针悬停/四角/点击/拖拽/滚轮的实际现象。
- 同进程重连、重开程序后是否还能输入，有无卡键。

## 实现边界

### 已通过的单组真机测试（2026-10-27）

Windows 默认 Intel 蓝牙适配器（USB VID 8087 / PID 0AAA）配合用户的
Huawei nova 9（系统版本未记录），已由用户确认：

- `--keyboard-only` 配对、订阅及 `key` 输入一个 `a`。
- 键盘 + 绝对指针模式可悬停；中心点击、水平拖拽和滚轮可用。
- 圆角屏下无法准确判断四角极限到达，不能标记全屏边界精度已验收。

这是手动低频测试，不代表 Deskflow 连续输入、断线重连、进程重启免重配、
休眠恢复或持键掉线释放已验证。Redmi 的 ROM 配对崩溃记录见下文。

### 已观测的手机 ROM 配对崩溃（2026-10-27）

Redmi K20 Pro / Android 14（设备报告 OS2.0.2.0.ULJCNXM）在测试配对期间，
20:12:37、20:13:30、20:15:50 的 Android crash buffer 均记录了
`com.android.bluetooth` 主线程未捕获异常：

```text
IllegalArgumentException: value of system property
'persist.vendor.bt.a2dp.lhdc.whitelist' is longer than 91 bytes
SystemProperties.set
A2dpService.addToWhiteList
A2dpService.handleLHDCdefaultCloseBond
A2dpService.dealBondStateChangedReceiver
```

这是这些已记录重启的直接原因：配对事件触发 ROM 的 LHDC 音频白名单写入异常，
而不是该崩溃栈显示 HID 描述符解析失败。仍不能据此断言原型已兼容该手机。
暂不重复配对；先保留日志，再调查 ROM 配置/实现。不要自动清除蓝牙数据、删除
其他设备配对或擅自修改持久属性；目前没有验证过的绕过方案。

### 协议与生命周期限制

- HID Service `0x1812`；Report Map、HID Information、Control Point、键盘输入/LED 输出、
  可选指针输入，Report Reference 分别为 `1/Input`、`1/Output`、`2/Input`。
- Report Map 在现有 `src/bridge.c` 的描述符快照中插入 Report ID；
  GATT Report 值为 8 字节，不包含 Report ID。CCCD 交给 Windows 自动生成。
- Report Map 为静态特征值，长读由 Windows 处理。输入报告可读，LED 写入及挂起命令可处理。
- 使用加密保护；未实现自定义配对代理、Boot Protocol、BAS 或私有 HCI 操作。
- Windows 保留的 DIS/GAP、广播外观、外设角色、动态句柄和配对缓存可能阻碍 Android HID
  识别。即使服务建立/通知成功，也必须以手机实际输入为准，不能据此声称符合完整 HOGP 认证。
- 手动测试串行等待通知完成，没有高频输入队列、Deskflow、GUI、常驻服务或自动重连。
- `--self-test` 仅检查 Report Map 和报文编码，不访问蓝牙，不是硬件兼容性测试。

API 参考：https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-server
