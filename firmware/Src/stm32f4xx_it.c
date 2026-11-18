/**
 * stm32f4xx_it.c —— 中断入口
 *
 * SysTick 给 HAL_Delay / HAL_GetTick 提供时基。
 * USART2 是板载 VCP，收字节用（M3 协议链路）。
 *
 * 纪律：中断里只搬字节，不做 CRC、不解析、不打印。
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "nav_transport.h"

extern void xPortSysTickHandler(void);


void NMI_Handler(void)        { while (1) { } }
void HardFault_Handler(void)  { while (1) { } }
void MemManage_Handler(void)  { while (1) { } }
void BusFault_Handler(void)   { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void DebugMon_Handler(void)   { }

void SysTick_Handler(void)
{
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

void NAV_UART_HANDLER(void)
{
    HAL_UART_IRQHandler(&NAV_UART_HANDLE);
}

void NAV_DMA_HANDLER(void)
{
    HAL_DMA_IRQHandler(&hdma_nav_rx);
}
