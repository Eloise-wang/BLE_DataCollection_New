/*
 * app_can.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "app_can.h"

#include <string.h>

#include "bsp_can.h"
#include "fsl_os_abstraction.h"

typedef struct
{
    app_can_capture_state_t state;
    uint32_t request_timestamp_ms;
    uint32_t timeout_ms;
    app_can_capture_t result;
    uint32_t start_rx_timestamp_ms;
    uint32_t last_rx_seq;
} app_can_capture_ctx_t;

static struct
{
    algo_can_gcu1_t latest;
    uint32_t latest_rx_timestamp_ms;
    uint32_t rx_seq;
    app_can_diag_t sticky_diag;
    uint32_t last_dropped;
    app_can_capture_ctx_t capture;
} s_ctx;

static void app_can_update_dropped(void)
{
    const uint32_t dropped = BSP_CAN_GetDroppedRxCount();
    if (dropped != s_ctx.last_dropped)
    {
        s_ctx.sticky_diag |= APP_CAN_DIAG_RX_DROPPED;
        s_ctx.last_dropped = dropped;
    }
}

static void app_can_on_frame_decoded(uint32_t rx_timestamp_ms, const algo_can_gcu1_t *decoded)
{
    s_ctx.latest = *decoded;
    s_ctx.latest_rx_timestamp_ms = rx_timestamp_ms;
    s_ctx.rx_seq++;

    if (s_ctx.capture.state == APP_CAN_CAPTURE_WAIT_FRESH)
    {
        if ((rx_timestamp_ms >= s_ctx.capture.request_timestamp_ms) && (s_ctx.capture.last_rx_seq != s_ctx.rx_seq))
        {
            (void)memset(&s_ctx.capture.result, 0, sizeof(s_ctx.capture.result));
            s_ctx.capture.result.request_timestamp_ms = s_ctx.capture.request_timestamp_ms;
            s_ctx.capture.result.rx_timestamp_ms      = rx_timestamp_ms;
            s_ctx.capture.result.rx_age_ms            = rx_timestamp_ms - s_ctx.capture.request_timestamp_ms;
            s_ctx.capture.result.gcu1                 = *decoded;
            s_ctx.capture.result.diag                 = APP_CAN_DIAG_OK;
            s_ctx.capture.state                       = APP_CAN_CAPTURE_DONE;
        }
    }
}

void APP_CAN_Init(void)
{
    (void)memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.capture.state = APP_CAN_CAPTURE_IDLE;
    BSP_CAN_Init();
}

void APP_CAN_Process(void)
{
    bsp_can_frame_t frame;
    const uint32_t now_ms = OSA_TimeGetMsec();

    app_can_update_dropped();

    while (BSP_CAN_TryReadFrame(&frame))
    {
        if ((frame.id == BSP_CAN_FILTER_ID0) && (frame.dlc == ALGO_CAN_DLC) && frame.is_extended && (!frame.is_remote))
        {
            algo_can_gcu1_t decoded;
            if (algo_can_decode_gcu1(frame.data, frame.dlc, &decoded))
            {
                app_can_on_frame_decoded(now_ms, &decoded);
            }
            else
            {
                s_ctx.sticky_diag |= APP_CAN_DIAG_DECODE_FAIL;
            }
        }
        else
        {
            s_ctx.sticky_diag |= APP_CAN_DIAG_DECODE_FAIL;
        }
    }

    if (s_ctx.capture.state == APP_CAN_CAPTURE_WAIT_FRESH)
    {
        if ((now_ms - s_ctx.capture.request_timestamp_ms) >= s_ctx.capture.timeout_ms)
        {
            (void)memset(&s_ctx.capture.result, 0, sizeof(s_ctx.capture.result));
            s_ctx.capture.result.request_timestamp_ms = s_ctx.capture.request_timestamp_ms;
            s_ctx.capture.result.rx_timestamp_ms      = s_ctx.latest_rx_timestamp_ms;
            s_ctx.capture.result.rx_age_ms            = (s_ctx.latest_rx_timestamp_ms == 0U) ? 0U : (now_ms - s_ctx.latest_rx_timestamp_ms);
            s_ctx.capture.result.gcu1                 = s_ctx.latest;
            s_ctx.capture.result.diag                 = (app_can_diag_t)(APP_CAN_DIAG_CAPTURE_TIMEOUT | s_ctx.sticky_diag);
            s_ctx.capture.state                       = APP_CAN_CAPTURE_DONE;
        }
    }
}

void APP_CAN_StartCapture(uint32_t request_timestamp_ms, uint32_t timeout_ms)
{
    const uint32_t now_ms = OSA_TimeGetMsec();
    const uint32_t tmo    = (timeout_ms == 0U) ? APP_CAN_CAPTURE_DEFAULT_TIMEOUT_MS : timeout_ms;

    s_ctx.sticky_diag = APP_CAN_DIAG_OK;
    s_ctx.last_dropped = BSP_CAN_GetDroppedRxCount();

    s_ctx.capture.state                = APP_CAN_CAPTURE_WAIT_FRESH;
    s_ctx.capture.request_timestamp_ms = request_timestamp_ms;
    s_ctx.capture.timeout_ms           = tmo;
    s_ctx.capture.last_rx_seq          = s_ctx.rx_seq;
    s_ctx.capture.start_rx_timestamp_ms = s_ctx.latest_rx_timestamp_ms;

    (void)memset(&s_ctx.capture.result, 0, sizeof(s_ctx.capture.result));
}

bool APP_CAN_IsCaptureDone(void)
{
    return (s_ctx.capture.state == APP_CAN_CAPTURE_DONE);
}

bool APP_CAN_GetCapture(app_can_capture_t *out)
{
    if ((out == NULL) || (s_ctx.capture.state != APP_CAN_CAPTURE_DONE))
    {
        return false;
    }

    *out = s_ctx.capture.result;
    s_ctx.capture.state = APP_CAN_CAPTURE_IDLE;
    s_ctx.sticky_diag = APP_CAN_DIAG_OK;
    return true;
}

bool APP_CAN_GetLatest(algo_can_gcu1_t *out,
                       uint32_t *out_rx_timestamp_ms,
                       uint32_t *out_rx_age_ms,
                       app_can_diag_t *out_diag)
{
    const uint32_t now_ms = OSA_TimeGetMsec();
    app_can_diag_t diag   = s_ctx.sticky_diag;

    if (s_ctx.latest_rx_timestamp_ms == 0U)
    {
        diag |= APP_CAN_DIAG_OFFLINE;
    }
    else if ((now_ms - s_ctx.latest_rx_timestamp_ms) > APP_CAN_OFFLINE_TIMEOUT_MS)
    {
        diag |= APP_CAN_DIAG_OFFLINE;
    }

    if (out != NULL)
    {
        *out = s_ctx.latest;
    }
    if (out_rx_timestamp_ms != NULL)
    {
        *out_rx_timestamp_ms = s_ctx.latest_rx_timestamp_ms;
    }
    if (out_rx_age_ms != NULL)
    {
        *out_rx_age_ms = (s_ctx.latest_rx_timestamp_ms == 0U) ? 0U : (now_ms - s_ctx.latest_rx_timestamp_ms);
    }
    if (out_diag != NULL)
    {
        *out_diag = diag;
    }

    return (diag == APP_CAN_DIAG_OK);
}
