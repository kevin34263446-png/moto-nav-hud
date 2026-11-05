/**
 * st7789.c —— ST7789 240x320 SPI 屏驱动实现
 *
 * ============================ 未在硬件上验证过 ============================
 * 这份代码是对着 ST7789 数据手册和常见模组的初始化序列写的，逻辑保守、
 * 只用最基础的 HAL 调用。第一次烧上去请严格按 docs/04-bringup.md 阶段 1 的顺序看：
 *
 *   1) 背光亮不亮          —— 不亮是硬件，别改代码
 *   2) st7789_test_colors() —— 红绿蓝三色对 = SPI 通 + 初始化序列对
 *   3) st7789_test_checker(16) —— 无错位 = CASET/RASET 和偏移对
 *   4) 提 SPI 频率到 22.5 / 45MHz —— 记录花屏起始档位
 *
 * 三色不对时按 docs/07-cubemx-setup.md §9 的症状表逐条排查，不要盲改初始化序列。
 * ==========================================================================
 */
#include "main.h"
#include "st7789.h"

#include <stddef.h>   /* NULL */

/* 引脚宏由 CubeMX 按 User Label 自动生成到 main.h */
#ifndef TFT_CS_Pin
#error "main.h 里没有 TFT_CS_Pin：请在 CubeMX 里把 PB6/PB5/PB4 的 User Label 设为 TFT_CS/TFT_DC/TFT_RST。见 docs/07-cubemx-setup.md §4"
#endif

extern SPI_HandleTypeDef hspi1;

#define SPI_TIMEOUT_MS   1000u
#define CHUNK_PX         64u          /* 每次连续发送的像素数（缓冲 128 字节） */

/* ---------------------------------------------------------------- 引脚 */

static inline void cs_low(void)
{
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_high(void)
{
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

static inline void dc_cmd(void)
{
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET);
}

static inline void dc_data(void)
{
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);
}

/* 测量标记：给逻辑分析仪量刷屏耗时用。没配 PA8 就自动变成空操作。 */
#ifdef MEAS_Pin
#define MEAS_HIGH()  HAL_GPIO_WritePin(MEAS_GPIO_Port, MEAS_Pin, GPIO_PIN_SET)
#define MEAS_LOW()   HAL_GPIO_WritePin(MEAS_GPIO_Port, MEAS_Pin, GPIO_PIN_RESET)
#else
#define MEAS_HIGH()  do { } while (0)
#define MEAS_LOW()   do { } while (0)
#endif

/* ---------------------------------------------------------------- 底层传输 */

static void spi_tx(const uint8_t *d, uint16_t n)
{
    /* 阻塞发送：M2 够用。M4 换 DMA 时只改这一个函数。 */
    (void)HAL_SPI_Transmit(&hspi1, (uint8_t *)d, n, SPI_TIMEOUT_MS);
}

/**
 * 一条命令 + 若干参数，在整个 CS 低电平期间发完。
 * 不要每条参数单独拉一次 CS —— 部分模组会把中间的空闲当成命令结束。
 */
static void write_cmd(uint8_t cmd, const uint8_t *params, uint16_t n)
{
    cs_low();
    dc_cmd();
    spi_tx(&cmd, 1u);
    if (n > 0u) {
        dc_data();
        spi_tx(params, n);
    }
    cs_high();
}

/* ---------------------------------------------------------------- 初始化 */

/* 伽马表：这两组是 ST7789 模组最常见的取值，先别动，颜色不对先查 MADCTL/INVON */
static const uint8_t s_gamma_p[14] = {
    0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23
};
static const uint8_t s_gamma_n[14] = {
    0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23
};

void st7789_init(void)
{
    const uint8_t p;

    /* 1. 硬件复位：拉低 10ms，释放后必须等 ≥120ms 再发命令 */
    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(120);

    /* 2. 软复位 + 退出睡眠。这两步的延时是手册要求，不能省 */
    write_cmd(0x01, NULL, 0);            /* SWRESET */
    HAL_Delay(150);
    write_cmd(0x11, NULL, 0);            /* SLPOUT */
    HAL_Delay(120);

    /* 3. 颜色格式：0x55 = 16bit RGB565 */
    p = 0x55;
    write_cmd(0x3A, &p, 1);              /* COLMOD */

    /* 4. 扫描方向 / 镜像 / BGR。红蓝互换就是这里的 BGR 位 */
    p = (uint8_t)ST7789_MADCTL;
    write_cmd(0x36, &p, 1);              /* MADCTL */

    /* 5. 电源与门控参数（模组通用值） */
    {
        const uint8_t b2[5] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
        const uint8_t b7[1] = { 0x35 };
        const uint8_t bb[1] = { 0x19 };  /* VCOM：画面偏暗/闪烁时试 0x28 */
        const uint8_t c0[1] = { 0x2C };
        const uint8_t c2[1] = { 0x01 };
        const uint8_t c3[1] = { 0x12 };
        const uint8_t c4[1] = { 0x20 };
        const uint8_t c6[1] = { 0x0F };  /* 60Hz 刷新 */
        const uint8_t d0[2] = { 0xA4, 0xA1 };

        write_cmd(0xB2, b2, sizeof(b2)); /* PORCTRL  */
        write_cmd(0xB7, b7, sizeof(b7)); /* GCTRL    */
        write_cmd(0xBB, bb, sizeof(bb)); /* VCOMS    */
        write_cmd(0xC0, c0, sizeof(c0)); /* LCMCTRL  */
        write_cmd(0xC2, c2, sizeof(c2)); /* VDVVRHEN */
        write_cmd(0xC3, c3, sizeof(c3)); /* VRHS     */
        write_cmd(0xC4, c4, sizeof(c4)); /* VDVS     */
        write_cmd(0xC6, c6, sizeof(c6)); /* FRCTRL2  */
        write_cmd(0xD0, d0, sizeof(d0)); /* PWCTRL1  */
    }
    write_cmd(0xE0, s_gamma_p, sizeof(s_gamma_p));
    write_cmd(0xE1, s_gamma_n, sizeof(s_gamma_n));

    /* 6. 反转与显示开关。颜色像底片 → 把 0x21 换成 0x20 */
    write_cmd(0x21, NULL, 0);            /* INVON  */
    write_cmd(0x13, NULL, 0);            /* NORON  */
    write_cmd(0x29, NULL, 0);            /* DISPON */
    HAL_Delay(20);
}

