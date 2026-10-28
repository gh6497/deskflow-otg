# Linux BLE HID 接入交接（2026-10-16）

## 接手摘要

**续接更新：CLI/GUI 与 DEB 接入已落地，新增要求及验证范围见第 8 节。旧命令中的 BLE 桥接
现需添加 `--ble-peer`；IPC 已升至 v2，不能直接复用本文件第 7 节仍运行的旧版服务。不得擅自
停止用户连接。以下历史记录保留当时的原始事实，不作为新版 GUI 已完成真机验收的证据。**

分支：`feat/linux-ble-hid`。基于 `d11c432`（`main`）。实验功能包含本地实现及本轮续测记录，
未推送、未开 PR、未发布安装包；不要将其当作完成真机验收的正式版本。

**已有用户实测证据：Python 隔离入口 + 正式 C 桥接能操控手机，键盘正常；
鼠标曾延迟/拖尾。手动把 BLE 连接间隔从 45ms 缩短后，用户确认有改善。**

**续测已验证：原生隔离启动、常驻 GATT 下桥接会话正常重启、两次手机断线重连、
普通桥接子进程 SIGKILL 后自动恢复及再次接入均通过；实际连接间隔确认降至 12.5～15ms。
首次迁移卡在 Android HID 连接中，保留配对切手机蓝牙开关一次后才恢复，不得隐去此边界。
常驻服务/BlueZ/主机重启、长时间与完整输入验收仍未完成，详见第 6、7 节。**

建议阅读顺序：本文件 → `README.md` 的 Linux BLE 章节 → `AGENTS.md` → 对应源码和测试。
项目计划“Linux BLE HID 功能接入与真机验证”仍未完成，不能把编译/mock 通过当作完成验收。

## Steps

- [x] 接入可替换 HID 输出接口，保留 USB 默认行为及共享报告编码。
- [x] 实现实验 Linux BLE 后端、事件调度、原生隔离/恢复入口及离线测试。
- [x] 真机验证新常驻路径原生启动、桥接正常退出恢复/重启、桥接子进程 SIGKILL 恢复与再次接入。
- [x] 常驻服务保持运行，保留配对验证两次手机断线重连及桥接会话重启，鼠标控制恢复。
- [x] 原生低延迟请求与 controller event/IPC 回传确认，实际 12.5～15ms。
- [ ] 首次缓存迁移无需切手机蓝牙、常驻服务/BlueZ/主机重启后的免重配恢复。
- [ ] 新路径完整键盘/修饰键、拖拽/滚轮、持键异常退出、拖尾和长时间稳定性验收。
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

### 当前尚未验证

- 常驻服务、BlueZ/主机重启后恢复；首次旧缓存迁移无需人工切手机蓝牙。
- root supervisor 自身 SIGKILL、服务崩溃/硬件卡死的恢复，跨重启快照持久化。
- 不同手机/BlueZ/内核组合，目标白名单与多主机安全隔离。
- 长时间稳定性、锁屏/唤醒、方向变化、全部按钮/拖拽/滚轮/修饰键、拔 USB 独立无线验收。
- GUI BLE 接入、三平台 GUI 回归、发布包依赖与 CI。

## 2. 本次代码结构

| 文件 | 职责 |
| --- | --- |
| `src/bridge.h/.c` | `otg_hid_transport_ops`，默认 AOA 适配器、共享描述符/报告编码、输入复位 |
| `platform/linux/ble_hid.h/.c` | GIO/BlueZ GATT、广告、HID 报告、Device1 在线/配对状态、坐标合并与 IPC |
| `platform/linux/ble_supervisor.h/.c` | 原生 root 隔离父进程，MGMT 快照/切换/核对恢复、runuser 子进程、锁 |
| `platform/linux/ble_link.h/.c` | root 父进程的 HCI 连接/加密/更新事件监控、低延迟请求及实际间隔通知 |
| `platform/linux/ble_session.h/.c` | 普通用户常驻 GATT 服务、私有 Unix IPC 会话、EOF/lease 清理及空闲状态检查 |
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
- 正常退出/可捕获信号触发恢复。普通桥接子进程 SIGKILL 后，存活的常驻服务/父进程可清理；
  root supervisor 自身 SIGKILL、断电不能自动清理；`/run` 不跨重启持久化。
