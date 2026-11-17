/**
 * main.c —— M4：LVGL 界面 + 协议数据驱动
 *
 * 数据流：
 *   PC 5Hz 发 16 字节帧 → USART2 中断逐字节收 → 环形缓冲
 *     → 主循环喂 proto_feed() → PROTO_OK 时更新 LVGL 控件 + 回 "OK <seq>"
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

SPI_HandleTypeDef  hspi1;
UART_HandleTypeDef huart2;

/* ------------------------------------------------------------------ 接收环形缓冲 */
#define RB_SIZE 256u
static volatile uint8_t  g_rb[RB_SIZE];
static volatile uint16_t g_rb_wr, g_rb_rd;
static volatile uint32_t g_rb_overflow;
static uint8_t g_rx_byte;

static inline int ring_pop(uint8_t *out)
{
    if (g_rb_rd == g_rb_wr) return 0;
    *out = g_rb[g_rb_rd];
    g_rb_rd = (uint16_t)((g_rb_rd + 1u) & (RB_SIZE - 1u));
    return 1;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        uint16_t next = (uint16_t)((g_rb_wr + 1u) & (RB_SIZE - 1u));
        if (next == g_rb_rd) {
            g_rb_overflow++;
        } else {
            g_rb[g_rb_wr] = g_rx_byte;
            g_rb_wr = next;
        }
        HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);
    }
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

/* ------------------------------------------------------------------ 初始化声明 */
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_Init(void);

int main(void)
{
    static proto_parser_t parser;
    static nav_state_t    st;
    uint8_t  b;
    uint32_t now, last_rx = 0, last_tick;
    int      have_rx = 0, connected = 1;

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_USART2_Init();

    st7789_init();
    lv_init();
    lv_port_disp_init();
    ui_init();

    proto_init(&parser);
    memset(&st, 0, sizeof(st));

    last_tick = HAL_GetTick();

    while (1) {
        /* 1. LVGL 心跳与刷新 */
        now = HAL_GetTick();
        lv_tick_inc((uint32_t)(now - last_tick));
        last_tick = now;
        (void)lv_timer_handler();

        /* 2. 协议：把收到的字节全部喂给状态机 */
        while (ring_pop(&b)) {
            if (proto_feed(&parser, b, now, &st) == PROTO_OK) {
                char echo[16];
                int  n = 0;
                uint32_t v = st.seq;

                last_rx = now;
                have_rx = 1;

                if (!connected) {
                    connected = 1;
                }
                ui_render_nav(&st);

                echo[n++] = 'O'; echo[n++] = 'K'; echo[n++] = ' ';
                if (v >= 100u) echo[n++] = (char)('0' + (v / 100u) % 10u);
                if (v >= 10u)  echo[n++] = (char)('0' + (v / 10u) % 10u);
                echo[n++] = (char)('0' + (v % 10u));
                echo[n++] = '\r'; echo[n++] = '\n';
                (void)HAL_UART_Transmit(&huart2, (uint8_t *)echo, (uint16_t)n, 20);
            }
        }

        /* 3. 断连判定 */
        if (have_rx && connected && (uint32_t)(now - last_rx) > 3000u) {
            connected = 0;
            ui_render_lost();
        }
    }
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

    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {
        /* 卡在这里说明初始化失败 */
    }
}