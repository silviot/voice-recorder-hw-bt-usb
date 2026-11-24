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
extern uint32_t _sbss_hi, _ebss_hi;

extern int openfw_main(void);

volatile uint32_t g_systick_ms = 0;
volatile uint8_t  g_openfw_boot_strike_counted = 0u;
volatile uint8_t  g_openfw_xip_mode = OPENFW_XIP_MODE_1WIRE_24M;

/* 64-byte XIP verification pattern spanning multiple cache lines in .rodata */
__attribute__((section(".rodata"), used, aligned(16)))
static const uint32_t k_xip_verify[16] = {
    0x51584950u, 0x52594A51u, 0x535A4B52u, 0x545B4C53u,
    0x555C4D54u, 0x565D4E55u, 0x575E4F56u, 0x585F5057u,
    0x59605158u, 0x5A615259u, 0x5B62535Au, 0x5C63545Bu,
    0x5D64555Cu, 0x5E65565Du, 0x5F66575Eu, 0x6067585Fu,
};

void _start(void);
void reset_c_entry(void);
void Default_Handler(void);
void Fault_Handler_Asm(void);
void fault_handler_c(const uint32_t *frame, uint32_t ipsr);
void SysTick_Handler(void);
#if OPENFW_RTOS
#include "openfw_rtos.h"
void SVC_Handler(void);
void PendSV_Handler(void);
#endif
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
#if OPENFW_RTOS
    (uint32_t)(uintptr_t)SVC_Handler,      /* 11: SVCall (FreeRTOS) */
#else
    (uint32_t)(uintptr_t)Default_Handler,  /* 11: SVCall */
#endif
    (uint32_t)(uintptr_t)Default_Handler,  /* 12: DebugMon */
    0,
#if OPENFW_RTOS
    (uint32_t)(uintptr_t)PendSV_Handler,   /* 14: PendSV (FreeRTOS) */
#else
    (uint32_t)(uintptr_t)Default_Handler,  /* 14: PendSV */
#endif
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

__attribute__((section(".ramtext"), used, noreturn))
void fault_handler_c(const uint32_t *frame, uint32_t ipsr)
{
    __asm__ volatile ("cpsid i" ::: "memory");

    uint32_t faddr = (uint32_t)(uintptr_t)frame;
    uint32_t lr    = 0xDEAD57ACu;
    uint32_t pc    = 0xDEAD57ACu;
#if OPENFW_RTOS
    /* Task stacks live in the .rtos_heap region (up to 0x00880000), not only below _estack. */
    if ((faddr & 3u) == 0u && faddr >= 0x00800000u && faddr <= (0x00880000u - 28u)) {
#else
    if ((faddr & 3u) == 0u && faddr >= 0x00800000u && faddr <= (0x0086AC08u - 28u)) {
#endif
        lr = frame[5];
        pc = frame[6];
    }

    uint32_t cfsr   = SCB_CFSR;
    uint32_t hfsr   = SCB_HFSR;
    uint32_t far    = (cfsr & (1u << 15)) ? SCB_BFAR
                    : (cfsr & (1u << 7))  ? SCB_MMFAR
                    : hfsr;
    uint32_t in_exp = (g_exp_running != 0u) ? 1u : 0u;
    g_exp_running   = 0u;

    /* Preserve AON_BACKUP(6) (0x40085198) used by stock firmware RTC init */
    AON_BACKUP(0u) = OPENFW_FAULT_MAGIC_BASE | (ipsr & 0xFFu);
    AON_BACKUP(1u) = pc;
    AON_BACKUP(2u) = lr;
    AON_BACKUP(3u) = cfsr;
    AON_BACKUP(4u) = far;
    AON_BACKUP(5u) = in_exp;

    if (!in_exp) {
        uint32_t b7      = AON_BACKUP(7u);
        uint32_t strikes = ((b7 & 0xFFFF0000u) == OPENFW_BOOT_STRIKE_MAGIC) ? (b7 & 0xFFFFu) : 0u;
        if (!g_openfw_boot_strike_counted) {
            strikes++;
        }
        if (strikes >= OPENFW_BOOT_MAX_STRIKES) {
            AON_BACKUP(7u) = 0u;
            ram_pmu_write_reg(0x23u, (uint8_t)(ram_pmu_read_reg(0x23u) | 0xC0u));
            ram_pmu_write_reg(0x00u, (uint8_t)(ram_pmu_read_reg(0x00u) | 0x01u));
        } else {
            AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC | (strikes & 0xFFFFu);
        }
    }

    __asm__ volatile ("dsb" ::: "memory");
    system_reboot();
    while (1) {
        __asm__ volatile ("nop");
    }
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
        system_reboot();
    }
    keys_scan_tick(now);
#if OPENFW_RTOS
    openfw_rtos_tick();
#endif
}

