/* Firmware noise source for bt_entropy (plan M2.5, docs/BT_HW_RESULTS.md Exp 2): the microphone ADC's two least
 * significant bits, captured through the recorder's own capture path (audio_dma.c) while the recorder is idle.
 *
 * Symbol: (uint16_t)pcm & 3 of each 16-bit mono sample at 16 kHz, analogue gain 8 (Exp 2's configuration).
 * Credit: BT_ENT_MIC_H_X16 = 8, i.e. 0.5 bit per 2-bit symbol. Exp 2 (16 KiB dumps, quick estimators: MCV upper
 * bound, binary Markov, lag-1, compression) measured ~0.95 bit per bit on each of bits 0..2 at gain 8, i.e. ~1.9 bits
 * per symbol; the provisional credit there was 1 bit/symbol. Until SP 800-90B ea_non_iid has run on >= 1M symbols
 * from several cold starts (recipe: docs/BT_HW_RESULTS.md "Entropy source assessment"), the credit is a quarter of the
 * measured value. A NIST min-entropy below 1 bit/symbol must lower this constant (keep it <= half of the assessed
 * h); the health-test cutoffs follow from it (RCT 41, APT 410 of 512 at H = 0.5, alpha = 2^-20,
 * tests/test_bt_entropy.c recomputes them).
 * Every collection discards BT_ENT_MIC_WARMUP_SAMPLES first (ADC / decimation-filter settling after the capture
 * starts), feeds BT_ENT_MIC_SAMPLES symbols to the health tests and the pool, and mixes the full 16-bit frames into
 * the pool with zero credit. First boot collection: 1024 start-up samples (no credit) + 3072 x 0.5 = 1536 credited
 * bits >= BT_ENT_SEED_BITS (384); every later one adds 2048 bits (reseed >= 320).
 * Source sanity gate (review finding): the 0.5 bit/symbol claim assumes low-level electronic noise. The RCT/APT only
 * catch a source stuck on one symbol, not a saturated input alternating between full-scale codes (0x7FFF -> symbol 3,
 * 0x8000 -> symbol 0) or a dead analogue path. So each DMA frame's used samples are checked BEFORE any of them is
 * credited (bt_entropy_mic_frame_check): a sample at or near full scale, or a peak-to-peak below a few LSB, ends the
 * collection without crediting that frame (BT_ENT_MIC_CLIPPED / BT_ENT_MIC_FLAT). Frames already credited were
 * themselves checked. The peak |x| of the collection is logged ("peak N").
 * Only the collection at BT-mode entry credits (radio off: the controller starts after it). There is no collection
 * while the radio is on (not measured by Exp 2), and the REC switch aborts a collection (BT_ENT_MIC_ABORTED).
 * SPDX-License-Identifier: MIT */
#ifndef BT_ENTROPY_MIC_H
#define BT_ENTROPY_MIC_H

#include <stdint.h>

#define BT_ENT_MIC_GAIN             8u      /* analogue gain index (0..31), Exp 2 */
#define BT_ENT_MIC_SYMBOL_MASK      0x03u   /* two LSBs per sample */
#define BT_ENT_MIC_H_X16            8u      /* 0.5 bit/symbol (see above) */
#define BT_ENT_MIC_RCT_CUTOFF       41u     /* 1 + ceil(20 / 0.5) */
#define BT_ENT_MIC_APT_CUTOFF       410u    /* 1 + CRITBINOM(512, 2^-0.5, 1 - 2^-20) */
#define BT_ENT_MIC_WARMUP_SAMPLES   2048u   /* one DMA slot (128 ms) discarded per capture */
#define BT_ENT_MIC_SAMPLES          4096u   /* symbols per collection (256 ms) */
#define BT_ENT_MIC_TIMEOUT_MS       1500u   /* whole collection, capture start to stop */
#define BT_ENT_MIC_CLIP_LEVEL       32000   /* |sample| at or above this: (near) full scale, the input saturates */
#define BT_ENT_MIC_PTP_MIN          8       /* peak-to-peak below this (LSB) over a frame: no analogue noise (Exp 2
                                             * found bit 2 near-uniform at gain 8, i.e. a spread far above 8 LSB) */

#define BT_ENT_MIC_CFG_INIT { "mic-lsb2", BT_ENT_MIC_H_X16, BT_ENT_MIC_RCT_CUTOFF, BT_ENT_MIC_APT_CUTOFF }

enum {
    BT_ENT_MIC_OK = 0,           /* samples fed (health tests passed); bt_entropy_ready() tells whether seeded */
    BT_ENT_MIC_BUSY = 1,         /* the recorder owns the ADC: nothing captured, call again when it is idle */
    BT_ENT_MIC_TIMEOUT = 2,      /* not enough frames within BT_ENT_MIC_TIMEOUT_MS (partial samples were fed) */
    BT_ENT_MIC_HEALTH_FAIL = 3,  /* SP 800-90B health test failed: bt_entropy latched until reboot */
    BT_ENT_MIC_ERROR = 4,        /* registration / capture start failed */
    BT_ENT_MIC_CLIPPED = 5,      /* a frame reached full scale (loud sound, saturation): that frame not credited */
    BT_ENT_MIC_FLAT = 6,         /* a frame without analogue noise (peak-to-peak < BT_ENT_MIC_PTP_MIN): not credited */
    BT_ENT_MIC_ABORTED = 7,      /* the REC switch went on: capture stopped at once for the recorder */
};

/* Sanity gate on the samples x[0..n) of one frame that are about to be credited (pure; host-tested in
 * tests/test_bt_entropy.c). Returns BT_ENT_MIC_OK, BT_ENT_MIC_CLIPPED or BT_ENT_MIC_FLAT; *peak (if not NULL) gets
 * max |x| (32768 for -32768). n == 0 is OK. */
static inline int bt_entropy_mic_frame_check(const int16_t *x, uint32_t n, uint32_t *peak)
{
    int32_t lo = 32767, hi = -32768;
    uint32_t pk = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        int32_t v = x[i];
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > pk) {
            pk = a;
        }
        if (v < lo) {
            lo = v;
        }
        if (v > hi) {
            hi = v;
        }
    }
    if (peak != NULL) {
        *peak = pk;
    }
    if (n == 0u) {
        return BT_ENT_MIC_OK;
    }
    if (pk >= (uint32_t)BT_ENT_MIC_CLIP_LEVEL) {
        return BT_ENT_MIC_CLIPPED;
    }
    if (hi - lo < BT_ENT_MIC_PTP_MIN) {
        return BT_ENT_MIC_FLAT;
    }
    return BT_ENT_MIC_OK;
}

/* App task only (owns the recorder and the ADC; blocks up to BT_ENT_MIC_TIMEOUT_MS with vTaskDelay, feeding the
 * watchdog; returns at once with BT_ENT_MIC_ABORTED when the REC switch goes on). bt_entropy_init() must have run
 * (bt_port_init). Registers the source on first use. Afterwards the mic PGA and the ADC-only analogue enables are off
 * again (boot state; the jack-detect bias stays), unless the recorder owned the ADC (BUSY: nothing touched). */
int bt_entropy_mic_collect(void);

/* Text for a BT_ENT_MIC_* status (log lines). */
const char *bt_entropy_mic_status_str(int st);

#endif /* BT_ENTROPY_MIC_H */
