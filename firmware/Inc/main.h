/**
 * main.h —— 板级与引脚定义
 *
 * 引脚分配见 docs/03-hardware.md：
 *   TFT_CS = PB6   TFT_DC = PB5   TFT_RST = PB4
 *   SPI1:  SCK = PA5(AF5)  MOSI = PA7(AF5)
 *   MEAS = PA8  （逻辑分析仪测量标记，没接也不影响）
 *
 * 注意：PA5 同时是板载 LD2，SPI 跑起来绿灯会跟着闪，属正常现象。
 */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* ---- TFT 控制引脚 ---- */
#define TFT_CS_Pin          GPIO_PIN_6
#define TFT_CS_GPIO_Port    GPIOB
#define TFT_DC_Pin          GPIO_PIN_5
#define TFT_DC_GPIO_Port    GPIOB
#define TFT_RST_Pin         GPIO_PIN_4
#define TFT_RST_GPIO_Port   GPIOB

/* ---- 测量标记引脚（接逻辑分析仪 CH4）---- */
#define MEAS_Pin            GPIO_PIN_8
#define MEAS_GPIO_Port      GPIOA

extern SPI_HandleTypeDef hspi1;

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */