/* Host tests for the headphones transmit path (bt_port/hp_tx.c + hp_sbc.c + hp_sink.c; docs/BT_A2DP_DESIGN.md
 * §4.3-4.6, §8.1) on virtual time against a fake stack: audio_tx_q with patch 0034's cap and counters, a downstream
 * (BTA/L2CAP/controller) with a link rate, NOCP completions, a "NOCP stops" stall, suspend and flushing. The peer
 * side decodes every packet with the vendored SBC decoder (external/sbc/decoder, host-only).
 *
 * Gates: pacing within +-0.1 % over 60 s at 44.1/48 kHz for fpp 7/10/11; stall recovery <= 1.05x nominal and every
 * 200 ms window <= nominal + CREDIT_MAX + one packet, burst <= HP_MAX_PKT_PER_TICK; headroom rule; queue cap; no PCM
 * lost on ESP_FAIL; pads counted apart from real frames and is_busy settling at EOF under congestion; flush with and
 * without the txq drop (RTP ts rewind); suspend with packets downstream that never complete (rebase); per-packet
 * bitpool 35 <-> 24 decoding with a fixed fpp; SBC round-trip SNR; queued_frames vs ground truth; the credit-stall
 * watchdog; units (source-rate frames).
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "hp_tx.h"
#include "hp_selftest.h"
#include "hp_test_util.h"
#include "oi_codec_sbc.h"

/* ------------------------------------------------------------------ fake stack */
#define FQ 64
typedef struct { uint8_t *data; uint32_t len; uint16_t nf; uint32_t ts; int64_t done_at; uint32_t epoch; } fpkt_t;
typedef struct { uint32_t len; uint8_t data[]; } fbuf_t;

static struct {
    int64_t now;
    fpkt_t q[FQ]; int qh, qn;            /* audio_tx_q */
    fpkt_t d[FQ]; int dh, dn;            /* downstream: a2d_list + L2CAP + controller */
    hp_txq_stat_t st;
    int cap_len;                         /* "length > cap_len" refuses (patch 0034: 3) */
    int started, flushing, nocp_stop;
    int down_max;                        /* packets downstream before BTU stops dequeuing */
    int64_t air_us;                      /* link time per packet */
    int64_t last_done;
    uint32_t bufs_out, qmax;
    uint32_t refuse_next;                /* refuse the next N sends after can_send said yes (must not happen) */
    uint32_t alloc_fail_next;            /* fail the next N allocations */
    /* peer: SBC payload as received, plus per-packet metadata */
    uint8_t *peer; size_t peer_len, peer_cap;
    uint32_t peer_pkts, peer_frames, last_ts, ts_gaps, nf_bad;
    uint16_t want_nf;
    int have_ts;
    uint32_t epoch, old_after;           /* flush epochs: old-epoch frames the peer got after a flush */
} F;

static hp_tx_t T;
static hp_sink_t S;
static int16_t g_ring[8192];
static int g_events;

static int64_t f_now(void *c) { (void)c; return F.now; }
static int f_can_send(void *c) { (void)c; return F.started && !F.flushing; }
static void *f_alloc(void *c, uint32_t len, uint8_t **data)
{
    (void)c;
    if (F.alloc_fail_next) { F.alloc_fail_next--; return NULL; }
    fbuf_t *b = malloc(sizeof(fbuf_t) + len);
    b->len = len;
    *data = b->data;
    F.bufs_out++;
    return b;
}
static void f_free(void *c, void *buf) { (void)c; free(buf); F.bufs_out--; }
static int f_send(void *c, void *buf, uint32_t len, uint16_t nf, uint32_t ts)
{
    (void)c;
    if (!F.started || F.flushing || F.qn > F.cap_len) return HP_SEND_FAIL;
    if (F.refuse_next) { F.refuse_next--; return HP_SEND_FAIL; }
    fbuf_t *b = buf;
    fpkt_t *p = &F.q[(F.qh + F.qn) % FQ];
    p->data = malloc(len);
    memcpy(p->data, b->data, len);
    p->len = len; p->nf = nf; p->ts = ts; p->epoch = F.epoch;
    F.qn++;
    if ((uint32_t)F.qn > F.qmax) F.qmax = (uint32_t)F.qn;
    F.st.enq_pkts++; F.st.enq_sbc += nf;
    free(buf);   /* the stack owns it now */
    F.bufs_out--;
    return HP_SEND_OK;
}
static void f_stat(void *c, hp_txq_stat_t *st) { (void)c; *st = F.st; st->qlen = (uint32_t)F.qn; }
static uint32_t f_drop(void *c)
{
    (void)c;
    uint32_t n = 0;
    while (F.qn) { fpkt_t *p = &F.q[F.qh]; n += p->nf; F.st.flushed_pkts++; F.st.flushed_sbc += p->nf; free(p->data); F.qh = (F.qh + 1) % FQ; F.qn--; }
    return n;
}
static void f_event(void *c, int e) { (void)c; if (e == HP_TX_EVT_STALL) g_events++; }
static const hp_tx_ops_t k_ops = { f_now, f_can_send, f_alloc, f_free, f_send, f_stat, f_drop, f_event, NULL };

