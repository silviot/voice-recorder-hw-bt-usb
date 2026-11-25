/* bt_bench firmware glue (BT=1 phone images): the SD request (bt_bench.c) turned into a BT-mode run. Hooks in
 * src/main.c: bt_bench_boot() before the home loop, bt_bench_begin() / bt_bench_tick() in run_bt_xrec_mode(),
 * bt_bench_end() after it. App task only. Log lines start with "BENCH ".
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "openfw.h"
#include "bt_bench.h"
#include "bt_ctrl.h"
#include "bt_xrec_bluedroid.h"
#include "bt_hp.h"
#include "openfw_bench_meas.h"
#include <string.h>

/* SRAM_HI (.bss_hi, zeroed at boot) like hci_probe.c's log copy: .bt_bss would come out of the RTOS heap region. */
#define BENCH_HI           __attribute__((section(".bss_hi"), aligned(4)))
#define BENCH_CAP_BYTES    8192u    /* ~100 lines; beyond that the head is kept here, the tail stays in the ring */

static char s_cap_buf[BENCH_CAP_BYTES] BENCH_HI;
static exfat_writer_t s_writer BENCH_HI;
static bt_bench_cap_t s_cap BENCH_HI;
static bt_bench_run_t s_run BENCH_HI;
static bt_bench_cfg_t s_cfg BENCH_HI;
static char s_line[512] BENCH_HI;        /* "BENCH meas" carries ~360 B of fields */
static uint32_t s_cap_ui[(sizeof(bt_hp_ui_t) + 3u) / 4u] BENCH_HI;   /* bt_hp_get_ui() scratch (off the stack) */
static uint32_t s_ll;
static uint8_t s_armed;          /* request taken this boot, run not ended yet */
static uint8_t s_in;             /* between bt_bench_begin() and bt_bench_end() */
static uint8_t s_why_time;       /* left BT mode because the time was up */
static volatile uint8_t s_ac_weak;   /* the weak bt_xrec_set_bench_autoconfirm() below was called */

/* Bench SSP auto-confirm override: provided by the phone-mode confirm UI (bt_xrec_bluedroid.c). This weak no-op
 * keeps images without it linking; the run then logs that the override is unavailable. Declared here only, so a
 * header with a different integer parameter type cannot clash with it. */
void bt_xrec_set_bench_autoconfirm(int on);
__attribute__((weak)) void bt_xrec_set_bench_autoconfirm(int on)
{
    (void)on;
    s_ac_weak = 1u;
}

/* Bench XREC PIN override (bt_xrec_bluedroid.c), same weak-default scheme. */
static volatile uint8_t s_pin_weak;
void bt_xrec_set_bench_xrec_pin(const char *pin6);
__attribute__((weak)) void bt_xrec_set_bench_xrec_pin(const char *pin6)
{
    (void)pin6;
    s_pin_weak = 1u;
}

/* ---- one log line, emitted whole (other tasks log too) ---- */
static void lc(char c)
{
    if (s_ll < sizeof(s_line) - 2u) {
        s_line[s_ll++] = c;
    }
}

static void ls(const char *s)
{
    while (*s != '\0') {
        lc(*s++);
    }
}

