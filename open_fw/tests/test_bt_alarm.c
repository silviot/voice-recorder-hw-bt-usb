/* Host tests for bt_port/bt_alarm.c (plan M2 "Alarms"): deterministic fake clock, every interleaving of
 * cancel / re-arm against expiry, BTC queue full at expiry, periodic coalescing, reuse after free, and the
 * hci_layer.c command-timeout re-validation pattern.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "bt_alarm.h"
#include "bt_esp_timer.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fake platform ---- */
static uint64_t t_now;
static int t_crit, t_crit_max, t_wakes, t_posts, t_post_fail;
static void (*t_on_exit)(void);
static uint64_t f_now(void) { return t_now; }
static void f_enter(void) { t_crit++; if (t_crit > t_crit_max) t_crit_max = t_crit; }
static void f_exit(void)
{
    t_crit--;
    if (t_crit == 0 && t_on_exit != NULL) {
        void (*fn)(void) = t_on_exit;
        t_on_exit = NULL;
        fn();
    }
}
static void f_wake(void) { t_wakes++; }
static int f_post(void) { t_posts++; return t_post_fail ? -1 : 0; }
static const bt_alarm_platform_t k_plat = { f_now, f_enter, f_exit, f_wake, f_post };

static void reset(void)
{
    t_now = 1000000u;
    t_crit = t_crit_max = t_wakes = t_posts = t_post_fail = 0;
    bt_alarm_init(&k_plat);
}

static int hits;
static uint32_t last_gen;
static void cb_count(void *arg) { (void)arg; hits++; last_gen = bt_alarm_delivery_gen(); }

static void test_one_shot(void)
{
    reset();
    static bt_alarm_t a;
    memset(&a, 0, sizeof(a));
    hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    uint32_t gen = bt_alarm_set(&a, 1000u, 0u);
    CHECK(t_wakes == 1, "set did not wake the service");
    t_now += 999u;
    CHECK(bt_alarm_service() == 1u, "wait time before deadline");
    bt_alarm_drain();
    CHECK(hits == 0, "fired early");
    t_now += 1u;
    CHECK(bt_alarm_service() == UINT32_MAX, "no further deadline expected");
    CHECK(t_posts == 1, "drain not posted");
    bt_alarm_drain();
    CHECK(hits == 1 && last_gen == gen, "not delivered once with its generation");
    bt_alarm_drain();
    (void)bt_alarm_service();
    bt_alarm_drain();
    CHECK(hits == 1, "delivered twice");
    CHECK(!bt_alarm_is_armed(&a), "one-shot still armed");
    CHECK(t_crit == 0 && t_crit_max == 1, "critical sections unbalanced or nested");
}

static void test_queue_full_at_expiry(void)
{
    reset();
    static bt_alarm_t a;
    memset(&a, 0, sizeof(a));
    hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10u, 0u);
    t_now += 10u;
    t_post_fail = 1;                 /* BTC queue full (and no FreeRTOS timer queue involved at all) */
    (void)bt_alarm_service();
    bt_alarm_stats_t st;
    bt_alarm_get_stats(&st);
    CHECK(st.drain_post_failures == 1, "post failure not counted");
    t_post_fail = 0;
    (void)bt_alarm_service();        /* the service may retry posting; either way: */
    bt_alarm_drain();                /* BTC drains after its next message */
    bt_alarm_drain();
    CHECK(hits == 1, "expiry lost or duplicated with a full BTC queue (hits=%d)", hits);
}

