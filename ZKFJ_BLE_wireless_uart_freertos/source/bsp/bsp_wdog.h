/*
 * bsp_wdog.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef BSP_BSP_WDOG_H_
#define BSP_BSP_WDOG_H_

#include <stdbool.h>
#include <stddef.h>
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

// 获取本次上电时锁存的复位状态。
// 首次调用 BSP_WDOG_GetResetStatus 时会自动锁存当前（即上一次复位的原因）SRS 值，
// 锁存值不受后续 BSP_WDOG_ClearResetStatus 影响，便于复位后随时查询复位原因。
uint32_t BSP_WDOG_GetLatchedResetStatus(void);
// 获取本次上电锁存的复位计数（自上次冷复位以来的复位序列数），可判断是否反复复位。
uint32_t BSP_WDOG_GetLatchedResetCount(void);
// 在启动早期调用一次：锁存本次复位原因，并清零 SSRS（写 1 清除）。
// 清零是必须的——SSRS 是"自上次冷复位以来、未被软件清除的"复位来源累加器，
// 若不清零，历史置位会一直残留，导致下一次复位的原因被旧位污染。
void BSP_WDOG_CaptureBootResetStatus(void);
// 锁存的复位原因是否由看门狗触发（基于锁存值，即使 WDOG 位已被清除也能判断）
bool BSP_WDOG_WasLatchedResetByWatchdog(void);
// 将复位状态各位解码为逗号分隔的可读字符串（无任何已知位置位时写 "NONE"）。
// 返回写入 out 的字符串长度（不含结尾 '\0'）。
size_t BSP_WDOG_FormatResetReason(uint32_t srs, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BSP_WDOG_H_ */
