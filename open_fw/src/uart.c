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

#define UART0_TX_WAIT_MAX 50000u

openfw_log_t g_openfw_log;

void openfw_log_reset(void)
{
    g_openfw_log.magic         = OPENFW_LOG_MAGIC;
    g_openfw_log.size          = OPENFW_LOG_SIZE;
    g_openfw_log.write_idx     = 0u;
    g_openfw_log.dropped_bytes = 0u;
    for (uint32_t i = 0u; i < OPENFW_LOG_SIZE; i++) {
        g_openfw_log.buf[i] = '\0';
    }
}

static void log_ring_push(char c)
{
    if (g_openfw_log.magic != OPENFW_LOG_MAGIC || g_openfw_log.size != OPENFW_LOG_SIZE) {
        openfw_log_reset();
    }
    g_openfw_log.buf[g_openfw_log.write_idx & (OPENFW_LOG_SIZE - 1u)] = c;
    g_openfw_log.write_idx++;
}

static void uart0_tx_raw(uint8_t b)
{
    for (uint32_t i = 0u; i < UART0_TX_WAIT_MAX; i++) {
        if ((openfw_mmio_rd32(UART0_STAT_ADDR) & 0x1Fu) != 0u) {
            openfw_mmio_wr32(UART0_DATA_ADDR, (uint32_t)b);
            return;
        }
    }
    g_openfw_log.dropped_bytes++;
}

void uart0_init_115200(void)
{
    if (g_openfw_log.magic != OPENFW_LOG_MAGIC || g_openfw_log.size != OPENFW_LOG_SIZE) {
        openfw_log_reset();
    }
    gpio_set_mux(GPIO_PORT_D, 0u, 12u); /* PD0 = ALT12 (UART0 TX) */
    openfw_mmio_wr32(UART0_IE_ADDR, 0u);
    openfw_mmio_clr32(UART0_REG18_ADDR, 3u);
    openfw_mmio_wr32(UART0_REG1C_ADDR, 0u);
    openfw_mmio_wr32(UART0_REG20_ADDR, 0u);
    /* div = 24,000,000 / (115200 * 16) = 13 -> EN | TX | RX */
    openfw_mmio_wr32(UART0_CTRL_ADDR, (13u << 16) | (13u << 4) | 0x7u);
}

void openfw_log_putc(char c)
{
    log_ring_push(c);
}

void openfw_log_puts(const char *s)
{
    if (!s) {
        return;
    }
    while (*s != '\0') {
        openfw_log_putc(*s++);
    }
}

static void fmt_u32(void (*putc_fn)(char), uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0u) {
        putc_fn('0');
        return;
    }
    while (v > 0u && i < 11) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        putc_fn(buf[--i]);
    }
}

static void fmt_hex(void (*putc_fn)(char), uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    putc_fn('0');
    putc_fn('x');
    for (int shift = 28; shift >= 0; shift -= 4) {
        putc_fn(hex[(v >> shift) & 0xFu]);
    }
}

void openfw_log_u32(uint32_t v)
{
    fmt_u32(openfw_log_putc, v);
}

void openfw_log_hex(uint32_t v)
{
    fmt_hex(openfw_log_putc, v);
}

void uart0_putc(char c)
{
    openfw_log_putc(c);
    if (c == '\n') {
        uart0_tx_raw((uint8_t)'\r');
    }
    uart0_tx_raw((uint8_t)c);
}

void uart0_puts(const char *s)
{
    if (!s) {
        return;
    }
    while (*s != '\0') {
        uart0_putc(*s++);
    }
}

void uart0_put_u32(uint32_t v)
{
    fmt_u32(uart0_putc, v);
}

void uart0_put_hex(uint32_t v)
{
    fmt_hex(uart0_putc, v);
}