static void peer_rx(const fpkt_t *p)
{
    if (F.peer_len + p->len > F.peer_cap) { F.peer_cap = (F.peer_cap + p->len) * 2; F.peer = realloc(F.peer, F.peer_cap); }
    memcpy(F.peer + F.peer_len, p->data, p->len);
    F.peer_len += p->len;
    F.peer_pkts++;
    F.peer_frames += p->nf;
    if (p->epoch < F.epoch) F.old_after += p->nf;
    if (F.want_nf && p->nf != F.want_nf) F.nf_bad++;
    if (F.have_ts && p->ts != F.last_ts) F.ts_gaps++;
    F.last_ts = p->ts + p->nf * 128u;
    F.have_ts = 1;
}

/* BTU + link + controller for one millisecond */
static void stack_step(void)
{
    while (F.qn && F.dn < F.down_max) {
        fpkt_t *p = &F.q[F.qh];
        F.qh = (F.qh + 1) % FQ; F.qn--;
        F.st.deq_pkts++; F.st.deq_sbc += p->nf;
        peer_rx(p);
        fpkt_t *d = &F.d[(F.dh + F.dn) % FQ];
        *d = *p;
        int64_t start = F.last_done > F.now ? F.last_done : F.now;
        d->done_at = start + F.air_us;
        F.last_done = d->done_at;
        F.dn++;
    }
    while (F.dn && !F.nocp_stop && F.d[F.dh].done_at <= F.now) {
        free(F.d[F.dh].data);
        F.dh = (F.dh + 1) % FQ; F.dn--;
        hp_tx_on_nocp(&T, 1);
    }
}

static void fake_reset(int64_t air_us, int down_max)
{
    for (int i = 0; i < F.qn; i++) free(F.q[(F.qh + i) % FQ].data);
    for (int i = 0; i < F.dn; i++) free(F.d[(F.dh + i) % FQ].data);
    free(F.peer);
    memset(&F, 0, sizeof(F));
    F.now = 1000000;
    F.cap_len = 3;
    F.started = 1;
    F.air_us = air_us;
    F.down_max = down_max;
    g_events = 0;
}

/* ------------------------------------------------------------------ source (the app task) */
static uint32_t g_src_rate, g_written;
static int g_feed = 1;
static double g_tone = 1000.0;
static int g_src_ch = 2;
static uint32_t g_feed_limit = 0xFFFFFFFFu;   /* EOF after this many frames */

static void feed(void)
{
    if (!g_feed) return;
    int16_t buf[2 * 256];
    for (;;) {
        uint32_t n = 256;
        if (g_written >= g_feed_limit) return;
        if (n > g_feed_limit - g_written) n = g_feed_limit - g_written;
        for (uint32_t i = 0; i < n; i++)
            for (int c = 0; c < g_src_ch; c++) buf[i * g_src_ch + c] = tone_sample(g_tone, g_src_rate, 0.5, g_written + i);
        uint32_t k = hp_sink_write(&S, buf, n, (uint8_t)g_src_ch);
        g_written += k;
        if (k < n) return;
    }
}

static hp_sbc_cfg_t cfg_of(uint32_t rate, uint8_t max_bp)
{
    hp_sbc_cfg_t c = { rate, HP_SBC_JOINT_STEREO, 16, 8, HP_SBC_LOUDNESS, 2, max_bp };
    return c;
}

static uint32_t mtu_for_fpp(uint32_t fpp, const hp_sbc_cfg_t *c) { return fpp * hp_sbc_frame_len(c, c->max_bp) + 5; }

