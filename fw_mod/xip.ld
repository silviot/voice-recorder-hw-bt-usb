/* Linker script for SL6801 XIP extension at 0x00D74000 (flash 0x174000) */
ENTRY(bt_filesync_hook)

MEMORY
{
    XIP (rx) : ORIGIN = 0x00D74000, LENGTH = 0x00004000
}

SECTIONS
{
    .text 0x00D74000 : {
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > XIP

    /* Reject any writable globals (code runs from read-only XIP SPI NOR flash) */
    .data : { *(.data .data.*) } > XIP
    .bss  : { *(.bss .bss.* COMMON) } > XIP
    ASSERT(SIZEOF(.data) == 0, "Writable .data not allowed in XIP flash!")
    ASSERT(SIZEOF(.bss)  == 0, "Writable .bss not allowed in XIP flash!")

    /DISCARD/ : {
        *(.ARM.exidx* .ARM.extab* .comment .note*)
    }
}
