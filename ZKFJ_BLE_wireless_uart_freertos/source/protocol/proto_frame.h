/*
 * proto_frame.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef PROTOCOL_PROTO_FRAME_H_
#define PROTOCOL_PROTO_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_CMD_SOF               ((uint8_t)0xA5U)
#define PROTO_DATA_SOF              ((uint8_t)0x5AU)
#define PROTO_STATUS_SOF            ((uint8_t)0x5BU)

#define PROTO_CMD_ACK_OFFSET        ((uint8_t)0x80U)

#define PROTO_CMD_SELFTEST          ((uint8_t)0x01U)
#define PROTO_CMD_START_COLLECT     ((uint8_t)0x02U)
#define PROTO_CMD_STOP_COLLECT      ((uint8_t)0x03U)
#define PROTO_CMD_REQUEST_HISTORY   ((uint8_t)0x04U)
#define PROTO_CMD_CLEAR_TASK        ((uint8_t)0x05U)

/* Debug commands: no CRC required, no response framing needed.
 * Usage: send raw bytes A5 <cmd> <len> [payload] <crc_lo> <crc_hi>
 *   - cmd=0xE0: list all task_ids in flash
 *   - cmd=0xE1: query specific task (payload = 8B task_id LE)
 *   - cmd=0xE2: query record count (payload = 8B task_id LE) */
#define PROTO_CMD_DEBUG_LIST        ((uint8_t)0xE0U)
#define PROTO_CMD_DEBUG_QUERY       ((uint8_t)0xE1U)
#define PROTO_CMD_DEBUG_RECCNT      ((uint8_t)0xE2U)

/* BLE MTU=247, max data per DATA frame = 247 - 21 = 226, but round down to 220 for safety margin */
#ifndef PROTO_MAX_CHUNK_SIZE
#define PROTO_MAX_CHUNK_SIZE        220U
#endif

typedef uint8_t proto_status_t;

#define PROTO_STATUS_OK             ((proto_status_t)0x00U)
#define PROTO_STATUS_CRC_ERROR      ((proto_status_t)0x01U)
#define PROTO_STATUS_LEN_ERROR      ((proto_status_t)0x02U)
#define PROTO_STATUS_CMD_UNSUPPORTED ((proto_status_t)0x03U)
#define PROTO_STATUS_PARAM_ERROR    ((proto_status_t)0x04U)
#define PROTO_STATUS_BUSY           ((proto_status_t)0x05U)
#define PROTO_STATUS_NOT_FOUND      ((proto_status_t)0x06U)
#define PROTO_STATUS_STORAGE_ERROR  ((proto_status_t)0x07U)

typedef struct
{
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[255];
} proto_cmd_frame_t;

typedef enum
{
    PROTO_PARSE_WAIT_SOF = 0,
    PROTO_PARSE_CMD,
    PROTO_PARSE_LEN,
    PROTO_PARSE_PAYLOAD,
    PROTO_PARSE_CRC_LO,
    PROTO_PARSE_CRC_HI,
} proto_parse_state_t;

typedef struct
{
    proto_parse_state_t state;
    proto_cmd_frame_t frame;
    uint8_t payload_pos;
    uint16_t crc_rx;
} proto_parser_t;

void PROTO_FrameParserInit(proto_parser_t *p);
bool PROTO_FrameParseByte(proto_parser_t *p, uint8_t byte, proto_cmd_frame_t *out_frame);
bool PROTO_FrameBuildAck(uint8_t req_cmd, proto_status_t status, uint8_t *out, size_t out_size, size_t *out_len);

bool PROTO_DataBuildFrame(uint64_t task_id,
                          uint32_t offset,
                          uint32_t total_bytes,
                          const uint8_t *data,
                          uint16_t data_len,
                          uint8_t *out,
                          size_t out_size,
                          size_t *out_len);

bool PROTO_StatusBuildFrame(uint64_t task_id,
                            uint32_t event_bits,
                            uint8_t erase_progress,
                            uint8_t *out,
                            size_t out_size,
                            size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_PROTO_FRAME_H_ */