static uint32_t g_tick_ms;
static int64_t g_next_tick;
static double g_jitter;   /* fraction of the period a tick may be late */
static unsigned g_seed = 1;

static void setup(uint32_t src_rate, int src_ch, uint32_t conn_rate, uint32_t fpp, uint8_t max_bp, const hp_tx_tun_t *tun,
                  int64_t air_us, int down_max)
{
    fake_reset(air_us, down_max);
    hp_sink_init(&S, g_ring, sizeof(g_ring));
    g_src_rate = src_rate; g_src_ch = src_ch; g_written = 0; g_feed = 1; g_feed_limit = 0xFFFFFFFFu;
    hp_sink_set_format(&S, src_rate, (uint8_t)src_ch);
    hp_tx_init(&T, &k_ops, &S, tun);
    hp_sbc_cfg_t c = cfg_of(conn_rate, max_bp);
    CHECK(hp_tx_set_cfg(&T, &c, mtu_for_fpp(fpp, &c)) == 0, "set_cfg");
    feed();
    g_tick_ms = hp_tx_start(&T);
    CHECK(g_tick_ms != 0, "start");
    CHECK(T.fpp == fpp, "fpp %u want %u", T.fpp, fpp);
    F.want_nf = (uint16_t)fpp;
    g_next_tick = F.now + (int64_t)g_tick_ms * 1000;
    g_jitter = 0;
}

/* advance virtual time by ms, ticking and feeding */
static void run_ms(uint32_t ms, int tick)
{
    for (uint32_t i = 0; i < ms; i++) {
        F.now += 1000;
        stack_step();
        if (tick && F.now >= g_next_tick) {
            feed();
            hp_tx_tick(&T);
            int64_t late = g_jitter > 0 ? (int64_t)((double)(rand_r(&g_seed) % 1000) / 1000.0 * g_jitter * g_tick_ms * 1000) : 0;
            g_next_tick += (int64_t)g_tick_ms * 1000;
            g_next_tick = g_next_tick + late - (late ? (int64_t)(g_jitter * g_tick_ms * 500) : 0);
        }
        if (!tick) g_next_tick = F.now + (int64_t)g_tick_ms * 1000;
    }
}

/* decode everything the peer received; returns decoded stereo frames (left in *pcm, malloc'd) */
static size_t peer_decode(int16_t **pcm)
{
    static OI_CODEC_SBC_DECODER_CONTEXT ctx;
    static OI_UINT32 data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
    OI_CODEC_SBC_DecoderReset(&ctx, data, sizeof(data), 2, 2, FALSE, FALSE);
    size_t cap = F.peer_frames * 128 * 2 + 256, n = 0;
    *pcm = malloc(cap * sizeof(int16_t));
    const OI_BYTE *p = F.peer;
    OI_UINT32 left = (OI_UINT32)F.peer_len;
    while (left > 0) {
        OI_UINT32 bytes = (OI_UINT32)((cap - n * 2) * sizeof(int16_t));
        OI_STATUS st = OI_CODEC_SBC_DecodeFrame(&ctx, &p, &left, *pcm + n * 2, &bytes);
        if (st != OI_OK) { CHECK(0, "decode status %d at %zu bytes left", (int)st, (size_t)left); break; }
        n += bytes / 4;
    }
    return n;
}

static double expected_frames(double secs, uint32_t rate) { return secs * rate / 128.0; }

/* ------------------------------------------------------------------ tests */

static void test_rate(void)
{
    const uint32_t rates[] = { 44100, 48000 }, fpps[] = { 7, 10, 11 };
    for (int r = 0; r < 2; r++) for (int f = 0; f < 3; f++) {
        setup(rates[r], 2, rates[r], fpps[f], 35, NULL, 600, 6);
        run_ms(1000, 1);
        uint32_t s0 = T.sent_sbc;
        run_ms(60000, 1);
        double got = T.sent_sbc - s0, want = expected_frames(60.0, rates[r]);
        double err = (got - want) / want;
        printf("  rate %u fpp %u: %.0f frames / %.0f expected (%+.4f %%), pads %u, max_burst %u, txq_max %u\n",
               rates[r], fpps[f], got, want, err * 100, T.st.pad_pcm, T.st.max_burst, F.qmax);
        CHECK(fabs(err) < 0.001, "rate error %.4f %%", err * 100);
        CHECK(T.st.pad_pcm == 0, "pads %u", T.st.pad_pcm);
        CHECK(F.qmax <= HP_TXQ_PKT_CAP, "queue %u > cap", F.qmax);
        CHECK(F.nf_bad == 0 && F.ts_gaps == 0, "nf_bad %u ts_gaps %u", F.nf_bad, F.ts_gaps);
    }
}

