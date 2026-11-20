/*
 * Linker script for dynamic Thumb-2 OpenFW SRAM experiments (0x00850000..0x00857FFF)
 *
 * SPDX-License-Identifier: MIT
 */
ENTRY(exp_main)

MEMORY
{
    EXP_ARENA (rwx) : ORIGIN = 0x00850000, LENGTH = 0x00008000
}

SECTIONS
{
    .text 0x00850000 : ALIGN(4) {
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > EXP_ARENA

    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    } > EXP_ARENA

    .bss (NOLOAD) : ALIGN(4) {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
    } > EXP_ARENA

    /DISCARD/ : {
        *(.ARM.exidx* .ARM.extab* .comment .note*)
    }
}