static void test_cancel_interleavings(void)
{
    static bt_alarm_t a;
    /* (1) cancel before due */
    reset(); memset(&a, 0, sizeof(a)); hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10u, 0u);
    (void)bt_alarm_cancel(&a);
    t_now += 20u; (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(hits == 0, "cancel before due: delivered");
    /* (2) due, but cancelled before the service scan */
    reset(); memset(&a, 0, sizeof(a)); hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10u, 0u);
    t_now += 20u;
    (void)bt_alarm_cancel(&a);
    (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(hits == 0, "cancel after due before scan: delivered");
    /* (3) stamped (pending), cancelled before the drain */
    reset(); memset(&a, 0, sizeof(a)); hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10u, 0u);
    t_now += 20u; (void)bt_alarm_service();
    (void)bt_alarm_cancel(&a);
    bt_alarm_drain();
    CHECK(hits == 0, "cancel while pending: delivered");
    /* (4) stamped, re-armed before the drain: old dropped, new fires once */
    reset(); memset(&a, 0, sizeof(a)); hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10u, 0u);
    t_now += 20u; (void)bt_alarm_service();
    uint32_t g2 = bt_alarm_set(&a, 50u, 0u);
    bt_alarm_drain();
    CHECK(hits == 0, "re-arm while pending: old expiry delivered");
    t_now += 50u; (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(hits == 1 && last_gen == g2, "re-armed expiry not delivered once with new generation");
    /* (5) free while pending, then reuse the object: the stale stamp must never match */
    reset(); memset(&a, 0, sizeof(a)); hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10u, 0u);
    t_now += 20u; (void)bt_alarm_service();
    bt_alarm_free(&a);
    bt_alarm_drain();
    CHECK(hits == 0, "free while pending: delivered");
    bt_alarm_setup(&a, cb_count, NULL);
    bt_alarm_drain();
    CHECK(hits == 0, "reuse after free delivered a stale expiry");
    bt_alarm_stats_t st;
    bt_alarm_get_stats(&st);
    CHECK(st.delivered == 0u, "unexpected deliveries");
}

/* hci_layer.c pattern: the command-response timer is re-armed under the HCI lock, which records the generation;
 * the callback takes that lock and ignores the expiry unless bt_alarm_delivery_gen() matches. Emulate a
 * higher-priority task re-arming between the drain's validation and the callback taking the lock. */
static bt_alarm_t g_cmd_timer;
static uint32_t g_cmd_gen;           /* protected by the (emulated) HCI lock */
static int g_timeouts_accepted, g_rearm_inside;
static void hci_cmd_timeout_cb(void *arg)
{
    (void)arg;
    if (g_rearm_inside) {             /* preemption point: new command sent, timer re-armed */
        g_rearm_inside = 0;
        g_cmd_gen = bt_alarm_set(&g_cmd_timer, 100u, 0u);
    }
    /* lock(commands_pending_response_lock) */
    if (bt_alarm_delivery_gen() == g_cmd_gen) {
        g_timeouts_accepted++;
    }
    /* unlock */
}

