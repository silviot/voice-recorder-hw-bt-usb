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

extern uint32_t _estack;
extern uint32_t _sbss, _ebss;

extern int openfw_main(void);

volatile uint32_t g_systick_ms = 0;

void _start(void);
void reset_c_entry(void);
void Default_Handler(void);
void SysTick_Handler(void);

__attribute__((section(".isr_vector"), used, aligned(256)))
const uint32_t g_isr_vector[64] = {
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
    [16 ... 63] = (uint32_t)(uintptr_t)Default_Handler,
};

void SysTick_Handler(void)
{
    g_systick_ms++;
}

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

__attribute__((used, noreturn))
void reset_c_entry(void)
{
    /* Point VTOR at our 256-byte aligned SRAM vector table */
    SCB_VTOR = (uint32_t)(uintptr_t)g_isr_vector;

    /* Enable Cortex-M4F FPU (CP10 & CP11 full access) */
    SCB_CPACR |= SCB_CPACR_FPU_FULL;
    __asm__ volatile ("dsb\nisb");

    /* Zero .bss in SRAM */
    for (uint32_t *dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0u;
    }

    /* Enable interrupts (Stage-1 entered with cpsid i) */
    __asm__ volatile ("cpsie i");

    (void)openfw_main();

    /* Reboot cleanly if main ever returns */
    __asm__ volatile ("dsb");
    SCB_AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET;
    while (1) {
        __asm__ volatile ("wfi");
    }
}