- `--ble-restore` 仅识别原生 `.state`，不读取旧 Python 实验脚本的 JSON 文件。
- `platform/linux/ble_link.c` 仅对唯一、已加密、本机为 peripheral 的已观测 LE 链路请求 7.5～15ms。
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

## 6. 续测记录（2026-10-16，尚未验收）

- 已观察原生进程树：root supervisor → `/usr/sbin/runuser -u len` → 普通用户桥接；
  未见 Python 隔离入口。此证据证明降权入口运行过，不代表全部恢复/重连功能通过。
- 用户报告：首次 CLI 启动后手机连接、断开可用；关闭 CLI 再启动，手机显示已连接，
  但没有两路 `report subscribed` 和 `BLE HID input channel ready` 日志。
  尚无此次重启的 ATT/加密抓包，不能认定缺少日志的唯一原因。
- 退出时 MGMT `0x0005`（Set Powered）多次超时；显式 `--ble-restore` 失败，
  `bluetoothctl power on` 也报 `org.bluez.Error.Failed`。当时适配器 DOWN、无 rfkill 阻断，
  内核反复记录 HCI `Opcode 0x0c03 failed: -110`（Reset 超时）及 MSFT 扩展错误。
  恢复失败保留了 root 快照；控制器超时的触发原因尚未确认，不应归因于手机配对。
- 随后主机重启，观察到 `hci0` Powered=yes、Alias=`len-mint`，手机 Paired/Bonded=yes；
  `/run` 快照已不存在。重启后的可用状态不能计为原生自动恢复成功，也无法据此核对旧快照。
- 修正确定的 MGMT 解析错误：Command Complete 和 Command Status 的 payload 均为
  `opcode LE16 + status u8`；旧实现错误地把 Command Status 当成另一种字段排列，
  忽略内核错误后只报告超时。新增 Unix datagram 原始报文测试，不触碰真实蓝牙。
  **此修复不等于解决控制器 HCI Reset 超时。**
- 撤回曾短暂加入的“已配对且已连接即推断两路 CCC 保留”逻辑。核对 BlueZ 5.72
  `src/gatt-database.c`：`att_disconnected()` 对 bonded peer 可保留 CCC，但
  `gatt_db_service_removed()` 调用 `remove_device_ccc()` 清除注销服务的 CCC。
  保持服务的断线重连和注销服务后的进程重启不能混为一谈；没有真实订阅时不能伪造 ready。

下一轮先保留现有配对，从原生启动前开始同时记录 CLI 与本地 `btmon`，依次观察
连接角色、加密、ATT 服务发现/CCC 写入、StartNotify 和 Deskflow 就绪，再正常退出。
出现 HCI Reset/MGMT 超时就停止反复启动，不删除恢复快照；不得自动重启 bluetoothd、
重载驱动或复位 USB 硬件。硬件恢复需用户确认，主机重启前如需保留原始状态，须将 root
快照安全备份到持久位置（不可公开提交）。现有 USB GUI 会话不要为 BLE 测试擅自终止。

### 后续抓包与 Android 侧证据

- 原生启动日志仍停在等待订阅，且报告 `native BLE timing monitor unavailable`。
  首轮抓包看到了 HID 服务/特征/CCC 枚举和成功加密，但无键鼠 CCC 写入。
  抓包中有 Unpair Device，用户确认因无法连接而删除过配对，因此首轮不能计为免重配成功。
- 用户关闭再开启手机“输入设备”后，CLI 依旧没有订阅日志。新抓包显示手机主动断开，
  随后以 peripheral 角色的电脑建立连接，复用已有 LTK、成功启用 AES-CCM；只看到
  Service Changed 的 CCC `0x000b=0200`，没有 HID `0x0107/0x010b` 的写入。
  **链路/密钥复用成功不等于 HID 输入恢复。**
- 经已授权 USB ADB 只读诊断，实测系统为 Android 14/API 34（不要由手机型号推断系统版本）。
  Android Bluetooth 日志在 14:34:01 报 HID Host 连接成功，使用 164 字节的描述符创建 UHID；
  `dumpsys input` 存在蓝牙 `Deskflow OTG`，Sources 为 KEYBOARD/MOUSE/STYLUS。
  与抓包对照表明 Android 使用已有 HID 状态，但电脑缺少实际订阅；缓存/服务生命周期不一致
  是待验证方向，尚不能宣布唯一根因或将“手机已连接”视为恢复成功。
