#include "task_manager.h"

#include <assert.h>

#include "app_global.h"
#include "task.h"

QueueHandle_t g_sensor_data_queue;
EventGroupHandle_t g_system_event_group;

static uint32_t s_active_task_id = 1U;

extern void TASK_SensorCollectTask(void *pvParameters);
extern void TASK_StorageTask(void *pvParameters);
extern void TASK_LedTask(void *pvParameters);
extern void TASK_WatchdogTask(void *pvParameters);

void TASK_InitPipelineResources(void)
{
    if (g_sensor_data_queue == NULL)
    {
        g_sensor_data_queue = xQueueCreate(20U, (UBaseType_t)sizeof(sensor_record_t));
        assert(g_sensor_data_queue != NULL);
    }

    if (g_system_event_group == NULL)
    {
        g_system_event_group = xEventGroupCreate();
        assert(g_system_event_group != NULL);
    }

    if (g_system_event_group != NULL)
    {
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
}

void TASK_CreateAllTasks(void)
{
    (void)xTaskCreate(TASK_WatchdogTask, "Task_Wdog", 256U, NULL, 5U, NULL);
    (void)xTaskCreate(TASK_SensorCollectTask, "Task_Sensor", 384U, NULL, 3U, NULL);
    (void)xTaskCreate(TASK_StorageTask, "Task_Storage", 1024U, NULL, 2U, NULL);
    (void)xTaskCreate(TASK_LedTask, "Task_Led", 256U, NULL, 1U, NULL);
}

uint32_t TASK_GetActiveTaskId(void)
{
    return s_active_task_id;
}

void TASK_SetActiveTaskId(uint32_t task_id)
{
    if (task_id == 0U)
    {
        return;
    }
    s_active_task_id = task_id;
}

void TASK_SetCollectEnabled(bool enabled)
{
    if (g_system_event_group == NULL)
    {
        return;
    }

    if (enabled)
    {
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
    else
    {
        (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
}

bool TASK_GetCollectEnabled(void)
{
    if (g_system_event_group == NULL)
    {
        return false;
    }

    const EventBits_t bits = xEventGroupGetBits(g_system_event_group);
    return (bits & TASK_EVENT_BIT_COLLECT_RUNNING) != 0U;
}
