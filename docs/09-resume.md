# 简历材料（直接可用）

> 原则：**只写做过的**。标 `【待填】` 的地方等你实测后再填，没测就删掉那句话，别编。
> 当前可理直气壮写：协议设计、96 项单元测试、固件可编译可烧录。
> 等屏幕点亮 + 指标测出后，再加"驱动显示""性能数字"两段。

---

## 一、中文完整版（推荐，约 4 条 bullet）

**骑行导航辅助显示终端（Moto Nav HUD）** ｜ 个人项目 ｜ 2025.10–至今
技术栈：STM32F446RE / C / SPI / UART / HAL / LVGL（规划）/ Python

> 面向骑行场景的导航信息显示原型：上位机以 5 Hz 推送导航状态，STM32 解析后驱动 240×320 SPI 屏显示转向、距离与车速，并处理断连告警与按键交互。

- **自定义 16 字节定长二进制协议**（CRC-16/MODBUS 校验、小端、无效值语义），实现「帧头搜索—定长接收—校验—重同步」状态机；针对字节流粘包/半帧问题，采用 **CRC 失败仅回退 1 字节重同步** 的策略，避免坏帧导致后续好帧连坐丢失，并加入 50 ms 半帧超时防止状态机卡死。
- **将协议核心与硬件解耦**（不依赖 HAL/RTOS，仅依赖 stdint），配套 **96 项宿主机单元测试**，覆盖 8 条基准帧、粘包、双帧头、错帧后紧跟好帧、半帧超时边界（40/60 ms）、seq 回绕、垃圾流与 5 Hz 连发 200 帧，`-Wall -Wextra -Werror` 零告警。
- **STM32 底层配置与驱动开发**：手写 RCC 时钟树（HSE 8 MHz → PLL → 180 MHz，APB2 90 MHz，Flash 5 WS）、SPI1 主机模式（模式 0 / MSB / 软件 NSS / 11.25 MHz）与 ST7789 初始化序列、`CASET/RASET` 窗口寻址与 RGB565 批量刷屏；固件 6.6 KB Flash / 1.7 KB RAM。
- **自建调试工具链**：Python 上位机模拟器支持 5 Hz 发送与**故障注入**（错 CRC / 半帧 / 粘包 / 噪声前缀），用于验证接收端健壮性；独立复算校验 8 条协议基准帧，确保 PC 端与 MCU 端组帧一致。

**（屏幕点亮并测完后补第 5 条）**
- 在 128 KB RAM 约束下采用 **240×40 双缓冲 + 脏矩形局部刷新**驱动 240×320 屏（全屏缓冲 150 KB 不可行），单帧刷新 【待填】ms，SPI 稳定工作 【待填】MHz。

---

## 二、精简版（3 行，简历空间紧时用）

**骑行导航 HUD 显示终端**（STM32F446 + SPI 屏 + 自定义协议 / 个人项目）
- 设计 16 字节定长二进制协议（CRC-16/MODBUS）并实现带重同步的接收状态机，**CRC 失败仅回退 1 字节**避免坏帧连坐；配套 96 项 PC 侧单元测试覆盖粘包、半帧超时与错帧恢复。
- 手写 RCC/SPI 底层配置（180 MHz 主频、SPI1 11.25 MHz）与 ST7789 驱动（初始化序列、窗口寻址、RGB565 刷屏）。
- 自研 Python 上位机模拟器，支持 5 Hz 发送与错帧/半帧/粘包故障注入，完成协议端到端验证。

---

## 三、英文版（投外企或英文简历时用）

**Motorcycle Navigation HUD** — Personal Project (2025.10 – Present)
*STM32F446RE · C · SPI · UART · HAL · Python*

- Designed a **fixed-length 16-byte binary protocol** (CRC-16/MODBUS, little-endian) and implemented a frame-sync state machine with **single-byte resynchronization on CRC failure**, preventing one corrupted frame from dropping subsequent valid frames; added a 50 ms half-frame timeout to avoid deadlock on baud-rate mismatch.
- **Decoupled the protocol core from hardware** (no HAL/RTOS dependency) and wrote **96 host-side unit tests** covering frame concatenation, noise prefixes, timeout boundaries, sequence wraparound and a 200-frame 5 Hz stream, compiled with `-Wall -Wextra -Werror`.
- Hand-wrote RCC clock-tree (HSE 8 MHz → PLL → 180 MHz) and SPI1 master configuration (mode 0, 11.25 MHz) plus an ST7789 driver (init sequence, window addressing, RGB565 bulk fill); firmware footprint 6.6 KB flash / 1.7 KB RAM.
- Built a Python host simulator with **fault injection** (bad CRC, half frame, concatenation, noise) to validate receiver robustness.

