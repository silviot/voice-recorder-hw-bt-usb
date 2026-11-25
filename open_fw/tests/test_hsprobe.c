/*
 * Host tests for the headset button probe:
 *   1. pure log logic (hsprobe_log.c): line builder, bounded buffer, change
 *      detection (> ±40 LSB), drop/rise edge counting per phase, GPIO auto-mask,
 *      codec window stats, summary + END marker under overflow;
 *   2. full hsprobe_run() against a mock SoC (sparse MMIO register file,
 *      simulated SysTick driving the real keys.c scanner, mock codec capture)
 *      and a real exFAT image: verifies all-channel enable during the run,
 *      PB11 analog mux in phase C, MICBIAS/codec mux use, register restore
 *      (self-check PASS), paused key scanning, BACK abort, REC refusal,
 *      no-SD path and the saved /HSPROBE.LOG contents.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_fs.h"
#include "openfw_hsprobe.h"

#define HSP_GPIOB_MUX_PROBE   (GPIOB_BASE + 0x04u)
#define HSP_SARADC_THR1_PROBE (SARADC_BASE + 0x30u)

/* =========================================================================
 * Mock SoC
 * ========================================================================= */
#define NREG 1024
static uint32_t s_addr[NREG], s_val[NREG];
static unsigned s_nreg;
static uint32_t s_ms;

volatile uint32_t g_exp_running;
volatile uint32_t g_exp_deadline_ms;

/* Scenario knobs */
static int      s_jack_plugged   = 1;   /* PB11 low when plugged */
static int      s_rec_switch_on  = 0;
static uint32_t s_back_at_ms     = 0u;  /* absolute sim ms to press BACK (0 = never) */
static int      s_btn_on_ch      = 3;   /* SARADC channel wired to the "headset button" */
static int      s_btn_needs_bias = 1;   /* button visible only with MICBIAS on */
static int      s_dac_busy       = 0;
static int      s_recorder_saving = 0;
static uint32_t s_probe_t0       = 0u;

/* Observations */
static uint32_t s_max_ctrl_during = 0u;
static int      s_seen_pb11_analog = 0;
static int      s_seen_micbias = 0;
static uint32_t s_mux_values_seen = 0u;

static uint32_t *reg_slot(uint32_t addr, int create)
{
    for (unsigned i = 0; i < s_nreg; i++) {
        if (s_addr[i] == addr) return &s_val[i];
    }
    if (!create) return NULL;
    assert(s_nreg < NREG);
    s_addr[s_nreg] = addr;
    s_val[s_nreg]  = 0u;
    return &s_val[s_nreg++];
}

static int button_pressed_now(void)
{
    /* User presses the headset button during each prompt window (0.4 s press). */
    if (s_probe_t0 == 0u) return 0;
    uint32_t t = s_ms - s_probe_t0;
    for (uint8_t i = 1; i <= HSP_PROMPT_COUNT; i++) {
        uint32_t st = hsp_prompt_start_ms(i) + 300u;
        if (t >= st && t < st + 400u) return 1;
    }
    return 0;
}

static int micbias_on(void)
{
    uint32_t *a = reg_slot(CODEC_ANA_CTRL0_ADDR, 0);
    return a && ((*a & 0x03000000u) != 0u);
}

uint32_t openfw_mmio_rd32(uint32_t addr)
{
    if (addr == GPIOB_BASE + 0x10u) {
        uint32_t v = 0xFFFFFFFFu;
        if (s_jack_plugged) v &= ~(1u << 11);
        if (s_rec_switch_on) v &= ~(1u << 6);
        return v;
    }
    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u && (addr & 0x3Fu) == 0x10u) {
        return 0x00000000u; /* ports A, C, D: static */
    }
    if (addr >= SARADC_BASE + 0x24u && addr <= SARADC_BASE + 0xB4u && ((addr - SARADC_BASE - 0x24u) & 0xFu) == 0u) {
        uint32_t ch   = (addr - SARADC_BASE - 0x24u) / 0x10u;
        uint32_t *ctl = reg_slot(SARADC_CTRL_ADDR, 0);
        uint32_t en   = ctl ? *ctl : 0u;
        if (!(en & (1u << ch))) return 0u;
        if (ch == 1u) {
            if (s_back_at_ms && s_ms >= s_back_at_ms && s_ms < s_back_at_ms + 200u) return 2300u;
            return 3500u;
        }
        if ((int)ch == s_btn_on_ch) {
            int pressed = button_pressed_now() && (!s_btn_needs_bias || micbias_on());
            return pressed ? 150u : 2600u + (s_ms % 7u);
        }
        return 1000u + ch * 100u + ((s_ms / 10u) % 3u) * 5u; /* quiet floating channels */
    }
    uint32_t *p = reg_slot(addr, 0);
    return p ? *p : 0u;
}

