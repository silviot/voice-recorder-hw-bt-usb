/* bt_ctrl_leak: what the bt_ctrl task does when a ROM path returned with the ROM lock (PRIMASK) still held, after
 * bt_ctrl_run(). Kept apart from bt_ctrl_rtos.c (SL6801 registers, FreeRTOS) so the host tests drive it with the fake
 * ROM (tests/test_bt_ctrl.c test_lock_leak_recovery).
 *
 * On a leak: stop the controller (stop), try to unwind the lock (unwind; NULL = this ROM has none), and post
 * BT_CTRL_FAULT_INVARIANT once per leak (not once per task loop). Then:
 *   BT_CTRL_LEAK_UNWOUND: the lock is free again; run bt_ctrl again at once so it finishes the stop.
 *   BT_CTRL_LEAK_STUCK:   the lock is still held (no unwind for this ROM, or it did not clear the depth). Running
 *                         bt_ctrl again cannot clear it, so the caller must NOT re-run at once: re-running is a busy
 *                         loop that never blocks and posts a fault every pass. It waits for the next wake instead.
 *                         The real ROM always unwinds (bt_rom_real_force_unlock() zeroes the depth), so this case is
 *                         reached only with a ROM model without PRIMASK, where blocking is safe.
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_LEAK_H
#define BT_CTRL_LEAK_H

#include <stdint.h>
#include "bt_ctrl.h"

enum { BT_CTRL_LEAK_NONE = 0, BT_CTRL_LEAK_UNWOUND, BT_CTRL_LEAK_STUCK };

typedef struct {
    uint8_t stuck;          /* the previous check returned BT_CTRL_LEAK_STUCK (the same leak: no new fault) */
    uint32_t leaks;         /* checks that found the lock held */
    uint32_t reported;      /* faults posted (one per leak) */
} bt_ctrl_leak_t;

static inline int bt_ctrl_leak_check(bt_ctrl_leak_t *st, const bt_rom_if_t *rom, void (*stop)(void),
                                     void (*unwind)(void), void (*fault)(uint32_t code))
{
    if (rom == NULL || rom->lock_depth == NULL || rom->lock_depth() == 0u) {
        st->stuck = 0u;
        return BT_CTRL_LEAK_NONE;
    }
    st->leaks++;
    stop();
    if (unwind != NULL) {
        unwind();
    }
    int r = rom->lock_depth() == 0u ? BT_CTRL_LEAK_UNWOUND : BT_CTRL_LEAK_STUCK;
    if (!st->stuck) {
        st->reported++;
        fault(BT_CTRL_FAULT_INVARIANT);
    }
    st->stuck = (uint8_t)(r == BT_CTRL_LEAK_STUCK);
    return r;
}

#endif /* BT_CTRL_LEAK_H */
