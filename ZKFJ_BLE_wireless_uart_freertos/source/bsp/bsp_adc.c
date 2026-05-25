/*
 * bsp_adc.c
 *
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#include "bsp_adc.h"

#include <stdbool.h>

#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_lpadc.h"
#include "fsl_os_abstraction.h"
#include "fsl_vref.h"

#define BSP_ADC_USE_HIGH_RESOLUTION true
#define BSP_ADC_USER_CMDID          1U

static bool s_adcInitialized    = false;
static bool s_mutexCreated      = false;
static OSA_MUTEX_HANDLE_DEFINE(s_adcMutexHandle);

#if (defined(BSP_ADC_USE_HIGH_RESOLUTION) && BSP_ADC_USE_HIGH_RESOLUTION)
static const uint32_t s_lpadcResultShift = 0U;
#else
static const uint32_t s_lpadcResultShift = 3U;
#endif

static void BSP_ADC_InitHw(void)
{
    if (s_adcInitialized)
    {
        return;
    }

    vref_config_t vrefConfig;
    lpadc_config_t lpadcConfig;

    VREF_GetDefaultConfig(&vrefConfig);
    VREF_Init(VREF0, &vrefConfig);
    VREF_SetTrim21Val(VREF0, 8U);

    CLOCK_EnableClock(kCLOCK_Lpadc0);
    CLOCK_SetIpSrc(kCLOCK_Lpadc0, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpadc0, kSCG_SysClkDivBy4);

    LPADC_GetDefaultConfig(&lpadcConfig);
    lpadcConfig.enableAnalogPreliminary = true;
    lpadcConfig.referenceVoltageSource  = kLPADC_ReferenceVoltageAlt1;
    lpadcConfig.conversionAverageMode   = kLPADC_ConversionAverage128;
    LPADC_Init(BSP_ADC_BASE, &lpadcConfig);

    LPADC_DoAutoCalibration(BSP_ADC_BASE);

    if (!s_mutexCreated)
    {
        if (KOSA_StatusSuccess == OSA_MutexCreate((osa_mutex_handle_t)s_adcMutexHandle))
        {
            s_mutexCreated = true;
        }
    }

    s_adcInitialized = true;
}

bool BSP_ADC_TryReadRaw(uint32_t channelNumber, uint16_t *outRaw, uint32_t timeout_ms)
{
    lpadc_conv_command_config_t commandConfig;
    lpadc_conv_trigger_config_t triggerConfig;
    lpadc_conv_result_t resultConfig;
    uint32_t startMs;
    bool gotResult = false;

    if (outRaw == NULL)
    {
        return false;
    }

    BSP_ADC_InitHw();

    if (s_mutexCreated)
    {
        if (KOSA_StatusSuccess != OSA_MutexLock((osa_mutex_handle_t)s_adcMutexHandle, timeout_ms))
        {
            *outRaw = BSP_ADC_INVALID_RAW;
            return false;
        }
    }

#if (defined(FSL_FEATURE_LPADC_FIFO_COUNT) && (FSL_FEATURE_LPADC_FIFO_COUNT == 2))
    LPADC_DoResetFIFO0(BSP_ADC_BASE);
    LPADC_DoResetFIFO1(BSP_ADC_BASE);
#else
    LPADC_DoResetFIFO(BSP_ADC_BASE);
#endif
    LPADC_ClearStatusFlags(BSP_ADC_BASE, kLPADC_ResultFIFO0OverflowFlag);
#if (defined(FSL_FEATURE_LPADC_FIFO_COUNT) && (FSL_FEATURE_LPADC_FIFO_COUNT == 2U))
    LPADC_ClearStatusFlags(BSP_ADC_BASE, kLPADC_ResultFIFO1OverflowFlag);
#endif

    LPADC_GetDefaultConvCommandConfig(&commandConfig);
    commandConfig.channelNumber       = channelNumber;
    commandConfig.sampleChannelMode   = kLPADC_SampleChannelSingleEndSideA;
    commandConfig.sampleTimeMode      = kLPADC_SampleTimeADCK35;
    commandConfig.hardwareAverageMode = kLPADC_HardwareAverageCount16;
#if defined(BSP_ADC_USE_HIGH_RESOLUTION) && BSP_ADC_USE_HIGH_RESOLUTION
    commandConfig.conversionResolutionMode = kLPADC_ConversionResolutionHigh;
#endif

    LPADC_SetConvCommandConfig(BSP_ADC_BASE, BSP_ADC_USER_CMDID, &commandConfig);

    LPADC_GetDefaultConvTriggerConfig(&triggerConfig);
    triggerConfig.targetCommandId       = BSP_ADC_USER_CMDID;
    triggerConfig.enableHardwareTrigger = false;
    LPADC_SetConvTriggerConfig(BSP_ADC_BASE, 0U, &triggerConfig);

    LPADC_DoSoftwareTrigger(BSP_ADC_BASE, 1U);

    startMs = OSA_TimeGetMsec();
    while ((OSA_TimeGetMsec() - startMs) < timeout_ms)
    {
        if (LPADC_GetConvResult(BSP_ADC_BASE, &resultConfig, 0U))
        {
            gotResult = true;
            break;
        }

        if ((0U == __get_IPSR()) && ((OSA_TimeGetMsec() - startMs) >= 1U))
        {
            OSA_TimeDelay(1U);
        }
    }

    if (s_mutexCreated)
    {
        (void)OSA_MutexUnlock((osa_mutex_handle_t)s_adcMutexHandle);
    }

    if (!gotResult)
    {
        *outRaw = BSP_ADC_INVALID_RAW;
        return false;
    }

    *outRaw = (uint16_t)(resultConfig.convValue >> s_lpadcResultShift);
    return true;
}

void BSP_ADC_Init(void)
{
    BSP_ADC_InitHw();
}

bool BSP_ADC_ReadRaw(uint32_t channelNumber, uint16_t *outRaw)
{
    if (outRaw == NULL)
    {
        return false;
    }
    return BSP_ADC_TryReadRaw(channelNumber, outRaw, BSP_ADC_DEFAULT_TIMEOUT_MS);
}


void BSP_ADC_Deinit(void)
{
    if (!s_adcInitialized)
    {
        return;
    }

    s_adcInitialized    = false;

    if (s_mutexCreated)
    {
        (void)OSA_MutexDestroy((osa_mutex_handle_t)s_adcMutexHandle);
        s_mutexCreated = false;
    }

    LPADC_Deinit(BSP_ADC_BASE);
    VREF_Deinit(VREF0);
}
