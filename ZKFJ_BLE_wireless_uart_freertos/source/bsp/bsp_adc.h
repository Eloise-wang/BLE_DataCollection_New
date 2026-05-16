/*
 * bsp_adc.h
 *
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#ifndef BSP_BSP_ADC_H_
#define BSP_BSP_ADC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_ADC_BASE ADC0

#define BSP_ADC_TEMPERATURE_CHANNEL 11U
#define BSP_ADC_PRESSURE_CHANNEL    12U

#define BSP_ADC_INVALID_RAW       0xFFFFU
#define BSP_ADC_DEFAULT_TIMEOUT_MS 50U

void BSP_ADC_Init(void);
void BSP_ADC_Deinit(void);
uint16_t BSP_ADC_ReadRaw(uint32_t channel);
bool BSP_ADC_TryReadRaw(uint32_t channel, uint16_t *outRaw, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BSP_ADC_H_ */

