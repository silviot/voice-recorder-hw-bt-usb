/* Host test for the output sink abstraction (include/audio_out.h, src/audio_out.c; docs/BT_A2DP_DESIGN.md §3).
 * Built twice by `make test-bt-hp`:
 *   -DAOUT_TEST_BT0: the BT=0 macros, each aout_* call must reach exactly its audio_dac_* counterpart;
 *   BT=1: src/audio_out.c over a fake DAC (4 slots drained in virtual real time) and the real hp_sink + hp_tx on a
 *   fake stack that completes packets after a fixed delay: routes per mode, async start, no speaker fallback,
 *   mono expansion for the DAC, flush/mute/volume routing, and output mode BOTH: the DAC is delayed so that both
 *   outputs play the same source frame at about the same time.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_out.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fake DAC: 4 slots of up to 1024 frames, drained at the sample rate on the virtual clock ---- */
static struct {
    int running, muted, busy_calls, flushes, stops, starts, vol_calls, vol;
    uint32_t rate, slots[4], h, n, played, written;
    int16_t first_l[4];       /* first left sample of each slot (alignment probe) */
    uint32_t first_idx[4];
    int64_t frac;
} D;
int audio_dac_start(uint32_t r, uint8_t v) { (void)v; D.running = 1; D.rate = r; D.starts++; return 0; }
int audio_dac_write_stereo(const int16_t *p, uint32_t n)
{
    if (!D.running || D.n == 4 || !n) return 0;
    if (n > 1024) n = 1024;
    uint32_t i = (D.h + D.n) % 4;
    D.slots[i] = n; D.first_l[i] = p[0]; D.first_idx[i] = D.written;
    D.n++; D.written += n;
    return (int)n;
}
int audio_dac_is_busy(void) { D.busy_calls++; return D.n != 0; }
void audio_dac_flush(void) { D.flushes++; D.n = 0; }
uint32_t audio_dac_queued_frames(void) { uint32_t q = 0; for (uint32_t k = 0; k < D.n; k++) q += D.slots[(D.h + k) % 4]; return q; }
void audio_dac_set_mute(int m) { D.muted = m; }
void audio_dac_set_volume(uint8_t v, uint16_t d) { (void)d; D.vol = v; D.vol_calls++; }
void audio_dac_stop(void) { D.stops++; D.running = 0; D.n = 0; }
static __attribute__((unused)) void dac_run_us(int64_t us)
{
    if (!D.running) return;
    D.frac += us * D.rate;
    uint32_t frames = (uint32_t)(D.frac / 1000000);
    D.frac -= (int64_t)frames * 1000000;
    while (frames && D.n) {
        uint32_t *s = &D.slots[D.h];
        uint32_t k = frames < *s ? frames : *s;
        *s -= k; frames -= k; D.played += k;
        if (!*s) { D.h = (D.h + 1) % 4; D.n--; }
    }
}

