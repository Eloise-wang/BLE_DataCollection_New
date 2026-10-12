/*! *********************************************************************************
 * \addtogroup Main
 * @{
 ********************************************************************************** */
/*! *********************************************************************************
* Copyright 2021-2025 NXP
*
*
* \file
*
* This is the source file for the main entry point for a FreeRTOS application.
*
* SPDX-License-Identifier: BSD-3-Clause
********************************************************************************** */

/************************************************************************************
 *************************************************************************************
 * Include
 *************************************************************************************
 ************************************************************************************/
#include "app.h"
#include "app_conn.h"
#include "bsp_crc.h"
#include "bsp_uart.h"
#include "bsp_wdog.h"
#include "fsl_os_abstraction.h"
#include "task_manager.h"

#include "FreeRTOS.h"
#include "task.h"

/************************************************************************************
 *************************************************************************************
 * Private functions prototypes
 *************************************************************************************
 ************************************************************************************/
static void start_task(void *argument);

/************************************************************************************
 *************************************************************************************
 * Private memory declarations
 *************************************************************************************
 ************************************************************************************/
static OSA_TASK_HANDLE_DEFINE(s_startTaskHandle);
static OSA_TASK_DEFINE(start_task, gMainThreadPriority_c, 1, gMainThreadStackSize_c, 0);

/************************************************************************************
*************************************************************************************
* Private functions
*************************************************************************************
************************************************************************************/

static void start_task(void *argument)
{
    /* Start Application services (timers, serial manager, low power, led, button, etc..) */
    APP_InitServices();

    BSP_UART_Init();
    /* 锁存本次复位原因并清零 SSRS，供启动打印及 0xE1 事后查询 */
    BSP_WDOG_CaptureBootResetStatus();
    const uint32_t srs = BSP_WDOG_GetLatchedResetStatus();
    const bool wdog_reset = BSP_WDOG_WasLatchedResetByWatchdog();
    char reset_reason[96];
    (void)BSP_WDOG_FormatResetReason(srs, reset_reason, sizeof(reset_reason));
    BSP_UART_Print("reset_status=0x%08lX wdog=%u reason=%s\r\n",
                   (unsigned long)srs, (unsigned)wdog_reset, reset_reason);

    TASK_InitPipelineResources();
    TASK_CreateAllTasks();

    /* Start BLE Platform related ressources such as clocks, Link layer and HCI transport to Link Layer */
    (void)APP_InitBle();

    /* Start Host stack */
    BluetoothLEHost_AppInit();

    while(TRUE)
    {
        BluetoothLEHost_HandleMessages();
    }
}

/************************************************************************************
*************************************************************************************
* Public functions
*************************************************************************************
************************************************************************************/
int main(void)
{
    /* Init OSA: should be called before any other OSA API */
    OSA_Init();

    BOARD_InitHardware();

    BSP_CRC_Init();

    (void)OSA_TaskCreate((osa_task_handle_t)s_startTaskHandle, OSA_TASK(start_task), NULL);

    /* Start scheduler*/
    OSA_Start();

    /*won't run here*/
    assert(0);
    return 0;
}

/*! *********************************************************************************
 * \brief  Provides static memory for the FreeRTOS idle task.
 * Required when configSUPPORT_STATIC_ALLOCATION = 1 and
 * configKERNEL_PROVIDED_STATIC_MEMORY = 0.
 ********************************************************************************** */
void vApplicationGetIdleTaskMemory(StaticTask_t ** ppxIdleTaskTCBBuffer,
                                   StackType_t ** ppxIdleTaskStackBuffer,
                                   configSTACK_DEPTH_TYPE * puxIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  xIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer    = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = xIdleTaskStack;
    *puxIdleTaskStackSize   = (configSTACK_DEPTH_TYPE)configMINIMAL_STACK_SIZE;
}

/*! *********************************************************************************
 * \brief  Provides static memory for the FreeRTOS timer daemon task.
 * Required when configSUPPORT_STATIC_ALLOCATION = 1 and
 * configKERNEL_PROVIDED_STATIC_MEMORY = 0.
 ********************************************************************************** */
void vApplicationGetTimerTaskMemory(StaticTask_t ** ppxTimerTaskTCBBuffer,
                                    StackType_t ** ppxTimerTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * puxTimerTaskStackSize)
{
    static StaticTask_t     xTimerTaskTCB;
    static StackType_t      xTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
    *ppxTimerTaskTCBBuffer    = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = xTimerTaskStack;
    *puxTimerTaskStackSize   = (configSTACK_DEPTH_TYPE)configTIMER_TASK_STACK_DEPTH;
}

/*! *********************************************************************************
 * @}
 ********************************************************************************** */
