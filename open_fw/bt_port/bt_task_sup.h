/* bt_task_sup: liveness supervisor for the Bluedroid tasks (plan M4 "Watchdog", step 8).
 *
 * The app task (priority 1, the lowest but idle) feeds the hardware WDT, so a fed WDT already proves that no task above
 * it keeps the CPU. It does not prove that a BT task blocked forever (a lost wake-up, a wait on a peer that never
 * answers, a deadlock between BTU and BTC) is still doing its work. This module closes that gap without patching the
 * stack: while BT runs, the app posts a small "ping" work item to each supervised Bluedroid thread (hciT, BTU, BTC; osi
 * work queue 0) and checks that it ran.
 *
 *   - Idle threads with no pending work answer at once: an idle bonded phone or a transfer stalled by the peer (RFCOMM
 *     credits withheld, baseband not acked) is healthy, because the threads keep returning to their dispatch loop.
 *   - A ping still unanswered after stall_ms means that thread has had outstanding work (at least the ping) and made no
 *     progress for that long: bt_task_sup_poll() reports it once per episode and the session raises a platform fault
 *     (graceful recovery, else the M1 hard path: recording finalised, fault record 0xB1, reboot).
 *   - A post the queue refuses (full) counts as outstanding work from the first refusal; it is retried every poll.
 *   - Backstop: while any ping is older than hold_ms, bt_task_sup_hold() returns 1 and the firmware's wdt_feed() stops
 *     writing the WDT (the hardware resets the device about 32 s later). Reached only if the fault path itself never
 *     ran (the session normally reacts at stall_ms; its teardown has its own 2 s deadline + hard path).
 *
 * The supervisor is armed only while the stack is enabled (first platform poll after the bring-up) and disarmed when
 * the teardown starts, so the 2 s graceful window and the bring-up (RF calibration, HCI Reset) are covered by the
 * session's own deadlines, not by this module. Late pings from an earlier session never match: sequence numbers keep
 * counting across sessions. A ping item dropped by osi_thread_free() at teardown is harmless (static context only).
 *
 * Not supervised here: bt_ctrl (its own hang and CPU-share guards in bt_ctrl_rtos.c, timer daemon), bt_dlv (blocks
 * only on the hciT/BTU queues, so a stall there shows as an unanswered hciT/BTU ping), bt_recover (jobs bounded by the
 * session's up/down deadlines), bt_alarm (feeds BTU/BTC only).
 *
 * Pure logic, no RTOS calls: the platform supplies the post function. Host-tested (tests/test_bt_task_sup.c) and run
 * against the real Bluedroid threads in test-bt-host (phone scenario P7b/P8/P22).
 * SPDX-License-Identifier: MIT */
#ifndef BT_TASK_SUP_H
#define BT_TASK_SUP_H

#include <stdint.h>

#define BT_TASK_SUP_N          3u        /* supervised threads: hciT, BTU, BTC */
#define BT_TASK_SUP_PERIOD_MS  1000u     /* one ping per thread at most this often */
#define BT_TASK_SUP_STALL_MS   8000u     /* unanswered this long: stalled (session fault) */
#define BT_TASK_SUP_HOLD_MS    20000u    /* unanswered this long: WDT feed withheld (backstop) */

enum { BT_TASK_SUP_HCI = 0, BT_TASK_SUP_BTU = 1, BT_TASK_SUP_BTC = 2 };

typedef struct bt_task_sup_slot {
    volatile uint32_t done;    /* written by the ping in the supervised thread: the seq it answered */
    uint32_t seq;              /* last seq handed to a ping (never reset: late pings of an old session never match) */
    uint32_t t0;               /* when the outstanding ping was first attempted (ms) */
    uint32_t t_last;           /* when the last ping was answered or the slot was armed (ms) */
    uint32_t answers, refused, max_ms;
    uint8_t  out;              /* a ping is outstanding (posted, or refused and to be retried) */
    uint8_t  posted;           /* ... and it is in the thread's queue */
    uint8_t  stalled;          /* this episode was reported */
} bt_task_sup_slot_t;

/* Post fn(ctx) to thread `idx` (BT_TASK_SUP_*) without blocking; 1 = queued, 0 = refused / no thread. */
typedef int (*bt_task_sup_post_fn)(unsigned idx, void (*fn)(void *ctx), void *ctx);

typedef struct {
    bt_task_sup_slot_t slot[BT_TASK_SUP_N];
    bt_task_sup_post_fn post;
    uint32_t period_ms, stall_ms, hold_ms;
    volatile uint8_t armed;
    uint32_t stalls;           /* stall episodes reported (all sessions) */
} bt_task_sup_t;

typedef struct {
    uint8_t  armed;
    uint32_t stalls;
    uint32_t answers[BT_TASK_SUP_N], refused[BT_TASK_SUP_N], max_ms[BT_TASK_SUP_N];
    uint32_t outstanding_ms[BT_TASK_SUP_N];   /* age of the outstanding ping, 0 = none */
} bt_task_sup_stats_t;

/* Once: post function and the default limits. */
void bt_task_sup_init(bt_task_sup_t *s, bt_task_sup_post_fn post);
/* Test knob (host harness): shorter limits; 0 keeps a value. */
void bt_task_sup_set_limits(bt_task_sup_t *s, uint32_t period_ms, uint32_t stall_ms, uint32_t hold_ms);
/* Start supervising (stack enabled). Every slot starts healthy with no ping outstanding. Idempotent while armed. */
void bt_task_sup_arm(bt_task_sup_t *s, uint32_t now_ms);
/* Stop supervising (teardown starts). Outstanding pings are forgotten; hold drops to 0 at once. */
void bt_task_sup_disarm(bt_task_sup_t *s);
/* App task, every platform poll while armed: account answered pings, post due ones. Returns the mask of threads whose
 * stall was detected in this call (each episode once; 0 = healthy or already reported). */
uint32_t bt_task_sup_poll(bt_task_sup_t *s, uint32_t now_ms);
/* Any task / context (read-only): 1 while armed and a ping has been outstanding for hold_ms or more. */
int bt_task_sup_hold(const bt_task_sup_t *s, uint32_t now_ms);
void bt_task_sup_stats(const bt_task_sup_t *s, uint32_t now_ms, bt_task_sup_stats_t *out);
const char *bt_task_sup_name(unsigned idx);

#endif /* BT_TASK_SUP_H */
