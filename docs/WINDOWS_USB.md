# Windows USB OTG 设备识别

## 结论

Windows 并不是不支持 USB OTG。`deskflow-otg` 的 USB 模式和
`scrcpy --otg` 一样，直接通过 libusb 向 Android 设备发送 AOA HID 控制请求，
不使用 ADB 传输键鼠事件，也不需要 ADB daemon 或 RSA 授权。

但是，Windows 必须把手机对应的 USB 设备/接口交给 **WinUSB（或其它
libusb 可用的驱动）**。很多手机在关闭 USB 调试时会以 MTP/WPD 设备枚举，
由 Windows 的 `WUDFHost.exe` 和 MTP 驱动占用。此时 libusb 可能能看到设备，
却不能打开接口或读取序列号，最终表现为手机没有候选设备。

因此，Windows 上“USB 调试开启后能用、关闭后不能识别”通常是驱动/枚举差异，
不是 OTG 协议要求 ADB。开启 USB 调试只是某些手机上让 WinUSB/ADB 接口出现的
临时绕过办法。

## 常见现象与含义

| 日志或现象 | 含义 |
| --- | --- |
| `Could not find any Android USB device` | 没有找到既能打开又能读取序列号的 libusb 候选设备。 |
| `libusb error: Operation not supported or unimplemented on this platform` | Windows 当前驱动不支持 libusb 需要的访问方式，或接口已被系统驱动占用。 |
| `libusb error: Entity not found` | 常见于读取 USB 序列号失败；设备可能枚举了，但不能作为候选设备使用。 |
| 只列出鼠标、耳机等其它 USB 设备 | 手机没有被 libusb 访问，程序可能误把唯一可读设备选成了其它设备；不要对该设备安装 Android 驱动。 |
| ADB 能看到手机，但 OTG 仍失败 | ADB 可见不等于 AOA HID 控制请求可用；ADB 驱动和 libusb/WinUSB 访问权限要分别满足。 |

## 推荐排查步骤

1. 使用支持数据传输的 USB 线和直连 USB 口，退出其它可能占用手机的程序。
2. 在 PowerShell 中先运行：

   ```powershell
   .\build\Release\deskflow-otg.exe --usb-scan
   ```

   输出中必须出现手机的 `serial`。序列号为空或手机完全不出现时，继续处理
   Windows 驱动，不要只反复修改 `-s` 参数。

3. 打开设备管理器，确认目标是手机对应的 Android/USB 接口，不是鼠标、耳机或
   其它同样带序列号的设备。必要时启用“查看 → 显示隐藏的设备”。
4. 用 [Zadig](https://zadig.akeo.ie/) 的 **Options → List All Devices** 显示完整列表，
   选中手机对应的接口并安装/替换为 **WinUSB**。不要凭 VID/PID 猜设备，也不要
   给其它 USB 设备替换驱动；替换前记录原驱动。
5. 重新插拔手机，再次运行 `--usb-scan`。只有一个候选设备时可直接运行；多个候选
   设备时使用扫描结果中的序列号：

   ```powershell
   .\build\Release\deskflow-otg.exe `
     --host 127.0.0.1 --port 24800 --name android `
     --width 1080 --height 1920 -s <usb-scan 输出的 serial>
   ```

安装 WinUSB 可能使 MTP、厂商工具或 ADB 暂时不可用。需要恢复文件传输或 ADB
时，应在设备管理器中恢复原厂商/Google 驱动；更换驱动前请确认这是可接受的取舍。

## USB 调试与 ADB 的边界

- `deskflow-otg` 的 HID 注入不依赖 ADB；Windows 驱动问题不能通过启动或重启
  ADB daemon 修复。
- GUI 的“刷新设备”、自动读取分辨率和方向仍使用 ADB，因此这些 GUI 功能需要
  USB 调试和可用的 ADB 驱动；也可以手动填写 USB 序列号和屏幕尺寸。
- 即使手动模式不需要 ADB，USB HID 初始化仍需要手机接口能被 libusb/WinUSB 打开。
- `-s` 只能选择 libusb 扫描到的 USB 序列号，不能把网络 ADB 地址或无法读取的
  MTP/ADB 标识强行变成 AOA 设备。

## 来源与项目状态

该行为与 scrcpy [#3654](https://github.com/Genymobile/scrcpy/issues/3654) 及其关联的
[#3050](https://github.com/Genymobile/scrcpy/issues/3050) 一致：Windows 上 OTG 本身
可工作，但不同手机模式和驱动绑定会决定 libusb 是否能访问设备；部分用户通过
Google USB Driver 或 Zadig 的 WinUSB 驱动解决。该 workaround 可能影响 Windows
上的其它 Android USB 功能。

本项目的 Windows USB 真机兼容性仍需按具体手机、Windows 版本和驱动组合验证；
CI 只验证构建和不访问真实手机的测试。
