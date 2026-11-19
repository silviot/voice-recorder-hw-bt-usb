/*
 * Smartlink SL6801 Bare-Metal Watchdog & Microsecond Timer Driver
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"

extern volatile uint32_t g_systick_ms;

void wdt_feed(void)
{
    openfw_mmio_wr32(WDT_FEED_ADDR, WDT_FEED_KEY);
}

void system_reboot(void)
{
    OPENFW_DSB();
    openfw_mmio_wr32(SCB_AIRCR_ADDR, SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET);
    while (1) {
        OPENFW_WFI();
    }
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
    openfw_mmio_wr32(SYSTICK_CSR_ADDR, 0u);
    openfw_mmio_wr32(SYSTICK_RVR_ADDR, (cpu_hz / 1000u) - 1u);
    openfw_mmio_wr32(SYSTICK_CVR_ADDR, 0u);
    openfw_mmio_wr32(SYSTICK_CSR_ADDR, 0x7u); /* CLKSOURCE=CPU | TICKINT | ENABLE */
}

uint32_t millis(void)
{
    return g_systick_ms;
}