void openfw_mmio_wr32(uint32_t addr, uint32_t val)
{
    *reg_slot(addr, 1) = val;
    if (addr == SARADC_CTRL_ADDR && (val & 0x3FFu) > s_max_ctrl_during) s_max_ctrl_during = val & 0x3FFu;
    if (addr == HSP_GPIOB_MUX_PROBE && ((val >> 12) & 0xFu) == 15u) s_seen_pb11_analog = 1;
    if (addr == CODEC_ANA_CTRL0_ADDR && (val & 0x03000000u)) s_seen_micbias = 1;
    if (addr == CODEC_ADC_CH_MUX_ADDR) s_mux_values_seen |= 1u << ((val >> 16) & 0xFu);
}
uint16_t openfw_mmio_rd16(uint32_t a) { return (uint16_t)openfw_mmio_rd32(a); }
void openfw_mmio_wr16(uint32_t a, uint16_t v) { openfw_mmio_wr32(a, v); }
uint8_t openfw_mmio_rd8(uint32_t a) { return (uint8_t)openfw_mmio_rd32(a); }
void openfw_mmio_wr8(uint32_t a, uint8_t v) { openfw_mmio_wr32(a, v); }
void openfw_mmio_nop(void) {}
void openfw_mmio_dsb(void) {}
void openfw_host_dmac_register_ptr(uint32_t ch, void *buf, uint32_t len) { (void)ch; (void)buf; (void)len; }

/* Simulated 1 ms SysTick: drives the real keys.c scanner. */
static void tick(void)
{
    s_ms++;
    keys_scan_tick(s_ms);
}
void openfw_mmio_wfi(void) { tick(); }
uint32_t millis(void) { return s_ms; }
void mdelay(uint32_t ms) { while (ms--) tick(); }
void wdt_feed(void) {}
void rtc_get(rtc_datetime_t *dt) { memset(dt, 0, sizeof(*dt)); dt->year = 2026; dt->month = 11; dt->day = 24; dt->hour = 12; }
uint32_t rtc_to_dos_timestamp(const rtc_datetime_t *dt) { (void)dt; return 0x5B780000u; }
int recorder_is_saving(void) { return s_recorder_saving; }
int audio_dac_is_busy(void) { return s_dac_busy; }

