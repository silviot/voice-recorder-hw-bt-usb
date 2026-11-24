/* Bluedroid end-to-end host harness, path (b) "through M1" (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M3, step 5):
 * real Bluedroid (patches 0001-0010) + bt_port (alarms, esp_timer shim, store, entropy) + bt_ctrl + tests/fake_rom.c,
 * on FreeRTOS-Kernel V11.1.0 (POSIX port), under ASan/UBSan (TSan variant: make test-bt-host-tsan).
 *
 *   test_bt_host [all [N]]       all scenarios: patch 0009 fixed_queue slot restore, init+enable, GAP name/scan mode,
 *                                disable+deinit, patch 0010 NOACK at zero command credits, N-cycle leak loop (default 200)
 *   test_bt_host fixedq          the patch 0009 scenario alone
 *   test_bt_host noack           the patch 0010 scenario alone
 *   test_bt_host cycles N        the leak loop with N cycles
 *   test_bt_host sweep-count     prints "ENABLE_ALLOCS <n>": allocation attempts made while esp_bluedroid_enable() runs
 *   test_bt_host sweep-one N     one cycle with the N-th allocation attempt of enable failing; exit 0 when enable
 *                                completed (ENABLED) or failed cleanly, and the stack tore down with no leak; a point
 *                                whose N-th attempt never happened (nothing injected) fails
 *
 * Every blocking step runs under a deadline enforced by a watchdog pthread outside FreeRTOS (exit code 124 + the
 * step name on expiry), so a hang never stalls the gate.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "openfw_rtos.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "osi/fixed_queue.h"
#include "bt_ctrl.h"
#include "fake_rom.h"
#include "host_env.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            g_fail++;                                                                                                 \
            bth_log("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            bth_log(__VA_ARGS__);                                                                                     \
            bth_log("\n");                                                                                            \
        }                                                                                                             \
    } while (0)

/* ------------------------------------------------------------------------------------------------------------- */
/* watchdog report (the watchdog thread itself lives in host_main.c, which is built without bt_port's pthread.h)   */
/* ------------------------------------------------------------------------------------------------------------- */
static uint64_t mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void bth_hang_report(void)
{
    bt_ctrl_debug_t d;
    bt_ctrl_get_debug(&d);
    fprintf(stderr, "  bt_ctrl state %u admission %u dlv_open %u fwd_pending %u tx_free %u evt_used %u acl_used %u, "
            "bluedroid status %d\n", d.state, d.admission_open, d.dlv_open, d.fwd_pending, (unsigned)d.tx_free,
            (unsigned)d.evt_used, (unsigned)d.acl_used, (int)esp_bluedroid_get_status());
}

/* ------------------------------------------------------------------------------------------------------------- */
/* GAP observation                                                                                                */
/* ------------------------------------------------------------------------------------------------------------- */
static volatile int s_name_evt;
static char s_name[64];

/* BTC hands callbacks the copy it placed right after the 4-byte btc_msg_t: 4-aligned (fine on Cortex-M4), not
 * 8-aligned as x86-64 wants for this pointer-holding union (see the Makefile's alignment note). */
__attribute__((no_sanitize("alignment")))
static void gap_cb(esp_bt_gap_cb_event_t e, esp_bt_gap_cb_param_t *p)
{
    if (e == ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT) {
        if (p->get_dev_name_cmpl.status == ESP_BT_STATUS_SUCCESS && p->get_dev_name_cmpl.name != NULL) {
            strncpy(s_name, p->get_dev_name_cmpl.name, sizeof(s_name) - 1u);
        }
        s_name_evt++;
    }
}

/* Last forwarded ROM command with this request id (fake ROM command log), or NULL. Caller holds the ROM lock. */
static const fake_cmd_rec_t *last_cmd(uint16_t id)
{
    for (uint32_t i = fake_rom.n_cmds; i > 0u; i--) {
        if (fake_rom.cmds[i - 1u].id == id) {
            return &fake_rom.cmds[i - 1u];
        }
    }
    return NULL;
}

/* The fake ROM keeps the first 64 forwarded commands; scenarios that inspect them start from an empty log. */
static void rom_log_clear(void)
{
    bth_rom_lock();
    fake_rom.n_cmds = 0u;
    bth_rom_unlock();
}

