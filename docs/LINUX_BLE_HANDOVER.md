# Linux BLE HID 接入交接（2026-10-16）

## 接手摘要

分支：`feat/linux-ble-hid`。基于 `d11c432`（`main`）。本次为实验功能的本地提交，
未推送、未开 PR、未发布安装包；不要将其当作完成真机验收的正式版本。

**已有用户实测证据：Python 隔离入口 + 正式 C 桥接能操控手机，键盘正常；
鼠标曾延迟/拖尾。手动把 BLE 连接间隔从 45ms 缩短后，用户确认有改善。**

**最新代码已替换为无需 Python 的原生隔离入口，并修改了重连流程；
用户尚未反馈这套最新原生流程的实测结果。下一会话首先验证这一点，不要继续盲调鼠标频率。**

建议阅读顺序：本文件 → `README.md` 的 Linux BLE 章节 → `AGENTS.md` → 对应源码和测试。
项目计划“Linux BLE HID 功能接入与真机验证”仍未完成，不能把编译/mock 通过当作完成验收。

## Steps

- [x] 接入可替换 HID 输出接口，保留 USB 默认行为及共享报告编码。
- [x] 实现实验 Linux BLE 后端、事件调度、原生隔离/恢复入口及离线测试。
- [ ] 真机验证原生启动、正常退出恢复、异常退出恢复及重复启动。
- [ ] 不删除任何配对，验证手机连续断开/重连、程序退出/重启后的键鼠恢复。
- [ ] 验证原生低延迟参数请求与实际生效结果，再评估拖尾。
- [ ] 补齐目标手机身份/授权隔离、GUI、打包/CI及完整无线验收。

## 1. 用户当前诉求与证据边界

用户要求先解决：

1. 断开后经常需要取消两端配对，还要恢复蓝牙模式/重置手机或应用设置。
2. 不应依赖 Python 脚本作为启动入口。

对“重置配置”的澄清中，用户同时选择了“恢复电脑蓝牙模式”“删除两端配对”
及“重置手机或应用设置”；**尚没有更具体的手机设置操作记录**。不能声称所有这些现象
都已经找到唯一根因或已在真机修好。

### 已观察到

- Linux Mint、BlueZ 5.72、Intel AX210；测试手机为 Redmi K20 Pro，虚拟尺寸 1080×2340。
- 双模下 Android 会把电脑连接成音频设备。仅改广告名称、临时改 Adapter Alias 不能解决
  音频 Profile 选择问题；名称从 `Deskflow OTG` 变为电脑名称也不独立证明连接失败原因。
- 显式仅 BLE 隔离后，用户报告正式桥接键盘和鼠标都可用，但鼠标存在延迟和单光标拖尾，
  不是同时出现两个光标。
- 一次有效 btmon 抓包记录到连接间隔 **45ms**、peripheral latency=0；原约 30Hz 通知
  实际间隔约 36ms，控制器报告完成的中位等待约 62ms。后者不是手机显示延迟测量。
- 用户手动使用 `hcitool lecup` 请求 7.5～15ms 后报告“有改善”。当时未记录请求后最终
  实际间隔，因此不能直接把请求值写成已验证的手机最终参数。

### 尚未验证

- 最新 `--ble-isolate` 原生父进程：真实 MGMT 命令、runuser 降权/继承 FD、原始模式恢复。
- 最新 GATT 固定句柄、Device1 监控与 Deskflow 重连组合在该手机上是否免重配。
- 原生 HCI 监控是否收到加密/连接更新事件、IPC 就绪通知是否到达、请求是否被手机接受。
- 长时间稳定性、锁屏/唤醒、方向变化、全部按钮/拖拽/滚轮/修饰键、拔 USB 独立无线验收。
- GUI BLE 接入、三平台 GUI 回归、发布包依赖与 CI。

## 2. 本次代码结构

| 文件 | 职责 |
| --- | --- |
| `src/bridge.h/.c` | `otg_hid_transport_ops`，默认 AOA 适配器、共享描述符/报告编码、输入复位 |
| `src/ble_hid.h/.c` | GIO/BlueZ GATT、广告、HID 报告、Device1 在线/配对状态、坐标合并与 IPC |
| `src/ble_supervisor.h/.c` | 原生 root 隔离父进程，MGMT 快照/切换/核对恢复、runuser 子进程、锁 |
| `src/ble_link.h/.c` | root 父进程的 HCI 连接/加密/更新事件监控、低延迟请求及实际间隔通知 |
| `src/main.c` | 新 CLI 参数、BLE 等待与 Deskflow TCP 重连；USB 仍使用原有单次连接流程 |
| `src/deskflow_client.h/.c` | `on_tick`、可配置 poll 间隔，持续 TCP 流量期间也驱动 BLE 事件泵 |
| `scripts/ble_latency.py` | 历史 Python 隔离入口的参数监控实现，仅兼容/测试参考；原生入口不加载它 |

