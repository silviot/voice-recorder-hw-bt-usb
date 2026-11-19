/*
 * Smartlink SL6801 Internal 384 KB Mask ROM (0x000000..0x05FFFF) Bindings
 *
 * Verified Thumb-2 entry points baked into the SL6801 silicon Mask ROM.
 * Usable by standalone open-source FIRM images with zero stock FIRM code.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL6801_ROM_H
#define SL6801_ROM_H

#include <stdint.h>
#include <stddef.h>

#define ROM_FN(addr, ret, args) ((ret (*) args)((uintptr_t)(addr) | 1u))

/* Debug UART0 printf (115200 bps, PD0 TX) */
#define rom_printf          ROM_FN(0x000002B0, int,      (const char *fmt, ...))

/* GPIO helpers (pin_cfg = (port<<16)|(pin<<11)|(mux<<7)|(init<<6)|(drv<<4)|pull_idx) */
#define rom_gpio_config     ROM_FN(0x000007AC, void,     (uint32_t cfg))
#define rom_gpio_read       ROM_FN(0x000007EC, int,      (uint32_t cfg))
#define rom_gpio_write      ROM_FN(0x0000080E, void,     (uint32_t cfg, int val))

/* Standard libc primitives */
#define rom_strlen          ROM_FN(0x00000AA2, size_t,   (const char *s))
#define rom_memcpy          ROM_FN(0x00000B2A, void *,   (void *dst, const void *src, size_t n))
#define rom_memset          ROM_FN(0x00000BAA, void *,   (void *dst, int val, size_t n))

/* CRU Clock & Reset helpers */
#define rom_rst_is_released ROM_FN(0x00002518, int,      (uint32_t rst_id))
#define rom_rst_release     ROM_FN(0x00002564, void,     (uint32_t rst_id))
#define rom_rst_hold        ROM_FN(0x00002570, void,     (uint32_t rst_id))
#define rom_clk_enable      ROM_FN(0x000027A0, void,     (uint32_t clk_id))
#define rom_clk_disable     ROM_FN(0x00002A7C, void,     (uint32_t clk_id))
#define rom_clk_div         ROM_FN(0x00002D58, void,     (uint32_t clk_id, uint32_t div))
#define rom_clk_set_src     ROM_FN(0x00003118, void,     (uint32_t clk_id, uint32_t src))
#define rom_clk_set_rate    ROM_FN(0x00003904, void,     (uint32_t clk_id, uint32_t hz))

/* 64-bit System Microsecond Timer, Delay & WDT */
#define rom_us_timer_get    ROM_FN(0x0000B6D8, uint64_t, (void))
#define rom_us_delay        ROM_FN(0x0000B730, void,     (uint32_t us))
#define rom_wdt_reset       ROM_FN(0x0000B7B0, void,     (uint32_t timeout))

#endif /* SL6801_ROM_H */
