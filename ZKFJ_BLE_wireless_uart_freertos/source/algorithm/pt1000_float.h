/*
 * pt1000_float.h
 *
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#ifndef ALGORITHM_PT1000_FLOAT_H_
#define ALGORITHM_PT1000_FLOAT_H_

#include <stdint.h>

// 定义错误码，用于表示输入电阻超出范围
#define PT1000_RESISTANCE_OUT_OF_RANGE_F (-999.0f)

/**
 * @brief 根据测量的电阻值计算温度
 * @param resistance 测得的PT1000电阻值 (单位: Ohm)
 * @return float     计算出的温度值 (单位: °C)
 *                   如果电阻值超出查找表范围，返回 PT1000_RESISTANCE_OUT_OF_RANGE_F
 */
float pt1000_get_temp_float(float resistance);

#endif /* ALGORITHM_PT1000_FLOAT_H_ */
