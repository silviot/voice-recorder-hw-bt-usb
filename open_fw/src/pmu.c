/*
 * Smartlink SL6801 Bare-Metal PMU RTWI Driver (0x40085100, Slave 0x30)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

#define RTWI_WAIT_MAX 100000u

int pmu_write_reg(uint8_t reg, uint8_t val)
{
    openfw_mmio_wr32(RTWI_WDATA_ADDR, (uint32_t)val);
    uint32_t cmd = 0x40000060u | ((uint32_t)reg << 8);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd | (1u << 31));
    uint32_t wait = RTWI_WAIT_MAX;
    while ((int32_t)openfw_mmio_rd32(RTWI_CMD_ADDR) < 0) {
        if (--wait == 0u) {
            return 1;
        }
    }
    return (openfw_mmio_rd32(RTWI_STATUS_ADDR) & 0xF000u) ? 1 : 0;
}

uint8_t pmu_read_reg(uint8_t reg)
{
    uint32_t cmd = 0x40000061u | ((uint32_t)reg << 8);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd | (1u << 31));
    uint32_t wait = RTWI_WAIT_MAX;
    while ((int32_t)openfw_mmio_rd32(RTWI_CMD_ADDR) < 0) {
        if (--wait == 0u) {
            return 0u;
        }
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
    if (st & 0x70u) {
        openfw_log_puts("KEY_ONOFF ");
        openfw_log_u32(millis());
        openfw_log_putc(' ');
        openfw_log_hex(st);
        openfw_log_putc('\n');
    }
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

power_button_action_t power_button_update(uint8_t events, uint32_t now, int screen_off,
                                          uint8_t *progress)
{
    static int down, woke_screen, shutdown_armed;
    static uint32_t pressed_at, armed_at;
    int wake = 0;
    if (!down && (events & (KEY_ONOFF_PRESS | KEY_ONOFF_LONG))) {
        down = 1;
        pressed_at = now;
        shutdown_armed = 0;
        woke_screen = screen_off;
        wake = screen_off;
    }
    if (!down) {
        return POWER_BUTTON_IDLE;
    }
    /* A press started while the display was black only wakes the screen and
     * never shuts down — holding Power for 2s from a dark screen stays awake. */
    if (woke_screen) {
        if ((events & (KEY_ONOFF_RELEASE | KEY_ONOFF_LONG)) ||
            (uint32_t)(now - pressed_at) >= 2500u) {
            down = 0;
            woke_screen = 0;
        }
        return wake ? POWER_BUTTON_WAKE : POWER_BUTTON_IDLE;
    }
    /* A hardware PMU long-press arms shutdown immediately. */
    if (!shutdown_armed && (events & KEY_ONOFF_LONG)) {
        shutdown_armed = 1;
        armed_at = now;
    }
    /* ON/OFF is a wake source: prefer release after arming, with a 400 ms fallback
     * because SL6801 PMU SK (0x10) does not fire after LK (0x20). */
    if (shutdown_armed) {
        if ((events & KEY_ONOFF_RELEASE) || (uint32_t)(now - armed_at) >= 400u) {
            down = 0;
            shutdown_armed = 0;
            return POWER_BUTTON_SHUTDOWN;
        }
        if (progress) {
            *progress = 100u;
        }
        return POWER_BUTTON_PROGRESS;
    }
    uint32_t held = (uint32_t)(now - pressed_at);
    /* Check release BEFORE held >= 2000u so a delayed poll after a short press
     * never triggers an accidental shutdown. */
    if (events & KEY_ONOFF_RELEASE) {
        down = 0;
        if (held >= 300u) {
            return POWER_BUTTON_CANCEL;
        }
        return POWER_BUTTON_SLEEP;
    }
    if (held >= 2000u) {
        shutdown_armed = 1;
        armed_at = now;
        if (progress) {
            *progress = 100u;
        }
        return POWER_BUTTON_PROGRESS;
    }
    if (held >= 300u) {
        if (progress) {
            *progress = (uint8_t)(held / 20u);
        }
        return POWER_BUTTON_PROGRESS;
    }
    return POWER_BUTTON_IDLE;
}

/* Universal SL6801 PMU cold rail reset (SL6801_HW76_HARDWARE.md §5.2):
 * Drops and re-sequences all SoC rails cleanly (resets PLLs, CRU, USB PHY, XIP)
 * while preserving sticky PMU flags (PMU[0x23], PMU[0x00]). */