- 修正独立的 HCI filter ABI 问题：原代码 `setsockopt(HCI_FILTER)` 写死长度 14，
  而当前内核的 `copy_safe_from_sockptr()` 要求 native 结构大小（本机含 padding 为 16）。
  改为结构体及 `sizeof`，并输出 socket/bind/filter 的具体 errno；新增 mock 验证 filter
  布局及各阶段失败清理。此次构建、8 项 CTest 和修改源码严格编译检查通过。
  正在运行的进程仍为旧构建，监控修复尚未真机验证，也不解决缺失 HID CCC。
- 本地证据在 `/tmp/opencode/ble-native-review/run-20261016-01/`。用户第二次抓包复用了
  `btmon.log` 并覆盖首轮；保留下来的文件仅代表开关输入设备这一次，不可冒充完整启动抓包。
  抓包含配对密钥，仅本地使用，不得提交或公开。后续抓包使用新文件名，不再覆盖。

## 7. 常驻 GATT / 会话分离实现（待切换真机）

用户批准按服务生命周期分离方向实施。新增 `platform/linux/ble_session.c/.h`：

- `--ble-service`：普通用户前台服务，不隐式安装/后台运行；首次会话才创建 BlueZ GATT，
  空闲时不广告、不修改名称、不发送输入，但保留应用及真实订阅状态。
- `--transport ble --ble-session`：通过同 UID 的私有 Unix seqpacket 接入；可叠加原生
  `--ble-isolate`。只有 root supervisor 切换/恢复模式，常驻服务永不运行 MGMT/HCI 命令。
- 目录 0700、socket 0600；SO_PEERCRED 核对双方 UID、版本/包长校验、单会话占用，
  root 仅可读取 idle 状态，不可接管会话发送输入。旧 socket 在持有本适配器锁后才清理。
- 会话正常关闭、IPC EOF 或 5 秒无 IPC 时：松键/结束范围、清除待发移动、停广告、恢复名称；
  不注销 GATT。新会话描述符必须完全一致，目前限定 absolute 模式，拒绝静默换 schema。
- supervisor 在隔离前要求服务空闲，桥接退出后再次等待清理确认再尝试恢复；无法确认
  时不继续切换模式、保留快照供停止会话后显式恢复，不宣称完整恢复。手机掉线时丢弃来自
  旧 ready 状态的报告，不回放旧输入。
- 真实 ready/实测 interval 在会话与 root 监督器之间转发；不凭 Connected 伪造两路 CCC。
  停止常驻服务或 BlueZ/主机重启后仍会丢失连续性，既有缺失订阅不保证自动迁移。

测试新增 `ble_session`（真实 socket/fork、fake HID、正常/EOF/lease、schema/busy/UID/报文校验），
扩展 `ble_gatt`（私有 D-Bus 上真实方法调用与 mock BlueZ 管理器、跨会话不注销应用、停止广告、
松键、失败不伪称 idle）。普通 BLE 构建 9 项、ASan/UBSan 9 项、USB-only 3 项通过；
修改源码 `-Wall -Wextra -Werror` 和 diff 检查通过。

**尚未启动新常驻服务或停止旧真机实例；上述测试不是硬件验收。** 下一轮需用户在有抓包的
条件下先正常结束旧实例并确认原生恢复成功，再按 README 的两个原生进程命令切换。
不能在旧 GATT 实例仍注册时并行试新服务；不能因旧 CCC 缺失而宣称服务分离已经解决全部重启。

### 切换进度

用户随后正常关闭了旧 CLI。`/tmp/opencode/ble-native-review/exit-20261016-145719.log`
中 HCI Reset、Set BR/EDR、Set SSP、Set Powered 均成功；主机观察 Powered=yes、
Alias=`len-mint`、ActiveInstances=0、`.state` 已清理。这是本轮旧实例退出恢复成功的证据，
不代表此前 AX210 超时已修复，也不等同于新常驻路径验收。

已启动普通用户 `--ble-service --adapter hci0`，确认仅处于监听/空闲状态，目录 0700、
socket 0600；日志 `/tmp/opencode/ble-native-review/resident-aEMphpoG/service.log`。
新桥接会话尚未启动，等待用户在终端 sudo 启动 `--ble-session --ble-isolate` 并抓包。

