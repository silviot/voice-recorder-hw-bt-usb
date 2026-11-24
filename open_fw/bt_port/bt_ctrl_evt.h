/* bt_ctrl_evt: ROM TASK_HCI message -> H4 event packers (plan M1 "src/bt_ctrl_evt.c (event packers)").
 * Ports the legacy in-place repacking table (bt_hci.c:720-863, BT_ROM_INTERFACE §4.3) to a pure copy-out function
 * that validates param_len for every id (BT_ROM_INTERFACE §5.2). ACL RX (E47D) and NOCP (E462) are handled by
 * bt_ctrl itself because they carry flow-control state.
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_EVT_H
#define BT_CTRL_EVT_H

#include <stdint.h>

enum {
    BT_CTRL_EVK_UNKNOWN = 0,   /* id not in the table: counted and consumed */
    BT_CTRL_EVK_CC,            /* Command Complete for info.opcode */
    BT_CTRL_EVK_CS,            /* Command Status for info.opcode */
    BT_CTRL_EVK_EVT,           /* any other event (info.evt_code) */
    BT_CTRL_EVK_SHORT,         /* known id but param_len below the fields the packer reads: counted, consumed */
};

typedef struct {
    uint8_t  kind;
    uint8_t  evt_code;
    uint8_t  status;           /* first parameter byte (CC/CS status, or event status where the event has one) */
    uint16_t opcode;           /* CC/CS only */
    uint16_t handle;           /* events whose packer de-pads a connection handle */
} bt_ctrl_evt_info_t;

#define BT_CTRL_EVT_MAX_H4  258u   /* 1 + 2 + 255 */

/* Pack ROM message `id` into `out` (>= BT_CTRL_EVT_MAX_H4 bytes). Returns the H4 length (0 for UNKNOWN/SHORT). */
uint16_t bt_ctrl_evt_pack(uint16_t id, const uint8_t *param, uint16_t param_len, uint8_t *out,
                          bt_ctrl_evt_info_t *info);

/* Opcode a ROM reply id completes (CC or CS), 0 if `id` is not a command reply. */
uint16_t bt_ctrl_evt_reply_opcode(uint16_t id);

/* Number Of Completed Packets event from n (handle, count) pairs. Returns the H4 length. */
uint16_t bt_ctrl_evt_pack_nocp(uint8_t *out, uint8_t n, const uint16_t *handles, const uint16_t *counts);

#endif /* BT_CTRL_EVT_H */
