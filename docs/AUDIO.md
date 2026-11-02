# 音频转发（实验性）

音频功能沿用 scrcpy 的 Android Server 和音频流格式，不通过 AOA HID 传输。
AOA HID 继续负责键盘鼠标；ADB 独立负责音频：

```text
Android AudioRecord → scrcpy-server(raw PCM) → ADB forward → ALSA/WASAPI
```

ADB 与 USB HID 可以使用不同的传输。`--serial` 始终是 USB AOA HID 的物理设备序列号；
`--adb-serial` 可单独指定音频使用的 ADB transport，例如无线调试地址
`192.168.1.20:5555`。这样 Windows 不需要同时为同一条 USB 连接提供 ADB 和 WinUSB 驱动。
先完成 Android 无线调试配对（如需要，可使用 `adb pair`），再填写手机无线调试主页的
IP 和**连接端口**，而非配对弹窗的端口。Linux、Windows、macOS 的共享会话会对显式
`主机:端口`（IPv6 为 `[地址]:端口`）先检查 `adb -s ... get-state`；未就绪时自动执行一次
`adb connect` 并再次确认状态为 `device`，然后才推送 server。已在线则不重复连接。
连接/授权失败会提示检查端口、网络和配对；不自动配对、不回退到另一台 USB 设备，
退出也不执行 `adb disconnect` 或重启 ADB server。macOS 当前仅提供剪贴板，不提供音频播放。

主机端由共享的 `scrcpy_session` 管理 ADB、scrcpy-server、forward 以及音频/控制
socket 生命周期；`audio_player` 只负责将 RAW PCM 播放到 ALSA/WASAPI。这样只启用
剪贴板时不依赖音频后端，音频和剪贴板组合启用时仍只启动一个 Android Server。

当前实现使用 scrcpy 的：

- `audio_source=output`
- `audio_codec=raw`
- 48 kHz、双声道、16-bit PCM
- 关闭设备元数据；首 socket 先读 1 字节 dummy，再读 4 字节 codec id、12 字节 PTS/flags + payload 帧

这样后续切换到 scrcpy 的 Opus 流时，不需要改变 ADB tunnel 和 Session 生命周期。
可与[文本剪贴板共享](CLIPBOARD.md)独立或组合启用；两者复用一个 Android Server 和
ADB forward，音频和控制使用独立 socket。控制 socket 必须在等待读取音频头之前连接。

## 构建

Linux 需要 ALSA 开发包，Windows 使用系统 WASAPI；两者都需要一个与 Android Server
实现匹配的 `scrcpy-server`。
发布包会直接携带这个 server；运行时不需要安装或检测完整的 `scrcpy` 客户端。
当前实现按 scrcpy `v5.0.1` 的协议和参数实现：

```bash
sudo apt install libasound2-dev
cmake -S . -B build-audio \
  -DBUILD_AUDIO=ON \
  -DBUILD_GUI=ON
cmake --build build-audio
```

Windows 使用 Visual Studio 生成器时同样启用 `-DBUILD_AUDIO=ON`，不需要额外音频库：
WASAPI 由 Windows SDK 提供。ADB 可以是 PATH 中的 `adb.exe`，或 GUI 随包携带的版本。

GUI 在“设备配置”页提供“转发安卓音频”开关，与“共享文本”一样按设备独立保存，
新设备默认关闭；连接期间仅锁定当前设备的开关，不影响其他空闲设备。当前 GUI 音频仍仅支持
Linux/Windows 的 USB 键鼠连接模式，未编译音频功能时不显示开关。
勾选音频后，可在同页的“音频/剪贴板 ADB 序列号”字段填写无线 ADB 序列号；
USB serial 仍填写 `--usb-scan` 得到的物理 USB 序列号。该字段留空时，音频回退使用 USB serial。

