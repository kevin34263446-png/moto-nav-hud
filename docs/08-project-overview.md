# 项目总览：要点与技术点

> 用途：简历写作、面试自述、复盘。本文只写**已经存在的东西**，未完成的一律标注"规划中"。
> 数字和结论以仓库实际代码与 `06-metrics.md` 为准，不要背这里的数当实测值。

---

## 一、一句话概括

基于 STM32F446RE 的骑行导航辅助显示终端原型：上位机（PC/手机）以 5 Hz 发送 **16 字节定长二进制帧**，单片机完成**帧同步、CRC 校验、状态机解析**，驱动 240×320 SPI 屏显示转向、距离、车速，并处理**断连检测、按键交互与异常恢复**。

**两层价值**：
- 工程侧：一个能在资源受限目标上跑通的完整垂直切片（协议 → 传输 → 显示 → 监控）。
- 方法侧：协议核心与硬件解耦，**96 项宿主机单元测试**保证正确性，先测后上板。

---

## 二、项目要点（做了什么 / 为什么这样做）

### 1. 通信协议设计（`docs/02-protocol.md`）
| 决策 | 理由 |
|---|---|
| 16 字节**定长**帧，无长度字段 | BLE 默认 MTU 23 → 有效载荷 20 字节；定长让状态机退化为"计数到 16 就校验"，无长度分支、无越界可能 |
| CRC-16/MODBUS 覆盖帧头 | 帧头是重同步唯一依据；有标准向量 `"123456789"→0x4B37` 可验证 |
| 小端 + 字段名带单位（`dist_m` / `remain_dam`） | 米 vs 10 米两种单位并存，靠命名防错 |
| 无效值语义（`0xFFFF`、`TURN_UNKNOWN=7`） | GPS 无解时显示 `--` 而不是"0 米右转"这种假数据 |
| `seq` 只统计丢包，不做重传 | 5 Hz 状态帧有 200 ms 时效性，重传旧数据比丢更糟 |

### 2. 接收端状态机（`firmware/proto/proto_parser.c`）
- 三态：`IDLE`（找 0xA5）→ `WAIT_LSB`（等 0x5A）→ `RECV`（收满 16 字节后校验）
- **CRC/ver/type 失败只回退 1 字节重同步**，不清空缓冲：清空会把缓冲里已收到的下一个好帧一起丢掉，表现为"错一帧、连丢一串"
- **半帧 50 ms 超时**：依据是 16 字节 @9600 8N1 ≈ 16.7 ms，留 3 倍裕量；防止波特率错误时状态机永久卡死
- 返回码区分 `PROTO_OK / WAIT / DROP_CRC / DROP_VER / DROP_TYPE`，并带统计计数器（`n_ok`、`n_drop_crc`、`n_timeout`）

### 3. 可测试性（`tests/test_proto.c`，**96 项用例**）
协议核心不 include 任何 HAL/FreeRTOS 头，传进来的是"一个字节 + 当前毫秒"，所以能在 PC 上用 gcc 直接跑：

- 8 条 Golden Vector 逐字段校验（文档即规格）
- 组帧一致性（C 端组帧 == 文档字节）
- 粘包、噪声前缀、`A5 A5 5A` 双帧头
- **坏 CRC 帧后紧跟好帧**（验证"只丢 1 字节"的设计承诺）
- 半帧超时边界（40 ms 应过、60 ms 应丢）
- `seq` 255→0 回绕、500 字节垃圾流、假帧头恢复、5 Hz 连发 200 帧

### 4. 显示驱动（`firmware/bsp/st7789.c`）
- ST7789 初始化序列（`SWRESET`→`SLPOUT`→`COLMOD=0x55`→`MADCTL`→`GAMMA`→`INVON`→`DISPON`），每步延时都有手册依据
- `CASET/RASET` 窗口 + `RAMWR` 批量灌像素，RGB565 **大端**发送
- 全屏填充带测量标记（GPIO 翻转，供逻辑分析仪量耗时）
- 背光/片选/复位时序全软件控制（软件 NSS），因为 DC 语义无法用硬件 NSS 表达

