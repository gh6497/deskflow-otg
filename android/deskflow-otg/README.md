# deskflow-otg

`deskflow-otg` 是 deskflow-otg 的 Android companion app。当前版本只负责一件事：

> 手机收到 USB accessory 拔出事件后，暂停当前正在播放的媒体会话。

工程使用 Android 原生 Java API，不依赖 AndroidX 或第三方库，便于后续增加设备握手、
状态同步等功能。

## 工作方式

普通 APK 不能直接执行 `adb shell cmd media_session dispatch pause`，也不能可靠地向其他
应用注入媒体按键。因此应用实现了 `NotificationListenerService`：

1. 用户首次安装后，在系统设置中启用 deskflow-otg 的「通知使用权」；
2. Android 绑定 `DeskflowNotificationListenerService`；通知监听服务由系统管理，不需要额外的前台服务权限；
3. 服务动态监听 `UsbManager.ACTION_USB_ACCESSORY_DETACHED`；
4. 拔出时获取活动媒体会话，只对优先级最高且处于播放/缓冲状态的会话调用 `pause()`。

应用不读取通知正文，也不会暂停已经处于暂停状态的播放器。

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

## 首次安装

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -a android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS
```

在设置中启用 deskflow-otg 后，服务由 Android 自动绑定。后续可由桌面端在已有 ADB
连接时自动安装/更新 APK；USB 拔出后的暂停动作在手机本地执行，不再依赖拔出后的 ADB
连接仍然可用。

## 限制

- 需要 Android 10（API 29）或更高版本。
- 首次使用必须由用户手动授予通知使用权，应用不能通过 ADB 静默授予该权限。
- 当前监听的是 USB accessory 拔出广播，后续如需支持更多连接类型，可在服务中增加独立
  的连接状态模块。
