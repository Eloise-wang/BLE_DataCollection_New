/*
 * proto_frame.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "proto_frame.h"

#include <string.h>

#include "algo_crc.h"

static uint16_t proto_crc16_calc(const uint8_t *data, size_t len)
{
    return algo_crc16_calc(data, len);
}

void PROTO_FrameParserInit(proto_parser_t *p)
{
    if (p == NULL)
    {
        return;
    }
    (void)memset(p, 0, sizeof(*p));
    p->state = PROTO_PARSE_WAIT_SOF;
}

static void proto_reset(proto_parser_t *p)
{
    p->state = PROTO_PARSE_WAIT_SOF;
    p->payload_pos = 0U;
    p->crc_rx = 0U;
    (void)memset(&p->frame, 0, sizeof(p->frame));
}

bool PROTO_FrameParseByte(proto_parser_t *p, uint8_t byte, proto_cmd_frame_t *out_frame)
{
    if ((p == NULL) || (out_frame == NULL))
    {
        return false;
    }

    switch (p->state)
    {
        case PROTO_PARSE_WAIT_SOF:
        {
            if (byte == PROTO_CMD_SOF)
            {
                p->state = PROTO_PARSE_CMD;
            }
        }
        break;

        case PROTO_PARSE_CMD:
        {
            p->frame.cmd = byte;
            p->state = PROTO_PARSE_LEN;
        }
        break;

        case PROTO_PARSE_LEN:
        {
            p->frame.len = byte;
            p->payload_pos = 0U;
            if (p->frame.len == 0U)
            {
                p->state = PROTO_PARSE_CRC_LO;
            }
            else
            {
                p->state = PROTO_PARSE_PAYLOAD;
            }
        }
        break;

        case PROTO_PARSE_PAYLOAD:
        {
            p->frame.payload[p->payload_pos] = byte;
            p->payload_pos++;
            if (p->payload_pos >= p->frame.len)
            {
                p->state = PROTO_PARSE_CRC_LO;
            }
        }
        break;

        case PROTO_PARSE_CRC_LO:
        {
            p->crc_rx = (uint16_t)byte;
            p->state = PROTO_PARSE_CRC_HI;
        }
        break;

        case PROTO_PARSE_CRC_HI:
        {
            p->crc_rx |= (uint16_t)((uint16_t)byte << 8);

            /* Debug commands (0xE0-0xEF) bypass CRC check for quick testing */
            const bool is_debug_cmd = ((p->frame.cmd & 0xF0U) == 0xE0U);

            if (is_debug_cmd)
            {
                *out_frame = p->frame;
                proto_reset(p);
                return true;
            }

            uint8_t crc_buf[2 + 255];
            crc_buf[0] = p->frame.cmd;
            crc_buf[1] = p->frame.len;
            if (p->frame.len != 0U)
            {
                (void)memcpy(&crc_buf[2], p->frame.payload, p->frame.len);
            }

            const uint16_t crc_calc = proto_crc16_calc(crc_buf, (size_t)2U + (size_t)p->frame.len);
            if (crc_calc == p->crc_rx)
            {
                *out_frame = p->frame;
                proto_reset(p);
                return true;
            }

            proto_reset(p);
        }
        break;

        default:
            proto_reset(p);
            break;
    }

    return false;
}

bool PROTO_FrameBuildAck(uint8_t req_cmd, proto_status_t status, uint8_t *out, size_t out_size, size_t *out_len)
{
    if ((out == NULL) || (out_len == NULL))
    {
        return false;
    }

    const uint8_t cmd = (uint8_t)(req_cmd + PROTO_CMD_ACK_OFFSET);
    const uint8_t len = 1U;

    uint8_t buf[1 + 1 + 1 + 2];
    buf[0] = PROTO_CMD_SOF;
    buf[1] = cmd;
    buf[2] = len;

    uint8_t crc_in[2 + 1];
    crc_in[0] = cmd;
    crc_in[1] = len;
    crc_in[2] = status;
    const uint16_t crc = proto_crc16_calc(crc_in, sizeof(crc_in));

    if (out_size < (size_t)6U)
    {
        return false;
    }

    out[0] = buf[0];
    out[1] = buf[1];
    out[2] = buf[2];
    out[3] = status;
    out[4] = (uint8_t)(crc & 0xFFU);
    out[5] = (uint8_t)((crc >> 8) & 0xFFU);
    *out_len = 6U;
    return true;
}

