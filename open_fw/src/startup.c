/*
 * Smartlink SL6801 Bare-Metal Cortex-M4F Startup & Vector Table
 *
 * Entered from Stage-1 Bootloader via `blx 0x00804C01` with IRQs disabled
 * and MSP pointing to Stage-1's stack (~0x0083BE90). `_start` immediately
 * switches SP to `_estack` (0x0086AC08) before initializing VTOR, FPU, and .bss.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;
extern uint32_t _sbss, _ebss;

extern int openfw_main(void);

volatile uint32_t g_systick_ms = 0;

void _start(void);
void reset_c_entry(void);
void Default_Handler(void);
void Fault_Handler_Asm(void);
void fault_handler_c(const uint32_t *frame, uint32_t ipsr);
void SysTick_Handler(void);
void RWBT_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMAC_CH0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMAC_CH1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used, aligned(512)))
const uint32_t g_isr_vector[96] = {
    (uint32_t)(uintptr_t)&_estack,         /*  0: Initial MSP (0x0086AC08) */
    (uint32_t)(uintptr_t)_start,           /*  1: Reset */
    (uint32_t)(uintptr_t)Default_Handler,  /*  2: NMI */
    (uint32_t)(uintptr_t)Fault_Handler_Asm, /* 3: HardFault */
    (uint32_t)(uintptr_t)Fault_Handler_Asm, /* 4: MemManage */
    (uint32_t)(uintptr_t)Fault_Handler_Asm, /* 5: BusFault */
    (uint32_t)(uintptr_t)Fault_Handler_Asm, /* 6: UsageFault */
    0, 0, 0, 0,
    (uint32_t)(uintptr_t)Default_Handler,  /* 11: SVCall */
    (uint32_t)(uintptr_t)Default_Handler,  /* 12: DebugMon */
    0,
    (uint32_t)(uintptr_t)Default_Handler,  /* 14: PendSV */
    (uint32_t)(uintptr_t)SysTick_Handler,  /* 15: SysTick */
    [16 ... 41] = (uint32_t)(uintptr_t)Default_Handler,
    [42]        = (uint32_t)(uintptr_t)RWBT_IRQHandler,     /* IRQ 26: RW-BT Baseband */
    [43 ... 75] = (uint32_t)(uintptr_t)Default_Handler,
    [76]        = (uint32_t)(uintptr_t)DMAC_CH0_IRQHandler, /* IRQ 60: DMAC CH0 */
    [77]        = (uint32_t)(uintptr_t)DMAC_CH1_IRQHandler, /* IRQ 61: DMAC CH1 */
    [78 ... 95] = (uint32_t)(uintptr_t)Default_Handler,
};

__attribute__((section(".ramtext"), noinline))
static uint8_t ram_pmu_read_reg(uint8_t reg)
{
    uint32_t cmd = 0x40000061u | ((uint32_t)reg << 8);
    REG32(RTWI_CMD_ADDR) = cmd;
    REG32(RTWI_CMD_ADDR) = cmd | (1u << 31);
    for (uint32_t t = 0u; t < 100000u && (int32_t)REG32(RTWI_CMD_ADDR) < 0; t++) {
    }
    return (uint8_t)(REG32(RTWI_RDATA_ADDR) & 0xFFu);
}

__attribute__((section(".ramtext"), noinline))
static void ram_pmu_write_reg(uint8_t reg, uint8_t val)
{
    REG32(RTWI_WDATA_ADDR) = (uint32_t)val;
    uint32_t cmd = 0x40000060u | ((uint32_t)reg << 8);
    REG32(RTWI_CMD_ADDR) = cmd;
    REG32(RTWI_CMD_ADDR) = cmd | (1u << 31);
    for (uint32_t t = 0u; t < 100000u && (int32_t)REG32(RTWI_CMD_ADDR) < 0; t++) {
    }
}