__attribute__((section(".ramtext"), naked, used))
void Default_Handler(void)
{
    __asm__ volatile ("b.w Fault_Handler_Asm\n");
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

__attribute__((section(".ramtext"), noinline))
static void xip_cache_flush_ram(void)
{
    uint32_t c = XIP_CACHE_CTRL1;
    c &= ~(0x1FF00000u | 0x0Cu);
    c |= (((0x800u >> 3) - 1u) << 20);
    XIP_CACHE_CTRL1 = c;
    XIP_CACHE_CTRL0 &= ~0x2u;
    XIP_CACHE_CTRL0 = 0x1u;
    XIP_CACHE_CTRL1 = (XIP_CACHE_CTRL1 & ~0x2u) | 0x1u;
    __asm__ volatile ("dsb\nisb");
}

__attribute__((section(".ramtext"), noinline))
static int xip_verify_ram(void)
{
    const volatile uint32_t *p = k_xip_verify;
    uint32_t expected = 0x51584950u;
    for (uint32_t i = 0u; i < 16u; i++) {
        if (p[i] != expected) {
            return 0;
        }
        expected += 0x01010101u;
    }
    return 1;
}

__attribute__((section(".ramtext"), noinline))
static void xip_set_nor_clk(uint32_t pll_hz, uint32_t nor_hz)
{
    uint32_t div = pll_hz / nor_hz;
    if (div < 2u) {
        div = 2u;
    }
    rom_clk_disable(CRU_CLK_NOR);
    rom_clk_div(CRU_CLK_NOR, div);
    rom_clk_set_src(CRU_CLK_NOR, CRU_CLK_SRC_PLL0);
    rom_clk_enable(CRU_CLK_NOR);
}

/*
 * Must run purely in SRAM (.ramtext) before jumping to XIP 0x00C0F430+.
 * Attempts fastest verified SPI NOR XIP mode with brick-proof auto-fallback:
 *   1. 4-wire Quad I/O (0xEB) @ 48 MHz (8x throughput vs 1W@24MHz)
 *   2. 2-wire Dual Output (0x3B) @ 48 MHz (4x throughput)
 *   3. 1-wire Read (0x03) @ 24 MHz (universal fallback)
 */
__attribute__((section(".ramtext"), noinline))
static uint8_t xip_enable_best(uint32_t pll_hz)
{
    rom_rst_release(CRU_RST_XIP_BUS);
    if (!rom_rst_is_released(CRU_RST_NOR)) {
        rom_rst_release(CRU_RST_NOR);
        for (volatile int i = 0; i < 2000; i++) {
        }
    }

    /* Configure PA0..PA3 (SCK/CS/IO0/IO1) drive/pull, then switch PA0..PA5 to mux=2 (Quad) */
    rom_gpio_config(0x00000110u);
    rom_gpio_config(0x00000910u);
    rom_gpio_config(0x00001110u);
    rom_gpio_config(0x0000191Bu);
    rom_gpio_config(0x0000211Bu);
    rom_gpio_config(0x0000291Bu);
    REG32(GPIOA_BASE) = (REG32(GPIOA_BASE) & 0xFF000000u) | 0x00222222u;

    NOR_CTRL = 0x00000001u;
    XIP_BRIDGE_CTRL |= (1u << 8);
    NOR_XIP_OFFSET = 0x00000040u; /* 2 MiB XIP window */

    /* Try 1: 4-wire Quad I/O @ 48 MHz */
    xip_set_nor_clk(pll_hz, 48000000u);
    NOR_XIP_CFG = NOR_XIP_CFG_QUAD;
    xip_cache_flush_ram();
    if (xip_verify_ram()) {
        return OPENFW_XIP_MODE_QUAD_48M;
    }

    /* Restore PA4/PA5 as WP#/HOLD# high for 2-wire / 1-wire modes */
    rom_gpio_config(0x0000279Bu);
    rom_gpio_config(0x00002F9Bu);

    /* Try 2: 2-wire Dual Output (0x3B) @ 48 MHz */
    NOR_XIP_CFG = NOR_XIP_CFG_DUAL;
    xip_cache_flush_ram();
    if (xip_verify_ram()) {
        return OPENFW_XIP_MODE_DUAL_48M;
    }

    /* Fallback 3: rock-solid 1-wire 0x03 @ 24 MHz */
    xip_set_nor_clk(pll_hz, 24000000u);
    NOR_XIP_CFG = NOR_XIP_CFG_1WIRE;
    xip_cache_flush_ram();
    return OPENFW_XIP_MODE_1WIRE_24M;
}

#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID
/* Bluedroid/port/libc static data lives in the RTOS region (sl6801_firm.ld .bt_data/.bt_bss). Runs from XIP,
 * after xip_enable_best() and the .data copy in reset_c_entry(). */
__attribute__((noinline)) static void bt_static_init(void)
{
    extern uint32_t _sibt_data, _sbt_data, _ebt_data, _sbt_bss, _ebt_bss;
    const volatile uint32_t *src = &_sibt_data;
    for (volatile uint32_t *dst = &_sbt_data; dst < &_ebt_data; dst++, src++) {
        *dst = *src;
    }
    for (volatile uint32_t *dst = &_sbt_bss; dst < &_ebt_bss; dst++) {
        *dst = 0u;
    }
}
#endif

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
    uint8_t xip_mode = xip_enable_best(192000000u);

    /* Copy initialized .data (including .fast_ramtext hot code) from XIP LMA to SRAM */
    const volatile uint32_t *src = &_sidata;
    for (volatile uint32_t *dst = &_sdata; dst < &_edata; dst++, src++) {
        *dst = *src;
    }

    /* Zero .bss and .bss_hi in SRAM */
    for (volatile uint32_t *dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0u;
    }
    for (volatile uint32_t *dst = &_sbss_hi; dst < &_ebss_hi; dst++) {
        *dst = 0u;
    }
#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID
    bt_static_init(); /* XIP code (XIP is mapped above); keeps .ram_boot small */
#endif

    g_openfw_xip_mode = xip_mode;

    /* Ensure no stale SysTick or NVIC interrupts fire when IRQs are enabled */
    SYSTICK_CSR = 0u;
    for (uint32_t i = 0u; i < 8u; i++) {
        NVIC_ICER(i) = 0xFFFFFFFFu;
        NVIC_ICPR(i) = 0xFFFFFFFFu;
    }

    /* Enable interrupts (Stage-1 entered with cpsid i) */
    __asm__ volatile ("dsb\nisb\ncpsie i");

#if OPENFW_RTOS
    openfw_rtos_start(openfw_main); /* never returns */
#else
    (void)openfw_main();
#endif

    /* Reboot cleanly if main ever returns */
    __asm__ volatile ("cpsid i\ndsb" ::: "memory");
    system_reboot();
    while (1) {
        __asm__ volatile ("nop");
    }
}