static void lu(uint32_t v)
{
    char b[10];
    int i = 0;
    do {
        b[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && i < 10);
    while (i > 0) {
        lc(b[--i]);
    }
}

static void lku(const char *k, uint32_t v)
{
    lc(' ');
    ls(k);
    lc('=');
    lu(v);
}

static void lks(const char *k, const char *v)
{
    lc(' ');
    ls(k);
    lc('=');
    ls(v);
}

static void lkx(const char *k, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    lc(' ');
    ls(k);
    ls("=0x");
    for (int i = 7; i >= 0; i--) {
        lc(hx[(v >> (4 * i)) & 0xFu]);
    }
}

static void lbegin(const char *tag)
{
    s_ll = 0u;
    ls("BENCH ");
    ls(tag);
}

static void lemit(void)
{
    s_line[s_ll++] = '\n';
    s_line[s_ll] = '\0';
    taskENTER_CRITICAL();
    uint32_t w = g_openfw_log.write_idx;
    if (w != 0u && g_openfw_log.buf[(w - 1u) & (OPENFW_LOG_SIZE - 1u)] != '\n') {
        openfw_log_putc('\n');               /* another task was mid-line: keep "BENCH " at a line start */
    }
    openfw_log_puts(s_line);
    taskEXIT_CRITICAL();
}

static void drain(void)
{
    bt_bench_cap_drain(&s_cap, g_openfw_log.buf, OPENFW_LOG_SIZE, g_openfw_log.write_idx);
}

/* ---- per-task CPU over a window (run-time stats clock: us_timer, FreeRTOSConfig.h) ----
 * FreeRTOS charges an ISR to the task it interrupted. A window's first take only sets the baseline (dt = 0). */
#define BENCH_TASKS_MAX 16u
typedef struct {
    struct {
        uint32_t num, rt;
    } prev[BENCH_TASKS_MAX];
    uint32_t n;
    uint32_t prev_us;
} cpu_win_t;

static TaskStatus_t s_ts[BENCH_TASKS_MAX] BENCH_HI;   /* app task only (both runs), off the stack */
static uint32_t s_td[BENCH_TASKS_MAX] BENCH_HI;

typedef struct {
    UBaseType_t n;          /* tasks in s_ts / s_td */
    uint32_t dt;            /* window length, us (0 = baseline only) */
    uint32_t sum;           /* run time charged to the tasks in the window */
    uint32_t idle, btc, app;
} cpu_take_t;

static void cpu_win_take(cpu_win_t *w, cpu_take_t *o)
{
    o->n = uxTaskGetSystemState(s_ts, BENCH_TASKS_MAX, NULL);
    uint32_t now = (uint32_t)us_timer_now();
    o->dt = w->prev_us != 0u ? now - w->prev_us : 0u;
    o->sum = o->idle = o->btc = o->app = 0u;
    for (UBaseType_t i = 0; i < o->n; i++) {
        uint32_t rt = (uint32_t)s_ts[i].ulRunTimeCounter, prev = rt;
        for (uint32_t j = 0u; j < w->n; j++) {
            if (w->prev[j].num == (uint32_t)s_ts[i].xTaskNumber) {
                prev = w->prev[j].rt;
                break;
            }
        }
        s_td[i] = rt - prev;
        o->sum += s_td[i];
        if (s_ts[i].xHandle == xTaskGetIdleTaskHandle()) {
            o->idle = s_td[i];
        } else if (strcmp(s_ts[i].pcTaskName, "BTC_TASK") == 0) {
            o->btc = s_td[i];
        } else if (strcmp(s_ts[i].pcTaskName, "app") == 0) {
            o->app = s_td[i];
        }
    }
    w->n = 0u;
    for (UBaseType_t i = 0; i < o->n && i < BENCH_TASKS_MAX; i++) {
        w->prev[i].num = (uint32_t)s_ts[i].xTaskNumber;
        w->prev[i].rt = (uint32_t)s_ts[i].ulRunTimeCounter;
        w->n++;
    }
    w->prev_us = now != 0u ? now : 1u;
}

static uint32_t pm_of(uint32_t v, uint32_t dt)
{
    return dt != 0u ? (uint32_t)((uint64_t)v * 1000u / dt) : 0u;
}

static void lk_task(const char *name, uint32_t v)
{
    char k[9];
    uint32_t i = 0u;
    for (; i < 8u && name[i] != '\0'; i++) {
        char c = name[i];
        k[i] = (c == ' ' || c == '=') ? '_' : c;
    }
    k[i] = '\0';
    lku(k, v);
}

/* "BENCH <tag> name=pm ... other=pm" (continued as "BENCH <tag>+") for the window just taken. */
static void log_tasks(const char *tag, const cpu_take_t *c)
{
    lbegin(tag);
    for (UBaseType_t i = 0; i < c->n; i++) {
        if (s_ll > 120u) {
            lemit();
            lbegin(tag);
            lc('+');
        }
        lk_task(s_ts[i].pcTaskName, pm_of(s_td[i], c->dt));
    }
    uint32_t sum_pm = pm_of(c->sum, c->dt);
    lku("other", sum_pm < 1000u ? 1000u - sum_pm : 0u);
    lemit();
}

/* ---- phone-mode measurement (docs/BT_SYNC_SPEED_AND_RECORDING.md F0) ----
 * Every second: a 1 s CPU window into g_bench_meas (the +XREC:DBG idle/app/btc values and the recording's lowest
 * idle). It reads only the idle, app and BTC_TASK run-time counters (vTaskGetInfo without the stack high-water scan,
 * no scheduler suspension), and its own duration goes into take_us: the measurement must not be a 1 Hz load that
 * only the BT captures carry. With every BENCH_MEAS_EVERY-th status line (2.5 min) and at the end: "BENCH cpu" +
 * "BENCH tasks" over that window (the full uxTaskGetSystemState) and "BENCH meas" (the recording counters, live):
 * the 8 KiB capture keeps the head of the run, so fewer lines leave room for more of it. Whole-recording checks use
 * the counters (BENCH rec, +XREC:DBG), never the log text. */
#define BENCH_CPU1_MS     1000u
#define BENCH_MEAS_EVERY  5u
static cpu_win_t s_win30 BENCH_HI;
static uint32_t s_cpu1_next BENCH_HI;
static uint32_t s_status_n BENCH_HI;
static struct {
    TaskHandle_t btc;           /* BTC_TASK, looked up once it exists */
    uint32_t idle, app, btc_rt; /* run-time counters at the window start */
    uint32_t prev_us;           /* 0 = no baseline yet */
} s_win1 BENCH_HI;

/* Append the F0 fields to the line being built (formatted in place: no second buffer in the tight .bss_hi). */
static void lmeas_rec(const bench_meas_rec_t *r)
{
    s_ll += bench_meas_format_rec(r, ' ', &s_line[s_ll], (uint32_t)(sizeof(s_line) - 2u - s_ll));
}

static void lmeas_cpu(void)
{
    s_ll += bench_meas_format_cpu(&g_bench_meas, ' ', &s_line[s_ll], (uint32_t)(sizeof(s_line) - 2u - s_ll));
}

static uint32_t task_rt(TaskHandle_t h)
{
    TaskStatus_t st;
    if (h == NULL) {
        return 0u;
    }
    vTaskGetInfo(h, &st, pdFALSE, eInvalid);   /* pdFALSE: no stack scan; eInvalid: state read in a short critical */
    return (uint32_t)st.ulRunTimeCounter;
}

static void meas_cpu1(uint32_t now)
{
    if ((int32_t)(now - s_cpu1_next) < 0) {
        return;
    }
    s_cpu1_next = now + BENCH_CPU1_MS;
    uint32_t t0 = (uint32_t)us_timer_now();
    if (s_win1.btc == NULL) {
        s_win1.btc = xTaskGetHandle("BTC_TASK");   /* a list walk: only until the task exists */
        s_win1.prev_us = 0u;                       /* its counter starts now: new baseline */
    }
    uint32_t idle = task_rt(xTaskGetIdleTaskHandle());
    uint32_t app = task_rt(xTaskGetCurrentTaskHandle());   /* app task only */
    uint32_t btc = task_rt(s_win1.btc);
    uint32_t t1 = (uint32_t)us_timer_now();
    if (s_win1.prev_us != 0u) {
        uint32_t dt = t1 - s_win1.prev_us;
        bench_meas_cpu(&g_bench_meas, pm_of(idle - s_win1.idle, dt), pm_of(app - s_win1.app, dt),
                       pm_of(btc - s_win1.btc_rt, dt), dt / 1000u);
    }
    s_win1.idle = idle;
    s_win1.app = app;
    s_win1.btc_rt = btc;
    s_win1.prev_us = t1 != 0u ? t1 : 1u;
    bench_meas_cpu_take(&g_bench_meas, t1 - t0);
}

static void log_meas(void)
{
    lbegin("meas ");
    lmeas_rec(&g_bench_meas.cur);
    lemit();
    lbegin("meas+ ");
    lmeas_cpu();
    lemit();
}

static void log_cpu30(void)
{
    cpu_take_t c;
    uint32_t t0 = (uint32_t)us_timer_now();
    cpu_win_take(&s_win30, &c);
    uint32_t take_us = (uint32_t)us_timer_now() - t0;
    if (c.dt == 0u) {
        return;
    }
    lbegin("cpu");
    lku("ms", c.dt / 1000u);
    lku("take_us", take_us);                 /* this full take's own cost (stack scans, scheduler suspended) */
    lku("idle_pm", pm_of(c.idle, c.dt));
    lku("app_pm", pm_of(c.app, c.dt));
    lku("btc_pm", pm_of(c.btc, c.dt));
    lku("rec", g_bench_meas.cur.active);
    lemit();
    log_tasks("tasks", &c);
}

static void log_status(uint32_t el_ms, int last)
{
    bt_status_t st;
    bt_xrec_get_status(&st);
    lbegin("status");
    lku("t", el_ms / 1000u);
    lku("acl", st.acl_connected);
    lku("spp", st.spp_open);
    lku("bound", st.xrec_bound);
    lku("auth", st.xrec_auth);
    lku("pair", st.pair_active);
    lku("rx", st.rx_acl_pkts);
    lku("tx", st.tx_acl_pkts);
    lku("pairs", s_run.pairs);
    static bt_xrec_debug_t dbg BENCH_HI;    /* app task only, off the stack */
    bt_xrec_get_debug(&dbg);
    lku("links", dbg.acl_links);             /* acl= is the screen's "connected" (hides links the host is closing): */
    lku("closing", dbg.acl_closing);         /* links= / closing= keep the raw ACL picture (BT_ROM_HCI_MAP §11 H1/H2) */
    lemit();
    static char cl[BT_CTRL_STATS_LINE] BENCH_HI;
    (void)bt_ctrl_stats_format(cl, (uint16_t)sizeof(cl));
    lbegin(cl);                              /* "BENCH BT ctrl stats unk=.. short=.. ...": bt_ctrl's silent drops */
    lemit();
    if (last || (s_status_n++ % BENCH_MEAS_EVERY) == 0u) {
        log_cpu30();
        log_meas();
    }
}

static void act(uint32_t a, uint32_t now)
{
    uint32_t el = now - s_run.t0;
    if (a & BT_BENCH_ACT_PAIR) {
        bt_xrec_open_pairing((uint32_t)s_cfg.pair_secs * 1000u);
        lbegin("pair");
        lku("n", s_run.pairs);
        lku("t", el / 1000u);
        lku("secs", s_cfg.pair_secs);
        lemit();
    }
    if (a & BT_BENCH_ACT_STATUS) {
        log_status(el, 0);
    }
}

/* ============================================ headphones run (mode=hp) ========================================= */
#include "bt_hp.h"
#include "hp_tx.h"
#include "hp_tx_bd.h"
#include "bt_rom_real.h"
#include "bt_ctrl_rtos.h"
#ifndef OPENFW_BT_BLUEDROID
#define OPENFW_BT_BLUEDROID 1                /* BT=1 only; the bt_port library is built without the app's -D */
#endif
#include "audio_out.h"

enum { HPB_WAIT_READY = 0, HPB_SCANNING, HPB_CONNECTING, HPB_CONNECTED, HPB_STREAMING, HPB_PLAY_DUE, HPB_DONE };

static struct {
    uint8_t  st;
    uint8_t  scanned;
    uint8_t  tone_on;
    uint8_t  played;
    uint8_t  last_link;
    uint32_t last_msg_seq;
    uint32_t next_stat, stat_n, t_tone;
    uint32_t phase;
    uint32_t prev_pkts, prev_frames, prev_acked, prev_pads, prev_el_ms;
    uint32_t prev_us, prev_late, prev_ticks, prev_sbc, prev_irq, prev_ui_fr;
    uint32_t prev_runs, prev_run_us, prev_svc_us, prev_disp_us, prev_disp_n, prev_more;
} s_hp BENCH_HI;
static char s_hp_play_path[BT_BENCH_HP_FILE_MAX + 1u] BENCH_HI;

static const int16_t k_sin64[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329, 23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12539, -15446, -18204, -20787, -23170, -25329, -27245, -28898, -30273, -31356, -32137,
    -32609, -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329, -23170, -20787, -18204, -15446, -12539,
    -9512, -6393, -3212,
};

static void lka(const char *k, const uint8_t b[6])
{
    static const char hx[] = "0123456789ABCDEF";
    lc(' ');
    ls(k);
    ls("=..:");
    for (int i = 3; i < 6; i++) {
        lc(hx[b[i] >> 4]);
        lc(hx[b[i] & 0xFu]);
        if (i < 5) {
            lc(':');
        }
    }
}

static void hp_boot_log(void)
{
    lbegin("start");
    lks("mode", "hp");
    lks("stage", bt_bench_hp_stage_name(s_cfg.hp_stage));
    lku("secs", s_cfg.secs);
    lku("hp_secs", s_cfg.hp_secs);
    lku("pick_first", s_cfg.hp_pick_first);
    lku("pick_last", s_cfg.hp_pick_last);
    lku("addr_set", s_cfg.hp_addr_set);
    if (s_cfg.hp_addr_set) {
        lka("addr", s_cfg.hp_addr);
    }
    lku("pair", s_cfg.hp_pair);
    lku("rate", s_cfg.hp_rate);
    lku("bitpool", s_cfg.hp_bitpool);
    lku("speech_bp", s_cfg.hp_speech_bp);
    lku("tick_ms", s_cfg.hp_tick_ms);
    lku("tone_hz", s_cfg.hp_tone_hz);
    lku("credits", s_cfg.hp_credits);
    lks("file", s_cfg.hp_file[0] ? s_cfg.hp_file : "-");
    lkx("prev_fault", g_prev_fault_code);
    lks("log", BT_BENCH_LOG_FILE);
    lemit();
    if (s_cfg.hp_credits != BT_CTRL_H2C_ACL_CREDITS) {
        lbegin("hp");
        lks("note", "hp_credits differs from this image's build-time credit count (not applied, design §7)");
        lku("image", BT_CTRL_H2C_ACL_CREDITS);
        lemit();
    }
    if (s_cfg.hp_pscan) {
        lbegin("hp");
        lks("note", "hp_pscan=1 ignored: this session never enables page scan (live log)");
        lemit();
    }
}

/* Before bt_hp_start(): the SD request's tunables. */
void bt_bench_hp_prepare(void)
{
    bt_hp_bench_tun_t t;
    memset(&t, 0, sizeof(t));
    t.rate = s_cfg.hp_rate;
    t.bitpool = s_cfg.hp_bitpool;
    t.speech_bp = s_cfg.hp_speech_bp;
    t.tick_ms = s_cfg.hp_tick_ms;
    t.pkt_per_tick = s_cfg.hp_pkt_per_tick;
    t.txq = s_cfg.hp_txq;
    t.credit_ticks = s_cfg.hp_credit_ticks;
    t.pscan = s_cfg.hp_pscan;
    t.pin0000 = s_cfg.hp_pin;
    bt_hp_set_bench_tun(&t);
}

void bt_bench_hp_begin(void)
{
    if (!s_armed || s_in) {
        return;
    }
    s_in = 1u;
    memset(&s_hp, 0, sizeof(s_hp));
    s_win30.prev_us = 0u;                    /* the first CPU window of the run is a baseline */
    uint32_t now = millis();
    bt_bench_run_begin(&s_run, &s_cfg, now);
    s_run.next_pair = UINT32_MAX;
    drain();
}

static void hp_found_log(void)
{
    bt_hp_ui_t *u = (bt_hp_ui_t *)s_cap_ui;
    bt_hp_get_ui(u);
    lbegin("hp scan");
    lku("found", u->n_found);
    lemit();
    for (uint32_t i = 0u; i < u->n_found; i++) {
        const bt_hp_found_t *f = &u->found[i];
        lbegin("hp dev");
        lku("i", i);
        lka("addr", f->bda);
        lkx("cod", f->cod);
        lku("audio", f->is_audio);
        if (f->has_rssi) {
            lks("rssi", "-");
            lu((uint32_t)(f->rssi < 0 ? -(int32_t)f->rssi : (int32_t)f->rssi));
        }
        lks("name", f->named ? f->name : "?");
        lemit();
    }
}

/* CPU over the stats interval (run-time stats clock: us_timer, FreeRTOSConfig.h), permille of wall time.
 * "hp cpu": idle, the BTC task (hp_tx tick: resampler + SBC), the app task (player), BTC microseconds per SBC frame,
 * late = BTC ticks past the credit clamp (air time lost), ticks run, RW-BT IRQ 26 entries, CPU1 UI frames and the
 * last frame's render time. "hp tasks": every task (FreeRTOS charges an ISR to the task it interrupted; the only
 * busy ISRs here are SysTick and the IRQ 26 top half, which is in RAM and short), then other = 1000 - sum (time not
 * charged to any task: counter wrap or a task created/deleted in the interval).
 * Silicon 2026-11-25: the player stage ran BTC at ~46 % with ~45 % of the CPU outside BTC and the app. */

static void hp_cpu_stats(const hp_tx_t *t)
{
    cpu_take_t c;
    cpu_win_take(&s_win30, &c);              /* a headphones run never runs the phone-mode windows: shared */
    UBaseType_t n = c.n;
    uint32_t dt = c.dt, idle = c.idle, btc = c.btc, app = c.app;   /* dt = 0: first window of the run */
    bt_rom_real_diag_t rd;
    bt_rom_real_diag(&rd);
    bt_ctrl_rtos_stats_t cs;
    bt_ctrl_rtos_stats(&cs);
    uint32_t sbc = t->st.frames_sbc - s_hp.prev_sbc, ui_fr = g_ui_frames;
    if (dt != 0u) {
        lbegin("hp cpu");
        lku("ms", dt / 1000u);
        lku("idle_pm", (uint32_t)((uint64_t)idle * 1000u / dt));
        lku("btc_pm", (uint32_t)((uint64_t)btc * 1000u / dt));
        lku("app_pm", (uint32_t)((uint64_t)app * 1000u / dt));
        lku("btc_us_sbc", sbc ? btc / sbc : 0u);
        lku("late", t->st.tick_late - s_hp.prev_late);
        lku("ticks", t->st.ticks - s_hp.prev_ticks);
        lku("irq26", rd.irq_total - s_hp.prev_irq);
        lku("ui_fr", ui_fr - s_hp.prev_ui_fr);
        lku("ui_us", g_ui_last_render_us);
        lku("xip", g_openfw_xip_mode);
        lemit();
        log_tasks("hp tasks", &c);
        /* bt_ctrl split: its task time = ROM ISR work (irq_service) + ROM kernel dispatches (incl. TASK_HCI upcalls)
         * + our glue (the rest: bottom half, rings, ACL/event paths, FreeRTOS calls). us per interval, per run. */
        uint32_t runs = cs.ctrl_runs - s_hp.prev_runs, run_us = cs.ctrl_run_total_us - s_hp.prev_run_us;
        uint32_t isr = rd.svc_sum_us - s_hp.prev_svc_us, ke = rd.disp_sum_us - s_hp.prev_disp_us;
        lbegin("hp ctrl");
        lku("runs", runs);
        lku("run_us", run_us);
        lku("rom_isr_us", isr);
        lku("rom_ke_us", ke);
        lku("glue_us", run_us > isr + ke ? run_us - isr - ke : 0u);
        lku("disp", rd.disp_count - s_hp.prev_disp_n);
        lku("us_run", runs ? run_us / runs : 0u);
        lku("more", cs.ctrl_more - s_hp.prev_more);
        lku("streak_max", cs.more_streak_max);
        lku("streak_us", cs.more_streak_max_us);
        lemit();
        /* review A11: stack high-water marks (free words, lowest seen) every 5th line */
        if ((s_hp.stat_n % 5u) == 0u) {
            lbegin("hp stack");
            for (UBaseType_t i = 0; i < n; i++) {
                if (s_ll > 120u) {
                    lemit();
                    lbegin("hp stack+");
                }
                lku(s_ts[i].pcTaskName, (uint32_t)s_ts[i].usStackHighWaterMark);
            }
            lemit();
        }
    }
    s_hp.prev_late = t->st.tick_late;
    s_hp.prev_ticks = t->st.ticks;
    s_hp.prev_sbc = t->st.frames_sbc;
    s_hp.prev_irq = rd.irq_total;
    s_hp.prev_ui_fr = ui_fr;
    s_hp.prev_runs = cs.ctrl_runs;
    s_hp.prev_run_us = cs.ctrl_run_total_us;
    s_hp.prev_more = cs.ctrl_more;
    s_hp.prev_svc_us = rd.svc_sum_us;
    s_hp.prev_disp_us = rd.disp_sum_us;
    s_hp.prev_disp_n = rd.disp_count;
}

static void hp_stats(uint32_t el_ms)
{
    hp_tx_t *t = hp_tx_bd();
    bt_hp_ui_t *u = (bt_hp_ui_t *)s_cap_ui;
    bt_hp_get_ui(u);
    lbegin("hp");
    lku("t", el_ms / 1000u);
    lku("link", u->link);
    lku("rate", t->conn_rate);
    lku("bp", t->cfg.max_bp);
    /* on air: the speech bitpool for a source <= 16 kHz (the tone stage: hp_speech_bp=0 for bp), payload bytes/s */
    uint8_t bp_eff = t->cfg_valid ? hp_tx_packet_bitpool(t) : 0u;
    lku("bp_eff", bp_eff);
    lku("fpp", t->fpp);
    lku("pkts", t->st.pkts - s_hp.prev_pkts);
    uint32_t dt_ms = el_ms - s_hp.prev_el_ms ? el_ms - s_hp.prev_el_ms : 1000u;
    lku("Bps", t->cfg_valid ? (uint32_t)((uint64_t)(t->st.frames_sbc - s_hp.prev_frames) *
                                         hp_sbc_frame_len(&t->cfg, bp_eff) * 1000u / dt_ms) : 0u);
    lku("sbc", t->st.frames_sbc - s_hp.prev_frames);
    lku("acked", t->acked_sbc - s_hp.prev_acked);
    lku("pads", t->st.pad_pkts - s_hp.prev_pads);
    lku("lost", t->st.tx_lost_sbc);
    lku("drop", t->st.flush_drop_sbc);
    lku("allocf", t->st.alloc_fail);
    lku("cs_no", t->st.can_send_no);
    lku("txq", t->last_st.qlen);
    lku("txq_max", t->st.txq_max);
    lku("down", hp_tx_down_sbc(t));
    lku("nocp_max_ms", t->st.nocp_gap_max_ms);
    lku("stalls", t->st.stalls);
    lku("late", t->st.tick_late);
    lemit();
    s_hp.prev_pkts = t->st.pkts;
    s_hp.prev_frames = t->st.frames_sbc;
    s_hp.prev_acked = t->acked_sbc;
    s_hp.prev_pads = t->st.pad_pkts;
    s_hp.prev_el_ms = el_ms;
    uint16_t h = bt_hp_acl_handle();
    bt_ctrl_link_info_t li;
    if (h != 0xFFFFu && bt_ctrl_link_info(h, &li)) {
        lbegin("hp linkinfo");
        lks("role", li.role == BT_CTRL_ROLE_MASTER ? "M" : li.role == BT_CTRL_ROLE_SLAVE ? "S" : "?");
        lkx("pkt_type", li.pkt_type);
        lkx("req_pt", li.req_pkt_type);
        lku("max_slots", li.max_slots);
        lku("edr", li.rmt_feat_seen ? (uint32_t)((li.rmt_feat3 >> 1) & 3u) : 9u);   /* 9 = features not seen */
        lku("mode", li.mode);
        lku("acl_sent", li.acl_sent);
        lku("acl_done", li.acl_completed);
        lku("heap_free", (uint32_t)xPortGetFreeHeapSize());
        lku("heap_min", (uint32_t)xPortGetMinimumEverFreeHeapSize());
        lemit();
    }
    hp_cpu_stats(t);
}

/* Tone through the full output path (aout -> hp_sink ring -> resampler -> SBC), 16 kHz mono. */
static void hp_tone_feed(uint32_t now)
{
    int16_t buf[128];
    for (int round = 0; round < 8; round++) {
        uint32_t hz = s_cfg.hp_tone_hz;
        if (hz == 0u) {
            hz = (((now - s_hp.t_tone) / 1000u) & 1u) ? 880u : 440u;
        }
        uint32_t step = (hz << 16) / 16000u * 64u;       /* phase step, 64-entry table in 16.16 */
        uint32_t ph = s_hp.phase;
        for (int i = 0; i < 128; i++) {
            buf[i] = (int16_t)(k_sin64[(ph >> 16) & 63u] / 3);   /* -9.5 dBFS */
            ph += step;
        }
        int w = aout_write(buf, 128u, 1u);
        if (w <= 0) {
            break;
        }
        uint32_t adv = step * (uint32_t)w;
        s_hp.phase += adv;
    }
}

/* In the headphones bench loop (main.c): returns BT_BENCH_HP_ACT_* bits; *path gets the file for PLAY. */
int bt_bench_hp_tick(uint32_t now, char *path, uint32_t cap)
{
    if (!s_in) {
        return 0;
    }
    uint32_t el = now - s_run.t0;
    int act = 0;
    bt_hp_ui_t *u = (bt_hp_ui_t *)s_cap_ui;
    bt_hp_get_ui(u);
    if (u->msg_seq != s_hp.last_msg_seq) {
        s_hp.last_msg_seq = u->msg_seq;
        lbegin("hp msg");
        lku("msg", u->msg);
        lku("link", u->link);
        lemit();
    }
    if (u->link != s_hp.last_link) {
        lbegin("hp link");
        lku("from", s_hp.last_link);
        lku("to", u->link);
        lku("t_ms", el);
        lemit();
        s_hp.last_link = u->link;
    }
    if (u->state == BT_HP_ST_FAULTED || u->state == BT_HP_ST_OFF) {
        if (s_hp.st != HPB_DONE) {
            lbegin("hp");
            lks("st", "session ended (fault or off): see the BT hp lines above");
            lemit();
            s_hp.st = HPB_DONE;
        }
    }
    switch (s_hp.st) {
    case HPB_WAIT_READY:
        if (u->state == BT_HP_ST_READY) {
            lbegin("hp ready");
            lku("t_ms", el);
            lku("last", u->has_last);
            lemit();
            if (s_cfg.hp_stage == BT_BENCH_HP_SCAN || s_cfg.hp_pick_first) {
                if (bt_hp_scan(s_cfg.hp_secs) == 0) {
                    s_hp.st = HPB_SCANNING;
                } else {
                    lbegin("hp");
                    lks("err", "scan refused");
                    lemit();
                    s_hp.st = HPB_DONE;
                }
            } else if (s_cfg.hp_pick_last) {
                if (!u->has_last) {
                    lbegin("hp");
                    lks("err", "hp_pick=last: no stored headset (run a connect stage with hp_pair=1 first)");
                    lemit();
                    s_hp.st = HPB_DONE;
                    break;
                }
                lbegin("hp pick");
                lks("from", "last");
                lka("addr", u->last);
                lks("name", u->last_name[0] != '\0' ? u->last_name : "?");
                lemit();
                (void)bt_hp_connect_addr(u->last, 0);   /* usually already reconnecting (READY starts it) */
                s_hp.st = HPB_CONNECTING;
            } else {
                if (s_cfg.hp_pair) {
                    lbegin("hp pair window");
                    lks("note", "bench, no user: Just Works for this address only, 120 s");
                    lemit();
                }
                (void)bt_hp_connect_addr(s_cfg.hp_addr, s_cfg.hp_pair);
                s_hp.st = HPB_CONNECTING;
            }
        }
        break;
    case HPB_SCANNING:
        if (!u->scanning) {
            hp_found_log();
            if (s_cfg.hp_stage == BT_BENCH_HP_SCAN) {
                s_hp.st = HPB_DONE;
                break;
            }
            uint32_t cods[BT_HP_FOUND_MAX];
            for (uint32_t i = 0u; i < u->n_found; i++) {
                cods[i] = u->found[i].cod;
            }
            int pick = bt_bench_hp_pick(cods, u->n_found);   /* AV major class, headset types first */
            if (pick < 0) {
                lbegin("hp");
                lks("err", "no AV-class device found (put the headphones in pairing mode; phones/PCs are never picked)");
                lemit();
                s_hp.st = HPB_DONE;
                break;
            }
            lbegin("hp pick");
            lku("i", (uint32_t)pick);
            lka("addr", u->found[pick].bda);
            lks("name", u->found[pick].named ? u->found[pick].name : "?");
            lku("pair", s_cfg.hp_pair);
            lemit();
            if (s_cfg.hp_pair) {
                lbegin("hp pair window");
                lks("note", "bench, no user: Just Works for this address only, 120 s");
                lemit();
            }
            (void)bt_hp_pick((uint8_t)pick, s_cfg.hp_pair);
            s_hp.st = HPB_CONNECTING;
        }
        break;
    case HPB_CONNECTING:
        if (u->link == BT_HP_LINK_OPEN || u->link == BT_HP_LINK_STARTED) {
            lbegin("hp connected");
            lku("t_ms", el);
            lku("rate", u->rate);
            lku("bp", u->bp_max);
            lku("abs_vol", u->abs_vol);
            lks("name", u->target_name[0] ? u->target_name : "?");
            lemit();
            s_hp.st = HPB_CONNECTED;
        }
        break;
    case HPB_CONNECTED:
        if (s_cfg.hp_stage == BT_BENCH_HP_TONE) {
            aout_set_mode(AOUT_MODE_HEADPHONES);
            aout_set_src_channels(1u);
            int r = aout_start(16000u, 24u);
            lbegin("hp tone");
            lku("hz", s_cfg.hp_tone_hz);
            lku("start", (uint32_t)(r == 0));
            lemit();
            s_hp.t_tone = now;
            s_hp.tone_on = r == 0;
            s_hp.next_stat = el + 1000u;
            s_hp.st = HPB_STREAMING;
        } else if (s_cfg.hp_stage == BT_BENCH_HP_PLAYER) {
            s_hp.st = HPB_PLAY_DUE;
        } else {
            s_hp.next_stat = el + 1000u;
            s_hp.st = HPB_STREAMING;   /* connect stage: hold the link, stats only */
        }
        break;
    case HPB_PLAY_DUE:
        if (!s_hp.played) {
            s_hp.played = 1u;
            const char *f = s_cfg.hp_file;
            uint32_t i = 0u;
            for (; f[i] != '\0' && i + 1u < cap; i++) {
                path[i] = f[i];
            }
            path[i] = '\0';
            act |= BT_BENCH_HP_ACT_PLAY;
            s_hp.next_stat = el + 1000u;
            s_hp.st = HPB_STREAMING;
        }
        break;
    case HPB_STREAMING:
        if (s_hp.tone_on) {
            hp_tone_feed(now);
        }
        break;
    default:
        break;
    }
    if (s_hp.st == HPB_STREAMING && (int32_t)(el - s_hp.next_stat) >= 0) {
        hp_stats(el);
        s_hp.stat_n++;
        s_hp.next_stat = el + (s_hp.stat_n < 15u ? 1000u : 15000u);
    }
    uint32_t a = bt_bench_run_tick(&s_run, now);
    if (a & BT_BENCH_ACT_EXIT) {
        if (!s_why_time) {
            s_why_time = 1u;
            hp_stats(el);
            lbegin("exit");
            lks("why", "time");
            lku("ms", el);
            lemit();
        }
        act |= BT_BENCH_HP_ACT_EXIT;
    }
    drain();
    return act;
}

/* After the player stage's file (main.c) and at the end of the run: tone off, output mode back. */
void bt_bench_hp_finish(uint8_t out_mode)
{
    if (s_hp.tone_on) {
        aout_stop();
        s_hp.tone_on = 0u;
    }
    aout_set_src_channels(2u);
    aout_set_mode((aout_mode_t)out_mode);
    bt_hp_set_bench_tun(NULL);           /* the run's tuning (PIN 0000) ends with it (STEP10 C17) */
}

int bt_bench_boot(exfat_vol_t *vol, int sd_mounted)
{
    bt_bench_err_t err = BT_BENCH_OK;
    uint32_t at = 0u;
    bt_bench_trig_t t = bt_bench_take(vol, sd_mounted, &s_cfg, &err, &at);
    if (t != BT_BENCH_TRIG_TAKEN) {
        lbegin("OFF");
        lks("trig", bt_bench_trig_name(t));
        if (t == BT_BENCH_TRIG_BAD) {
            lks("err", bt_bench_err_name(err));
            lku("at", at);
        }
        lks("file", BT_BENCH_TRIGGER_FILE);
        lemit();
        return 0;
    }
    bt_bench_cap_init(&s_cap, s_cap_buf, sizeof(s_cap_buf), g_openfw_log.write_idx);
    if (s_cfg.mode == BT_BENCH_MODE_HP) {
        hp_boot_log();
        s_armed = 1u;
        s_in = 0u;
        s_why_time = 0u;
        return 2;                            /* main.c: run_bt_hp_bench() */
    }
    s_armed = 1u;
    s_in = 0u;
    s_why_time = 0u;
    lbegin("start");
    lku("secs", s_cfg.secs);
    lku("pair", s_cfg.pair);
    lku("pair_every", s_cfg.pair_every);
    lku("pair_secs", s_cfg.pair_secs);
    lku("autoconfirm", s_cfg.autoconfirm);
    lku("xrec_pin", s_cfg.xrec_pin[0] != '\0');     /* set or not; the operator wrote the value */
    lks("exit", "auto");
    lku("reboot", s_cfg.reboot);
    lkx("prev_fault", g_prev_fault_code);
    lks("log", BT_BENCH_LOG_FILE);
    lemit();
    return 1;
}

void bt_bench_begin(void)
{
    if (!s_armed || s_in) {
        return;
    }
    s_in = 1u;
    s_ac_weak = 0u;
    if (s_cfg.autoconfirm) {
        bt_xrec_set_bench_autoconfirm(1);    /* logs "BT SSP BENCH autoconfirm ON" (bt_xrec_bluedroid.c) */
    }
    if (s_cfg.autoconfirm && s_ac_weak) {
        lbegin("autoconfirm");
        lks("st", "unavailable");            /* this image has no confirm-UI override: confirm on the device */
        lemit();
    }
    s_pin_weak = 0u;
    if (s_cfg.xrec_pin[0] != '\0') {
        bt_xrec_set_bench_xrec_pin(s_cfg.xrec_pin);   /* before the first window (act() below) */
        if (s_pin_weak) {
            lbegin("xrec_pin");
            lks("st", "unavailable");
            lemit();
        }
    }
    uint32_t now = millis();
    bt_bench_run_begin(&s_run, &s_cfg, now);
    s_win1.prev_us = 0u;                     /* CPU windows start with this run (baselines) */
    s_win1.btc = NULL;
    s_win30.prev_us = 0u;
    s_status_n = 0u;
    s_cpu1_next = now;
    meas_cpu1(now);
    cpu_take_t c;
    cpu_win_take(&s_win30, &c);
    act(bt_bench_run_tick(&s_run, now), now);
    drain();
}

int bt_bench_tick(uint32_t now_ms)
{
    if (!s_in) {
        return 0;
    }
    meas_cpu1(now_ms);
    uint32_t a = bt_bench_run_tick(&s_run, now_ms);
    if (a & BT_BENCH_ACT_EXIT) {
        if (!s_why_time) {
            s_why_time = 1u;
            log_status(now_ms - s_run.t0, 1);
            lbegin("exit");
            lks("why", "time");
            lku("ms", now_ms - s_run.t0);
            lemit();
        }
        drain();
        return 1;
    }
    act(a, now_ms);
    drain();
    return 0;
}

#define BENCH_TAIL_RECS 2u                   /* finished recordings in the BTBENCH.LOG trailer */
static char s_tail[1408] BENCH_HI;           /* LOGCAP + final + 2 recordings (<= ~470 B each) */

/* Append the line builder's content (one whole line) to s_tail at tl; a line that does not fit is dropped. */
static uint32_t tail_add(uint32_t tl)
{
    if (tl + s_ll <= sizeof(s_tail)) {
        memcpy(&s_tail[tl], s_line, s_ll);
        tl += s_ll;
    }
    return tl;
}

int bt_bench_end(exfat_vol_t *vol, int sd_mounted)
{
    if (!s_armed) {
        return 0;
    }
    if (s_cfg.autoconfirm) {
        bt_xrec_set_bench_autoconfirm(0);    /* never outlives the run: a later manual BT session asks the user */
    }
    if (s_cfg.xrec_pin[0] != '\0') {
        bt_xrec_set_bench_xrec_pin(NULL);    /* later windows draw their PIN from the DRBG again */
    }
    uint32_t el = millis() - s_run.t0;
    lbegin("end");
    lks("why", s_why_time ? "time" : "user");   /* user: BACK, REC slide, or BT mode left by itself */
    lku("ms", s_in ? el : 0u);
    lku("pairs", s_run.pairs);
    lku("reboot", s_cfg.reboot);
    lemit();
    drain();
    s_armed = 0u;
    s_in = 0u;

    /* The file's trailer: the capture counters, then the F0 measurement summary (the capture keeps the head of a long
     * run, so these always survive): "BENCH final" (GETS, SD errors, last CPU window) and the latest finished
     * recordings, newest first ("BENCH final rec i=<n> <fields>"). No running one: run_bt_xrec_mode() stops a
     * recording before it returns, and this runs after it. */
    uint32_t tl = 0u;
    s_ll = 0u;                               /* reuse the line builder for the file's trailer */
    ls("BENCH LOGCAP");
    lku("bytes", s_cap.len);
    lku("lost", s_cap.lost);
    lku("resets", s_cap.resets);
    lc('\n');
    tl = tail_add(tl);
    s_ll = 0u;
    ls("BENCH final ");
    lmeas_cpu();
    lku("recs", g_bench_meas.hist_n);
    lku("sd_err_tot", g_bench_meas.sd_err_total);   /* SD errors since boot (the recordings carry their own) */
    lku("sd_mdto_tot", g_bench_meas.sd_mdto_total);
    lc('\n');
    tl = tail_add(tl);
    for (uint32_t i = 0u; i < BENCH_TAIL_RECS; i++) {
        const bench_meas_rec_t *rec = bench_meas_hist(&g_bench_meas, i);
        if (rec == NULL) {
            break;
        }
        s_ll = 0u;
        ls("BENCH final rec i=");
        lu(i);
        lc(' ');
        lmeas_rec(rec);
        lc('\n');
        tl = tail_add(tl);
    }
    int r = bt_bench_log_save(vol, sd_mounted, s_cap.buf, s_cap.len, s_tail, tl, &s_writer);
    lbegin("LOGFILE");
    lks("file", BT_BENCH_LOG_FILE);
    lks("st", r == 0 ? "OK" : "FAIL");
    lku("bytes", s_cap.len);
    lku("lost", s_cap.lost);
    lemit();
    return s_cfg.reboot ? 1 : 0;
}
