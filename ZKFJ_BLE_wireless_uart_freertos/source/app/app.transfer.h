/*
 * app.transfer.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef APP_APP_TRANSFER_H_
#define APP_APP_TRANSFER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define APP_TRANSFER_PACKED __attribute__((packed))
#else
#define APP_TRANSFER_PACKED
#endif

#define APP_TRANSFER_MAGIC0             ((uint8_t)0x5AU)
#define APP_TRANSFER_MAGIC1             ((uint8_t)0xA5U)
#define APP_TRANSFER_PAYLOAD_MAX        ((uint8_t)230U)

#define APP_TRANSFER_HEADER_SIZE        ((size_t)5U)
#define APP_TRANSFER_CRC_SIZE           ((size_t)2U)
#define APP_TRANSFER_PACKET_MAX_SIZE    (APP_TRANSFER_HEADER_SIZE + (size_t)APP_TRANSFER_PAYLOAD_MAX + APP_TRANSFER_CRC_SIZE)

size_t APP_Transfer_GetHistoryPacketSize(uint8_t payload_len);

bool APP_Transfer_BuildHistoryPacket(uint16_t seq,
                                     const uint8_t *payload,
                                     uint8_t payload_len,
                                     uint8_t *out_buf,
                                     size_t out_buf_size,
                                     size_t *out_packet_len);

#ifdef __cplusplus
}
#endif
#endif /* APP_APP_TRANSFER_H_ */
