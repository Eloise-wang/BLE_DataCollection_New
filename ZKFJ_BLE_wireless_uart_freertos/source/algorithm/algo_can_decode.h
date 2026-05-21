/*
 * algo_can_protocol.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef ALGORITHM_ALGO_CAN_DECODE_H_
#define ALGORITHM_ALGO_CAN_DECODE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALGO_CAN_DLC 8U

#define ALGO_CAN_PGN_GCU1       0xFFEBU

//GCU1数据结构体
typedef struct
{
    uint16_t residualFluidVolume; //剩余液体体积
    uint16_t gasCylinderEffectVolume; //气体缸效果体积
    uint8_t gasCylinderPresNorValue; //气体缸压力正常值
    uint8_t bufTankPresNorValue; //缓冲区压力正常值
    uint8_t bufTankPresSenAcSt; //缓冲区压力传感器激活状态
    uint8_t gasCylinderPresSenAcSt; //气体缸压力传感器激活状态
    uint8_t liquidLevelSenAcSt; //液位传感器激活状态
    uint8_t bufTankPresMeasSt; //缓冲区压力测量状态
    uint8_t gasCylinderPresMeasSt; //气体缸压力测量状态
    uint8_t liquidLevelMeasSt; //液位测量状态
    uint8_t bufTankPresSelfTeSt; //缓冲区压力自测状态
    uint8_t gasCylinderPresSelfTSt; //气体缸压力自测状态
    uint8_t liquidLevelSelfTSt; //液位自测状态
} algo_can_gcu1_t;

//获取J1939 PGN
uint32_t algo_can_j1939_get_pgn(uint32_t can_id_29bit);
//解码GCU1数据
bool algo_can_decode_gcu1(const uint8_t data[ALGO_CAN_DLC], uint8_t dlc, algo_can_gcu1_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ALGORITHM_ALGO_CAN_DECODE_H_ */
