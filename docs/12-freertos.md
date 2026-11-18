# M5：FreeRTOS 导航固件

本文保留 M5 逐字节中断版的设计和验收记录。当前接收链路已改为循环 DMA，见 `13-uart-dma.md`。

## 设计

使用 FreeRTOS-Kernel V11.1.0 原生 API、GCC ARM_CM4F 移植层。
固定上游提交 `dbf70559b27d39c1fdb68dfb9a32140b6a6777a0`（MIT）。
上游来源：[FreeRTOS-Kernel V11.1.0](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/dbf70559b27d39c1fdb68dfb9a32140b6a6777a0)。
采用原生 API，便于直接表达单槽 `xQueueOverwrite` 和中断任务通知；
原始施工图的 CMSIS-RTOS v2 包装层本阶段不引入。

| 任务 | 优先级 | 静态栈 | 职责 |
|---|---|---|---|
| comms | 3 | 2 KiB | 等待 UART 任务通知、消费环形缓冲、解析协议、覆盖邮箱、串口回显 |
| ui | 2 | 8 KiB | 屏幕/LVGL 初始化、消费导航邮箱、刷新、显示断连 |
| mon | 1 | 2 KiB | 每 100 ms 判断断连、检查心跳、条件喂狗、采集栈余量 |
| Idle | 0 | 1 KiB | FreeRTOS 空闲任务 |

任务、TCB、队列均静态分配，FreeRTOS 动态分配关闭。
LVGL 仍保留独立的 32 KiB 内存池，不使用 FreeRTOS 堆。
中断只搬字节、重新启动接收并通知 comms，解析器保持纯 C。
UART 仍为逐字节中断，DMA 是下一步独立改动。

单槽队列按结构体复制导航状态，发送使用覆盖语义；UI 只展示最新值。
USART2 IRQ 优先级 5，满足 FreeRTOS FromISR API 的中断优先级约束。
任务句柄创建、调度器启动之后才由 comms 开启接收。
所有 LVGL API 都只由 ui 调用，不跨任务加锁。

SysTick 保持 1 kHz：始终递增 HAL 毫秒计数，调度器启动后再执行内核 tick；
SVC、PendSV 由 CM4F 移植层接管。没有启用 tickless。
启动阶段 HAL_Delay 仍可工作，进入调度后任务使用 vTaskDelay。

只有收到有效帧才更新连接时间；mon 判断超过 3 秒为断连，ui 显示状态。
comms 空闲时也定期更新心跳，正常断连不会导致看门狗复位。
mon 每次喂狗必须同时看到 comms、ui 心跳相较上一次喂狗推进，
任一任务停滞都会停止喂狗。约 4 秒超时依赖 LSI，精确时间需实测。

## 构建与验收

```powershell
.\tools\setup-freertos.ps1
.\tools\build-firmware.ps1 -ToolchainBin 'D:\path\to\tools\bin'
.\tests\run.ps1
python tools\sim_sender.py --selftest
python tools\hardware_regression.py --port COM13 --output firmware\build_cli\rtos-regression.json
# 3000 帧，约 10 分钟，随后做错误恢复与停发恢复检查。
python tools\hardware_regression.py --port COM13 --frames 3000 --output firmware\build_cli\rtos-soak.json
# 动态画面回归：改变转向、距离、速度和偏航状态。
python tools\hardware_regression.py --port COM13 --frames 300 --vary-ui --output firmware\build_cli\rtos-dynamic.json
```

`-FreeRTOSRoot` 可指定内核路径。默认相邻 `../deps/FreeRTOS-Kernel`。
M5 初期仅使用命令行构建；后续 CubeIDE 四配置已补齐并通过验证，见 `17-cubeide.md`。

`-WatchdogSelfTest` 将固件输出到 `firmware/build_cli/watchdog_selftest/`。
该版本运行约 5 秒后仅挂起 UI 任务，通信及监控仍可调度，
用来验证条件喂狗，而非直接关闭所有中断。自测后必须恢复正常 ELF。

调试器可读取全局 `g_app_metrics`，11 个连续 uint32_t 字段依次为：
comms/ui/mon 心跳、有效帧数、环形缓冲溢出、UART 错误、
comms/ui/mon 栈历史最小空闲字节数、断连标志、喂狗次数。
栈余量通过 uxTaskGetStackHighWaterMark 得到，单位已换算为字节；
未引入需要额外堆支持的 vTaskList 格式化输出。
启用了栈溢出检查和 configASSERT，故障时停止执行并等待 IWDG。

## 本次验证记录（2026-11-18）

正常固件 ELF SHA-256：`3D6CCC987BA9597ADED11CB0081170C59BF28DC9C64022E174FA022062D6798F`。
Arm GCC 14.3.1、`-Os`，220 个源文件完整编译、链接通过：
Flash 区域使用 267,040 B（50.93%），RAM 区域使用 69,360 B（52.92%，含静态任务栈及链接预留区）。

- 协议核心 96 项测试、Python 模拟器自检通过。
- 正常版烧录校验通过；100 帧短时测试及全部 10 项串口检查通过。
- 用户现场确认 FreeRTOS 版导航画面显示正常。
- 短时测试后的栈最小余量：comms 1824 B、ui 6556 B、mon 1924 B；
  环形缓冲溢出和 UART 错误为 0。余量仅覆盖已运行路径，不代表所有极端输入。
- UI 挂起自测：备份寄存器启动计数 7 → 8，正常固件恢复并校验通过。
  没有直接测量复位延时，不能写成“实测 4 秒”。
- 初次定时测试 3000/3000，648.485 秒；PC 相对超时累积使实际速率约 4.63Hz。
  修正为绝对时间节拍，后续动态短测 300/300，用户确认画面正常变化。
- 最终动态长测：3000/3000，600.313 秒（含末尾 0.3 秒收尾窗口），
  统计速率 4.997Hz，确认序号顺序一致；随后全部错误恢复与停发恢复检查通过。
- 恢复正常固件后的三轮测试共解析 6327 个有效帧，与板上计数 `0x18B7` 一致。
  长测前后启动计数均为 9，说明期间未发生计数复位；溢出与 UART 错误均为 0。
- 最终最小栈余量：comms 1824 B、ui 6556 B、mon 1884 B，均超过 128 B。
  断连逻辑标志最终为 1，串口停发恢复通过；断连画面未单独获得人工确认。

看门狗自动自测（会临时替换固件，并在 finally 中恢复正常版本）：

```powershell
.\tools\build-firmware.ps1 -ToolchainBin 'D:\path\to\tools\bin' -WatchdogSelfTest
.\tools\test-watchdog.ps1 -Programmer 'D:\path\to\STM32_Programmer_CLI.exe' -SerialNumber 'your-stlink-serial'
```

正常固件、自测固件及日志均位于忽略的 `firmware/build_cli/`。
最终长测报告为 `rtos-soak-paced.json`，诊断快照为 `rtos-final-metrics.log`；
可提交的验收摘要为 `docs/validation/2026-11-18-m5.json`。
裸机退路固件保存在本机 `firmware/build_baseline/app.elf`，不提交二进制。
