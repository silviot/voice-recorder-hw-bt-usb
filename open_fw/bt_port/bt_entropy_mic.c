/* Microphone-LSB noise source for bt_entropy (see bt_entropy_mic.h). Firmware only (BT=1 phone mode).
 *
 * Recording is never disturbed: the ADC has one owner. If the recorder holds it (saving, waiting for VOR, or an error
 * state that still captures), nothing is captured and BT_ENT_MIC_BUSY tells the caller (bt_xrec_fw.c: no credit this
 * BT session; no later collection while the radio runs). The REC switch aborts a running collection at once (the
 * BT-mode loop then leaves for the recorder: no lost audio beyond one 10 ms poll). Sampling the recorder's own frames instead was rejected: the
 * recorder runs the user's gain (Exp 2 measured gain 8 and 31 only), can be stereo, and its frames reach this module
 * only through a hook in recorder.c (default image) or audio_dma_peek_pcm, whose windows overlap (credit counted
 * twice). The capture used here is the recorder's own path (audio_capture_start_ex / audio_pop_frame /
 * audio_capture_stop), exactly what exp/exp_entropy_dump.c source 5 records for offline assessment.
 * SPDX-License-Identifier: MIT */
#include "bt_entropy_mic.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "openfw.h"
#include "sl6801_regs.h"
#include "bt_entropy.h"

static const bt_ent_source_cfg_t k_mic_cfg = BT_ENT_MIC_CFG_INIT;
static int s_id = -1;

const char *bt_entropy_mic_status_str(int st)
{
    switch (st) {
    case BT_ENT_MIC_OK: return "ok";
    case BT_ENT_MIC_BUSY: return "deferred (recorder owns the ADC)";
    case BT_ENT_MIC_TIMEOUT: return "timeout (too few ADC frames)";
    case BT_ENT_MIC_HEALTH_FAIL: return "HEALTH TEST FAILED (latched until reboot)";
    case BT_ENT_MIC_CLIPPED: return "rejected: input at full scale (loud sound / saturation), frame not credited";
    case BT_ENT_MIC_FLAT: return "rejected: no analogue noise in a frame, frame not credited";
    case BT_ENT_MIC_ABORTED: return "aborted (REC switch)";
    default: return "error";
    }
}

static int adc_busy(void)
{
    if (recorder_is_saving() || recorder_is_vor_waiting()) {
        return 1;
    }
    if (!recorder_has_error()) {
        return 0;
    }
    /* After a write error the recorder can still hold the capture without saving; after a failed start it does not.
     * Frames advancing over more than one DMA slot (128 ms) tell the two apart. */
    uint32_t f0 = audio_get_frames();
    vTaskDelay(pdMS_TO_TICKS(150));
    return audio_get_frames() != f0;
}

/* Back to the boot-time analogue state after the collection (review finding: every BT entry used to leave the mic PGA
 * and the ADC's analogue enables powered until reboot). Only what the capture start set on top of the boot state is
 * cleared: AMIC1 PGA enables (audio_amic_16k_init step 5) and ANA_CTRL0's high enables (0xC0000000). ANA_CTRL0 0x22,
 * the codec clock / global enable and DAC_ANA_CFG0[9] stay: keys.c sets them at boot for the PB11 jack detect. The APLL
 * stays as after a playback (audio_dac_stop leaves it too; its power-down is not traced). The next recorder or player
 * start re-initialises all of it. */
static void mic_analog_off(void)
{
    openfw_mmio_clr32(CODEC_AMIC01_PGA_ADDR, 0x20430000u);
    openfw_mmio_clr32(CODEC_ANA_CTRL0_ADDR, 0xC0000000u);
}

int bt_entropy_mic_collect(void)
{
    if (s_id < 0) {
        s_id = bt_entropy_register_source(&k_mic_cfg);
        if (s_id < 0) {
            return BT_ENT_MIC_ERROR;
        }
    }
    if (adc_busy()) {
        return BT_ENT_MIC_BUSY;
    }
    if (keys_read_rec_switch() && bt_entropy_ready()) {
        return BT_ENT_MIC_ABORTED;
    }
    uint32_t t0 = millis();
    if (audio_capture_start_ex(BT_ENT_MIC_GAIN, 1u) != 0) {
        return BT_ENT_MIC_ERROR;
    }
    uint32_t skip = BT_ENT_MIC_WARMUP_SAMPLES, fed = 0u, counts[4] = { 0u, 0u, 0u, 0u }, peak = 0u;
    int rc = BT_ENT_MIC_TIMEOUT;
    uint8_t sym[128];
    while (fed < BT_ENT_MIC_SAMPLES && (uint32_t)(millis() - t0) < BT_ENT_MIC_TIMEOUT_MS) {
        wdt_feed();
        if (keys_read_rec_switch() && bt_entropy_ready()) {
            rc = BT_ENT_MIC_ABORTED;       /* the user wants to record: hand the ADC over now */
            goto stop;
        }
        uint32_t nb = 0u;
        const int16_t *f = audio_pop_frame(&nb);   /* the slot is processed at once, long before the ring wraps */
        if (f == NULL || nb < 2u) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint32_t n = nb / 2u;
        bt_entropy_mix_uncredited(f, nb);          /* whole frame, zero credit */
        uint32_t i = 0u;
        if (skip != 0u) {
            i = skip < n ? skip : n;
            skip -= i;
        }
        uint32_t use = n - i;                      /* samples of this frame that would be credited */
        if (use > BT_ENT_MIC_SAMPLES - fed) {
            use = BT_ENT_MIC_SAMPLES - fed;
        }
        uint32_t fpk = 0u;
        int gate = bt_entropy_mic_frame_check(&f[i], use, &fpk);
        if (fpk > peak) {
            peak = fpk;
        }
        if (gate != BT_ENT_MIC_OK) {
            rc = gate;                             /* nothing of this frame credited; earlier frames passed it */
            goto stop;
        }
        while (i < n && fed < BT_ENT_MIC_SAMPLES) {
            uint32_t m = 0u;
            while (m < sizeof(sym) && i < n && fed + m < BT_ENT_MIC_SAMPLES) {
                sym[m] = (uint8_t)((uint16_t)f[i++] & BT_ENT_MIC_SYMBOL_MASK);
                counts[sym[m]]++;
                m++;
            }
            if (bt_entropy_add_samples(s_id, sym, m) != BT_ENT_OK) {
                rc = BT_ENT_MIC_HEALTH_FAIL;
                goto stop;
            }
            fed += m;
        }
    }
    if (fed == BT_ENT_MIC_SAMPLES) {
        rc = BT_ENT_MIC_OK;
    }
stop:
    audio_capture_stop();
    mic_analog_off();
    for (uint32_t k = 0u; k < sizeof(sym); k++) {
        ((volatile uint8_t *)sym)[k] = 0u;
    }
    openfw_log_puts("BT entropy mic: ");
    openfw_log_u32(fed);
    openfw_log_puts(" symbols (0:");
    openfw_log_u32(counts[0]);
    openfw_log_puts(" 1:");
    openfw_log_u32(counts[1]);
    openfw_log_puts(" 2:");
    openfw_log_u32(counts[2]);
    openfw_log_puts(" 3:");
    openfw_log_u32(counts[3]);
    openfw_log_puts(") peak ");
    openfw_log_u32(peak);
    openfw_log_puts(" in ");
    openfw_log_u32((uint32_t)(millis() - t0));
    openfw_log_puts(" ms, ");
    openfw_log_puts(bt_entropy_mic_status_str(rc));
    openfw_log_putc('\n');
    return rc;
}