static int wait_until(volatile int *flag, int want, uint32_t ms)
{
    for (uint32_t t = 0u; t < ms; t += 5u) {
        if (*flag >= want) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return *flag >= want;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* lifecycle                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------- */
typedef struct {
    size_t bytes, blocks;
    UBaseType_t tasks;
    uint32_t timers;
} baseline_t;

static baseline_t snap(void)
{
    baseline_t b;
    bth_heap_stats_t h;
    bth_heap_stats(&h);
    b.bytes = h.live_bytes;
    b.blocks = h.live_blocks;
    b.tasks = uxTaskGetNumberOfTasks();
    b.timers = bth_timers_in_use();
    return b;
}

/* Self-deleted tasks are freed by the idle task: give it time, then compare. Returns 1 when back at baseline. */
static int settle_to(const baseline_t *base, baseline_t *now)
{
    for (int i = 0; i < 400; i++) {
        *now = snap();
        if (now->bytes == base->bytes && now->blocks == base->blocks && now->tasks == base->tasks &&
            now->timers == base->timers) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

static void check_controller_clean(const char *where)
{
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    bth_rom_lock();
    uint32_t rom_errors = fake_rom.errors;
    char first[160];
    memcpy(first, fake_rom.first_error, sizeof(first));
    bth_rom_unlock();
    CHECK(rom_errors == 0u, "%s: fake ROM contract error: %s", where, first);
    CHECK(s.cmd_unknown == 0u, "%s: %u unknown opcodes (last %04X)", where, s.cmd_unknown, s.unknown_opcode_last);
    CHECK(s.mandatory_fwd_fail == 0u, "%s: mandatory FWD failed", where);
    CHECK(s.tx_contract_violation == 0u && s.tx_malformed == 0u, "%s: host TX contract violation", where);
    CHECK(s.invariant == 0u && s.lock_violations == 0u, "%s: bt_ctrl invariant/lock violation", where);
    CHECK(bth_ctrl_faults() == 0u, "%s: BT fault posted (first code %u)", where, bth_ctrl_first_fault());
    CHECK(bth_hci_cmd_timeouts() == 0u, "%s: HCI command timeout (patch 0007)", where);
}

static esp_err_t do_init(void)
{
    CHECK(bth_ctrl_open() == 0, "bt_ctrl_open / attach");
    bth_step("esp_bluedroid_init", 20000u);
    esp_err_t e = esp_bluedroid_init();
    bth_step_done();
    return e;
}

static esp_err_t do_enable(void)
{
    bth_step("esp_bluedroid_enable", 20000u);
    esp_err_t e = esp_bluedroid_enable();
    bth_step_done();
    return e;
}

/* disable (if enabled) -> quiesce bt_ctrl deliveries -> deinit (contract §2: bt_dlv quiesced before deinit). */
static void do_teardown(void)
{
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        bth_step("esp_bluedroid_disable", 20000u);
        CHECK(esp_bluedroid_disable() == ESP_OK, "disable");
        bth_step_done();
    }
    bth_step("bt_ctrl close", 5000u);
    int st = bth_ctrl_close(4000u);
    bth_step_done();
    CHECK(st == BT_CTRL_STOP_OK, "bt_ctrl shutdown status %d", st);
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        bth_step("esp_bluedroid_deinit", 20000u);
        CHECK(esp_bluedroid_deinit() == ESP_OK, "deinit");
        bth_step_done();
    }
    CHECK(esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED, "status after deinit %d",
          (int)esp_bluedroid_get_status());
}

/* ------------------------------------------------------------------------------------------------------------- */
/* scenarios                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------- */
static void scenario_enable_gap_disable(const baseline_t *base)
{
    bth_log("[1] esp_bluedroid_init + enable\n");
    rom_log_clear();
    uint64_t t0 = mono_us();
    CHECK(do_init() == ESP_OK, "esp_bluedroid_init");
    CHECK(do_enable() == ESP_OK, "esp_bluedroid_enable");
    CHECK(esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED, "status %d", (int)esp_bluedroid_get_status());
    bth_log("    ENABLED after %u ms\n", (unsigned)((mono_us() - t0) / 1000u));
    check_controller_clean("enable");

    bth_log("[2] GAP: set device name + scan mode (connectable, general discoverable)\n");
    static const char k_name[] = "openfw-host";
    CHECK(esp_bt_gap_register_callback(gap_cb) == ESP_OK, "gap register");
    CHECK(esp_bt_gap_set_device_name(k_name) == ESP_OK, "set name");
    CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE) == ESP_OK, "set scan mode");
    s_name_evt = 0;
    s_name[0] = '\0';
    CHECK(esp_bt_gap_get_device_name() == ESP_OK, "get name");
    bth_step("GAP get name event", 5000u);
    CHECK(wait_until(&s_name_evt, 1, 4000u), "no ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT");
    bth_step_done();
    CHECK(strcmp(s_name, k_name) == 0, "get name returned '%s'", s_name);
    /* the commands reached the (fake) ROM: 0C13 Write Local Name (E025, name first) and 0C1A Write Scan Enable
     * (E02C, 0x03 = inquiry + page scan); BT_CTRL_OPCODE_TABLE.md §8 rows 0C13/0C1A. */
    int name_ok = 0, scan_ok = 0;
    for (int i = 0; i < 200 && !(name_ok && scan_ok); i++) {
        bth_rom_lock();
        const fake_cmd_rec_t *n = last_cmd(0xE025u);
        const fake_cmd_rec_t *s = last_cmd(0xE02Cu);
        name_ok = n != NULL && n->len >= sizeof(k_name) && memcmp(n->data, k_name, sizeof(k_name)) == 0;
        scan_ok = s != NULL && s->len >= 1u && s->data[0] == 0x03u;
        bth_rom_unlock();
        if (!(name_ok && scan_ok)) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    CHECK(name_ok, "0C13 with the new name did not reach the ROM");
    CHECK(scan_ok, "0C1A scan enable 0x03 did not reach the ROM");
    check_controller_clean("gap");

    bth_log("[3] disable + deinit\n");
    do_teardown();
    check_controller_clean("teardown");
    baseline_t now;
    CHECK(settle_to(base, &now), "after one cycle: heap %zu B / %zu blocks (base %zu / %zu), tasks %lu (base %lu), "
          "timers %u (base %u)", now.bytes, now.blocks, base->bytes, base->blocks, (unsigned long)now.tasks,
          (unsigned long)base->tasks, now.timers, base->timers);
}

static void scenario_cycles(const baseline_t *base, int n)
{
    bth_log("[4] %d x (init, enable, GAP scan mode, disable, deinit): heap/tasks/alarms back to baseline\n", n);
    bth_heap_stats_t h0;
    bth_heap_stats(&h0);
    uint64_t t0 = mono_us();
    for (int i = 0; i < n && g_fail == 0; i++) {
        CHECK(do_init() == ESP_OK, "cycle %d init", i);
        CHECK(do_enable() == ESP_OK, "cycle %d enable", i);
        CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE) == ESP_OK, "cycle %d scan", i);
        do_teardown();
        baseline_t now;
        CHECK(settle_to(base, &now), "cycle %d: heap %zu B / %zu blocks (base %zu / %zu), tasks %lu (base %lu), "
              "timers %u (base %u)", i, now.bytes, now.blocks, base->bytes, base->blocks, (unsigned long)now.tasks,
              (unsigned long)base->tasks, now.timers, base->timers);
        if ((i + 1) % 50 == 0) {
            bth_log("    %d cycles, %u ms\n", i + 1, (unsigned)((mono_us() - t0) / 1000u));
        }
    }
    check_controller_clean("cycles");
    bth_heap_stats_t h1;
    bth_heap_stats(&h1);
    bth_log("    %llu allocations, peak %zu B live\n", (unsigned long long)(h1.allocs - h0.allocs), h1.peak_bytes);
}

