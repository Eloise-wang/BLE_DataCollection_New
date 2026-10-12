#include "task_manager.h"

#include <assert.h>

#include "app_global.h"
#include "app_storage.h"
#include "bsp_uart.h"
#include "bsp_wdog.h"
#include "proto_cmd_handler.h"
#include "task.h"
#include "fsl_os_abstraction.h"

QueueHandle_t g_sensor_data_queue;
EventGroupHandle_t g_system_event_group;

static uint64_t s_active_task_id = 1U;
static uint32_t s_task_gen;

typedef struct
{
    uint32_t host_epoch_s;
    uint32_t local_base_ms;
    uint32_t duration_s;
    uint32_t period_ms;
    uint32_t total_count;
    uint32_t remaining_count;
    bool time_valid;
    bool is_pretest;
    bool resumed;
} task_collect_plan_t;

static task_collect_plan_t s_collect;

extern void TASK_SensorCollectTask(void *pvParameters);
extern void TASK_StorageTask(void *pvParameters);
extern void TASK_LedTask(void *pvParameters);
extern void TASK_WatchdogTask(void *pvParameters);
extern void APP_Storage_EraseTask(void *pvParameters);

void TASK_InitPipelineResources(void)
{
    if (g_sensor_data_queue == NULL)
    {
        g_sensor_data_queue = xQueueCreate(TASK_SENSOR_QUEUE_DEPTH, (UBaseType_t)sizeof(sensor_record_t));
        assert(g_sensor_data_queue != NULL);
    }

    if (g_system_event_group == NULL)
    {
        g_system_event_group = xEventGroupCreate();
        assert(g_system_event_group != NULL);
    }
    if (g_system_event_group != NULL)
    {
        (void)xEventGroupClearBits(g_system_event_group,
                                   TASK_EVENT_BIT_COLLECT_RUNNING | TASK_EVENT_BIT_STORAGE_READY | TASK_EVENT_BIT_QUEUE_DROPPED |
                                       TASK_EVENT_BIT_COLLECT_DONE | TASK_EVENT_BIT_BLE_CONNECTED | TASK_EVENT_BIT_HISTORY_SENDING |
                                       TASK_EVENT_BIT_COLLECTION_PULSE | TASK_EVENT_BIT_FINALIZE_PENDING);
    }
}

void TASK_CreateAllTasks(void)
{
    (void)xTaskCreate(TASK_WatchdogTask, "Task_Wdog", TASK_STACK_WDOG, NULL, 5U, NULL);
    (void)xTaskCreate(PROTO_CmdTask, "Task_Proto", TASK_STACK_PROTO, NULL, 4U, NULL);
    (void)xTaskCreate(TASK_SensorCollectTask, "Task_Sensor", TASK_STACK_SENSOR, NULL, 3U, NULL);
    (void)xTaskCreate(TASK_StorageTask, "Task_Storage", TASK_STACK_STORAGE, NULL, 2U, NULL);
    (void)xTaskCreate(TASK_LedTask, "Task_Led", TASK_STACK_LED, NULL, 1U, NULL);
    (void)xTaskCreate(APP_Storage_EraseTask, "Task_Erase", TASK_STACK_SENSOR, NULL, 2U, NULL);
}

uint64_t TASK_GetActiveTaskId(void)
{
    return s_active_task_id;
}

void TASK_SetActiveTaskId(uint64_t task_id)
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

