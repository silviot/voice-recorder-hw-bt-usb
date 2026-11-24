/*
 * Bluetooth plan Experiment 2: raw noise-source dumps for offline min-entropy estimation (plan M2 entropy).
 * Writes 16 KiB of raw 8-bit samples to out_buf (0x00858000); retrieve with --dump-out.
 *   arg & 0xFF = source:
 *     0  SARADC key ladder (keys_read_adc), low 8 bits of each 12-bit reading, back-to-back
 *     1  SARADC key ladder, low 8 bits, 200 us apart
 *     2  audio ADC (mic) low 8 bits of each 16-bit sample; gain = (arg >> 8) & 31 (default 31)
 *     3  timer jitter: low 8 bits of the us_timer delta across a fixed busy loop
 *     4  audio ADC high byte (to see the signal, not used as a source)
 *     5  FIRMWARE SOURCE, sequential: exactly what bt_port/bt_entropy_mic.c feeds bt_entropy (gain (arg >> 8) & 31,
 *        default 8; the first 2048 samples of the capture discarded unless arg bit 16 is set; symbol = sample & 3),
 *        packed 4 symbols per byte, first symbol in bits 1:0: 65536 contiguous symbols per run (4.1 s of audio)
 *     6  FIRMWARE SOURCE, restarts: 16 captures (start, discard 2048, 1000 symbols, stop) per run, one symbol per byte,
 *        16000 bytes = 16 rows of the SP 800-90B restart matrix (ea_restart needs 1000 rows: 63 runs)
 *     Sources 5/6 control flags (is the noise analogue? review finding: Exp 2 never ran the control):
 *       bit 17  PGA off: right after the capture starts, the AMIC1 PGA enables (CODEC_AMIC01_PGA 0x20430000) are
 *               cleared; the ADC keeps running on an unamplified, unbiased input
 *       bit 18  analogue path off: bit 17 plus ANA_CTRL0 0xC3000022 (analogue core, MICBIAS) cleared; 0x22 is put back
 *               after the capture (PB11 jack-detect bias, keys.c). Low-bit entropy that stays near the gain-8 value
 *               here comes from the digital side (dither / idle tones), not from the microphone path
 *       bit 19  gain field literal: (arg >> 8) & 31 used as given, so gain 0 can be captured (without it 0 = 8)
 *   Assessment recipe (SP800-90B_EntropyAssessment ea_non_iid / ea_restart): docs/BT_HW_RESULTS.md
 *   "Entropy source assessment"; tools/bt_entropy_unpack.py turns the dumps into the tool's input.
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

#define N 16384u
#define FW_GAIN      8u      /* = BT_ENT_MIC_GAIN */
#define FW_WARMUP    2048u   /* = BT_ENT_MIC_WARMUP_SAMPLES */
#define FW_MASK      3u      /* = BT_ENT_MIC_SYMBOL_MASK */
#define ROW          1000u   /* restart-matrix row length (SP 800-90B 3.1.4) */
#define ROWS         16u
#define CTL_PGA_OFF  (1u << 17)
#define CTL_ANA_OFF  (1u << 18)
#define CTL_GAIN_LIT (1u << 19)

