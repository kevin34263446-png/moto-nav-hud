#ifndef NAV_TRANSPORT_H
#define NAV_TRANSPORT_H

#include "main.h"

#ifndef NAV_USE_BLE
#define NAV_USE_BLE 0
#endif

extern UART_HandleTypeDef huart2;
extern DMA_HandleTypeDef hdma_nav_rx;

#if NAV_USE_BLE
extern UART_HandleTypeDef huart1;
#define NAV_UART_HANDLE huart1
#define NAV_UART_INSTANCE USART1
#define NAV_UART_IRQ USART1_IRQn
#define NAV_UART_HANDLER USART1_IRQHandler
#define NAV_DMA_IRQ DMA2_Stream2_IRQn
#define NAV_DMA_HANDLER DMA2_Stream2_IRQHandler
#define NAV_DMA_STREAM DMA2_Stream2
#else
#define NAV_UART_HANDLE huart2
#define NAV_UART_INSTANCE USART2
#define NAV_UART_IRQ USART2_IRQn
#define NAV_UART_HANDLER USART2_IRQHandler
#define NAV_DMA_IRQ DMA1_Stream5_IRQn
#define NAV_DMA_HANDLER DMA1_Stream5_IRQHandler
#define NAV_DMA_STREAM DMA1_Stream5
#endif

#endif
