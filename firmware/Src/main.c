/**
 * main.c —— M5：FreeRTOS 通信、UI、监控三任务
 *
 * 数据流：
 *   PC 5Hz 发 16 字节帧 → USART2 循环 DMA + IDLE/HT/TC → 环形缓冲
 *     → comms 解析并回显 → 单槽覆盖队列 → ui 独占 LVGL
 *   mon 检查断连与两任务进度，健康时才喂 IWDG。
 *
 * 界面（LVGL 8.3）：
 *   顶部：转向符号（LVGL 内置 symbol 字体，不需要自己做字库）
 *   中部：距离大数字
 *   底部：车速 / 剩余里程 / ETA + 状态条（绿=正常，红=偏航或断连）
 *
 * 断连：3 秒没有 PROTO_OK → 显示 "--" 并把状态条置红
 */
#include "main.h"
#include "st7789.h"
#include "lv_port_disp.h"
#include "proto.h"
#include "lvgl.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "nav_transport.h"

SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart2;
#if NAV_USE_BLE
UART_HandleTypeDef huart1;
#endif
DMA_HandleTypeDef hdma_nav_rx;

/* ------------------------------------------------------------------ 接收环形缓冲 */
#define RB_SIZE 256u
static volatile uint8_t  g_rb[RB_SIZE];
static volatile uint16_t g_rb_wr, g_rb_rd;
static volatile uint32_t g_rb_overflow;
#define RX_DMA_SIZE 64u
static uint8_t s_rx_dma[RX_DMA_SIZE];
static uint16_t s_dma_cursor;
static volatile uint32_t s_rx_restart;
static TaskHandle_t s_comms_handle, s_ui_handle;
static QueueHandle_t s_nav_queue;
static volatile uint32_t s_last_rx;
static volatile uint32_t s_have_rx, s_link_lost;

/* Debugger-readable metrics; stacks are reported as unused bytes. */
volatile struct {
    uint32_t comms_heartbeat, ui_heartbeat, mon_heartbeat;
    uint32_t frames_ok, rx_overflow, uart_errors;
    uint32_t comms_stack_free, ui_stack_free, mon_stack_free;
    uint32_t link_lost, watchdog_feeds;
    uint32_t dma_idle_events, dma_half_events, dma_full_events;
    uint32_t dma_bytes, dma_restarts, dma_start_failures;
    uint32_t nav_source; /* 0: USART2 VCP, 1: USART1 BLE */
} g_app_metrics;

static inline int ring_pop(uint8_t *out)
{
    if (g_rb_rd == g_rb_wr) return 0;
    *out = g_rb[g_rb_rd];
    g_rb_rd = (uint16_t)((g_rb_rd + 1u) & (RB_SIZE - 1u));
    return 1;
}

static void ring_push(uint8_t byte)
{
    uint16_t next = (uint16_t)((g_rb_wr + 1u) & (RB_SIZE - 1u));
    if (next == g_rb_rd) g_rb_overflow++;
    else {
        g_rb[g_rb_wr] = byte;
        g_rb_wr = next;
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance == NAV_UART_INSTANCE) {
        BaseType_t wake = pdFALSE;
        uint16_t position;
        HAL_UART_RxEventTypeTypeDef event = HAL_UARTEx_GetRxEventType(huart);
        (void)size;
        if (event == HAL_UART_RXEVENT_IDLE) g_app_metrics.dma_idle_events++;
        else if (event == HAL_UART_RXEVENT_HT) g_app_metrics.dma_half_events++;
        else if (event == HAL_UART_RXEVENT_TC) g_app_metrics.dma_full_events++;
        if (s_rx_restart) return;
        /* Sample the live DMA cursor: a delayed HT/TC callback's Size can be
         * older than a preceding IDLE event. Both IRQs use the same priority. */
        position = (uint16_t)((RX_DMA_SIZE - __HAL_DMA_GET_COUNTER(huart->hdmarx)) % RX_DMA_SIZE);
        __DMB();
        while (s_dma_cursor != position) {
            ring_push(s_rx_dma[s_dma_cursor]);
            s_dma_cursor = (uint16_t)((s_dma_cursor + 1u) % RX_DMA_SIZE);
            g_app_metrics.dma_bytes++;
        }
        vTaskNotifyGiveFromISR(s_comms_handle, &wake);
        portYIELD_FROM_ISR(wake);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == NAV_UART_INSTANCE) {
        BaseType_t wake = pdFALSE;
        g_app_metrics.uart_errors++;
        s_rx_restart = 1;
        vTaskNotifyGiveFromISR(s_comms_handle, &wake);
        portYIELD_FROM_ISR(wake);
    }
}

static int restart_rx_dma(void)
{
    HAL_StatusTypeDef status;
    /* Only mask the two producer IRQs; leave SysTick available for HAL timeouts. */
    HAL_NVIC_DisableIRQ(NAV_UART_IRQ);
    HAL_NVIC_DisableIRQ(NAV_DMA_IRQ);
    (void)HAL_UART_AbortReceive(&NAV_UART_HANDLE);
    __HAL_UART_CLEAR_PEFLAG(&NAV_UART_HANDLE);
    s_dma_cursor = 0;
    g_rb_rd = g_rb_wr = 0;
    HAL_NVIC_ClearPendingIRQ(NAV_DMA_IRQ);
    HAL_NVIC_ClearPendingIRQ(NAV_UART_IRQ);
    s_rx_restart = 0;
    status = HAL_UARTEx_ReceiveToIdle_DMA(&NAV_UART_HANDLE, s_rx_dma, RX_DMA_SIZE);
    if (status != HAL_OK) {
        s_rx_restart = 1;
        g_app_metrics.dma_start_failures++;
    } else g_app_metrics.dma_restarts++;
    HAL_NVIC_EnableIRQ(NAV_DMA_IRQ);
    HAL_NVIC_EnableIRQ(NAV_UART_IRQ);
    return status == HAL_OK;
}

/* ------------------------------------------------------------------ LVGL 界面 */
static lv_obj_t *s_lbl_arrow;
static lv_obj_t *s_lbl_dist;
static lv_obj_t *s_lbl_info;
static lv_obj_t *s_bar;

static void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_lbl_arrow = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_arrow, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_arrow, lv_color_hex(0x00E5FF), 0);
    lv_label_set_text(s_lbl_arrow, LV_SYMBOL_UP);
    lv_obj_align(s_lbl_arrow, LV_ALIGN_TOP_MID, 0, 16);

    s_lbl_dist = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_dist, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_dist, lv_color_white(), 0);
    lv_label_set_text(s_lbl_dist, "--");
    lv_obj_align(s_lbl_dist, LV_ALIGN_CENTER, 0, -20);

    s_lbl_info = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0xA0A0A0), 0);
    lv_label_set_text(s_lbl_info, "waiting...");
    lv_obj_align(s_lbl_info, LV_ALIGN_BOTTOM_MID, 0, -40);

    s_bar = lv_obj_create(scr);
    lv_obj_set_size(s_bar, 240, 14);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
}

static const char *turn_symbol(uint8_t turn)
{
    switch (turn) {
    case TURN_LEFT:
    case TURN_SLIGHT_LEFT:  return LV_SYMBOL_LEFT;
    case TURN_RIGHT:
    case TURN_SLIGHT_RIGHT: return LV_SYMBOL_RIGHT;
    case TURN_UTURN:        return LV_SYMBOL_REFRESH;
    case TURN_ARRIVE:       return LV_SYMBOL_OK;
    default:                return LV_SYMBOL_UP;
    }
}

static void ui_render_nav(const nav_state_t *s)
{
    lv_label_set_text(s_lbl_arrow, turn_symbol(s->turn));

    if (s->dist_m == 0xFFFFu) {
        lv_label_set_text(s_lbl_dist, "--");
    } else {
        lv_label_set_text_fmt(s_lbl_dist, "%u", (unsigned)s->dist_m);
    }

    lv_label_set_text_fmt(s_lbl_info, "%u km/h   %u.%u km   %u min",
                          (unsigned)s->speed_kmh,
                          (unsigned)(s->remain_dam / 100u),
                          (unsigned)((s->remain_dam % 100u) / 10u),
                          (unsigned)s->eta_min);

    lv_obj_set_style_bg_color(s_bar,
        (s->flags & FLAG_OFF_ROUTE) ? lv_color_hex(0xD00000) : lv_color_hex(0x00B000), 0);
}

static void ui_render_lost(void)
{
    lv_label_set_text(s_lbl_arrow, LV_SYMBOL_WARNING);
    lv_label_set_text(s_lbl_dist, "--");
    lv_label_set_text(s_lbl_info, "signal lost");
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xD00000), 0);
}

