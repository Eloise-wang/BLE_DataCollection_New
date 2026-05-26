/*
 * app_can.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef APP_APP_CAN_H_
#define APP_APP_CAN_H_

#include <stdbool.h>
#include <stdint.h>

#include "algo_can_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t app_can_diag_t;

#define APP_CAN_DIAG_OK               ((app_can_diag_t)0x0000U)
#define APP_CAN_DIAG_OFFLINE          ((app_can_diag_t)0x0001U)
#define APP_CAN_DIAG_DECODE_FAIL      ((app_can_diag_t)0x0002U)
#define APP_CAN_DIAG_CAPTURE_TIMEOUT  ((app_can_diag_t)0x0004U)
#define APP_CAN_DIAG_RX_DROPPED       ((app_can_diag_t)0x0008U)

#define APP_CAN_OFFLINE_TIMEOUT_MS        9000U
#define APP_CAN_CAPTURE_DEFAULT_TIMEOUT_MS 1200U

typedef enum
{
    APP_CAN_CAPTURE_IDLE = 0,
    APP_CAN_CAPTURE_WAIT_FRESH,
    APP_CAN_CAPTURE_DONE,
} app_can_capture_state_t;

typedef struct
{
    uint32_t request_timestamp_ms;
    uint32_t rx_timestamp_ms;
    uint32_t rx_age_ms;
    algo_can_gcu1_t gcu1;
    app_can_diag_t diag;
} app_can_capture_t;

void APP_CAN_Init(void);
void APP_CAN_Process(void);

void APP_CAN_StartCapture(uint32_t request_timestamp_ms, uint32_t timeout_ms);
bool APP_CAN_IsCaptureDone(void);
bool APP_CAN_GetCapture(app_can_capture_t *out);

bool APP_CAN_GetLatest(algo_can_gcu1_t *out, uint32_t *out_rx_timestamp_ms, uint32_t *out_rx_age_ms, app_can_diag_t *out_diag);

#ifdef __cplusplus
}
#endif

#endif /* APP_APP_CAN_H_ */
