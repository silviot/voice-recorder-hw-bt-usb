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
#include "bt_task_sup.h"

/* SSP_ANSWER(arg 1 = ENTER, 0 = BACK); START_PAIR(arg ms) = bt_xrec_start + bt_xrec_open_pairing in one app-task step
 * (the SD bench run's pair=1 at t=0, before READY); BENCH_AUTOCONFIRM(arg 0/1) = bt_xrec_set_bench_autoconfirm;
 * BENCH_XREC_PIN(arg 0..999999, larger = off) = bt_xrec_set_bench_xrec_pin. */
enum { BTHX_START = 1, BTHX_STOP, BTHX_OPEN_PAIRING, BTHX_SSP_ANSWER, BTHX_START_PAIR, BTHX_BENCH_AUTOCONFIRM,
       BTHX_BENCH_XREC_PIN };

typedef struct {
    bt_status_t st;               /* bt_xrec_get_status() as the UI sees it */
    bt_xrec_debug_t d;            /* bt_xrec_get_debug() */
    bt_xrec_ui_t ui;              /* bt_xrec_get_ui(): the BT screen's pairing prompt / link state */
    int bonds;                    /* esp_bt_gap_get_bond_device_num() while enabled, else -1 */
    uint32_t polls;
} bthx_snap_t;

/* Create the app task, the worker and the deadline timer (once; before the heap/task baseline). vol = the mounted
 * RAM exFAT volume the session uses as its SD card. */
int  bthx_init(exfat_vol_t *vol);
/* Run one bt_xrec_* call in the app task and wait for it (BTHX_START / STOP / OPEN_PAIRING(arg ms) / SSP_ANSWER). */
int  bthx_call(int op, uint32_t arg, uint32_t timeout_ms);
/* The same without blocking the caller (the scenario keeps the fake laptop answering while a stop runs). */
uint32_t bthx_post(int op, uint32_t arg);
int  bthx_done(uint32_t seq, int *result);
void bthx_snap(bthx_snap_t *out);
uint32_t bthx_hard_path_reason(void);    /* bt_xrec_plat_hard_path() calls (reason of the first; 0 = never) */
uint32_t bthx_deadline_fired(void);
uint32_t bthx_bond_writes(void);         /* /bt_bonds.bin replaced */
void bthx_fail_bond_writes(int on);       /* every /bt_bonds.bin write fails while on (full / removed card) */
uint32_t bthx_bond_write_fails(void);
void bthx_fail_bond_reads(int on);        /* reading the mode's bond file at a start fails (SD error / no memory) */
uint32_t bthx_key_writes(void);          /* /cassini.key replaced */
uint32_t bthx_reboot_mode(void);
/* TEST knob: the session's entropy as after a BT-mode entry whose mic collection credited nothing (XREC PIN / CHAL fail
 * closed, M2.3 gate refuses). Read at stack_up_begin for the ROM key model, and live for everything else. */
void bthx_withhold_entropy(int on);
/* TEST knob: the ROM RNG hook served a security site from the legacy generator (bt_rom_rng `starved`++): latched until
 * the next start; the M2.3 gate and bt_xrec_plat_rng_starved() see it at once (STEP10 C3). */
void bthx_rng_starve(void);
/* TEST knob: the next up job waits while on (the worker outranks the app task, and the fake ROM answers at once, so
 * without it the bring-up always finishes before the app task runs again; on target the ROM bring-up does not). */
void bthx_hold_bringup(int on);
/* TEST knob: while on, the next up job ends with BTC_TASK suspended, so the session's esp_spp_enhanced_init() stays
 * queued (ST_CONFIG, SPP INIT posted, spp_inited still false). Off resumes BTC_TASK; returns 1 if it was suspended. */
int  bthx_hold_btc(int on);
int  bthx_btc_held(void);
uint32_t bthx_down_spp_checked(void);    /* down jobs that have made the SPP-deinit decision */
uint32_t bthx_down_spp_deinit(void);     /* ... of which decided to call esp_spp_deinit() */
uint32_t bthx_ssp_value_logged(void);    /* value of the last "numeric comparison accepted" log line */
/* Read a root file of the RAM SD card (test task, only while the session is OFF or idle). Returns bytes or -1. */
int  bthx_read_root_file(const char *name, uint8_t *buf, uint32_t cap);

/* Headphones scenario: run fn(arg) in the app task (as main.c would call bt_hp_*), blocking / not blocking. */
int  bthx_call_fn(int (*fn)(uint32_t), uint32_t arg, uint32_t timeout_ms);
uint32_t bthx_post_fn(int (*fn)(uint32_t), uint32_t arg);
/* Called on every app-task loop after bt_xrec_poll() (NULL = none). */
void bthx_set_app_hook(void (*hook)(void));
uint32_t bthx_profile_down_rc(void);     /* the last down job's profile teardown result (0xFFFFFFFF = none ran) */
exfat_vol_t *bthx_card(void);
/* bt_xrec_plat_pairing_block() calls made inside bt_xrec_plat_lock() (must stay 0: a critical section on target). */
uint32_t bthx_gate_in_lock(void);

/* Bluedroid task supervisor (bt_port/bt_task_sup.h) as the firmware runs it: shorter limits for the scenario (0 keeps),
 * its counters, the WDT-feed hold, platform faults it raised. */
void bthx_sup_limits(uint32_t period_ms, uint32_t stall_ms, uint32_t hold_ms);
void bthx_sup_stats(bt_task_sup_stats_t *out);
int  bthx_sup_hold(void);
uint32_t bthx_sup_faults(void);
/* TEST knob: on = post a work item to that Bluedroid thread (BT_TASK_SUP_*) that keeps it busy until off (the stuck
 * task the supervisor exists for). Returns -1 if the post failed. bthx_block_running: the item is executing. */
int  bthx_block_thread(unsigned idx, int on);
int  bthx_block_running(unsigned idx);

#endif /* BT_HOST_XREC_H */
