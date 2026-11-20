/*
 * Smartlink SL6801 Bare-Metal PMU RTWI Driver (0x40085100, Slave 0x30)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

int pmu_write_reg(uint8_t reg, uint8_t val)
{
    openfw_mmio_wr32(RTWI_WDATA_ADDR, (uint32_t)val);
    uint32_t cmd = 0x40000060u | ((uint32_t)reg << 8);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd | (1u << 31));
    while ((int32_t)openfw_mmio_rd32(RTWI_CMD_ADDR) < 0) {
    }
    return (openfw_mmio_rd32(RTWI_STATUS_ADDR) & 0xF000u) ? 1 : 0;
}

uint8_t pmu_read_reg(uint8_t reg)
{
    uint32_t cmd = 0x40000061u | ((uint32_t)reg << 8);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd | (1u << 31));
    while ((int32_t)openfw_mmio_rd32(RTWI_CMD_ADDR) < 0) {
    }
    return (uint8_t)(openfw_mmio_rd32(RTWI_RDATA_ADDR) & 0xFFu);
}

uint32_t pmu_read_vbat_mv(void)
{
    uint8_t r47 = pmu_read_reg(0x47u);
    pmu_write_reg(0x47u, (uint8_t)(r47 | 0x10u));
    uint32_t raw = (uint32_t)(pmu_read_reg(0x2Eu) & 0x7Fu);
    pmu_write_reg(0x47u, (uint8_t)(r47 & ~0x10u));
    return (raw * 14173u + 2800000u) / 1000u;
}

pmu_chg_state_t pmu_get_charge_state(void)
{
    uint8_t r2a = pmu_read_reg(0x2Au);
    if ((r2a & 0x04u) != 0u) {
        return PMU_CHG_CHARGING;
    }
    if ((r2a & 0x40u) != 0u) {
        return PMU_CHG_FULL;
    }
    return PMU_CHG_DISCHARGING;
}

int pmu_is_vbus_present(void)
{
    /*
     * Bit 2 (0x04) is asserted while charging from VBUS; bit 6 (0x40) stays
     * asserted when VBUS remains connected after reaching 100% full charge.
     */
    return (pmu_read_reg(0x2Au) & 0x44u) ? 1 : 0;
}

static uint8_t g_key_onoff_down = 0u;

void key_onoff_init(void)
{
    pmu_write_reg(0x30u, pmu_read_reg(0x30u));
    pmu_write_reg(0x31u, pmu_read_reg(0x31u));
    pmu_write_reg(0x4Au, pmu_read_reg(0x4Au));
    pmu_write_reg(0x1Au, (uint8_t)(pmu_read_reg(0x1Au) & ~0x70u));
    g_key_onoff_down = 0u;
}

int key_onoff_pressed(int *out_long_press)
{
    uint8_t st = pmu_read_reg(0x31u);
    if (st != 0u) {
        pmu_write_reg(0x31u, st); /* W1C */
        if (st & 0x40u) {
            g_key_onoff_down = 1u;
        }
        if (st & 0x10u) {
            g_key_onoff_down = 0u;
        }
        if (out_long_press) {
            *out_long_press = (st & 0x20u) ? 1 : 0;
        }
    } else if (out_long_press) {
        *out_long_press = 0;
    }
    return (int)g_key_onoff_down;
}

uint8_t key_onoff_poll(void)
{
    uint8_t st = pmu_read_reg(0x31u);
    uint8_t ev = 0u;
    if (st != 0u) {
        pmu_write_reg(0x31u, st); /* W1C */
        if (st & 0x40u) {
            g_key_onoff_down = 1u;
            ev |= KEY_ONOFF_PRESS;
        }
        if (st & 0x20u) {
            ev |= KEY_ONOFF_LONG;
        }
        if (st & 0x10u) {
            g_key_onoff_down = 0u;
            ev |= KEY_ONOFF_RELEASE;
        }
    }
    return ev;
}