/* ------------------------------------------------------------------ 看门狗
 * IWDG 由独立的 LSI（约 32kHz）驱动，主循环卡死时必然复位。
 * 4 秒超时：既够长（一次 LVGL 全屏重绘约 111ms，留足余量），
 * 又够短（真死机时 4 秒内能救回来）。
 *
 * 复位次数记在 RTC 备份寄存器 BKP0R 里，复位不清零，
 * 这样不用示波器也能验证"看门狗到底有没有动作"。
 */
static IWDG_HandleTypeDef s_hiwdg;

static void wdg_init(void)
{
#ifdef DEBUG
    /* Only core-halt time is excluded; a running stalled task still resets. */
    __HAL_DBGMCU_FREEZE_IWDG();
#else
    /* Do not inherit a debugger freeze setting from a previous debug session. */
    __HAL_DBGMCU_UNFREEZE_IWDG();
#endif
    s_hiwdg.Instance       = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_32;   /* LSI 32kHz / 32 = 1kHz */
    s_hiwdg.Init.Reload    = 4000;                /* 4000 / 1000 = 4 秒 */
    if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK) {
        Error_Handler();
    }
}

static uint32_t boot_counter_inc(void)
{
    uint32_t v;

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();          /* 解锁备份域 */
    v = RTC->BKP0R;
    RTC->BKP0R = v + 1u;                 /* 每次上电/复位 +1 */
    return v;
}

/* ------------------------------------------------------------------ 初始化声明 */
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_Init(void);
static void MX_NAV_UART_Init(void);

static void comms_task(void *argument)
{
    proto_parser_t parser;
    nav_state_t state;
    uint8_t byte;
    (void)argument;
    proto_init(&parser);
    configASSERT(HAL_UARTEx_ReceiveToIdle_DMA(&NAV_UART_HANDLE, s_rx_dma, RX_DMA_SIZE) == HAL_OK);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        if (s_rx_restart) {
            proto_init(&parser);
            (void)restart_rx_dma();
        }
        while (ring_pop(&byte)) {
            uint32_t now = HAL_GetTick();
            if (proto_feed(&parser, byte, now, &state) == PROTO_OK) {
                char echo[16];
                int n = 0;
                uint32_t v = state.seq;
                taskENTER_CRITICAL();
                s_last_rx = now;
                s_have_rx = 1;
                s_link_lost = 0;
                taskEXIT_CRITICAL();
                (void)xQueueOverwrite(s_nav_queue, &state);
                g_app_metrics.frames_ok++;
                echo[n++] = 'O'; echo[n++] = 'K'; echo[n++] = ' ';
                if (v >= 100u) echo[n++] = (char)('0' + (v / 100u) % 10u);
                if (v >= 10u) echo[n++] = (char)('0' + (v / 10u) % 10u);
                echo[n++] = (char)('0' + v % 10u);
                echo[n++] = '\r'; echo[n++] = '\n';
                (void)HAL_UART_Transmit(&NAV_UART_HANDLE, (uint8_t *)echo, (uint16_t)n, 20);
#if NAV_USE_BLE
                /* Keep VCP diagnostics available without mixing its RX bytes into BLE. */
                (void)HAL_UART_Transmit(&huart2, (uint8_t *)echo, (uint16_t)n, 20);
#endif
            }
        }
        proto_tick(&parser, HAL_GetTick());
        g_app_metrics.comms_heartbeat++;
    }
}