### 就绪与重连

- 不能只凭蓝牙 Connected 或 Deskflow DSOP 判断 BLE 输入可用。
- BlueZ 5.72 的 bonded peer 断线会保留 CCC 状态，**不一定调用 StopNotify**。
  此结论核对过上游 `src/gatt-database.c` 的 `att_disconnected()`，不是仅凭现象推测。
- 代码现在通过 BlueZ ObjectManager 缓存检查 `Device1.Connected` 与 `Paired`，并要求键盘、
  指针已订阅且未挂起。仅有一台已连接设备、且与本进程首次选中的 peer 相符时才就绪。
- 手机掉线：输入报告清零、取消待发送移动，断开 Deskflow TCP 让主机收回光标；
  **不注销 GATT、不退出隔离进程、不删除配对**。原手机就绪后重新连接 Deskflow。
- `Paired`/单连接门控和进程内 peer 绑定不等同于已完成持久目标白名单或多主机安全隔离，
  正式产品认证边界仍需审查和补齐。

### GATT 与调度

- 固定服务范围 `0x0100..0x0111`；BlueZ characteristic `Handle` 是 value handle。
- HID Report ID：键盘 1、绝对指针 2，通过 Report Reference 传递；Value 不重复带 ID。
- 新增键盘 LED 输出及 HID Control Point，Report Map 长读处理 offset；CCC 仍由 BlueZ 管理。
- 固定句柄冲突应失败，不得静默换成漂移的新句柄。旧版布局/手机缓存迁移仍需实测。
- 绝对指针仍是 Digitizer + Stylus + Tip Switch/In Range + 嵌套 Pointer Collection，
  不得为了降低延迟退回普通相对鼠标并声称无漂移。
- 旧实现只在约 500ms 空闲时发送最后一个待合并坐标，现使用 5ms BLE 事件泵，持续事件时
  也处理回调。只合并坐标；按键状态、滚轮、范围变化不丢弃。
- 默认移动约 30Hz；收到实测间隔后使用 `max(连接间隔, 16.67ms)` 的发送周期。

### 原生监督与恢复

- `--ble-isolate` 是显式、需 sudo 的 CLI 选项，可能断开该适配器上的全部蓝牙连接。
  默认 USB、普通 `--transport ble` 不会静默切换模式。GUI 没有新增 BLE 控件。
- root 父进程使用 Linux MGMT API，不调用 Python、btmgmt，不改配置文件、不重启 bluetoothd。
  使用固定的 `/usr/sbin/runuser` 以 sudo 发起者的普通用户身份运行自己的桥接子进程。
- `/run/deskflow-otg-ble-hciN.lock` 防并发；root 专用 `.state` 快照绑定适配器地址。
  已有快照不覆盖，恢复不成功不删除快照；上电有有限重试，最终重新读取标志核对。
- 正常退出/可捕获信号触发恢复。SIGKILL、断电不能自动清理；`/run` 不跨重启持久化。
- `--ble-restore` 仅识别原生 `.state`，不读取旧 Python 实验脚本的 JSON 文件。
- `src/ble_link.c` 仅对唯一、已加密、本机为 peripheral 的已观测 LE 链路请求 7.5～15ms。
  通过专用 Unix datagram FD 传递 `ready:hciN` 和实测间隔，不接受子进程指定 HCI 命令/句柄。
- Command Status 成功只表示请求被接受；必须看到 LE Connection Update Complete 才能确认
  新参数。拒绝或 5 秒未确认时保留链路，不无限重试。

## 3. 新会话启动与检查

先检查 `git status`、分支和已有 PR；本分支未发布。不要启动第二个隔离进程。
如果旧 Python 隔离入口仍在运行，先正常结束、等待它恢复完毕。不要直接删其恢复快照。

```bash
cmake -S . -B build -DBUILD_BLE=ON -DBUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure

sudo ./build/deskflow-otg --transport ble --ble-isolate --adapter hci0 \
  --host 127.0.0.1 --port 24800 --name android \
  --width 1080 --height 2340
```

只需要 USB、或系统没有 GIO 开发包时：`-DBUILD_BLE=OFF`，不会查找 pkg-config/GLib。
Linux 默认编译 BLE；非 Linux 默认关闭。BLE 开发依赖见 README。