__attribute__((section(".ramtext"), noinline, noreturn))
static void ram_soc_wdt_reset(void)
{
    /* Exact HLKJ 0x008284D6 clock/PLL teardown to HOSC from SRAM (Q14 verified) */
    REG32(0x40080040u) &= ~0x03u;
    REG32(0x40080048u) &= ~0x03u;
    REG32(0x400800A0u) &= ~0x30u;
    for (volatile uint32_t i = 0u; i < 32u; i++) {
        __asm__ volatile ("nop");
    }
    REG32(0x40080030u) &= ~0x70u;
    REG32(0x40080010u) &= ~0xC0000000u;
    REG32(0x40080000u) &= ~0xC0010000u;
    REG32(0x400800FCu) &= ~0x10u;

    REG32(AON_WDT_PMU_RST_CMD_ADDR) = 0u;
    REG32(AON_WDT_RST_CTL_ADDR) &= ~0x02u;
    REG32(WDT_CFG_ADDR) = 0u;
    REG32(WDT_EN_ADDR) |= 1u;
    REG32(WDT_MODE_ADDR) = WDT_MODE_HW_RESET;
    REG32(WDT_CFG_ADDR) = 0x01u;
    __asm__ volatile ("dsb" ::: "memory");
    /* Wait purely for SoC WDT digital reset (~1 s); never hit SCB_AIRCR on silicon */
    while (1) {
        __asm__ volatile ("nop");
    }
}

__attribute__((section(".ramtext"), used, noreturn))
void fault_handler_c(const uint32_t *frame, uint32_t ipsr)
{
    __asm__ volatile ("cpsid i" ::: "memory");

    uint32_t faddr = (uint32_t)(uintptr_t)frame;
    uint32_t lr    = 0xDEAD57ACu;
    uint32_t pc    = 0xDEAD57ACu;
    if ((faddr & 3u) == 0u && faddr >= 0x00800000u && faddr <= 0x0086B000u) {
        lr = frame[5];
        pc = frame[6];
    }

    uint32_t cfsr   = SCB_CFSR;
    uint32_t hfsr   = SCB_HFSR;
    uint32_t bfar   = (cfsr & (1u << 15)) ? SCB_BFAR : hfsr;
    uint32_t in_exp = (g_exp_running != 0u) ? 1u : 0u;
    g_exp_running   = 0u;

    /* Preserve AON_BACKUP(6) (0x40085198) used by stock firmware RTC init */
    AON_BACKUP(0u) = OPENFW_FAULT_MAGIC_BASE | (ipsr & 0xFFu);
    AON_BACKUP(1u) = pc;
    AON_BACKUP(2u) = lr;
    AON_BACKUP(3u) = cfsr;
    AON_BACKUP(4u) = bfar;
    AON_BACKUP(5u) = in_exp;

    if (!in_exp) {
        uint32_t b7      = AON_BACKUP(7u);
        uint32_t strikes = ((b7 & 0xFFFF0000u) == OPENFW_BOOT_STRIKE_MAGIC) ? (b7 & 0xFFFFu) : 0u;
        strikes++;
        if (strikes >= OPENFW_BOOT_MAX_STRIKES) {
            AON_BACKUP(7u) = 0u;
            ram_pmu_write_reg(0x23u, (uint8_t)(ram_pmu_read_reg(0x23u) | 0xC0u));
            ram_pmu_write_reg(0x00u, (uint8_t)(ram_pmu_read_reg(0x00u) | 0x01u));
        } else {
            AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC | (strikes & 0xFFFFu);
        }
    }

    __asm__ volatile ("dsb" ::: "memory");
    ram_soc_wdt_reset();
}

__attribute__((section(".ramtext"), naked, used))
void Fault_Handler_Asm(void)
{
    __asm__ volatile (
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mrs r1, ipsr\n"
        "ldr r2, =_estack\n"
        "mov sp, r2\n"
        "b.w fault_handler_c\n"
    );
}