/* Mock codec capture: mimics the register side effects of audio_dma.c. */
static int      s_cap_running;
static uint32_t s_cap_next_ms;
static int16_t  s_frame[2048 * 2];
int audio_capture_start_ex(uint8_t gain, uint8_t ch)
{
    (void)gain;
    assert(ch == 2u);
    gpio_set_mux(GPIO_PORT_B, 12u, 15u);
    gpio_set_mux(GPIO_PORT_B, 13u, 15u);
    gpio_set_mux(GPIO_PORT_B, 14u, 15u);
    gpio_set_mux(GPIO_PORT_B, 15u, 15u);
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0xC0000022u);
    openfw_mmio_set32(CODEC_AMIC01_PGA_ADDR, 0x20430000u);
    openfw_mmio_set32(CODEC_AMIC2_CTRL_ADDR, 0x00010043u);
    openfw_mmio_rmw32(CODEC_ADC_CH_MUX_ADDR, 0x000D0D0Du, 0x000D0D00u);
    openfw_mmio_rmw32(CODEC_GLB_ADDR, 0x000FF000u, 0x00055000u);
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x03000000u);
    openfw_mmio_set32(CODEC_ADC_DIG_CTRL_ADDR, 0x303u);
    mdelay(22u);
    s_cap_running = 1;
    s_cap_next_ms = s_ms + 128u;
    return 0;
}
void audio_capture_stop(void)
{
    s_cap_running = 0;
    openfw_mmio_clr32(CODEC_ADC_DIG_CTRL_ADDR, 0x3u);
    openfw_mmio_clr32(CODEC_AMIC2_CTRL_ADDR, 0x00010043u);
    openfw_mmio_clr32(CODEC_ANA_CTRL0_ADDR, 0x03000000u);
}
const int16_t *audio_pop_frame(uint32_t *out_bytes)
{
    if (!s_cap_running || s_ms < s_cap_next_ms) return NULL;
    s_cap_next_ms += 128u;
    uint32_t mux = (openfw_mmio_rd32(CODEC_ADC_CH_MUX_ADDR) >> 16) & 0xFu;
    int loud = (mux == 5u) || button_pressed_now();
    for (int i = 0; i < 2048; i++) {
        s_frame[2 * i]     = (int16_t)((i % 50) - 25);
        s_frame[2 * i + 1] = (int16_t)(loud ? ((i & 1) ? 9000 : -9000) : ((i % 30) - 15));
    }
    if (out_bytes) *out_bytes = sizeof(s_frame);
    return s_frame;
}

/* exFAT image in RAM */
static uint8_t *s_img;
static uint32_t s_img_secs;
static int img_rd(void *c, uint32_t lba, uint32_t n, void *b) { (void)c; if (lba + n > s_img_secs) return -1; memcpy(b, s_img + (size_t)lba * 512u, (size_t)n * 512u); return 0; }
static int img_wr(void *c, uint32_t lba, uint32_t n, const void *b) { (void)c; if (lba + n > s_img_secs) return -1; memcpy(s_img + (size_t)lba * 512u, b, (size_t)n * 512u); return 0; }

/* =========================================================================
 * Helpers
 * ========================================================================= */
static int count_lines_with(const char *buf, uint32_t len, const char *needle)
{
    int n = 0;
    const char *p = buf, *end = buf + len;
    size_t nl = strlen(needle);
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        for (const char *q = p; q + nl <= e; q++) {
            if (memcmp(q, needle, nl) == 0) { n++; break; }
        }
        p = e + 1;
    }
    return n;
}

static int render_calls;
static uint8_t last_prompt_seen_max;
static int8_t plug_seen_unknown;
static void render_cb(uint8_t secs, uint8_t phase, uint8_t prompt, int8_t plug)
{
    (void)secs;
    assert(phase >= 1u && phase <= 3u);
    render_calls++;
    if (prompt > last_prompt_seen_max) last_prompt_seen_max = prompt;
    if (phase == 3u && plug == -1) plug_seen_unknown = 1;
}
static int poll_cb(uint32_t now) { (void)now; return 1; }

/* =========================================================================
 * 1. Pure logic tests
 * ========================================================================= */
static void test_line_and_text(void)
{
    hsp_line_t l;
    hsp_line_reset(&l);
    hsp_line_u32(&l, 0u); hsp_line_chr(&l, ' ');
    hsp_line_u32(&l, 4294967295u); hsp_line_chr(&l, ' ');
    hsp_line_i32(&l, -2147483647 - 1); hsp_line_chr(&l, ' ');
    hsp_line_hex(&l, 0xDEADBEEFu); hsp_line_chr(&l, ' ');
    hsp_line_hex_n(&l, 0xABCu, 4u);
    assert(strcmp(l.s, "0 4294967295 -2147483648 0xDEADBEEF 0ABC") == 0);
    for (int i = 0; i < 400; i++) hsp_line_chr(&l, 'x'); /* clamps */
    assert(l.n == sizeof(l.s) - 1u && l.s[l.n] == '\0');

    char buf[32];
    hsp_text_t t;
    hsp_text_init(&t, buf, sizeof(buf), 11u);
    hsp_line_reset(&l); hsp_line_str(&l, "0123456789"); /* 11 B with \n */
    assert(hsp_text_line(&t, &l, 0) == 0);
    assert(hsp_text_line(&t, &l, 0) == -1);            /* 22 > 32-11 */
    assert(hsp_text_line(&t, &l, 1) == 0);             /* reserve usable */
    assert(hsp_text_line(&t, &l, 1) == -1);            /* full */
    assert(t.len == 22u && t.dropped_lines == 2u);
    assert(memcmp(buf, "0123456789\n0123456789\n", 22) == 0);
}

