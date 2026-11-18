# 导航固件开发基线

## 源码归属

`moto-nav-hud/firmware` 是自有固件的维护入口。`../ws/moto-nav-hud-fw`
是已有 CubeIDE 工程，并暂时提供本地 HAL/CMSIS 和 LVGL 依赖。
M5 起在仓库固件上进行 FreeRTOS 开发，当前代码与构建入口见 `12-freertos.md`。
CubeIDE 原工程暂保留裸机版本，不能直接视作当前 RTOS 工程。
后续已补齐仓库自身的 CubeIDE 工程；当前导入及构建方式以 `17-cubeide.md` 为准。

2026-11-18 已将工程中的 IWDG 初始化、主循环喂狗、备份寄存器启动计数、
`WDG_SELFTEST` 条件编译代码及 HAL IWDG 开关同步回仓库。
两份启动文件内容相同，仓库名称为 `startup_stm32f446xx.s`。

## 可重复构建

在仓库根目录执行，PowerShell 5.1 或更新版本：

```powershell
.\tests\run.ps1
python tools\sim_sender.py --selftest

# 替换为本机 Arm GNU 工具链 tools/bin 的实际路径。
.\tools\build-firmware.ps1 -ToolchainBin 'D:\path\to\tools\bin'
```

构建脚本每次重编译仓库固件、所需 HAL 模块及 LVGL 的 src 源码，
不依赖已有对象文件。输出 `firmware/build_cli/app.elf`、`app.map`，
并打印链接内存占用。不定义 `WDG_SELFTEST`，不烧录设备。

依赖默认使用相邻 CubeIDE 工程。也可用 `-DependenciesRoot` 指定同时包含
`Drivers/` 和 `lvgl/` 的目录；依赖恢复到仓库 firmware 后可传 `-DependenciesRoot .\firmware`。
`lv_conf.h` 和 HAL 配置均优先使用仓库文件。

现有 CubeIDE `Debug` makefile 未包含 LVGL，因此暂不作为完整固件的构建入口。
本阶段没有自动修改 IDE 元数据。

## 裸机基线运行配置（M5 前）

- 裸机主循环；尚未引入 FreeRTOS。
- USART2：**115200 8N1**，逐字节中断接收、256 字节环形缓冲。
- ST7789：阻塞 SPI；LVGL 8.3.11，240×40 RGB565 单缓冲（19,200 字节）。
- 有效帧驱动界面；收到过有效帧后，超过 3 秒无有效帧显示断连。
- IWDG：预分频 32、重装值 4000，按 LSI 标称频率约 4 秒，实际超时有振荡器偏差。

模拟器默认 9600，连接当前 USART2 固件必须显式覆盖：

```powershell
python tools\sim_sender.py --port COM13 --baud 115200 --log
python tools\sim_sender.py --port COM13 --baud 115200 --fault all --seconds 60
```

COM13 仅为示例，以本机设备管理器为准。DX-BT24 出厂波特率不能直接当作当前 VCP 配置。

## 验证边界

2026-11-18：宿主机协议测试 96 项通过，Python 模拟器自检通过。
使用 Arm GCC 14.3.1、`-Os` 重新编译 216 个源文件并链接成功：

| 项目 | 链接结果 |
|---|---|
| Flash（text + data） | 261,832 B，约 255.70 KiB，占 49.94% |
| RAM（data + bss，含链接脚本预留区） | 55,424 B，约 54.13 KiB，占 42.29% |

RAM 数值不是运行时栈高水位。历史 README 的 318 KB Flash 数字未作为本次结果沿用；
不同构建参数的产物大小不能直接比较性能。

## 2026-11-18 上板回归

设备：NUCLEO-F446RE，ST-Link V2，VCP COM13，115200 8N1。
正常固件 `app.elf` 烧录后通过 CubeProgrammer 校验。
ELF SHA-256：`B3ACD2CCAA4F792BB9B25A08C9773FFBD2C142079B7848784AEFF056E7F16119`。

| 验收项 | 结果与边界 |
|---|---|
| 正常帧 | 5Hz 连续 100 帧，收到 100 个匹配序号的确认 |
| 错误帧恢复 | 错 CRC、超时半帧、错误版本、错误类型均未收到确认；随后好帧均被确认 |
| 字节流恢复 | 双帧粘包、噪声前缀、重叠 A5 帧头均收到预期确认 |
| 停发后恢复 | 停发 4 秒无异常确认；重新发帧后收到确认，视觉断连状态尚未人工验收 |
| 导航画面 | 用户现场确认左转符号、200 距离及 45 km/h 等内容正常显示 |
| IWDG 故障自测 | 用同批对象文件替换为带 WDG_SELFTEST 的 main，故意关中断并死循环；相隔 12 秒读取 BKP0R，计数 3 → 4，证明自动复位；未测定精确超时时间 |
| 自测后恢复 | 正常固件重新烧录、校验并复位成功；再次运行全部 10 项串口检查，全部通过 |

串口自动回归脚本（约 30 秒）与报告：

```powershell
python tools\hardware_regression.py --port COM13 --output firmware\build_cli\hardware-regression.json
```

本次原始串口报告和看门狗日志位于忽略的 `firmware/build_cli/` 目录，
后者为 `watchdog-test.log`。临时自测脚本同目录 `check-watchdog.ps1`，含本机绝对路径，
不作为通用构建入口；自测 ELF 与正常 `app.elf` 分开保存。
100 帧短时回归不等同于历史 10 分钟稳定性测试；后续 RTOS 改造后需要重新验收。
