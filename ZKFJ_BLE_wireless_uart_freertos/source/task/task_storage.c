#include "task_manager.h"

#include <string.h>

#include "app_global.h"
#include "app_storage.h"
#include "task.h"

static bool s_storage_ready;
static uint32_t s_storage_task_id;

static void task_storage_begin_if_needed(uint32_t task_id)
{
    if (s_storage_ready)
    {
        return;
    }

    if (!APP_Storage_Init())
    {
        return;
    }

    app_storage_task_meta_t meta;
    (void)memset(&meta, 0, sizeof(meta));
    meta.task_id            = task_id;
    meta.start_timestamp_ms = 0U;
    meta.sample_period_ms   = 1000U;
    meta.duration_ms        = 0U;
    meta.record_size        = (uint32_t)sizeof(sensor_record_t);
    meta.record_version     = 1U;

    if (APP_Storage_BeginTask(task_id, &meta))
    {
        s_storage_ready = true;
        if (g_system_event_group != NULL)
        {
            (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_STORAGE_READY);
        }
    }
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

        const uint32_t active_task_id = TASK_GetActiveTaskId();
        if (!s_storage_ready)
        {
            s_storage_task_id = active_task_id;
            task_storage_begin_if_needed(s_storage_task_id);
        }
        else if ((active_task_id != s_storage_task_id) && (uxQueueMessagesWaiting(g_sensor_data_queue) == 0U))
        {
            s_storage_ready = false;
            if (g_system_event_group != NULL)
            {
                (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_STORAGE_READY);
            }
            s_storage_task_id = active_task_id;
            task_storage_begin_if_needed(s_storage_task_id);
        }

        sensor_record_t record;
        if (xQueueReceive(g_sensor_data_queue, &record, pdMS_TO_TICKS(500U)) == pdTRUE)
        {
            if (s_storage_ready)
            {
                (void)APP_Storage_AppendData(s_storage_task_id, &record, (uint32_t)sizeof(record));
            }
        }
    }
}
