# CubeMX 配置（M2：从空工程到能点屏）

> 原则：**一次只开一个外设**。这一份只配 RCC + SPI1 + 4 个 GPIO + USART2，不要顺手把 DMA、LWIP、FreeRTOS 勾上——那些出了问题会让你分不清是谁的锅。
> 配套代码：`firmware/bsp/st7789.c`。

---

## 1. 建工程

`File → New → STM32 Project` → 搜索框输入 **NUCLEO-F446RE** → 选 **Board Selector** 里的 NUCLEO-F446RE → Next → 工程名 `moto-nav-hud-fw` → Toolchain 选 **STM32CubeIDE**。

用 Board 模板的好处：CubeMX 会自动把 HSE 配成"ST-Link MCO 提供的 8MHz"，不用自己猜板载有没有晶振。

## 2. 时钟树（目标 180MHz）

在 `Clock Configuration` 里填：

| 项 | 值 |
|---|---|
| Input frequency | 8 MHz |
| PLL Source Mux | HSE |
| PLLM | **8**（8MHz÷8 = 1MHz） |
| PLLN | **360**（VCO = 360MHz） |
| PLLP | **2**（SYSCLK = 180MHz） |
| PLLQ | **8**（45MHz，留用） |
| System Clock Mux | PLLCLK |
| AHB Prescaler | /1 → HCLK **180MHz** |
| APB1 Prescaler | /4 → **45MHz** |
| APB2 Prescaler | /2 → **90MHz** |
| Flash Latency | **5 WS** |

> 生成后第一件事：`printf("HCLK=%lu\r\n", HAL_RCC_GetHCLKFreq());` 打印核对。**别相信配置界面，相信运行时的数。**

**SPI1 在 APB2 上 = 90MHz**，所以 SPI 时钟只能取 90 的整数分频：

| Prescaler | 实际 SPI 时钟 | 用途 |
|---|---|---|
| /16 | 5.625 MHz | 保底档（花屏时退这里） |
| /8 | **11.25 MHz** | M2 起点 |
| /4 | 22.5 MHz | 第二档 |
| /2 | 45 MHz | 上限档（可能超 ST7789 规格） |

**没有 15MHz 和 30MHz 这两档**——这是 180MHz 主频下的数学结果，不是配置错误。

## 3. SPI1

`Connectivity → SPI1`：

| 项 | 值 | 为什么 |
|---|---|---|
| Mode | **Transmit Only Master** | 单向写屏，顺便把 PA6(MISO) 空出来 |
| Hardware NSS Signal | Disable | 片选我们手动控制（软件 NSS） |
| Data Size | 8 Bits | |
| First Bit | MSB First | ST7789 要求 |
| Clock Polarity (CPOL) | **Low** | SPI 模式 0 |
| Clock Phase (CPHA) | **1 Edge** | SPI 模式 0 |
| Prescaler | 8（先慢，通了再提） | |
| Baud Rate | 11.25 MBit/s | |
| CRC Calculation | Disabled | |
| FIFO Threshold / TI mode | 不用管 | |

**引脚速度必须改**：`GPIO Settings → PA5 / PA7 → Maximum output speed = Very High`。
默认的 Low 在 11MHz 以上会出现边沿变缓、偶发花屏，这个坑很多人查一天。

## 4. GPIO（4 个输出）

`System Core → GPIO`，按下表配，并且**在 User Label 里填名字**——CubeMX 会据此生成 `TFT_CS_Pin` 之类的宏，驱动代码直接用：

| 引脚 | 模式 | User Label | 初始电平 | 输出速度 |
|---|---|---|---|---|
| PB6 | Output Push Pull | `TFT_CS` | **High** | High |
| PB5 | Output Push Pull | `TFT_DC` | Low | High |
| PB4 | Output Push Pull | `TFT_RST` | **High** | Low |
| PA8 | Output Push Pull | `MEAS` | Low | Low |

- `TFT_CS` 初始 **High**（低有效，上电不能拽着片选）；`TFT_RST` 初始 High，复位靠代码拉低。
- `MEAS`（PA8/D7）是给你接逻辑分析仪量刷屏耗时用的，见 `docs/03-hardware.md` §4。没有逻辑分析仪可以先不配。

## 5. USART2（板载 VCP）

| 项 | 值 |
|---|---|
| Mode | Asynchronous |
| Baud Rate | 115200（**VCP 上随便填**，它是虚拟串口，不受物理波特率限制） |
| Word Length | 8 Bits |
| Parity | None |
| Stop Bits | 1 |
| NVIC | USART2 global interrupt **使能**（M3 逐字节接收要用） |

`printf` 重定向（写在 `main.c` 的 `USER CODE` 区）：

```c
int __io_putchar(int ch)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 100);
    return ch;
}
```

> 注意：这是**阻塞**发送，单字节 100ms 超时。M3 灌帧时会和接收抢时间，那时再换成"日志走独立环形缓冲、低优先级任务发"。现在够用。

---

## 6. 生成工程

`Project Manager`：

| 项 | 值 |
|---|---|
| Code Generator → Generate peripheral initialization as a pair of .c/.h files | **勾上**（否则全挤在 main.c 里） |
| Keep User Code when re-generating | 默认即可 |
| Delete previously generated files | 不勾 |

生成后目录里应该有 `Core/Src/main.c`、`spi.c`、`gpio.c`、`usart.c`。

## 7. 把驱动放进来

```
Core/
├── Inc/   ← 放 st7789.h
└── Src/   ← 放 st7789.c
```

`main.c` 里**只在 USER CODE 区间**改这几行：

```c
/* USER CODE BEGIN Includes */
#include "st7789.h"
/* USER CODE END Includes */

/* USER CODE BEGIN 2 */
printf("HCLK=%lu MHz\r\n", HAL_RCC_GetHCLKFreq() / 1000000u);
st7789_init();
st7789_test_colors();        /* 先看三纯色 */
/* USER CODE END 2 */
```

## 8. 上屏顺序（严格按这个来，每一步都能独立证伪）

1. **背光亮不亮**（BL 接 3.3V）—— 不亮是硬件，别动代码；
2. **刷红** —— 亮且红 = SPI 通、初始化序列对；
3. **刷绿、刷蓝** —— 三色都对 = 数据线没接反；
4. **棋盘格** —— 无错位 = `CASET/RASET` 和偏移对；
5. **全屏填充计时** —— 逻辑分析仪量，记进 `06-metrics.md`；
6. **提频到 22.5 / 45** —— 记下哪一档开始花屏。

## 9. 症状对照

| 现象 | 先查 |
|---|---|
| 背光不亮 | BL 是否接 3.3V；屏是否 3.3V 版本 |
| 全白 / 全黑，三纯色无反应 | `TFT_DC` 接反；`TFT_RST` 没拉低再放；SPI 模式不是 mode 0 |
| 花屏雪花 | PA5/PA7 输出速度不是 Very High；线太长；GND 没共 |
| 画面整体偏移一半/右边一条 | `ST7789_X_OFFSET` / `Y_OFFSET`；240×320 面板应为 0 |
| 红蓝互换 | `MADCTL` 的 BGR 位（`0x08`）；或 RGB565 高低字节顺序 |
| 颜色像底片 | 把 `INVON(0x21)` 换成 `INVOFF(0x20)` |
| 只有左上 1/4 有画面 | `MADCTL` 的 MX/MY 与分辨率不匹配（旋转设置错） |