static void test_schedule(void)
{
    assert(hsp_phase_at(0) == 0 && hsp_phase_at(9999) == 0 && hsp_phase_at(10000) == 1);
    assert(hsp_phase_at(19999) == 1 && hsp_phase_at(20000) == 2 && hsp_phase_at(29999) == 2);
    assert(hsp_prompt_at(4999) == 0 && hsp_prompt_at(5000) == 1 && hsp_prompt_at(6499) == 1);
    assert(hsp_prompt_at(6500) == 0 && hsp_prompt_at(27100) == 5);
    /* one prompt in A, two in B, two in C */
    int per[3] = {0, 0, 0};
    for (uint8_t i = 1; i <= HSP_PROMPT_COUNT; i++) per[hsp_phase_at(hsp_prompt_start_ms(i))]++;
    assert(per[0] == 1 && per[1] == 2 && per[2] == 2);
    assert(hsp_prompt_start_ms(0) == 0u && hsp_prompt_start_ms(6) == 0u);
}

static char s_buf[HSP_LOG_CAP];
static hsp_state_t s_st;

static void test_change_and_edges(void)
{
    hsp_init(&s_st, s_buf, sizeof(s_buf), NULL);
    uint16_t v[HSP_NCH];
    uint32_t g[HSP_NPORT] = {0, 0, 0, 0};
    for (int c = 0; c < (int)HSP_NCH; c++) v[c] = 2000;
    uint32_t t = 0;
    hsp_set_phase(&s_st, 0, 0, "x");
    hsp_sample(&s_st, t, v, 1, g);
    /* +40 is not a change, +41 is */
    v[2] = 2040; t += 10; hsp_sample(&s_st, t, v, 1, g);
    assert(count_lines_with(s_buf, s_st.txt.len, " K2 ") == 0);
    v[2] = 2081; t += 10; hsp_sample(&s_st, t, v, 1, g);
    assert(count_lines_with(s_buf, s_st.txt.len, "20 K2 2081") == 1);
    /* relative to last LOGGED value: 2081 -> 2041 (=-40) no event, -> 2040 (-41) event */
    v[2] = 2041; t += 10; hsp_sample(&s_st, t, v, 1, g);
    assert(count_lines_with(s_buf, s_st.txt.len, " K2 ") == 1);
    v[2] = 2040; t += 10; hsp_sample(&s_st, t, v, 1, g);
    assert(count_lines_with(s_buf, s_st.txt.len, " K2 ") == 2);

    /* Let the baseline settle, then 3 presses in phase B on ch5 (drop to 100) */
    for (int i = 0; i < 100; i++) { t += 10; hsp_sample(&s_st, t, v, 1, g); }
    hsp_set_phase(&s_st, t, 1, "b");
    for (int p = 0; p < 3; p++) {
        v[5] = 100;
        for (int i = 0; i < 20; i++) { t += 10; hsp_sample(&s_st, t, v, 0, g); }
        v[5] = 2000;
        for (int i = 0; i < 50; i++) { t += 10; hsp_sample(&s_st, t, v, 1, g); }
    }
    assert(s_st.ch[5].drops[0] == 0 && s_st.ch[5].drops[1] == 3 && s_st.ch[5].rises[1] == 0);
    assert(count_lines_with(s_buf, s_st.txt.len, " D5 100 ref=2000") == 3);
    assert(count_lines_with(s_buf, s_st.txt.len, " E5 2000 ref=2000 len=") == 3);
    assert(s_st.pb11_changes[1] == 6); /* PB11 toggled with every press */
    /* a slow drift (5 LSB per sample) never counts as an edge */
    for (int i = 0; i < 200; i++) { v[7] = (uint16_t)(2000 - i * 5); t += 10; hsp_sample(&s_st, t, v, 1, g); }
    assert(s_st.ch[7].drops[1] == 0);
    /* a permanent level shift (plug event) re-baselines after 3 s and counts once */
    hsp_set_phase(&s_st, t, 2, "c");
    v[8] = 3500;
    for (int i = 0; i < 400; i++) { t += 10; hsp_sample(&s_st, t, v, 1, g); }
    assert(s_st.ch[8].rises[2] == 1 && s_st.ch[8].edge == 0);
    assert(count_lines_with(s_buf, s_st.txt.len, " R8 3500") == 1);
    assert(s_st.ch[5].min == 100 && s_st.ch[5].max == 2000);
    assert(s_st.ch[5].min_ph[0] == 2000 && s_st.ch[5].min_ph[1] == 100);

    hsp_snapshot(&s_st, t, v, -1);
    assert(count_lines_with(s_buf, s_st.txt.len, " S - 2000 2000 2040 2000 2000 2000 2000 1005 3500 2000") == 1);
}

