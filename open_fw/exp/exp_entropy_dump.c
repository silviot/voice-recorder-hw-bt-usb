/*
 * Bluetooth plan Experiment 2: raw noise-source dumps for offline min-entropy estimation (plan M2 entropy).
 * Writes 16 KiB of raw 8-bit samples to out_buf (0x00858000); retrieve with --dump-out.
 *   arg & 0xFF = source:
 *     0  SARADC key ladder (keys_read_adc), low 8 bits of each 12-bit reading, back-to-back
 *     1  SARADC key ladder, low 8 bits, 200 us apart
 *     2  audio ADC (mic) low 8 bits of each 16-bit sample; gain = (arg >> 8) & 31 (default 31)
 *     3  timer jitter: low 8 bits of the us_timer delta across a fixed busy loop
 *     4  audio ADC high byte (to see the signal, not used as a source)
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

#define N 16384u

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