static bt_ctrl_stats_t ctrl_stats(void)
{
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    return s;
}

static uint32_t rom_cmd_count(uint16_t id)
{
    bth_rom_lock();
    uint32_t n = 0u;
    for (uint32_t i = 0; i < fake_rom.n_cmds; i++) {
        n += fake_rom.cmds[i].id == id;
    }
    bth_rom_unlock();
    return n;
}

static void inject_acl(int n)
{
    uint8_t data[64];
    for (int i = 0; i < (int)sizeof(data); i++) {
        data[i] = (uint8_t)(i * 3);
    }
    data[0] = 60;                       /* L2CAP basic header: length 60, CID 0x0040 (no channel: L2CAP drops it) */
    data[1] = 0;
    data[2] = 0x40;
    data[3] = 0;
    bth_rom_lock();
    for (int i = 0; i < n; i++) {
        CHECK(fake_rom_inject_acl(0x80, 2, data, sizeof(data)) == 0, "inject ACL %d", i);
    }
    bth_rom_unlock();
    bth_ctrl_kick();
}

/* Wait until bt_ctrl has received at least `want` C2H credits back from the host (0C35). */
static int wait_c2h_returned(uint32_t want, uint32_t ms)
{
    for (uint32_t t = 0u; t < ms; t += 2u) {
        if (ctrl_stats().c2h_returned >= want) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ctrl_stats().c2h_returned >= want;
}

/* Patch 0010 through the real stack (contract §13.4, plan M1 credit window): with zero host command credits (a
 * forwarded command's reply withheld by the ROM) and an ordinary command queued behind it, Bluedroid's C2H credit
 * return (0C35, NOACK) must still leave hciT, reach bt_ctrl, and resume inbound ACL; the queued command must stay
 * queued until the withheld reply arrives. ACL goes to handle 0x0080, which Bluedroid has no link for: L2CAP drops the
 * data and the credit is returned all the same. */
static void scenario_noack_zero_credits(const baseline_t *base)
{
    bth_log("[5] patch 0010: 0C35 leaves hciT with zero command credits and a command queued ahead of it\n");
    rom_log_clear();
    CHECK(do_init() == ESP_OK, "init");
    CHECK(do_enable() == ESP_OK, "enable");
    bth_rom_lock();
    fake_rom_link(0, true);
    bth_rom_unlock();
    bt_ctrl_stats_t s0 = ctrl_stats();

    bth_step("C2H credits for a full window", 5000u);
    inject_acl(4);                       /* a full window: Bluedroid flushes at C2H_CREDIT_FLUSH_MIN = 4 */
    CHECK(wait_c2h_returned(s0.c2h_returned + 4u, 3000u), "credits for 4 ACL packets not returned (%u)",
          ctrl_stats().c2h_returned - s0.c2h_returned);
    bth_step_done();

    bth_step("zero command credits", 5000u);
    bth_rom_lock();
    fake_rom.hold_replies = true;        /* the ROM swallows the next forwarded command's reply */
    bth_rom_unlock();
    uint32_t e02c_before = rom_cmd_count(0xE02Cu);
    CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE) == ESP_OK, "scan mode");
    for (int i = 0; i < 400 && rom_cmd_count(0xE02Cu) == e02c_before; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* BTM writes discoverability first (0C1A = 0x01, inquiry scan); the ROM withholds its reply, so the connectability
     * write (0x03) is already credit-blocked behind it. */
    int scan_held = rom_cmd_count(0xE02Cu) == e02c_before + 1u;
    uint32_t e025_before = rom_cmd_count(0xE025u);
    CHECK(scan_held, "the first 0C1A did not reach the ROM (or more than one did)");
    CHECK(esp_bt_gap_set_device_name("held-behind") == ESP_OK, "set name");
    vTaskDelay(pdMS_TO_TICKS(50));
    bt_ctrl_debug_t d;
    bt_ctrl_get_debug(&d);
    CHECK(d.rom_cmd_credits == 0u && d.fwd_pending == 1u, "ROM command credit not held (%u, %u)", d.rom_cmd_credits,
          d.fwd_pending);
    bth_step_done();

    bth_step("0C35 at zero command credits", 5000u);
    bt_ctrl_stats_t s1 = ctrl_stats();
    inject_acl(6);                       /* 4 delivered (window), 2 held by bt_ctrl until credits come back */
    CHECK(wait_c2h_returned(s1.c2h_returned + 6u, 3000u), "credits not returned at zero command credits: %u of 6 "
          "(patch 0010 missing?)", ctrl_stats().c2h_returned - s1.c2h_returned);
    bt_ctrl_stats_t s2 = ctrl_stats();
    CHECK(s2.dlv_acl - s1.dlv_acl == 6u, "inbound ACL did not resume: %u of 6 delivered", s2.dlv_acl - s1.dlv_acl);
    CHECK(s2.tx_noack > s1.tx_noack, "no NOACK command reached bt_ctrl");
    CHECK(s2.c2h_invalid == 0u && s2.c2h_excess == 0u && s2.c2h_malformed == 0u, "bad credit return");
    CHECK(rom_cmd_count(0xE025u) == e025_before, "0C13 overtook the withheld 0C1A reply (host credit window broken)");
    bth_step_done();

    bth_step("release the withheld reply", 5000u);
    bth_rom_lock();
    fake_rom.hold_replies = false;
    fake_rom_release_held();
    bth_rom_unlock();
    bth_ctrl_kick();
    int sent = 0;
    for (int i = 0; i < 400 && !sent; i++) {
        sent = rom_cmd_count(0xE025u) > e025_before;
        if (!sent) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    CHECK(sent, "queued 0C13 never left after the withheld reply was released");
    bth_step_done();
    check_controller_clean("noack");
    bth_rom_lock();
    fake_rom_link(0, false);
    bth_rom_unlock();
    do_teardown();
    baseline_t now;
    CHECK(settle_to(base, &now), "after the NOACK scenario: heap %zu B / %zu blocks (base %zu / %zu), tasks %lu, "
          "timers %u", now.bytes, now.blocks, base->bytes, base->blocks, (unsigned long)now.tasks, now.timers);
}

/* Patch 0009 directly (contract §13.2/§13.4): the vendored fixed_queue.c under the tracked allocator. A failed
 * list_append() must hand the enqueue_sem slot back, otherwise the queue shrinks by one slot per failure. The
 * allocation-failure sweep cannot see that: 0008 frees the copy and returns -1 either way, and losing 1 of the RX
 * queue's 254 slots changes nothing observable during enable. Capacity 2 and timeout 0 make every lost slot visible:
 * without 0009, the third failing enqueue already fails at the semaphore (2 injections, not 3) and, once the
 * allocator works again, the first enqueue finds no free slot. The capacity bound is checked too (the slot must come
 * back once, not twice). */
static void scenario_fixed_queue_slot(void)
{
    bth_log("[0] patch 0009: fixed_queue_enqueue() returns the slot when list_append() fails\n");
    static int items[3];
    bth_heap_stats_t h0, h1;
    bth_heap_stats(&h0);
    fixed_queue_t *q = fixed_queue_new(2u);
    CHECK(q != NULL, "fixed_queue_new(2)");
    if (q == NULL) {
        return;
    }
    bth_heap_stats_t ha, hb;
    bth_heap_stats(&ha);
    bth_heap_fail_arm(1u, true);            /* every allocation fails: list_append()'s node */
    bool r0 = fixed_queue_enqueue(q, &items[0], 0u);
    bool r1 = fixed_queue_enqueue(q, &items[1], 0u);
    bool r2 = fixed_queue_enqueue(q, &items[2], 0u);
    bth_heap_stats(&hb);
    bth_heap_fail_disarm();
    CHECK(!r0 && !r1 && !r2, "enqueue succeeded with the allocator failing (%d %d %d)", r0, r1, r2);
    CHECK(hb.injected - ha.injected == 3u, "%llu node allocations failed, want 3: the semaphore ran out of slots "
          "(patch 0009 missing?)", (unsigned long long)(hb.injected - ha.injected));
    CHECK(fixed_queue_length(q) == 0u, "length %zu after failed enqueues", fixed_queue_length(q));
    CHECK(fixed_queue_enqueue(q, &items[0], 0u), "first enqueue after the failures: no free slot (patch 0009 missing?)");
    CHECK(fixed_queue_enqueue(q, &items[1], 0u), "second enqueue after the failures: no free slot");
    CHECK(!fixed_queue_enqueue(q, &items[2], 0u), "third enqueue into a full capacity-2 queue succeeded (slot over-"
          "returned)");
    CHECK(fixed_queue_length(q) == 2u, "length %zu, want 2", fixed_queue_length(q));
    CHECK(fixed_queue_dequeue(q, 0u) == &items[0], "dequeue order (first)");
    CHECK(fixed_queue_dequeue(q, 0u) == &items[1], "dequeue order (second)");
    CHECK(fixed_queue_dequeue(q, 0u) == NULL, "dequeue from an empty queue");
    fixed_queue_free(q, NULL);
    bth_heap_stats(&h1);
    CHECK(h1.live_bytes == h0.live_bytes && h1.live_blocks == h0.live_blocks, "fixed_queue heap %zu B / %zu blocks "
          "(before %zu / %zu)", h1.live_bytes, h1.live_blocks, h0.live_bytes, h0.live_blocks);
}

/* Allocation attempts made while esp_bluedroid_enable() runs (all tasks). */
static uint64_t count_enable_allocs(void)
{
    CHECK(do_init() == ESP_OK, "init");
    bth_heap_count_start();
    CHECK(do_enable() == ESP_OK, "enable");
    uint64_t n = bth_heap_counted();
    do_teardown();
    return n;
}

/* One sweep point: fail the n-th allocation attempt of enable. Pass = completion or a clean failure, then a full
 * teardown with no leak, no fault, no contract error. */
static int sweep_one(const baseline_t *base, uint64_t n)
{
    CHECK(do_init() == ESP_OK, "init");
    bth_heap_fail_arm(n, false);
    esp_err_t e = do_enable();
    bth_heap_stats_t h;
    bth_heap_stats(&h);
    bth_heap_fail_disarm();
    /* A point beyond the attempts this run made tests nothing (run_sweep.py also rejects injected=0). */
    CHECK(h.injected == 1u, "sweep %llu: %llu allocations injected, want exactly 1", (unsigned long long)n,
          (unsigned long long)h.injected);
    const char *outcome = e == ESP_OK ? "ENABLED" : "CLEAN_FAIL";
    if (e == ESP_OK) {
        CHECK(esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED, "status %d", (int)esp_bluedroid_get_status());
    }
    check_controller_clean("sweep");
    do_teardown();
    baseline_t now;
    CHECK(settle_to(base, &now), "sweep %llu: heap %zu B / %zu blocks (base %zu / %zu), tasks %lu (base %lu), "
          "timers %u (base %u)", (unsigned long long)n, now.bytes, now.blocks, base->bytes, base->blocks,
          (unsigned long)now.tasks, (unsigned long)base->tasks, now.timers, base->timers);
    bth_log("SWEEP n=%llu injected=%llu ra=%p outcome=%s dlv_retries=%u fail=%d\n", (unsigned long long)n,
            (unsigned long long)h.injected, h.last_injected_ra, outcome, bth_dlv_retries(), g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* main                                                                                                           */
/* ------------------------------------------------------------------------------------------------------------- */
int s_argc;
char **s_argv;

extern char _end[];   /* end of .bss (GNU ld) */

void bth_test_task(void *arg)
{
    (void)arg;
    /* Makefile: -no-pie keeps the image below 4 GiB for Bluedroid's UINT32 timer parameters. */
    CHECK((uintptr_t)_end < ((uintptr_t)1 << 32), "image end %p above 4 GiB: build is not -no-pie", (void *)_end);
    bth_step("harness init", 10000u);
    CHECK(bth_port_init() == 0, "bth_port_init");
    CHECK(bth_ctrl_init() == 0, "bth_ctrl_init (opcode table validation)");
    bth_step_done();
    vTaskDelay(pdMS_TO_TICKS(20));
    baseline_t base = snap();
    const char *mode = s_argc > 1 ? s_argv[1] : "all";
    int rc;
    if (strcmp(mode, "sweep-count") == 0) {
        uint64_t n = count_enable_allocs();
        printf("ENABLE_ALLOCS %llu\n", (unsigned long long)n);
        rc = g_fail ? 1 : 0;
    } else if (strcmp(mode, "sweep-one") == 0 && s_argc > 2) {
        rc = sweep_one(&base, strtoull(s_argv[2], NULL, 0));
    } else if (strcmp(mode, "fixedq") == 0) {
        scenario_fixed_queue_slot();
        rc = g_fail ? 1 : 0;
    } else if (strcmp(mode, "noack") == 0) {
        scenario_noack_zero_credits(&base);
        rc = g_fail ? 1 : 0;
    } else if (strcmp(mode, "cycles") == 0 && s_argc > 2) {
        scenario_cycles(&base, atoi(s_argv[2]));
        rc = g_fail ? 1 : 0;
    } else {
        scenario_fixed_queue_slot();
        if (g_fail == 0) {
            scenario_enable_gap_disable(&base);
        }
        if (g_fail == 0) {
            scenario_noack_zero_credits(&base);
        }
        if (g_fail == 0) {
            scenario_cycles(&base, (strcmp(mode, "all") == 0 && s_argc > 2) ? atoi(s_argv[2]) : 200);
        }
        bth_log("test_bt_host: %d checks, %d failures\n", g_checks, g_fail);
        rc = g_fail ? 1 : 0;
    }
    fflush(stdout);
    fake_rom_shutdown();
    exit(rc);
}