void TASK_StartCollect(uint64_t task_id,
                       uint32_t host_epoch_s,
                       uint32_t duration_s,
                       uint32_t period_ms,
                       bool time_valid,
                       uint32_t forced_total_count)
{
    if ((task_id == 0U) || (g_system_event_group == NULL))
    {
        return;
    }

    if (period_ms == 0U)
    {
        period_ms = 1000U;
    }

    uint32_t total_count = forced_total_count;
    if (total_count == 0U)
    {
        if ((duration_s != 0U) && (period_ms != 0U))
        {
            const uint32_t duration_ms = duration_s * 1000U;
            total_count = (duration_ms + period_ms - 1U) / period_ms;
        }
    }

    taskENTER_CRITICAL();
    s_active_task_id          = task_id;
    s_task_gen++;
    s_collect.host_epoch_s    = host_epoch_s;
    s_collect.local_base_ms   = OSA_TimeGetMsec();
    s_collect.duration_s      = duration_s;
    s_collect.period_ms       = period_ms;
    s_collect.total_count     = total_count;
    s_collect.remaining_count = total_count;
    s_collect.time_valid      = time_valid;
    s_collect.is_pretest      = !time_valid;
    s_collect.resumed         = false;
    taskEXIT_CRITICAL();

    (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_DONE | TASK_EVENT_BIT_FINALIZE_PENDING);

    if (total_count != 0U)
    {
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
    else
    {
        (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
}

bool TASK_IsPretest(void)
{
    bool v;
    taskENTER_CRITICAL();
    v = s_collect.is_pretest;
    taskEXIT_CRITICAL();
    return v;
}

bool TASK_IsResumed(void)
{
    bool v;
    taskENTER_CRITICAL();
    v = s_collect.resumed;
    taskEXIT_CRITICAL();
    return v;
}

void TASK_StartCollectResume(uint64_t task_id,
                             uint32_t host_epoch_s,
                             uint32_t last_ts_ms,
                             uint32_t duration_s,
                             uint32_t period_ms,
                             uint32_t remaining_count)
{
    if ((task_id == 0U) || (g_system_event_group == NULL))
    {
        return;
    }

    if (period_ms == 0U)
    {
        period_ms = 1000U;
    }

    uint32_t total_count = 0U;
    if (duration_s != 0U)
    {
        const uint32_t duration_ms = duration_s * 1000U;
        total_count = (duration_ms + period_ms - 1U) / period_ms;
    }

    taskENTER_CRITICAL();
    s_active_task_id          = task_id;
    s_task_gen++;
    s_collect.host_epoch_s    = host_epoch_s;
    /* 采集循环会先 delay 一个 period 才采第一条：令首条 elapsed = last_ts_ms + period，
       时间即从最后一条记录自然接续（u32 回绕安全） */
    s_collect.local_base_ms   = OSA_TimeGetMsec() - last_ts_ms;
    s_collect.duration_s      = duration_s;
    s_collect.period_ms       = period_ms;
    s_collect.total_count     = total_count;
    s_collect.remaining_count = remaining_count;
    s_collect.time_valid      = true;
    s_collect.is_pretest      = false;
    s_collect.resumed         = true;
    taskEXIT_CRITICAL();

    (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_DONE | TASK_EVENT_BIT_FINALIZE_PENDING);

    if (remaining_count != 0U)
    {
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
    else
    {
        (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    }
}

bool TASK_StopCollect(uint64_t task_id)
{
    if (g_system_event_group == NULL)
    {
        return false;
    }

    if (TASK_GetActiveTaskId() != task_id)
    {
        return false;
    }

    TASK_SetCollectEnabled(false);
    TASK_MarkCollectDone();
    return true;
}

uint32_t TASK_GetTaskGeneration(void)
{
    uint32_t v;
    taskENTER_CRITICAL();
    v = s_task_gen;
    taskEXIT_CRITICAL();
    return v;
}

uint32_t TASK_GetCollectPeriodMs(void)
{
    uint32_t v;
    taskENTER_CRITICAL();
    v = s_collect.period_ms;
    taskEXIT_CRITICAL();
    return (v == 0U) ? 1000U : v;
}

uint32_t TASK_GetCollectDurationS(void)
{
    uint32_t v;
    taskENTER_CRITICAL();
    v = s_collect.duration_s;
    taskEXIT_CRITICAL();
    return v;
}

uint32_t TASK_GetCollectTotalCount(void)
{
    uint32_t v;
    taskENTER_CRITICAL();
    v = s_collect.total_count;
    taskEXIT_CRITICAL();
    return v;
}

uint32_t TASK_GetCollectRemainingCount(void)
{
    uint32_t v;
    taskENTER_CRITICAL();
    v = s_collect.remaining_count;
    taskEXIT_CRITICAL();
    return v;
}

uint32_t TASK_ConsumeOneSample(void)
{
    if (g_system_event_group == NULL)
    {
        return 0U;
    }

    uint32_t remaining;
    taskENTER_CRITICAL();
    remaining = s_collect.remaining_count;
    if (remaining != 0U)
    {
        remaining--;
        s_collect.remaining_count = remaining;
    }
    taskEXIT_CRITICAL();

    if (remaining == 0U)
    {
        (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_DONE);
        /* 自然采完：请求把状态落盘为 COMPLETED（初测无 header 状态，跳过） */
        if (!TASK_IsPretest())
        {
            TASK_RequestFinalize();
        }
    }

    return remaining;
}

bool TASK_GetTimeBase(uint32_t *host_epoch_s, uint32_t *local_base_ms)
{
    if ((host_epoch_s == NULL) || (local_base_ms == NULL))
    {
        return false;
    }

    bool ok;
    taskENTER_CRITICAL();
    ok            = s_collect.time_valid;
    *host_epoch_s = s_collect.host_epoch_s;
    *local_base_ms = s_collect.local_base_ms;
    taskEXIT_CRITICAL();
    return ok;
}

uint32_t TASK_GetLocalBaseMs(void)
{
    uint32_t v;
    taskENTER_CRITICAL();
    v = s_collect.local_base_ms;
    taskEXIT_CRITICAL();
    return v;
}

void TASK_MarkCollectDone(void)
{
    if (g_system_event_group == NULL)
    {
        return;
    }
    (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_RUNNING);
    (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECT_DONE);
}

bool TASK_IsCollectDone(void)
{
    if (g_system_event_group == NULL)
    {
        return false;
    }
    const EventBits_t bits = xEventGroupGetBits(g_system_event_group);
    return (bits & TASK_EVENT_BIT_COLLECT_DONE) != 0U;
}

void TASK_SetBleConnected(bool connected)
{
    if (g_system_event_group == NULL)
    {
        return;
    }
    if (connected)
    {
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_BLE_CONNECTED);
    }
    else
    {
        (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_BLE_CONNECTED);
    }
}

void TASK_SetHistorySending(bool sending)
{
    if (g_system_event_group == NULL)
    {
        return;
    }
    if (sending)
    {
        (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_HISTORY_SENDING);
    }
    else
    {
        (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_HISTORY_SENDING);
    }
}

void TASK_RequestCollectionPulse(void)
{
    if (g_system_event_group == NULL)
    {
        return;
    }
    (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_COLLECTION_PULSE);
}

void TASK_RequestFinalize(void)
{
    if (g_system_event_group == NULL)
    {
        return;
    }
    (void)xEventGroupSetBits(g_system_event_group, TASK_EVENT_BIT_FINALIZE_PENDING);
}

void TASK_ResumeCollect(void)
{
    if (g_system_event_group == NULL)
    {
        return;
    }

    if (!APP_Storage_Init())
    {
        BSP_UART_Print("[RSM] storage init fail, no resume\r\n");
        return;
    }

    uint64_t task_id = 0U;
    app_storage_task_meta_t meta;
    app_store_state_t state = APP_STORE_STATE_UNKNOWN;
    uint32_t host_epoch_s = 0U;
    if (!APP_Storage_FindResumable(&task_id, &meta, &state, &host_epoch_s))
    {
        return;
    }
    if (state != APP_STORE_STATE_RUNNING)
    {
        return;
    }

    uint32_t period_ms = meta.sample_period_ms;
    if (period_ms == 0U)
    {
        period_ms = 1000U;
    }
    const uint32_t duration_s = meta.duration_ms / 1000U;

    const uint32_t rec_size = (uint32_t)sizeof(sensor_record_t);
    uint32_t remaining     = 0U;
    uint32_t last_ts_s     = host_epoch_s;
    uint32_t last_ts_ms    = 0U;
    bool     has_records   = false;

    uint32_t data_bytes = 0U;
    if (APP_Storage_GetDataSize(task_id, &data_bytes) && (data_bytes >= rec_size))
    {
        sensor_record_t last;
        if (APP_Storage_ReadData(task_id, data_bytes - rec_size, &last, rec_size) == (int)rec_size)
        {
            has_records = true;
            remaining   = last.remaining_count;
            last_ts_s   = last.timestamp_s;
            last_ts_ms  = last.timestamp_ms;
        }
    }

    if (has_records && (remaining == 0U))
    {
        /* 最后一条即最后一样本：视为已完成，落盘并阻止续采 */
        (void)APP_Storage_SetTaskState(task_id, APP_STORE_STATE_COMPLETED);
        BSP_UART_Print("[RSM] task already done, skip\r\n");
        return;
    }

    if (!has_records)
    {
        if ((host_epoch_s == 0U) || (host_epoch_s == 0xFFFFFFFFu))
        {
            /* 无记录且无绝对时间基准：无法建立时间连续性，放弃续采 */
            BSP_UART_Print("[RSM] no record & no epoch, skip\r\n");
            return;
        }
        /* 尚无任何记录落盘：剩余=总数，时间从 header 的 epoch 起 */
        const uint32_t total = (duration_s != 0U)
                                   ? (uint32_t)(((uint64_t)duration_s * 1000ULL + period_ms - 1U) / period_ms)
                                   : 0U;
        remaining  = total;
        last_ts_s  = host_epoch_s;
        last_ts_ms = 0U;
    }

    /* 把"中断本次采集的复位原因/计数"持久化到 flash（供跨多次复位追溯） */
    (void)APP_Storage_SetTaskInterruptReason(task_id,
                                             BSP_WDOG_GetLatchedResetStatus(),
                                             BSP_WDOG_GetLatchedResetCount());

    TASK_StartCollectResume(task_id, last_ts_s, last_ts_ms, duration_s, period_ms, remaining);

    char tid[17];
    BSP_UART_FormatU64LE(tid, task_id);
    BSP_UART_Print("[RSM] resume task=%s remain=%lu period=%lu ts_s=%lu\r\n",
                   tid, (unsigned long)remaining, (unsigned long)period_ms, (unsigned long)last_ts_s);
}
