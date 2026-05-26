#include "task_manager.h"

#include <string.h>

#include "app_global.h"
#include "app_storage.h"
#include "bsp_uart.h"
#include "task.h"

#ifndef TASK_STORAGE_VERIFY_READBACK_ENABLE
#define TASK_STORAGE_VERIFY_READBACK_ENABLE 1
#endif

static bool s_storage_ready;
static uint64_t s_storage_task_id;
static uint32_t s_storage_task_gen;
static TickType_t s_next_retry_tick;
static TickType_t s_last_begin_log_tick;
static bool s_logged_first_write;
static uint32_t s_verify_ok_count;
static uint32_t s_verify_fail_count;
static uint32_t s_verify_total_count;

#if TASK_STORAGE_VERIFY_READBACK_ENABLE
static void task_storage_print_record(const char *tag, const sensor_record_t *r)
{
    if ((tag == NULL) || (r == NULL))
    {
        return;
    }
    BSP_UART_Print("[STO] %s ts=%u.%03u temp=%d press=%u lvl=%u rem=%u acc=%u st=0x%02X\r\n",
                   tag,
                   (unsigned)r->timestamp_s,
                   (unsigned)r->timestamp_ms,
                   (int)r->temperature_centi_c,
                   (unsigned)r->pressure_kpa,
                   (unsigned)r->liquid_level,
                   (unsigned)r->remaining_count,
                   (unsigned)r->liquid_access_state,
                   (unsigned)r->status);
}

static void task_storage_print_hex18(const char *tag, const uint8_t *p)
{
    if ((tag == NULL) || (p == NULL))
    {
        return;
    }
    BSP_UART_Print("[STO] %s %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\r\n",
                   tag,
                   (unsigned)p[0], (unsigned)p[1], (unsigned)p[2], (unsigned)p[3], (unsigned)p[4], (unsigned)p[5],
                   (unsigned)p[6], (unsigned)p[7], (unsigned)p[8], (unsigned)p[9], (unsigned)p[10], (unsigned)p[11],
                   (unsigned)p[12], (unsigned)p[13], (unsigned)p[14], (unsigned)p[15], (unsigned)p[16], (unsigned)p[17]);
}

static void task_storage_verify_tail(uint64_t task_id, bool pretest, const sensor_record_t *expected)
{
    if (expected == NULL)
    {
        return;
    }

    uint32_t size = 0U;
    bool ok = false;
    if (pretest)
    {
        ok = APP_Storage_GetPreDataSize(task_id, &size);
    }
    else
    {
        ok = APP_Storage_GetDataSize(task_id, &size);
    }

    if (!ok)
    {
        s_verify_fail_count++;
        BSP_UART_Print("[STO] VerifyTail failed: size query (%s)\r\n", pretest ? "pre" : "formal");
        (void)APP_Storage_LogPrintf(task_id, "[STO] VerifyTail failed: size query (%s)\n", pretest ? "pre" : "formal");
        return;
    }

    if (size < (uint32_t)sizeof(sensor_record_t))
    {
        s_verify_fail_count++;
        BSP_UART_Print("[STO] VerifyTail failed: size too small=%u (%s)\r\n", (unsigned)size, pretest ? "pre" : "formal");
        (void)APP_Storage_LogPrintf(task_id, "[STO] VerifyTail failed: size too small=%u (%s)\n", (unsigned)size, pretest ? "pre" : "formal");
        return;
    }

    const uint32_t off = size - (uint32_t)sizeof(sensor_record_t);
    sensor_record_t actual;
    int nread = -1;
    if (pretest)
    {
        nread = APP_Storage_ReadPreData(task_id, off, &actual, (uint32_t)sizeof(actual));
    }
    else
    {
        nread = APP_Storage_ReadData(task_id, off, &actual, (uint32_t)sizeof(actual));
    }

    if (nread != (int)sizeof(actual))
    {
        s_verify_fail_count++;
        BSP_UART_Print("[STO] VerifyTail failed: read ret=%d off=%u (%s)\r\n", nread, (unsigned)off, pretest ? "pre" : "formal");
        (void)APP_Storage_LogPrintf(task_id, "[STO] VerifyTail failed: read ret=%d off=%u (%s)\n",
                                    nread, (unsigned)off, pretest ? "pre" : "formal");
        return;
    }

    if (memcmp(&actual, expected, sizeof(actual)) != 0)
    {
        s_verify_fail_count++;
        BSP_UART_Print("[STO] VerifyTail mismatch off=%u (%s)\r\n", (unsigned)off, pretest ? "pre" : "formal");
        task_storage_print_record("exp", expected);
        task_storage_print_record("act", &actual);
        task_storage_print_hex18("exp_hex", (const uint8_t *)expected);
        task_storage_print_hex18("act_hex", (const uint8_t *)&actual);
        (void)APP_Storage_LogPrintf(task_id,
                                    "[STO] VerifyTail mismatch off=%u (%s) exp_ts=%u.%03u act_ts=%u.%03u exp_rem=%u act_rem=%u\n",
                                    (unsigned)off,
                                    pretest ? "pre" : "formal",
                                    (unsigned)expected->timestamp_s,
                                    (unsigned)expected->timestamp_ms,
                                    (unsigned)actual.timestamp_s,
                                    (unsigned)actual.timestamp_ms,
                                    (unsigned)expected->remaining_count,
                                    (unsigned)actual.remaining_count);
        return;
    }

    s_verify_ok_count++;
    if (s_verify_total_count <= 5U)
    {
        BSP_UART_Print("[STO] VerifyTail ok off=%u (%s)\r\n", (unsigned)off, pretest ? "pre" : "formal");
    }
}
#endif


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
        s_logged_first_write = false;
        s_verify_ok_count = 0U;
        s_verify_fail_count = 0U;
        s_verify_total_count = 0U;
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
                if (ok && (!s_logged_first_write))
                {
                    s_logged_first_write = true;
                    BSP_UART_Print("[STO] First write ok\r\n");
                }

#if TASK_STORAGE_VERIFY_READBACK_ENABLE
                if (ok)
                {
                    s_verify_total_count++;
                    if ((s_verify_total_count <= 5U) || ((s_verify_total_count & 0x1FU) == 0U))
                    {
                        task_storage_verify_tail(s_storage_task_id, pre, &record);
                    }
                }
#endif
            }
        }
    }
}