随后用户启动首个新会话并点击手机已有配对设备：电脑 Connected=yes，手机显示“已保存”。
`resident-aEMphpoG/start.log` 表明已有 LTK 回复成功、AES-CCM 加密成功、HID 服务/特征/CCC
枚举结束，但没有 HID Report Map 读取和两路 HID CCC 写入；服务日志无 StartNotify。
Android `dumpsys bluetooth_manager` 的 HidHostService 对电脑为状态 1（CONNECTING），
不是 CONNECTED。15:00:29 的连接请求后未见 HID open complete；随后多次点击被
`Hid Device not disconnected` 拒绝。日志还出现 GATT cache file 缺失、服务变化 indication
和 GATT service discovery finished，尚不能据此确定唯一根因。
新会话已接入常驻服务，且无此前 timing monitor unavailable 警告；未就绪前没有调参请求
符合设计，但尚未验证新监控的实测参数回传。不得把当前首会话卡住记为方案已免重配通过。

用户在保留配对、常驻服务和会话的条件下，将手机蓝牙总开关关闭再开启一次，随后报告
自动连接且手机光标出现。15:05 左右核对：服务日志有真实 keyboard/pointer subscribed、
HID input channel ready；会话日志有 Deskflow Barrier 1.8 握手及 enter/leave 事件。
Android HidHostService 为状态 2（CONNECTED），主机 Paired/Bonded/Connected 均 yes。
原生 HCI 监控记录实际 45ms → 发起 7.5～15ms 请求 → controller event 确认实际 **15ms**、
latency=0；服务收到 interval IPC，指针 pacing=16.67ms。这次真实链路验证了 HCI filter
修正后的监控及参数回传通路，不是端到端显示延迟测量。

该次需要手机蓝牙开关来离开此前卡住的状态，因此只计为一次保留配对的初始化恢复，
不计为日常重连/桥接重启免重配通过。接下来保持常驻服务，验证仅结束/重新启动桥接会话
后无需再操作蓝牙总开关或删除配对，并继续验证两次手机侧断线重连；键盘、拖拽、滚轮及
异常退出释放输入仍需用户实测，不能仅从订阅和光标出现推断全部通过。

用户随后正常结束新路径的第一个桥接会话。15:07 核对：桥接/supervisor 已退出，常驻服务
PID 23757 保持运行；服务记录 input channel disconnected、名称恢复和 session idle/GATT
retained。适配器 Powered=yes、Alias=`len-mint`、ActiveInstances=0，HID UUID 仍在；
`.state` 已清理。会话日志经普通 `tee` 的管道缺少 Ctrl-C 后的恢复尾部输出，后续改用
`tee -i`，以免把日志管道随 SIGINT 中断。接下来启动第二个会话，不再切手机蓝牙或删配对。

15:10，用户确认第二个会话可连接、鼠标恢复。核对 `session-2.log`：input channel ready、
Deskflow Barrier 1.8 接入及 enter/leave；实际间隔 controller event 为 45ms → 请求 →
**12.5ms**、latency=0，服务收到 interval IPC、pacing=16.67ms。Android HID Host 状态 2，
主机 Paired/Bonded/Connected=yes；常驻服务仍为同一 PID 23757。服务日志只有首次会话的
两条 subscribed，第二会话直接由 retained 状态恢复 ready，未伪造新的 StartNotify。

据用户反馈，本次未删配对、未再次切手机蓝牙总开关：可计为**一次常驻服务不退出、桥接
会话正常结束/重启后的免重配恢复成功**。不能扩大为常驻服务自身重启、主机/BlueZ 重启
或异常退出已验收。下一步保持第二会话运行，手机侧断开/重连至少两次，再验证异常退出
和键盘/拖拽/滚轮；这些仍未完成。

15:13，用户确认保持第二会话和常驻服务运行、手机侧连续断开/重连两次均正常。
`session-2.log` 有两组 resident input channel disconnected → service retained → ready →
重新 Barrier 1.8 握手，随后均有 enter/leave；每次 root HCI 监控确认实际间隔
37.5ms → 请求 → **12.5ms**、latency=0。服务日志亦有对应两组 disconnected/ready，
没有新增 subscribed；常驻服务 PID 23757、桥接 PID 29508 均未重启，原配对保留。

