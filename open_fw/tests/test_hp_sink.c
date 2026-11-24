/* Host tests for bt_port/hp_sink.c (docs/BT_A2DP_DESIGN.md §3.3-3.4): SPSC ring, producer-side flush index,
 * format change, channel mapping, gain/mute, tap (output mode BOTH), resampler gates (SNR, DC gain, rate accuracy),
 * and a two-thread producer/consumer stress with random flushes (run under TSan by `make test-bt-hp`).
 * Usage: test_hp_sink [--stress-only] [seconds]
 * SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hp_sink.h"
#include "hp_test_util.h"

static int16_t g_store[8192];   /* 16 KiB = 4096 frames */

static void test_ring_basic(void)
{
    hp_sink_t s;
    hp_sink_init(&s, g_store, sizeof(g_store));
    CHECK(s.cap == 4096u, "cap %u", s.cap);
    hp_sink_set_format(&s, 48000, 2);
    hp_sink_set_out_rate(&s, 48000);   /* copy path */
    int16_t in[2 * 1000], out[2 * 1000];
    uint32_t idx = 0, got_idx = 0;
    for (int round = 0; round < 50; round++) {   /* wraps the ring many times */
        for (int i = 0; i < 700; i++) { in[2 * i] = (int16_t)(idx + i); in[2 * i + 1] = (int16_t)-(int16_t)(idx + i); }
        uint32_t k = hp_sink_write(&s, in, 700, 2);
        CHECK(k == 700u, "write %u", k);
        idx += k;
        CHECK(hp_sink_queued(&s) == 700u, "queued %u", hp_sink_queued(&s));
        uint32_t used = 0, r = hp_sink_pull(&s, out, 700, 2, &used);
        CHECK(r == 700u && used == 700u, "pull %u used %u", r, used);
        for (uint32_t i = 0; i < r; i++) {
            CHECK(out[2 * i] == (int16_t)(got_idx + i) && out[2 * i + 1] == (int16_t)-(int16_t)(got_idx + i), "data");
        }
        got_idx += r;
    }
    CHECK(hp_sink_queued(&s) == 0u, "drained");
    /* full ring: partial write, then 0 */
    for (int i = 0; i < 1000; i++) in[2 * i] = in[2 * i + 1] = 7;
    uint32_t tot = 0;
    for (int i = 0; i < 6; i++) tot += hp_sink_write(&s, in, 1000, 2);
    CHECK(tot == 4096u && hp_sink_free(&s) == 0u, "fill %u", tot);
    /* underrun pads zeros and returns the real count */
    hp_sink_flush(&s);
    CHECK(hp_sink_queued(&s) == 0u && hp_sink_free(&s) == 4096u, "flush frees at once");
    hp_sink_write(&s, in, 10, 2);
    memset(out, 0x55, sizeof(out));
    uint32_t r = hp_sink_pull(&s, out, 100, 2, NULL);
    CHECK(r == 10u && out[19] == 7 && out[20] == 0 && out[199] == 0, "pad r=%u", r);
    /* ch mismatch: 1-channel write into a stereo ring */
    CHECK(hp_sink_write(&s, in, 5, 1) == 0u && s.ch_mismatch == 1u, "mismatch");
}