/* Bare-metal ARM EABI memory helpers emitted by Clang for struct copies (zero-WS SRAM) */
OPENFW_FASTFUNC
void __aeabi_memcpy4(void *dst, const void *src, uint32_t n)
{
    uint32_t *dw = (uint32_t *)dst;
    const uint32_t *sw = (const uint32_t *)src;
    while (n >= 4u) {
        *dw++ = *sw++;
        n -= 4u;
    }
    uint8_t *d = (uint8_t *)dw;
    const uint8_t *s = (const uint8_t *)sw;
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

OPENFW_FASTFUNC
void __aeabi_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (n >= 4u && (((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (((uintptr_t)d & 3u) != 0u) {
            *d++ = *s++;
            n--;
        }
        __aeabi_memcpy4(d, s, n);
        return;
    }
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

OPENFW_FASTFUNC
void __aeabi_memcpy8(void *dst, const void *src, uint32_t n)
{
    __aeabi_memcpy4(dst, src, n);
}

OPENFW_FASTFUNC
void __aeabi_memset4(void *dst, uint32_t n, int c)
{
    uint8_t v = (uint8_t)c;
    uint32_t w = (uint32_t)v * 0x01010101u;
    uint32_t *dw = (uint32_t *)dst;
    while (n >= 4u) {
        *dw++ = w;
        n -= 4u;
    }
    uint8_t *d = (uint8_t *)dw;
    while (n-- > 0u) {
        *d++ = v;
    }
}

OPENFW_FASTFUNC
void __aeabi_memset(void *dst, uint32_t n, int c)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    while (n > 0u && ((uintptr_t)d & 3u) != 0u) {
        *d++ = v;
        n--;
    }
    __aeabi_memset4(d, n, c);
}

OPENFW_FASTFUNC
void __aeabi_memset8(void *dst, uint32_t n, int c)
{
    __aeabi_memset4(dst, n, c);
}

OPENFW_FASTFUNC
void __aeabi_memclr(void *dst, uint32_t n)
{
    __aeabi_memset(dst, n, 0);
}

OPENFW_FASTFUNC
void __aeabi_memclr4(void *dst, uint32_t n)
{
    __aeabi_memset4(dst, n, 0);
}

OPENFW_FASTFUNC
void __aeabi_memclr8(void *dst, uint32_t n)
{
    __aeabi_memset4(dst, n, 0);
}

/* Bare-metal 64-bit integer division helpers (ARM EABI: q in r0:r1, r in r2:r3) */
static uint64_t udivmod64(uint64_t num, uint64_t den, uint64_t *rem_out)
{
    uint64_t q = 0ull;
    uint64_t r = 0ull;
    if (den == 0ull) {
        if (rem_out) *rem_out = 0ull;
        return 0ull;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((num >> i) & 1ull);
        if (r >= den) {
            r -= den;
            q |= (1ull << i);
        }
    }
    if (rem_out) *rem_out = r;
    return q;
}

typedef struct {
    uint64_t q;
    uint64_t r;
} aeabi_uldiv_t;

__attribute__((used))
static void aeabi_uldivmod_c(uint64_t num, uint64_t den, aeabi_uldiv_t *out)
{
    out->q = udivmod64(num, den, &out->r);
}

__attribute__((used))
static void aeabi_ldivmod_c(int64_t num, int64_t den, aeabi_uldiv_t *out)
{
    int neg_q = (num < 0) ^ (den < 0);
    int neg_r = (num < 0);
    uint64_t un = (num < 0) ? (uint64_t)(-(num + 1)) + 1ull : (uint64_t)num;
    uint64_t ud = (den < 0) ? (uint64_t)(-(den + 1)) + 1ull : (uint64_t)den;
    uint64_t r = 0ull;
    uint64_t q = udivmod64(un, ud, &r);
    out->q = neg_q ? (0ull - q) : q;
    out->r = neg_r ? (0ull - r) : r;
}

__attribute__((naked))
void __aeabi_uldivmod(void)
{
    __asm__ volatile (
        "push {r4, lr}\n"
        "sub sp, sp, #24\n"
        "add r12, sp, #8\n"
        "str r12, [sp, #0]\n"
        "bl aeabi_uldivmod_c\n"
        "ldr r0, [sp, #8]\n"
        "ldr r1, [sp, #12]\n"
        "ldr r2, [sp, #16]\n"
        "ldr r3, [sp, #20]\n"
        "add sp, sp, #24\n"
        "pop {r4, pc}\n"
    );
}

__attribute__((naked))
void __aeabi_ldivmod(void)
{
    __asm__ volatile (
        "push {r4, lr}\n"
        "sub sp, sp, #24\n"
        "add r12, sp, #8\n"
        "str r12, [sp, #0]\n"
        "bl aeabi_ldivmod_c\n"
        "ldr r0, [sp, #8]\n"
        "ldr r1, [sp, #12]\n"
        "ldr r2, [sp, #16]\n"
        "ldr r3, [sp, #20]\n"
        "add sp, sp, #24\n"
        "pop {r4, pc}\n"
    );
}
