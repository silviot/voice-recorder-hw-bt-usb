/* Host tests for bt_port/bt_drbg.c and bt_port/bt_entropy.c (plan M2, step 2).
 *   - HMAC_DRBG SHA-256 against NIST CAVP vectors (no reseed + PredictionResistance=False with reseed)
 *   - SP 800-90B health-test cutoffs verified against their definition (exact binomial tail, alpha = 2^-20)
 *   - repetition count / adaptive proportion failures latch; start-up samples earn no credit
 *   - fail-closed gate: no output before credit, after a health failure, or when a reseed is due without credit
 * SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_drbg.h"
#include "bt_entropy.h"
#include "bt_entropy_mic.h"
#include "vectors/hmac_drbg_sha256_cavp.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_cavp(const hmac_drbg_vec_t *v, size_t n, const char *name)
{
    uint8_t out[256];
    for (size_t i = 0; i < n; i++) {
        bt_drbg_t d;
        CHECK(v[i].returned_len <= sizeof(out), "%s[%zu] returned too long", name, i);
        CHECK(bt_drbg_instantiate(&d, v[i].entropy, v[i].entropy_len, v[i].nonce, v[i].nonce_len, v[i].pers,
                                  v[i].pers_len) == BT_DRBG_OK, "%s[%zu] instantiate", name, i);
        if (v[i].reseed) {
            CHECK(bt_drbg_reseed(&d, v[i].entropy_reseed, v[i].entropy_reseed_len, v[i].addl_reseed,
                                 v[i].addl_reseed_len) == BT_DRBG_OK, "%s[%zu] reseed", name, i);
        }
        CHECK(bt_drbg_generate(&d, out, v[i].returned_len, v[i].addl1, v[i].addl1_len) == BT_DRBG_OK,
              "%s[%zu] generate 1", name, i);
        CHECK(bt_drbg_generate(&d, out, v[i].returned_len, v[i].addl2, v[i].addl2_len) == BT_DRBG_OK,
              "%s[%zu] generate 2", name, i);
        CHECK(memcmp(out, v[i].returned, v[i].returned_len) == 0, "%s[%zu] output mismatch", name, i);
    }
    printf("  CAVP %s: %zu vectors checked\n", name, n);
}

static void test_drbg_api(void)
{
    bt_drbg_t d;
    uint8_t e[32] = { 1 }, nonce[16] = { 2 }, out[32];
    memset(&d, 0, sizeof(d));
    CHECK(bt_drbg_generate(&d, out, 32, NULL, 0) == BT_DRBG_ERR_NOT_INSTANTIATED, "generate before instantiate");
    CHECK(bt_drbg_instantiate(&d, e, 31, nonce, 16, NULL, 0) == BT_DRBG_ERR_BAD_ARG, "short entropy accepted");
    CHECK(bt_drbg_instantiate(&d, e, 32, nonce, 15, NULL, 0) == BT_DRBG_ERR_BAD_ARG, "short nonce accepted");
    CHECK(bt_drbg_instantiate(&d, e, 32, nonce, 16, NULL, 0) == BT_DRBG_OK, "instantiate");
    CHECK(bt_drbg_generate(&d, out, BT_DRBG_MAX_REQUEST + 1u, NULL, 0) == BT_DRBG_ERR_BAD_ARG, "oversized request");
    d.reseed_counter = BT_DRBG_RESEED_INTERVAL + 1u;
    CHECK(bt_drbg_generate(&d, out, 32, NULL, 0) == BT_DRBG_ERR_RESEED_REQUIRED, "reseed interval not enforced");
    CHECK(bt_drbg_reseed(&d, e, 32, NULL, 0) == BT_DRBG_OK, "reseed");
    CHECK(bt_drbg_generate(&d, out, 32, NULL, 0) == BT_DRBG_OK, "generate after reseed");
    bt_drbg_uninstantiate(&d);
    CHECK(bt_drbg_generate(&d, out, 32, NULL, 0) == BT_DRBG_ERR_NOT_INSTANTIATED, "generate after uninstantiate");
}

/* ---- SP 800-90B §4.4 cutoffs from the definition ---- */
static unsigned rct_cutoff(unsigned h_x16) { return 1u + (320u + h_x16 - 1u) / h_x16; } /* 1 + ceil(20/H) */

/* log P(X = k), X ~ Binomial(n, p) */
static long double log_binom_pmf(unsigned n, unsigned k, long double p)
{
    return lgammal(n + 1.0L) - lgammal(k + 1.0L) - lgammal(n - k + 1.0L) + k * logl(p) + (n - k) * log1pl(-p);
}