static void test_flush_races(void)
{
    hp_sink_t s;
    hp_sink_init(&s, g_store, sizeof(g_store));
    hp_sink_set_format(&s, 16000, 1);
    hp_sink_set_out_rate(&s, 16000);
    int16_t a[100], b[100], out[400];
    for (int i = 0; i < 100; i++) { a[i] = 1000; b[i] = 2000; }
    /* flush, then write before the consumer runs: post-flush data survives, pre-flush is gone */
    hp_sink_write(&s, a, 100, 1);
    hp_sink_flush(&s);
    CHECK(hp_sink_queued(&s) == 0u, "queued after flush %u", hp_sink_queued(&s));
    hp_sink_write(&s, b, 60, 1);
    CHECK(hp_sink_queued(&s) == 60u, "queued %u", hp_sink_queued(&s));
    uint32_t r = hp_sink_pull(&s, out, 200, 1, NULL);
    CHECK(r == 60u, "pull %u", r);
    for (uint32_t i = 0; i < r; i++) CHECK(out[i] == 2000, "post-flush data %d at %u", out[i], i);
    /* double flush with writes in between, consumer runs only after both */
    hp_sink_write(&s, a, 100, 1);
    hp_sink_flush(&s);
    hp_sink_write(&s, a, 50, 1);
    hp_sink_flush(&s);
    hp_sink_write(&s, b, 30, 1);
    r = hp_sink_pull(&s, out, 200, 1, NULL);
    CHECK(r == 30u && out[0] == 2000 && out[29] == 2000, "double flush r=%u", r);
    /* flush while the consumer holds a partly used chunk: the rest of the chunk is dropped at the next pull */
    hp_sink_write(&s, a, 100, 1);
    r = hp_sink_pull(&s, out, 10, 1, NULL);   /* copies a 64-frame chunk, uses 10 */
    CHECK(r == 10u && hp_sink_queued(&s) == 90u, "held chunk counted: q=%u", hp_sink_queued(&s));
    hp_sink_flush(&s);
    CHECK(hp_sink_queued(&s) == 0u, "flush hides the held chunk: q=%u", hp_sink_queued(&s));
    hp_sink_write(&s, b, 20, 1);
    r = hp_sink_pull(&s, out, 100, 1, NULL);
    CHECK(r == 20u && out[0] == 2000, "after held-chunk flush r=%u out0=%d", r, out[0]);
    /* wrap with a flush index near the end */
    for (int k = 0; k < 200; k++) { hp_sink_write(&s, a, 37, 1); hp_sink_pull(&s, out, 37, 1, NULL); }
    hp_sink_write(&s, a, 100, 1);
    hp_sink_flush(&s);
    hp_sink_write(&s, b, 100, 1);
    r = hp_sink_pull(&s, out, 100, 1, NULL);
    CHECK(r == 100u && out[0] == 2000 && out[99] == 2000, "wrap flush");
}

static void test_format_and_mapping(void)
{
    hp_sink_t s;
    hp_sink_init(&s, g_store, sizeof(g_store));
    int16_t st[2 * 64], out[2 * 64];
    for (int i = 0; i < 64; i++) { st[2 * i] = 1000; st[2 * i + 1] = 3000; }
    /* stereo writes into a mono ring are downmixed; mono ring to stereo output duplicates */
    hp_sink_set_format(&s, 44100, 1);
    hp_sink_set_out_rate(&s, 44100);
    CHECK(hp_sink_write(&s, st, 64, 2) == 64u, "downmix write");
    uint32_t r = hp_sink_pull(&s, out, 64, 2, NULL);
    CHECK(r == 64u && out[0] == 2000 && out[1] == 2000, "mono->stereo %d %d", out[0], out[1]);
    /* stereo ring to mono output: (L+R)/2 */
    hp_sink_set_format(&s, 44100, 2);
    hp_sink_write(&s, st, 64, 2);
    r = hp_sink_pull(&s, out, 64, 1, NULL);
    CHECK(r == 64u && out[0] == 2000 && out[63] == 2000, "stereo->mono");
    /* gain and mute */
    hp_sink_set_gain_q15(&s, 16384);
    hp_sink_write(&s, st, 64, 2);
    r = hp_sink_pull(&s, out, 64, 2, NULL);
    CHECK(out[0] == 500 && out[1] == 1500, "gain %d %d", out[0], out[1]);
    hp_sink_set_mute(&s, 1);
    hp_sink_write(&s, st, 64, 2);
    r = hp_sink_pull(&s, out, 64, 2, NULL);
    CHECK(r == 64u && out[0] == 0 && hp_sink_queued(&s) == 0u, "mute drains and counts as real");
}

