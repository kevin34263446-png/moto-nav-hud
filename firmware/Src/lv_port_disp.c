/**
 * lv_port_disp.c —— LVGL 8.3 显示移植层
 *
 * 关键设计：
 *   1. 缓冲区 240×40 = 19.2KB（RAM 只有 128KB，全屏缓冲 150KB 不可能）
 *   2. 单缓冲 + 同步刷新：flush_cb 里阻塞发完再调 lv_disp_flush_ready()。
 *      M4 阶段优先保证正确性，等情况稳定再换双缓冲 + DMA 提速度。
 *   3. 颜色顺序：lv_conf.h 里 LV_COLOR_16_SWAP=1，使 LVGL 的缓冲区
 *      直接就是 ST7789 要的大端顺序，刷屏时不用逐像素交换。
 */
#include "lv_port_disp.h"
#include "lvgl.h"
#include "main.h"
#include "st7789.h"

#define DISP_W          240
#define DISP_H          320
#define DISP_BUF_LINES  40          /* 一次刷 40 行，占 RAM 19.2KB */

static lv_color_t        s_buf[DISP_W * DISP_BUF_LINES];
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;

static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    st7789_set_window((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)area->x2, (uint16_t)area->y2);
    st7789_push_pixels(color_p, w * h);

    /* 同步刷新：发完立刻告诉 LVGL 这块可以重用 */
    lv_disp_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, DISP_W * DISP_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = DISP_W;
    s_disp_drv.ver_res  = DISP_H;
    s_disp_drv.flush_cb = disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf;

    lv_disp_drv_register(&s_disp_drv);
}