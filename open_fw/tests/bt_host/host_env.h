/* Host harness environment for Bluedroid + bt_port + bt_ctrl on the FreeRTOS POSIX port (make test-bt-host,
 * plan docs/BLUETOOTH_BLUEDROID_PLAN.md M3 path (b) "through M1"). Shared by host_heap.c, host_glue.c, host_ctrl.c and
 * test_bt_host.c. Nothing here is built into the firmware.
 * SPDX-License-Identifier: MIT */
#ifndef BT_HOST_ENV_H
#define BT_HOST_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bt_ctrl.h"

/* ---- heap (host_heap.c): every malloc/calloc/realloc/free of the harness objects (linked with --wrap), every
 * pvPortMalloc and every heap_caps_* goes through one tracked allocator. ---- */
typedef struct {
    size_t live_bytes, live_blocks, peak_bytes;
    uint64_t allocs;            /* successful allocations since start */
    uint64_t armed_seen;        /* allocation attempts seen since bth_heap_fail_arm() */
    uint64_t injected;          /* NULL returns caused by injection */
    uint64_t rtos_failed_hook;  /* vApplicationMallocFailedHook calls */
    void *last_injected_ra;     /* return address of the call that got the injected NULL */
} bth_heap_stats_t;

void bth_heap_stats(bth_heap_stats_t *out);
void bth_heap_peak_reset(void);   /* peak_bytes = live_bytes (scenario-local peaks) */
/* Fail the n-th allocation attempt (1-based) from now on; n = 0 disarms. persistent: fail every attempt >= n. */
void bth_heap_fail_arm(uint64_t n, bool persistent);
void bth_heap_fail_disarm(void);
/* Every allocation of exactly `size` bytes made by the FreeRTOS task named `task` fails until called with NULL (e.g.
 * BTU_TASK's BT_HDR-sized messages: patch 0039's local close, STEP10 A6). Counts what it failed. */
void bth_heap_fail_task_size(const char *task, size_t size);
uint64_t bth_heap_task_size_injected(void);
/* One-shot: the next allocation attempt of exactly n bytes fails (n = 0 disarms); pending = still armed. */
void bth_heap_fail_size(size_t n);
size_t bth_heap_fail_size_pending(void);
/* Allocation attempts counted since the last arm (also counts while armed with n = 0 via bth_heap_count_start). */
void bth_heap_count_start(void);
uint64_t bth_heap_counted(void);
uint32_t bth_heap_seq(void);                /* allocation sequence number (live blocks carry theirs) */
void bth_heap_dump_since(uint32_t seq);     /* stderr: live blocks allocated after seq (BTH_HEAP_TRACE=1: stacks) */

/* ---- BTC context (host_glue.c) ---- */
/* Run fn(arg) on BTC_TASK (a BTC_PID_ALARM work item, the way Bluedroid runs its own alarms) and wait for it, at most
 * timeout_ms. 0 = it ran. */
int bth_btc_run(void (*fn)(void *), void *arg, uint32_t timeout_ms);
void *bth_a2dp_app_cb(void);                /* the callback given to esp_a2d_register_callback(), or NULL */
/* ---- targeted out-of-memory levers (host_oom.c; patch 0044, review STEP10 A9 / C7 / C11) ---- */
enum {
    BTH_OOM_SPP_WRITE_CMPL = 0,   /* BTA -> BTC hand-off of BTA_JV_RFCOMM_WRITE_EVT */
    BTH_OOM_SPP_DATA_IND,         /* BTA -> BTC hand-off of BTA_JV_RFCOMM_DATA_IND_EVT (bta_co_rfc_data_incoming) */
    BTH_OOM_SPP_DISCONNECT,       /* esp_spp_disconnect()'s BTC post (the API returns ESP_FAIL) */
    BTH_OOM_BTU_ENQUEUE,          /* fixed_queue_enqueue() on BTU_TASK (list node) */
    BTH_OOM_BOND_STORE,           /* btc_storage_add_bonded_device() */
    BTH_OOM_BTC_FREE_SCN_UD,      /* an allocation of sizeof(tBTA_JV_FREE_SCN_USER_DATA) on BTC_TASK (SPP deinit) */
    BTH_OOM_N
};
void bth_oom_arm(int what, int n);      /* the next n matching calls fail; 0 disarms */
uint32_t bth_oom_hits(int what);        /* failures injected so far */
bool bth_oom_take_alloc(size_t n);      /* host_heap.c, under its lock: fail this allocation (BTH_OOM_BTC_FREE_SCN_UD) */

/* ---- logging: serialised (FreeRTOS critical section), safe from any task ---- */
void bth_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- port glue (host_glue.c) ---- */
int bth_port_init(void);                    /* entropy (TEST ONLY source), store, bt_alarm task, esp_timer shim */
uint32_t bth_timers_in_use(void);           /* esp_timer slots held by Bluedroid (osi alarms) */
uint32_t bth_hci_cmd_timeouts(void);        /* patch 0007 reports (bt_port_on_hci_command_timeout) */
uint32_t bth_store_flush_requests(void);
void bth_alarm_arm_drain_exit_hook(void (*fn)(void)); /* fires once right after bt_alarm_drain pops an item */

/* ---- controller side (host_ctrl.c): bt_ctrl tasks over tests/fake_rom.c ---- */
int  bth_ctrl_init(void);                   /* bt_ctrl_init + bt_ctrl / bt_dlv tasks; once */
int  bth_ctrl_open(void);                   /* bt_ctrl_open + esp_bluedroid_attach_hci_driver */
int  bth_ctrl_close(uint32_t timeout_ms);   /* bt_ctrl_shutdown_begin, wait for the stop status and an idle bt_dlv */
bool bth_forget_dm_peer(void);  /* model a missing BTA DM entry for a live ACL */
void bth_ctrl_kick(void);                   /* wake the bt_ctrl task (after fake_rom_inject*) */
void bth_rom_lock(void);                    /* serialises fake_rom_* calls with the bt_ctrl task */
void bth_rom_unlock(void);
uint32_t bth_ctrl_faults(void);
uint32_t bth_ctrl_first_fault(void);
uint32_t bth_dlv_retries(void);

/* ---- watchdog (host_main.c): every blocking step runs under a deadline enforced by a non-FreeRTOS thread ---- */
void bth_step(const char *what, uint32_t timeout_ms);
void bth_step_done(void);

#endif /* BT_HOST_ENV_H */
