/*
 * Experiment 4 (Audio ADC AMIC1 + DMAC CH1 Capture Check):
 * Captures 4 frames (512 ms @ 16 kHz mono 16-bit), copies the first 4,096
 * samples (8,192 B) into out_buf (0x00858000) for host retrieval, and logs
 * DC offset, AC mean abs amplitude, min/max, peak %, and DMA overruns.
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint8_t gain = (arg >= 1u && arg <= 31u) ? (uint8_t)arg : 24u;
    api->wdt_feed();
    *api->out_len_ptr = 0u;

    if (api->audio_capture_start(gain) != 0) {
        api->log_puts("[exp_adc] audio_capture_start failed\n");
        return 0xFFFFFFFFu;
    }

    uint32_t t0        = api->millis();
    uint32_t frames    = 0u;
    uint32_t total_smp = 0u;
    uint32_t copied_b  = 0u;
    int32_t  min_s     = 32767;
    int32_t  max_s     = -32768;
    int32_t  sum_s     = 0;

    while (frames < 4u && (uint32_t)(api->millis() - t0) < 1500u) {
        api->wdt_feed();
        uint32_t nbytes = 0u;
        const int16_t *pcm = api->audio_pop_frame(&nbytes);
        if (!pcm || nbytes == 0u) {
            api->mdelay(5u);
            continue;
        }
        uint32_t nsmp = nbytes / 2u;
        const uint8_t *raw = (const uint8_t *)pcm;
        uint32_t copy_limit = (api->out_size < 8192u) ? api->out_size : 8192u;
        if (copied_b < copy_limit) {
            uint32_t take = copy_limit - copied_b;
            if (take > nbytes) take = nbytes;
            for (uint32_t i = 0u; i < take; i++) {
                api->out_buf[copied_b + i] = raw[i];
            }
            copied_b += take;
        }
        for (uint32_t i = 0u; i < nsmp; i++) {
            int32_t s = pcm[i];
            if (s < min_s) min_s = s;
            if (s > max_s) max_s = s;
            sum_s += s;
        }
        total_smp += nsmp;
        frames++;
    }

    uint8_t  peak_pct = api->audio_get_peak_pct();
    uint32_t overruns = api->audio_get_overruns();
    uint32_t irq_frms = api->audio_get_frames();
    api->audio_capture_stop();

    *api->out_len_ptr = copied_b;
    int32_t dc_mean = (total_smp > 0u) ? (sum_s / (int32_t)total_smp) : 0;

    uint32_t ac_sum = 0u;
    uint32_t dumped_smp = copied_b / 2u;
    const int16_t *out_s = (const int16_t *)api->out_buf;
    for (uint32_t i = 0u; i < dumped_smp; i++) {
        int32_t d = (int32_t)out_s[i] - dc_mean;
        ac_sum += (uint32_t)(d < 0 ? -d : d);
    }
    uint32_t ac_avg = dumped_smp ? (ac_sum / dumped_smp) : 0u;

    api->log_puts("[exp_adc] gain=");
    api->log_u32(gain);
    api->log_puts(" popped=");
    api->log_u32(frames);
    api->log_puts(" irq_frames=");
    api->log_u32(irq_frms);
    api->log_puts(" overruns=");
    api->log_u32(overruns);
    api->log_puts(" peak_pct=");
    api->log_u32(peak_pct);
    api->log_putc('\n');

    api->log_puts("[exp_adc] min=");
    exp_log_i32(api, min_s);
    api->log_puts(" max=");
    exp_log_i32(api, max_s);
    api->log_puts(" dc_mean=");
    exp_log_i32(api, dc_mean);
    api->log_puts(" ac_abs_avg=");
    api->log_u32(ac_avg);
    api->log_puts(" out_bytes=");
    api->log_u32(copied_b);
    api->log_putc('\n');
    return frames;
}
