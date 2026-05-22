/*
 * bsp_uart.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "bsp_uart.h"

#if BSP_UART_ENABLE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_ringBuff.h"
#include "app.h"
#include "fsl_component_serial_manager.h"

static bool s_inited = false;

static SERIAL_MANAGER_WRITE_HANDLE_DEFINE(s_writeHandle);
static SERIAL_MANAGER_READ_HANDLE_DEFINE(s_readHandle);

static volatile bool s_txBusy = false;
static bsp_ringBuff_t s_txRb;
static uint8_t s_txStorage[BSP_UART_TX_RING_SIZE];
static uint8_t s_txChunk[64];

static bool s_rxInited = false;

static void bsp_uart_rx_cb(void *callbackParam, serial_manager_callback_message_t *message, serial_manager_status_t status)
{
    (void)callbackParam;
    (void)message;
    (void)status;
}

//进入临界区
static uint32_t bsp_uart_enter_critical(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

// 退出临界区
static void bsp_uart_exit_critical(uint32_t primask)
{
    if (primask == 0U)
    {
        __enable_irq();
    }
}

static void bsp_uart_try_start_tx(void);

// UART发送回调函数
static void bsp_uart_tx_cb(void *callbackParam, serial_manager_callback_message_t *message, serial_manager_status_t status)
{
    (void)callbackParam;
    (void)message;
    (void)status;

    s_txBusy = false;
    bsp_uart_try_start_tx();
}

// 尝试发送数据
static void bsp_uart_try_start_tx(void)
{
    if (!s_inited || s_txBusy)
    {
        return;
    }

    uint32_t primask = bsp_uart_enter_critical();
    uint32_t used    = BSP_RingBuff_Used(&s_txRb);
    bsp_uart_exit_critical(primask);

    if (used == 0U)
    {
        return;
    }

    uint32_t chunkLen = used;
    if (chunkLen > (uint32_t)sizeof(s_txChunk))
    {
        chunkLen = (uint32_t)sizeof(s_txChunk);
    }

    primask = bsp_uart_enter_critical();
    uint32_t peeked = BSP_RingBuff_Peek(&s_txRb, s_txChunk, chunkLen);
    bsp_uart_exit_critical(primask);
    if (peeked == 0U)
    {
        return;
    }

    serial_manager_status_t smStatus =
        SerialManager_WriteNonBlocking((serial_write_handle_t)s_writeHandle, s_txChunk, peeked);
    if (smStatus != kStatus_SerialManager_Success)
    {
        return;
    }

    primask = bsp_uart_enter_critical();
    (void)BSP_RingBuff_Discard(&s_txRb, peeked);
    s_txBusy = true;
    bsp_uart_exit_critical(primask);
}

// 初始化UART
void BSP_UART_Init(void)
{
    if (s_inited)
    {
        return;
    }

    serial_handle_t appSerMgrIf = (serial_handle_t)&gSerMgrIf[0];

    serial_manager_status_t status =
        SerialManager_OpenWriteHandle((serial_handle_t)appSerMgrIf, (serial_write_handle_t)s_writeHandle);
    if (status != kStatus_SerialManager_Success)
    {
        return;
    }

    status = SerialManager_InstallTxCallback((serial_write_handle_t)s_writeHandle, bsp_uart_tx_cb, NULL);
    if (status != kStatus_SerialManager_Success)
    {
        (void)SerialManager_CloseWriteHandle((serial_write_handle_t)s_writeHandle);
        return;
    }

    status = SerialManager_OpenReadHandle((serial_handle_t)appSerMgrIf, (serial_read_handle_t)s_readHandle);
    if (status == kStatus_SerialManager_Success)
    {
        (void)SerialManager_InstallRxCallback((serial_read_handle_t)s_readHandle, bsp_uart_rx_cb, NULL);
        s_rxInited = true;
    }

    uint32_t primask = bsp_uart_enter_critical();
    BSP_RingBuff_Init(&s_txRb, s_txStorage, BSP_UART_TX_RING_SIZE);
    s_txBusy = false;
    bsp_uart_exit_critical(primask);

    s_inited = true;
}

bool BSP_UART_Write(const uint8_t *data, uint32_t length)
{
    if (!s_inited || (data == NULL) || (length == 0U))
    {
        return false;
    }

    uint32_t primask = bsp_uart_enter_critical();
    uint32_t free    = BSP_RingBuff_Free(&s_txRb);
    bsp_uart_exit_critical(primask);

    if (length > free)
    {
        return false;
    }

    primask = bsp_uart_enter_critical();
    bool ok = BSP_RingBuff_Push(&s_txRb, data, length);
    bsp_uart_exit_critical(primask);

    bsp_uart_try_start_tx();
    return ok;
}

void BSP_UART_Print(const char *fmt, ...)
{
    if (!s_inited || (fmt == NULL))
    {
        return;
    }

    char buf[BSP_UART_PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n <= 0)
    {
        return;
    }

    uint32_t len = (uint32_t)n;
    if (len > (uint32_t)(sizeof(buf) - 1U))
    {
        len = (uint32_t)(sizeof(buf) - 1U);
    }

    (void)BSP_UART_Write((const uint8_t *)buf, len);
}

bool BSP_UART_TryRead(uint8_t *out, uint32_t out_size, uint32_t *out_read)
{
    if ((out == NULL) || (out_read == NULL) || (out_size == 0U))
    {
        return false;
    }

    *out_read = 0U;

    if (!s_inited || !s_rxInited)
    {
        return false;
    }

    return SerialManager_TryRead((serial_read_handle_t)s_readHandle, out, out_size, out_read) == kStatus_SerialManager_Success;
}

#endif
