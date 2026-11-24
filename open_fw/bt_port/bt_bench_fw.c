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

/* SRAM_HI (.bss_hi, zeroed at boot) like hci_probe.c's log copy: .bt_bss would come out of the RTOS heap region. */
#define BENCH_HI           __attribute__((section(".bss_hi"), aligned(4)))
#define BENCH_CAP_BYTES    8192u    /* ~100 lines; beyond that the head is kept here, the tail stays in the ring */

static char s_cap_buf[BENCH_CAP_BYTES] BENCH_HI;
static exfat_writer_t s_writer BENCH_HI;
static bt_bench_cap_t s_cap BENCH_HI;
static bt_bench_run_t s_run BENCH_HI;
static bt_bench_cfg_t s_cfg BENCH_HI;
static char s_line[160] BENCH_HI;
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

static void log_status(uint32_t el_ms)
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
    lemit();
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
        log_status(el);
    }
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
    act(bt_bench_run_tick(&s_run, now), now);
    drain();
}

int bt_bench_tick(uint32_t now_ms)
{
    if (!s_in) {
        return 0;
    }
    uint32_t a = bt_bench_run_tick(&s_run, now_ms);
    if (a & BT_BENCH_ACT_EXIT) {
        if (!s_why_time) {
            s_why_time = 1u;
            log_status(now_ms - s_run.t0);
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

    char tail[80];
    uint32_t tl = 0u;
    s_ll = 0u;                               /* reuse the line builder for the file's trailer */
    ls("BENCH LOGCAP");
    lku("bytes", s_cap.len);
    lku("lost", s_cap.lost);
    lku("resets", s_cap.resets);
    lc('\n');
    for (; tl < s_ll && tl < sizeof(tail); tl++) {
        tail[tl] = s_line[tl];
    }
    int r = bt_bench_log_save(vol, sd_mounted, s_cap.buf, s_cap.len, tail, tl, &s_writer);
    lbegin("LOGFILE");
    lks("file", BT_BENCH_LOG_FILE);
    lks("st", r == 0 ? "OK" : "FAIL");
    lku("bytes", s_cap.len);
    lku("lost", s_cap.lost);
    lemit();
    return s_cfg.reboot ? 1 : 0;
}