static long double upper_tail(unsigned n, unsigned c, long double p) /* P(X >= c) */
{
    long double s = 0.0L;
    for (unsigned k = c; k <= n; k++) {
        s += expl(log_binom_pmf(n, k, p));
    }
    return s;
}

static unsigned apt_cutoff(unsigned h_x16) /* smallest C with P(X >= C) <= 2^-20 */
{
    long double p = powl(2.0L, -(long double)h_x16 / 16.0L), alpha = powl(2.0L, -20.0L);
    for (unsigned c = 1; c <= BT_ENT_APT_WINDOW; c++) {
        if (upper_tail(BT_ENT_APT_WINDOW, c, p) <= alpha) {
            return c;
        }
    }
    return BT_ENT_APT_WINDOW + 1u;
}

static void test_cutoffs(void)
{
    static const unsigned hs[] = { 8, 16, 32, 64, 128 };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++) {
        unsigned c = apt_cutoff(hs[i]);
        long double p = powl(2.0L, -(long double)hs[i] / 16.0L), alpha = powl(2.0L, -20.0L);
        CHECK(upper_tail(BT_ENT_APT_WINDOW, c, p) <= alpha && upper_tail(BT_ENT_APT_WINDOW, c - 1u, p) > alpha,
              "APT cutoff for H=%u/16 not minimal", hs[i]);
        printf("  H=%5.2f bits/sample: RCT cutoff %u, APT cutoff %u (W=512)\n", hs[i] / 16.0, rct_cutoff(hs[i]), c);
    }
}

/* The firmware mic source (bt_entropy_mic.h): its constants are the definition's cutoffs for its claimed H, a first
 * collection seeds the DRBG and every later one is a reseed, and a stuck ADC (constant symbol) is caught by the RCT
 * well inside one collection. */
static void test_mic_cfg(void)
{
    static const bt_ent_source_cfg_t mic = BT_ENT_MIC_CFG_INIT;
    CHECK(mic.rct_cutoff == rct_cutoff(BT_ENT_MIC_H_X16) && mic.apt_cutoff == apt_cutoff(BT_ENT_MIC_H_X16),
          "mic cutoffs %u/%u, definition %u/%u", mic.rct_cutoff, mic.apt_cutoff, rct_cutoff(BT_ENT_MIC_H_X16),
          apt_cutoff(BT_ENT_MIC_H_X16));
    CHECK(BT_ENT_MIC_H_X16 <= 8u, "mic credit above 0.5 bit/symbol needs the NIST assessment first");
    CHECK((BT_ENT_MIC_SAMPLES - BT_ENT_STARTUP_SAMPLES) * BT_ENT_MIC_H_X16 / 16u >= BT_ENT_SEED_BITS,
          "first collection does not seed the DRBG");
    CHECK(BT_ENT_MIC_SAMPLES * BT_ENT_MIC_H_X16 / 16u >= BT_ENT_RESEED_BITS, "later collection does not reseed");
    CHECK(BT_ENT_MIC_RCT_CUTOFF < BT_ENT_MIC_SAMPLES / 4u, "RCT cannot fire within a collection");
    printf("  mic source: H=%.2f bits/symbol, RCT %u, APT %u; first collection credits %u bits\n",
           BT_ENT_MIC_H_X16 / 16.0, mic.rct_cutoff, mic.apt_cutoff,
           (BT_ENT_MIC_SAMPLES - BT_ENT_STARTUP_SAMPLES) * BT_ENT_MIC_H_X16 / 16u);
}

/* ---- entropy pool ---- */
static uint32_t xs = 0x12345678u;
static uint8_t rnd8(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return (uint8_t)(xs >> 24); }

static bt_ent_source_cfg_t make_cfg(unsigned h_x16)
{
    bt_ent_source_cfg_t c = { "test", (uint16_t)h_x16, (uint16_t)rct_cutoff(h_x16), (uint16_t)apt_cutoff(h_x16) };
    return c;
}

static int g_locks;
static void t_lock(void *c) { (void)c; g_locks++; }
static void t_unlock(void *c) { (void)c; g_locks--; }

static void feed_random(int id, size_t n)
{
    uint8_t buf[256];
    while (n) {
        size_t k = n < sizeof(buf) ? n : sizeof(buf);
        for (size_t i = 0; i < k; i++) {
            buf[i] = rnd8();
        }
        CHECK(bt_entropy_add_samples(id, buf, k) == BT_ENT_OK, "random samples rejected");
        n -= k;
    }
}

