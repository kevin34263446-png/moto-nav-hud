# 如何把协议核心搬进 CubeIDE

> `firmware/proto/` 是**平台无关的纯 C**，只依赖 `<stdint.h>` 和 `<string.h>`。
> 这一页讲清楚三件事：放在哪、怎么喂字节、以及新手最容易卡住的四个坑。

---

## 1. 文件放置

```
<CubeIDE 工程>/
├── Core/
│   ├── Inc/            # 不需要动
│   ├── Src/
│   │   ├── main.c
│   │   ├── proto.h         ← 复制进来（或放 Inc/）
│   │   ├── proto_parser.c  ← 复制进来
│   │   └── app.c           ← 你自己写的胶水层
```

- 两个文件都复制进去后，CubeIDE 会自动把 `Core/Src` 加进编译（新文件需要按 `F5` 刷新工程树）。
- 若把 `proto.h` 放 `Core/Inc`，则 `#include "proto.h"` 不用改路径。

## 2. 唯一的规则：调用代码只能写在 USER CODE 区间

CubeMX 重新生成代码会覆盖 `main.c` / `stm32f4xx_it.c`，但**不会删除你新增的 .c/.h 文件**。
所以：

```c
/* USER CODE BEGIN Includes */
#include "proto.h"
/* USER CODE END Includes */
```

> 不加这个标记，你每改一次 CubeMX 配置就要重贴一遍代码。这是新手第一个星期最常白干的地方。

---

## 3. 喂字节：从简到繁分两步

### 第 1 步（M3 先这么干）：逐字节中断，30 行跑通

```c
static uint8_t g_rx;                                     /* 单字节接收缓冲 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {                     /* M3 用板载 VCP */
        ring_push(g_rx);                                 /* 中断里只做这件事 */
        HAL_UART_Receive_IT(&huart2, &g_rx, 1);          /* 立刻重新 arm */
    }
}

/* main 初始化里 */
HAL_UART_Receive_IT(&huart2, &g_rx, 1);
```

5Hz × 16 字节 = 80 B/s，9600 波特下平均每 1ms 一个中断，**完全够用**。
先让这条路通，再去换 DMA——一次只改一个变量。

### 第 2 步（简历级写法）：DMA + 空闲中断

```c
static uint8_t g_dma_buf[64];

void app_uart_start(void)
{
    HAL_UARTEx_ReceiveToIdle_DMA(&huart2, g_dma_buf, sizeof(g_dma_buf));
    __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);   /* 只用空闲事件，不要半满事件 */
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART2) {
        for (uint16_t i = 0; i < Size; i++) ring_push(g_dma_buf[i]);
        HAL_UARTEx_ReceiveToIdle_DMA(&huart2, g_dma_buf, sizeof(g_dma_buf));  /* 必须重新 arm */
    }
}
```

**必须重新 arm**：`ReceiveToIdle_DMA` 是单次触发，事件回调里不复位就再也收不到数据。
症状是"发第一帧有反应，之后就死了"，而这个坑会花掉你半天。

---

## 4. 环形缓冲：单生产者单消费者，无锁

```c
#define RB_SIZE 256u                       /* 必须是 2 的幂 */
static volatile uint16_t rb_wr, rb_rd;
static uint8_t rb_buf[RB_SIZE];
static volatile uint32_t rb_overflow;      /* 覆盖计数，打进日志 */

static inline void ring_push(uint8_t b)    /* 中断上下文 */
{
    uint16_t next = (uint16_t)((rb_wr + 1u) & (RB_SIZE - 1u));
    if (next == rb_rd) { rb_overflow++; return; }   /* 满：丢新的，不阻塞 */
    rb_buf[rb_wr] = b;
    rb_wr = next;
}

static inline int ring_pop(uint8_t *out)   /* 任务上下文 */
{
    if (rb_rd == rb_wr) return 0;
    *out = rb_buf[rb_rd];
    rb_rd = (uint16_t)((rb_rd + 1u) & (RB_SIZE - 1u));
    return 1;
}
```

要点：
- `rb_wr` / `rb_rd` 必须是 `volatile`，且只有一个写者、一个读者，所以不需要关中断；
- **满了丢新的并计数**，绝不在中断里等；
- `rb_overflow` 打到串口日志里——它非 0 就说明"消费得比生产慢"，是真 bug 的信号。

---

## 5. 在任务/超循环里消费

```c
nav_state_t st;
uint8_t b;

while (ring_pop(&b)) {
    if (proto_feed(&g_parser, b, HAL_GetTick(), &st) == PROTO_OK) {
        g_last_rx_ms = HAL_GetTick();     /* 断连判定的唯一依据：解析成功 */
        g_state = st;                     /* 最新值覆盖语义 */
    }
}
```

两条纪律：
- **`proto_feed()` 不在中断里调**；
- **断连计时用"解析成功"的时刻**，不是"收到字节"的时刻。收到噪声说明链路在动但协议不通，那不是"已连接"。

---

## 6. 四个必踩的坑

| 坑 | 现象 | 处理 |
|---|---|---|
| `HAL_GetTick()` 与 FreeRTOS tick 冲突 | 超时判断失灵、`proto_feed` 里 50ms 超时乱触发 | 上 RTOS 后确认 `SysTick_Handler` 同时调了 `HAL_IncTick()` 和 RTOS 的 tick；或把 `now_ms` 换成 `xTaskGetTickCount()` 并相应放大 `PROTO_TIMEOUT_MS` |
| `printf` 重定向阻塞/在中断里调用 | 卡死、丢帧 | 只重定向 `_write` 到阻塞发送；**绝不在中断里 printf**；调试期可以用 `--log` 让上位机代收 |
| `ui` 任务栈太小 | 随机 HardFault、花屏后死机 | LVGL 调用深度大，`ui` 栈 ≥ 4KB（先给 8KB 再压）；用 `vTaskList()` 看余量 ≥128B |
| 协议数据和 `printf` 抢同一个 UART | 收到混杂字符、日志与帧互相污染 | VCP 上把 `printf` 和帧都交给上位机 `--log` 解析；或日志走 USART2、帧走 USART1 |

---

## 7. 搬完先跑这 4 项

1. 板子上电，`printf("parser init\r\n")` 正常 → 串口通了；
2. 用串口助手手发一帧样例，`printf` 打出 `seq/turn/dist` 正确 → 协议通了；
3. `python tools/sim_sender.py --port COMx --log` 5Hz 灌帧，打印稳定 → 吞吐通了；
4. `--fault badcrc` 连续注入，`n_drop_crc` 涨但 `n_ok` 继续涨 → 重同步对了。

四项全过再动屏幕。屏幕出问题的时候，你需要一个**已被证明可靠的数据源**。