### 5. 系统分层（当前已落地 + 规划）
```
中断/DMA 只搬字节 → 环形缓冲(256B) → comms 取字节 → proto_feed()
                                                        ↓
                                             最新值邮箱（覆盖语义，非消息流）
                                                        ↓
                                              ui 只读结构体 → LVGL/自绘
```
- **中断里不做 CRC、不 printf**，只搬字节
- **UI 任务不接触字节流**，这是整个架构最重要的一条约束
- 导航状态是"状态量不是事件量"：队列满时丢旧的，UI 永远看最新值
- 传输层可替换：`proto_feed()` 不认串口，M3 用板载 VCP，M7 换 BLE 只改"从哪个 UART 取字节"

### 6. 工程环境（踩过的真坑，面试可讲）
| 坑 | 表现 | 处理 |
|---|---|---|
| CubeIDE 2.2.0 **去掉了 CubeMX 图形界面** | 新建工程向导里没有 `.ioc`，`Help → Manage Embedded Software Packages` 菜单消失 | 手工集成 HAL：从固件包复制 Drivers，写 `hal_conf`，改 `.cproject` 的 include 路径/宏/源文件夹 |
| GitHub 上的 STM32CubeF4 用 **git submodule** 管 HAL | 下源码 zip 后 `Drivers/STM32F4xx_HAL_Driver` 是空目录，`stm32f4xx_hal.c` 不存在 | `git clone --recurse-submodules` 补齐 |
| Eclipse 源文件夹白名单 | `Drivers/` 已复制进工程但**不参与编译**，链接时报 `undefined reference to HAL_Init` | 在 `.cproject` 的 `sourceEntries` 里登记源路径 |
| 端口冲突 | `Failed to start GDB server` | 61234 被 WPS 云服务占用 → 改调试端口 |
| `*_template.c` | 链接期 `HAL_InitTick`/`HAL_Delay` 重复定义 | 删除 4 个 `*_template.c`（CubeMX 本来就不会复制它们） |

---

## 三、技术点清单

### A. 硬件与电路
- NUCLEO-F446RE 最小系统认知（板载 ST-Link/V2-1、VCP、HSE 由 ST-Link MCO 提供 8 MHz）
- 引脚复用与冲突识别（**PA5 = SPI1_SCK 同时是板载 LD2**）
- GPIO 电气特性：推挽/上拉、输出速度档位（**11 MHz 以上必须配 Very High，否则边沿变缓导致花屏**）
- 外设供电与电流预算（屏+背光+蓝牙+蜂鸣器 ≈ 50–90 mA，峰值 130 mA）
- 三极管驱动有源蜂鸣器（S8050 + 基极电阻，IO 不能直接灌电流）
- **电平适配**：蓝牙模块 TXD 若为 5 V，需 1kΩ+2kΩ 分压后再进 MCU

### B. STM32 外设与底层
- **RCC 时钟树**：HSE 8 MHz → PLLM=8 / PLLN=360 / PLLP=2 → SYSCLK 180 MHz；AHB=180、APB1=45、APB2=90；Flash 5 WS；VOS Scale1
- **SPI 主机**：模式 0（CPOL=0/CPHA=1Edge）、MSB First、8 bit、软件 NSS、**APB2 90 MHz ÷8 = 11.25 MHz**（分频档位只有 5.625/11.25/22.5/45）
- **USART**：板载 VCP（USART2/PA2,PA3）；规划 DMA + 空闲中断（`HAL_UARTEx_ReceiveToIdle_DMA`）收包
- SysTick + `HAL_IncTick` 提供时基；中断优先级分组
- IWDG 独立看门狗（**喂狗条件是 comms 与 ui 双心跳都推进**，只看自己活着是假安全）
- 按键消抖（内部上拉，20 ms 窗口）

### C. 通信协议
- BLE 基础：ATT MTU 23 → 有效载荷 20 字节；GATT 服务/特征（`0xFFE0` / `0xFFE1` / `0xFFE2`）
- 带宽预算：16 B × 5 Hz = 80 B/s，9600 8N1 = 960 B/s，占用 8%
- CRC-16/MODBUS（多项式 0x8005 反射 0xA001、初值 0xFFFF、无异或输出）
- 字节序、定长帧、帧同步与**贪心重同步**策略的取舍
- 链路质量指标：`seq` 跳变统计算丢包率

### D. 软件工程
- **分层与依赖倒置**：协议层零硬件依赖 → 可在 PC 上做单元测试
- 单生产者/单消费者**无锁环形缓冲**（`volatile` 读写指针 + 2 的幂掩码）
- **最新值覆盖** vs 消息流队列的语义选择
- 状态机设计、错误码分类、可观测性（计数器 + 日志）
- 单元测试方法：Golden Vector、边界值（40/60 ms）、故障注入（错 CRC/半帧/粘包）
- Git 工作流、按里程碑提交、`.gitignore` 与构建产物隔离