/* One capture: skip `skip` samples, then store `n` symbols, packed 4 per byte if `pack`. Returns symbols stored. */
static uint32_t fw_capture(const openfw_exp_api_t *api, uint8_t *out, uint32_t n, uint32_t skip, uint8_t gain,
                           int pack, uint32_t *overruns, uint32_t ctl)
{
    uint32_t got = 0u;
    if (api->audio_capture_start(gain) != 0) {
        api->log_puts("capture_start failed\n");
        return 0u;
    }
    if (ctl & (CTL_PGA_OFF | CTL_ANA_OFF)) {        /* control runs: inside the warm-up window that is discarded */
        openfw_mmio_clr32(CODEC_AMIC01_PGA_ADDR, 0x20430000u);
    }
    if (ctl & CTL_ANA_OFF) {
        openfw_mmio_clr32(CODEC_ANA_CTRL0_ADDR, 0xC3000022u);
    }
    uint32_t t0 = api->millis();
    while (got < n && (uint32_t)(api->millis() - t0) < 8000u) {
        api->wdt_feed();
        uint32_t nb = 0u;
        const int16_t *f = api->audio_pop_frame(&nb);
        if (f == 0 || nb == 0u) {
            api->mdelay(2u);
            continue;
        }
        for (uint32_t i = 0u; i < nb / 2u && got < n; i++) {
            if (skip != 0u) {
                skip--;
                continue;
            }
            uint8_t sym = (uint8_t)((uint16_t)f[i] & FW_MASK);
            if (pack) {
                if ((got & 3u) == 0u) {
                    out[got >> 2] = 0u;
                }
                out[got >> 2] |= (uint8_t)(sym << (2u * (got & 3u)));
            } else {
                out[got] = sym;
            }
            got++;
        }
    }
    *overruns += api->audio_get_overruns();   /* per capture (reset by capture start): nonzero = gaps in the data */
    api->audio_capture_stop();
    if (ctl & CTL_ANA_OFF) {
        openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x22u);   /* jack-detect bias back (keys.c boot state) */
    }
    return got;
}

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint8_t *out = (uint8_t *)0x00858000u;
    uint32_t src = arg & 0xFFu, n = 0u;
    api->log_puts("[exp_entropy_dump] source=");
    api->log_u32(src);
    api->log_putc('\n');
    if (src == 0u || src == 1u) {
        for (n = 0u; n < N; n++) {
            out[n] = (uint8_t)api->keys_read_adc();
            if (src == 1u) {
                api->udelay(200u);
            }
            if ((n & 1023u) == 0u) {
                api->wdt_feed();
            }
        }
    } else if (src == 2u || src == 4u) {
        uint8_t gain = (uint8_t)((arg >> 8) & 31u);
        if (gain == 0u) {
            gain = 31u;
        }
        if (api->audio_capture_start(gain) != 0) {
            api->log_puts("capture_start failed\n");
            return 0u;
        }
        uint32_t t0 = api->millis();
        while (n < N && (uint32_t)(api->millis() - t0) < 5000u) {
            api->wdt_feed();
            uint32_t nb = 0u;
            const int16_t *f = api->audio_pop_frame(&nb);
            if (f == 0 || nb == 0u) {
                api->mdelay(2u);
                continue;
            }
            for (uint32_t i = 0u; i < nb / 2u && n < N; i++) {
                uint16_t v = (uint16_t)f[i];
                out[n++] = (src == 2u) ? (uint8_t)v : (uint8_t)(v >> 8);
            }
        }
        exp_log_kv_u32(api, "overruns", api->audio_get_overruns());
        api->audio_capture_stop();
    } else if (src == 5u || src == 6u) {
        uint8_t gain = (uint8_t)((arg >> 8) & 31u);
        if (gain == 0u && !(arg & CTL_GAIN_LIT)) {
            gain = FW_GAIN;
        }
        uint32_t ctl = arg & (CTL_PGA_OFF | CTL_ANA_OFF);
        uint32_t skip = (arg & (1u << 16)) ? 0u : FW_WARMUP, ov = 0u, sym = 0u;
        if (src == 5u) {
            sym = fw_capture(api, out, 4u * N, skip, gain, 1, &ov, ctl);
            n = (sym + 3u) / 4u;
        } else {
            for (uint32_t r = 0u; r < ROWS; r++) {
                uint32_t k = fw_capture(api, out + r * ROW, ROW, skip, gain, 0, &ov, ctl);
                sym += k;
                if (k != ROW) {
                    break;
                }
            }
            n = sym;
        }
        exp_log_kv_u32(api, "gain", gain);
        exp_log_kv_u32(api, "control", ctl >> 17);   /* 0 normal, 1 PGA off, 2/3 analogue path off */
        exp_log_kv_u32(api, "warmup_skipped", skip);
        exp_log_kv_u32(api, "symbols", sym);
        exp_log_kv_u32(api, "overruns", ov);
    } else if (src == 3u) {
        for (n = 0u; n < N; n++) {
            uint32_t a = (uint32_t)api->us_timer_now();
            for (volatile uint32_t k = 0u; k < 300u; k++) {
            }
            uint32_t b = (uint32_t)api->us_timer_now();
            out[n] = (uint8_t)(b - a);
            if ((n & 1023u) == 0u) {
                api->wdt_feed();
            }
        }
    }
    *api->out_len_ptr = n;
    exp_log_kv_u32(api, "bytes", n);
    return n;
}
