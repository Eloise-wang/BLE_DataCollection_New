/*
 * proto_cmd_handler.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "proto_cmd_handler.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "app_storage.h"
#include "bsp_fs.h"
#include "bsp_uart.h"
#include "proto_frame.h"
#include "sensors.h"
#include "task_manager.h"

typedef struct
{
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[255];
} proto_cmd_msg_t;

static QueueHandle_t s_cmd_queue;
static proto_parser_t s_parser;

static proto_tx_fn_t s_tx_fn;
static void *s_tx_user;
static SemaphoreHandle_t s_tx_mutex;
static bool s_tx_is_uart;
static bool s_ble_connected;

static uint8_t s_uart_hex_buf[512];
static size_t s_uart_hex_len;

static void proto_tx_uart(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if ((data == NULL) || (len == 0U))
    {
        return;
    }
    (void)BSP_UART_Write(data, (uint32_t)len);
}

static int proto_hex_nibble(uint8_t c)
{
    if ((c >= (uint8_t)'0') && (c <= (uint8_t)'9'))
    {
        return (int)(c - (uint8_t)'0');
    }
    if ((c >= (uint8_t)'a') && (c <= (uint8_t)'f'))
    {
        return 10 + (int)(c - (uint8_t)'a');
    }
    if ((c >= (uint8_t)'A') && (c <= (uint8_t)'F'))
    {
        return 10 + (int)(c - (uint8_t)'A');
    }
    return -1;
}

static void proto_uart_hex_flush(void)
{
    uint8_t decoded[256];
    size_t dec_len = 0U;
    int hi = -1;

    for (size_t i = 0U; i < s_uart_hex_len; i++)
    {
        const int n = proto_hex_nibble(s_uart_hex_buf[i]);
        if (n < 0)
        {
            continue;
        }
        if (hi < 0)
        {
            hi = n;
        }
        else
        {
            decoded[dec_len++] = (uint8_t)(((uint8_t)hi << 4) | (uint8_t)n);
            hi = -1;
            if (dec_len >= sizeof(decoded))
            {
                break;
            }
        }
    }

    if (dec_len != 0U)
    {
        PROTO_OnRxBytes(decoded, dec_len);
    }

    s_uart_hex_len = 0U;
}

static void proto_send(const uint8_t *data, size_t len)
{
    if (s_tx_fn != NULL)
    {
        if ((!s_tx_is_uart) && (!s_ble_connected))
        {
            return;
        }
        if (s_tx_mutex != NULL)
        {
            (void)xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
        }
        s_tx_fn(data, len, s_tx_user);
        if (s_tx_mutex != NULL)
        {
            (void)xSemaphoreGive(s_tx_mutex);
        }
    }
}

static void proto_send_ack(uint8_t req_cmd, proto_status_t status)
{
    uint8_t buf[16];
    size_t out_len = 0U;
    if (PROTO_FrameBuildAck(req_cmd, status, buf, sizeof(buf), &out_len))
    {
        proto_send(buf, out_len);
    }
}

static bool proto_payload_read_u64_le(const uint8_t *p, size_t len, uint64_t *out)
{
    if ((p == NULL) || (out == NULL) || (len < 8U))
    {
        return false;
    }
    uint64_t v = 0U;
    for (uint8_t i = 0U; i < 8U; i++)
    {
        v |= ((uint64_t)p[i]) << (8U * i);
    }
    *out = v;
    return true;
}

static bool proto_payload_read_u32_le(const uint8_t *p, size_t len, uint32_t *out)
{
    if ((p == NULL) || (out == NULL) || (len < 4U))
    {
        return false;
    }
    *out = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

static bool proto_payload_read_u16_le(const uint8_t *p, size_t len, uint16_t *out)
{
    if ((p == NULL) || (out == NULL) || (len < 2U))
    {
        return false;
    }
    *out = (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
    return true;
}

void PROTO_CmdInit(void)
{
    if (s_cmd_queue == NULL)
    {
        s_cmd_queue = xQueueCreate(8U, (UBaseType_t)sizeof(proto_cmd_msg_t));
    }
    PROTO_FrameParserInit(&s_parser);

    if (s_tx_mutex == NULL)
    {
        s_tx_mutex = xSemaphoreCreateMutex();
    }

    if (s_tx_fn == NULL)
    {
        BSP_UART_Init();
        PROTO_CmdSetTx(proto_tx_uart, NULL);
    }
}

void PROTO_CmdSetTx(proto_tx_fn_t fn, void *user)
{
    s_tx_fn = fn;
    s_tx_user = user;
    s_tx_is_uart = (fn == proto_tx_uart);
}

void PROTO_SetBleConnected(bool connected)
{
    s_ble_connected = connected;
    TASK_SetBleConnected(connected);
}

void PROTO_TxRaw(const uint8_t *data, size_t len)
{
    proto_send(data, len);
}

void PROTO_SendStatusNow(void)
{
    const uint64_t task_id = TASK_GetActiveTaskId();
    const uint32_t bits = (g_system_event_group != NULL) ? (uint32_t)xEventGroupGetBits(g_system_event_group) : 0U;
    const uint8_t bat = SENSORS_GetBatteryLevel();

    uint8_t buf[1 + 2 + 8 + 4 + 1 + 1 + 2];
    size_t out_len = 0U;
    if (PROTO_StatusBuildFrame(task_id, bits, bat, buf, sizeof(buf), &out_len))
    {
        proto_send(buf, out_len);
    }
}

bool PROTO_SendRealtimeRecord(uint64_t task_id, const void *record, uint16_t record_size, uint32_t seq)
{
    if ((record == NULL) || (record_size == 0U))
    {
        return false;
    }

    const uint32_t offset = (uint32_t)(seq * (uint32_t)record_size);
    uint8_t buf[1 + 2 + 8 + 4 + 4 + 255 + 2];
    size_t out_len = 0U;
    if (!PROTO_DataBuildFrame(task_id, offset, 0U, (const uint8_t *)record, record_size, buf, sizeof(buf), &out_len))
    {
        return false;
    }

    proto_send(buf, out_len);
    return true;
}

void PROTO_OnRxBytes(const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    if (s_cmd_queue == NULL)
    {
        PROTO_CmdInit();
    }

    taskENTER_CRITICAL();
    for (size_t i = 0U; i < len; i++)
    {
        proto_cmd_frame_t frame;
        if (PROTO_FrameParseByte(&s_parser, data[i], &frame))
        {
            proto_cmd_msg_t msg;
            msg.cmd = frame.cmd;
            msg.len = frame.len;
            if (frame.len != 0U)
            {
                (void)memcpy(msg.payload, frame.payload, frame.len);
            }
            (void)xQueueSend(s_cmd_queue, &msg, 0U);
        }
    }
    taskEXIT_CRITICAL();
}

static void proto_print_rx(const proto_cmd_msg_t *msg)
{
    if (msg == NULL)
    {
        return;
    }

    BSP_UART_Print("RX CMD=0x%02X LEN=%u", msg->cmd, (unsigned)msg->len);
    if (msg->len != 0U)
    {
        BSP_UART_Print(" PAYLOAD=");
        for (uint8_t i = 0U; i < msg->len; i++)
        {
            BSP_UART_Print("%02X", msg->payload[i]);
        }
    }
    BSP_UART_Print("\r\n");
}

static void proto_handle_selftest(const proto_cmd_msg_t *msg)
{
    uint64_t task_id;
    if (!proto_payload_read_u64_le(msg->payload, msg->len, &task_id))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_PARAM_ERROR);
        return;
    }
    TASK_StartCollect(task_id, 0U, 0U, 1000U, false, 1U);
    proto_send_ack(msg->cmd, PROTO_STATUS_OK);
}

static void proto_handle_start_collect(const proto_cmd_msg_t *msg)
{
    if (msg->len < (uint8_t)(8U + 4U + 4U + 4U))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_LEN_ERROR);
        return;
    }

    uint64_t task_id;
    uint32_t start_s;
    uint32_t duration_s;
    uint32_t period_ms;

    if (!proto_payload_read_u64_le(&msg->payload[0], msg->len, &task_id) ||
        !proto_payload_read_u32_le(&msg->payload[8], msg->len - 8U, &start_s) ||
        !proto_payload_read_u32_le(&msg->payload[12], msg->len - 12U, &duration_s) ||
        !proto_payload_read_u32_le(&msg->payload[16], msg->len - 16U, &period_ms))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_PARAM_ERROR);
        return;
    }

    TASK_StartCollect(task_id, start_s, duration_s, period_ms, true, 0U);
    proto_send_ack(msg->cmd, PROTO_STATUS_OK);
}

static void proto_handle_stop_collect(const proto_cmd_msg_t *msg)
{
    uint64_t task_id;
    if (!proto_payload_read_u64_le(msg->payload, msg->len, &task_id))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_PARAM_ERROR);
        return;
    }

    if (!TASK_StopCollect(task_id))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_NOT_FOUND);
        return;
    }

    proto_send_ack(msg->cmd, PROTO_STATUS_OK);
}

static void proto_handle_clear_task(const proto_cmd_msg_t *msg)
{
    if (msg->len < (uint8_t)(8U + 1U))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_LEN_ERROR);
        return;
    }

    uint64_t task_id;
    if (!proto_payload_read_u64_le(&msg->payload[0], msg->len, &task_id))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_PARAM_ERROR);
        return;
    }

    const uint8_t mode = msg->payload[8];
    if (mode == 0U)
    {
        if (!APP_Storage_DeleteTask(task_id))
        {
            proto_send_ack(msg->cmd, PROTO_STATUS_STORAGE_ERROR);
            return;
        }
    }
    else
    {
        if (!BSP_FS_Format())
        {
            proto_send_ack(msg->cmd, PROTO_STATUS_STORAGE_ERROR);
            return;
        }
    }

    proto_send_ack(msg->cmd, PROTO_STATUS_OK);
}

static void proto_handle_request_history(const proto_cmd_msg_t *msg)
{
    if (msg->len < (uint8_t)(8U + 4U + 2U))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_LEN_ERROR);
        return;
    }

    uint64_t task_id;
    uint32_t offset;
    uint16_t max_bytes;

    if (!proto_payload_read_u64_le(&msg->payload[0], msg->len, &task_id) ||
        !proto_payload_read_u32_le(&msg->payload[8], msg->len - 8U, &offset) ||
        !proto_payload_read_u16_le(&msg->payload[12], msg->len - 12U, &max_bytes))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_PARAM_ERROR);
        return;
    }

    uint32_t total_bytes = 0U;
    if (!APP_Storage_GetDataSize(task_id, &total_bytes))
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_STORAGE_ERROR);
        return;
    }

    if (offset >= total_bytes)
    {
        proto_send_ack(msg->cmd, PROTO_STATUS_PARAM_ERROR);
        return;
    }

    uint16_t chunk_max = (max_bytes == 0U) ? 200U : max_bytes;
    if (chunk_max > 200U)
    {
        chunk_max = 200U;
    }

    uint8_t data_buf[200];
    uint8_t frame_buf[1 + 2 + 8 + 4 + 4 + 200 + 2];
    proto_send_ack(msg->cmd, PROTO_STATUS_OK);
    TASK_SetHistorySending(true);

    while (offset < total_bytes)
    {
        if ((!s_tx_is_uart) && (!s_ble_connected))
        {
            break;
        }

        uint16_t to_read = chunk_max;
        if ((uint32_t)to_read > (total_bytes - offset))
        {
            to_read = (uint16_t)(total_bytes - offset);
        }

        const int nread = APP_Storage_ReadData(task_id, offset, data_buf, (uint32_t)to_read);
        if (nread <= 0)
        {
            break;
        }

        size_t out_len = 0U;
        if (!PROTO_DataBuildFrame(task_id, offset, total_bytes, data_buf, (uint16_t)nread, frame_buf, sizeof(frame_buf), &out_len))
        {
            break;
        }

        proto_send(frame_buf, out_len);
        offset += (uint32_t)nread;
        vTaskDelay(pdMS_TO_TICKS(5U));
    }

    TASK_SetHistorySending(false);
}

void PROTO_UartRxTask(void *pvParameters)
{
    (void)pvParameters;

    PROTO_CmdInit();

    for (;;)
    {
        uint8_t buf[64];
        uint32_t n = 0U;

        if (BSP_UART_TryRead(buf, sizeof(buf), &n) && (n != 0U))
        {
            for (uint32_t i = 0U; i < n; i++)
            {
                const uint8_t c = buf[i];
                if (c == (uint8_t)'\n')
                {
                    proto_uart_hex_flush();
                }
                else if (c != (uint8_t)'\r')
                {
                    if (s_uart_hex_len < sizeof(s_uart_hex_buf))
                    {
                        s_uart_hex_buf[s_uart_hex_len++] = c;
                    }
                    else
                    {
                        s_uart_hex_len = 0U;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

void PROTO_CmdTask(void *pvParameters)
{
    (void)pvParameters;

    PROTO_CmdInit();

    for (;;)
    {
        proto_cmd_msg_t msg;
        if ((s_cmd_queue != NULL) && (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdTRUE))
        {
            proto_print_rx(&msg);
            switch (msg.cmd)
            {
                case PROTO_CMD_SELFTEST:
                    proto_handle_selftest(&msg);
                    break;
                case PROTO_CMD_START_COLLECT:
                    proto_handle_start_collect(&msg);
                    break;
                case PROTO_CMD_STOP_COLLECT:
                    proto_handle_stop_collect(&msg);
                    break;
                case PROTO_CMD_REQUEST_HISTORY:
                    proto_handle_request_history(&msg);
                    break;
                case PROTO_CMD_CLEAR_TASK:
                    proto_handle_clear_task(&msg);
                    break;
                default:
                    proto_send_ack(msg.cmd, PROTO_STATUS_CMD_UNSUPPORTED);
                    break;
            }
        }
    }
}
