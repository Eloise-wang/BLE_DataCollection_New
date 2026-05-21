/*
 * bsp_led.h
 *
 *  Created on: 2026年5月20日
 *      Author: elois
 */

#ifndef BSP_BSP_LED_H_
#define BSP_BSP_LED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    BSP_LED_BLE = 0,
    BSP_LED_COLLECTION,
    BSP_LED_SEND,
    BSP_LED_ELECTRICITY,
    BSP_LED_COUNT
} bsp_led_id_t;

void BSP_LED_Init(void);
void BSP_LED_Deinit(void);

void BSP_LED_Set(bsp_led_id_t led, bool on);
void BSP_LED_Toggle(bsp_led_id_t led);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BSP_LED_H_ */
