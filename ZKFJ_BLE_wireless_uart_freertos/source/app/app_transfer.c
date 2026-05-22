/*
 * app_transfer.c
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#include "app.transfer.h"

#include <string.h>

#include "algo_crc.h"

size_t APP_Transfer_GetHistoryPacketSize(uint8_t payload_len)
{
    if (payload_len > APP_TRANSFER_PAYLOAD_MAX)
    {
        return 0U;
    }
    return APP_TRANSFER_HEADER_SIZE + (size_t)payload_len + APP_TRANSFER_CRC_SIZE;
}

bool APP_Transfer_BuildHistoryPacket(uint16_t seq,
                                     const uint8_t *payload,
                                     uint8_t payload_len,
                                     uint8_t *out_buf,
                                     size_t out_buf_size,
                                     size_t *out_packet_len)
{
    if ((out_buf == NULL) || (out_packet_len == NULL))
    {
        return false;
    }

    if ((payload_len > APP_TRANSFER_PAYLOAD_MAX) || ((payload == NULL) && (payload_len != 0U)))
    {
        return false;
    }

    const size_t packet_len = APP_Transfer_GetHistoryPacketSize(payload_len);
    if ((packet_len == 0U) || (out_buf_size < packet_len))
    {
        return false;
    }

    if (payload_len != 0U)
    {
        (void)memcpy(&out_buf[APP_TRANSFER_HEADER_SIZE], payload, payload_len);
    }

    out_buf[0] = APP_TRANSFER_MAGIC0;
    out_buf[1] = APP_TRANSFER_MAGIC1;
    out_buf[2] = (uint8_t)(seq & 0xFFU);
    out_buf[3] = (uint8_t)((seq >> 8) & 0xFFU);
    out_buf[4] = payload_len;

    const uint16_t crc16 = algo_crc16_calc(out_buf, APP_TRANSFER_HEADER_SIZE + (size_t)payload_len);
    out_buf[APP_TRANSFER_HEADER_SIZE + (size_t)payload_len]         = (uint8_t)(crc16 & 0xFFU);
    out_buf[APP_TRANSFER_HEADER_SIZE + (size_t)payload_len + 1U]    = (uint8_t)((crc16 >> 8) & 0xFFU);

    *out_packet_len = packet_len;
    return true;
}
