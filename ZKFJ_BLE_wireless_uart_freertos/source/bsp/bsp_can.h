/*
 * bsp_can.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef BSP_BSP_CAN_H_
#define BSP_BSP_CAN_H_

#include <stdbool.h>
#include <stdint.h>

#include "fsl_flexcan.h"

#ifdef __cplusplus
extern "C" {
#endif

// CAN0 配置
#define BSP_CAN_BASE CAN0

// CAN0 配置
#define BSP_CAN_BITRATE_BPS 250000U

#define BSP_CAN_RX_MB0 8U

#define BSP_CAN_FILTER_ID_SINOTRUK 0x18FFEB4EU
#define BSP_CAN_FILTER_MASK_EXACT  0x1FFFFFFFU

#define BSP_CAN_DLC_MAX 8U

typedef struct
{
    uint32_t id;
    uint8_t dlc;
    uint8_t data[BSP_CAN_DLC_MAX];
    bool is_extended;
    bool is_remote;
} bsp_can_frame_t;

//CAN初始化
void BSP_CAN_Init(void);

//CAN去初始化
void BSP_CAN_Deinit(void);

//尝试读取一帧CAN数据
bool BSP_CAN_TryReadFrame(bsp_can_frame_t *out_frame);

//获取CAN接收丢弃计数
uint32_t BSP_CAN_GetDroppedRxCount(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BSP_CAN_H_ */
