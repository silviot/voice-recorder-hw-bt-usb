/* bt_ctrl_rtos: FreeRTOS bindings for bt_ctrl (plan §3 / M1): the bt_ctrl task (OPENFW_PRIO_BT_CTRL), the bt_dlv
 * task (OPENFW_PRIO_BT_DLV), both static in .rtos_static, and the bt_ctrl_platform_t callbacks (critical sections,
 * task notifications, IRQ-26 wake from the top half, bt_stop latch, fault post).
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_RTOS_H
#define BT_CTRL_RTOS_H

#include <stdint.h>
#include "bt_ctrl.h"

typedef struct {
    uint32_t ctrl_runs;          /* bt_ctrl_run() calls */
    uint32_t ctrl_run_max_us;    /* longest bt_ctrl_run() since bt_ctrl_rtos_reset_max() (window maximum) */
    uint32_t ctrl_run_max_all_us; /* longest bt_ctrl_run() ever, incl. HCI Reset (RF calibration + ROM start) */
    uint32_t ctrl_run_total_us;  /* sum over ctrl_runs (CPU time of the bt_ctrl task body) */
    uint32_t ctrl_more;          /* bt_ctrl_run() returned "more work" */
    uint32_t dlv_runs, dlv_retries;
    uint32_t faults, first_fault;
    uint32_t lock_violations;    /* FreeRTOS call attempted with the ROM PRIMASK lock held (plan §3 rule (e)) */
    uint32_t hung;               /* bt_ctrl_rtos_guard() demoted a bt_ctrl stuck inside one bt_ctrl_run() */
    uint32_t ctrl_stack_free_words, dlv_stack_free_words;   /* uxTaskGetStackHighWaterMark */
} bt_ctrl_rtos_stats_t;

/* bt_ctrl_init() + both tasks (once). Returns bt_ctrl_init()'s table error count, or -1 if a task could not be made. */
int  bt_ctrl_rtos_init(const bt_rom_if_t *rom, const uint8_t bd_addr[6]);
/* Plan M1 hard path without the reboot: latch bt_stop (bt_ctrl parks before its next ROM call) and mask IRQ 26.
 * Never calls the ROM; safe from any task. */
void bt_ctrl_rtos_hard_stop(void);
/* Hang guard, called periodically from a context that preempts bt_ctrl (FreeRTOS timer daemon, priority 9): if one
 * bt_ctrl_run() has been executing for more than limit_us (stuck in a ROM busy-wait), latch bt_stop, mask IRQ 26 and
 * drop bt_ctrl to the idle priority so the app keeps its cadence. Returns 1 once that happened. Cannot help if the
 * ROM spins with PRIMASK set (then the hardware WDT resets the device). */
int  bt_ctrl_rtos_guard(uint32_t limit_us);
void bt_ctrl_rtos_stats(bt_ctrl_rtos_stats_t *out);
void bt_ctrl_rtos_reset_max(void);

#endif /* BT_CTRL_RTOS_H */
