/* bt_task_sup: liveness supervisor for the Bluedroid tasks. Contract and rationale: bt_task_sup.h.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bt_task_sup.h"

#define SEQ_MASK 0x00FFFFFFu     /* the ping's context carries the slot index (bits 24..31) and the sequence */

/* The one supervisor whose slots the pings answer to (one instance per image). */
static bt_task_sup_t *volatile s_target;

static void ping_cb(void *ctx)
{
    uintptr_t v = (uintptr_t)ctx;
    unsigned idx = (unsigned)((v >> 24) & 0xFFu);
    bt_task_sup_t *s = s_target;
    if (s != NULL && idx < BT_TASK_SUP_N) {
        __atomic_store_n(&s->slot[idx].done, (uint32_t)(v & SEQ_MASK), __ATOMIC_RELEASE);
    }
}

void bt_task_sup_init(bt_task_sup_t *s, bt_task_sup_post_fn post)
{
    memset(s, 0, sizeof(*s));
    s->post = post;
    s->period_ms = BT_TASK_SUP_PERIOD_MS;
    s->stall_ms = BT_TASK_SUP_STALL_MS;
    s->hold_ms = BT_TASK_SUP_HOLD_MS;
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        s->slot[i].done = SEQ_MASK;   /* != seq 0 */
    }
    s_target = s;
}

void bt_task_sup_set_limits(bt_task_sup_t *s, uint32_t period_ms, uint32_t stall_ms, uint32_t hold_ms)
{
    if (period_ms != 0u) {
        s->period_ms = period_ms;
    }
    if (stall_ms != 0u) {
        s->stall_ms = stall_ms;
    }
    if (hold_ms != 0u) {
        s->hold_ms = hold_ms;
    }
}

void bt_task_sup_arm(bt_task_sup_t *s, uint32_t now_ms)
{
    if (s->armed) {
        return;
    }
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        bt_task_sup_slot_t *sl = &s->slot[i];
        sl->out = sl->posted = sl->stalled = 0u;
        sl->t_last = now_ms - s->period_ms;   /* first ping at the first poll */
        sl->seq = (sl->seq + 1u) & SEQ_MASK;  /* a late ping of the previous session never matches */
    }
    __atomic_store_n(&s->armed, 1u, __ATOMIC_RELEASE);
}

void bt_task_sup_disarm(bt_task_sup_t *s)
{
    __atomic_store_n(&s->armed, 0u, __ATOMIC_RELEASE);
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        s->slot[i].out = s->slot[i].posted = s->slot[i].stalled = 0u;
    }
}

uint32_t bt_task_sup_poll(bt_task_sup_t *s, uint32_t now_ms)
{
    if (!s->armed || s->post == NULL) {
        return 0u;
    }
    uint32_t mask = 0u;
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        bt_task_sup_slot_t *sl = &s->slot[i];
        if (sl->out && sl->posted && __atomic_load_n(&sl->done, __ATOMIC_ACQUIRE) == sl->seq) {
            uint32_t ms = now_ms - sl->t0;
            if (ms > sl->max_ms) {
                sl->max_ms = ms;
            }
            sl->answers++;
            sl->out = sl->posted = sl->stalled = 0u;
            sl->t_last = now_ms;
        }
        if (!sl->out && now_ms - sl->t_last >= s->period_ms) {
            sl->seq = (sl->seq + 1u) & SEQ_MASK;
            sl->out = 1u;
            sl->posted = 0u;
            sl->t0 = now_ms;
        }
        if (sl->out && !sl->posted) {
            if (s->post(i, ping_cb, (void *)(uintptr_t)(((uintptr_t)i << 24) | sl->seq))) {
                sl->posted = 1u;
            } else {
                sl->refused++;          /* queue full or thread gone: outstanding work since t0, retried next poll */
            }
        }
        if (sl->out && !sl->stalled && now_ms - sl->t0 >= s->stall_ms) {
            sl->stalled = 1u;
            s->stalls++;
            mask |= 1u << i;
        }
    }
    return mask;
}

int bt_task_sup_hold(const bt_task_sup_t *s, uint32_t now_ms)
{
    if (!__atomic_load_n(&s->armed, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        const bt_task_sup_slot_t *sl = &s->slot[i];
        if (sl->out && now_ms - sl->t0 >= s->hold_ms) {
            return 1;
        }
    }
    return 0;
}

void bt_task_sup_stats(const bt_task_sup_t *s, uint32_t now_ms, bt_task_sup_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->armed = s->armed;
    out->stalls = s->stalls;
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        const bt_task_sup_slot_t *sl = &s->slot[i];
        out->answers[i] = sl->answers;
        out->refused[i] = sl->refused;
        out->max_ms[i] = sl->max_ms;
        out->outstanding_ms[i] = (s->armed && sl->out) ? now_ms - sl->t0 : 0u;
    }
}

const char *bt_task_sup_name(unsigned idx)
{
    static const char *const k_names[BT_TASK_SUP_N] = { "hciT", "BTU_TASK", "BTC_TASK" };
    return idx < BT_TASK_SUP_N ? k_names[idx] : "?";
}