可计为**两次手机侧断线重连免重配、Deskflow 自动重接及原生调参重新生效通过**。
鼠标控制恢复由用户确认；不得据此宣称所有键盘/拖拽/滚轮、锁屏和异常恢复通过。
下一项异常退出测试应先征得用户确认：鼠标先回电脑，只 SIGKILL 普通用户桥接子进程，
保留 root supervisor 和常驻 GATT 服务，检查 EOF 松键/停广告/名称及原模式恢复和快照清理。
不要直接 SIGKILL root supervisor（那属于不能自动清理的另一种故障边界）。

15:16，用户明确批准执行上述异常测试。通过 pidfd 固定 PID 29508，并核对 UID=1000、
可执行路径、`--ble-session` 且无 `--ble-isolate/--ble-service` 后，只 SIGKILL 该普通用户
桥接进程，未向 root supervisor/runuser/常驻服务发信号。服务日志新增 disconnected、
名称恢复、session idle/GATT retained；`session-2.log` 尾部完整记录“已杀死”及
`original Bluetooth settings verified and restored; pairing records retained`。
独立核对：桥接/runuser/root supervisor 均退出，常驻 PID 23757 保留；Powered=yes、
Alias=`len-mint`、ActiveInstances=0、`.state` 已清理。

可计为**一次普通桥接子进程 SIGKILL 后，服务 EOF 清理及 root 自动恢复通过**。
测试前鼠标已离开手机、没有故意按住键，因此未验证真机“按住按键/拖拽时强杀”的释放效果；
该场景仅离线测试覆盖。root supervisor 自身 SIGKILL、常驻服务崩溃、断电等仍不计通过。
接下来保留常驻服务，启动第三会话验证异常恢复后也能免重配重新控制手机。

15:17，用户确认第三会话可正常连接。`session-3.log` 有 resident ready、Deskflow Barrier 1.8
握手及两组 enter/leave；controller event 确认 37.5ms → 请求 → **12.5ms**、latency=0，
服务收到 interval IPC。主机 Paired/Bonded/Connected=yes，Android HID Host 状态 2，
常驻服务仍为 PID 23757，服务日志没有新增 StartNotify。可计为**上述桥接子进程 SIGKILL
恢复后再次启动会话免重配重新接入通过**；不扩大到常驻服务/BlueZ/主机重启。
本条验证记录结束时第三会话与常驻服务仍运行，未擅自结束用户正在使用的连接；
当时尚未提交本轮修改，未推送、未发布。后续提交状态以 Git 历史为准。

## 8. Linux CLI / GUI / 安装包接入（2026-10-16）

用户确认蓝牙具备 OTG 同等的操控能力，并批准按 Linux CLI + GUI + 安装包范围接入。
没有新增独立可执行文件：同一个 `deskflow-otg` 负责服务和会话两种角色。

- BLE 桥接新增必填 `--ble-peer AA:BB:CC:DD:EE:FF`（BlueZ 身份地址）。服务生命周期内
  固定该目标，换手机须显式停止空闲服务。输入仍须目标已配对、唯一在线、真实两路订阅。
  多连接失败关闭输入；这不是任意多主机环境的强隔离保证，推荐专用适配器。
- GATT 方法核对 BlueZ 唯一 D-Bus owner；键鼠 Value 信号定向 BlueZ，不在系统总线上广播。
  输入 ReadValue / LED / Control Point 校验 BlueZ 提供的目标 device。
- IPC 升至 v2，新增目标绑定、只读 `--ble-status` 和同 UID 空闲服务停止
  `--ble-stop-service`。不隐式停止/替换旧服务，不用 PID 猜测进程归属。
- Linux BLE GUI 提供传输、适配器、目标地址、仅 BLE 选项和停止空闲服务；保存设置、隐藏
  无关 USB/ADB 控件、暂停 USB 自动连接。服务不存在时后台启动，已有空闲服务则复用；
  会话断开和 GUI 退出保留常驻服务。手机/Deskflow 断线后等待重连，不把旧连接状态留成已连接。
- GUI 隔离须确认该适配器所有连接中断，再经系统 pkexec 认证；监督器支持 PKEXEC_UID，
  桥接继续降权。stdin 继承给子进程，取消/GUI EOF 仍由桥接优雅退出，监督器负责恢复。
  认证等待期间取消会在切换控制器前检查 stdin；GUI 不强杀特权监督器。
