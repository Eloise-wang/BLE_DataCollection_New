/*
 * bsp_crc.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef BSP_BSP_CRC_H_
#define BSP_BSP_CRC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CRC宽度
typedef enum
{
    BSP_CRC_WIDTH_16 = 0,
    BSP_CRC_WIDTH_32,
} bsp_crc_width_t;

// 初始化CRC
void BSP_CRC_Init(void);
// 去初始化CRC
void BSP_CRC_Deinit(void);
// 计算CRC
uint32_t BSP_CRC_Calculate(const void *data, size_t length, bsp_crc_width_t width);

#ifdef __cplusplus
}
#endif
#endif /* BSP_BSP_CRC_H_ */
