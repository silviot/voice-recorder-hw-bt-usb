/* Host platform for the BT=1 XREC session (bt_port/bt_xrec_bluedroid.c) in make test-bt-host: the app task that owns
 * the session, the bt_recover worker (bring-up / teardown jobs over bt_ctrl + Bluedroid), the teardown deadline timer,
 * and a RAM-backed exFAT "SD card" holding /record, /bt_bonds.bin and /cassini.key. Same contract as the firmware
 * platform bt_port/bt_xrec_fw.c; see host_xrec.c for the differences.
 * SPDX-License-Identifier: MIT */
#ifndef BT_HOST_XREC_H
#define BT_HOST_XREC_H

#include <stdint.h>
#include "openfw.h"
#include "bt_xrec_bluedroid.h"

enum { BTHX_START = 1, BTHX_STOP, BTHX_OPEN_PAIRING };

typedef struct {
    bt_status_t st;               /* bt_xrec_get_status() as the UI sees it */
    bt_xrec_debug_t d;            /* bt_xrec_get_debug() */
    int bonds;                    /* esp_bt_gap_get_bond_device_num() while enabled, else -1 */
    uint32_t polls;
} bthx_snap_t;

/* Create the app task, the worker and the deadline timer (once; before the heap/task baseline). vol = the mounted
 * RAM exFAT volume the session uses as its SD card. */
int  bthx_init(exfat_vol_t *vol);
/* Run one bt_xrec_* call in the app task and wait for it (BTHX_START / STOP / OPEN_PAIRING(arg ms)). */
int  bthx_call(int op, uint32_t arg, uint32_t timeout_ms);
/* The same without blocking the caller (the scenario keeps the fake laptop answering while a stop runs). */
uint32_t bthx_post(int op, uint32_t arg);
int  bthx_done(uint32_t seq, int *result);
void bthx_snap(bthx_snap_t *out);
uint32_t bthx_hard_path_reason(void);    /* bt_xrec_plat_hard_path() calls (reason of the first; 0 = never) */
uint32_t bthx_deadline_fired(void);
uint32_t bthx_bond_writes(void);         /* /bt_bonds.bin replaced */
uint32_t bthx_key_writes(void);          /* /cassini.key replaced */
uint32_t bthx_reboot_mode(void);
uint32_t bthx_ssp_value_logged(void);    /* value of the last "numeric comparison accepted" log line */
/* Read a root file of the RAM SD card (test task, only while the session is OFF or idle). Returns bytes or -1. */
int  bthx_read_root_file(const char *name, uint8_t *buf, uint32_t cap);

#endif /* BT_HOST_XREC_H */