OPENFW_FASTFUNC __attribute__((noinline, noreturn))
static void pmu_cold_reset(void)
{
#ifndef OPENFW_HOST_MMIO
    cpu1_ui_stop();
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    (void)pm;
    pmu_write_reg(0x22u, 0x00u);
    pmu_write_reg(0x20u, 0x2Fu);
    uint8_t r1b = pmu_read_reg(0x1Bu);
    pmu_write_reg(0x1Bu, (uint8_t)(r1b | 0x80u));
    pmu_write_reg(0x1Bu, (uint8_t)(r1b & ~0x80u));
    pmu_write_reg(0x20u, 0xAFu);
    OPENFW_DSB();
    while (1) {
        OPENFW_NOP();
    }
#else
    system_reboot();
    while (1) {
        OPENFW_WFI();
    }
#endif
}

/* The HW_76 wakes straight back up when its PMU cuts power with VBUS high.
 * Keep the charger serviced with USB and the display off until unplug,
 * Record slider activation (PB6), or a new Power button press. */
static void wait_in_charging_standby(void)
{
    uint32_t start = millis();
    uint32_t last_vbus = start;
    uint32_t last_poll = start;
    uint32_t pressed_at = 0u;
    int new_press = 0;
    int prev_rec = keys_read_rec_switch();

    /* Discard any residual edge from the shutdown gesture and debounce 250 ms. */
    (void)key_onoff_poll();
    for (;;) {
        OPENFW_WFI();
        uint32_t now = millis();
        if ((uint32_t)(now - last_poll) < 50u) {
            continue;
        }
        last_poll = now;
        wdt_feed();
        if (pmu_is_vbus_present()) {
            last_vbus = now;
            pmu_poll_charge_manage();
        } else if ((uint32_t)(now - last_vbus) >= 500u) {
            return;
        }

        /* Activating the Record slider switches the recorder back on immediately,
         * even if moved right after entering charging standby. */
        int rec_now = keys_read_rec_switch();
        if (rec_now && !prev_rec) {
#ifndef OPENFW_HOST_MMIO
            AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC;
#endif
            pmu_cold_reset();
        }
        prev_rec = rec_now;

        uint8_t events = key_onoff_poll();
        /* Ignore residual shutdown button edges for the first 250 ms */
        if ((uint32_t)(now - start) < 250u) {
            continue;
        }

        /* Any deliberate Power press (clean tap, >=500 ms hold, or LK) powers back on */
        int wake_now = 0;
        if (events & KEY_ONOFF_PRESS) {
            new_press = 1;
            pressed_at = now;
        }
        if (events & KEY_ONOFF_LONG) {
            wake_now = 1;
        } else if (new_press && ((events & KEY_ONOFF_RELEASE) || (uint32_t)(now - pressed_at) >= 500u)) {
            wake_now = 1;
        }
        if (wake_now) {
#ifndef OPENFW_HOST_MMIO
            AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC;
#endif
            (void)key_onoff_poll();
            pmu_cold_reset();
        }
    }
}

OPENFW_RAMFUNC __attribute__((noinline))
static uint8_t pmu_ram_read_reg(uint8_t reg)
{
    uint32_t cmd = 0x40000061u | ((uint32_t)reg << 8);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd | (1u << 31));
    for (uint32_t t = 0u; t < RTWI_WAIT_MAX && (int32_t)openfw_mmio_rd32(RTWI_CMD_ADDR) < 0; t++) {
    }
    return (uint8_t)(openfw_mmio_rd32(RTWI_RDATA_ADDR) & 0xFFu);
}

OPENFW_RAMFUNC __attribute__((noinline))
static void pmu_ram_write_reg(uint8_t reg, uint8_t val)
{
    openfw_mmio_wr32(RTWI_WDATA_ADDR, (uint32_t)val);
    uint32_t cmd = 0x40000060u | ((uint32_t)reg << 8);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd);
    openfw_mmio_wr32(RTWI_CMD_ADDR, cmd | (1u << 31));
    for (uint32_t t = 0u; t < RTWI_WAIT_MAX && (int32_t)openfw_mmio_rd32(RTWI_CMD_ADDR) < 0; t++) {
    }
}

