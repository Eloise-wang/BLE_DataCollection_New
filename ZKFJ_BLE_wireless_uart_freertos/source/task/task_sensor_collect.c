#include "task_manager.h"

#include <string.h>

#include "app_can.h"
#include "app_global.h"
#include "app_sensor.h"
#include "task.h"

static int16_t task_sensor_temp_to_centi_c(float celsius)
{
    float scaled = celsius * 100.0f;
    if (scaled > 32767.0f)
    {
        scaled = 32767.0f;
    }
    if (scaled < -32768.0f)
    {
        scaled = -32768.0f;
    }
    return (int16_t)scaled;
}

static uint16_t task_sensor_pressure_to_kpa(float mpa)
{
    float scaled = mpa * 1000.0f;
    if (scaled < 0.0f)
    {
        scaled = 0.0f;
    }
    if (scaled > 65535.0f)
    {
        scaled = 65535.0f;
    }
    return (uint16_t)scaled;
}

void TASK_SensorCollectTask(void *pvParameters)
{
    (void)pvParameters;

    APP_Sensor_Init();
    APP_CAN_Init();

    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000U);

    for (;;)
    {
        vTaskDelayUntil(&lastWake, period);

        if (g_system_event_group == NULL)
        {
            continue;
        }

        const EventBits_t bits = xEventGroupGetBits(g_system_event_group);
        if ((bits & TASK_EVENT_BIT_COLLECT_RUNNING) == 0U)
        {
            continue;
        }

        APP_CAN_Process();

        app_sensor_data_t sensor;
        (void)APP_Sensor_Collect(&sensor);

        algo_can_gcu1_t gcu1;
        app_can_diag_t can_diag;
        (void)APP_CAN_GetLatest(&gcu1, NULL, NULL, &can_diag);

        sensor_record_t record;
        (void)memset(&record, 0, sizeof(record));
        record.timestamp_ms         = sensor.timestamp_ms;
        record.temperature_centi_c  = task_sensor_temp_to_centi_c(sensor.temp_celsius);
        record.pressure_kpa         = task_sensor_pressure_to_kpa(sensor.pressure_mpa);
        record.liquid_level         = gcu1.residualFluidVolume;
        record.status               = APP_RECORD_STATUS_OK;

        if (sensor.global_diag != APP_SENSOR_DIAG_OK)
        {
            record.status |= APP_RECORD_STATUS_SENSOR_ERROR;
        }
        if (can_diag != APP_CAN_DIAG_OK)
        {
            record.status |= APP_RECORD_STATUS_CAN_ERROR;
        }

        if (g_sensor_data_queue != NULL)
        {
            if (xQueueSend(g_sensor_data_queue, &record, 0U) != pdPASS)
            {
                if (g_system_event_group != NULL)
                {
                    (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_QUEUE_DROPPED);
                }
            }
            else
            {
                if (g_system_event_group != NULL)
                {
                    (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_QUEUE_DROPPED);
                }
            }
        }
    }
}
