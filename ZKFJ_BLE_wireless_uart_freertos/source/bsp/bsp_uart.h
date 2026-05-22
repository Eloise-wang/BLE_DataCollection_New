/*
 * bsp_uart.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef BSP_BSP_UART_H_
#define BSP_BSP_UART_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef BSP_UART_ENABLE
#define BSP_UART_ENABLE 1
#endif

#if BSP_UART_ENABLE

#ifndef BSP_UART_TX_RING_SIZE
#define BSP_UART_TX_RING_SIZE 1024U
#endif

#ifndef BSP_UART_PRINTF_BUF_SIZE
#define BSP_UART_PRINTF_BUF_SIZE 256U
#endif

// 初始化UART
void BSP_UART_Init(void);

// 打印UART数据
void BSP_UART_Print(const char *fmt, ...);

// 尝试读取UART接收缓冲区数据（非阻塞）
bool BSP_UART_TryRead(uint8_t *out, uint32_t out_size, uint32_t *out_read);

#else

#define BSP_UART_Init()
#define BSP_UART_Print(...)
#define BSP_UART_TryRead(...) false

#endif

#endif /* BSP_BSP_UART_H_ */
