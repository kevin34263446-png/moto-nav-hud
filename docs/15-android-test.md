# 安卓手机 BLE 验收

当前状态：安卓手机首帧解析与 ACK 回传已通过。
用户截图显示 FFE1 收到 `4F-4B-20-30-0D-0A`（OK 0），连接保持 CONNECTED；
电脑 VCP 只读监听同时捕获 `OK 0`，确认 MCU 镜像输出。
手机显示变化、多场景及距离测试继续独立验收，不混入历史 PC BLE 的帧数统计。
本阶段继续跳过长测，不需要修改或重新烧录当前 BLE 固件。

## 安装与连接

使用 Nordic 的 nRF Connect for Mobile：
[Google Play](https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp)，
无 Google Play 时使用 [Nordic 官方 APK 发布页](https://github.com/nordicsemi/Android-nRF-Connect/releases)。
应用支持扫描、读写 GATT 和通知，见 [官方介绍](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile?lang=en)。

1. 启用手机蓝牙，允许应用所请求的蓝牙扫描权限。
2. 确认电脑 BLE 发送工具已经结束，手机扫描并连接 `BT24`。
   本次电脑发现的地址为 `48:87:2D:C7:D1:43`，地址以手机实际扫描结果为准。
3. 打开服务 `0000FFE0-0000-1000-8000-00805F9B34FB`。
4. 在特征 `0000FFE1-0000-1000-8000-00805F9B34FB` 上启用通知。
5. 点击同一个 FFE1 特征的写入操作，选择 HEX / Byte Array，粘贴下方 32 个十六进制字符。
   这是 16 个二进制字节，不是 UTF-8 文本。不需要添加回车换行。

首次只发送第一帧，确认手机通知出现 `OK 0`，并现场观察屏幕。
单次发送后超过 3 秒会显示 `signal lost`；这是停发告警，不代表手机 BLE 已断开。
先核对手机通知里的 ACK，再在发送后及时观察界面，避免将短暂画面误判为无响应。

## 手动测试向量

1. 右转 350m，45 km/h，剩余 12.4km，ETA 18min；绿色状态条；ACK `OK 0`：

```text
A55A010100025E012D02D8041200AAAD
```

2. 右转 120m，38 km/h，剩余 12.1km，ETA 17min；ACK `OK 1`：

```text
A55A0101010278002602BA04110077A9
```

3. 左转 800m，52 km/h，剩余 11.5km，ETA 16min；ACK `OK 2`：

```text
A55A01010201200334027E0410009B1E
```

4. 右转 600m，44 km/h，偏航状态条为红色；ACK `OK 6`：

```text
A55A0101060258022C0384030D006E28
```

5. 到达符号、30m、12 km/h，剩余 0.2km，ETA 1min；绿色状态条；ACK `OK 5`：

```text
A55A010105061E000C0214000100E99D
```

序号用于确认收帧，不要求单调增长。以上向量来自 `sim_sender.demo_frames()`，
结构化文件见 `docs/validation/mobile-test-vectors.json`；复制的每个帧均应为 16 字节。

## 电脑侧旁路核对

电脑不连接模块 BLE，只监听 NUCLEO 的 ST-Link VCP，核对固件输出的 ACK 镜像：

```powershell
python tools\vcp_monitor.py --port COM13 --seconds 120 --until-acks 1 --output firmware\build_cli\ble\android-first.json
```

该命令不发送任何导航数据，收到首个 ACK 或超时后退出。
如果用户手动发送上述第一帧，手机和 VCP 都应出现 `OK 0`。
ACK 数量本身不代表整套手机测试通过，需要结合发送的具体帧和人工画面确认。
完成手机首帧后再继续逐帧核对、停发恢复及应用断开重连，记录实际结果。

## 当前排查记录

- 首次 120 秒 VCP 监听未收到 ACK，不能据此单独判定无线故障：当时用户尚未找到写入入口。
- 用户截图确认设备从 DISCONNECTED 重新连接到 CONNECTED，并已找到 FFE1 的读写按钮。
- 随后用户报告写入后手机顶部变成 DISCONNECTED，已经区分于屏幕的 signal lost。
- 只读检查：MCU 启动计数仍为 14，有效帧仍为 300，UART 错误与溢出为 0；
  本次尝试未增加正确导航帧，未发现主控复位。
- DMA 最近字节中含 ASCII `Power On`，此处仅记录观察，不能单独确定模块断开原因。
- 下一步需要查看手机实际写入弹窗的特征、数据类型、长度与写入方式，
  以及必要时查看应用断开日志；暂不更改 MCU 固件或模块参数。

原始只读快照：`firmware/build_cli/ble/android-write-disconnect.log`、`android-last-bytes.log`。

## 首帧成功（2026-11-18）

用户截图核对写入设置为 FFE1、BYTE ARRAY、Request，填入有效的 16B 导航帧后发送成功。
截图显示仍为 CONNECTED，FFE1 Value 为 `4F-4B-20-30-0D-0A`。
VCP 报告 `android-first-retry.json` 捕获一次 `OK 0` 后按条件退出。

只读快照 `android-first-success.log` 显示：

- 有效帧从先前的 300 增加到 303，接收字节增加 48，符合三次 16B 合法帧。
- 启动计数仍为 14；UART 错误、环形缓冲溢出及 DMA 重启次数均为 0。
- 本轮监听收到第一条 ACK 就退出，未逐次抓取其余重复发送，不写作 3/3 ACK 测试。
- 未修改 MCU 固件或 DX-BT24 参数。先前断线的根因未单独复现确认，不直接归因于某个设置。

手机单帧解析、BLE 返回与 VCP 镜像已通过。

## 左转帧确认

用户随后按左转 800m、52 km/h 测试帧发送，VCP 报告 `android-left.json` 捕获 `OK 2`。
已验证不同转向状态的手机输入可以被 MCU 解析。
后续检查偏航红条、停发告警与手机主动断开/重连后的恢复。

## 手机恢复测试与颜色问题

`android-recovery.json` 按顺序捕获 `OK 6`、`OK 1`，
用户确认其他表现正常，但恢复状态条是紫色，而不是预期绿色。
进一步确认：屏幕为白底深色字，偏航/断连条呈蓝色或青色。
因此手机传输与重连已经得到验证，颜色验收需要单独修复。

当前配置下的 LVGL 主机探针输出：正常绿 `05 80`、告警红 `D0 00`，
RGB565 的字节顺序与 SPI 接口要求一致。
驱动此前启用 INVON（0x21），本块面板观察到整屏负色；
现增加 `ST7789_INVERSION_ON` 面板选项，默认关闭反色，重新编译后上板确认。
修复版已烧录，用户确认黑底白字、绿色正常条和红色告警条均正常，
VCP 再次捕获 `OK 1`。颜色问题已关闭，详见 `16-display-colors.md`。
