/*
 * Experiment 1 (Non-Interactive Hardware Probe):
 * Reads CPU_ID, eFuse words 0..3 & DAC DC trims, VBAT/VBUS/PMU registers,
 * PB6 record switch, PB11 headphone detect, and idle SARADC stats over 100 samples.
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    (void)arg;
    api->wdt_feed();
    api->log_puts("[exp_probe] SL6801 hardware state probe\n");

    exp_log_kv_u32(api, "cpu_id", REG32(SYSCTL_CPU_ID_ADDR));

    /* Enable eFuse reset 0x58 & clock 0x1B before reading words 0..3 */
    rom_rst_release(0x58u);
    rom_clk_enable(0x1Bu);
    uint32_t w0 = REG32(EFUSE_WORD_ADDR(0u));
    uint32_t w1 = REG32(EFUSE_WORD_ADDR(1u));
    uint32_t w2 = REG32(EFUSE_WORD_ADDR(2u));
    uint32_t w3 = REG32(EFUSE_WORD_ADDR(3u));
    exp_log_kv_hex(api, "efuse_w0", w0);
    exp_log_kv_hex(api, "efuse_w1", w1);
    exp_log_kv_hex(api, "efuse_w2", w2);
    exp_log_kv_hex(api, "efuse_w3", w3);
    exp_log_kv_u32(api, "dac0_dc_trim", ((w3 << 3) & 0x78u) | (w2 >> 29));
    exp_log_kv_u32(api, "dac1_dc_trim", (w3 >> 4) & 0x7Fu);

    exp_log_kv_u32(api, "vbat_mv", api->pmu_read_vbat_mv());
    exp_log_kv_u32(api, "vbus", (uint32_t)api->pmu_is_vbus_present());
    exp_log_kv_u32(api, "pb6_rec_sw", (uint32_t)api->keys_read_rec_switch());
    exp_log_kv_u32(api, "pb11_hp_det", (uint32_t)api->keys_read_hp_detect());

    static const uint8_t regs[] = { 0x00u, 0x1Au, 0x20u, 0x21u, 0x23u, 0x24u, 0x2Au, 0x31u };
    api->log_puts("pmu_regs=");
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(regs)); i++) {
        if (i > 0u) {
            api->log_putc(' ');
        }
        api->log_hex(regs[i]);
        api->log_putc(':');
        api->log_hex(api->pmu_read_reg(regs[i]));
    }
    api->log_putc('\n');

    uint32_t min_a = 4095u, max_a = 0u, sum_a = 0u;
    for (uint32_t i = 0u; i < 100u; i++) {
        uint16_t a = api->keys_read_adc();
        if (a < min_a) min_a = a;
        if (a > max_a) max_a = a;
        sum_a += a;
        api->wdt_feed();
        api->mdelay(2u);
    }
    api->log_puts("idle_adc min=");
    api->log_u32(min_a);
    api->log_puts(" max=");
    api->log_u32(max_a);
    api->log_puts(" avg=");
    api->log_u32(sum_a / 100u);
    api->log_putc('\n');
    return 0u;
}
