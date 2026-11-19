/*
 * Smartlink SL6801 Standalone Open-Source FIRM Linker Script
 *
 * Matches the Stage-1 HLKJ Bootloader FIRM partition format at flash 0x00E000:
 *   - Flash 0x00E000..0x00E02F (48 B)  : FIRM header (added by tools/pack_firm.py)
 *   - Flash 0x00E030..0x00F02F (4 KB)  : .ram_boot copied by Stage-1 to SRAM 0x00804C00
 *   - Flash 0x00F030..0x1FBFFF (~1.9M) : XIP .text & .rodata at VMA 0x00C0F030+
 *   - SRAM  0x0081A380..0x0086A800     : .data & .bss (MSP top at 0x0086AC08)
 *
 * SPDX-License-Identifier: MIT
 */
ENTRY(Reset_Handler)

MEMORY
{
    RAM_BOOT (rwx) : ORIGIN = 0x00804C00, LENGTH = 0x00001000
    XIP      (rx)  : ORIGIN = 0x00C0F030, LENGTH = 0x001ECFD0
    SRAM     (rwx) : ORIGIN = 0x0081A380, LENGTH = 0x00050480
}

_estack = 0x0086AC08;

SECTIONS
{
    /* 4 KB RAM block copied by Stage-1 from flash 0x00E030 -> SRAM 0x00804C00 */
    .ram_boot : {
        KEEP(*(.isr_vector))
        *(.ramtext .ramtext.*)
        . = ALIGN(4);
    } > RAM_BOOT

    /* XIP code & read-only data executing directly from SPI NOR at 0x00C0F030 */
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > XIP

    _sidata = LOADADDR(.data);

    .data : {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > XIP

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /DISCARD/ : {
        *(.ARM.exidx* .ARM.extab* .comment .note*)
    }
}
