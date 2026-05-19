/*
 * algo_pressure.h
 *  功能:将电压转化为压力。
 *  Created on: 2026年5月16日
 *      Author: elois
 */


#ifndef ALGORITHM_ALGO_PRESSURE_H_
#define ALGORITHM_ALGO_PRESSURE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRESSURE_VOLTAGE_MIN 0.6f  // 压力最小电压，单位：V
#define PRESSURE_VOLTAGE_MAX 3.0f  // 压力最大电压，单位：V
#define PRESSURE_RANGE_MAX   2.5f  // 压力最大范围，单位：MPa

#define PRESSURE_OFFSET_MPA 0.06f  // 压力偏移量，单位：MPa


// 将电压转化为压力
float algo_voltage_to_pressure_mpa(float voltage);

#ifdef __cplusplus
}
#endif

#endif /* ALGORITHM_ALGO_PRESSURE_H_ */
