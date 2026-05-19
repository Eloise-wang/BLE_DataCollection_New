/*
 * algo_pressure.c
 *  功能:将电压转化为压力。
 *  Created on: 2026年5月16日
 *      Author: elois
 */

#include "algo_pressure.h"

// 将电压转化为压力
float algo_voltage_to_pressure_mpa(float voltage)
{
    static const float voltage_span = (PRESSURE_VOLTAGE_MAX - PRESSURE_VOLTAGE_MIN);
    static const float slope        = (voltage_span != 0.0f) ? (PRESSURE_RANGE_MAX / voltage_span) : 0.0f;

    if (voltage < PRESSURE_VOLTAGE_MIN)
    {
        return 0.0f + PRESSURE_OFFSET_MPA;
    }

    if (voltage > PRESSURE_VOLTAGE_MAX)
    {
        voltage = PRESSURE_VOLTAGE_MAX;
    }

    float pressure    = (voltage - PRESSURE_VOLTAGE_MIN) * slope;

    pressure += PRESSURE_OFFSET_MPA;

    if (pressure < 0.0f)
    {
        pressure = 0.0f;
    }

    if (pressure > PRESSURE_RANGE_MAX)
    {
        pressure = PRESSURE_RANGE_MAX;
    }

    return pressure;
}
