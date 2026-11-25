/* bt_ctrl_rtos: FreeRTOS bindings for bt_ctrl (plan §3 / M1): the bt_ctrl task (OPENFW_PRIO_BT_CTRL), the bt_dlv
 * task (OPENFW_PRIO_BT_DLV), both static in .rtos_static, and the bt_ctrl_platform_t callbacks (critical sections,
 * task notifications, IRQ-26 wake from the top half, bt_stop latch, fault post).
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_RTOS_H
#define BT_CTRL_RTOS_H

#include <stdint.h>
#include "bt_ctrl.h"

/* bt_ctrl task stack. Plan §4 budgets 2 KiB; Exp 1 stage A measured 888 B free with the controller idle. Stage B runs
 * forwarded KE messages into the ROM's LM/LC handlers on this stack (depth unknown), and configCHECK_FOR_STACK_OVERFLOW=2
 * only notices an overflow at the next context switch, after neighbouring .rtos_static data may be overwritten. This
 * file is built only into the probe image (bt/Makefile.bluedroid PROBE_SRCS), which therefore gets 4 KiB: the
 * watermark still measures the real depth (used = size - free, HCIP STACK ctrl_size/ctrl_free) without risking the run.
 * Size the integrated image's task from that measurement. */
#define BT_CTRL_RTOS_CTRL_STACK_WORDS 1024u

typedef struct {
    uint32_t ctrl_runs;          /* bt_ctrl_run() calls */
    uint32_t ctrl_run_max_us;    /* longest bt_ctrl_run() since bt_ctrl_rtos_reset_max() (window maximum) */
    uint32_t ctrl_run_max_all_us; /* longest bt_ctrl_run() ever, incl. HCI Reset (RF calibration + ROM start) */
    uint32_t ctrl_run_total_us;  /* sum over ctrl_runs (CPU time of the bt_ctrl task body) */
    uint32_t ctrl_more;          /* bt_ctrl_run() returned "more work" */
    uint32_t more_streak_max;    /* most consecutive "more" runs (no block in between), ever (review C5) */
    uint32_t more_streak_max_us; /* longest time from the first run of such a streak to the end of its last run */
    uint32_t dlv_runs, dlv_retries;
    uint32_t faults, first_fault;
    uint32_t lock_violations;    /* FreeRTOS call attempted with the ROM PRIMASK lock held (plan §3 rule (e)) */
    uint32_t hung;               /* bt_ctrl demoted: stuck inside one bt_ctrl_run() (bt_ctrl_rtos_guard) or over its
                                  * CPU share (bt_ctrl_rtos_cpu_guard, then cpu_tripped = 1 too) */
    uint32_t cpu_tripped;        /* bt_ctrl_rtos_cpu_guard() demoted it */
    uint32_t cpu_peak_pm;        /* highest bt_ctrl CPU share in one guard sample (permille) while armed, since
                                  * bt_ctrl_rtos_reset_max() */
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
/* CPU-share guard (bt_cpu_guard.h), called from the same periodic context as bt_ctrl_rtos_guard(): while armed, if
 * bt_ctrl's busy time is above limit_pm (permille) of the elapsed time in `samples` consecutive calls, take the same
 * action as the hang guard (bt_stop, IRQ 26 masked, bt_ctrl demoted to idle priority; hung = cpu_tripped = 1).
 * Returns 1 once that happened. Disarmed by default: an HCI Reset legitimately keeps bt_ctrl busy for ~0.2-0.5 s (RF
 * calibration busy-wait), several back to back during restarts, so arm it only around phases without Resets. */
int  bt_ctrl_rtos_cpu_guard(uint32_t limit_pm, uint32_t samples);
/* Arm (on != 0) or disarm the CPU-share guard; either way its baseline and consecutive count restart. Any task. */
void bt_ctrl_rtos_cpu_guard_arm(int on);
void bt_ctrl_rtos_stats(bt_ctrl_rtos_stats_t *out);
void bt_ctrl_rtos_reset_max(void);

#endif /* BT_CTRL_RTOS_H */
