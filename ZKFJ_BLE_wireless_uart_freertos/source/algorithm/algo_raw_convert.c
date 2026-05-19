/*
 * algo_raw_convert.c
 *  用途：将原始值转为电压，电压转为电阻
 *  Created on: 2026年5月16日
 *      Author: elois
 */


#include "algo_raw_convert.h"

typedef struct
{
    float voltage;
    float resistance;
} algo_vr_point_t;

float algo_adc_raw_to_voltage(uint16_t raw)
{
    return ((float)raw * ALGO_ADC_VREF_V) / ALGO_ADC_FULL_RANGE;
}

float algo_voltage_to_resistance_pt1000(float voltage)
{
    //
    static const algo_vr_point_t points[] = {
        {0.015f, 185.2f},   //-200℃
        {0.145f, 228.3f},   //-190℃
        {0.268f, 271.0f},   //-180℃
        {0.392f, 313.4f},   //-170℃
        {0.514f, 355.4f},   //-160℃
        {0.634f, 397.2f},   //-150℃
        {0.754f, 438.8f},   //-140℃
        {0.869f, 480.0f},   //-130℃
        {0.986f, 521.1f},   //-120℃
        {1.098f, 561.9f},   //-110℃
        {1.216f, 602.6f},  //-100℃
        {1.322f, 643.0f},  //-90℃   
        {1.430f, 683.3f},  //-80℃
        {1.540f, 723.3f},  //-70℃
        {1.650f, 763.3f},  //-60℃
        {1.753f, 803.1f},  //-50℃
        {1.818f, 842.7f},  //-40℃
        {1.859f, 882.2f},  //-30℃
        {1.900f, 921.6f},  //-20℃
        {1.941f, 960.9f},  //-10℃
        {1.981f, 1000.0f}, //0℃
        {2.030f, 1039.0f}, //10℃
        {2.070f, 1077.9f}, //20℃
        {2.114f, 1116.7f}, //30℃
        {2.159f, 1155.4f}, //40℃
        {2.201f, 1194.0f}, //50℃
        {2.247f, 1232.4f}, //60℃
        {2.288f, 1270.8f}, //70℃
        {2.330f, 1309.0f}, //80℃
        {2.380f, 1347.1f}, //90℃
        {2.426f, 1385.1f}, //100℃
    };

    const uint32_t n = (uint32_t)(sizeof(points) / sizeof(points[0]));
    if (n < 2U)
    {
        return 1000.0f;
    }

    if (voltage <= points[0].voltage)
    {
        const float dv    = points[1].voltage - points[0].voltage;
        const float slope = (dv != 0.0f) ? ((points[1].resistance - points[0].resistance) / dv) : 0.0f;
        float r            = points[0].resistance + slope * (voltage - points[0].voltage);
        if (r < 100.0f)
        {
            r = 100.0f;
        }
        return r;
    }

    if (voltage >= points[n - 1U].voltage)
    {
        const float dv    = points[n - 1U].voltage - points[n - 2U].voltage;
        const float slope = (dv != 0.0f) ? ((points[n - 1U].resistance - points[n - 2U].resistance) / dv) : 0.0f;
        float r            = points[n - 1U].resistance + slope * (voltage - points[n - 1U].voltage);
        if (r > 2000.0f)
        {
            r = 2000.0f;
        }
        return r;
    }

    uint32_t lo = 0U;
    uint32_t hi = n - 1U;
    while ((hi - lo) > 1U)
    {
        const uint32_t mid = lo + (hi - lo) / 2U;
        if (voltage < points[mid].voltage)
        {
            hi = mid;
        }
        else
        {
            lo = mid;
        }
    }

    const float v1 = points[lo].voltage;
    const float v2 = points[lo + 1U].voltage;
    const float r1 = points[lo].resistance;
    const float r2 = points[lo + 1U].resistance;
    const float dv = v2 - v1;
    if (dv == 0.0f)
    {
        return r1;
    }
    return r1 + (r2 - r1) * (voltage - v1) / dv;
}
