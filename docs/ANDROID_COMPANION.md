# Android 伴侣应用与桌面 GUI

`android/deskflow-otg` 是可选的手机端伴侣应用。0.1.1 同时监听系统 USB 连接状态及 accessory 拔出广播，
通过已授权的通知监听服务调用活动媒体会话的 `pause()`。不是新的键鼠连接方式，也不替代
USB AOA / BLE、scrcpy 音频或剪贴板；不安装它仍可使用原有功能。

0.1.3 按用户要求移除等待，USB 已连接转为断开后在广播回调内立即请求暂停，
不保留定时器或待执行动作；首次 sticky 状态不触发暂停，已知 USB 断开状态保持去重。
没有 USB 状态广播的 ROM 保留 accessory 事件回退，但无法据此精确判断重连或区分重复事件。
监听服务失联/销毁时重置状态基线。
适用于所有 USB 数据连接，不仅限于 Deskflow；不自动恢复播放。

0.1.2 对断开时可见的所有活动媒体会话逐个检查，播放/缓冲中的都发送 `pause()`，
不再只暂停优先级最高的一个。单会话状态读取或暂停异常不会中断后续会话；
暂停、停止、空状态的会话不处理。日志记录各播放器请求及失败并汇总请求数，
不能把请求数当作播放器已确认暂停数。没有向系统暴露媒体会话的播放器无法通过此 API 控制。

## GUI 使用

在「设备配置」中选择 USB 连接，找到「Android 伴侣应用（可选）」：

1. 手机需 Android 10+。先开启并授权 USB 调试，或自行配对、连接无线 ADB。
   「共享服务」里的 ADB 程序必须可用。
2. 核对设备页的「ADB 目标」：非空时优先使用它（支持 `host:port` / `[IPv6]:port`），
   留空才使用当前设备的 USB 序列号。每条操作都使用 `adb -s`，不会自动选择其他手机，
   不自动 pair/connect、不回退 USB、不 disconnect/kill-server。
3. 选择**已签名**的伴侣 APK，或使用桌面包附带的 APK。点击「安装 / 更新 APK」。
   使用 `install -r --user current` 保留应用数据；签名不匹配、降级或手机拒绝安装时报告错误，
   **不会先卸载旧应用**，不使用 `-d`、`-g`。
4. 点击「打开手机端设置」，启动固定包 `com.github.gh6497.deskflowotg` 的 `MainActivity`。
   在手机内点击通知使用权按钮，手动开启 deskflow-otg。桌面端不静默授予权限。
5. 点击「检查授权状态」，查询当前 Android 用户的安装和通知使用权设置。
   不读取通知正文，不记录其他应用的通知授权列表。

安装、打开、查询均为用户主动操作，不在 GUI 启动、键鼠连接或 USB 自动连接时执行。
当前设备需先断开键鼠会话；操作期间锁定设备选择及连接入口。退出等待有界 ADB 操作完成，
安装最长 60 秒，其他单条命令最长 10 秒。安装结果、授权提示中英双语实时切换，错误详情见设备日志。
APK 路径全局保存，设备 ADB 目标仍按设备保存。BLE 页不提供此 USB 拔出功能入口。

Windows 的 ADB 与 WinUSB 可能不能同时访问同一 USB 接口；可先用 ADB 安装授权，
或填写已连接的无线 ADB 目标，再使用原有 AOA HID 连接。不更换驱动、不重新连接设备。

## 桌面打包

桌面 GUI 不强制依赖 Android SDK。发布者先构建并签名 APK，再显式提供：

```bash
cmake -S . -B build-gui -DBUILD_GUI=ON \
  -DCOMPANION_APK=/absolute/path/deskflow-otg-signed.apk \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64
cmake --build build-gui --parallel
cmake --install build-gui --prefix "$PWD/stage"
```

CMake 将文件复制到 GUI 可执行文件旁，统一命名 `deskflow-otg-companion.apk`，
并安装到 Linux/Windows `bin/` 或 macOS `.app/Contents/MacOS/`，沿用现有桌面包收集流程。
CMake 只检查文件路径及后缀，不验证 Android 签名；签名有效性由发布者检查、手机安装器验证。
不传 `COMPANION_APK` 的普通桌面构建/CI 不附带 APK，GUI 仍允许手动选择；
不自动打包工作目录的 debug 或 unsigned APK，不引入签名私钥。

## 验证边界

`gui_logic` / `gui_window` 使用 fake ADB 覆盖目标固定、无线目标优先、USB 序列号回退、
安装更新与失败、缺包、打开错误、精确授权组件匹配、重复操作、语言切换、设置保存及退出等待。
`android_usb_disconnect` 另外用真实 Java 状态机覆盖首次 sticky 状态、HID-only 断开、
重复及乱序广播、立即断开处理、快速重新枚举也暂停、连续多次插拔与服务重绑定。
同一测试还覆盖多播放器暂停策略，包括多个播放/缓冲会话、空列表、跳过暂停会话及异常后继续。
需要 JDK；没有 JDK 时显式跳过。以上测试不访问 USB、系统蓝牙或真实手机。

授权设置包含组件名**不等于** Android 已绑定通知监听服务，也不代表拔出暂停已验证。
当前 AOA HID 主路径不执行 accessory-mode 切换，因此不能只依赖 `USB_ACCESSORY_DETACHED`。
Redmi K20 Pro / Android 14 已确认发出 `USB_STATE`，旧版 0.1.0 没有进入暂停逻辑；
新版仍需安装后实测，其他 ROM 的平台广播投递与播放器响应不作保证，不支持 BLE 断开暂停。
USB 重新枚举和快速拔插的短暂断开也会暂停，重连不会撤销请求或恢复播放。
立即处理表示不增加人为等待，仍受系统广播投递与播放器响应耗时影响。
原有音频断开时的 ADB `media_session dispatch pause` 路径保持不变。
