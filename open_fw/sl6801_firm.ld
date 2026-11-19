/*
 * Smartlink SL6801 Standalone Open-Source FIRM Linker Script (Layer 1b: SRAM-Resident)
 *
 * Stage-1 Bootloader copies `loadLength` bytes from flash 0x00E430 directly to
 * `loadToRam = 0x00804C00` and invokes `blx 0x00804C01` (without remapping XIP
 * or setting SCB->VTOR / MSP, while Stage-1's temporary SP is ~0x0083BE90).
 *
 * Keeping the flat Layer 1b image inside 0x00804C00..0x0082CBFF (160 KB) guarantees
 * Stage-1 safely loads the entire image without overwriting its own stack, after
 * which `_start` switches MSP to `_estack = 0x0086AC08`.
 *
 * SPDX-License-Identifier: MIT
 */
ENTRY(_start)

MEMORY
{
    SRAM_IMG (rwx) : ORIGIN = 0x00804C00, LENGTH = 0x00028000
}

_estack = 0x0086AC08;

SECTIONS
{
    /* Naked Thumb entry trampoline MUST sit at exact 0x00804C00 (blx 0x00804C01) */
    .text.entry 0x00804C00 : {
        KEEP(*(.text.entry))
    } > SRAM_IMG

    /* 256-byte aligned Cortex-M4F vector table at 0x00804D00 */
    .isr_vector ALIGN(256) : {
        KEEP(*(.isr_vector))
    } > SRAM_IMG

    .text : {
        *(.ramtext .ramtext.*)
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > SRAM_IMG

    .data : ALIGN(4) {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM_IMG

    ASSERT((_edata - 0x00804C00) <= 0x00028000, "SRAM image exceeds safe pre-stack limit")

    .bss (NOLOAD) : ALIGN(4) {
        _sbss = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM_IMG

    /DISCARD/ : {
        *(.ARM.exidx* .ARM.extab* .comment .note*)
    }
}
