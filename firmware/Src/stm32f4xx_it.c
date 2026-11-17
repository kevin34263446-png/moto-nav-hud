/**
 * stm32f4xx_it.c —— 中断入口
 *
 * SysTick 给 HAL_Delay / HAL_GetTick 提供时基。
 * USART2 是板载 VCP，收字节用（M3 协议链路）。
 *
 * 纪律：中断里只搬字节，不做 CRC、不解析、不打印。
 */
#include "main.h"

extern UART_HandleTypeDef huart2;

void NMI_Handler(void)        { while (1) { } }
void HardFault_Handler(void)  { while (1) { } }
void MemManage_Handler(void)  { while (1) { } }
void BusFault_Handler(void)   { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void SVC_Handler(void)        { }
void DebugMon_Handler(void)   { }
void PendSV_Handler(void)     { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}