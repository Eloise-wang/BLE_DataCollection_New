/*
 * bsp_crc.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "bsp_crc.h"

#include <stdbool.h>

#include "fsl_clock.h"
#include "fsl_crc.h"
#include "fsl_os_abstraction.h"

#define BSP_CRC_BASE CRC0

static bool s_mutexCreated;
static bool s_inited;
static OSA_MUTEX_HANDLE_DEFINE(s_crcMutex);

static void bsp_crc_create_mutex_if_needed(void)
{
    if (s_mutexCreated)
    {
        return;
    }

    if (KOSA_StatusSuccess == OSA_MutexCreate((osa_mutex_handle_t)s_crcMutex))
    {
        s_mutexCreated = true;
    }
}

void BSP_CRC_Init(void)
{
    if (s_inited)
    {
        return;
    }
    bsp_crc_create_mutex_if_needed();
    s_inited = s_mutexCreated;
}

void BSP_CRC_Deinit(void)
{
    if (!s_inited)
    {
        return;
    }

    if (s_mutexCreated)
    {
        (void)OSA_MutexDestroy((osa_mutex_handle_t)s_crcMutex);
        s_mutexCreated = false;
    }

    s_inited = false;
}

uint32_t BSP_CRC_Calculate(const void *data, size_t length, bsp_crc_width_t width)
{
    if ((data == NULL) || (length == 0U))
    {
        return 0U;
    }

    if (!s_mutexCreated)
    {
        return 0U;
    }

    if (KOSA_StatusSuccess != OSA_MutexLock((osa_mutex_handle_t)s_crcMutex, osaWaitForever_c))
    {
        return 0U;
    }

    CLOCK_EnableClock(kCLOCK_Crc0);

    crc_config_t cfg;
    CRC_GetDefaultConfig(&cfg);

    if (width == BSP_CRC_WIDTH_32)
    {
        cfg.polynomial         = 0x04C11DB7U;
        cfg.seed               = 0xFFFFFFFFU;
        cfg.reflectIn          = true;
        cfg.reflectOut         = true;
        cfg.complementChecksum = true;
        cfg.crcBits            = kCrcBits32;
        cfg.crcResult          = kCrcFinalChecksum;
    }
    else
    {
        cfg.polynomial         = 0x1021U;
        cfg.seed               = 0xFFFFU;
        cfg.reflectIn          = false;
        cfg.reflectOut         = false;
        cfg.complementChecksum = false;
        cfg.crcBits            = kCrcBits16;
        cfg.crcResult          = kCrcFinalChecksum;
    }

    CRC_Init(BSP_CRC_BASE, &cfg);
    CRC_WriteData(BSP_CRC_BASE, (const uint8_t *)data, length);

    uint32_t result = 0U;
    if (width == BSP_CRC_WIDTH_32)
    {
        result = CRC_Get32bitResult(BSP_CRC_BASE);
    }
    else
    {
        result = (uint32_t)CRC_Get16bitResult(BSP_CRC_BASE);
    }

    CRC_Deinit(BSP_CRC_BASE);
#if (defined(FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL) && FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL)
    CLOCK_DisableClock(kCLOCK_Crc0);
#endif

    if (s_mutexCreated)
    {
        (void)OSA_MutexUnlock((osa_mutex_handle_t)s_crcMutex);
    }

    return result;
}
