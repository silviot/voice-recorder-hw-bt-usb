/* Public entry of the Bluedroid port for the app (plan M2). SPDX-License-Identifier: MIT */
#ifndef BT_PORT_H
#define BT_PORT_H
#include <stdint.h>

/* Called (non-blocking, from the committing BT task) when the bond store became dirty; the app serialises it with
 * bt_store_serialize() and writes it with the exFAT staged replace in its own context. */
typedef void (*bt_store_flush_fn_t)(void);

/* App task, before esp_bluedroid_init(): entropy, store, alarm service + bt_alarm task. Idempotent. 0 = OK. */
int bt_port_init(bt_store_flush_fn_t request_store_flush);

uint32_t bt_port_hci_command_timeouts(void);
#endif