static void test_gate(void)
{
    static bt_ent_source_cfg_t cfg;
    cfg = make_cfg(64); /* claim 4 bits per sample */
    bt_ent_platform_t plat = { t_lock, t_unlock, NULL };
    bt_entropy_init(&plat);
    int id = bt_entropy_register_source(&cfg);
    CHECK(id == 0, "register");
    uint8_t a[32], b[32];
    CHECK(bt_entropy_get(a, 32) == BT_ENT_NOT_READY, "output before any entropy");

    bt_entropy_mix_uncredited("seed-file-contents", 18);
    feed_random(id, BT_ENT_STARTUP_SAMPLES);            /* start-up window: hashed, never credited */
    CHECK(!bt_entropy_ready(), "start-up samples were credited");
    feed_random(id, (BT_ENT_SEED_BITS / 4u) - 1u);     /* one sample short of 384 bits */
    CHECK(!bt_entropy_ready(), "ready one sample early");
    feed_random(id, 1);
    CHECK(bt_entropy_ready(), "not ready after 384 credited bits");
    CHECK(bt_entropy_get(a, 32) == BT_ENT_OK && bt_entropy_get(b, 32) == BT_ENT_OK, "get after ready");
    CHECK(memcmp(a, b, 32) != 0, "consecutive outputs equal");
    CHECK(g_locks == 0, "unbalanced lock");

    feed_random(id, BT_ENT_RESEED_BITS / 4u);
    CHECK(bt_entropy_get(a, 32) == BT_ENT_OK, "get with reseed credit");
    bt_ent_stats_t st;
    bt_entropy_get_stats(&st);
    CHECK(st.reseeds == 1u && st.instantiated, "reseed not performed (reseeds=%u)", st.reseeds);

    /* Reseed due and no fresh credit: fail closed, then recover when credit arrives. */
    for (unsigned i = 0; i < BT_DRBG_RESEED_INTERVAL + 1u; i++) {
        if (bt_entropy_get(a, 1) != BT_ENT_OK) {
            break;
        }
    }
    CHECK(bt_entropy_get(a, 32) == BT_ENT_NOT_READY, "output while reseed due without credit");
    CHECK(!bt_entropy_ready(), "ready while reseed due");
    feed_random(id, BT_ENT_RESEED_BITS / 4u);
    CHECK(bt_entropy_get(a, 32) == BT_ENT_OK, "no recovery after fresh credit");
}

static void test_rct(void)
{
    static bt_ent_source_cfg_t cfg;
    cfg = make_cfg(64);
    bt_entropy_init(NULL);
    int id = bt_entropy_register_source(&cfg);
    uint8_t run[64];
    memset(run, 0x5A, sizeof(run));
    CHECK(bt_entropy_add_samples(id, run, cfg.rct_cutoff - 1u) == BT_ENT_OK, "RCT fired below cutoff");
    uint8_t other = 0x11;
    CHECK(bt_entropy_add_samples(id, &other, 1) == BT_ENT_OK, "run break");
    CHECK(bt_entropy_add_samples(id, run, cfg.rct_cutoff) == BT_ENT_HEALTH_FAIL, "RCT did not fire at cutoff");
    uint8_t x[32];
    CHECK(bt_entropy_get(x, 32) == BT_ENT_HEALTH_FAIL, "output after health failure");
    feed_random(id, 0);
    CHECK(bt_entropy_add_samples(id, &other, 1) == BT_ENT_HEALTH_FAIL, "failure not latched");
    bt_ent_stats_t st;
    bt_entropy_get_stats(&st);
    CHECK(st.failed_source == 0 && st.failed_test == 1, "RCT failure not recorded");
}

static void test_apt(void)
{
    static bt_ent_source_cfg_t cfg;
    cfg = make_cfg(64);
    bt_entropy_init(NULL);
    int id = bt_entropy_register_source(&cfg);
    /* Window starting with 0x00, then alternate 0x00 with distinct values: no RCT run, APT count grows. */
    uint8_t buf[BT_ENT_APT_WINDOW];
    size_t n = 0;
    buf[n++] = 0x00;
    while (n < BT_ENT_APT_WINDOW) {
        buf[n] = (n & 1u) ? (uint8_t)(1u + (n % 200u)) : 0x00;
        n++;
    }
    int r = bt_entropy_add_samples(id, buf, n);
    CHECK(r == BT_ENT_HEALTH_FAIL, "APT did not fire on 50%% bias with H=4");
    bt_ent_stats_t st;
    bt_entropy_get_stats(&st);
    CHECK(st.failed_test == 2, "APT failure not recorded (test=%u)", st.failed_test);
}


/* Source sanity gate (bt_entropy_mic_frame_check; review finding): saturation alternating between the two full-scale
 * codes passes the SP 800-90B health tests at H = 0.5 (each symbol run below the RCT cutoff, ~50 % in the APT window),
 * so without the gate a clipped capture would be credited in full. */