static void test_tap(void)
{
    hp_sink_t s;
    hp_sink_init(&s, g_store, sizeof(g_store));
    hp_sink_set_format(&s, 16000, 1);
    hp_sink_set_out_rate(&s, 16000);
    hp_sink_tap_enable(&s, 1);
    int16_t m[1000], out[2 * 1000];
    for (int i = 0; i < 1000; i++) m[i] = (int16_t)i;
    hp_sink_write(&s, m, 1000, 1);
    hp_sink_pull(&s, out, 1000, 2, NULL);   /* the headset reader is done ... */
    CHECK(s.cap == 8192u, "mono ring: 8192 frames in 16 KiB (%u)", s.cap);
    CHECK(hp_sink_free(&s) == 8192u - 1000u, "tap holds the space: free %u", hp_sink_free(&s));
    uint32_t k = hp_sink_tap_peek(&s, out, 300);
    CHECK(k == 300u && out[0] == 0 && out[1] == 0 && out[598] == 299 && out[599] == 299, "tap peek");
    CHECK(hp_sink_tap_avail(&s) == 1000u, "peek does not consume");
    hp_sink_tap_advance(&s, 300);
    CHECK(hp_sink_tap_avail(&s) == 700u && hp_sink_free(&s) == 8192u - 700u, "advance frees");
    hp_sink_flush(&s);
    CHECK(hp_sink_tap_avail(&s) == 0u && hp_sink_free(&s) == 8192u, "flush resets the tap");
    hp_sink_tap_enable(&s, 0);
}

/* ---- resampler gates (§3.4) ---- */
static double rs_snr(uint32_t src, uint32_t dst, uint8_t ch, double f, double *dc_gain, double *ratio_err)
{
    hp_rs_t rs;
    hp_rs_reset(&rs, src, dst, ch);
    const uint32_t n_in = src * 2u;   /* 2 s */
    int16_t *in = malloc((size_t)n_in * ch * sizeof(int16_t));
    int16_t *out = malloc((size_t)(n_in * (uint64_t)dst / src + 64u) * ch * sizeof(int16_t));
    for (uint32_t i = 0; i < n_in; i++) for (uint32_t c = 0; c < ch; c++) in[i * ch + c] = tone_sample(f, src, 0.5, i);
    uint32_t pos = 0, made = 0, pos_a = 0, made_a = 0;
    while (pos < n_in) {   /* feed in uneven pieces: state must carry across calls */
        uint32_t piece = 1u + (pos * 7u) % 97u, used = 0;
        if (piece > n_in - pos) piece = n_in - pos;
        made += hp_rs_process(&rs, in + (size_t)pos * ch, piece, &used, out + (size_t)made * ch, 100000);
        pos += used;
        if (!pos_a && pos >= n_in / 4u) { pos_a = pos; made_a = made; }
    }
    /* rate: output/input slope between 1/4 and the end (the 8-input lookahead of the general path is a constant
     * offset, not a rate error) */
    double expect = (double)(pos - pos_a) * dst / src;
    *ratio_err = fabs((double)(made - made_a) - expect) / expect;
    /* skip the filter start-up, measure 1 s */
    double amp, snr = tone_snr(out + (size_t)(dst / 4u) * ch, dst, ch, f, dst, &amp);
    /* DC gain: constant input */
    hp_rs_reset(&rs, src, dst, ch);
    for (uint32_t i = 0; i < n_in; i++) for (uint32_t c = 0; c < ch; c++) in[i * ch + c] = 16384;
    pos = 0; made = 0;
    while (pos < n_in) {
        uint32_t used = 0;
        made += hp_rs_process(&rs, in + (size_t)pos * ch, n_in - pos, &used, out + (size_t)made * ch, 100000);
        pos += used;
    }
    double sum = 0;
    for (uint32_t i = dst / 4u; i < dst / 4u + dst / 2u; i++) sum += out[i * ch];
    *dc_gain = sum / (dst / 2u) / 16384.0;
    free(in);
    free(out);
    return snr;
}

