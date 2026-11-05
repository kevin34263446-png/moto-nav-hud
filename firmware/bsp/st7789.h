/**
 * st7789.h —— ST7789 240x320 SPI 屏驱动（M2 用，裸机 + 阻塞式 SPI）
 *
 * 设计取舍：
 *   1. 只用阻塞式 HAL_SPI_Transmit。M2 的目标是"点亮"，不是"快"。
 *      等屏能稳定显示，M4 再换成 DMA —— 一次只改一个变量。
 *   2. 片选/DC/RST 全部软件控制（CubeMX 里不开 Hardware NSS），
 *      因为 ST7789 的 DC 语义无法用硬件 NSS 表达。
 *   3. 引脚宏用 CubeMX 的 User Label 生成（TFT_CS / TFT_DC / TFT_RST / MEAS），
 *      配法见 docs/07-cubemx-setup.md §4。这样改了引脚不用回来改驱动。
 *
 * 接线（docs/03-hardware.md）：
 *   SCK=PA5  MOSI=PA7  CS=PB6  DC=PB5  RST=PB4  VCC/BL=3.3V  MISO 不接
 */
#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

/* ------------------------------------------------------------------ 面板参数
 * 240x320 面板 GRAM 就是 240x320，偏移为 0。
 * 如果你买的是 240x240 方屏，Y 偏移必须设成 80，否则画面会整体偏上/偏下 ——
 * 这是"画面偏移一半"最常见的原因，见 docs/07-cubemx-setup.md §9。
 */
#define ST7789_WIDTH     240u
#define ST7789_HEIGHT    320u
#define ST7789_X_OFFSET  0u
#define ST7789_Y_OFFSET  0u

/* MADCTL 旋转/镜像：
 *   0x00 竖屏正常
 *   0x60 横屏（MX|MV，同时要把 WIDTH/HEIGHT 互换）
 *   0xC0 竖屏 180°
 *   0x08 位是 BGR 顺序 —— 红蓝互换时把它打开
 */
#define ST7789_MADCTL    0x00u

/* ------------------------------------------------------------------ 颜色
 * RGB565：R5 G6 B5，高字节先发（ST7789 要求大端）。
 */
#define RGB565(r, g, b) \
    ((uint16_t)((((uint16_t)(r) & 0xF8u) << 8) | \
                (((uint16_t)(g) & 0xFCu) << 3) | \
                (((uint16_t)(b) & 0xF8u) >> 3)))

#define COLOR_BLACK   RGB565(0, 0, 0)
#define COLOR_WHITE   RGB565(255, 255, 255)
#define COLOR_RED     RGB565(255, 0, 0)
#define COLOR_GREEN   RGB565(0, 255, 0)
#define COLOR_BLUE    RGB565(0, 0, 255)
#define COLOR_YELLOW  RGB565(255, 255, 0)
#define COLOR_CYAN    RGB565(0, 255, 255)
#define COLOR_GRAY    RGB565(128, 128, 128)

/* ------------------------------------------------------------------ API */

/* 上电初始化。内部含硬件复位 + 初始化序列，耗时约 300ms。 */
void st7789_init(void);

/* 设置写入窗口并把光标移到 (x0,y0)，函数返回后可以连续写像素数据。 */
void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* 全屏填充。内含测量点 MEAS 翻转，可用逻辑分析仪量耗时。 */
void st7789_fill(uint16_t color);

/* 矩形填充。x/y 为左上角，w/h 为宽高，超出屏幕的部分自动裁掉。 */
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color);

/* 单像素。 */
void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/* ------------------------------------------------------------------ 验收用
 * M2 的验收顺序就靠这两个函数，见 docs/04-bringup.md 阶段 1。
 */
void st7789_test_colors(void);        /* 红→绿→蓝→黑→白，每色 800ms */
void st7789_test_checker(uint8_t cell); /* 棋盘格，cell 为格子边长（建议 16/32） */

#endif /* ST7789_H */
