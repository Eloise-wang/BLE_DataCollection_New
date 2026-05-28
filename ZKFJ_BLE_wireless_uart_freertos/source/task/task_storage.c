#include "task_manager.h"

#include <string.h>

#include "app_global.h"
#include "app_storage.h"
#include "bsp_uart.h"
#include "task.h"

static bool s_storage_ready;
static uint64_t s_storage_task_id;
static uint32_t s_storage_task_gen;
static TickType_t s_next_retry_tick;
static TickType_t s_last_begin_log_tick;
static bool task_storage_begin_if_needed(uint64_t task_id)
{
    if (s_storage_ready)
    {
        return true;
    }

    if (!APP_Storage_Init())
    {
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_begin_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_begin_log_tick = now;
            BSP_UART_Print("[STO] Init failed\r\n");
        }
        return false;
    }

    app_storage_task_meta_t meta;
    (void)memset(&meta, 0, sizeof(meta));
    meta.task_id            = task_id;
    meta.start_timestamp_ms = TASK_GetLocalBaseMs();
    meta.sample_period_ms   = TASK_GetCollectPeriodMs();
    meta.duration_ms        = TASK_GetCollectDurationS() * 1000U;
    meta.record_size        = (uint32_t)sizeof(sensor_record_t);
    meta.record_version     = 2U;

    const app_storage_phase_t phase = TASK_IsPretest() ? APP_STORAGE_PHASE_PRETEST : APP_STORAGE_PHASE_FORMAL;
    if (APP_Storage_BeginTaskEx(task_id, &meta, phase))
    {
        s_storage_ready = true;
        s_storage_task_gen = TASK_GetTaskGeneration();
        if (g_system_event_group != NULL)
        {
            (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_STORAGE_READY);
        }
        return true;
    }
    else
    {
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_begin_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_begin_log_tick = now;
            BSP_UART_Print("[STO] BeginTask failed\r\n");
        }
    }
    return false;
}

void TASK_StorageTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        if (g_sensor_data_queue == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(200U));
            continue;
        }

        const bool collecting = TASK_GetCollectEnabled();
        const UBaseType_t pending = uxQueueMessagesWaiting(g_sensor_data_queue);
        const bool need_storage_session = collecting || (pending > 0U);

        const uint64_t active_task_id = TASK_GetActiveTaskId();
        const uint32_t active_gen = TASK_GetTaskGeneration();
        const TickType_t now = xTaskGetTickCount();
        if (!s_storage_ready)
        {
            if (!need_storage_session)
            {
                vTaskDelay(pdMS_TO_TICKS(100U));
                continue;
            }
            if (now < s_next_retry_tick)
            {
                vTaskDelay(pdMS_TO_TICKS(50U));
                continue;
            }
            s_storage_task_id = active_task_id;
            if (!task_storage_begin_if_needed(s_storage_task_id))
            {
                s_next_retry_tick = now + pdMS_TO_TICKS(500U);
                vTaskDelay(pdMS_TO_TICKS(50U));
                continue;
            }
        }
        else if ((active_task_id == s_storage_task_id) && (active_gen != s_storage_task_gen))
        {
            s_storage_ready = false;
            if (g_system_event_group != NULL)
            {
                (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_STORAGE_READY);
            }
            (void)xQueueReset(g_sensor_data_queue);
            s_storage_task_id = active_task_id;
            if (need_storage_session)
            {
                (void)task_storage_begin_if_needed(s_storage_task_id);
            }
        }
        else if ((active_task_id != s_storage_task_id) && (uxQueueMessagesWaiting(g_sensor_data_queue) == 0U))
        {
            s_storage_ready = false;
            if (g_system_event_group != NULL)
            {
                (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_STORAGE_READY);
            }
            s_storage_task_id = active_task_id;
            if (need_storage_session)
            {
                (void)task_storage_begin_if_needed(s_storage_task_id);
            }
        }

        sensor_record_t record;
        if (xQueueReceive(g_sensor_data_queue, &record, pdMS_TO_TICKS(500U)) == pdTRUE)
        {
            if (s_storage_ready)
            {
                const bool pre = TASK_IsPretest();
                const bool ok = pre ?
                                    APP_Storage_AppendPreData(s_storage_task_id, &record, (uint32_t)sizeof(record)) :
                                    APP_Storage_AppendData(s_storage_task_id, &record, (uint32_t)sizeof(record));
                (void)ok;
            }
        }
    }
}
