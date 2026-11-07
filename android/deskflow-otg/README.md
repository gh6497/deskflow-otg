# deskflow-otg

`deskflow-otg` 是 deskflow-otg 的 Android companion app，提供：

- 手机收到 USB 数据连接断开事件后，立即请求暂停所有正在播放或缓冲的媒体会话。
- 0.1.4 起，在应用首页显示由桌面 GUI 通过 ADB 同步的实际 Deskflow 会话状态。
- 0.1.5 起，可主动开启「连接后自动恢复所选播放器」（默认关闭），每次真实连接仅尝试一次。
- 0.1.6 起，支持系统「分享至电脑」与图片 / 文件双向传输，复用已授权 ADB，不需要通知使用权。
  分享后在桌面「文件与分享」刷新、选择接收；电脑发来的文件在 App 中通过系统选择器另存。
  单文件1 GiB、私有暂存合计32个 / 2 GiB，详见 [文件分享](../../docs/FILE_SHARING.md)。
- 新版桌面的中键 Home / 右键 Back 直接发送 Win+Enter / Win+Backspace 系统 HID 快捷键，不依赖本应用；两项在桌面独立配置、默认普通鼠标按钮，需要手机 ROM 支持系统快捷键。

工程使用 Android 原生 Java API，不依赖 AndroidX 或第三方库，便于后续增加设备握手、
状态同步等功能。

## 工作方式

普通 APK 不能直接执行 `adb shell cmd media_session dispatch pause`，也不能可靠地向其他
应用注入媒体按键。因此应用实现了 `NotificationListenerService`：

1. 用户首次安装后，在系统设置中启用 deskflow-otg 的「通知使用权」；
2. Android 绑定 `DeskflowNotificationListenerService`；通知监听服务由系统管理，不需要额外的前台服务权限；
3. 清单中的 `UsbDisconnectReceiver` 监听系统 `android.hardware.usb.action.USB_STATE` 的
   `connected` 状态，并保留 `UsbManager.ACTION_USB_ACCESSORY_DETACHED` 作为兼容路径；
4. 首次 sticky 状态仅初始化，不触发暂停；从已连接转为断开后，在 Receiver 回调内立即处理，
   不使用定时器或延迟确认。已知 USB 断开状态保持去重；重连只重新启用下次断开检测；
5. 收到断开事件后遍历活动媒体会话，对每一个处于播放/缓冲状态的会话调用 `pause()`，
   不会处理完第一个就返回；单个会话读取状态或暂停失败时，记录错误并继续处理其他会话。
    连接状态保存在应用私有设置中，避免 Receiver 进程重启后重复暂停。

应用不读取通知正文，也不会暂停已经处于暂停状态的播放器。
日志逐会话记录暂停请求和失败，最后汇总成功发出的请求数；请求成功不等于播放器已确认暂停。

## 界面语言

界面支持简体中文和英文，跟随手机系统语言；其他语言使用英文回退。
所有界面文本通过 Android 字符串资源引用：

- `app/src/main/res/values/strings.xml`：完整的英文默认文本。
- `app/src/main/res/values-zh-rCN/strings.xml`：简体中文翻译。

新增界面文本时使用 `R.string` / `@string` 引用，并同步补齐两种语言。
应用名称 `deskflow-otg` 标记为不可翻译，所有语言共用。

## 界面布局

界面使用原生 XML 布局，位于 `app/src/main/res/layout/`，可在 IDE 中预览和调整：

- `activity_main.xml`：首页、连接状态、通知使用权与自动播放设置。
- `activity_transfer.xml`：文件列表页，`item_transfer.xml` 定义动态列表项，
  `transfer_empty.xml` 定义空状态。
- `activity_share.xml`：系统分享确认、进度与取消。

Activity 通过 `setContentView` / `LayoutInflater` 加载布局，Java 仅负责控件绑定、
动态内容、事件和业务逻辑；不依赖 AndroidX、Compose 或第三方界面库。
布局资源与控件绑定的主机检查（仅需 Python 3，不代表真机界面验收）：

