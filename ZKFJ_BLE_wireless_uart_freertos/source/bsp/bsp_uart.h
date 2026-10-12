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

// 将 u64 格式化为 16 个十六进制字符（低字节在前，与协议线上/0xE0/0xE1 的 task_id 字节序一致）。
// out 需 >= 17 字节。用于统一各处 task_id 的打印形式。
static inline void BSP_UART_FormatU64LE(char *out17, uint64_t v)
{
    static const char h[] = "0123456789ABCDEF";
    for (uint8_t i = 0U; i < 8U; i++)
    {
        const uint8_t b = (uint8_t)(v >> (8U * i));
        out17[i * 2U]      = h[(b >> 4) & 0xFU];
        out17[(i * 2U) + 1U] = h[b & 0xFU];
    }
    out17[16] = '\0';
}

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

// 是否启用UART打印功能 0: 禁用 1: 启用
#ifndef BSP_UART_PRINT_ENABLE
#define BSP_UART_PRINT_ENABLE 1
#endif

// 初始化UART
void BSP_UART_Init(void);

bool BSP_UART_Write(const uint8_t *data, uint32_t length);

#if BSP_UART_PRINT_ENABLE
void BSP_UART_Print(const char *fmt, ...);
#else
#define BSP_UART_Print(...) do { } while (0)
#endif

// 尝试读取UART接收缓冲区数据（非阻塞）
bool BSP_UART_TryRead(uint8_t *out, uint32_t out_size, uint32_t *out_read);

#else

#define BSP_UART_Init()
#define BSP_UART_Write(...) false
#define BSP_UART_Print(...)
#define BSP_UART_TryRead(...) false

#endif

#endif /* BSP_BSP_UART_H_ */
