/*
 * Synthetic QEMU SL6801 Reset Stub (loaded at 0x10000000 via rom-stub=)
 *
 * Emulates Stage-1 HLKJ's handoff for Cassini OpenFW:
 *   1. Provides Cortex-M4F vector table with MSP=_estack (0x0086AC08)
 *   2. Copies 4 KiB .ram_boot from XIP (0x00C0E430) to SRAM (0x00804C00)
 *   3. Jumps in Thumb state to _start (0x00804C01)
 *
 * SPDX-License-Identifier: MIT
 */
.syntax unified
.cpu cortex-m4
.thumb

.section .text,"ax",%progbits
.global _qemu_stub_vectors
_qemu_stub_vectors:
    .word 0x0086AC08          /*  0: Initial MSP (_estack) */
    .word reset_handler       /*  1: Reset vector (.thumb_func already sets bit 0) */
    .rept 14
    .word hang
    .endr

.thumb_func
hang:
    wfi
    b hang

.thumb_func
reset_handler:
    cpsid i
    movw r0, #0xE430          /* XIP source: 0x00C0E430 (.ram_boot) */
    movt r0, #0x00C0
    movw r1, #0x4C00          /* SRAM dest:  0x00804C00 */
    movt r1, #0x0080
    movw r2, #0x1000          /* Copy 4 KiB */
    movt r2, #0x0000
copy_ram_boot:
    ldr  r3, [r0], #4
    str  r3, [r1], #4
    subs r2, r2, #4
    bne  copy_ram_boot
    dsb
    isb
    movw r0, #0x4C01          /* Enter open_fw _start (Thumb) */
    movt r0, #0x0080
    bx   r0
