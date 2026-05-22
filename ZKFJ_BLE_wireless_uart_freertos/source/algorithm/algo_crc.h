/*
 * algo_crc.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef ALGORITHM_ALGO_CRC_H_
#define ALGORITHM_ALGO_CRC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 计算CRC32
uint32_t algo_crc32_calc(const void *data, size_t length);
// 计算CRC16
uint16_t algo_crc16_calc(const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* ALGORITHM_ALGO_CRC_H_ */
