/**
 * lv_port_disp.h —— LVGL 显示移植层接口
 */
#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LVGL 显示驱动（必须在 st7789_init() 之后、lv_init() 之后调用） */
void lv_port_disp_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_DISP_H */