- BUILD_FEATURES.json 随安装树提供；DEB 按 BLE 功能声明 BlueZ/GLib/pkexec 依赖，增加 CLI
  入口及可选的用户 systemd 单元。安装不自动启动/启用单元，不重启 bluetoothd。
  CLI 安装 RPATH 支持在不依赖 LD_LIBRARY_PATH 的情况下运行。AppImage FUSE 提权边界见 README。
- CI 补充 GLib/BlueZ 和 USB-only GUI 回归；补充 GLib 许可证来源。

本轮验证（全部离线，不访问系统蓝牙、不停止第 7 节的用户服务）：

- Qt 6.8.3 + BLE + GUI，`-Wall -Wextra -Werror` 构建通过；14 项 CTest 通过。
- BUILD_BLE=OFF + GUI，严格构建和 8 项 CTest 通过。
- BLE CLI 的 ASan/UBSan 构建与 10 项 CTest 通过。
- 9 项 Python 打包测试通过，含实际构建/解包 BLE 与 USB-only DEB 的依赖、CLI 入口和单元检查。
- 私有 D-Bus 模拟 BlueZ 验证目标匹配、未配对、非目标/多连接关闭输入及非 BlueZ 调用拒绝；
  真实私有 seqpacket 验证目标绑定、占用时拒绝停止、空闲停止和旧版协议拒绝。
- Qt 模拟子进程验证 BLE 参数（不传 USB serial）、断线/重连、忙碌拒绝、取消、配置恢复、无 ADB
  及切回 USB。`/tmp/opencode/ble-product-gui.png` 为模拟界面截图，不是真实手机连接截图。

开发构建：`/tmp/opencode/ble-product-build`；USB-only：`/tmp/opencode/ble-product-usb`；
sanitizer：`/tmp/opencode/ble-product-asan`。本地 DEB 在 `/tmp/opencode/deskflow-otg-ble-linux-amd64.deb`，
尚未安装/发布，构建版本为 0.1.4 的 tracked-dirty 开发版；源码提交状态以 Git 为准。

**下一步仍须用户批准后切换旧服务，再真机验证新版 GUI 启动、桌面认证/取消、会话退出恢复、
目标地址门控和无线输入；常驻服务/BlueZ/系统重启、持键强杀、锁屏及长时间输入未因本次
GUI/打包接入而变成已验收。不要擅自停止旧连接、切模式或删除配对。**

## 9. GUI 无光标诊断与身份路径修复（2026-10-16）

用户报告仅改名、后来显示输入设备但无光标。只读检查发现：起初 GUI 未开启隔离；用户改用
隔离后，旧服务记录了键盘/指针两路订阅，但 ready=false。BlueZ 同时存在一个以历史随机地址
命名的在线 Device1（Address 已是配置的身份地址，Paired/Bonded=false），以及同身份的离线
已配对对象。旧实现把身份 MAC 直接拼为对象路径，与 BlueZ 隐私地址对象生命周期不兼容。

修复：固定保存身份地址，按在线 Device1.Address 比较，再记录当前认证对象路径；路径可以
在重连后变化。仍须该在线对象自身 Paired=true，不借用同身份离线对象的配对状态；GATT
device 参数也必须指向当前认证对象。增加去重的 BLE waiting 日志，区分未连接、身份不符、
未配对、多连接、缺订阅和挂起。未移除任何配对/订阅/唯一连接门控，也未改 GATT schema/IPC。

严格构建及 10 项 CTest、ASan/UBSan 10 项通过。私有 D-Bus 回归覆盖身份 Address 与随机路径
不同但在线已配对时就绪，以及在线未配对/另有离线已配对同身份对象时继续拒绝输入。

用户明确批准正常重启会话/服务以验证。经 pidfd 核对只向普通桥接发送 SIGINT，监督器正常
退出、恢复快照已删除；再通过同 UID 空闲停止请求结束旧服务。未强杀 root、未删除配对。
修正版常驻服务从 `/tmp/opencode/ble-identity-build/deskflow-otg` 启动，PID 64403；
日志 `/tmp/opencode/ble-identity-run/service.log`，不是原 GUI 自动后台服务日志。
原安装包 GUI/桥接与修正版服务保持 IPC v2 兼容，已重新启动隔离会话。

