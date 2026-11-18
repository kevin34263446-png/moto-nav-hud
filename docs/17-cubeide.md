# 当前工程入口与 CubeIDE 验证

仓库 `firmware/` 是应用源码的唯一维护入口，CubeIDE 项目名为 **moto-nav-hud**。
旧 `../ws/moto-nav-hud-fw` 工程保留历史裸机代码，目前只复用其 HAL/CMSIS/LVGL 依赖。
不要再用旧项目的 Debug 启动配置烧录当前系统。

## 依赖配置

本机已有依赖时，在仓库根目录执行：

```powershell
python tools\configure-cubeide.py
```

默认依赖目录与命令行构建一致：`../ws/moto-nav-hud-fw`、`../deps/FreeRTOS-Kernel`。
脚本使用 Eclipse 相对链接引用第三方目录，不复制应用源码。
HAL 与内核的编译清单由 `tools/firmware-sources.json` 统一维护。

在新机器上，先准备依赖，再按实际目录重新生成链接：

```powershell
# setup-deps 需要本地已有 STM32CubeF4 包，并会下载 LVGL 8.3.11。
.\tools\setup-deps.ps1
.\tools\setup-freertos.ps1
python tools\configure-cubeide.py --dependencies-root firmware
```

也可显式指定 `--dependencies-root` 与 `--freertos-root`，无需照搬本机磁盘路径。
生成器要求 Python 3.9+，重复执行结果一致；运行后刷新 IDE 项目。

## IDE 使用

1. File → Import → General → Existing Projects into Workspace，选择仓库的 `firmware/`。
2. 不勾选 Copy projects into workspace，保持源码在仓库内。
3. 导入项目 **moto-nav-hud**；旧项目 **moto-nav-hud-fw** 可保留，但不要选错。
4. 项目 Build Configurations → Set Active，选择目标配置，再 Build Project。
5. Debug Configurations 选择项目随附的 `moto-nav-hud Debug-BLE` 或 `Debug-VCP` 启动配置。

| 配置 | 导航输入 | 编译选项 | ELF |
|---|---|---|---|
| Debug-BLE | USART1 9600，BLE | -Og、-g3、DEBUG | Debug-BLE/moto-nav-hud.elf |
| Debug-VCP | USART2 115200，VCP | -Og、-g3、DEBUG | Debug-VCP/moto-nav-hud.elf |
| Release-BLE | USART1 9600，BLE | -Os | Release-BLE/moto-nav-hud.elf |
| Release-VCP | USART2 115200，VCP | -Os | Release-VCP/moto-nav-hud.elf |

Debug 源码在启动 IWDG 前设置 DBGMCU 的暂停冻结位，启动配置也显式设置，
用于覆盖调试器连接时的寄存器改写及停在 main 之前的情况。
冻结只影响 CPU 被调试器暂停的时间；正常运行时任务卡死仍由看门狗处理。
Release 启动时显式清除冻结位。调试工具之后仍可能修改该寄存器，
不能把通过其他调试工具连接后读取的值直接当作启动代码的结果。

## 无界面构建

```powershell
.\tools\build-cubeide.ps1 -CubeIDE 'D:\path\to\STM32CubeIDE' -Configuration Debug-BLE,Debug-VCP,Release-BLE,Release-VCP
```

脚本使用独立的 `../ide-validation-workspace`，不修改原交互工作区。
只进行构建，不自动烧录。日志保存在忽略的 `firmware/build_cli/`。
这是 CubeIDE 自身的 managed build；原 `build-firmware.ps1` 仍可独立使用。
两种构建的源码清单一致，不要求 ELF 字节完全相同。

## 2026-11-18 验收

- CubeIDE 2.2.0、Arm GCC 14.3.1，四配置均实际编译通过，零错误、零警告，各编译 220 个源文件。
- 原命令行 BLE 构建仍通过；生成器重复执行一致，XML 和脚本语法检查通过。
- ST-Link GDB 命中 `monitor_task`，源码定位到仓库 `firmware/Src/main.c`。
- Debug 暂停 CPU 6 秒，启动计数保持 17 → 17，冻结位为 0x1000。
- GDB 的 FreeRTOS 任务入口栈回溯存在不可展开帧，本次只验证断点、当前源码与暂停行为；
  不宣称已验证完整 RTOS 线程感知调试。IDE 图形界面的启动操作未另行手动点击验证。
- 最后恢复并校验 CubeIDE **Release-BLE** 固件，VCP 捕获手机帧的 `OK 1`，用户确认画面正常。
- 不执行长测；调试过程中的烧录与主动复位不计为异常复位。

GNU size 数据如下（Flash 列为 text+data，非链接区域含对齐的口径）：

| 配置 | Flash B | 静态 RAM B |
|---|---:|---:|
| Debug-BLE | 295000 | 69632 |
| Debug-VCP | 294880 | 69560 |
| Release-BLE | 269056 | 69624 |
| Release-VCP | 268952 | 69552 |

原始构建、GDB 和手机确认日志位于 `firmware/build_cli/cubeide-*`；
摘要见 `docs/validation/2026-11-18-cubeide.json`。