static void test_stall_recovery(void)
{
    const uint32_t fpps[] = { 7, 10, 11 };
    for (int f = 0; f < 3; f++) {
        setup(48000, 2, 48000, fpps[f], 35, NULL, 600, 6);
        run_ms(2000, 1);
        uint32_t before = T.st.max_burst;
        (void)before;
        run_ms(1000, 0);   /* the tick does not run for 1 s (BTC starved) */
        uint32_t s0 = T.sent_sbc;
        /* sample 200 ms windows over the 2 s after the stall */
        uint32_t win_max = 0, prev = s0;
        for (int w = 0; w < 10; w++) {
            run_ms(200, 1);
            uint32_t d = T.sent_sbc - prev;
            if (d > win_max) win_max = d;
            prev = T.sent_sbc;
        }
        double nominal2s = expected_frames(2.0, 48000), got = T.sent_sbc - s0;
        uint64_t cmax = hp_tx_credit_max(g_tick_ms, HP_CREDIT_TICKS, 48000, HP_MAX_PKT_PER_TICK, fpps[f], 128);
        double bound200 = expected_frames(0.2, 48000) + cmax / 128.0 + fpps[f];
        printf("  stall fpp %u: 2 s after = %.3fx nominal, worst 200 ms window %u frames (bound %.1f), burst %u\n",
               fpps[f], got / nominal2s, win_max, bound200, T.st.max_burst);
        CHECK(got / nominal2s <= 1.05, "recovery %.3fx", got / nominal2s);
        CHECK(win_max <= bound200, "window %u > %.1f", win_max, bound200);
        CHECK(T.st.max_burst <= HP_MAX_PKT_PER_TICK, "burst %u", T.st.max_burst);
        CHECK(T.st.tick_late >= 1, "late tick seen");
    }
}

static void test_headroom(void)
{
    CHECK(hp_tx_tick_for_headroom(30, 1, 7, 128, 48000) == 15, "cap 1, fpp 7: %u", hp_tx_tick_for_headroom(30, 1, 7, 128, 48000));
    CHECK(hp_tx_tick_for_headroom(30, 2, 7, 128, 48000) == 30, "cap 2 at 30 ms keeps it");
    CHECK(hp_tx_tick_for_headroom(40, 2, 7, 128, 48000) == 31, "cap 2, fpp 7: <= 31 ms");
    hp_tx_tun_t tun = { .tick_ms = 30, .max_pkt_per_tick = 1, .speech_bp = 0xFF, .flush_drops_txq = 0xFF };
    setup(48000, 2, 48000, 7, 35, &tun, 600, 6);
    CHECK(g_tick_ms == 15, "bench override lowered to %u", g_tick_ms);
    /* 5 % late tick jitter causes no pads with a full source */
    hp_tx_tun_t tun2 = { .speech_bp = 0xFF, .flush_drops_txq = 0xFF };
    setup(48000, 2, 48000, 7, 35, &tun2, 600, 6);
    g_jitter = 0.05;
    run_ms(20000, 1);
    CHECK(T.st.pad_pcm == 0, "jitter pads %u", T.st.pad_pcm);
    printf("  headroom: override 30 ms/1 pkt -> 15 ms; 5 %% jitter: %u pads\n", T.st.pad_pcm);
}