重连后观察到正确身份对象 Connected/Paired/Bonded=true，但新服务尚未收到两路订阅，
日志为 `waiting for both HID subscriptions`。已请用户保留配对，切换手机“输入设备”或仅对
该设备断开重连。不能据此宣布光标恢复；服务重新注册后的手机缓存恢复仍需确认。
另发现当时 GUI 会话指向 localhost:24801，而只读监听检查只有 :24800，需要用户确认/修正。

此时运行的是临时路径的修正版服务，尚未替换 `/opt` 已安装二进制，也未提交本轮修复；
不要覆盖运行中的服务可执行文件，后续状态以实时进程/日志/Git 为准。

## 10. 高频输入积压与指针节奏修复（2026-10-16）

用户要求排查并修复蓝牙高延迟。当前服务日志最后记录连接间隔 15ms、旧指针 pacing
16.67ms。源码发现：每条普通移动经过取消检查、tick、读包头/体检查和 REPORT，合计约
5 次同步 IPC；常驻服务每处理一次请求固定休眠 1ms，高频输入在 Deskflow socket 积压，
服务端再合并坐标已经太晚。

本轮修复：

- REPORT/POLL 改为非阻塞、单请求在途，保持 IPC v2。状态查询间隔 5ms，重复取消检查
  不再同步往返。客户端最多保留 64 条报告，只合并相邻纯坐标移动；按下位置、键盘、
  按钮、滚轮、范围变化保序。断线清空队列，超时/溢出关闭 IPC 触发服务释放输入。
- 服务通过 socket 可读立即唤醒，活动期间最多 1ms 轮询 BlueZ/末条坐标，取消固定休眠。
  退出先消费在途回复，再请求 DETACH，避免把旧回复误当退出确认。
- 指针默认 pacing 改为 15ms，收到实际连接间隔后直接跟随，移除额外 60Hz 上限。
  Deskflow BLE tick 和不完整包读取等待均设为 1ms，避免未收齐 TCP 包时冻结 IPC。

严格构建与 10 项 CTest、ASan/UBSan 10 项、USB-only 4 项通过；新增回归覆盖 2000 条
连续移动/拖拽在服务暂不回复时仍保留最新位置、原始按下位置及所有状态屏障，并覆盖
断线不重放、队列溢出/超时释放和不完整 TCP 包的短间隔取消检查。

离线对比使用修改前 `32a7d90` 的源码与修复后源码、相同本地 socket 输入与 fake HID 后端。
连续输入 1 秒，计时终点为服务端 fake HID 实际收到报告（不是客户端异步入队返回）：

| 模拟输入频率 | 修复前 P95 | 修复后 P95 |
| --- | --- | --- |
| 125Hz | 3.24ms | 0.08ms |
| 250Hz | 341.32ms | 0.07ms |
| 500Hz | 1625.31ms | 0.06ms |
| 1000Hz | 4198.37ms | 0.06ms |

这是软件输入/IPC 开销，不含 GATT 指针节流、无线传输与手机显示时间，也不是实际鼠标
事件频率测量。复现夹具和结果在 `/tmp/opencode/ble-latency-review/`，新 CLI 构建在
`/tmp/opencode/ble-latency-build/deskflow-otg`。本轮未切换正在运行的用户连接；新版桥接和
常驻服务均需更新并正常重启后才完整生效，仍需真机验证移动/停止、按键、拖拽和滚轮。

后续已构建 GUI + BLE Release DEB，14 项 CTest 与解包运行/版本/依赖检查通过。
包为 `dist/deskflow-otg-gui-0.1.4+git.32a7d90258e9.dirty-ble-latency-fix-linux-amd64.deb`。
用户安装后确认“跟手了，但是有顿挫”。当时桥接已更新，但常驻服务 PID 73136 仍运行被
替换前的旧可执行文件，日志仍为实际 12.5ms、pacing 16.67ms。

用户批准切换服务。执行前旧桥接与监督器已经退出，服务 idle=true、恢复快照已清除；
通过 `--ble-stop-service` 正常结束空闲旧服务，确认 Powered=true、Alias=`len-mint`。
随后用户在 GUI 重连，检查时新版服务 PID 115937 与桥接 PID 117216 的运行二进制 SHA-256
均与新包一致，ready=true，日志实际 interval=15ms、pacing=15ms。新版服务切换后的
平滑度尚无进一步用户验收，不能将此前反馈当作新版完整链路已消除顿挫的证据。
