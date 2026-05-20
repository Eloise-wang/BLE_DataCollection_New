/*
 * algo_raw_convert.h
 *  功能:将原始值转化为电压，电压转化为电阻。
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#ifndef ALGORITHM_ALGO_RAW_CONVERT_H_
#define ALGORITHM_ALGO_RAW_CONVERT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALGO_ADC_VREF_V
#define ALGO_ADC_VREF_V 3.3f
#endif

#ifndef ALGO_ADC_FULL_RANGE
#define ALGO_ADC_FULL_RANGE 65536.0f
#endif

float algo_adc_raw_to_voltage(uint16_t raw);
float algo_voltage_to_resistance_pt1000(float voltage);

#ifdef __cplusplus
}
#endif

#endif /* ALGORITHM_ALGO_RAW_CONVERT_H_ */
