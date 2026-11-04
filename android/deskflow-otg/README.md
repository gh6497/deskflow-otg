# deskflow-otg

`deskflow-otg` 是 deskflow-otg 的 Android companion app。当前版本只负责一件事：

> 手机收到 USB 数据连接断开事件后，立即请求暂停所有正在播放或缓冲的媒体会话。

工程使用 Android 原生 Java API，不依赖 AndroidX 或第三方库，便于后续增加设备握手、
状态同步等功能。

## 工作方式

普通 APK 不能直接执行 `adb shell cmd media_session dispatch pause`，也不能可靠地向其他
应用注入媒体按键。因此应用实现了 `NotificationListenerService`：

1. 用户首次安装后，在系统设置中启用 deskflow-otg 的「通知使用权」；
2. Android 绑定 `DeskflowNotificationListenerService`；通知监听服务由系统管理，不需要额外的前台服务权限；
3. 服务动态监听系统 `android.hardware.usb.action.USB_STATE` 的 `connected` 状态，
   并保留 `UsbManager.ACTION_USB_ACCESSORY_DETACHED` 作为兼容路径；
4. 首次 sticky 状态仅初始化，不触发暂停；从已连接转为断开后，在广播回调内立即处理，
   不使用定时器或延迟确认。已知 USB 断开状态保持去重；重连只重新启用下次断开检测；
5. 收到断开事件后遍历活动媒体会话，对每一个处于播放/缓冲状态的会话调用 `pause()`，
   不会处理完第一个就返回；单个会话读取状态或暂停失败时，记录错误并继续处理其他会话。
   通知监听服务失联或销毁时，取消待执行动作并重置连接基线。

应用不读取通知正文，也不会暂停已经处于暂停状态的播放器。
日志逐会话记录暂停请求和失败，最后汇总成功发出的请求数；请求成功不等于播放器已确认暂停。

## 界面语言

界面支持简体中文和英文，跟随手机系统语言；其他语言使用英文回退。
所有界面文本通过 Android 字符串资源引用：

- `app/src/main/res/values/strings.xml`：完整的英文默认文本。
- `app/src/main/res/values-zh-rCN/strings.xml`：简体中文翻译。

新增界面文本时使用 `R.string` / `@string` 引用，并同步补齐两种语言。
应用名称 `deskflow-otg` 标记为不可翻译，所有语言共用。

## 构建

需要 JDK 17、Android SDK platform 36 和 build-tools。使用 Gradle wrapper：

```bash
./gradlew :app:assembleDebug
./gradlew :app:assembleRelease
```

APK 位于 `app/build/outputs/apk/`。

USB 状态策略的主机回归（仅需 Python 3 与 JDK，不访问手机）：

```bash
python3 ../../tests/test_android_usb_disconnect.py
```

桌面 CTest 也纳入 `android_usb_disconnect`；未安装 JDK 时显式跳过，不影响桌面构建。

## 首次安装

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -a android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS
```

在通知使用权设置中启用 deskflow-otg 后，由 Android 尝试绑定服务；ROM 的自启动或省电限制
可能阻止绑定，不能仅凭「已授权」判断生效。桌面 GUI 的 USB 设备页现提供
「安装 / 更新 APK」「打开手机端设置」「检查授权状态」，使用当前设备的已有 ADB 连接，
由用户点击触发，不会静默安装或授予通知使用权。USB 拔出后的暂停动作在手机本地执行，
不再依赖拔出后的 ADB 连接仍然可用。打包与限制见
[桌面 GUI 接入说明](../../docs/ANDROID_COMPANION.md)。

## 生效条件：自启动与电池策略

拔线暂停要求 **通知使用权已开启，且通知监听服务在拔线时实际运行**。请在手机上完成：

1. 手动开启 deskflow-otg 的「通知使用权」，不是普通的「允许发送通知」。
2. 如果系统提供「自启动 / 后台自启动」选项，允许 deskflow-otg 自启动及后台启动。
3. 将应用电池 / 省电策略设为「无限制」「不限制」或同等选项，允许后台活动。
   小米 / Redmi 等 ROM 尤其需要检查；具体菜单名称因系统而异。
4. 安装或更新 APK 后打开应用一次，确认服务恢复。若仍未运行，关闭再开启本应用的
   通知使用权后重试；强制停止、后台清理和手机重启后也要重新确认。

应用界面和桌面 GUI 的授权检查不代表服务存活，也不会替用户修改上述设置。
即使放宽限制，不同 ROM 仍可能结束服务；服务未运行时无法收到拔线事件，恢复后也不会
补处理已经错过的事件。应恢复服务并重新连接 USB，再进行播放、拔线测试。

已在 Redmi K20 Pro / Android 14 观察到更新后授权仍在、服务列表却为空，系统提示
`AutStart Unable to bind notification listener service`。只读检查命令和详细限制见
[手机端生效条件与后台限制](../../docs/ANDROID_COMPANION.md#手机端生效条件与后台限制)。

## 限制

- 需要 Android 10（API 29）或更高版本。
- 首次使用必须由用户手动授予通知使用权，应用不能通过 ADB 静默授予该权限。
- 自启动与电池策略不得阻止通知监听服务运行；授权成功不代表后台运行条件已经满足。
- 0.1.1 修复 0.1.0 仅监听 accessory 拔出而漏掉 AOA HID-only 断开的情况。
  Redmi K20 Pro / Android 14 的只读证据已确认拔线时发出 `USB_STATE`，旧版没有暂停调用；
  新版仍需覆盖安装后实测拔线，不能把主机状态机测试当成真机验收。
- 0.1.2 将单个最高优先级播放器改为所有正在播放/缓冲的会话，版本码 3。
  主机回归同时覆盖多个播放器、单会话异常隔离和不干扰暂停会话；手机端仍需更新后实测。
- 0.1.3 按用户要求移除 1.5 秒去抖等待，版本码 4；收到断开广播即发送暂停请求。
  系统广播投递和播放器响应仍有自身耗时，不代表硬实时暂停。
- 监听所有 USB 数据连接，不限于 Deskflow；仅充电、BLE 断线不保证产生相同广播。
  重新枚举和快速拔插的短暂断开也会触发暂停，重连不会撤销暂停请求或恢复播放。
- `USB_STATE` 是 Android 平台广播但常量不属于公开 SDK，因此以 action 字符串监听；
  其他 ROM 的广播投递、后台存活与播放器响应仍需实测。不会自动恢复播放。
