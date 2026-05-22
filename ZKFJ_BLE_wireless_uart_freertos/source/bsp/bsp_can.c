/*
 * bsp_can.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "bsp_can.h"

#include <string.h>

#include "bsp_ringBuff.h"
#include "fsl_clock.h"

typedef struct
{
    uint8_t mb_idx;
    flexcan_frame_t *frame;
} bsp_can_mb_map_t;

static bsp_ringBuff_t s_rxRb;
static uint8_t s_rxStorage[32U * (uint32_t)sizeof(bsp_can_frame_t)];
static volatile uint32_t s_rxDropped;

static flexcan_handle_t s_flexcanHandle;
static flexcan_frame_t s_rxFrame0;
static flexcan_mb_transfer_t s_rxXfer0;

//接收缓冲区映射表
static const bsp_can_mb_map_t s_mbMap[] = {
    {BSP_CAN_RX_MB0, &s_rxFrame0},
};

//进入临界区
static uint32_t bsp_can_enter_critical(void)
{
    uint32_t primask;
    __asm volatile("MRS %0, PRIMASK" : "=r"(primask));
    __disable_irq();
    return primask;
}

//退出临界区
static void bsp_can_exit_critical(uint32_t primask)
{
    __asm volatile("MSR PRIMASK, %0" : : "r"(primask));
}

static bool bsp_can_rb_push_frame(const bsp_can_frame_t *frame)
{
    const uint32_t key = bsp_can_enter_critical();
    const bool ok      = BSP_RingBuff_Push(&s_rxRb, (const uint8_t *)frame, (uint32_t)sizeof(*frame));
    bsp_can_exit_critical(key);
    return ok;
}

//从接收缓冲区弹出一帧CAN数据
static bool bsp_can_rb_pop_frame(bsp_can_frame_t *out_frame)
{
    bool ok = false;
    const uint32_t key = bsp_can_enter_critical();
    if (BSP_RingBuff_Used(&s_rxRb) >= (uint32_t)sizeof(*out_frame))
    {
        ok = (BSP_RingBuff_Pop(&s_rxRb, (uint8_t *)out_frame, (uint32_t)sizeof(*out_frame)) ==
              (uint32_t)sizeof(*out_frame));
    }
    bsp_can_exit_critical(key);
    return ok;
}

//将flexcan_frame_t转换为bsp_can_frame_t
static void bsp_can_convert_frame(const flexcan_frame_t *in, bsp_can_frame_t *out)
{
    (void)memset(out, 0, sizeof(*out));

    out->id          = in->id;
    out->dlc         = (uint8_t)in->length;
    out->is_extended = (in->format == (uint32_t)kFLEXCAN_FrameFormatExtend);
    out->is_remote   = (in->type == (uint32_t)kFLEXCAN_FrameTypeRemote);

    out->data[0] = in->dataByte0;
    out->data[1] = in->dataByte1;
    out->data[2] = in->dataByte2;
    out->data[3] = in->dataByte3;
    out->data[4] = in->dataByte4;
    out->data[5] = in->dataByte5;
    out->data[6] = in->dataByte6;
    out->data[7] = in->dataByte7;
}

static FLEXCAN_CALLBACK(bsp_can_rx_cb)
{
    (void)handle;
    (void)userData;

    if ((status != (status_t)kStatus_FLEXCAN_RxIdle) && (status != (status_t)kStatus_FLEXCAN_RxOverflow))
    {
        return;
    }

    const uint32_t mb_idx = (uint32_t)result;
    for (uint32_t i = 0U; i < (uint32_t)(sizeof(s_mbMap) / sizeof(s_mbMap[0])); i++)
    {
        if (mb_idx == (uint32_t)s_mbMap[i].mb_idx)
        {
            bsp_can_frame_t frame;
            bsp_can_convert_frame(s_mbMap[i].frame, &frame);

            if (!bsp_can_rb_push_frame(&frame))
            {
                s_rxDropped++;
            }

            (void)FLEXCAN_TransferReceiveNonBlocking(base, &s_flexcanHandle, &s_rxXfer0);
            break;
        }
    }
}

void BSP_CAN_Init(void)
{
    CLOCK_EnableClock(kCLOCK_Can0);

    BSP_RingBuff_Init(&s_rxRb, s_rxStorage, (uint32_t)sizeof(s_rxStorage));
    s_rxDropped = 0U;

    flexcan_config_t config;
    FLEXCAN_GetDefaultConfig(&config);

    config.baudRate        = BSP_CAN_BITRATE_BPS;
    config.enableIndividMask = true;
    config.maxMbNum        = 16U;

    const uint32_t canClock = CLOCK_GetIpFreq(kCLOCK_Can0);
    FLEXCAN_Init(BSP_CAN_BASE, &config, canClock);

    FLEXCAN_SetRxMbGlobalMask(BSP_CAN_BASE, FLEXCAN_RX_MB_EXT_MASK(0x1FFFFFFFU, 1U, 1U));

    const flexcan_rx_mb_config_t rxMbCfg0 = {
        .id     = FLEXCAN_ID_EXT(BSP_CAN_FILTER_ID0),
        .format = kFLEXCAN_FrameFormatExtend,
        .type   = kFLEXCAN_FrameTypeData,
    };

    FLEXCAN_SetRxMbConfig(BSP_CAN_BASE, BSP_CAN_RX_MB0, &rxMbCfg0, true);

    FLEXCAN_SetRxIndividualMask(BSP_CAN_BASE, BSP_CAN_RX_MB0, FLEXCAN_RX_MB_EXT_MASK(0x1FFFFFFFU, 1U, 1U));

    FLEXCAN_TransferCreateHandle(BSP_CAN_BASE, &s_flexcanHandle, bsp_can_rx_cb, NULL);

    s_rxXfer0.frame = &s_rxFrame0;
    s_rxXfer0.mbIdx = BSP_CAN_RX_MB0;

    (void)FLEXCAN_TransferReceiveNonBlocking(BSP_CAN_BASE, &s_flexcanHandle, &s_rxXfer0);

    NVIC_ClearPendingIRQ(CAN0_IRQn);
    NVIC_EnableIRQ(CAN0_IRQn);
}

void BSP_CAN_Deinit(void)
{
    NVIC_DisableIRQ(CAN0_IRQn);
    FLEXCAN_Deinit(BSP_CAN_BASE);
}

bool BSP_CAN_TryReadFrame(bsp_can_frame_t *out_frame)
{
    if (out_frame == NULL)
    {
        return false;
    }
    return bsp_can_rb_pop_frame(out_frame);
}

uint32_t BSP_CAN_GetDroppedRxCount(void)
{
    return s_rxDropped;
}

void CAN0_IRQHandler(void)
{
    FLEXCAN_TransferHandleIRQ(BSP_CAN_BASE, &s_flexcanHandle);
}