#ifdef AOUT_TEST_BT0
int main(void)
{
    int16_t pcm[8] = { 0 };
    CHECK(aout_start(16000, 18) == 0 && D.starts == 1 && D.rate == 16000, "start");
    CHECK(aout_write(pcm, 4, 2) == 4 && D.written == 4, "write");
    CHECK(aout_queued_frames() == 4, "queued");
    CHECK(aout_is_busy() == 1 && D.busy_calls == 1, "busy");
    aout_set_mute(1); CHECK(D.muted == 1, "mute");
    aout_set_volume(3, 0x1FF); CHECK(D.vol_calls == 1, "volume");
    aout_flush(); CHECK(D.flushes == 1 && D.n == 0, "flush");
    aout_stop(); CHECK(D.stops == 1, "stop");
    CHECK(aout_is_bt() == 0, "is_bt");
    printf("test_audio_out (BT=0 macros): %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
#else
#include "hp_sink.h"
#include "hp_tx.h"

/* ---- fake stack: every packet completes (NOCP) DELAY_US after it is sent ---- */
#define DELAY_US 60000
static int64_t g_now = 1000000;
static hp_tx_t T;
static hp_sink_t S;
static int16_t g_ring[16384];   /* 32 KiB heap-like ring (BOTH) */
static hp_txq_stat_t g_st;
static struct { int64_t done; uint16_t nf; } g_inflight[256];
static int g_ih, g_in;
static int g_ready = 1, g_start_req;
static uint32_t g_acked_sbc;
static int64_t f_now(void *c) { (void)c; return g_now; }
static void *f_alloc(void *c, uint32_t len, uint8_t **d) { (void)c; uint8_t *b = malloc(len); *d = b; return b; }
static void f_free(void *c, void *b) { (void)c; free(b); }
static int f_send(void *c, void *b, uint32_t len, uint16_t nf, uint32_t ts)
{
    (void)c; (void)len; (void)ts;
    free(b);
    g_st.enq_pkts++; g_st.enq_sbc += nf; g_st.deq_pkts++; g_st.deq_sbc += nf;   /* straight downstream */
    g_inflight[(g_ih + g_in) % 256].done = g_now + DELAY_US;
    g_inflight[(g_ih + g_in) % 256].nf = nf;
    g_in++;
    return HP_SEND_OK;
}
static void f_stat(void *c, hp_txq_stat_t *s) { (void)c; *s = g_st; s->qlen = 0; }
static uint32_t f_drop(void *c) { (void)c; return 0; }
static const hp_tx_ops_t k_ops = { f_now, NULL, f_alloc, f_free, f_send, f_stat, f_drop, NULL, NULL };
static int link_ready(void) { return g_ready; }
static void link_start(void) { g_start_req++; }
static const aout_bt_link_t k_link = { link_ready, link_start };

static void stack_run(void)
{
    while (g_in && g_inflight[g_ih].done <= g_now) {
        g_acked_sbc += g_inflight[g_ih].nf;
        hp_tx_on_nocp(&T, 1);
        g_ih = (g_ih + 1) % 256; g_in--;
    }
}

/* source: a ramp so that every frame's value identifies it (index mod 32768) */
static uint32_t g_src_idx;
static void write_some(uint8_t ch)
{
    int16_t buf[2 * 160];
    for (;;) {
        for (uint32_t i = 0; i < 160; i++)
            for (uint32_t c = 0; c < ch; c++) buf[i * ch + c] = (int16_t)((g_src_idx + i) & 0x7FFF);
        int k = aout_write(buf, 160, ch);
        g_src_idx += (uint32_t)k;
        if (k < 160) return;
    }
}

static void setup_bt(void)
{
    static int16_t dummy;
    (void)dummy;
    memset(&g_st, 0, sizeof(g_st));
    g_ih = g_in = 0; g_acked_sbc = 0;
    hp_sink_init(&S, g_ring, sizeof(g_ring));
    hp_tx_init(&T, &k_ops, &S, NULL);
    hp_sbc_cfg_t c = { 48000, HP_SBC_JOINT_STEREO, 16, 8, HP_SBC_LOUDNESS, 2, 35 };
    hp_tx_set_cfg(&T, &c, 900);
}

static void test_routes(void)
{
    int16_t m[64];
    for (int i = 0; i < 64; i++) m[i] = (int16_t)i;
    /* SPEAKER: the DAC, mono expanded */
    aout_set_mode(AOUT_MODE_SPEAKER);
    CHECK(aout_start(16000, 18) == 0 && aout_route() == AOUT_ROUTE_DAC && !aout_is_bt(), "speaker route");
    CHECK(aout_write(m, 64, 1) == 64 && D.written == 64, "mono to DAC");
    aout_stop();
    CHECK(D.stops == 1 && aout_route() == AOUT_ROUTE_NONE, "stop");
    /* HEADPHONES without a headset: -1, never the speaker */
    aout_set_mode(AOUT_MODE_HEADPHONES);
    aout_bt_attach(NULL, NULL, NULL, 0);
    int starts = D.starts;
    CHECK(aout_start(16000, 18) == -1 && D.starts == starts, "no headset: refused, DAC untouched");
    /* with a headset: async start, the ring accepts at once, queued counts the wait */
    setup_bt();
    aout_bt_attach(&T, &S, &k_link, 1);
    CHECK(aout_start(16000, 31) == 0 && aout_route() == AOUT_ROUTE_BT && aout_is_bt() && g_start_req == 1, "bt route");
    CHECK(aout_write(m, 64, 1) == 0, "mono write into a stereo ring refused (ch must match or be 2)");
    aout_set_src_channels(1);
    CHECK(aout_start(16000, 31) == 0, "restart mono");
    CHECK(aout_write(m, 64, 1) == 64, "mono ring");
    CHECK(g_start_req >= 3, "demand start on accepted writes (%d)", g_start_req);
    uint32_t q = aout_queued_frames();
    CHECK(q == 64u, "queued = ring before START (%u)", q);
    CHECK(aout_is_busy(), "busy while queued");
    aout_flush();
    CHECK(aout_queued_frames() == 0u, "flush: 0 at once");
    aout_set_mute(1);
    CHECK(HP_LD(&S.mute) == 1 && D.muted == 0, "mute routed to the sink only");
    aout_set_volume(16, 0x1FF);
    CHECK(HP_LD(&S.gain_q15) == 2068, "volume -> Q15 gain (%u)", HP_LD(&S.gain_q15));
    aout_stop();
    /* BOTH while the ring borrows the DAC buffer: degraded to the headset */
    aout_set_mode(AOUT_MODE_BOTH);
    CHECK(aout_start(16000, 31) == 0 && aout_route() == AOUT_ROUTE_BT, "BOTH degraded");
    aout_stats_t st;
    aout_get_stats(&st);
    CHECK(st.both_degraded == 1, "counted");
    aout_stop();
    /* BOTH without a headset: just the speaker */
    g_ready = 0;
    CHECK(aout_start(16000, 31) == 0 && aout_route() == AOUT_ROUTE_DAC, "BOTH without headset = speaker");
    aout_stop();
    g_ready = 1;
    aout_set_src_channels(2);
}

/* BOTH: run 6 s of a 16 kHz stereo ramp; every 10 ms compare which source frame each output plays now. */
static void test_both_alignment(void)
{
    setup_bt();
    aout_bt_attach(&T, &S, &k_link, 0);
    aout_set_mode(AOUT_MODE_BOTH);
    memset(&D, 0, sizeof(D));
    g_src_idx = 0;
    CHECK(aout_start(16000, 31) == 0 && aout_route() == AOUT_ROUTE_BOTH, "both route");
    write_some(2);
    /* STARTED arrives 100 ms later (async): the ring waits, the DAC is held back meanwhile */
    for (int ms = 0; ms < 100; ms++) { g_now += 1000; dac_run_us(1000); write_some(2); }
    uint32_t tick = hp_tx_start(&T);
    CHECK(tick == 20, "start");
    int64_t next = g_now;
    int32_t worst = 0;
    double sum = 0; int nsum = 0;
    for (int ms = 0; ms < 6000; ms++) {
        g_now += 1000;
        dac_run_us(1000);
        stack_run();
        if (g_now >= next) { hp_tx_tick(&T); next += tick * 1000; }
        write_some(2);
        if (ms > 1500 && ms % 10 == 0) {
            /* headset plays: acked SBC frames (128 @ 48 kHz = 42.67 @ 16 kHz) minus its latency (150 ms) */
            double bt_heard = g_acked_sbc * 128.0 / 3.0 - 150 * 16;
            double dac_heard = D.played;
            int32_t diff = (int32_t)(dac_heard - bt_heard);
            if (abs(diff) > worst) worst = abs(diff);
            sum += diff; nsum++;
        }
    }
    aout_stats_t st;
    aout_get_stats(&st);
    printf("  BOTH at 16 kHz: DAC vs headset offset mean %.1f ms, worst %.1f ms (DAC fed %u frames in %u calls)\n",
           sum / nsum / 16.0, worst / 16.0, st.dac_pump_frames, st.dac_pump_calls);
    CHECK(worst <= 16 * 40, "alignment worst %d frames (> 40 ms)", worst);
    CHECK(D.played > 16000 * 5, "DAC played (%u)", D.played);
    aout_stop();
    hp_tx_stop(&T, HP_STOP_SUSPEND);
}

/* BOTH, merge review r3 #1: the player stops writing (speed dial at 8x+, or at its end) and only asks
 * aout_queued_frames. The DAC must keep being fed until it has every frame the headset got, and the output must not
 * count as drained (the dial's end flushes then) while the DAC still holds some. */
static void test_both_drain(void)
{
    setup_bt();
    aout_bt_attach(&T, &S, &k_link, 0);
    aout_set_mode(AOUT_MODE_BOTH);
    memset(&D, 0, sizeof(D));
    g_src_idx = 0;
    CHECK(aout_start(16000, 31) == 0 && aout_route() == AOUT_ROUTE_BOTH, "both route");
    uint32_t tick = hp_tx_start(&T);
    int64_t next = g_now;
    for (int ms = 0; ms < 1000; ms++) {   /* 1 s of normal playback */
        g_now += 1000; dac_run_us(1000); stack_run();
        if (g_now >= next) { hp_tx_tick(&T); next += tick * 1000; }
        write_some(2);
    }
    uint32_t total = g_src_idx;
    CHECK(D.written < total, "the tap holds frames back while writing (%u of %u)", D.written, total);
    int drained_ms = -1, early = 0;
    for (int ms = 0; ms < 2000; ms++) {   /* silent: no writes, no aout_is_busy */
        g_now += 1000; dac_run_us(1000); stack_run();
        if (g_now >= next) { hp_tx_tick(&T); next += tick * 1000; }
        if (aout_queued_frames() == 0u && drained_ms < 0) {
            drained_ms = ms;
            early = D.n != 0 || hp_sink_tap_avail(&S) != 0u;
        }
    }
    printf("  BOTH drain without writes: queued 0 after %d ms, DAC got %u of %u frames\n", drained_ms, D.written, total);
    CHECK(D.written == total, "the DAC got every frame (%u of %u)", D.written, total);
    CHECK(drained_ms >= 0 && !early, "queued 0 only once the DAC played everything (at %d ms, early %d)", drained_ms,
          early);
    /* volume, audit r3 bug 2: an absolute-volume headset at unity must not take the DAC to 31 */
    aout_set_volume_split(9, 31, 0x1FF);
    CHECK(D.vol == 9 && HP_LD(&S.gain_q15) == 32768u, "split volume: DAC %d, headset gain %u", D.vol,
          HP_LD(&S.gain_q15));
    aout_set_volume(12, 0x1FF);
    CHECK(D.vol == 12 && HP_LD(&S.gain_q15) == 990u, "same volume on both: DAC %d, gain %u", D.vol, HP_LD(&S.gain_q15));
    aout_stop();
    hp_tx_stop(&T, HP_STOP_SUSPEND);
}

int main(void)
{
    test_routes();
    test_both_alignment();
    test_both_drain();
    printf("test_audio_out (BT=1): %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
#endif