仓库已经内置并打包 `third_party/scrcpy/scrcpy-server-v5.0.1`，不需要联网下载。
`fetch_scrcpy_server.py` 仍然保留，用于以后升级 server；它会从官方 Release 下载、校验 SHA-256，
并使用 `.cache/scrcpy/` 缓存。升级后需要同步修改 CMake 中的版本和文件名，然后提交新的 server 文件。
也可以通过 `SCRCPY_SERVER` 和 `SCRCPY_VERSION` 覆盖默认值。

## CLI

```bash
./build-audio/deskflow-otg \
  --audio \
  --adb /usr/bin/adb \
  --adb-serial 192.168.1.20:5555 \
  --scrcpy-server $HOME/.local/share/scrcpy/scrcpy-server \
  --scrcpy-version 5.0.1 \
  --serial <手机序列号> \
  --host 127.0.0.1 --port 24800 --name android \
  --width 1080 --height 1920
```

Windows CLI 示例：

```powershell
.\build-audio\Release\deskflow-otg.exe --audio --adb .\platform-tools\adb.exe `
  --adb-serial 192.168.1.20:5555 `
  --scrcpy-server .\scrcpy-server --scrcpy-version 5.0.1 `
  --serial <手机序列号> --host 127.0.0.1 --port 24800 --name android `
  --width 1080 --height 1920
```

ADB、Server 或主机 ALSA/WASAPI 初始化失败会使本次连接失败；Android 运行期间如果音频
采集被系统禁用，则记录警告并保留键鼠连接。不带该参数时，原有无需 ADB 的 AOA
HID 行为不变。

### 断开时暂停播放

启用音频且会话启动成功后，GUI 断开/退出、CLI 正常退出或 Deskflow 断线触发清理时，
会在关闭音频流前，对同一 `--adb-serial`（未指定时使用 USB serial）执行一次：

```text
adb -s <音频设备> shell cmd media_session dispatch pause
```

该命令由 Android `MediaSessionService` 分发明确的 `KEYCODE_MEDIA_PAUSE`，不经过
`InputManager.injectInputEvent`，因此不依赖「USB 调试（安全设置）」提供的 `INJECT_EVENTS`
注入权限。普通 USB 调试或已授权的无线 ADB 仍是音频的前提。旧版 scrcpy control socket
注入同样需要 `INJECT_EVENTS`，不能绕过这项限制。

使用明确的“暂停”而非“播放/暂停切换”，不会主动恢复播放，重新连接也不会自动播放。
仅共享剪贴板、未启用音频或音频会话启动失败时不发送。纯音频会话不创建 control socket；
共享剪贴板仍使用独立 control socket，辅助 server 禁用自动按电源键唤醒（`power_on=false`）。

等待暂停命令完成后再关闭音频流，命令最多等待 2 秒；失败或超时记录警告并继续清理，
不重连 ADB、不切换目标设备。日志“pause request sent”仅表示命令成功返回，
不是播放器已暂停的确认。这不是模拟耳机插拔，也不修改音量；具体播放器/ROM 仍可能
不响应媒体会话暂停请求。
USB 拔出、无线 ADB 已掉线或进程被强杀时不能保证暂停，不能保证完全避免手机短暂外放。
Linux/Windows 共用此行为；macOS 当前不支持音频，不受影响。自动化验证使用 fake ADB，
本次 Linux GUI 测试构建已由用户实测确认断开暂停正常；其他播放器/ROM 和 Windows
仍需分别验证。

要求 Android 11 或更高版本；Android 11 在设备锁定时可能无法启动采集。DRM、
通话以及主动禁止音频捕获的应用可能不会出现在输出中。

## 版本同步

`scrcpy-server` 必须来自与本实现相同的 scrcpy 版本。打包时通过
`SCRCPY_SERVER` 和 `SCRCPY_VERSION` 固定 server 及其版本，GUI 只读取包内的
server 和版本文件，不查找或执行系统 `scrcpy`。Server 会主动拒绝版本不匹配。
主机端只依赖 scrcpy 公开的音频 demux 格式，不复制 scrcpy 的私有客户端代码。升级 scrcpy 时应同时运行
音频头、RAW 帧、Android 11/13 设备和拔线清理测试。
