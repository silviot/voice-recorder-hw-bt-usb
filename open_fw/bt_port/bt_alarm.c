/* bt_alarm: see bt_alarm.h.
 * SPDX-License-Identifier: MIT */
#include "bt_alarm.h"
#include <stddef.h>

static struct {
    bt_alarm_platform_t p;
    bt_alarm_t *deadlines;     /* sorted by deadline_us */
    bt_alarm_t *pending_head;  /* FIFO */
    bt_alarm_t *pending_tail;
    uint8_t drain_outstanding;
    uint32_t delivery_gen;
    bt_alarm_stats_t stats;
} g;

static void dl_remove(bt_alarm_t *a)
{
    for (bt_alarm_t **pp = &g.deadlines; *pp != NULL; pp = &(*pp)->next_dl) {
        if (*pp == a) {
            *pp = a->next_dl;
            break;
        }
    }
    a->next_dl = NULL;
}

static void dl_insert(bt_alarm_t *a)
{
    bt_alarm_t **pp = &g.deadlines;
    while (*pp != NULL && (*pp)->deadline_us <= a->deadline_us) {
        pp = &(*pp)->next_dl;
    }
    a->next_dl = *pp;
    *pp = a;
}

static void pd_remove(bt_alarm_t *a)
{
    bt_alarm_t *prev = NULL;
    for (bt_alarm_t *it = g.pending_head; it != NULL; prev = it, it = it->next_pd) {
        if (it == a) {
            if (prev == NULL) {
                g.pending_head = it->next_pd;
            } else {
                prev->next_pd = it->next_pd;
            }
            if (g.pending_tail == it) {
                g.pending_tail = prev;
            }
            break;
        }
    }
    a->next_pd = NULL;
    a->pending = 0u;
}

static void pd_append(bt_alarm_t *a)
{
    a->next_pd = NULL;
    if (g.pending_tail == NULL) {
        g.pending_head = a;
    } else {
        g.pending_tail->next_pd = a;
    }
    g.pending_tail = a;
    a->pending = 1u;
}

void bt_alarm_init(const bt_alarm_platform_t *platform)
{
    g.p = *platform;
    g.deadlines = NULL;
    g.pending_head = g.pending_tail = NULL;
    g.drain_outstanding = 0u;
    g.delivery_gen = 0u;
    g.stats = (bt_alarm_stats_t){ 0 };
}

void bt_alarm_setup(bt_alarm_t *a, bt_alarm_cb_t cb, void *arg)
{
    g.p.enter();
    uint32_t gen = a->generation; /* preserved across reuse so stale stamps can never match again */
    if (a->in_use) {
        dl_remove(a);
        pd_remove(a);
    }
    a->cb = cb;
    a->arg = arg;
    a->deadline_us = 0u;
    a->period_us = 0u;
    a->generation = gen + 1u;
    a->stamp = 0u;
    a->coalesced = 0u;
    a->next_dl = a->next_pd = NULL;
    a->armed = 0u;
    a->pending = 0u;
    a->in_use = 1u;
    g.p.exit();
}

uint32_t bt_alarm_set(bt_alarm_t *a, uint32_t timeout_us, uint32_t period_us)
{
    g.p.enter();
    bt_alarm_t *old_first = g.deadlines;
    dl_remove(a);
    pd_remove(a);
    a->generation++;
    a->period_us = period_us;
    a->deadline_us = g.p.now_us() + timeout_us;
    a->armed = 1u;
    dl_insert(a);
    uint32_t gen = a->generation;
    int need_wake = g.deadlines != old_first || g.deadlines == a;
    g.p.exit();
    if (need_wake) {
        g.p.wake();
    }
    return gen;
}

uint32_t bt_alarm_cancel(bt_alarm_t *a)
{
    g.p.enter();
    dl_remove(a);
    pd_remove(a);
    a->armed = 0u;
    a->generation++;
    uint32_t gen = a->generation;
    g.p.exit();
    return gen;
}

void bt_alarm_free(bt_alarm_t *a)
{
    g.p.enter();
    dl_remove(a);
    pd_remove(a);
    a->armed = 0u;
    a->generation++;
    a->in_use = 0u;
    a->cb = NULL;
    a->arg = NULL;
    g.p.exit();
}

bool bt_alarm_is_armed(const bt_alarm_t *a)
{
    return a->armed != 0u;
}

uint32_t bt_alarm_generation(const bt_alarm_t *a)
{
    return a->generation;
}

uint32_t bt_alarm_service(void)
{
    int post = 0;
    uint32_t wait_us = UINT32_MAX;
    g.p.enter();
    uint64_t now = g.p.now_us();
    while (g.deadlines != NULL && g.deadlines->deadline_us <= now) {
        bt_alarm_t *a = g.deadlines;
        g.deadlines = a->next_dl;
        a->next_dl = NULL;
        if (a->pending) {
            a->coalesced++;           /* previous expiry not delivered yet: merge */
            g.stats.coalesced++;
        } else {
            a->stamp = a->generation;
            pd_append(a);
        }
        if (a->period_us != 0u) {
            uint64_t next = a->deadline_us + a->period_us;
            while (next <= now) {     /* late: skip missed periods instead of bursting */
                next += a->period_us;
                a->coalesced++;
                g.stats.coalesced++;
            }
            a->deadline_us = next;
            dl_insert(a);
        } else {
            a->armed = 0u;
        }
    }
    if (g.pending_head != NULL && !g.drain_outstanding) {
        g.drain_outstanding = 1u;
        post = 1;
    }
    if (g.deadlines != NULL) {
        uint64_t d = g.deadlines->deadline_us - now;
        wait_us = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
    }
    g.p.exit();
    if (post && g.p.post_drain() != 0) {
        /* BTC queue full: BTC is busy and drains after every message, so nothing is lost. */
        g.p.enter();
        g.drain_outstanding = 0u;
        g.stats.drain_post_failures++;
        g.p.exit();
    }
    return wait_us;
}

void bt_alarm_drain(void)
{
    g.p.enter();
    g.drain_outstanding = 0u;   /* cleared before draining so a concurrent expiry can post a new item */
    g.p.exit();
    for (;;) {
        g.p.enter();
        bt_alarm_t *a = g.pending_head;
        if (a == NULL) {
            g.p.exit();
            break;
        }
        g.pending_head = a->next_pd;
        if (g.pending_head == NULL) {
            g.pending_tail = NULL;
        }
        a->next_pd = NULL;
        a->pending = 0u;
        /* set/cancel/free unlink from the pending list synchronously, so a stale stamp cannot normally reach here;
         * the generation compare is a defensive invariant check (counted as dropped_stale, which tests require
         * to stay 0). Cancels racing with the callback itself are caught by the user's re-validation against
         * bt_alarm_generation() under its own lock (see bt_alarm.h). */
        int valid = a->in_use && a->cb != NULL && a->stamp == a->generation;
        bt_alarm_cb_t cb = a->cb;
        void *arg = a->arg;
        uint32_t stamp = a->stamp;
        if (valid) {
            g.stats.delivered++;
        } else {
            g.stats.dropped_stale++;
        }
        g.p.exit();
        if (valid) {
            g.delivery_gen = stamp;
            cb(arg);
            g.delivery_gen = 0u;
        }
    }
}

uint32_t bt_alarm_delivery_gen(void)
{
    return g.delivery_gen;
}

uint64_t bt_alarm_now_us(void)
{
    return g.p.now_us();
}

void bt_alarm_get_stats(bt_alarm_stats_t *out)
{
    g.p.enter();
    *out = g.stats;
    g.p.exit();
}