```bash
python3 ../../tests/test_android_layouts.py
```

## Deskflow 连接状态（0.1.4）

手机首页显示「正在连接 / 已连接 / 正在断开 / 已断开 / 状态未知」，不把 USB 插线或
通知使用权当作 Deskflow 已连接。请同时更新桌面 GUI，安装 APK 后先打开一次应用。

- 桌面 GUI 以桥接进程的 `connected` 事件（完整 Deskflow 握手完成）为依据，转换时立即
  尝试上报，活动会话每 5 秒续报。手机前台每秒刷新，超过 15 秒没有新报告显示「状态未知」。
  已断开只上报一次，也会在 15 秒后过期；不是永久保留的历史连接记录。
- 状态同步需要**已经在线并授权的 ADB**；不依赖通知使用权，也不启动通知监听服务。
  无 ADB 时键鼠仍照常运行，手机显示未知；CLI 暂不提供同步。
- 每条命令固定 `adb -s`；优先设备配置的 ADB 目标，USB 才回退该设备 USB 序列号。
  BLE 必须有显式 ADB 目标；不会使用 BLE MAC，不自动 pair/connect 或选择其他设备。
- 显式广播接收器要求发送者持有 `android.permission.DUMP`（ADB shell 具备）；普通应用
  无法伪造状态。接收协议版本 1/2 和固定状态值，不接收任意执行命令或键鼠输入。
  v2 的一次性连接事件可在用户明确开启后触发目标媒体会话的播放请求。
- 报告以手机单调时钟和启动次数校验，过期、重启或缺少启动次数时均显示未知。
  原有 USB 拔线暂停路径及立即处理策略不变；USB 断开后不能通过已断开的 ADB 保证立即上报。
- 安装后未打开、应用被强制停止或 ROM 拦截广播时可能收不到状态；不主动唤起界面、不绕过
  强制停止。ADB 故障或桌面异常退出后，旧状态最多保留至租约过期。

主机回归覆盖租约、重启和无效状态；桌面 `companion_status` 用 fake ADB/桥接验证真实事件
驱动、续报、断开、目标固定、超时和旧回调丢弃，不代表真机广播投递已验收。

## 连接后自动播放（0.1.5）

1. 同时更新桌面 GUI 和 APK；安装更新后打开伴侣应用，手动授权通知使用权并确认监听服务运行。
2. 在音乐 App 中播放一次，再返回伴侣应用，点击「从活动媒体会话选择播放器」选定目标。
3. 开启「连接后自动恢复所选播放器」（默认关闭），暂停音乐，再连接 Deskflow。

仅恢复指定应用支持 `ACTION_PLAY` 的已暂停媒体会话；无会话就跳过，不启动 App、不换播放器、
不恢复全部应用。启用音频转发时等待电脑 RAW 音频头和 ALSA/WASAPI 输出就绪，不等首帧 PCM。
每次连接独立 ID；一次性播放事件与心跳分离，先持久化去重，再请求 `play()`。
ADB 离线、监听未绑定、权限缺失或播放器失败时不补播；App/服务重启、租约续报不重试，
自动播放后手动暂停不会被心跳恢复。关闭开关或修改目标不对当前连接补播。
真正断开再连接才允许再次尝试。USB 插线本身不触发播放，原有拔线立即暂停不变。

v1 桌面仍可显示状态但不自动播放；新版桌面使用 v2，旧 APK 不识别，需要一起更新。
没有已授权 ADB 的连接与 CLI 会话不支持该功能。BLE 必须配置显式 ADB 目标。
手机 ROM、后台保活及播放器响应仍需真机验收，发送播放请求不等于实际已播放。
详见 [桌面 GUI 接入说明](../../docs/ANDROID_COMPANION.md)。

## 构建