static void test_refusals_and_pads(void)
{
    /* the stack is flushing (suspend in progress) or out of memory: nothing is pulled, so nothing is lost */
    setup(48000, 2, 48000, 10, 35, NULL, 600, 6);
    run_ms(500, 1);
    F.flushing = 1;
    run_ms(100, 1);
    F.flushing = 0;
    F.alloc_fail_next = 3;
    run_ms(200, 1);
    CHECK(T.st.can_send_no >= 4 && T.st.alloc_fail == 3, "can_send_no %u alloc_fail %u", T.st.can_send_no, T.st.alloc_fail);
    CHECK(F.ts_gaps == 0, "ts gaps: %u", F.ts_gaps);
    g_feed = 0;
    run_ms(3000, 1);
    CHECK(T.st.real_pcm == g_written, "real %u written %u", T.st.real_pcm, g_written);
    CHECK(T.st.tx_lost_sbc == 0, "tx_lost_sbc %u", T.st.tx_lost_sbc);
    CHECK(T.st.pad_pcm > 0 && T.st.pad_pkts > 0, "pads after EOF counted apart: %u", T.st.pad_pcm);
    CHECK(F.bufs_out == 0, "buffers outstanding %u", F.bufs_out);
    /* a refusal after can_send (must not happen): counted, the tick pauses until the next STARTED, its real frames
     * stay unheard so a rewind replays them */
    setup(48000, 2, 48000, 10, 35, NULL, 600, 6);
    run_ms(300, 1);
    F.refuse_next = 1;
    run_ms(100, 1);
    CHECK(T.st.tx_lost_sbc == 10 && T.tx_paused, "lost %u paused %u", T.st.tx_lost_sbc, T.tx_paused);
    CHECK(T.lost_src > 0, "lost frames kept as unheard (%u)", T.lost_src);
    CHECK(hp_tx_start(&T) != 0 && !T.tx_paused, "resumes on the next STARTED");

    /* EOF under congestion: a slow link (NOCP 50 ms per packet); is_busy must settle while pads keep flowing */
    setup(48000, 2, 48000, 10, 35, NULL, 50000, 6);
    g_feed_limit = 48000;   /* 1 s of audio */
    run_ms(200, 1);
    int settled_at = -1;
    for (int ms = 0; ms < 8000; ms += 10) {
        run_ms(10, 1);
        if (settled_at < 0 && !hp_tx_app_is_busy(&T)) settled_at = ms;
    }
    printf("  EOF under congestion: is_busy settled after %d ms, pads %u, queue max %u\n", settled_at, T.st.pad_pcm, F.qmax);
    CHECK(settled_at >= 0, "is_busy never settled");
    CHECK(T.st.pad_pcm > 0, "pads flowed");
    CHECK(hp_tx_app_queued_frames(&T) == 0, "queued 0 at the end (%u)", hp_tx_app_queued_frames(&T));
    CHECK(T.st.tx_lost_sbc == 0, "tx_lost_sbc %u", T.st.tx_lost_sbc);
}

static void test_flush(void)
{
    for (int drop = 0; drop <= 1; drop++) {
        hp_tx_tun_t tun = { .speech_bp = 0xFF, .flush_drops_txq = (uint8_t)drop };
        /* slow drain so audio_tx_q is full when the seek comes */
        setup(48000, 2, 48000, 10, 35, &tun, 30000, 2);
        run_ms(1000, 1);
        CHECK(F.qn >= 3, "queue filled (%d)", F.qn);
        uint32_t ts0 = T.ts, q0 = (uint32_t)F.qn;
        hp_tx_request_flush(&T);
        F.epoch++;
        CHECK(hp_sink_queued(&S) == 0, "ring flushed at once");
        uint32_t sent_mark = T.sent_sbc;
        run_ms(20, 1);   /* the tick applies the flush */
        if (drop) {
            /* everything still in audio_tx_q when the tick ran (at most what was there at the request) */
            CHECK(T.st.flush_drop_sbc > 0 && T.st.flush_drop_sbc <= q0 * 10u, "dropped %u (queue had %u)", T.st.flush_drop_sbc, q0 * 10u);
            CHECK(T.ts == ts0 - T.st.flush_drop_sbc * 128u + (T.sent_sbc - sent_mark) * 128u, "ts rewind");
        }
        run_ms(2000, 1);
        uint32_t old_after = F.old_after;
        uint32_t bound = drop ? 2u * 10u : (HP_TXQ_PKT_CAP + 2u) * 10u;
        printf("  flush (txq drop %d): %u old frames reached the peer after the seek (bound %u), ts gaps %u\n", drop,
               old_after, bound, F.ts_gaps);
        CHECK(old_after <= bound, "old frames %u > %u", old_after, bound);
        CHECK(F.ts_gaps == 0, "RTP timestamps contiguous across the flush (%u gaps)", F.ts_gaps);
    }
}

