/*
 * Smartlink SL6801 Bare-Metal Cortex-M4F Startup & Vector Table
 *
 * Placed in .ram_boot (SRAM 0x00804C00, copied from flash 0x00E030 by Stage-1).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"

extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;
extern uint32_t _sbss, _ebss;

extern int openfw_main(void);

volatile uint32_t g_systick_ms = 0;

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void);

__attribute__((section(".isr_vector"), used))
const uint32_t g_isr_vector[64] = {
    (uint32_t)(uintptr_t)&_estack,    /* 0: Initial MSP (0x0086AC08) */
    (uint32_t)(uintptr_t)Reset_Handler,
    (uint32_t)(uintptr_t)Default_Handler, /* NMI */
    (uint32_t)(uintptr_t)Default_Handler, /* HardFault */
    (uint32_t)(uintptr_t)Default_Handler, /* MemManage */
    (uint32_t)(uintptr_t)Default_Handler, /* BusFault */
    (uint32_t)(uintptr_t)Default_Handler, /* UsageFault */
    0, 0, 0, 0,
    (uint32_t)(uintptr_t)Default_Handler, /* SVCall */
    (uint32_t)(uintptr_t)Default_Handler, /* DebugMon */
    0,
    (uint32_t)(uintptr_t)Default_Handler, /* PendSV */
    (uint32_t)(uintptr_t)SysTick_Handler, /* SysTick */
    /* External IRQs 0..47 default to Default_Handler */
    [16 ... 63] = (uint32_t)(uintptr_t)Default_Handler,
};

__attribute__((section(".ramtext"), used))
void SysTick_Handler(void)
{
    g_systick_ms++;
}

__attribute__((section(".ramtext"), used))
void Default_Handler(void)
{
    while (1) {
        __asm__ volatile ("wfi");
    }
}

__attribute__((section(".ramtext"), used))
void Reset_Handler(void)
{
    /* Point VTOR at our RAM vector table */
    SCB_VTOR = (uint32_t)(uintptr_t)g_isr_vector;

    /* Enable Cortex-M4F FPU (CP10 & CP11 full access) */
    SCB_CPACR |= SCB_CPACR_FPU_FULL;
    __asm__ volatile ("dsb\nisb");

    /* Copy .data from XIP flash to SRAM */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* Zero .bss in SRAM */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    /* Enter XIP C main */
    (void)openfw_main();

    /* Reboot cleanly if main ever returns */
    SCB_AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET;
    while (1) {
        __asm__ volatile ("wfi");
    }
}