void pmu_soft_poweroff(void)
{
    pmu_write_reg(0x20u, 0x2Fu); /* wake: KEY_ONOFF + VBUS */
    pmu_write_reg(0x22u, 0x00u);
    pmu_write_reg(0x24u, (uint8_t)((pmu_read_reg(0x24u) & ~0x3Fu) | 0x04u)); /* 1s on, 8s force-rst */
    pmu_write_reg(0x00u, (uint8_t)(pmu_read_reg(0x00u) & ~0x06u));
    pmu_write_reg(0x30u, pmu_read_reg(0x30u));
    pmu_write_reg(0x31u, pmu_read_reg(0x31u));
    pmu_write_reg(0x4Au, pmu_read_reg(0x4Au));

    openfw_mmio_wr32(AON_RTC_BASE + 0xACu, 0x30u);
    openfw_mmio_wr32(AON_RTC_BASE + 0xA4u, (openfw_mmio_rd32(AON_RTC_BASE + 0xA4u) & ~0x60u) | 0x53u);
    openfw_mmio_wr32(AON_RTC_BASE + 0xBCu, 0x8020u);
    uint32_t rc32k = openfw_mmio_rd32(AON_RTC_BASE + 0x84u);
    if (rc32k != 0u) {
        uint32_t t = 1000000u / rc32k;
        if (t == 0u) {
            t = 1u;
        }
        openfw_mmio_wr32(AON_RTC_BASE + 0xDCu, 0x0A358000u | (3000u / t));
        openfw_mmio_wr32(AON_RTC_BASE + 0xE0u, 0x00800060u | ((500u / t) << 24));
        openfw_mmio_wr32(AON_RTC_BASE + 0xE4u, 0x03700000u | ((200u / t) << 8) | (400u / t));
    }

    openfw_mmio_clr32(SYSTICK_CSR_ADDR, 0x3u);
    {
        uint32_t pm;
        OPENFW_IRQ_SAVE(pm);
        (void)pm;
    }

    for (uint32_t c = 0x11u; c <= 0x20u; c++) {
        if (c != 0x15u) {
            rom_clk_disable(c);
        }
    }
    rom_clk_set_src(CRU_CLK_CPU, CRU_CLK_SRC_HOSC24M);
    rom_clk_set_src(CRU_CLK_BUS6, CRU_CLK_SRC_HOSC24M);
    rom_clk_set_src(CRU_CLK_NOR, CRU_CLK_SRC_HOSC24M);
    mdelay(5u);
    for (uint32_t c = 3u; c <= 6u; c++) {
        rom_clk_div(c, 1u);
    }
    mdelay(5u);
    rom_clk_disable(0x29u);
    rom_clk_disable(8u);
    rom_clk_disable(9u);
    rom_clk_disable(0x26u);
    mdelay(5u);
    rom_clk_disable(CRU_CLK_PLL0);
    rom_clk_disable(CRU_CLK_APLL);

    openfw_mmio_wr32(AON_RTC_BASE + 0x60u, 1u);
    openfw_mmio_clr32(AON_RTC_BASE + 0xD8u, 0x6u);
    pmu_write_reg(0x00u, (uint8_t)(pmu_read_reg(0x00u) & ~0xC0u));
    for (uint32_t i = 0u; i < 8u; i++) {
        openfw_mmio_wr32(NVIC_ICER_ADDR(i), 0xFFFFFFFFu);
        openfw_mmio_wr32(NVIC_ICPR_ADDR(i), 0xFFFFFFFFu);
    }

    openfw_mmio_clr32(AON_RTC_BASE + 0x80u, 0x80000000u);
    openfw_mmio_wr32(AON_RTC_BASE + 0x80u, 0x8FA00001u);

    uint8_t r21 = pmu_read_reg(0x21u);
    openfw_mmio_wr32(AON_RTC_BASE + 0xB0u, (((uint32_t)(r21 | 0x03u) & 0xFFu) << 8) | 0x21u);
    openfw_mmio_set32(AON_RTC_BASE + 0xD0u, 0x01u);
    uint32_t v_hi = (uint32_t)((r21 & 0xE7u) | 0x04u);
    uint32_t v_lo = v_hi & 0xFBu;
    openfw_mmio_wr32(AON_RTC_BASE + 0xB8u, (v_lo << 24) | (v_hi << 16) | 0x21u);
    pmu_write_reg(0x00u, (uint8_t)(pmu_read_reg(0x00u) | 0x08u));
    openfw_mmio_set32(AON_RTC_BASE + 0xD0u, 0x08u);

    while (1) {
        OPENFW_WFI();
    }
}

