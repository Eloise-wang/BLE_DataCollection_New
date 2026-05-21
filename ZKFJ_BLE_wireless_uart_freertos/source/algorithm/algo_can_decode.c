/*
 * algo_can_protocol.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "algo_can_decode.h"

#include <string.h>

uint32_t algo_can_j1939_get_pgn(uint32_t can_id_29bit)
{
    const uint32_t pf = (can_id_29bit >> 16) & 0xFFU;
    const uint32_t ps = (can_id_29bit >> 8) & 0xFFU;
    const uint32_t dp = (can_id_29bit >> 24) & 0x01U;

    if (pf < 240U)
    {
        return (dp << 16) | (pf << 8);
    }
    return (dp << 16) | (pf << 8) | ps;
}

bool algo_can_decode_gcu1(const uint8_t data[ALGO_CAN_DLC], uint8_t dlc, algo_can_gcu1_t *out)
{
    if ((data == NULL) || (out == NULL) || (dlc != ALGO_CAN_DLC))
    {
        return false;
    }

    (void)memset(out, 0, sizeof(*out));

    out->gasCylinderEffectVolume = (uint16_t)((uint16_t)data[5] | ((uint16_t)(data[6] & 0x0FU) << 8));
    out->residualFluidVolume     = (uint16_t)(((uint16_t)data[7] << 4) | ((uint16_t)data[6] >> 4));
    out->gasCylinderPresNorValue = data[4];
    out->bufTankPresNorValue     = data[3];

    out->bufTankPresSenAcSt     = (uint8_t)((data[0] >> 0) & 0x03U);
    out->gasCylinderPresSenAcSt = (uint8_t)((data[0] >> 2) & 0x03U);
    out->liquidLevelSenAcSt     = (uint8_t)((data[0] >> 4) & 0x03U);

    out->bufTankPresMeasSt     = (uint8_t)((data[1] >> 0) & 0x03U);
    out->gasCylinderPresMeasSt = (uint8_t)((data[1] >> 2) & 0x03U);
    out->liquidLevelMeasSt     = (uint8_t)((data[1] >> 4) & 0x03U);

    out->bufTankPresSelfTeSt     = (uint8_t)((data[2] >> 0) & 0x03U);
    out->gasCylinderPresSelfTSt  = (uint8_t)((data[2] >> 2) & 0x03U);
    out->liquidLevelSelfTSt      = (uint8_t)((data[2] >> 4) & 0x03U);

    return true;
}

