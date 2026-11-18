# M7：DX-BT24 无线透传

## 已完成与待验收

已完成 USART1 BLE / USART2 VCP 编译选择、BLE 固件构建、扫描/服务发现/发送工具。
两种固件完整编译链接通过，协议 96 项测试通过。
本阶段按用户要求跳过长测。

模块接线后已发现 BT24 广播，BLE 固件烧录与双向透传短测通过。
当前板上为 USART1 BLE 输入、USART2 ACK 日志镜像版。
用户已在连续无线发送期间确认数字和转向正常变化。
安卓手机首帧、左转、偏航与重连恢复已收到 MCU ACK，VCP 镜像亦已捕获。
面板反色问题已单独修复并经用户确认；当前 ELF 与颜色验收见 `16-display-colors.md`。
下面保留修复前 BLE 固件的短测记录；距离测试与长测仍未验收。
手机逐帧记录与手动发送帧见 `15-android-test.md`。

## 当前这块底板的接线

用户已确认背面标注 **POWER: 3.6–6V，LEVEL: 3.3V**。
这是带供电底板的具体版本，不能将供电规则套用到裸 DX-BT24 模组。
裸模组厂家工作电压为 3.3V，见 [厂家手册](https://www.szdx-smart.com/static/upload/2023/12/08/202312089264.pdf)。

断电接线，屏幕维持原接法：

| DX-BT24 底板 | NUCLEO-F446RE |
|---|---|
| VCC | 5V（仅适用于上述 POWER 标注的底板） |
| GND | GND |
| TXD | PA10 / D2，USART1_RX |
| RXD | PA9 / D8，USART1_TX |
| STATE / EN | 初次透传暂不接 |

先按规划的 9600 8N1 接入；若模块曾通过 AT 命令修改波特率，需要核实实际设置。
不自动发送会改变名称、密码或波特率的 AT 命令。

## 固件选择

```powershell
# 默认 VCP：USART2 115200，保持原 PC 灌帧链路。
.\tools\build-firmware.ps1 -ToolchainBin 'D:\path\to\tools\bin' -NavSource vcp
# BLE：USART1 9600，PA9/PA10；USART2 115200 仅镜像 ACK 日志。
.\tools\build-firmware.ps1 -ToolchainBin 'D:\path\to\tools\bin' -NavSource ble
```

VCP 输出 `firmware/build_cli/app.elf`，BLE 输出 `firmware/build_cli/ble/app.elf`。
不同时混合两个串口的字节，避免不同帧互相污染。
BLE 版 RX 为 DMA2 Stream2 / Channel4，64B 循环接收，IDLE/HT/TC，IRQ 优先级 5。
VCP 版仍使用 DMA1 Stream5 / Channel4。
解析成功后 ACK 返回当前导航串口；BLE 版额外镜像到 USART2。
COM13 收到镜像 ACK 可证明无线输入已经过 MCU 协议解析，不能用 GATT 写入成功替代此结论。

当前 `g_app_metrics` 末尾增加 `nav_source` 字段：0=VCP，1=BLE，共 18 个 uint32_t。
只依据有效导航帧判断断连；STATE 引脚不作为导航数据有效性的依据。

## PC BLE 工具

在仓库外建立专用虚拟环境，已在本机 `D:\moto_gap\.venv-ble` 安装 Bleak 3.1.0。
复现安装：

```powershell
python -m venv ..\.venv-ble
..\.venv-ble\Scripts\python.exe -m pip install -r tools\requirements-ble.txt
..\.venv-ble\Scripts\python.exe tools\ble_sender.py --scan --name BT24
# 扫到目标模块后使用其真实地址。
..\.venv-ble\Scripts\python.exe tools\ble_sender.py --address 'module-address' --inspect
# 根据实际发现的透传服务选择 write / notify UUID。
..\.venv-ble\Scripts\python.exe tools\ble_sender.py --address 'module-address' --write-uuid 'write-uuid' --notify-uuid 'notify-uuid' --frames 50 --output firmware\build_cli\ble\wireless.json
```

工具使用 [Bleak 官方 GATT API](https://bleak.readthedocs.io/en/stable/api/client.html)。
扫描不会连接设备，服务检查不会发送业务帧；发送时必须明确给出地址和写入 UUID。
BLE UART 透传使用 GATT，不应把 Windows 的普通蓝牙 COM 口当作 DX-BT24 的无线传输接口。
没有通知特征时，工具只报告写入完成，`passed` 为 null；需要结合 VCP ACK 和界面验收。
有通知特征时会核对所有预期 ACK 的数量和顺序，未收到 ACK 不算通过。

首次无线验证采用 50 帧短测，然后检查停发、恢复与重新连接。
模块连接到手机时可能无法同时连接 PC，联调阶段选一个中心设备。

## 2026-11-18 无线短测记录

设备广播名称 `BT24`，地址 `48:87:2D:C7:D1:43`。
实际服务为 `0000ffe0-0000-1000-8000-00805f9b34fb`；
透传特征 `0000ffe1-0000-1000-8000-00805f9b34fb` 支持 write / write-without-response / notify / read。
同时发现 FFE2，但本次未向其写入数据，未尝试配置模块参数。

BLE ELF SHA-256：`8D4C493BFEBD60D7041A10162D622103CB9E7F1048A30AA99961570E8CC8364D`。
Flash 区域使用 269,048 B（51.32%），RAM 区域使用 69,624 B（53.12%）。

| 验收项 | 结果 |
|---|---|
| 首次双向透传 | 50 个有效帧，50 个 MCU ACK，序号及顺序一致 |
| 断开后重新连接 | 第二次独立连接成功，动态 50 帧及 2 个恢复帧全部确认 |
| 错 CRC | 故障帧 seq=50 未获确认，随后 seq=51、52 正常确认 |
| 停发与恢复 | 连接内停发 4 秒后恢复确认；首轮断开后的板上断连标志为 1 |
| 有效帧累计 | 102（板上 `0x66`），与两轮确认总数一致 |
| DMA 字节累计 | 1648 = 103×16，包含 102 个有效帧与 1 个坏 CRC 帧 |
| 接收源 | `nav_source=1`，来自 USART1 BLE，非 VCP 灌帧 |
| 错误 / 溢出 / 故障重启 | UART 错误 0，环形缓冲溢出 0，DMA 故障重启 0 |
| 复位计数 | 11 → 11，期间没有计数复位 |
| 任务栈最小余量 | comms 1832 B / ui 6572 B / mon 1924 B |
| 显示验收 | 第三轮一分钟动态发送 300/300，用户现场确认数字和转向正常变化 |

ACK 路径为 MCU 解析后 USART1 TX → DX-BT24 → BLE notify。
仅 GATT 写入成功不足以得到上述通过结论，本次已核对 MCU ACK 与板上计数。
前述 PC 短测没有另行抓取 VCP 镜像；后续安卓首帧已在 COM13 捕获 `OK 0`，见手机验收记录。

短测报告位于 `firmware/build_cli/ble/wireless.json`、`reconnect.json`；
GATT 快照 `gatt.json`，诊断快照 `before.log`、`disconnected.log`、`after.log`。
早期 `reconnect.json` 的 frames_written 指有效帧数 52；其 expected/received 为有效帧序号。
工具现已将总写入与有效帧数分开记录，故障帧也计入总写入。
验收摘要见 `docs/validation/2026-11-18-ble.json`。

第三轮显示检查报告为 `display-check.json`，300 个 ACK 全部按序匹配，
本轮结束时板上有效帧数为 300，接收字节数 4800，UART 错误和溢出为 0。
三轮有效帧确认合计 402/402，但不是同一连续启动周期的累计值。
首两轮后的启动计数为 11，第三轮后为 14；用户确认期间按过复位或重新连接，
因此不能将这段计数变化当作自动异常复位，也不把三轮记录合并为连续无复位测试。
当前 RCC 复位标志未按启动清除，含历史累积标志，不能单独作为本次复位原因的证据。
最终诊断快照 `display-after.log`，复位标志快照 `reset-flags.log`。

```powershell
# 短测与错误恢复；此处是本次发现的真实设备地址和 UUID。
..\.venv-ble\Scripts\python.exe tools\ble_sender.py --address 48:87:2D:C7:D1:43 --write-uuid 0000ffe1-0000-1000-8000-00805f9b34fb --notify-uuid 0000ffe1-0000-1000-8000-00805f9b34fb --frames 50 --exercise --output firmware\build_cli\ble\reconnect.json
```
