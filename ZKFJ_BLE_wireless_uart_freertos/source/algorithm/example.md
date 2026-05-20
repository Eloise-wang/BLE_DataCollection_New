系统采用周期性主从调度架构。Master 调度任务作为系统的时间基准，在采集周期到达时，同步触发 ADC 底层突发采样，并原子地读取 CAN 数据缓存，确保所有物理量（温、压、液位）基于同一系统时间戳进行打包。所有原始数据与诊断掩码经逻辑归一化后统一写入 Flash，避免了异步采集导致的数据时空失准问题

疑问：使采集失败，也要讲数据存入数据库，是否要定义和错误码匹配的哨兵值？还是直接存错误码到flash中？

有一点我要补充：因为传感器存在误差，压力传感器只要接入，当显示电压0.55到0.6V左右时，都是0MPa，当检测没有0.6V，有时候只有0.58V左右时，也可能是正常的，压力传感器只要损坏或者未接传感器，就是0V左右。

至于温度的话，虽然量程是-200-80℃，但在实际使用中，基本不会达到这个多，因此不用管。至于温度传感器没接显示的数值在什么范围，目前我还不能确定。


另外我觉得传感器错误码不要太详细，因为我们没有这么准确的范围

我觉得主要是这几种：1.采集成功；2.底层ADC读取超时；3.传感器损坏或者未接传感器；

实际上在采集时，我们不可能一直盯着看，错误又很难复现，因此我觉得底层的错误码要更详细，到底是哪一步出现了问题，这样才方便改错。 

并且很重要的一点是:因为采集时间过长，我们不可能一直盯着，并且在实际采集的过程中，工程师不能跟车，因此所有的错误数据都是查看flash中的数据才行；

参考如下；

#ifndef APP_SENSOR_H
#define APP_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

// 定义错误码 (位掩码，方便记录多种复合故障)
#define SENSOR_DIAG_OK          (0x00U)
#define SENSOR_DIAG_TIMEOUT     (1U << 0)
#define SENSOR_DIAG_VOLT_LOW    (1U << 1) // 可能是短路
#define SENSOR_DIAG_VOLT_HIGH   (1U << 2) // 可能是断线
#define SENSOR_DIAG_INVALID     (1U << 3) // 转换结果不在物理合理区间

// 传感器数据包裹结构体
typedef struct {
    float temp_celsius;
    float pressure_mpa;
    uint16_t temp_raw;
    uint16_t press_raw;
    uint8_t  temp_diag;
    uint8_t  press_diag;
    uint32_t timestamp_ms;
} app_sensor_data_t;

// 初始化接口
void APP_Sensor_Init(void);

// 核心业务函数：采集并处理 (被 Master 调度器调用)
void APP_Sensor_Collect(app_sensor_data_t *out_data);

#endif /* APP_SENSOR_H */


#include "app_sensor.h"
#include "bsp_adc.h"
#include "algo_filter.h"
#include "algo_raw_convert.h"
#include "algo_temperature.h"
#include "algo_pressure.h"

// 内部 ADC 读取包装，适配滤波算法回调
static bool ADC_Read_Wrapper(void *ctx, uint16_t *out_value) {
    uint32_t channel = (uint32_t)(uintptr_t)ctx;
    return BSP_ADC_TryReadRaw(channel, out_value, 5); // 5ms 超时，快速突发采集
}

void APP_Sensor_Init(void) {
    BSP_ADC_Init(); // 底层初始化，包含自动校准
}

void APP_Sensor_Collect(app_sensor_data_t *out_data) {
    out_data->timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    out_data->temp_diag = SENSOR_DIAG_OK;
    out_data->press_diag = SENSOR_DIAG_OK;

    // --- 1. 温度处理逻辑 ---
    if (!filter_trimmed_mean_u16_8_read_drop_min_max(ADC_Read_Wrapper, (void*)BSP_ADC_TEMPERATURE_CHANNEL, &out_data->temp_raw)) {
        out_data->temp_diag |= SENSOR_DIAG_TIMEOUT;
    } else {
        float voltage = algo_adc_raw_to_voltage(out_data->temp_raw);
        // 温度断线保护：若电压异常高（断线）
        if (voltage > 3.25f) out_data->temp_diag |= SENSOR_DIAG_VOLT_HIGH;
        else if (voltage < 0.05f) out_data->temp_diag |= SENSOR_DIAG_VOLT_LOW;
        
        if (out_data->temp_diag == SENSOR_DIAG_OK) {
            float res = algo_voltage_to_resistance_pt1000(voltage);
            out_data->temp_celsius = algo_pt1000_resistance_to_temperature_c(res);
        } else {
            out_data->temp_celsius = -999.0f; // 哨兵值
        }
    }

    // --- 2. 压力处理逻辑 (含零点带处理) ---
    if (!filter_trimmed_mean_u16_8_read_drop_min_max(ADC_Read_Wrapper, (void*)BSP_ADC_PRESSURE_CHANNEL, &out_data->press_raw)) {
        out_data->press_diag |= SENSOR_DIAG_TIMEOUT;
    } else {
        float voltage = algo_adc_raw_to_voltage(out_data->press_raw);
        
        // 软零点带处理：[0.55V, 0.62V] 强制归零
        if (voltage >= 0.55f && voltage <= 0.62f) {
            out_data->pressure_mpa = 0.0f;
        } else if (voltage < 0.2f) {
            out_data->press_diag |= SENSOR_DIAG_VOLT_LOW; // 真正断线
        } else {
            out_data->pressure_mpa = algo_voltage_to_pressure_mpa(voltage);
        }

        if (out_data->press_diag != SENSOR_DIAG_OK) {
            out_data->pressure_mpa = -99.0f; // 哨兵值
        }
    }
}