static void test_mic_frame_gate(void)
{
    static int16_t f[2048];
    uint32_t pk = 0u;
    uint32_t r = 12345u;
    for (uint32_t i = 0; i < 2048u; i++) {          /* quiet electronic noise: a few tens of LSB */
        r = r * 1103515245u + 12345u;
        f[i] = (int16_t)((int32_t)((r >> 16) % 81u) - 40);
    }
    CHECK(bt_entropy_mic_frame_check(f, 2048u, &pk) == BT_ENT_MIC_OK && pk <= 40u, "quiet noise refused (peak %u)",
          pk);
    f[100] = 31999;
    CHECK(bt_entropy_mic_frame_check(f, 2048u, &pk) == BT_ENT_MIC_OK && pk == 31999u, "31999 is not full scale");
    f[100] = -32000;
    CHECK(bt_entropy_mic_frame_check(f, 2048u, &pk) == BT_ENT_MIC_CLIPPED && pk == 32000u, "-32000 not clipped (%u)",
          pk);
    f[100] = 0;
    /* hard clipping of a loud tone: runs of 0x7FFF / 0x8000 (symbols 3 / 0) of 3..37 samples */
    static uint8_t sym[2048];
    uint32_t i = 0u, run = 3u;
    int hi = 1;
    while (i < 2048u) {
        for (uint32_t k = 0; k < run && i < 2048u; k++, i++) {
            f[i] = hi ? 32767 : -32768;
            sym[i] = (uint8_t)((uint16_t)f[i] & BT_ENT_MIC_SYMBOL_MASK);
        }
        hi = !hi;
        run = 3u + (run * 7u + 5u) % 35u;
    }
    static const bt_ent_source_cfg_t mic = BT_ENT_MIC_CFG_INIT;
    bt_entropy_init(NULL);
    int id = bt_entropy_register_source(&mic);
    CHECK(bt_entropy_add_samples(id, sym, 2048u) == BT_ENT_OK, "precondition: the health tests pass clipped input");
    bt_ent_stats_t st;
    bt_entropy_get_stats(&st);
    CHECK(st.credited_bits_total > 0u, "precondition: clipped input would be credited (%u bits)",
          st.credited_bits_total);
    CHECK(bt_entropy_mic_frame_check(f, 2048u, &pk) == BT_ENT_MIC_CLIPPED && pk == 32768u,
          "clipped frame not refused by the gate (peak %u)", pk);
    for (i = 0; i < 2048u; i++) {                   /* dead analogue path: only +-1..2 LSB of digital dither */
        f[i] = (int16_t)((int32_t)(i % 5u) - 2);
    }
    CHECK(bt_entropy_mic_frame_check(f, 2048u, &pk) == BT_ENT_MIC_FLAT && pk == 2u, "flat frame not refused");
    f[7] = 6;                                         /* peak-to-peak 8: plausible noise again */
    CHECK(bt_entropy_mic_frame_check(f, 2048u, NULL) == BT_ENT_MIC_OK, "ptp 8 refused");
    CHECK(bt_entropy_mic_frame_check(f, 0u, &pk) == BT_ENT_MIC_OK && pk == 0u, "empty slice");
    printf("  mic frame gate: clip >= %d, peak-to-peak >= %d\n", BT_ENT_MIC_CLIP_LEVEL, BT_ENT_MIC_PTP_MIN);
}

static void test_bad_args(void)
{
    static bt_ent_source_cfg_t bad = { "bad", 0, 2, 2 };
    bt_entropy_init(NULL);
    CHECK(bt_entropy_register_source(&bad) == BT_ENT_BAD_ARG, "H=0 accepted");
    CHECK(bt_entropy_add_samples(7, (const uint8_t *)"x", 1) == BT_ENT_BAD_ARG, "unknown source accepted");
    uint8_t x[8];
    CHECK(bt_entropy_get(x, BT_DRBG_MAX_REQUEST + 1u) == BT_ENT_BAD_ARG, "oversized get accepted");
}

int main(void)
{
    test_cavp(k_hmac_drbg_no_reseed, sizeof(k_hmac_drbg_no_reseed) / sizeof(k_hmac_drbg_no_reseed[0]), "no_reseed");
    test_cavp(k_hmac_drbg_pr_false, sizeof(k_hmac_drbg_pr_false) / sizeof(k_hmac_drbg_pr_false[0]), "pr_false");
    test_drbg_api();
    test_cutoffs();
    test_mic_cfg();
    test_gate();
    test_rct();
    test_apt();
    test_bad_args();
    test_mic_frame_gate();
    if (g_fail) {
        printf("test_bt_entropy: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_bt_entropy: all tests passed\n");
    return 0;
}
