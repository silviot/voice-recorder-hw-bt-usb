/* Host tests for bt_port/bt_task_sup.c (plan M4 "Watchdog", step 8): fake threads with bounded queues that run, stall
 * or refuse; idle and busy threads stay healthy, a stalled one is reported once per episode at stall_ms and holds the
 * WDT feed from hold_ms, a full queue counts as outstanding work, disarm/re-arm, late pings of an earlier session,
 * clock wrap.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "bt_task_sup.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            g_fail++;                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

/* ---- fake osi threads: a 4-deep work queue each ---- */
#define QCAP 4u
typedef struct {
    void (*fn[QCAP])(void *);
    void *ctx[QCAP];
    unsigned n;
    int stalled;     /* the thread does not run its queue */
    int gone;        /* no thread (post refused) */
} fthread_t;
static fthread_t T[BT_TASK_SUP_N];
static unsigned posts;

static int f_post(unsigned idx, void (*fn)(void *), void *ctx)
{
    posts++;
    if (idx >= BT_TASK_SUP_N || T[idx].gone || T[idx].n >= QCAP) {
        return 0;
    }
    T[idx].fn[T[idx].n] = fn;
    T[idx].ctx[T[idx].n] = ctx;
    T[idx].n++;
    return 1;
}

static void run_threads(void)
{
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        if (T[i].stalled) {
            continue;
        }
        for (unsigned k = 0u; k < T[i].n; k++) {
            T[i].fn[k](T[i].ctx[k]);
        }
        T[i].n = 0u;
    }
}

static bt_task_sup_t S;
static uint32_t now;

static void reset(uint32_t t0)
{
    memset(T, 0, sizeof(T));
    posts = 0u;
    now = t0;
    bt_task_sup_init(&S, f_post);
}

/* Advance the clock in 10 ms steps, threads running between polls; returns the OR of the stall masks. */
static uint32_t advance(uint32_t ms)
{
    uint32_t mask = 0u;
    for (uint32_t t = 0u; t < ms; t += 10u) {
        now += 10u;
        mask |= bt_task_sup_poll(&S, now);
        run_threads();
    }
    return mask;
}

static void test_idle_healthy(void)
{
    reset(5000u);
    bt_task_sup_arm(&S, now);
    uint32_t m = advance(60000u);           /* an idle bonded phone for a minute */
    bt_task_sup_stats_t st;
    bt_task_sup_stats(&S, now, &st);
    CHECK(m == 0u && st.stalls == 0u, "idle threads reported stalled (mask %x)", m);
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        CHECK(st.answers[i] >= 58u && st.answers[i] <= 61u, "%s answered %u pings in 60 s (period 1 s)",
              bt_task_sup_name(i), st.answers[i]);
        CHECK(st.max_ms[i] <= 10u, "%s latency %u ms", bt_task_sup_name(i), st.max_ms[i]);
    }
    CHECK(!bt_task_sup_hold(&S, now), "hold while healthy");
}

static void test_not_armed(void)
{
    reset(0u);
    CHECK(advance(30000u) == 0u && posts == 0u, "posted %u pings while disarmed", posts);
    CHECK(!bt_task_sup_hold(&S, now), "hold while disarmed");
}

/* A thread that is slow but always answers within stall_ms (busy with long work items) stays healthy. */
static void test_slow_but_alive(void)
{
    reset(100u);
    bt_task_sup_arm(&S, now);
    uint32_t m = 0u;
    for (int round = 0; round < 10; round++) {
        T[BT_TASK_SUP_BTU].stalled = 1;
        m |= advance(BT_TASK_SUP_STALL_MS - 500u);   /* busy 7.5 s */
        T[BT_TASK_SUP_BTU].stalled = 0;
        m |= advance(200u);
    }
    bt_task_sup_stats_t st;
    bt_task_sup_stats(&S, now, &st);
    CHECK(m == 0u, "slow BTU reported (mask %x)", m);
    CHECK(st.max_ms[BT_TASK_SUP_BTU] >= 7000u && st.max_ms[BT_TASK_SUP_BTU] < BT_TASK_SUP_STALL_MS,
          "BTU max latency %u", st.max_ms[BT_TASK_SUP_BTU]);
}

static void test_stall_once_then_hold(void)
{
    reset(1000u);
    bt_task_sup_arm(&S, now);
    (void)advance(3000u);
    T[BT_TASK_SUP_BTC].stalled = 1;
    uint32_t t_stall = now;
    uint32_t first = 0u, at = 0u;
    for (uint32_t t = 0u; t < 12000u && first == 0u; t += 10u) {
        now += 10u;
        first = bt_task_sup_poll(&S, now);
        run_threads();
        at = now;
    }
    CHECK(first == (1u << BT_TASK_SUP_BTC), "mask %x, want BTC only", first);
    /* the ping went out at most one period after the stall began; reported stall_ms after the ping */
    CHECK(at - t_stall >= BT_TASK_SUP_STALL_MS && at - t_stall <= BT_TASK_SUP_STALL_MS + BT_TASK_SUP_PERIOD_MS + 20u,
          "reported %u ms after the stall began", at - t_stall);
    CHECK(advance(5000u) == 0u, "reported twice in one episode");
    CHECK(!bt_task_sup_hold(&S, now), "hold before hold_ms");
    (void)advance(BT_TASK_SUP_HOLD_MS - BT_TASK_SUP_STALL_MS);
    CHECK(bt_task_sup_hold(&S, now), "no hold after hold_ms");
    bt_task_sup_stats_t st;
    bt_task_sup_stats(&S, now, &st);
    CHECK(st.stalls == 1u && st.outstanding_ms[BT_TASK_SUP_BTC] >= BT_TASK_SUP_HOLD_MS, "stalls %u age %u", st.stalls,
          st.outstanding_ms[BT_TASK_SUP_BTC]);
    CHECK(st.outstanding_ms[BT_TASK_SUP_HCI] == 0u || st.outstanding_ms[BT_TASK_SUP_HCI] < 20u, "hciT outstanding");
    /* the thread recovers: answered, hold released, a later stall is a new episode */
    T[BT_TASK_SUP_BTC].stalled = 0;
    (void)advance(100u);
    CHECK(!bt_task_sup_hold(&S, now), "hold after recovery");
    T[BT_TASK_SUP_BTC].stalled = 1;
    CHECK(advance(BT_TASK_SUP_STALL_MS + BT_TASK_SUP_PERIOD_MS + 100u) == (1u << BT_TASK_SUP_BTC),
          "second episode not reported");
    /* disarm (teardown starts): hold drops at once, nothing posted any more */
    (void)advance(BT_TASK_SUP_HOLD_MS);
    CHECK(bt_task_sup_hold(&S, now), "no hold before disarm");
    bt_task_sup_disarm(&S);
    CHECK(!bt_task_sup_hold(&S, now), "hold after disarm");
    unsigned p0 = posts;
    CHECK(advance(5000u) == 0u && posts == p0, "activity while disarmed");
}