/* ---------------------------------------------------------------- 绘图 */

void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];
    uint16_t xs = (uint16_t)(x0 + ST7789_X_OFFSET);
    uint16_t xe = (uint16_t)(x1 + ST7789_X_OFFSET);
    uint16_t ys = (uint16_t)(y0 + ST7789_Y_OFFSET);
    uint16_t ye = (uint16_t)(y1 + ST7789_Y_OFFSET);

    b[0] = (uint8_t)(xs >> 8); b[1] = (uint8_t)(xs & 0xFFu);
    b[2] = (uint8_t)(xe >> 8); b[3] = (uint8_t)(xe & 0xFFu);
    write_cmd(0x2A, b, 4);               /* CASET */

    b[0] = (uint8_t)(ys >> 8); b[1] = (uint8_t)(ys & 0xFFu);
    b[2] = (uint8_t)(ye >> 8); b[3] = (uint8_t)(ye & 0xFFu);
    write_cmd(0x2B, b, 4);               /* RASET */

    write_cmd(0x2C, NULL, 0);            /* RAMWR：之后的数据就是像素 */
}

/* 在已经设好窗口的前提下，连续灌 count 个同色像素 */
static void burst_color(uint16_t color, uint32_t count)
{
    uint8_t buf[CHUNK_PX * 2u];
    uint16_t i;

    for (i = 0; i < CHUNK_PX; i++) {
        buf[2u * i]      = (uint8_t)(color >> 8);   /* RGB565 大端：高字节先发 */
        buf[2u * i + 1u] = (uint8_t)(color & 0xFFu);
    }

    cs_low();
    dc_data();
    while (count > 0u) {
        uint16_t n = (count > CHUNK_PX) ? (uint16_t)CHUNK_PX : (uint16_t)count;
        spi_tx(buf, (uint16_t)(n * 2u));
        count -= n;
    }
    cs_high();
}

void st7789_fill(uint16_t color)
{
    MEAS_HIGH();
    st7789_set_window(0, 0, (uint16_t)(ST7789_WIDTH - 1u), (uint16_t)(ST7789_HEIGHT - 1u));
    burst_color(color, (uint32_t)ST7789_WIDTH * (uint32_t)ST7789_HEIGHT);
    MEAS_LOW();
}

void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint16_t x1, y1;

    if ((w == 0u) || (h == 0u) || (x >= ST7789_WIDTH) || (y >= ST7789_HEIGHT)) {
        return;
    }
    /* 裁剪：超出屏的部分不能直接算进窗口，否则会把 GRAM 写错位 */
    x1 = (uint16_t)(x + w - 1u);
    y1 = (uint16_t)(y + h - 1u);
    if (x1 >= ST7789_WIDTH)  { x1 = (uint16_t)(ST7789_WIDTH - 1u); }
    if (y1 >= ST7789_HEIGHT) { y1 = (uint16_t)(ST7789_HEIGHT - 1u); }

    st7789_set_window(x, y, x1, y1);
    burst_color(color, (uint32_t)(x1 - x + 1u) * (uint32_t)(y1 - y + 1u));
}

void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    st7789_fill_rect(x, y, 1u, 1u, color);
}

/* ---------------------------------------------------------------- 验收用 */

void st7789_test_colors(void)
{
    static const uint16_t colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_BLACK, COLOR_WHITE
    };
    uint8_t i;

    for (i = 0; i < (uint8_t)(sizeof(colors) / sizeof(colors[0])); i++) {
        st7789_fill(colors[i]);
        HAL_Delay(800);
    }
}

void st7789_test_checker(uint8_t cell)
{
    uint16_t x, y;
    uint8_t on = 1u;

    if (cell == 0u) {
        cell = 16u;
    }
    for (y = 0; y < ST7789_HEIGHT; y = (uint16_t)(y + cell)) {
        for (x = 0; x < ST7789_WIDTH; x = (uint16_t)(x + cell)) {
            st7789_fill_rect(x, y, cell, cell, on ? COLOR_CYAN : COLOR_BLACK);
            on ^= 1u;
        }
        on ^= 1u;                        /* 行末再翻一次，上下行才错开 */
    }
}