bool PROTO_DataBuildFrame(uint64_t task_id,
                          uint32_t offset,
                          uint32_t total_bytes,
                          const uint8_t *data,
                          uint16_t data_len,
                          uint8_t *out,
                          size_t out_size,
                          size_t *out_len)
{
    if ((out == NULL) || (out_len == NULL))
    {
        return false;
    }

    if (((data == NULL) && (data_len != 0U)))
    {
        return false;
    }

    const uint16_t payload_len = (uint16_t)(8U + 4U + 4U + data_len);
    const size_t frame_len = (size_t)1U + (size_t)2U + (size_t)payload_len + (size_t)2U;
    if (out_size < frame_len)
    {
        return false;
    }

    size_t pos = 0U;
    out[pos++] = PROTO_DATA_SOF;
    out[pos++] = (uint8_t)(payload_len & 0xFFU);
    out[pos++] = (uint8_t)((payload_len >> 8) & 0xFFU);

    for (uint8_t i = 0U; i < 8U; i++)
    {
        out[pos++] = (uint8_t)((task_id >> (8U * i)) & 0xFFU);
    }

    out[pos++] = (uint8_t)(offset & 0xFFU);
    out[pos++] = (uint8_t)((offset >> 8) & 0xFFU);
    out[pos++] = (uint8_t)((offset >> 16) & 0xFFU);
    out[pos++] = (uint8_t)((offset >> 24) & 0xFFU);

    out[pos++] = (uint8_t)(total_bytes & 0xFFU);
    out[pos++] = (uint8_t)((total_bytes >> 8) & 0xFFU);
    out[pos++] = (uint8_t)((total_bytes >> 16) & 0xFFU);
    out[pos++] = (uint8_t)((total_bytes >> 24) & 0xFFU);

    if (data_len != 0U)
    {
        (void)memcpy(&out[pos], data, data_len);
        pos += (size_t)data_len;
    }

    const uint16_t crc = proto_crc16_calc(&out[1], (size_t)2U + (size_t)payload_len);
    out[pos++] = (uint8_t)(crc & 0xFFU);
    out[pos++] = (uint8_t)((crc >> 8) & 0xFFU);

    *out_len = pos;
    return true;
}

bool PROTO_StatusBuildFrame(uint64_t task_id,
                            uint32_t event_bits,
                            uint8_t erase_progress,
                            uint8_t *out,
                            size_t out_size,
                            size_t *out_len)
{
    if ((out == NULL) || (out_len == NULL))
    {
        return false;
    }

    const uint16_t payload_len = (uint16_t)(8U + 4U + 1U + 1U);
    const size_t frame_len = (size_t)1U + (size_t)2U + (size_t)payload_len + (size_t)2U;
    if (out_size < frame_len)
    {
        return false;
    }

    size_t pos = 0U;
    out[pos++] = PROTO_STATUS_SOF;
    out[pos++] = (uint8_t)(payload_len & 0xFFU);
    out[pos++] = (uint8_t)((payload_len >> 8) & 0xFFU);

    for (uint8_t i = 0U; i < 8U; i++)
    {
        out[pos++] = (uint8_t)((task_id >> (8U * i)) & 0xFFU);
    }

    out[pos++] = (uint8_t)(event_bits & 0xFFU);
    out[pos++] = (uint8_t)((event_bits >> 8) & 0xFFU);
    out[pos++] = (uint8_t)((event_bits >> 16) & 0xFFU);
    out[pos++] = (uint8_t)((event_bits >> 24) & 0xFFU);

    out[pos++] = erase_progress;
    out[pos++] = 0U;

    const uint16_t crc = proto_crc16_calc(&out[1], (size_t)2U + (size_t)payload_len);
    out[pos++] = (uint8_t)(crc & 0xFFU);
    out[pos++] = (uint8_t)((crc >> 8) & 0xFFU);

    *out_len = pos;
    return true;
}