---

## 四、技能关键词（可拆进"专业技能"栏）

`C` `STM32F4 / HAL` `裸机开发` `SPI / UART` `DMA（规划）` `RCC 时钟树` `GPIO 复用`
`二进制协议设计` `CRC 校验` `状态机` `环形缓冲` `分层解耦` `单元测试`
`FreeRTOS（规划）` `LVGL（规划）` `Python` `Git` `逻辑分析仪 / 示波器` `CubeIDE / GCC`

> 提示：**"（规划）"的部分不要写进技能栏**，除非面试时你能立刻讲出自己写过什么。写上去答不上来是硬伤。

---

## 五、数字与证据对照表

| 可写的数字 | 来源 / 证据 | 状态 |
|---|---|---|
| 16 字节定长帧 | `docs/02-protocol.md` + 8 条基准帧 | ✅ 已验证 |
| 96 项单元测试 | `tests\run.ps1` → `ALL PASS (96 checks)` | ✅ 已验证 |
| 5 Hz / 80 B/s 带宽占用 8% | 9600 8N1 = 960 B/s 推算 | ✅ 可推导 |
| 6.6 KB Flash / 1.7 KB RAM | `arm-none-eabi-size` | ✅ 已验证 |
| SPI1 11.25 MHz | APB2 90 MHz ÷ 8 | ✅ 可推导 |
| 180 MHz 主频 | PLL 配置 | ✅ 已验证（编译/烧录） |
| 单帧刷新 __ ms | 逻辑分析仪测 | ❌ 待测 |
| SPI 稳定上限 __ MHz | 逐档提频实测 | ❌ 待测 |
| 丢包率 __ % | 30 min soak | ❌ 待测 |
| 端到端延迟 __ ms | 回显 RTT | ❌ 待测 |

---

## 六、面试追问与答案要点

| 追问 | 答题骨架 |
|---|---|
| 为什么定长 16 字节？ | BLE 默认有效载荷 20 字节；定长让状态机不需要长度字段和越界保护，代价是扩展性差，V2 传折线必须升版本换帧格式 |
| CRC 错了为什么只退 1 字节？ | 缓冲里可能已经收进下一帧的开头，清空会连坐；定长 + 强校验让误锁代价可控。**有对应的单元测试** |
| 50 ms 超时怎么定的？ | 16 字节 @9600 8N1 ≈ 16.7 ms，留 3 倍裕量 |
| 数据本身含 A5 5A 怎么办？ | 会误锁，但下一次 CRC 必失败继续后移，代价可控 |
| seq 为什么不重传？ | 5 Hz 状态帧 200 ms 就过期，重传旧数据比丢更糟；seq 只用于统计丢包率、区分"没发/没收" |
| 你的测试怎么跑的？ | 协议核心不依赖 HAL，传的是"字节 + 毫秒"，在 PC 上 gcc 编译直接跑，不需要烧板子 |
| 遇到过什么困难？ | CubeIDE 2.2 已移除 CubeMX 图形界面 → 手工集成 HAL；GitHub 上 STM32CubeF4 用 submodule 管 HAL，zip 下载是空目录；Eclipse 源文件夹白名单导致 HAL 不参与编译，链接报 `undefined reference`。**讲定位过程，不要只讲结论** |
| 为什么上 RTOS？（若被问） | LVGL 单帧渲染 15–25 ms，超循环期间无法处理串口；用队列把接收与渲染解耦 |
| 喂狗放哪？ | `mon` 任务，且**喂狗条件是 comms 与 ui 双心跳都推进**——只看自己活着是假安全 |
| 这个项目最难的是哪部分？ | 必须是**真实经历**，比如 HAL 未参与编译那次排查 |
| 为什么屏用双缓冲而不是全屏缓冲？ | 240×320×2 B = 150 KB > 128 KB RAM |
| 还有什么没做完？ | 诚实说：屏幕驱动已写完待硬件确认、蓝牙与 LVGL 未开始。**不要假装全做完了** |

---

## 七、写简历时的三个雷区

1. **不要写"基于 HAL 库实现了……"** 这种没有信息量的开头。写做了什么、解决了什么问题。
2. **不要写没测过的数字**。写了"延迟 20 ms"，面试官问"怎么测的"，答不上来比没写更糟。
3. **不要堆技术名词**。写了 FreeRTOS / LVGL 就一定会被追问，宁可少写。