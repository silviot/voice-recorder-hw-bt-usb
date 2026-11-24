/* bt_cpu_guard: CPU-share starvation check for the bt_ctrl task (plan §3: bt_ctrl priority 8 runs above the app, so a
 * controller that keeps it busy -- an IRQ-26 status bit that re-latches at once, a flood of scan-slot IRQs -- starves
 * the app task and with it the WDT feed). Sampled from a context that preempts bt_ctrl (FreeRTOS timer daemon,
 * priority 9): each sample compares bt_ctrl's busy time (completed runs plus the one in progress) with the elapsed time
 * since the previous sample. Pure arithmetic, host-tested in tests/test_hci_probe.c (make test-bt-port).
 * Complements bt_ctrl_rtos_guard(), which only catches one bt_ctrl_run() stuck for seconds; neither helps against a
 * ROM spin with PRIMASK set or an interrupt storm that keeps the CPU in the IRQ-26 top half (then the WDT resets).
 * SPDX-License-Identifier: MIT */
#ifndef BT_CPU_GUARD_H
#define BT_CPU_GUARD_H

#include <stdint.h>

typedef struct {
    uint32_t t_us, busy_us;  /* previous sample */
    uint32_t over;           /* consecutive samples above the limit */
    uint32_t peak_pm;        /* highest per-sample share (permille) since the last reset */
    uint8_t  valid;          /* a baseline sample exists */
} bt_cpu_guard_t;

#define BT_CPU_GUARD_MIN_SAMPLE_US 1000u   /* shorter intervals are merged into the next sample */

static inline void bt_cpu_guard_reset(bt_cpu_guard_t *g)
{
    g->t_us = 0u;
    g->busy_us = 0u;
    g->over = 0u;
    g->peak_pm = 0u;
    g->valid = 0u;
}

/* One sample: now_us and busy_us are free-running 32-bit microsecond counters (wrap-safe). Returns 1 once the share
 * has been above limit_pm (permille) in `samples` consecutive samples (and keeps returning 1 while it stays above). */
static inline int bt_cpu_guard_step(bt_cpu_guard_t *g, uint32_t now_us, uint32_t busy_us, uint32_t limit_pm,
                                    uint32_t samples)
{
    if (!g->valid) {
        g->t_us = now_us;
        g->busy_us = busy_us;
        g->over = 0u;
        g->valid = 1u;
        return 0;
    }
    uint32_t dt = now_us - g->t_us;
    if (dt < BT_CPU_GUARD_MIN_SAMPLE_US) {
        return 0;                                     /* keep the baseline: too short to measure */
    }
    uint32_t db = busy_us - g->busy_us;
    g->t_us = now_us;
    g->busy_us = busy_us;
    if ((int32_t)db < 0) {
        db = 0u;                                      /* busy went backwards (torn read): not evidence of load */
    } else if (db > dt) {
        db = dt;                                      /* run-length rounding: never more than 100 % */
    }
    uint32_t pm = db / (dt / 1000u);                  /* no 64-bit division; dt >= 1 ms */
    if (pm > 1000u) {
        pm = 1000u;
    }
    if (pm > g->peak_pm) {
        g->peak_pm = pm;
    }
    g->over = pm > limit_pm ? g->over + 1u : 0u;
    return samples != 0u && g->over >= samples;
}

#endif /* BT_CPU_GUARD_H */