static void ui_task(void *argument)
{
    nav_state_t state;
    uint32_t last_tick;
    int showing_lost = 0;
    (void)argument;
    /* Every LVGL call, including initialization, belongs to this task. */
    st7789_init();
    lv_init();
    lv_port_disp_init();
    ui_init();
    last_tick = HAL_GetTick();
    for (;;) {
        uint32_t now = HAL_GetTick();
        int updated = xQueueReceive(s_nav_queue, &state, 0) == pdPASS;
        if (s_link_lost) {
            if (!showing_lost) ui_render_lost();
            showing_lost = 1;
        } else if (updated) {
            ui_render_nav(&state);
            showing_lost = 0;
        }
        lv_tick_inc((uint32_t)(now - last_tick));
        last_tick = now;
        (void)lv_timer_handler();
        g_app_metrics.ui_heartbeat++;
#ifdef WDG_SELFTEST
        /* Freeze only UI: comms and monitor keep running, monitor must stop feeding. */
        if (now > 5000u) vTaskSuspend(NULL);
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void monitor_task(void *argument)
{
    uint32_t last_comms = 0, last_ui = 0;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    (void)argument;
    for (;;) {
        uint32_t comms = g_app_metrics.comms_heartbeat;
        uint32_t ui = g_app_metrics.ui_heartbeat;
        taskENTER_CRITICAL();
        s_link_lost = s_have_rx && (uint32_t)(HAL_GetTick() - s_last_rx) > 3000u;
        taskEXIT_CRITICAL();
        if (comms != last_comms && ui != last_ui) {
            (void)HAL_IWDG_Refresh(&s_hiwdg);
            g_app_metrics.watchdog_feeds++;
            last_comms = comms;
            last_ui = ui;
        }
        g_app_metrics.rx_overflow = g_rb_overflow;
        g_app_metrics.link_lost = s_link_lost;
        g_app_metrics.comms_stack_free = uxTaskGetStackHighWaterMark(s_comms_handle) * sizeof(StackType_t);
        g_app_metrics.ui_stack_free = uxTaskGetStackHighWaterMark(s_ui_handle) * sizeof(StackType_t);
        g_app_metrics.mon_stack_free = uxTaskGetStackHighWaterMark(self) * sizeof(StackType_t);
        g_app_metrics.mon_heartbeat++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *depth)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
    *tcb = &idle_tcb;
    *stack = idle_stack;
    *depth = configMINIMAL_STACK_SIZE;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    Error_Handler();
}

int main(void)
{
    static StaticTask_t comms_tcb, ui_tcb, monitor_tcb;
    static StackType_t comms_stack[512], ui_stack[2048], monitor_stack[512];
    static StaticQueue_t queue_control;
    static uint8_t queue_storage[sizeof(nav_state_t)];
    TaskHandle_t monitor;
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_USART2_Init();
    MX_NAV_UART_Init();
    g_app_metrics.nav_source = NAV_USE_BLE;
    (void)boot_counter_inc();
    wdg_init();
    s_nav_queue = xQueueCreateStatic(1, sizeof(nav_state_t), queue_storage, &queue_control);
    s_comms_handle = xTaskCreateStatic(comms_task, "comms", 512, NULL, 3, comms_stack, &comms_tcb);
    s_ui_handle = xTaskCreateStatic(ui_task, "ui", 2048, NULL, 2, ui_stack, &ui_tcb);
    monitor = xTaskCreateStatic(monitor_task, "mon", 512, NULL, 1, monitor_stack, &monitor_tcb);
    configASSERT(s_nav_queue && s_comms_handle && s_ui_handle && monitor);
    vTaskStartScheduler();
    Error_Handler();
}

/* ------------------------------------------------------------------ 时钟 */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 8;
    RCC_OscInitStruct.PLL.PLLN       = 360;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 8;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }
}

/* ------------------------------------------------------------------ GPIO */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_GPIO_WritePin(GPIOB, TFT_CS_Pin | TFT_RST_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, TFT_DC_Pin, GPIO_PIN_RESET);

    gpio.Pin   = TFT_CS_Pin | TFT_DC_Pin | TFT_RST_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_GPIO_WritePin(MEAS_GPIO_Port, MEAS_Pin, GPIO_PIN_RESET);
    gpio.Pin   = MEAS_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MEAS_GPIO_Port, &gpio);
}

/* ------------------------------------------------------------------ SPI1 */
static void MX_SPI1_Init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();

    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        Error_Handler();
    }
}

/* ------------------------------------------------------------------ USART2 */
static void MX_USART2_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) {
        Error_Handler();
    }

}

static void MX_NAV_UART_Init(void)
{
#if NAV_USE_BLE
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 9600;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
#else
    __HAL_RCC_DMA1_CLK_ENABLE();
#endif
    hdma_nav_rx.Instance = NAV_DMA_STREAM;
    hdma_nav_rx.Init.Channel = DMA_CHANNEL_4;
    hdma_nav_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_nav_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_nav_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_nav_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_nav_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_nav_rx.Init.Mode = DMA_CIRCULAR;
    hdma_nav_rx.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_nav_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_nav_rx) != HAL_OK) Error_Handler();
    __HAL_LINKDMA(&NAV_UART_HANDLE, hdmarx, hdma_nav_rx);
    HAL_NVIC_SetPriority(NAV_UART_IRQ, 5, 0);
    HAL_NVIC_EnableIRQ(NAV_UART_IRQ);
    HAL_NVIC_SetPriority(NAV_DMA_IRQ, 5, 0);
    HAL_NVIC_EnableIRQ(NAV_DMA_IRQ);
    /* Reception is armed by comms after the scheduler and task handle are ready. */
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {
        /* 卡在这里说明初始化失败 */
    }
}
