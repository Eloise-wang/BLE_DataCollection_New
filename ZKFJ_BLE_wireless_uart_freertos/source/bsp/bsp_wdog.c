/*
 * bsp_wdog.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "bsp_wdog.h"

#include <string.h>

#include "fsl_clock.h"
#include "fsl_cmc.h"
#include "fsl_wdog32.h"

#define BSP_WDOG_BASE WDOG0

#define BSP_WDOG_CLOCK_HZ 32768U

static bool s_inited;

/* 上电时锁存的复位原因，供复位后随时查询 */
static bool     s_reset_latched;
static uint32_t s_reset_latched_srs;

typedef struct
{
    uint32_t    mask;
    const char *name;
} bsp_wdog_srs_bit_t;

/* 组成 SSRS（粘滞系统复位状态寄存器）的各复位来源位 */
static const bsp_wdog_srs_bit_t k_bsp_wdog_srs_bits[] = {
    { (uint32_t)kCMC_WakeUpReset,      "WAKEUP" },
    { (uint32_t)kCMC_PORReset,         "POR"    },
    { (uint32_t)kCMC_LVDReset,         "LVD"    },
    { (uint32_t)kCMC_HVDReset,         "HVD"    },
    { (uint32_t)kCMC_WarmReset,        "WARM"   },
    { (uint32_t)kCMC_FatalReset,       "FATAL"  },
    { (uint32_t)kCMC_PinReset,         "PIN"    },
    { (uint32_t)kCMC_DAPReset,         "DAP"    },
    { (uint32_t)kCMC_ResetTimeout,     "RSTACK" },
    { (uint32_t)kCMC_LowPowerAcknowledgeTimeoutReset, "LPACK" },
    { (uint32_t)kCMC_SCGReset,         "SCG"    },
    { (uint32_t)kCMC_Watchdog0Reset,   "WDOG0"  },
    { (uint32_t)kCMC_SoftwareReset,    "SW"     },
    { (uint32_t)kCMC_LockUpReset,      "LOCKUP" },
    { (uint32_t)kCMC_Watchdog1Reset,   "WDOG1"  },
};

#define BSP_WDOG_SRS_BIT_COUNT (sizeof(k_bsp_wdog_srs_bits) / sizeof(k_bsp_wdog_srs_bits[0]))

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
    const uint32_t srs = CMC_GetStickySystemResetStatus(CMC0);
    if (!s_reset_latched)
    {
        /* 首次读取发生在启动初始化早期，此时 SSRS 记录的正是本次复位的原因；
         * 锁存下来，便于后续（如 0xE1 查询）追溯，不受软件清零影响。 */
        s_reset_latched_srs = srs;
        s_reset_latched     = true;
    }
    return srs;
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

uint32_t BSP_WDOG_GetLatchedResetStatus(void)
{
    (void)BSP_WDOG_GetResetStatus(); /* 确保已完成锁存 */
    return s_reset_latched_srs;
}

void BSP_WDOG_CaptureBootResetStatus(void)
{
    /* 锁存本次复位原因（即导致本次开机的复位来源） */
    s_reset_latched_srs = CMC_GetStickySystemResetStatus(CMC0);
    s_reset_latched     = true;

    /* 清零 SSRS 中所有已知来源位（写 1 清除）。
     * SSRS 是"自上次冷复位以来、未被软件清除的"来源累加器，不清零会让旧位置残留，
     * 污染下一次复位的原因判断。 */
    uint32_t all = 0U;
    for (size_t i = 0U; i < BSP_WDOG_SRS_BIT_COUNT; i++)
    {
        all |= k_bsp_wdog_srs_bits[i].mask;
    }
    CMC_ClearStickySystemResetStatus(CMC0, all);
}

bool BSP_WDOG_WasLatchedResetByWatchdog(void)
{
    const uint32_t srs = BSP_WDOG_GetLatchedResetStatus();
    return ((srs & (uint32_t)kCMC_Watchdog0Reset) != 0U) || ((srs & (uint32_t)kCMC_Watchdog1Reset) != 0U);
}

size_t BSP_WDOG_FormatResetReason(uint32_t srs, char *out, size_t out_len)
{
    if ((out == NULL) || (out_len == 0U))
    {
        return 0U;
    }

    size_t used = 0U;
    bool   any  = false;
    for (size_t i = 0U; i < BSP_WDOG_SRS_BIT_COUNT; i++)
    {
        if ((srs & k_bsp_wdog_srs_bits[i].mask) == 0U)
        {
            continue;
        }
        const char  *name = k_bsp_wdog_srs_bits[i].name;
        const size_t nlen = strlen(name);
        const size_t need = nlen + (any ? 1U : 0U);
        if ((used + need + 1U) > out_len)
        {
            break; /* 空间不足，停止追加 */
        }
        if (any)
        {
            out[used++] = ',';
        }
        memcpy(&out[used], name, nlen);
        used += nlen;
        any = true;
    }

    if (!any)
    {
        static const char k_none[] = "NONE";
        const size_t nlen = sizeof(k_none) - 1U;
        if (out_len > nlen)
        {
            memcpy(out, k_none, nlen);
            used = nlen;
        }
    }

    out[used] = '\0';
    return used;
}

