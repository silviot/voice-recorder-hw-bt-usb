/*
 * Bluetooth plan Experiment 0 helper: exercise microphone DMA + SD writes between exp_sram_fill.c and
 * exp_sram_check.c (run exp_bt.c and exp_dac.c too, for the RW-BT ROM controller and DAC DMA).
 * arg = capture seconds (default 5). Writes three 8 KiB files of captured PCM to /EXP0/ on the SD card.
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

static int16_t s_pcm[4096]; /* 8 KiB */

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint32_t secs = (arg >= 1u && arg <= 30u) ? arg : 5u;
    api->log_puts("[exp_sram_exercise] mic DMA + SD writes\n");
    if (api->audio_capture_start(20u) != 0) {
        api->log_puts("capture_start failed\n");
    }
    uint32_t t0 = api->millis(), got = 0u, bytes_total = 0u;
    while ((uint32_t)(api->millis() - t0) < secs * 1000u) {
        api->wdt_feed();
        uint32_t nb = 0u;
        const int16_t *f = api->audio_pop_frame(&nb);
        if (f != 0 && nb != 0u) {
            uint32_t n = nb / 2u;
            for (uint32_t i = 0u; i < n && got < 4096u; i++) {
                s_pcm[got++] = f[i];
            }
            bytes_total += nb;
        } else {
            api->mdelay(2u);
        }
    }
    uint32_t frames = api->audio_get_frames(), overruns = api->audio_get_overruns();
    api->audio_capture_stop();
    exp_log_kv_u32(api, "captured_bytes", bytes_total);
    exp_log_kv_u32(api, "frames", frames);
    exp_log_kv_u32(api, "overruns", overruns);
    static const char *names[3] = { "EXP0_A.PCM", "EXP0_B.PCM", "EXP0_C.PCM" };
    for (uint32_t i = 0u; i < 3u; i++) {
        api->wdt_feed();
        int r = api->sd_write_file("EXP0", names[i], s_pcm, sizeof(s_pcm));
        api->log_puts("sd_write ");
        api->log_puts(names[i]);
        api->log_puts(" r=");
        exp_log_i32(api, r);
        api->log_putc('\n');
    }
    return bytes_total;
}