static void test_suspend_rebase(void)
{
    setup(48000, 2, 48000, 10, 35, NULL, 600, 6);
    run_ms(500, 1);
    /* packets downstream that will never be NOCP'd: the suspend flushes L2CAP silently */
    F.nocp_stop = 1;
    run_ms(100, 1);
    CHECK(hp_tx_down_sbc(&T) > 0, "frames downstream");
    uint32_t q_before = hp_tx_app_queued_frames(&T);
    F.flushing = 1;
    f_drop(NULL);   /* stack flushes audio_tx_q on suspend (counted) */
    for (int i = 0; i < F.dn; i++) free(F.d[(F.dh + i) % FQ].data);
    F.dn = 0;       /* ... and L2CAP / a2d_list without any count */
    F.started = 0;
    hp_tx_stop(&T, HP_STOP_SUSPEND);
    CHECK(hp_tx_down_sbc(&T) == 0 && hp_tx_stack_sbc(&T) == 0, "rebased at stop: down %u stack %u", hp_tx_down_sbc(&T), hp_tx_stack_sbc(&T));
    uint32_t snap = hp_tx_app_queued_frames(&T);
    printf("  remote suspend: queued %u before, rewind snapshot %u (ring %u)\n", q_before, snap, hp_sink_queued(&S));
    CHECK(snap + 2u * 1280u >= q_before && snap <= q_before + 1280u, "snapshot %u vs live %u", snap, q_before);
    run_ms(1000, 1);   /* suspended: the snapshot stays frozen */
    CHECK(hp_tx_app_queued_frames(&T) == snap, "frozen");
    /* the player auto-pauses and rewinds: its flush ends the snapshot; queued is 0 like the DAC's */
    hp_tx_request_flush(&T);
    CHECK(hp_tx_app_queued_frames(&T) == 0, "0 after flush (%u)", hp_tx_app_queued_frames(&T));
    CHECK(!hp_tx_app_is_busy(&T), "not busy after flush");
    F.nocp_stop = 0;
    F.started = 1;
    F.flushing = 0;
    g_feed = 0;
    CHECK(hp_tx_start(&T) != 0, "restart");
    CHECK(hp_tx_down_sbc(&T) == 0, "down 0 after STARTED");
    run_ms(2000, 1);
    CHECK(g_events == 0, "no stall after resume (%d)", g_events);
    CHECK(!hp_tx_app_is_busy(&T), "is_busy clears after resume with an empty ring");
    CHECK(T.st.tx_lost_sbc == 0, "tx_lost_sbc %u", T.st.tx_lost_sbc);
}

/* rev 3 position model: heard = written - queued must never walk backwards while playing, pause silence and pads
 * never count, a flush gives 0, and the latency term decays during a pause. */
static void test_position(void)
{
    setup(16000, 1, 48000, 10, 35, NULL, 30000, 6);
    double last_heard = -1, worst_back = 0;
    for (int i = 0; i < 400; i++) {
        run_ms(5, 1);
        double heard = (double)g_written - hp_tx_app_queued_frames(&T);
        if (last_heard >= 0 && heard < last_heard && last_heard - heard > worst_back) worst_back = last_heard - heard;
        last_heard = heard;
    }
    /* pause: mute and stop writing; the ring drains (counts as heard, as with the DAC), then pause silence */
    hp_sink_set_mute(&S, 1);
    g_feed = 0;
    run_ms(2000, 1);
    uint32_t q_paused = hp_tx_app_queued_frames(&T);
    double heard_paused = (double)g_written - q_paused;
    CHECK(heard_paused >= last_heard, "pause walked back: %.0f < %.0f", heard_paused, last_heard);
    CHECK(q_paused == 0, "queued decays to 0 in pause silence (%u)", q_paused);
    CHECK(!hp_tx_app_is_busy(&T), "not busy in pause silence");
    /* unpause and pause repeatedly: the position must not drift */
    uint32_t w0 = g_written;
    for (int k = 0; k < 5; k++) {
        hp_sink_set_mute(&S, 0);
        g_feed = 1;
        run_ms(300, 1);
        hp_sink_set_mute(&S, 1);
        g_feed = 0;
        run_ms(600, 1);
    }
    run_ms(2000, 1);   /* let the ring and the link drain into pause silence */
    double drift = ((double)g_written - hp_tx_app_queued_frames(&T)) - g_written;
    printf("  position: worst step back %.0f frames while playing; after 5 pause/unpause cycles heard - written = %.0f\n",
           worst_back, drift);
    CHECK(worst_back <= 16.0, "heard walked back %.0f frames", worst_back);
    CHECK(fabs(drift) <= 1.0, "pause/unpause drift %.0f", drift);
    CHECK(g_written > w0, "played");
    /* a seek: queued 0 at once */
    g_feed = 1;
    hp_sink_set_mute(&S, 0);
    run_ms(300, 1);
    CHECK(hp_tx_app_queued_frames(&T) > 0, "queued while playing");
    hp_tx_request_flush(&T);
    CHECK(hp_tx_app_queued_frames(&T) == 0, "queued 0 right after a flush");
    /* link loss: the snapshot adds back the last HP_LINK_END_PKTS acked packets */
    run_ms(500, 1);
    uint32_t live = hp_tx_app_queued_frames(&T);
    hp_tx_stop(&T, HP_STOP_LINK);
    uint32_t snap = hp_tx_app_queued_frames(&T);
    printf("  link loss: live %u, snapshot %u (+ up to %u)\n", live, snap, HP_LINK_END_PKTS * 10u * 128u / 3u);
    CHECK(snap >= live && snap <= live + HP_LINK_END_PKTS * 10u * 128u / 3u + 64u, "link-loss snapshot %u live %u", snap, live);
    CHECK(T.st.tx_lost_sbc == 0, "tx_lost_sbc %u", T.st.tx_lost_sbc);
}

