/*
 * bsp_wdog.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "bsp_wdog.h"

#include "fsl_clock.h"
#include "fsl_cmc.h"
#include "fsl_wdog32.h"

#define BSP_WDOG_BASE WDOG0

#define BSP_WDOG_CLOCK_HZ 32768U

static bool s_inited;

static uint32_t bsp_wdog_get_effective_hz(wdog32_clock_prescaler_t prescaler)
{
    if (prescaler == kWDOG32_ClockPrescalerDivide256)
    {
        return BSP_WDOG_CLOCK_HZ / 256U;
    }
    return BSP_WDOG_CLOCK_HZ;
}

static uint16_t bsp_wdog_ms_to_timeout_ticks(uint32_t timeout_ms, wdog32_clock_prescaler_t prescaler)
{
    const uint32_t hz = bsp_wdog_get_effective_hz(prescaler);
    if (hz == 0U)
    {
        return 1U;
    }

    uint32_t ticks = (timeout_ms * hz) / 1000U;
    if (ticks == 0U)
    {
        ticks = 1U;
    }
    if (ticks > 0xFFFFU)
    {
        ticks = 0xFFFFU;
    }
    return (uint16_t)ticks;
}

void BSP_WDOG_Init(const bsp_wdog_config_t *cfg)
{
    if ((cfg == NULL) || (cfg->timeout_ms == 0U))
    {
        return;
    }

    CLOCK_EnableClock(kCLOCK_Wdog0);

    wdog32_config_t wcfg;
    WDOG32_GetDefaultConfig(&wcfg);

    wcfg.enableWdog32 = true;
    wcfg.enableUpdate = false;

    wcfg.workMode.enableWait  = cfg->enable_in_wait;
    wcfg.workMode.enableStop  = cfg->enable_in_stop;
    wcfg.workMode.enableDebug = cfg->enable_in_debug;

    wcfg.clockSource = kWDOG32_ClockSource1;
    wcfg.prescaler   = kWDOG32_ClockPrescalerDivide256;

    wcfg.timeoutValue = bsp_wdog_ms_to_timeout_ticks(cfg->timeout_ms, wcfg.prescaler);
    wcfg.enableInterrupt = false;

    WDOG32_Init(BSP_WDOG_BASE, &wcfg);
    WDOG32_Refresh(BSP_WDOG_BASE);

    s_inited = true;
}

void BSP_WDOG_Refresh(void)
{
    if (!s_inited)
    {
        return;
    }
    WDOG32_Refresh(BSP_WDOG_BASE);
}

uint32_t BSP_WDOG_GetResetStatus(void)
{
    return CMC_GetStickySystemResetStatus(CMC0);
}

void BSP_WDOG_ClearResetStatus(uint32_t mask)
{
    CMC_ClearStickySystemResetStatus(CMC0, mask);
}

bool BSP_WDOG_WasResetByWatchdog(void)
{
    const uint32_t srs = BSP_WDOG_GetResetStatus();
    return ((srs & (uint32_t)kCMC_Watchdog0Reset) != 0U) || ((srs & (uint32_t)kCMC_Watchdog1Reset) != 0U);
}

