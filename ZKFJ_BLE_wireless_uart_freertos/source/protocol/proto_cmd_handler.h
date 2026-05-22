/*
 * proto_cmd_handler.h
 *
 *  Created on: 2026年5月22日
 *      Author: elois
 */

#ifndef PROTOCOL_PROTO_CMD_HANDLER_H_
#define PROTOCOL_PROTO_CMD_HANDLER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*proto_tx_fn_t)(const uint8_t *data, size_t len, void *user);

void PROTO_CmdInit(void);
void PROTO_CmdSetTx(proto_tx_fn_t fn, void *user);

void PROTO_OnRxBytes(const uint8_t *data, size_t len);

void PROTO_CmdTask(void *pvParameters);
void PROTO_UartRxTask(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_PROTO_CMD_HANDLER_H_ */