static void test_hci_revalidation(void)
{
    reset();
    memset(&g_cmd_timer, 0, sizeof(g_cmd_timer));
    g_timeouts_accepted = 0;
    bt_alarm_setup(&g_cmd_timer, hci_cmd_timeout_cb, NULL);
    g_cmd_gen = bt_alarm_set(&g_cmd_timer, 10u, 0u);
    t_now += 10u; (void)bt_alarm_service();
    g_rearm_inside = 1;
    bt_alarm_drain();
    CHECK(g_timeouts_accepted == 0, "stale command timeout accepted after re-arm");
    t_now += 100u; (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(g_timeouts_accepted == 1, "new command timeout not accepted exactly once (%d)", g_timeouts_accepted);
}

/* Same pattern, validated against the alarm's *current* generation under the user's lock (set/cancel also happen
 * under that lock): a cancel that lands between drain validation and the callback taking the lock must win. */
static bt_alarm_t g_t2;
static int g_t2_accepted, g_t2_cancel_inside;
static void t2_cb(void *arg)
{
    (void)arg;
    if (g_t2_cancel_inside) {
        g_t2_cancel_inside = 0;
        (void)bt_alarm_cancel(&g_t2);          /* command completed just before the timeout took the lock */
    }
    if (bt_alarm_delivery_gen() == bt_alarm_generation(&g_t2)) {
        g_t2_accepted++;
    }
}

static void test_cancel_between_validation_and_lock(void)
{
    reset();
    memset(&g_t2, 0, sizeof(g_t2));
    g_t2_accepted = 0;
    bt_alarm_setup(&g_t2, t2_cb, NULL);
    (void)bt_alarm_set(&g_t2, 10u, 0u);
    t_now += 10u; (void)bt_alarm_service();
    g_t2_cancel_inside = 1;
    bt_alarm_drain();
    CHECK(g_t2_accepted == 0, "timeout accepted although cancelled before the user lock");
    (void)bt_alarm_set(&g_t2, 10u, 0u);
    t_now += 10u; (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(g_t2_accepted == 1, "normal timeout not accepted");
    bt_alarm_stats_t st;
    bt_alarm_get_stats(&st);
    CHECK(st.dropped_stale == 0u, "drain saw a stale pending entry (invariant: set/cancel/free unlink)");
}

static void test_periodic(void)
{
    reset();
    static bt_alarm_t a;
    memset(&a, 0, sizeof(a));
    hits = 0;
    bt_alarm_setup(&a, cb_count, NULL);
    (void)bt_alarm_set(&a, 10000u, 10000u);       /* 10 ms tick, like the A2DP media timer */
    for (int i = 0; i < 5; i++) {
        t_now += 10000u;
        CHECK(bt_alarm_service() == 10000u, "periodic cadence drifted at tick %d", i);
        bt_alarm_drain();
    }
    CHECK(hits == 5, "periodic hits %d != 5", hits);
    /* slow BTC: service runs every tick for 35 ms but nothing drains -> one coalesced delivery */
    for (int i = 0; i < 3; i++) {
        t_now += 10000u;
        (void)bt_alarm_service();
    }
    t_now += 5000u;
    CHECK(bt_alarm_service() == 5000u, "periodic phase lost after coalescing");
    bt_alarm_drain();
    CHECK(hits == 6, "coalesced expiries not merged into one delivery (hits=%d)", hits);
    /* service itself late (from 85 ms to 125 ms; deadline was 90 ms): one delivery, missed periods skipped,
     * phase kept (next deadline 130 ms) */
    t_now += 40000u;
    CHECK(bt_alarm_service() == 5000u, "late service lost phase");
    bt_alarm_drain();
    CHECK(hits == 7, "late service burst or lost (hits=%d)", hits);
    bt_alarm_stats_t st;
    bt_alarm_get_stats(&st);
    CHECK(st.coalesced >= 5u, "coalescing not counted (%u)", st.coalesced);
    (void)bt_alarm_cancel(&a);
    t_now += 100000u; (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(hits == 7, "cancelled periodic fired");
}

static void test_ordering_and_wake(void)
{
    reset();
    static bt_alarm_t a, b, c;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&c, 0, sizeof(c));
    bt_alarm_setup(&a, cb_count, NULL);
    bt_alarm_setup(&b, cb_count, NULL);
    bt_alarm_setup(&c, cb_count, NULL);
    (void)bt_alarm_set(&a, 300u, 0u);
    int w = t_wakes;
    (void)bt_alarm_set(&b, 500u, 0u);
    CHECK(t_wakes == w, "later deadline woke the service needlessly");
    (void)bt_alarm_set(&c, 100u, 0u);
    CHECK(t_wakes == w + 1, "earlier deadline did not wake the service");
    CHECK(bt_alarm_service() == 100u, "next wait not the earliest deadline");
    hits = 0;
    t_now += 600u; (void)bt_alarm_service(); bt_alarm_drain();
    CHECK(hits == 3, "not all due alarms delivered (%d)", hits);
}

static esp_timer_handle_t g_del_race_h;
static int g_del_race_cb_hits, g_del_race_gen_matched, g_exits_left;
static void del_on_drain_exit(void)
{
    /* Preempt BTC right after bt_alarm_drain() releases its critical section for the popped item. */
    (void)esp_timer_delete(g_del_race_h);
}
static void step_exit_hook(void)
{
    if (--g_exits_left == 0) {
        del_on_drain_exit();
    } else {
        t_on_exit = step_exit_hook;
    }
}
static void del_race_cb(void *arg)
{
    int *cookie = (int *)arg;
    if (cookie != NULL && *cookie == 0x55AA) {
        g_del_race_cb_hits++;
    }
    if (bt_alarm_delivery_gen() == bt_esp_timer_generation(g_del_race_h)) {
        g_del_race_gen_matched++;
    }
}

static void test_esp_timer_delete_preempts_drain_and_64bit(void)
{
    reset();
    bt_esp_timer_slot_lock_t lk = { f_enter, f_exit };
    bt_esp_timer_init(lk);
    int cookie = 0x55AA;
    esp_timer_create_args_t args = {
        .callback = del_race_cb,
        .arg = &cookie,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "race",
    };
    CHECK(esp_timer_create(&args, &g_del_race_h) == ESP_OK, "esp_timer_create");
    CHECK(esp_timer_start_once(g_del_race_h, 100u) == ESP_OK, "esp_timer_start_once");
    t_now += 100u;
    (void)bt_alarm_service();
    /* bt_alarm_drain() does one enter/exit to clear drain_outstanding, then one enter/exit to pop the item. */
    g_exits_left = 2;
    t_on_exit = step_exit_hook;
    g_del_race_cb_hits = 0;
    g_del_race_gen_matched = 0;
    bt_alarm_drain();
    CHECK(g_del_race_cb_hits == 1, "esp_timer_delete preemption must not call NULL(NULL)");
    CHECK(g_del_race_gen_matched == 0, "deleted esp_timer generation must not match delivery_gen");

    /* 64-bit timeout > UINT32_MAX us (e.g. 5000 s L2CAP idle timer) and non-sentinel service wait */
    esp_timer_handle_t h64 = NULL;
    hits = 0;
    esp_timer_create_args_t a64 = { .callback = cb_count, .arg = NULL, .dispatch_method = ESP_TIMER_TASK, .name = "t64" };
    CHECK(esp_timer_create(&a64, &h64) == ESP_OK, "create 64-bit timer");
    uint64_t five_thousand_s = 5000ULL * 1000000ULL;
    CHECK(esp_timer_start_once(h64, five_thousand_s) == ESP_OK, "5000 s timeout rejected");
    uint32_t w = bt_alarm_service();
    CHECK(w == UINT32_MAX - 1u, "finite >UINT32_MAX deadline returned sentinel UINT32_MAX (%u)", w);
    t_now += five_thousand_s;
    CHECK(bt_alarm_service() == UINT32_MAX, "5000 s deadline should now be due");
    bt_alarm_drain();
    CHECK(hits == 1, "5000 s timer did not fire once (%d)", hits);
    (void)esp_timer_delete(h64);

    /* Generation wrap skips 0 sentinel */
    static bt_alarm_t wrap_a;
    memset(&wrap_a, 0, sizeof(wrap_a));
    bt_alarm_setup(&wrap_a, cb_count, NULL);
    wrap_a.generation = UINT32_MAX;
    uint32_t wg = bt_alarm_set(&wrap_a, 10u, 0u);
    CHECK(wg != 0u && bt_alarm_generation(&wrap_a) == wg, "generation wrapped to 0 sentinel");
}

int main(void)
{
    test_one_shot();
    test_queue_full_at_expiry();
    test_cancel_interleavings();
    test_hci_revalidation();
    test_cancel_between_validation_and_lock();
    test_periodic();
    test_ordering_and_wake();
    test_esp_timer_delete_preempts_drain_and_64bit();
    if (g_fail) {
        printf("test_bt_alarm: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_bt_alarm: all tests passed\n");
    return 0;
}
