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

extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;
extern uint32_t _sbss, _ebss;

extern int openfw_main(void);

volatile uint32_t g_systick_ms = 0;

void _start(void);
void reset_c_entry(void);
void Default_Handler(void);
void SysTick_Handler(void);
void DMAC_CH1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used, aligned(512)))
const uint32_t g_isr_vector[96] = {
    (uint32_t)(uintptr_t)&_estack,         /*  0: Initial MSP (0x0086AC08) */
    (uint32_t)(uintptr_t)_start,           /*  1: Reset */
    (uint32_t)(uintptr_t)Default_Handler,  /*  2: NMI */
    (uint32_t)(uintptr_t)Default_Handler,  /*  3: HardFault */
    (uint32_t)(uintptr_t)Default_Handler,  /*  4: MemManage */
    (uint32_t)(uintptr_t)Default_Handler,  /*  5: BusFault */
    (uint32_t)(uintptr_t)Default_Handler,  /*  6: UsageFault */
    0, 0, 0, 0,
    (uint32_t)(uintptr_t)Default_Handler,  /* 11: SVCall */
    (uint32_t)(uintptr_t)Default_Handler,  /* 12: DebugMon */
    0,
    (uint32_t)(uintptr_t)Default_Handler,  /* 14: PendSV */
    (uint32_t)(uintptr_t)SysTick_Handler,  /* 15: SysTick */
    [16 ... 76] = (uint32_t)(uintptr_t)Default_Handler,
    [77]        = (uint32_t)(uintptr_t)DMAC_CH1_IRQHandler, /* IRQ 61: DMAC CH1 */
    [78 ... 95] = (uint32_t)(uintptr_t)Default_Handler,
};

__attribute__((section(".ramtext")))
void SysTick_Handler(void)
{
    g_systick_ms++;
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
    NOR_XIP_OFFSET = 0x00000000u;

    /* Enable & flush XIP instruction cache (2048 KB window) */
    uint32_t c = XIP_CACHE_CTRL1;
    c = (c & ~0x1FF00000u) | (((0x800u >> 3) - 1u) << 20);
    c = (c | 0x4u) & ~0x8u;
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

    /* Enable Cortex-M4F FPU (CP10 & CP11 full access) */
    SCB_CPACR |= SCB_CPACR_FPU_FULL;
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
    __asm__ volatile ("dsb");
    SCB_AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET;
    while (1) {
        __asm__ volatile ("wfi");
    }
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