__attribute__((section(".ramtext")))
void SysTick_Handler(void)
{
    uint32_t now = g_systick_ms + 1u;
    g_systick_ms = now;
    if (g_exp_running != 0u && (int32_t)(now - g_exp_deadline_ms) >= 0) {
        __asm__ volatile ("cpsid i" ::: "memory");
        g_exp_running  = 0u;
        AON_BACKUP(0u) = OPENFW_FAULT_EXP_TIMEOUT;
        AON_BACKUP(1u) = now;
        AON_BACKUP(2u) = g_exp_deadline_ms;
        AON_BACKUP(3u) = 0u;
        AON_BACKUP(4u) = 0u;
        AON_BACKUP(5u) = 1u;
        __asm__ volatile ("dsb" ::: "memory");
        ram_soc_wdt_reset();
    }
    keys_scan_tick(now);
}

__attribute__((section(".ramtext")))
void Default_Handler(void)
{
    while (1) {
        __asm__ volatile ("wfi");
    }
}

__attribute__((section(".text.entry"), naked, used))
void _start(void)
{
    __asm__ volatile (
        "ldr r0, =_estack\n"
        "mov sp, r0\n"
        "b.w reset_c_entry\n"
    );
}

/* Must run purely in SRAM (.ramtext) before any XIP symbol is referenced */
__attribute__((section(".ramtext"), noinline))
static void sys_clock_boost(uint32_t pll_hz, uint32_t clk6_hz)
{
    AON_PWR_CTRL0 = 0u;

    /* Temporarily park CPU and bus clock on 24 MHz HOSC before changing PLL0 */
    rom_clk_set_src(CRU_CLK_CPU, CRU_CLK_SRC_HOSC24M);
    rom_clk_set_src(CRU_CLK_BUS6, CRU_CLK_SRC_HOSC24M);
    for (volatile int i = 0; i < 64; i++) {
    }

    /* Configure SRAM wait-states for high-speed PLL0 operation */
    uint32_t ws = SYSCTL_SRAM_WS0;
    ws = (ws & 0xF8F8F8F8u & ~0x00B800B8u) | 0x04830483u;
    SYSCTL_SRAM_WS0 = ws;
    SYSCTL_SRAM_WS1 = (SYSCTL_SRAM_WS1 & ~0x1Fu) | 0x13u;

    /* Program PLL0 (e.g. 192 MHz -> 96 MHz CPU at /2 divider) */
    rom_clk_set_rate(CRU_CLK_PLL0, pll_hz);

    /* Gate unused clocks 8 & 9 and clear AON PLL init bit */
    rom_clk_disable(8u);
    rom_clk_disable(9u);
    AON_PLL_CTRL &= ~(1u << 1);

    /* Configure CPU /2 (96 MHz), HCLK /1, PCLK /1, clk6 */
    rom_clk_div(CRU_CLK_CPU, 2u);
    rom_clk_div(CRU_CLK_HCLK, 1u);
    rom_clk_div(CRU_CLK_PCLK, 1u);
    rom_clk_div(CRU_CLK_BUS6, pll_hz / clk6_hz);
    for (volatile int i = 0; i < 64; i++) {
    }

    rom_clk_set_src(CRU_CLK_CPU, CRU_CLK_SRC_PLL0);
    rom_clk_set_src(CRU_CLK_BUS6, CRU_CLK_SRC_PLL0);
}