static void test_bitpool_switch_and_snr(void)
{
    /* 16 kHz mono source -> 48 kHz (exact 3x) with the speech bitpool, then a 48 kHz source at bitpool 35:
     * the same fpp throughout, every frame decodes. */
    hp_tx_tun_t tun = { .speech_bp = 24, .flush_drops_txq = 0xFF };
    setup(16000, 1, 48000, 10, 35, &tun, 600, 6);
    run_ms(3000, 1);
    int16_t *pcm;
    size_t n = peer_decode(&pcm);
    CHECK(n >= F.peer_frames * 128u - 128u, "decoded %zu of %u", n, F.peer_frames * 128u);
    double amp, snr = tone_snr(pcm + 48000 * 2, 48000, 2, 1000.0, 48000, &amp);
    printf("  16 kHz mono -> 48 kHz SBC bp 24: decoded %zu frames, SNR %.1f dB (L), amp %.3f\n", n, snr, amp);
    CHECK(snr >= 30.0, "speech-bitpool SNR %.1f", snr);
    free(pcm);
    /* format change to 48 kHz stereo mid-stream: bitpool goes to 35 at a packet boundary, fpp unchanged */
    g_src_rate = 48000; g_src_ch = 2; g_written = 0;
    hp_sink_set_format(&S, 48000, 2);
    run_ms(3000, 1);
    n = peer_decode(&pcm);
    CHECK(F.nf_bad == 0, "fpp changed (%u packets)", F.nf_bad);
    snr = tone_snr(pcm + (n - 48000) * 2, 48000, 2, 1000.0, 48000, &amp);
    printf("  -> 48 kHz stereo bp 35: SNR %.1f dB, amp %.3f, packets %u\n", snr, amp, F.peer_pkts);
    CHECK(snr >= 30.0, "bitpool-35 SNR %.1f", snr);
    free(pcm);
    /* 44.1 kHz round trip */
    setup(44100, 2, 44100, 7, 35, NULL, 600, 6);
    run_ms(2000, 1);
    n = peer_decode(&pcm);
    snr = tone_snr(pcm + 44100 * 2 / 2, 44100, 2, 1000.0, 44100, &amp);
    printf("  44.1 kHz stereo bp 35 round trip: SNR %.1f dB\n", snr);
    CHECK(snr >= 30.0, "44.1 SNR %.1f", snr);
    free(pcm);
}

