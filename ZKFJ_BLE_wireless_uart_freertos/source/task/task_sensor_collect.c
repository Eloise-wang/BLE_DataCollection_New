#include "task_manager.h"

#include <string.h>
#include <stdint.h>

#include "app_can.h"
#include "app_global.h"
#include "app_sensor.h"
#include "bsp_uart.h"
#include "proto_cmd_handler.h"
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

static uint32_t task_elapsed_ms_u32(uint32_t now_ms, uint32_t base_ms)
{
    if (now_ms >= base_ms)
    {
        return now_ms - base_ms;
    }
    return (UINT32_MAX - base_ms + 1U) + now_ms;
}

void TASK_SensorCollectTask(void *pvParameters)
{
    (void)pvParameters;

    APP_Sensor_Init();
    APP_CAN_Init();

    uint32_t stream_seq = 0U;
    uint32_t last_task_gen = TASK_GetTaskGeneration();

    TickType_t lastWake = xTaskGetTickCount();

    for (;;)
    {
        if (g_system_event_group == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }

        const EventBits_t bits = xEventGroupGetBits(g_system_event_group);
        if ((bits & TASK_EVENT_BIT_COLLECT_RUNNING) == 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }

        const uint32_t gen = TASK_GetTaskGeneration();
        if (gen != last_task_gen)
        {
            last_task_gen = gen;
            stream_seq    = 0U;
            lastWake      = xTaskGetTickCount();
        }

        const TickType_t period_ticks = pdMS_TO_TICKS(TASK_GetCollectPeriodMs());
        vTaskDelayUntil(&lastWake, period_ticks);

        const uint64_t task_id = TASK_GetActiveTaskId();

        APP_CAN_Process();

        app_sensor_data_t sensor;
        (void)APP_Sensor_Collect(&sensor);

        algo_can_gcu1_t gcu1;
        app_can_diag_t can_diag;
        (void)APP_CAN_GetLatest(&gcu1, NULL, NULL, &can_diag);

        sensor_record_t record;
        (void)memset(&record, 0, sizeof(record));

        uint32_t host_epoch_s = 0U;
        uint32_t local_base_ms = 0U;
        if (TASK_GetTimeBase(&host_epoch_s, &local_base_ms))
        {
            const uint32_t elapsed_ms = task_elapsed_ms_u32(sensor.timestamp_ms, local_base_ms);
            record.timestamp_s  = host_epoch_s + (elapsed_ms / 1000U);
            record.timestamp_ms = (uint16_t)(elapsed_ms % 1000U);
        }
        else
        {
            record.timestamp_s  = sensor.timestamp_ms / 1000U;
            record.timestamp_ms = (uint16_t)(sensor.timestamp_ms % 1000U);
        }

        record.temperature_centi_c  = task_sensor_temp_to_centi_c(sensor.temp_celsius);
        record.pressure_kpa         = task_sensor_pressure_to_kpa(sensor.pressure_mpa);
        record.status               = APP_RECORD_STATUS_OK;
        record.liquid_access_state  = gcu1.liquidLevelSenAcSt;

        if (record.liquid_access_state == 0U)
        {
            record.liquid_level = 0xFFFFU;
            record.status |= APP_RECORD_STATUS_LIQUID_OFFLINE;
        }
        else
        {
            record.liquid_level = gcu1.residualFluidVolume;
        }

        if (sensor.global_diag != APP_SENSOR_DIAG_OK)
        {
            record.status |= APP_RECORD_STATUS_SENSOR_ERROR;
        }
        if (can_diag != APP_CAN_DIAG_OK)
        {
            record.status |= APP_RECORD_STATUS_CAN_ERROR;
        }

        TASK_RequestCollectionPulse();
        record.remaining_count = TASK_ConsumeOneSample();

#if BSP_UART_PRINT_ENABLE
        const uint32_t total = TASK_GetCollectTotalCount();
        const uint32_t sample_index = (total != 0U) ? (total - record.remaining_count) : 0U;

        BSP_UART_Print("sample=%lu remaining=%lu\r\n", (unsigned long)sample_index, (unsigned long)record.remaining_count);

        const uint32_t press_mv = (uint32_t)(sensor.press_voltage_v * 1000.0f + 0.5f);
        const uint32_t temp_mv  = (uint32_t)(sensor.temp_voltage_v * 1000.0f + 0.5f);

        const unsigned pressure_int = (unsigned)(record.pressure_kpa / 1000U);
        const unsigned pressure_frac = (unsigned)(record.pressure_kpa % 1000U);

        int temp_int = (int)(record.temperature_centi_c / 100);
        int temp_frac = (int)(record.temperature_centi_c % 100);
        if (temp_frac < 0)
        {
            temp_frac = -temp_frac;
        }

        BSP_UART_Print("pressure_raw=%u (mV=%lu) | temperature_raw=%u (mV=%lu)\r\n",
                       (unsigned)sensor.press_raw,
                       (unsigned long)press_mv,
                       (unsigned)sensor.temp_raw,
                       (unsigned long)temp_mv);
        if ((record.status & APP_RECORD_STATUS_LIQUID_OFFLINE) != 0U)
        {
            BSP_UART_Print("pressure=%u.%03u(MPa) temperature=%d.%02d(C) liquid_level=-1(L)\r\n",
                           pressure_int,
                           pressure_frac,
                           temp_int,
                           temp_frac);
        }
        else
        {
            BSP_UART_Print("pressure=%u.%03u(MPa) temperature=%d.%02d(C) liquid_level=%u(L)\r\n",
                           pressure_int,
                           pressure_frac,
                           temp_int,
                           temp_frac,
                           (unsigned)record.liquid_level);
        }
#endif

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

        (void)PROTO_SendRealtimeRecord(task_id, &record, (uint16_t)sizeof(record), stream_seq);
        stream_seq++;
    }
}
