# 音频转发（实验性）

音频功能沿用 scrcpy 的 Android Server 和音频流格式，不通过 AOA HID 传输。
AOA HID 继续负责键盘鼠标；ADB 独立负责音频：

```text
Android AudioRecord → scrcpy-server(raw PCM) → ADB forward → ALSA
```

当前实现使用 scrcpy 的：

- `audio_source=output`
- `audio_codec=raw`
- 48 kHz、双声道、16-bit PCM
- 64 字节设备元数据、4 字节 codec id、12 字节 PTS/flags + payload 帧格式

这样后续切换到 scrcpy 的 Opus 流时，不需要改变 ADB tunnel 和 Session 生命周期。

## 构建

Linux 需要 ALSA 开发包和一个与 Android Server 实现匹配的 `scrcpy-server`。
发布包会直接携带这个 server；运行时不需要安装或检测完整的 `scrcpy` 客户端。
当前实现按 scrcpy `v5.0.1` 的协议和参数实现：

```bash
sudo apt install libasound2-dev
python3 scripts/fetch_scrcpy_server.py \
  --output .cache/scrcpy-server-v5.0.1
cmake -S . -B build-audio \
  -DBUILD_AUDIO=ON \
  -DSCRCPY_SERVER="$PWD/.cache/scrcpy-server-v5.0.1" \
  -DSCRCPY_VERSION=5.0.1 \
  -DBUILD_GUI=ON
cmake --build build-audio
```

`fetch_scrcpy_server.py` 第一次运行时从官方 Release 下载并校验 server，之后优先使用
`.cache/scrcpy/scrcpy-server-v5.0.1` 缓存，不会重复下载。`SCRCPY_SERVER` 只负责在安装时复制 server；
开发运行时也可以直接通过命令行传入路径。

## CLI

```bash
./build-audio/deskflow-otg \
  --audio \
  --adb /usr/bin/adb \
  --scrcpy-server $HOME/.local/share/scrcpy/scrcpy-server \
  --scrcpy-version 5.0.1 \
  --serial <手机序列号> \
  --host 127.0.0.1 --port 24800 --name android \
  --width 1080 --height 1920
```

ADB、Server 或主机 ALSA 初始化失败会使本次连接失败；Android 运行期间如果音频
采集被系统禁用，则记录警告并保留键鼠连接。不带该参数时，原有无需 ADB 的 AOA
HID 行为不变。

要求 Android 11 或更高版本；Android 11 在设备锁定时可能无法启动采集。DRM、
通话以及主动禁止音频捕获的应用可能不会出现在输出中。

## 版本同步

`scrcpy-server` 必须来自与本实现相同的 scrcpy 版本。打包时通过
`SCRCPY_SERVER` 和 `SCRCPY_VERSION` 固定 server 及其版本，GUI 只读取包内的
server 和版本文件，不查找或执行系统 `scrcpy`。Server 会主动拒绝版本不匹配。
主机端只依赖 scrcpy 公开的音频 demux 格式，不复制 scrcpy 的私有客户端代码。升级 scrcpy 时应同时运行
音频头、RAW 帧、Android 11/13 设备和拔线清理测试。
