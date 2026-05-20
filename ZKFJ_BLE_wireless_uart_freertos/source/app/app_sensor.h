#ifndef APP_APP_SENSOR_H_
#define APP_APP_SENSOR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t app_sensor_diag_t;

#define APP_SENSOR_DIAG_OK               ((app_sensor_diag_t)0x0000U)
#define APP_SENSOR_DIAG_ADC_TIMEOUT      ((app_sensor_diag_t)0x0001U)
#define APP_SENSOR_DIAG_SENSOR_OFFLINE   ((app_sensor_diag_t)0x0002U)
#define APP_SENSOR_DIAG_CONVERT_INVALID  ((app_sensor_diag_t)0x0004U)
#define APP_SENSOR_DIAG_MULTIPLE_ERRORS  ((app_sensor_diag_t)0x0008U)

#define APP_SENSOR_ADC_BURST_TIMEOUT_MS  5U

#define APP_PRESSURE_ZERO_MIN_V          0.55f
#define APP_PRESSURE_ZERO_MAX_V          0.62f

typedef enum
{
    APP_SENSOR_STATUS_OK = 0,
    APP_SENSOR_STATUS_ADC_TIMEOUT,
    APP_SENSOR_STATUS_SENSOR_OFFLINE,
    APP_SENSOR_STATUS_MULTIPLE_ERRORS,
    APP_SENSOR_STATUS_INVALID,
} app_sensor_status_t;

typedef struct
{
    uint32_t timestamp_ms;

    uint16_t temp_raw;
    uint16_t press_raw;

    float temp_voltage_v;
    float press_voltage_v;

    float temp_resistance_ohm;

    float temp_celsius;
    float pressure_mpa;

    app_sensor_diag_t temp_diag;
    app_sensor_diag_t press_diag;
    app_sensor_diag_t global_diag;
} app_sensor_data_t;

void APP_Sensor_Init(void);
bool APP_Sensor_Collect(app_sensor_data_t *out_data);
app_sensor_status_t APP_Sensor_MapDiag(app_sensor_diag_t diag);

#ifdef __cplusplus
}
#endif

#endif /* APP_APP_SENSOR_H_ */
