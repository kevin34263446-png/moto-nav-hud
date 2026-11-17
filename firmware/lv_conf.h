/**
 * lv_conf.h —— LVGL 8.3 配置（只写与默认值不同的项）
 *
 * 未在此定义的宏由 LVGL 的 lv_conf_internal.h 填默认值，所以这份文件很短。
 * 编译时需要在 include 路径里加上本目录，并定义 LV_CONF_INCLUDE_SIMPLE。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- 颜色 ---- */
/* 16 位色深 = RGB565，与 ST7789 的 COLMOD=0x55 对应 */
#define LV_COLOR_DEPTH 16

/* 关键：把 RGB565 的高低位对调。
 * LVGL 默认按 CPU 小端存放颜色，而 ST7789 要求大端（高字节先发）。
 * 打开这个开关后 lv_color_t 的内存布局直接就是 SPI 要发的顺序，
 * 刷屏时可以把缓冲区原样 memcpy 出去，不用逐像素交换。 */
#define LV_COLOR_16_SWAP 1

/* ---- 内存 ---- */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (32U * 1024U)     /* LVGL 自己的堆，128KB RAM 里占 32KB */

/* ---- 显示刷新周期（毫秒）---- */
#define LV_DISP_DEF_REFR_PERIOD 30

/* ---- 字体：只开需要的，省 Flash ---- */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ---- 关掉不需要的功能 ---- */
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

/* ---- 开一个简单的按键/输入设备框架（V1 暂时不用触摸）---- */
#define LV_USE_GROUP 1

#endif /* LV_CONF_H */