原生异常恢复（需要用户在终端输入 sudo 密码）：

```bash
sudo ./build/deskflow-otg --ble-restore --adapter hci0
```

### 首轮真机验收顺序

1. 原生启动后确认没有 Python 进程作为入口，桥接子进程是普通用户，不是 root。
2. 记录 GATT/广告注册、手机配对/订阅/输入通道就绪日志。已有配对先直接尝试连接，
   **不要预先取消配对、信任陌生设备或重置手机网络设置**。
3. 保持程序运行，在手机侧断开并重连至少两次：电脑光标收回、手机重连、输入恢复、
   不回放断线前的按键和坐标；记录是否重新发 StartNotify，不将其作为唯一就绪依据。
4. 正常退出后确认模式、名称和原始电源状态已恢复，快照成功清理；再重复启动一次。
5. 程序正常重启后再次尝试原配对；若失败，先抓具体连接/加密/服务缓存错误，不把
   反复取消配对当作正式修复。
6. 再验证原生 HCI 参数实际更新日志与鼠标延迟。之前用户说“没打印出来”的情况指
   Python 参数监控阶段，新原生通路尚无真实反馈；优先检查 FD 继承与 ready 消息。

日志关键字：`BLE HID input channel ready`、`BLE service retained`、
`BLE requesting interval`、`BLE actual interval=... (controller event)`、
`original Bluetooth settings verified and restored`。

若需要抓包：保持桥接运行，另开终端 `sudo timeout --signal=INT 45s btmon -i hci0`，
在**手机蓝牙设置**断开重连，再用**电脑鼠标**滑入手机屏幕区域移动/停下。
不要输入文字/密码。抓包可能包含设备身份及配对信息，只保存在本地临时目录，不公开提交。
广告 interval 不是 connection interval，不要混淆。

## 4. 本会话完成的自动验证

- 默认 BLE 构建：8 项 CTest 全通过。
- `BUILD_BLE=OFF` USB-only 构建：3 项 CTest 全通过。
- 独立 ASan/UBSan 构建：8 项 CTest 全通过。
- 核心源码 `-std=c11 -Wall -Wextra -Werror` 检查、USB-only main 检查、`git diff --check` 通过。
- 未运行最新原生模式的真实 root MGMT/HCI 操作；未做 GUI 构建/窗口/托盘回归；未做发布打包。

| 测试 | 覆盖 |
| --- | --- |
| `bridge_transport` | stub AOA 的默认 USB 描述符、键盘/绝对坐标/按钮/滚轮、生命周期 |
| `ble_pointer` | 移动合并、拖拽/松键/范围/滚轮屏障、实际 IPC、自适应节奏、保留 CCC 的重连状态 |
| `ble_supervisor` | mock MGMT 隔离/恢复、重复启动状态、上电重试、失败不伪称成功、适配器校验 |
| `ble_link` | 匿名 HCI 夹具，实际更新确认、拒绝、重连换句柄、多链路不自动选择 |
| `ble_gatt` | 私有 D-Bus 的真实方法分发、稳定句柄、长读 offset、LED/Control Point；不访问系统蓝牙 |
| `ble_latency` | 历史 Python 参数监控的离线匿名事件测试；Python 只用于此可选测试，不是运行时依赖 |
| `socket_io` | 原包帧、EOF/SIGPIPE/取消及持续 TCP 数据期间的事件泵 |
| `cli_version` | CLI 版本输出 |

本地验证目录 `/tmp/opencode/deskflow-usb-only`、`/tmp/opencode/deskflow-ble-asan`，
临时抓包 `/tmp/opencode/ble-pointer.log` 可能清理，不是永久资产，不能作为唯一证据。
提交会改变开发版 Git 版本号，接手后重新运行 CMake 更新 generated version。

## 5. 仓库外资料与剩余工作

原始交接/原型目录：`../Deskflow BT Test/`（含空格）。本会话此前还修改了其中的
`ble_isolation.py`、`test_ble_isolation.py`、`README.md`：修复 `secure-conn` 应调用 `sc`、
支持 `--child` 并尝试 Python HCI 监控。这些文件**不属于本仓库提交**。
新原生路径不依赖它们；不要误以为切换分支会带走或回滚这些仓库外文件。

下一会话不要默认宣布重连、恢复、授权隔离或低延迟全部完成。当前优先级仍是用户提出的
“免反复重配/重置 + 原生启动”，其次才是延迟、GUI、打包与完整产品化。