需要 JDK 17、Android SDK platform 36 和 build-tools。使用 Gradle wrapper：

```bash
./gradlew :app:assembleDebug
./gradlew :app:assembleRelease
```

APK 位于 `app/build/outputs/apk/`。
默认 `assembleRelease` 产物未签名。桌面 0.1.7 起的正式 CI 会先测试并构建 release APK，
再使用固定的 Actions Secrets 发布密钥签名/验签，将同一 APK 放进所有平台 GUI 包。
本地桌面构建仍可显式指定 `COMPANION_APK`，不要求 Android SDK；
签名配置和覆盖更新限制见[桌面打包说明](../../docs/ANDROID_COMPANION.md#ci-正式签名与各平台发布桌面-017-起)。

USB 状态策略的主机回归（仅需 Python 3 与 JDK，不访问手机）：

```bash
python3 ../../tests/test_android_usb_disconnect.py
python3 ../../tests/test_android_connection_receiver.py
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

拔线暂停要求 **通知使用权已开启，且应用未被 ROM 强制停止或拦截后台广播**。请在手机上完成：

1. 手动开启 deskflow-otg 的「通知使用权」，不是普通的「允许发送通知」。
2. 如果系统提供「自启动 / 后台自启动」选项，允许 deskflow-otg 自启动及后台启动。
3. 将应用电池 / 省电策略设为「无限制」「不限制」或同等选项，允许后台活动。
   小米 / Redmi 等 ROM 尤其需要检查；具体菜单名称因系统而异。
4. 安装或更新 APK 后打开应用一次，确认通知使用权仍然开启；若权限失效，关闭再开启本应用的
   通知使用权后重试。

应用界面和桌面 GUI 的授权检查不代表后台广播一定投递，也不会替用户修改上述设置。
即使放宽限制，不同 ROM 仍可能拦截广播；应用被强制停止时无法收到拔线事件，恢复后也不会
补处理已经错过的事件。应解除后台限制并重新连接 USB，再进行播放、拔线测试。

已在 Redmi K20 Pro / Android 14 观察到更新后授权仍在、服务列表却为空，系统提示
`AutStart Unable to bind notification listener service`。只读检查命令和详细限制见
[手机端生效条件与后台限制](../../docs/ANDROID_COMPANION.md#手机端生效条件与后台限制)。

## 限制

- 需要 Android 10（API 29）或更高版本。
- 媒体功能首次使用必须由用户手动授予通知使用权，应用不能通过 ADB 静默授予该权限；文件分享和状态显示不需要此权限。
- 自启动与电池策略不得阻止通知监听服务运行；授权成功不代表后台运行条件已经满足。
- 0.1.1 修复 0.1.0 仅监听 accessory 拔出而漏掉 AOA HID-only 断开的情况。
  Redmi K20 Pro / Android 14 的只读证据已确认拔线时发出 `USB_STATE`，旧版没有暂停调用；
  新版仍需覆盖安装后实测拔线，不能把主机状态机测试当成真机验收。
- 0.1.2 将单个最高优先级播放器改为所有正在播放/缓冲的会话，版本码 3。
  主机回归同时覆盖多个播放器、单会话异常隔离和不干扰暂停会话；手机端仍需更新后实测。
- 0.1.3 按用户要求移除 1.5 秒去抖等待，版本码 4；收到断开广播即发送暂停请求。
  系统广播投递和播放器响应仍有自身耗时，不代表硬实时暂停。
- 监听所有 USB 数据连接，不限于 Deskflow；仅充电、BLE 断线不保证产生相同广播。
  重新枚举和快速拔插的短暂断开也会触发暂停；USB 插线不会撤销暂停或直接恢复播放。
- `USB_STATE` 是 Android 平台广播但常量不属于公开 SDK，因此以 action 字符串监听；
  其他 ROM 的广播投递、后台存活与播放器响应仍需实测。自动播放仅在用户明确开启后生效。