static void test_budget_overflow_and_summary(void)
{
    hsp_init(&s_st, s_buf, sizeof(s_buf), NULL);
    uint16_t v[HSP_NCH];
    uint32_t g[HSP_NPORT] = {0, 0, 0, 0};
    uint32_t t = 0;
    /* every channel toggles by 500 every sample for 30 s: worst-case noise */
    for (int i = 0; i < 3000; i++) {
        for (int c = 0; c < (int)HSP_NCH; c++) v[c] = (uint16_t)((i & 1) ? 3000 : 2500);
        g[0] = (i & 1) ? 0x1u : 0x0u;       /* noisy GPIO A0 gets auto-masked */
        g[2] = (i == 1500) ? 0x80u : 0x0u;  /* one real GPIO C7 pulse */
        hsp_set_phase(&s_st, t, (uint8_t)hsp_phase_at(t), NULL);
        hsp_sample(&s_st, t, v, i & 1, g);
        if ((i % 100) == 0) hsp_snapshot(&s_st, t, v, i & 1);
        t += 10;
    }
    for (int c = 0; c < (int)HSP_NCH; c++) {
        assert(s_st.ch[c].events == HSP_CH_EVT_BUDGET);
        assert(s_st.ch[c].suppressed > 0);
    }
    assert((s_st.gpio_mask[0] & 1u) == 0u);
    assert(count_lines_with(s_buf, s_st.txt.len, "GMA noisy=00000001") == 1);
    assert(s_st.gpio_changed_bits[2] == 0x80u && s_st.gpio_changed_bits[0] == 0u);
    assert(s_st.txt.len <= HSP_LOG_CAP - HSP_LOG_RESERVE);
    hsp_summary(&s_st, t, "complete");
    assert(s_st.txt.len <= HSP_LOG_CAP);
    assert(count_lines_with(s_buf, s_st.txt.len, "SUM K9 ") == 1);
    assert(count_lines_with(s_buf, s_st.txt.len, "HINT candidates=") == 1);
    assert(count_lines_with(s_buf, s_st.txt.len, "GC:00000080") == 1);
    assert(count_lines_with(s_buf, s_st.txt.len, "overflow=1") == 1);
    /* the log must end with a complete END line */
    assert(s_buf[s_st.txt.len - 1] == '\n');
    const char *last = s_buf + s_st.txt.len - 2;
    while (last > s_buf && *last != '\n') last--;
    assert(strncmp(last + 1, "END t=30000 reason=complete", 27) == 0);

    /* END survives even with zero reserve left */
    char tiny[64];
    hsp_init(&s_st, tiny, sizeof(tiny), NULL);
    s_st.txt.reserve = 0;
    for (int i = 0; i < 10; i++) hsp_comment(&s_st, "# filler line 123");
    hsp_summary(&s_st, 123, "back");
    assert(tiny[s_st.txt.len - 1] == '\n');
    assert(s_st.txt.len >= 22u && memcmp(tiny + s_st.txt.len - 22u, "END t=123 reason=back\n", 22) == 0);
    assert(s_st.txt.len == 22u || tiny[s_st.txt.len - 23u] == '\n');
}

