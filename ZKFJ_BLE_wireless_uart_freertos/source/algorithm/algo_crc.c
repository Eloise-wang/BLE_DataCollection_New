/*
 * algo_crc.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "algo_crc.h"

#include "bsp_crc.h"

// 计算CRC32
uint32_t algo_crc32_calc(const void *data, size_t length)
{
    return BSP_CRC_Calculate(data, length, BSP_CRC_WIDTH_32);
}

// 计算CRC16
uint16_t algo_crc16_calc(const void *data, size_t length)
{
    return (uint16_t)BSP_CRC_Calculate(data, length, BSP_CRC_WIDTH_16);
}