/* Queue full / no thread: outstanding from the first refusal, retried every poll, reported at stall_ms. */
static void test_refused(void)
{
    reset(0u);
    T[BT_TASK_SUP_HCI].gone = 1;
    bt_task_sup_arm(&S, now);
    uint32_t m = advance(BT_TASK_SUP_STALL_MS + 20u);
    bt_task_sup_stats_t st;
    bt_task_sup_stats(&S, now, &st);
    CHECK(m == (1u << BT_TASK_SUP_HCI), "refused posts: mask %x", m);
    CHECK(st.refused[BT_TASK_SUP_HCI] >= BT_TASK_SUP_STALL_MS / 10u - 2u, "refusals %u (retried every poll)",
          st.refused[BT_TASK_SUP_HCI]);
    /* a full queue that drains in time is healthy */
    reset(0u);
    bt_task_sup_arm(&S, now);
    T[BT_TASK_SUP_BTU].n = QCAP;            /* full of other work, not running for 2 s */
    T[BT_TASK_SUP_BTU].stalled = 1;
    for (unsigned k = 0u; k < QCAP; k++) {
        T[BT_TASK_SUP_BTU].fn[k] = (void (*)(void *))0;
    }
    m = 0u;
    for (uint32_t t = 0u; t < 2000u; t += 10u) {
        now += 10u;
        m |= bt_task_sup_poll(&S, now);
    }
    T[BT_TASK_SUP_BTU].n = 0u;              /* the other work ran */
    T[BT_TASK_SUP_BTU].stalled = 0;
    m |= advance(BT_TASK_SUP_STALL_MS);
    bt_task_sup_stats(&S, now, &st);
    CHECK(m == 0u && st.refused[BT_TASK_SUP_BTU] > 0u, "full queue that drained: mask %x refused %u", m,
          st.refused[BT_TASK_SUP_BTU]);
}

/* A ping of the previous session that runs after the re-arm must not answer the new session's ping. */
static void test_late_ping(void)
{
    reset(0u);
    bt_task_sup_arm(&S, now);
    T[BT_TASK_SUP_BTC].stalled = 1;
    (void)advance(500u);                    /* BTC holds the old session's ping */
    CHECK(T[BT_TASK_SUP_BTC].n == 1u, "old ping queued (%u)", T[BT_TASK_SUP_BTC].n);
    void (*old_fn)(void *) = T[BT_TASK_SUP_BTC].fn[0];
    void *old_ctx = T[BT_TASK_SUP_BTC].ctx[0];
    bt_task_sup_disarm(&S);
    T[BT_TASK_SUP_BTC].n = 0u;              /* osi_thread_free dropped it ... */
    bt_task_sup_arm(&S, now);
    now += 10u;
    (void)bt_task_sup_poll(&S, now);        /* new ping posted (thread still stalled) */
    old_fn(old_ctx);                        /* ... or it ran late anyway */
    uint32_t m = advance(BT_TASK_SUP_STALL_MS + 20u);
    CHECK(m == (1u << BT_TASK_SUP_BTC), "late ping answered the new session (mask %x)", m);
}

static void test_wrap(void)
{
    reset(0xFFFFF000u);
    bt_task_sup_arm(&S, now);
    CHECK(advance(20000u) == 0u, "healthy across the 32-bit ms wrap");
    T[BT_TASK_SUP_HCI].stalled = 1;
    CHECK(advance(BT_TASK_SUP_STALL_MS + BT_TASK_SUP_PERIOD_MS + 20u) == (1u << BT_TASK_SUP_HCI), "stall after wrap");
}

static void test_limits(void)
{
    reset(0u);
    bt_task_sup_set_limits(&S, 100u, 400u, 900u);
    bt_task_sup_set_limits(&S, 0u, 0u, 0u);   /* 0 keeps */
    CHECK(S.period_ms == 100u && S.stall_ms == 400u && S.hold_ms == 900u, "limits");
    bt_task_sup_arm(&S, now);
    T[BT_TASK_SUP_BTU].stalled = 1;
    CHECK(advance(520u) == (1u << BT_TASK_SUP_BTU), "short limits");
    (void)advance(500u);
    CHECK(bt_task_sup_hold(&S, now), "short hold");
}

int main(void)
{
    test_idle_healthy();
    test_not_armed();
    test_slow_but_alive();
    test_stall_once_then_hold();
    test_refused();
    test_late_ping();
    test_wrap();
    test_limits();
    printf("test_bt_task_sup: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