static void test_codec_stats(void)
{
    hsp_init(&s_st, s_buf, sizeof(s_buf), NULL);
    int16_t pcm[2 * 100];
    hsp_codec_block(&s_st, 0, pcm, 100, 2); /* ignored: no window open */
    assert(s_st.a_blocks == 0);
    hsp_codec_window_begin(&s_st, 0, 7);
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < 100; i++) {
            pcm[2 * i]     = (int16_t)(i - 50);              /* c1: -50..49, mean -1 (round -0.5) */
            pcm[2 * i + 1] = (int16_t)(b == 2 ? 20000 : 10); /* c2: mean step + back */
        }
        hsp_codec_block(&s_st, (uint32_t)b * 128u, pcm, 100, 2);
    }
    hsp_codec_window_end(&s_st, 600);
    assert(s_st.mux[7].windows == 1 && s_st.mux[7].events == 2);
    assert(count_lines_with(s_buf, s_st.txt.len, "AE mux=7") == 2);
    assert(count_lines_with(s_buf, s_st.txt.len, "600 A mux=7 n=400 c1=-50/49/") == 1);
    assert(s_st.mux[7].max2 == 20000 && s_st.mux[7].min2 == 10);
    hsp_codec_window_end(&s_st, 700); /* no-op, closed */
    hsp_summary(&s_st, 700, "x");
    assert(count_lines_with(s_buf, s_st.txt.len, "SUM MUX7 1 ") == 1);
    assert(count_lines_with(s_buf, s_st.txt.len, "SUM MUX0 ") == 0);
}

/* =========================================================================
 * 2. Full run against the mock SoC
 * ========================================================================= */
static void soc_reset(void)
{
    s_nreg = 0;
    s_ms   = 1000u;
    /* keys.c steady-state configuration */
    openfw_mmio_wr32(SARADC_CTRL_ADDR, 0x80180002u);
    openfw_mmio_wr32(SARADC_START_ADDR, 0x0000A800u);
    openfw_mmio_wr32(HSP_SARADC_THR1_PROBE, 0x0FFF0030u);
    openfw_mmio_wr32(HSP_GPIOB_MUX_PROBE, 0x00000000u);
    openfw_mmio_wr32(GPIOB_BASE + 0x24u, 0x00800000u);
    openfw_mmio_wr32(CODEC_ANA_CTRL0_ADDR, 0x00000022u);
    openfw_mmio_wr32(CODEC_DAC_ANA_CFG0_ADDR, 0x00000200u);
    openfw_mmio_wr32(CODEC_GLB_ADDR, 0x00000001u);
    openfw_mmio_wr32(CODEC_ADC_CH_MUX_ADDR, 0x00000D00u);
    s_max_ctrl_during = 0u;
    s_seen_pb11_analog = 0;
    s_seen_micbias = 0;
    s_mux_values_seen = 0u;
    s_probe_t0 = 0u;
    render_calls = 0;
    last_prompt_seen_max = 0;
    plug_seen_unknown = 0;
    s_cap_running = 0;
}

static uint32_t snapshot_regs(uint32_t *a, uint32_t *v)
{
    for (unsigned i = 0; i < s_nreg; i++) { a[i] = s_addr[i]; v[i] = s_val[i]; }
    return s_nreg;
}

static void assert_regs_restored(const uint32_t *a, const uint32_t *v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] == SARADC_START_ADDR) continue;
        uint32_t now = openfw_mmio_rd32(a[i]);
        if (now != v[i]) {
            fprintf(stderr, "reg 0x%08X not restored: pre=0x%08X post=0x%08X\n", a[i], v[i], now);
            assert(0);
        }
    }
    assert(openfw_mmio_rd32(SARADC_START_ADDR) == 0x0000A800u);
}