static void test_queued_and_units(void)
{
    /* 16 kHz source on a 48 kHz link: queued_frames is in SOURCE frames and within one packet of the truth */
    setup(16000, 2, 48000, 10, 35, NULL, 20000, 6);   /* 20 ms per packet on the link: things downstream */
    uint32_t worst = 0;
    for (int i = 0; i < 300; i++) {
        run_ms(7, 1);
        /* truth: written minus heard; heard = NOCP-completed SBC frames converted to source frames, minus the
         * sink latency */
        uint32_t acked = HP_LD(&T.nocp_pkts) * 10u;
        double heard_src = acked * 128.0 * 16000 / 48000;
        double truth = g_written - heard_src + hp_tx_lat_ms(&T) * 16000 / 1000.0;
        double est = hp_tx_app_queued_frames(&T);
        uint32_t d = (uint32_t)fabs(est - truth);
        if (i > 60 && d > worst) worst = d;
    }
    uint32_t pkt_src = 10u * 128u * 16000u / 48000u;
    printf("  queued_frames vs truth: worst %u source frames (one packet = %u, plus a 64-frame chunk)\n", worst, pkt_src);
    CHECK(worst <= pkt_src + HP_SINK_CHUNK + 16u, "queued error %u", worst);
}

static void test_stall_watchdog(void)
{
    setup(48000, 2, 48000, 10, 35, NULL, 600, 6);
    run_ms(1000, 1);
    F.nocp_stop = 1;   /* link dead, no detach */
    int fired_ms = -1;
    for (int ms = 0; ms < 3000; ms += 10) {
        run_ms(10, 1);
        if (fired_ms < 0 && g_events) fired_ms = ms;
    }
    printf("  stall watchdog fired after %d ms (HP_STALL_MS %u)\n", fired_ms, HP_STALL_MS);
    CHECK(fired_ms >= (int)HP_STALL_MS - 40 && fired_ms <= (int)HP_STALL_MS + 100, "fired at %d", fired_ms);
    CHECK(g_events == 1 && T.stalled, "one event, tick latched");
    /* no false alarm on a healthy but slow link (NOCP every 100 ms) */
    setup(48000, 2, 48000, 10, 35, NULL, 100000, 4);
    run_ms(5000, 1);
    CHECK(g_events == 0, "slow healthy link: no stall");
}

static void test_cie(void)
{
    const uint8_t cie[4] = { 0x11, 0x15, 2, 35 };   /* 48 kHz, joint, 16 blocks, 8 subbands, loudness */
    hp_sbc_cfg_t c;
    CHECK(hp_sbc_cfg_from_cie(cie, &c) == 0 && c.rate == 48000 && c.ch_mode == HP_SBC_JOINT_STEREO && c.blocks == 16 &&
          c.subbands == 8 && c.alloc == HP_SBC_LOUDNESS && c.max_bp == 35, "cie parse");
    const uint8_t bad[4] = { 0x31, 0x15, 2, 35 };   /* two rates set: a capability, not a configuration */
    CHECK(hp_sbc_cfg_from_cie(bad, &c) != 0, "cie with two rates refused");
    hp_sbc_cfg_t j = cfg_of(48000, 35);
    CHECK(hp_sbc_frame_len(&j, 35) == 83 && hp_sbc_frame_len(&j, 53) == 119, "frame length");
    CHECK(hp_tx_fpp(659, 83) == 7 && hp_tx_fpp(882, 83) == 10 && hp_tx_fpp(992, 83) == 11 && hp_tx_fpp(5000, 83) == 15, "fpp");
    /* encoder output length matches the formula at every bitpool */
    hp_sbc_init(&j);
    uint8_t out[256];
    for (uint8_t bp = 2; bp <= 35; bp++) {
        memset(hp_sbc_pcm_buf(), 0, 256 * 2);
        CHECK(hp_sbc_encode(bp, out) == hp_sbc_frame_len(&j, bp), "len at bp %u", bp);
    }
}

static void test_selftest_golden(void)
{
    static hp_selftest_work_t w;
    uint32_t crc = hp_selftest_crc(&w);
    printf("  hp_selftest_crc = 0x%08x (golden 0x%08x; the RTOS self-test checks the same value on the ARM model)\n",
           crc, HP_SELFTEST_GOLDEN);
    CHECK(crc == HP_SELFTEST_GOLDEN, "host CRC differs from HP_SELFTEST_GOLDEN");
}

int main(void)
{
    test_selftest_golden();
    test_cie();
    test_headroom();
    test_rate();
    test_stall_recovery();
    test_refusals_and_pads();
    test_flush();
    test_suspend_rebase();
    test_bitpool_switch_and_snr();
    test_queued_and_units();
    test_position();
    test_stall_watchdog();
    fake_reset(0, 0);
    printf("test_hp_tx: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
