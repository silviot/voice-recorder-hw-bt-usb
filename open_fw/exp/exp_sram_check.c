/*
 * Bluetooth plan Experiment 0, part 2/2: verify the pattern written by exp_sram_fill.c.
 * Logs per region: changed-word count, the first 8 changed addresses with expected/actual values, and a bitmap of
 * 1 KiB blocks containing any change (bit i of dirty_kb_wN = block 32*N+i from the region start).
 * A region is usable as heap only if changed == 0 after realistic use (record, play, menus, USB, legacy BT screen).
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"
#include "exp_sram_pattern.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    (void)arg;
    api->log_puts("[exp_sram_check] Exp0 verify\n");
    uint32_t total_changed = 0u;
    for (uint32_t r = 0u; r < SRAMPAT_NREGIONS; r++) {
        uint32_t start = k_srampat_regions[r].start, end = k_srampat_regions[r].end;
        uint32_t changed = 0u, shown = 0u;
        uint32_t dirty[3] = { 0u, 0u, 0u }; /* up to 96 KiB blocks */
        for (uint32_t a = start; a < end; a += 4u) {
            uint32_t v = *(volatile uint32_t *)a;
            uint32_t e = srampat_value(a);
            if (v != e) {
                changed++;
                uint32_t blk = (a - start) >> 10;
                if (blk < 96u) {
                    dirty[blk >> 5] |= 1u << (blk & 31u);
                }
                if (shown < 8u) {
                    api->log_puts("  diff addr=");
                    api->log_hex(a);
                    api->log_puts(" exp=");
                    api->log_hex(e);
                    api->log_puts(" got=");
                    api->log_hex(v);
                    api->log_putc('\n');
                    shown++;
                }
            }
            if ((a & 0xFFFu) == 0u) {
                api->wdt_feed();
            }
        }
        exp_log_kv_hex(api, "region_start", start);
        exp_log_kv_hex(api, "region_end", end);
        exp_log_kv_u32(api, "changed_words", changed);
        exp_log_kv_hex(api, "dirty_kb_w0", dirty[0]);
        exp_log_kv_hex(api, "dirty_kb_w1", dirty[1]);
        exp_log_kv_hex(api, "dirty_kb_w2", dirty[2]);
        total_changed += changed;
    }
    api->log_puts(total_changed == 0u ? "[exp_sram_check] RESULT=INTACT\n" : "[exp_sram_check] RESULT=CHANGED\n");
    return total_changed;
}