### E. 显示与人机
- ST7789 内部 GRAM 与寻址：`CASET`（列）/`RASET`（行）/`RAMWR`；`MADCTL` 控制旋转与 RGB/BGR 顺序
- RGB565 编码与大端传输；`INVON/INVOFF` 决定颜色是否"像底片"
- 显存约束：240×320×2 B = 150 KB > 128 KB RAM → **240×40 双缓冲（38.4 KB）+ 脏矩形局部刷新**（规划）
- LVGL 8.x 移植要点：`lv_port_disp`、`flush_cb`、`lv_disp_flush_ready()`（与 DMA 完成回调配合，否则撕裂）

### F. RTOS（规划中）
- 任务划分（3 个：comms / ui / mon）、优先级、栈深度评估（`vTaskList` 看余量）
- 任务间通信：消息队列 vs 最新值邮箱；`CMSIS-RTOS v2` API
- 中断与任务的上下文边界（ISR 只搬字节）

### G. 工具链与测量
- CubeIDE 2.2.0（基于 Eclipse CDT）、`arm-none-eabi-gcc 14.3.1`、make/ninja
- `STM32_Programmer_CLI` 命令行烧录；ST-Link GDB server 与端口
- 上位机模拟器（Python + pyserial）：5 Hz 发送、**故障注入**（错 CRC / 半帧 / 粘包 / 噪声）、回显板子日志
- 逻辑分析仪 + PulseView：测 SPI 时序、单帧刷新耗时、UART 波形

---

## 四、可验证的成果（截至当前）

| 项 | 状态 | 证据 |
|---|---|---|
| 协议定义 v1 | 完成 | 8 条样例帧 CRC 独立复算 8/8 正确 |
| 协议解析核心 | 完成 | `tests\run.ps1` → **ALL PASS (96 checks)** |
| 上位机模拟器 | 完成 | `python tools\sim_sender.py --selftest` 全绿 |
| 固件工程 | 完成 | 97 个源文件 `-Wall` 编译链接通过，Flash 6.6 KB / RAM 1.7 KB |
| 烧录 | 完成 | `STM32_Programmer_CLI` 下载 + 复位成功 |
| 屏幕点亮 | **待确认** | 需实拍三纯色 |
| 量化指标 | 未测 | `06-metrics.md` 全部标注"未测" |

> 面试纪律：**没测的数字不要说**。当前能理直气壮说的是"96 项单元测试""协议设计权衡""固件可编译可烧录"；不能说"刷新 xx ms""SPI 稳定 xx MHz"。

---

## 五、面试自述模板（90 秒）

> 我做了一个骑行导航 HUD 原型：上位机按 5 Hz 发 16 字节定长帧，STM32F446 解析后驱动一块 240×320 的 SPI 屏显示转向和距离。
>
> 协议是我自己定的，核心考虑是 BLE 默认有效载荷只有 20 字节，所以选定长帧，让接收状态机不需要处理长度字段。
>
> 我做得比较扎实的是两件事：一是**把协议核心和硬件完全解耦**，它不依赖 HAL，能在 PC 上用 gcc 跑，我写了 96 项单元测试，包括坏帧后紧跟好帧、半帧超时、粘包这些真实场景；二是**CRC 失败只回退一个字节重同步**，而不是清空缓冲区，否则会把后面已经收到的好帧一起丢掉。
>
> 过程中踩的坑我也记着：我用的 CubeIDE 2.2 已经去掉 CubeMX 图形界面了，我是手工把 HAL 集成进工程的；还遇到 Eclipse 的源文件夹白名单导致 HAL 没参与编译、链接一堆未定义符号。

---

## 六、下一步（按优先级）

1. **确认屏幕三纯色** → 记录 SPI 稳定上限与全屏刷新耗时（逻辑分析仪）
2. 棋盘格 → 验证 `CASET/RASET` 寻址
3. M3：板载 VCP 收帧 → `proto_feed()` → 屏上显示距离，跑完阶段 2 的 8 项验收
4. M4：LVGL 两页界面
5. 之后才是 RTOS、蓝牙、量化指标全表