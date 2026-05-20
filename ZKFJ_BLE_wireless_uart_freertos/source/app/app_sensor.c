#include "app_sensor.h"

#include <string.h>

#include "algo_pressure.h"
#include "algo_raw_convert.h"
#include "algo_temperature.h"
#include "bsp_adc.h"
#include "algo_filter.h"
#include "fsl_os_abstraction.h"

typedef struct
{
    uint32_t channel;
    uint32_t timeout_ms;
} app_adc_read_ctx_t;

static bool app_adc_read(void *ctx, uint16_t *out_value)
{
    const app_adc_read_ctx_t *c = (const app_adc_read_ctx_t *)ctx;
    if ((c == NULL) || (out_value == NULL))
    {
        return false;
    }
    return BSP_ADC_TryReadRaw(c->channel, out_value, c->timeout_ms);
}

void APP_Sensor_Init(void)
{
    BSP_ADC_Init();
}

app_sensor_status_t APP_Sensor_MapDiag(app_sensor_diag_t diag)
{
    if ((diag & APP_SENSOR_DIAG_ADC_TIMEOUT) != 0U)
    {
        return APP_SENSOR_STATUS_ADC_TIMEOUT;
    }
    if ((diag & APP_SENSOR_DIAG_SENSOR_OFFLINE) != 0U)
    {
        return APP_SENSOR_STATUS_SENSOR_OFFLINE;
    }
    if ((diag & APP_SENSOR_DIAG_MULTIPLE_ERRORS) != 0U)
    {
        return APP_SENSOR_STATUS_MULTIPLE_ERRORS;
    }
    if (diag != APP_SENSOR_DIAG_OK)
    {
        return APP_SENSOR_STATUS_INVALID;
    }
    return APP_SENSOR_STATUS_OK;
}

bool APP_Sensor_Collect(app_sensor_data_t *out_data)
{
    if (out_data == NULL)
    {
        return false;
    }

    (void)memset(out_data, 0, sizeof(*out_data));
    out_data->timestamp_ms = OSA_TimeGetMsec();
    out_data->temp_diag    = APP_SENSOR_DIAG_OK;
    out_data->press_diag   = APP_SENSOR_DIAG_OK;
    out_data->global_diag  = APP_SENSOR_DIAG_OK;

    {
        const app_adc_read_ctx_t ctx = {
            .channel    = BSP_ADC_TEMPERATURE_CHANNEL,
            .timeout_ms = APP_SENSOR_ADC_BURST_TIMEOUT_MS,
        };

        if (!filter_trimmed_mean_u16_8_read_drop_min_max(app_adc_read, (void *)&ctx, &out_data->temp_raw))
        {
            out_data->temp_diag |= APP_SENSOR_DIAG_ADC_TIMEOUT;
        }
        else
        {
            out_data->temp_voltage_v     = algo_adc_raw_to_voltage(out_data->temp_raw);
            out_data->temp_resistance_ohm = algo_voltage_to_resistance_pt1000(out_data->temp_voltage_v);
            out_data->temp_celsius       = algo_pt1000_resistance_to_temperature_c(out_data->temp_resistance_ohm);
        }
    }

    {
        const app_adc_read_ctx_t ctx = {
            .channel    = BSP_ADC_PRESSURE_CHANNEL,
            .timeout_ms = APP_SENSOR_ADC_BURST_TIMEOUT_MS,
        };

        if (!filter_trimmed_mean_u16_8_read_drop_min_max(app_adc_read, (void *)&ctx, &out_data->press_raw))
        {
            out_data->press_diag |= APP_SENSOR_DIAG_ADC_TIMEOUT;
        }
        else
        {
            out_data->press_voltage_v = algo_adc_raw_to_voltage(out_data->press_raw);

            if (out_data->press_voltage_v < APP_PRESSURE_ZERO_MIN_V)
            {
                out_data->press_diag |= APP_SENSOR_DIAG_SENSOR_OFFLINE;
                out_data->pressure_mpa = 0.0f;
            }
            else if (out_data->press_voltage_v <= APP_PRESSURE_ZERO_MAX_V)
            {
                out_data->pressure_mpa = 0.0f;
            }
            else
            {
                out_data->pressure_mpa = algo_voltage_to_pressure_mpa(out_data->press_voltage_v);
            }
        }
    }

    out_data->global_diag = (app_sensor_diag_t)(out_data->temp_diag | out_data->press_diag);
    if ((out_data->temp_diag != APP_SENSOR_DIAG_OK) && (out_data->press_diag != APP_SENSOR_DIAG_OK))
    {
        out_data->global_diag |= APP_SENSOR_DIAG_MULTIPLE_ERRORS;
    }

    return true;
}