static void test_resampler(void)
{
    struct { uint32_t src, dst; double min_snr; } c[] = {
        { 16000, 48000, 60.0 },   /* exact 3x polyphase */
        { 44100, 48000, 55.0 },   /* general */
        { 16000, 44100, 55.0 },
        { 48000, 44100, 55.0 },   /* downsampling table */
        { 8000, 48000, 55.0 },
        { 22050, 44100, 55.0 },
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        for (uint8_t ch = 1; ch <= 2; ch++) {
            double dc, rerr, snr = rs_snr(c[i].src, c[i].dst, ch, 1000.0, &dc, &rerr);
            printf("  rs %5u -> %5u ch%u: SNR %.1f dB, DC gain %.5f, rate err %.6f %%\n", c[i].src, c[i].dst, ch, snr,
                   dc, rerr * 100.0);
            CHECK(snr > c[i].min_snr, "SNR %.1f < %.1f", snr, c[i].min_snr);
            CHECK(fabs(dc - 1.0) < 0.001, "DC gain %.5f", dc);
            CHECK(rerr < 0.0001, "rate error %.6f %%", rerr * 100.0);
        }
    }
    /* copy path is the identity */
    hp_rs_t rs;
    hp_rs_reset(&rs, 48000, 48000, 2);
    CHECK(rs.mode == HP_RS_COPY, "copy mode");
    hp_rs_reset(&rs, 16000, 48000, 2);
    CHECK(rs.mode == HP_RS_X3, "x3 mode");
}

/* ---- two-thread stress: producer writes (epoch, index) pairs with random flushes; the consumer (copy path) checks
 * that epochs never go back and indices are consecutive within an epoch, i.e. no torn or stale slot is ever
 * consumed. Run under TSan by the make target. ---- */
static hp_sink_t g_ss;
static int g_stop;   /* __atomic */
static uint32_t g_frames_out, g_torn;

static void *producer(void *arg)
{
    (void)arg;
    unsigned seed = 1234;
    int16_t buf[2 * 97];
    int16_t epoch = 0, idx = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        uint32_t n = 1u + (unsigned)rand_r(&seed) % 97u;
        for (uint32_t i = 0; i < n; i++) { buf[2 * i] = (int16_t)(idx + (int16_t)i); buf[2 * i + 1] = epoch; }
        uint32_t k = hp_sink_write(&g_ss, buf, n, 2);
        idx = (int16_t)(idx + (int16_t)k);
        if (rand_r(&seed) % 50u == 0u) {
            hp_sink_flush(&g_ss);
            epoch++;
            idx = 0;
        }
        (void)hp_sink_queued(&g_ss);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    int16_t out[2 * 128];
    int16_t last_epoch = -1, last_idx = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        uint32_t r = hp_sink_pull(&g_ss, out, 1u + g_frames_out % 128u, 2, NULL);
        for (uint32_t i = 0; i < r; i++) {
            int16_t e = out[2 * i + 1], x = out[2 * i];
            int16_t de = (int16_t)(e - last_epoch);   /* epochs wrap at 2^16 */
            if (de < 0 || (de == 0 && x != (int16_t)(last_idx + 1))) {
                g_torn++;
            }
            last_epoch = e;
            last_idx = x;
        }
        g_frames_out += r;
    }
    return NULL;
}

static void test_stress(double secs)
{
    static int16_t store[2 * 512];
    hp_sink_init(&g_ss, store, sizeof(store));
    hp_sink_set_format(&g_ss, 48000, 2);
    hp_sink_set_out_rate(&g_ss, 48000);
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELAXED);
    pthread_t p, c;
    pthread_create(&p, NULL, producer, NULL);
    pthread_create(&c, NULL, consumer, NULL);
    struct timespec ts = { (time_t)secs, (long)((secs - (double)(time_t)secs) * 1e9) };
    nanosleep(&ts, NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    printf("  stress: %u frames, %u torn-chunk retries, %u order violations\n", g_frames_out, g_ss.torn_chunks, g_torn);
    CHECK(g_torn == 0u, "order violations %u", g_torn);
    CHECK(g_frames_out > 10000u, "stress made progress (%u)", g_frames_out);
}

int main(int argc, char **argv)
{
    double secs = 2.0;
    int stress_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stress-only")) stress_only = 1;
        else secs = atof(argv[i]);
    }
    if (!stress_only) {
        test_ring_basic();
        test_flush_races();
        test_format_and_mapping();
        test_tap();
        test_resampler();
    }
    test_stress(secs);
    printf("test_hp_sink: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
