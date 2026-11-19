/*
 * Smartlink SL6801 Bare-Metal UART0 Debug Console Driver (0x40090000, PD0 TX)
 *
 * NOTE: PD0 is physically shared with SARADC kadc_ch1 (resistor-ladder buttons).
 * Call uart0_* during early boot before keys_init() switches PD0 to analog mode.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

void uart0_init_115200(void)
{
    gpio_set_mux(GPIO_PORT_D, 0u, 12u); /* PD0 = ALT12 (UART0 TX) */
    UART0_IE = 0u;
    UART0_REG18 &= ~3u;
    UART0_REG1C = 0u;
    UART0_REG20 = 0u;
    /* div = 24,000,000 / (115200 * 16) = 13 -> EN | TX | RX */
    UART0_CTRL = (13u << 16) | (13u << 4) | 0x7u;
}

void uart0_putc(char c)
{
    if (c == '\n') {
        while ((UART0_STAT & 0x1Fu) == 0u) {
        }
        UART0_DATA = (uint32_t)'\r';
    }
    while ((UART0_STAT & 0x1Fu) == 0u) {
    }
    UART0_DATA = (uint32_t)(uint8_t)c;
}

void uart0_puts(const char *s)
{
    while (*s != '\0') {
        uart0_putc(*s++);
    }
}

void uart0_put_u32(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0u) {
        uart0_putc('0');
        return;
    }
    while (v > 0u && i < 11) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        uart0_putc(buf[--i]);
    }
}

void uart0_put_hex(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    uart0_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart0_putc(hex[(v >> shift) & 0xFu]);
    }
}
