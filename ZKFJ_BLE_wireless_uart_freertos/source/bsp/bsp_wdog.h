/*
 * bsp_wdog.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef BSP_BSP_WDOG_H_
#define BSP_BSP_WDOG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 看门狗配置结构体
typedef struct
{
    uint32_t timeout_ms;
    bool enable_in_wait;
    bool enable_in_stop;
    bool enable_in_debug;
} bsp_wdog_config_t;

// 初始化看门狗
void BSP_WDOG_Init(const bsp_wdog_config_t *cfg);
// 刷新看门狗
void BSP_WDOG_Refresh(void);

// 获取看门狗重置状态
uint32_t BSP_WDOG_GetResetStatus(void);
// 清除看门狗重置状态
void BSP_WDOG_ClearResetStatus(uint32_t mask);
// 是否被看门狗重置 
bool BSP_WDOG_WasResetByWatchdog(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BSP_WDOG_H_ */