/* Must run purely in SRAM (.ramtext) before jumping to XIP 0x00C0F430+ */
__attribute__((section(".ramtext"), noinline))
static void xip_enable_1wire(uint32_t pll_hz, uint32_t nor_hz)
{
    rom_rst_release(CRU_RST_XIP_BUS);
    if (!rom_rst_is_released(CRU_RST_NOR)) {
        rom_rst_release(CRU_RST_NOR);
        for (volatile int i = 0; i < 2000; i++) {
        }
    }

    /* PA0..PA3 mux=2 (SPI NOR SCK/CS/IO0/IO1); PA4/PA5 high (WP#/HOLD#) */
    rom_gpio_config(0x00000110u);
    rom_gpio_config(0x00000910u);
    rom_gpio_config(0x00001110u);
    rom_gpio_config(0x0000191Bu);
    rom_gpio_config(0x0000279Bu);
    rom_gpio_config(0x00002F9Bu);

    uint32_t div = pll_hz / nor_hz;
    if (div < 2u) {
        div = 2u;
    }
    rom_clk_disable(CRU_CLK_NOR);
    rom_clk_div(CRU_CLK_NOR, div);
    rom_clk_set_src(CRU_CLK_NOR, CRU_CLK_SRC_PLL0);
    rom_clk_enable(CRU_CLK_NOR);

    NOR_CTRL = 0x00000001u;
    NOR_XIP_CFG = NOR_XIP_CFG_1WIRE;
    XIP_BRIDGE_CTRL |= (1u << 8);
    NOR_XIP_OFFSET = 0x00000040u; /* NOR_XIP_SIZE_CFG: 0x40 = 2 MiB (0x00 was 128 KiB!) */

    /* Enable & flush XIP instruction cache (2048 KiB window, matches stock FIRM 0x00805498) */
    uint32_t c = XIP_CACHE_CTRL1;
    c &= ~(0x1FF00000u | 0x0Cu);
    c |= (((0x800u >> 3) - 1u) << 20);
    XIP_CACHE_CTRL1 = c;
    XIP_CACHE_CTRL0 &= ~0x2u;
    XIP_CACHE_CTRL0 = 0x1u;
    XIP_CACHE_CTRL1 = (XIP_CACHE_CTRL1 & ~0x2u) | 0x1u;

    __asm__ volatile ("dsb\nisb");
}

__attribute__((section(".ramtext"), used, noreturn))
void reset_c_entry(void)
{
    /* Point VTOR at our 512-byte aligned SRAM vector table */
    SCB_VTOR = (uint32_t)(uintptr_t)g_isr_vector;

    /* Enable Cortex-M4F FPU (CP10 & CP11 full access) and granular fault handlers */
    SCB_CPACR |= SCB_CPACR_FPU_FULL;
    SCB_SHCSR |= SCB_SHCSR_FAULTS_EN;
    __asm__ volatile ("dsb\nisb");

    /* Boost CPU to 96 MHz and map SPI NOR XIP at 0x00C0F430+ before XIP access */
    sys_clock_boost(192000000u, 32000000u);
    xip_enable_1wire(192000000u, 24000000u);

    /* Copy initialized .data from XIP LMA (_sidata) to SRAM (_sdata.._edata) */
    const volatile uint32_t *src = &_sidata;
    for (volatile uint32_t *dst = &_sdata; dst < &_edata; dst++, src++) {
        *dst = *src;
    }

    /* Zero .bss in SRAM */
    for (volatile uint32_t *dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0u;
    }

    /* Enable interrupts (Stage-1 entered with cpsid i) */
    __asm__ volatile ("dsb\nisb\ncpsie i");

    (void)openfw_main();

    /* Reboot cleanly if main ever returns */
    __asm__ volatile ("cpsid i\ndsb" ::: "memory");
    ram_soc_wdt_reset();
}

/* Bare-metal ARM EABI memory helpers emitted by Clang for struct copies */
void __aeabi_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

void __aeabi_memcpy4(void *dst, const void *src, uint32_t n)
{
    __aeabi_memcpy(dst, src, n);
}

void __aeabi_memcpy8(void *dst, const void *src, uint32_t n)
{
    __aeabi_memcpy(dst, src, n);
}

void __aeabi_memset(void *dst, uint32_t n, int c)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    while (n-- > 0u) {
        *d++ = v;
    }
}

void __aeabi_memset4(void *dst, uint32_t n, int c)
{
    __aeabi_memset(dst, n, c);
}

void __aeabi_memset8(void *dst, uint32_t n, int c)
{
    __aeabi_memset(dst, n, c);
}

void __aeabi_memclr(void *dst, uint32_t n)
{
    __aeabi_memset(dst, n, 0);
}

void __aeabi_memclr4(void *dst, uint32_t n)
{
    __aeabi_memset(dst, n, 0);
}

void __aeabi_memclr8(void *dst, uint32_t n)
{
    __aeabi_memset(dst, n, 0);
}
