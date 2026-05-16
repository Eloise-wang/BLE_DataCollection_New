/*
 * filter.h
 *
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#ifndef ALGORITHM_FILTER_H_
#define ALGORITHM_FILTER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FILTER_TRIMMED_MEAN_SAMPLE_COUNT 8U //采样点数
#define FILTER_TRIMMED_MEAN_DROP_COUNT   1U //丢弃点数
#define FILTER_TRIMMED_MEAN_VALID_COUNT  6U //有效点数

// 读取函数指针类型
typedef bool (*filter_read_u16_fn)(void *ctx, uint16_t *out_value);

// 计算8个样本的中位数平均值，丢弃最小和最大值
bool filter_trimmed_mean_u16_8_drop_min_max(const uint16_t samples[FILTER_TRIMMED_MEAN_SAMPLE_COUNT],
                                            uint16_t *out_mean);

// 读取8个样本的中位数平均值，丢弃最小和最大值      
bool filter_trimmed_mean_u16_8_read_drop_min_max(filter_read_u16_fn read_fn, void *ctx, uint16_t *out_mean);

#ifdef __cplusplus
}
#endif


#endif // ALGORITHM_FILTER_H_
