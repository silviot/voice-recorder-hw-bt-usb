/*
 * Bluetooth plan Experiment 0, part 1/2: fill the two candidate heap regions with an address-derived pattern.
 *   lower: 0x00805C00..0x0081A37F (stock FIRM code/data; untouched by open_fw)
 *   upper: 0x0086B000..0x0087FFFF (RTOS_HEAP; unused by the default, non-RTOS image)
 * Run on the DEFAULT (non-RTOS) DEV_EXP=1 image, then use the device (record, play back, menus, USB copy, legacy BT
 * screen), then run exp_sram_check.c. Any changed word means something else (ROM, DMA, CPU1, USB stack) uses the
 * range and it must not become heap. See docs/BLUETOOTH_BLUEDROID_PLAN.md M5 "Exp 0".
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"
#include "exp_sram_pattern.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    (void)arg;
    api->log_puts("[exp_sram_fill] Exp0 fill\n");
    uint32_t words = 0u;
    for (uint32_t r = 0u; r < SRAMPAT_NREGIONS; r++) {
        for (uint32_t a = k_srampat_regions[r].start; a < k_srampat_regions[r].end; a += 4u) {
            *(volatile uint32_t *)a = srampat_value(a);
            if ((a & 0xFFFu) == 0u) {
                api->wdt_feed();
            }
            words++;
        }
        exp_log_kv_hex(api, "filled_start", k_srampat_regions[r].start);
        exp_log_kv_hex(api, "filled_end", k_srampat_regions[r].end);
    }
    exp_log_kv_u32(api, "filled_words", words);
    exp_log_kv_u32(api, "fill_us_now_lo", (uint32_t)api->us_timer_now());
    api->log_puts("[exp_sram_fill] done: now use the device, then run exp_sram_check.c\n");
    return words;
}