static void keys_resume_check(void)
{
    /* After the probe the normal scanner must work again: press BACK on the ladder. */
    key_event_t ev;
    /* main.c: keys_ignore_held() after the modal, then the loop keeps polling */
    keys_ignore_held();
    for (int i = 0; i < 20; i++) { tick(); while (keys_pop_event(&ev)) {} }
    s_back_at_ms = s_ms + 5u;
    for (int i = 0; i < 60; i++) tick();
    int got = 0;
    while (keys_pop_event(&ev)) {
        if (ev.key == KEY_BACK && ev.pressed) got = 1;
    }
    assert(got);
    for (int i = 0; i < 300; i++) tick();
    s_back_at_ms = 0;
}

static void test_full_run(const char *img_path)
{
    FILE *f = fopen(img_path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_img = malloc((size_t)sz);
    assert(s_img && fread(s_img, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    s_img_secs = (uint32_t)(sz / 512);
    blkdev_t dev = { img_rd, img_wr, s_img_secs, NULL };
    exfat_vol_t vol;

    soc_reset();
    keys_init();
    assert(exfat_mount(&vol, &dev) == 0);
    for (int i = 0; i < 50; i++) tick();

    static uint32_t pre_a[NREG], pre_v[NREG];
    uint32_t npre = snapshot_regs(pre_a, pre_v);

    hsprobe_ctx_t ctx = { &vol, 1, poll_cb, render_cb };

    /* --- refuse while busy (REC switch on / recorder saving / DAC busy) --- */
    s_rec_switch_on = 1;
    assert(hsprobe_run(&ctx) == HSP_RC_BUSY);
    s_rec_switch_on = 0;
    s_recorder_saving = 1;
    assert(hsprobe_run(&ctx) == HSP_RC_BUSY);
    s_recorder_saving = 0;
    s_dac_busy = 1;
    assert(hsprobe_run(&ctx) == HSP_RC_BUSY);
    s_dac_busy = 0;
    assert(s_max_ctrl_during == 0x002u);

    /* --- full 30 s run, button wired to SARADC ch3 (needs MICBIAS) --- */
    s_probe_t0 = s_ms;
    assert(hsprobe_run(&ctx) == HSP_RC_SAVED);
    assert(s_ms - s_probe_t0 >= HSP_T_END && s_ms - s_probe_t0 < HSP_T_END + 200u);
    assert(s_max_ctrl_during == 0x3FFu);
    assert(s_seen_pb11_analog && s_seen_micbias);
    assert(s_mux_values_seen == 0xFFFFu);
    assert(render_calls > 50 && last_prompt_seen_max == HSP_PROMPT_COUNT && plug_seen_unknown);
    assert_regs_restored(pre_a, pre_v, npre);

    const char *log = g_hsp_log_buf;
    uint32_t len = hsprobe_last_log_len();
    assert(len > 1000u && len <= HSP_LOG_CAP);
    if (getenv("HSP_DUMP")) fwrite(log, 1, len, stdout);
    assert(strncmp(log, "# HSPROBE v1", 12) == 0);
    assert(count_lines_with(log, len, "PHASE A") == 1);
    assert(count_lines_with(log, len, "PHASE B") == 1);
    assert(count_lines_with(log, len, "PHASE C pb11=analog_mux15") == 1);
    assert(count_lines_with(log, len, "CODEC_ON") == 1 && count_lines_with(log, len, "CODEC_OFF") == 1);
    assert(count_lines_with(log, len, " PROMPT ") == 5);
    assert(count_lines_with(log, len, " S 0 ") == 30); /* 1 Hz snapshots, jack plugged */
    assert(count_lines_with(log, len, "RESULT=PASS") == 1);
    assert(count_lines_with(log, len, "CHK DIFF") == 0);
    assert(count_lines_with(log, len, "key=NONE") == 1);
    /* ch3 drops only with MICBIAS: 0 in A, 2 in B, 2 in C */
    assert(count_lines_with(log, len, "SUM K3 150 ") == 1);
    assert(count_lines_with(log, len, "drops=0/2/2") == 1);
    assert(count_lines_with(log, len, "HINT candidates=K3:4") == 1);
    assert(count_lines_with(log, len, "SUM MUX5 ") == 1);
    assert(count_lines_with(log, len, "END t=") == 1 && count_lines_with(log, len, "reason=complete") == 1);
    assert(count_lines_with(log, len, "overflow=0") == 1);

    /* the file on the card matches the RAM log */
    exfat_entry_t ent;
    assert(exfat_find_path(&vol, "/HSPROBE.LOG", &ent) == 0 && ent.size == len);
    char *rd = malloc(len);
    uint32_t nrd = 0;
    assert(exfat_read_file(&vol, &ent, 0, rd, len, &nrd) == 0 && nrd == len);
    assert(memcmp(rd, log, len) == 0);
    free(rd);

    keys_resume_check();

    /* --- BACK aborts early, file is overwritten (still one HSPROBE.LOG) --- */
    soc_reset();
    keys_init();
    for (int i = 0; i < 50; i++) tick();
    npre = snapshot_regs(pre_a, pre_v);
    s_probe_t0   = s_ms;
    s_back_at_ms = s_ms + 3000u;
    assert(hsprobe_run(&ctx) == HSP_RC_SAVED);
    assert(s_ms - s_probe_t0 < 3300u);
    assert_regs_restored(pre_a, pre_v, npre);
    len = hsprobe_last_log_len();
    assert(count_lines_with(g_hsp_log_buf, len, "reason=back") == 1);
    assert(count_lines_with(g_hsp_log_buf, len, "RESULT=PASS") == 1);
    assert(exfat_find_path(&vol, "/HSPROBE.LOG", &ent) == 0 && ent.size == len);
    for (int i = 0; i < 400; i++) tick();
    s_back_at_ms = 0;
    {
        key_event_t ev;
        int back_events = 0;
        while (keys_pop_event(&ev)) if (ev.key == KEY_BACK) back_events++;
        assert(back_events == 0); /* the abort press is swallowed by the resync */
    }

    /* --- REC switch during phase B aborts with codec restored --- */
    soc_reset();
    keys_init();
    for (int i = 0; i < 50; i++) tick();
    npre = snapshot_regs(pre_a, pre_v);
    s_probe_t0 = s_ms;
    {
        /* flip REC on at 14 s via the render callback clock */
        hsprobe_ctx_t c2 = ctx;
        c2.poll = NULL;
        /* drive via s_ms check inside render: simple approach, run with a poll that flips it */
        extern int hsp_test_rec_poll(uint32_t now);
        c2.poll = hsp_test_rec_poll;
        assert(hsprobe_run(&c2) == HSP_RC_SAVED);
    }
    s_rec_switch_on = 0;
    assert_regs_restored(pre_a, pre_v, npre);
    len = hsprobe_last_log_len();
    assert(count_lines_with(g_hsp_log_buf, len, "reason=rec_switch") == 1);
    assert(count_lines_with(g_hsp_log_buf, len, "CODEC_OFF") == 1);

    /* --- no SD card --- */
    soc_reset();
    keys_init();
    hsprobe_ctx_t c3 = { NULL, 0, poll_cb, render_cb };
    s_back_at_ms = s_ms + 500u;
    assert(hsprobe_run(&c3) == HSP_RC_NO_SD);
    for (int i = 0; i < 400; i++) tick();
    s_back_at_ms = 0;

    /* write the image back so `fsck.exfat -n` validates the saved HSPROBE.LOG */
    f = fopen(img_path, "r+b");
    assert(f && fwrite(s_img, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    free(s_img);
}

int hsp_test_rec_poll(uint32_t now)
{
    if (now - s_probe_t0 >= 14000u) s_rec_switch_on = 1;
    return 1;
}

int main(int argc, char **argv)
{
    test_line_and_text();
    test_schedule();
    test_change_and_edges();
    test_budget_overflow_and_summary();
    test_codec_stats();
    printf("hsprobe log logic tests OK\n");
    if (argc > 1) {
        test_full_run(argv[1]);
        printf("hsprobe mock-SoC run tests OK (log %u B)\n", (unsigned)hsprobe_last_log_len());
    }
    return 0;
}