void pmu_unattended_init(void)
{
    /*
     * Clear HSW / rail-hold bits [2:1] while preserving bit 0 (DFU sub-flag),
     * bit 3 (0x08 rail/LDO power-hold on HW_76), and long-press rescue [7:6].
     */
    pmu_write_reg(0x00u, (uint8_t)(pmu_read_reg(0x00u) & ~0x06u));
    /* Match HAL_pmu_init wakeup/reset mask (KEY_ONOFF + VBUS, bit 7=0) and clear stale DFU flags */
    pmu_write_reg(0x20u, 0x2Fu);
    pmu_write_reg(0x22u, 0x00u);
    pmu_write_reg(0x23u, (uint8_t)(pmu_read_reg(0x23u) & ~0xC0u));
    /* Acknowledge any latched PMU status 0 IRQ flags (W1C) */
    pmu_write_reg(0x30u, pmu_read_reg(0x30u));
    if (pmu_is_vbus_present()) {
        pmu_write_reg(0x17u, (uint8_t)((pmu_read_reg(0x17u) & ~0x01u) | 0x04u));
    }
}

void pmu_poll_charge_manage(void)
{
    static int s_prev_vbus = -1;
    int vbus = pmu_is_vbus_present();
    if (vbus && s_prev_vbus != 1) {
        pmu_write_reg(0x17u, (uint8_t)((pmu_read_reg(0x17u) & ~0x01u) | 0x04u));
    }
    s_prev_vbus = vbus;

    uint8_t st30 = pmu_read_reg(0x30u);
    if (st30 != 0u) {
        pmu_write_reg(0x30u, st30); /* W1C */
        if (st30 & 0x02u) { /* PMU_CHG_FULL */
            pmu_write_reg(0x17u, (uint8_t)(pmu_read_reg(0x17u) & ~0x04u));
        }
    }
}

void pmu_reboot_to_usb_bootloader(void)
{
    uint8_t r23 = pmu_read_reg(0x23u);
    uint8_t r00 = pmu_read_reg(0x00u);
    pmu_write_reg(0x23u, (uint8_t)(r23 | 0xC0u));
    pmu_write_reg(0x00u, (uint8_t)(r00 | 0x01u));
#ifdef OPENFW_HOST_MMIO
    system_reboot();
    while (1) {
        OPENFW_WFI();
    }
#else
    /* Exact stock FIRM 0x00CEB8AE -> 0x0080CF9E PMU full-chip reset (PMU[0x20]=0xAF) */
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    (void)pm;
    pmu_write_reg(0x22u, 0x00u);
    pmu_write_reg(0x20u, 0xAFu);
    while (1) {
        OPENFW_NOP();
    }
#endif
}

void pmu_reboot_to_sd_ota(void)
{
    uint8_t r23 = pmu_read_reg(0x23u);
    uint8_t r00 = pmu_read_reg(0x00u);
    pmu_write_reg(0x23u, (uint8_t)(r23 | 0xC0u));
    pmu_write_reg(0x00u, (uint8_t)(r00 & ~0x01u));
#ifdef OPENFW_HOST_MMIO
    system_reboot();
    while (1) {
        OPENFW_WFI();
    }
#else
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    (void)pm;
    pmu_write_reg(0x22u, 0x00u);
    pmu_write_reg(0x20u, 0xAFu);
    while (1) {
        OPENFW_NOP();
    }
#endif
}
