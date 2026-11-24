/* Headphones audio core bit-exactness check (docs/BT_A2DP_DESIGN.md §10.1, milestone H1): a CRC32 over the SBC
 * encoder's output (48 kHz joint stereo, bitpool 35 <-> 24 every 10 frames) and the resampler's output (16 -> 48 kHz
 * exact 3x, 44.1 -> 48 kHz general path) for a deterministic integer-only input. tests/test_hp_tx.c computes it on the
 * host and checks HP_SELFTEST_GOLDEN; the BT=1 RTOS self-test (src/rtos/rtos_selftest.c, T_HPCORE, make
 * test-qemu-bt) computes it on the Cortex-M4 model, so host and ARM must agree bit for bit.
 * SPDX-License-Identifier: MIT */
#include "hp_selftest.h"
#include "hp_sbc.h"
#include "hp_sink.h"

static uint32_t crc32_upd(uint32_t crc, const void *data, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* integer-only test signal: a triangle at ~1 kHz plus a small LCG noise floor */
static int16_t sig(uint32_t n, uint32_t rate, uint32_t *lcg)
{
    uint32_t period = rate / 1000u, ph = n % period;
    int32_t tri = (int32_t)(ph < period / 2u ? ph : period - ph) * 40000 / (int32_t)period - 10000;
    *lcg = *lcg * 1664525u + 1013904223u;
    return (int16_t)(tri + (int32_t)((*lcg >> 24) & 0xFFu) - 128);
}

uint32_t hp_selftest_crc(hp_selftest_work_t *w)
{
    uint32_t crc = 0u, lcg = 1u;
    if (w == 0) {
        return 0u;
    }
    /* SBC: 375 frames = 1 s at 48 kHz */
    const hp_sbc_cfg_t c = { 48000u, HP_SBC_JOINT_STEREO, 16u, 8u, HP_SBC_LOUDNESS, 2u, 35u };
    if (hp_sbc_init(&c) != 0) {
        return 0u;
    }
    for (uint32_t f = 0; f < 375u; f++) {
        int16_t *pcm = hp_sbc_pcm_buf();
        for (uint32_t i = 0; i < 128u; i++) {
            pcm[2u * i] = sig(f * 128u + i, 48000u, &lcg);
            pcm[2u * i + 1u] = (int16_t)(pcm[2u * i] / 2);
        }
        uint32_t len = hp_sbc_encode((f / 10u) & 1u ? 24u : 35u, w->frame);
        crc = crc32_upd(crc, w->frame, len);
    }
    /* resampler through the ring: 16 kHz mono -> 48 kHz stereo, then 44.1 kHz stereo -> 48 kHz */
    static const uint32_t rates[2] = { 16000u, 44100u };
    for (uint32_t k = 0; k < 2u; k++) {
        const uint8_t ch = k == 0u ? 1u : 2u;
        hp_sink_init(&w->sink, w->ring, sizeof(w->ring));
        hp_sink_set_format(&w->sink, rates[k], ch);
        hp_sink_set_out_rate(&w->sink, 48000u);
        uint32_t n_in = 0, n_out = 0;
        while (n_out < 48000u / 4u) {   /* 250 ms of output */
            int16_t *in = w->in;
            for (uint32_t i = 0; i < 64u; i++) {
                in[i * ch] = sig(n_in + i, rates[k], &lcg);
                if (ch == 2u) {
                    in[i * ch + 1u] = (int16_t)-in[i * ch];
                }
            }
            n_in += hp_sink_write(&w->sink, in, 64u, ch);
            uint32_t r = hp_sink_pull(&w->sink, w->out, 128u, 2u, 0);
            crc = crc32_upd(crc, w->out, r * 4u);
            n_out += r;
        }
    }
    return crc;
}

_Static_assert(sizeof(hp_selftest_work_t) <= 4096u, "rtos_selftest.c allocates 4 KiB for the work area");
