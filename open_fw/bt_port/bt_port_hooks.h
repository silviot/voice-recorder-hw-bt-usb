/* Hooks called from the documented Bluedroid patches 0005-0007 (third_party/bluedroid_patches/README.md).
 * Implemented in bt_port (firmware: bt_port_glue.c; host harness: its own glue). SPDX-License-Identifier: MIT */
#ifndef BT_PORT_HOOKS_H
#define BT_PORT_HOOKS_H
#include <stdint.h>

/* BTC context: deliver pending alarm expiries (bt_alarm_drain). Called after every BTC message (patch 0006). */
void bt_port_alarm_drain(void);
/* Generation of the alarm expiry being delivered right now (bt_alarm_delivery_gen). */
uint32_t bt_port_alarm_delivery_gen(void);
/* hciT/BTC: a command-response timeout that survived re-validation (patch 0007). Posts a BT fault to the app. */
void bt_port_on_hci_command_timeout(uint16_t opcode);
#endif