/* Executes purely from SRAM (.ramtext) + Mask ROM after tearing down NOR/PLL0 */
OPENFW_RAMFUNC __attribute__((noinline, noreturn))
static void pmu_soft_poweroff_ram(void)
{
#ifndef OPENFW_HOST_MMIO
    rom_rst_hold(CRU_RST_CPU1);
    NOR_XIP_CFG = NOR_XIP_CFG_1WIRE;
    rom_gpio_config(0x0000279Bu);
    rom_gpio_config(0x00002F9Bu);
    OPENFW_DSB();
#endif
    for (uint32_t c = 0x11u; c <= 0x20u; c++) {
        if (c != 0x15u) {
            rom_clk_disable(c);
        }
    }
    rom_clk_set_src(CRU_CLK_CPU, CRU_CLK_SRC_HOSC24M);
    rom_clk_set_src(CRU_CLK_BUS6, CRU_CLK_SRC_HOSC24M);
    rom_clk_set_src(CRU_CLK_NOR, CRU_CLK_SRC_HOSC24M);
    rom_us_delay(5000u);
    for (uint32_t c = 3u; c <= 6u; c++) {
        rom_clk_div(c, 1u);
    }
    rom_us_delay(5000u);
    rom_clk_disable(0x29u);
    rom_clk_disable(8u);
    rom_clk_disable(9u);
    rom_clk_disable(0x26u);
    rom_us_delay(5000u);
    rom_clk_disable(CRU_CLK_PLL0);
    rom_clk_disable(CRU_CLK_APLL);

    openfw_mmio_wr32(AON_RTC_BASE + 0x60u, 1u);
    openfw_mmio_clr32(AON_RTC_BASE + 0xD8u, 0x6u);
    pmu_ram_write_reg(0x00u, (uint8_t)(pmu_ram_read_reg(0x00u) & ~0xC0u));
    for (uint32_t i = 0u; i < 8u; i++) {
        openfw_mmio_wr32(NVIC_ICER_ADDR(i), 0xFFFFFFFFu);
        openfw_mmio_wr32(NVIC_ICPR_ADDR(i), 0xFFFFFFFFu);
    }

    openfw_mmio_clr32(AON_RTC_BASE + 0x80u, 0x80000000u);
    openfw_mmio_wr32(AON_RTC_BASE + 0x80u, 0x8FA00001u);

    uint8_t r21 = pmu_ram_read_reg(0x21u);
    openfw_mmio_wr32(AON_RTC_BASE + 0xB0u, (((uint32_t)(r21 | 0x03u) & 0xFFu) << 8) | 0x21u);
    openfw_mmio_set32(AON_RTC_BASE + 0xD0u, 0x01u);
    uint32_t v_hi = (uint32_t)((r21 & 0xE7u) | 0x04u);
    uint32_t v_lo = v_hi & 0xFBu;
    openfw_mmio_wr32(AON_RTC_BASE + 0xB8u, (v_lo << 24) | (v_hi << 16) | 0x21u);
    pmu_ram_write_reg(0x00u, (uint8_t)(pmu_ram_read_reg(0x00u) | 0x08u));
    openfw_mmio_set32(AON_RTC_BASE + 0xD0u, 0x08u);

    /* Ensure watchdog is stopped right before final WFI */
    openfw_mmio_wr32(WDT_CFG_ADDR, 0u);
    OPENFW_DSB();
    while (1) {
        OPENFW_WFI();
    }
}

void pmu_soft_poweroff(void)
{
#ifndef OPENFW_HOST_MMIO
    cpu1_ui_stop();
#endif
    if (pmu_is_vbus_present()) {
        wait_in_charging_standby();
    }
    /* No code will service the reset watchdog once the CPU enters WFI. */
    wdt_stop();
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

    pmu_soft_poweroff_ram();
}

void pmu_unattended_init(void)
{
    /*
     * Clear HSW / rail-hold bits [2:1] while preserving bit 0 (DFU sub-flag)
     * and bit 3 (0x08 rail/LDO power-hold on HW_76). Re-enable hardware 8s
     * long-press reset ([7:6] = 0b10 = 0x80, matching stock HAL_pmu_init
     * 0x00CE8304) whenever soft power-off cleared [7:6] to 0.
     */
    uint8_t r00 = (uint8_t)(pmu_read_reg(0x00u) & ~0x06u);
    if ((r00 & 0xC0u) == 0u) {
        r00 |= 0x80u;
    }
    pmu_write_reg(0x00u, r00);
    /* Match HAL_pmu_init wakeup/reset mask (KEY_ONOFF + VBUS, bit 7=0) and clear stale DFU flags */
    pmu_write_reg(0x20u, 0x2Fu);
    pmu_write_reg(0x22u, 0x00u);
    pmu_write_reg(0x23u, (uint8_t)(pmu_read_reg(0x23u) & ~0xC0u));
    /* Runtime ON/OFF timings: 2s LK (0x01), 8s hardware force-reset (0x04) */
    pmu_write_reg(0x24u, (uint8_t)((pmu_read_reg(0x24u) & ~0x0Fu) | 0x05u));
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
    pmu_cold_reset();
}

void pmu_reboot_to_sd_ota(void)
{
    uint8_t r23 = pmu_read_reg(0x23u);
    uint8_t r00 = pmu_read_reg(0x00u);
    pmu_write_reg(0x23u, (uint8_t)(r23 | 0xC0u));
    pmu_write_reg(0x00u, (uint8_t)(r00 & ~0x01u));
    pmu_cold_reset();
}
