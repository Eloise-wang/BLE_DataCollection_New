/*
 * pt1000_float.c
 *
 * 针对MCX W71 Cortex-M33 FPU优化的浮点数版本实现
 * 针对MCX 电路放大倍数：10倍
 * 针对MCX 温度范围：-200°C 到 +100°C
 * 针对MCX
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#include "pt1000_float.h"
#include "pt1000_lut.h"

/**
 * @brief 根据测量的电阻值计算温度，使用二分查找+线性插值
 * @param resistance 测得的PT1000电阻值 (单位: Ohm)
 * @return float     计算出的温度值 (单位: °C)
 */
float pt1000_get_temp_float(float resistance) {
    // 1. 边界检查
    if (resistance < PT1000_LUT_FLOAT[0] ||
        resistance > PT1000_LUT_FLOAT[LUT_TABLE_SIZE - 1]) {
        return PT1000_RESISTANCE_OUT_OF_RANGE_F;
    }

    // 2. 二分法查找电阻所在的区间索引
    uint16_t low = 0;
    uint16_t high = LUT_TABLE_SIZE - 1;
    uint16_t mid;

    while (low <= high) {
        mid = low + (high - low) / 2;
        if (resistance < PT1000_LUT_FLOAT[mid]) {
            high = mid - 1;
        } else if (resistance > PT1000_LUT_FLOAT[mid]) {
            low = mid + 1;
        } else {
            // 精确匹配
            return (float)(LUT_TEMP_START_C + mid * LUT_TEMP_STEP_C);
        }
    }

    uint16_t index_low = high;

    // 防止数组越界
    if (index_low >= LUT_TABLE_SIZE - 1) {
        index_low = LUT_TABLE_SIZE - 2;
    }

    // 3. 分段线性插值
    float r1 = PT1000_LUT_FLOAT[index_low];
    float r2 = PT1000_LUT_FLOAT[index_low + 1];

    float t1 = (float)(LUT_TEMP_START_C + index_low * LUT_TEMP_STEP_C);
    float t2 = (float)(LUT_TEMP_START_C + (index_low + 1) * LUT_TEMP_STEP_C);

    // 线性插值公式: T = T1 + (R-R1)/(R2-R1) * (T2-T1)
    float temperature = t1 + (t2 - t1) * (resistance - r1) / (r2 - r1);

    return temperature;
}
