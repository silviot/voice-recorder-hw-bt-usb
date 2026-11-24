/*
 * Linker script for standalone open_fw Hybrid SRAM Bootstrap + SPI NOR XIP
 *
 * Flash layout (FIRM partition @ 0x00E000):
 *   0x00E000..0x00E02F : 48 B FIRM header (loadToRam=0x00804C00, loadLength=0x1000)
 *   0x00E030..0x00E42F : 1024 B Stage-1 pad
 *   0x00E430..0x00F42F : 4096 B .ram_boot copied by Stage-1 to SRAM 0x00804C00
 *   0x00F430+          : .text/.rodata + .data LMA, mapped by SPI NOR XIP to 0x00C0F430+
 *
 * SRAM 0x00800000..0x00804BFF (BootROM/RW-BT globals) and 0x00805C00..0x0081A37F
 * remain completely untouched. Stack starts at 0x0086AC08 descending.
 */
ENTRY(_start)

MEMORY
{
    RAM_BOOT  (rwx) : ORIGIN = 0x00804C00, LENGTH = 0x00001000
    XIP       (rx)  : ORIGIN = 0x00C0F430, LENGTH = 0x001ECBD0
    SRAM      (rwx) : ORIGIN = 0x0081A380, LENGTH = 0x00035C80
    EXP_ARENA (rwx) : ORIGIN = 0x00850000, LENGTH = 0x00008000
    EXP_OUT   (rw)  : ORIGIN = 0x00858000, LENGTH = 0x00004000
    SRAM_HI   (rwx) : ORIGIN = 0x0085C000, LENGTH = 0x0000B000
    /* Physical SRAM extends to 0x0087FFFF (stock FIRM used it for heaps). Unused by open_fw
     * unless built with OPENFW_RTOS=1; heap_5 region, NOT zeroed at boot. Verify on hardware. */
    RTOS_HEAP (rw)  : ORIGIN = 0x0086B000, LENGTH = 0x00015000
}

_estack = 0x0086AC08;

SECTIONS
{
    /* Stage-1 copies flash 0x00E430..0x00F42F (<= 4 KiB) to SRAM 0x00804C00 */
    .ram_boot 0x00804C00 : {
        KEEP(*(.text.entry))
        . = ALIGN(512);
        KEEP(*(.isr_vector))
        *(.ramtext .ramtext.*)
        . = ALIGN(4);
    } > RAM_BOOT

    ASSERT(SIZEOF(.ram_boot) <= 0x1000, ".ram_boot exceeds 4 KiB Stage-1 load window")

    /* Execute-in-Place code & read-only data mapped at 0x00C0F430 (flash 0x00F430) */
    .text 0x00C0F430 : {
        *(EXCLUDE_FILE(
            *lcd_st7735s.o
            *ui_port.o
            *ui_spectrogram.o
            *lv_draw_sw_fill.o
            *lv_draw_sw_letter.o
            *lv_draw_sw_blend.o
            *lv_draw_sw_blend_to_rgb565.o
            *lv_draw_sw_border.o
            *lv_draw_sw_mask_rect.o
            *lv_font_fmt_txt.o
            *lv_style.o
            *lv_tlsf.o
        ) .text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > XIP

    .data : ALIGN(4) {
        _sdata = .;
        /* Hot LCD/LVGL/UI code copied once at boot into zero-wait-state 96 MHz SRAM */
        *(.fast_ramtext .fast_ramtext.*)
        *lcd_st7735s.o(.text .text.*)
        *ui_port.o(.text .text.*)
        *ui_spectrogram.o(.text .text.*)
        *lv_draw_sw_fill.o(.text .text.*)
        *lv_draw_sw_letter.o(.text .text.*)
        *lv_draw_sw_blend.o(.text .text.*)
        *lv_draw_sw_blend_to_rgb565.o(.text .text.*)
        *lv_draw_sw_border.o(.text .text.*)
        *lv_draw_sw_mask_rect.o(.text .text.*)
        *lv_font_fmt_txt.o(.text .text.*)
        *lv_style.o(.text .text.*)
        *lv_tlsf.o(.text .text.*)
        . = ALIGN(4);
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > XIP

    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4) {
        _sbss = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    ASSERT(_ebss <= 0x00850000, ".data + .bss overflowed into EXP_ARENA")

    .exp_arena 0x00850000 (NOLOAD) : ALIGN(4) {
        KEEP(*(.exp_arena))
    } > EXP_ARENA

    .exp_out 0x00858000 (NOLOAD) : ALIGN(4) {
        KEEP(*(.exp_out))
    } > EXP_OUT

    .bss_hi (NOLOAD) : ALIGN(4) {
        _sbss_hi = .;
        *(.bss_hi .bss_hi.*)
        . = ALIGN(4);
        _ebss_hi = .;
    } > SRAM_HI

    ASSERT(_ebss_hi <= 0x00867000, ".bss_hi overflowed into main stack")

    /* RTOS build only: kernel-owned static objects (idle/timer TCBs + stacks), then heap_5 takes the rest. */
    .rtos_static (NOLOAD) : ALIGN(8) {
        _srtos_static = .;
        *(.rtos_static .rtos_static.*)
        . = ALIGN(8);
        _ertos_static = .;
    } > RTOS_HEAP

    .rtos_heap (NOLOAD) : ALIGN(8) {
        _srtos_heap0 = .;
        . = ORIGIN(RTOS_HEAP) + LENGTH(RTOS_HEAP);
        _ertos_heap0 = .;
    } > RTOS_HEAP

    ASSERT(_ertos_heap0 == ORIGIN(RTOS_HEAP) + LENGTH(RTOS_HEAP), ".rtos_heap must end at the RTOS_HEAP region end")
    ASSERT(_srtos_heap0 >= _ertos_static, ".rtos_heap overlaps .rtos_static")

    /DISCARD/ : {
        *(.ARM.exidx* .ARM.extab* .comment .note*)
    }
}
