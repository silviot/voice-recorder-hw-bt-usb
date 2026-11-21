/*
 * Smartlink SL6801 Bare-Metal Watchdog & Microsecond Timer Driver
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

extern volatile uint32_t g_systick_ms;

volatile uint32_t g_exp_running     = 0u;
volatile uint32_t g_exp_deadline_ms = 0u;
uint32_t          g_prev_fault_code = 0u;

static uint32_t wdt_ilog2(uint32_t x)
{
    uint32_t r = 0u;
    while (x > 1u) {
        x >>= 1;
        r++;
    }
    return r;
}

void wdt_stop(void)
{
    openfw_mmio_wr32(WDT_CFG_ADDR, 0u);
}

void wdt_start_reset(uint32_t timeout_sec, uint8_t pmu_reg20_cur)
{
    (void)pmu_reg20_cur;
    uint32_t trim0 = openfw_mmio_rd32(AON_WDT_TRIM0_ADDR);
    uint32_t trim1 = openfw_mmio_rd32(AON_WDT_TRIM1_ADDR);
    uint32_t ctrl1 = ((trim0 << 8) & 0x01FFFF00u) | (trim1 & 0xFFu);
    uint32_t timeout_bits;

    /* Use SoC-internal digital WDT reset so PMU rails & AON_BACKUP stay intact */
    openfw_mmio_wr32(AON_WDT_PMU_RST_CMD_ADDR, 0u);

    if (timeout_sec == 0u) {
        timeout_bits = 0u;
        openfw_mmio_clr32(AON_WDT_RST_CTL_ADDR, 0x2u);
    } else if (timeout_sec <= 4u) {
        timeout_bits = (timeout_sec + 1u) << 4;
    } else if (timeout_sec <= 4096u) {
        timeout_bits = ((wdt_ilog2(timeout_sec) + 3u) << 4) & 0xFFu;
    } else {
        timeout_bits = 0xF0u;
    }

    openfw_mmio_wr32(WDT_CFG_ADDR, 0u);
    openfw_mmio_set32(WDT_EN_ADDR, 1u);
    openfw_mmio_wr32(WDT_MODE_ADDR, WDT_MODE_HW_RESET);
    if (ctrl1 != 0u) {
        openfw_mmio_wr32(WDT_CTRL1_ADDR, ctrl1);
    }
    openfw_mmio_wr32(WDT_CFG_ADDR, timeout_bits | 1u);
}

void wdt_feed(void)
{
    openfw_mmio_wr32(WDT_FEED_ADDR, WDT_FEED_KEY);
    if (g_exp_running != 0u) {
        g_exp_deadline_ms = g_systick_ms + OPENFW_EXP_TIMEOUT_MS;
    }
}

OPENFW_RAMFUNC __attribute__((noinline))
void system_reboot(void)
{
    uint32_t pm;
    OPENFW_IRQ_SAVE(pm);
    (void)pm;
    OPENFW_DSB();

#ifndef OPENFW_HOST_MMIO
    /* Exact HLKJ 0x008284D6 clock/PLL teardown to HOSC from SRAM (Q14 verified) */
    openfw_mmio_clr32(0x40080040u, 0x03u);
    openfw_mmio_clr32(0x40080048u, 0x03u);
    openfw_mmio_clr32(0x400800A0u, 0x30u);
    for (volatile uint32_t i = 0u; i < 32u; i++) {
        OPENFW_NOP();
    }
    openfw_mmio_clr32(0x40080030u, 0x70u);
    openfw_mmio_clr32(0x40080010u, 0xC0000000u);
    openfw_mmio_clr32(0x40080000u, 0xC0010000u);
    openfw_mmio_clr32(0x400800FCu, 0x10u);
#endif

    /* Exact BootROM/HLKJ SoC digital WDT reset (preserves PMU rails & AON_BACKUP) */
    openfw_mmio_wr32(AON_WDT_PMU_RST_CMD_ADDR, 0u);
    openfw_mmio_clr32(AON_WDT_RST_CTL_ADDR, 0x02u);
    openfw_mmio_wr32(WDT_CFG_ADDR, 0u);
    openfw_mmio_set32(WDT_EN_ADDR, 1u);
    openfw_mmio_wr32(WDT_MODE_ADDR, WDT_MODE_HW_RESET);
    openfw_mmio_wr32(WDT_CFG_ADDR, 0x01u);

#ifdef OPENFW_HOST_MMIO
    /* Host MMIO test harness expects SCB_AIRCR write + WFI to observe reboot */
    OPENFW_DSB();
    openfw_mmio_wr32(SCB_AIRCR_ADDR, SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET);
    OPENFW_WFI();
    return;
#else
    /* Wait purely for the SoC WDT digital reset (~1 s); NEVER hit SCB_AIRCR
     * on real SL6801 silicon (preempts WDT and hangs peripherals/XIP). */
    OPENFW_DSB();
    while (1) {
        OPENFW_NOP();
    }
#endif
}

uint64_t us_timer_now(void)
{
    /* Lock-free 64-bit counter read (re-read hi if lo wrapped) */
    uint32_t hi1, lo, hi2;
    do {
        hi1 = openfw_mmio_rd32(US_TIMER_HI_ADDR);
        lo  = openfw_mmio_rd32(US_TIMER_LO_ADDR);
        hi2 = openfw_mmio_rd32(US_TIMER_HI_ADDR);
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | (uint64_t)lo;
}

void udelay(uint32_t us)
{
    uint64_t start = us_timer_now();
    while ((us_timer_now() - start) < (uint64_t)us) {
        OPENFW_NOP();
    }
}

void mdelay(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        udelay(1000u);
    }
}

void systick_init(uint32_t cpu_hz)
{
    uint32_t reload = (cpu_hz >= 1000u) ? ((cpu_hz / 1000u) - 1u) : 0u;
    if (reload > 0x00FFFFFFu) {
        reload = 0x00FFFFFFu;
    }
    if (reload == 0u) {
        reload = 1u;
    }
    openfw_mmio_wr32(SYSTICK_CSR_ADDR, 0u);
    openfw_mmio_wr32(SYSTICK_RVR_ADDR, reload);
    openfw_mmio_wr32(SYSTICK_CVR_ADDR, 0u);
    openfw_mmio_wr32(SYSTICK_CSR_ADDR, 0x7u); /* CLKSOURCE=CPU | TICKINT | ENABLE */
}

uint32_t millis(void)
{
    return g_systick_ms;
}
