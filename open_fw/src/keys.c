/*
 * Smartlink SL6801 Bare-Metal SARADC Key Ladder (0x40096000) & Rec Switch (PB6)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

void keys_init(void)
{
    /* PD0 = Analog mode (mux=15, floating) for kadc_ch1 resistor ladder */
    openfw_mmio_clr32(GPIOD_BASE + 0x24u, 0x3u);
    gpio_set_mux(GPIO_PORT_D, 0u, 15u);

    /* PB6 = GPIO input for Record slide switch */
    gpio_set_mux(GPIO_PORT_B, 6u, 0u);

    /* Release SARADC reset & enable clock (CRU bank 2 bit 20 = ID 0x54) */
    openfw_mmio_set32(CRU_SARADC_RST_ADDR, 1u << 20);
    openfw_mmio_set32(CRU_SARADC_CLK_ADDR, 1u << 20);
    while ((openfw_mmio_rd32(CRU_SARADC_CLK_ADDR) & (1u << 20)) == 0u) {
    }

    /* Configure continuous polling scan on ch1 (0x40096034) */
    openfw_mmio_wr32(SARADC_CTRL_ADDR,    0x80180000u | (1u << 1));
    openfw_mmio_wr32(SARADC_START_ADDR,   0u);
    openfw_mmio_wr32(SARADC_IE_ADDR,      0u);
    openfw_mmio_wr32(SARADC_REG10_ADDR,   0u);
    openfw_mmio_wr32(SARADC_REG18_ADDR,   0u);
    openfw_mmio_wr32(SARADC_CH1_THR_ADDR, 0x0FFF0030u);
    mdelay(5u);
    openfw_mmio_wr32(SARADC_START_ADDR,   0x0000A800u);
}

uint16_t keys_read_adc(void)
{
    return (uint16_t)(openfw_mmio_rd32(SARADC_CH1_DATA_ADDR) & 0x0FFFu);
}

int keys_read_rec_switch(void)
{
    return (gpio_read(GPIO_PORT_B, 6u) == 0) ? 1 : 0;
}

key_id_t keys_decode(uint16_t adc)
{
    if (adc > 3200u) {
        return KEY_NONE;
    }
    if (adc >= 200u && adc <= 400u) {
        return KEY_LEFT;
    }
    if (adc >= 580u && adc <= 920u) {
        return KEY_RIGHT;
    }
    if (adc >= 1200u && adc <= 1500u) {
        return KEY_VOL_UP;
    }
    if (adc >= 2050u && adc <= 2300u) {
        return KEY_VOL_DOWN;
    }
    if (adc >= 2450u && adc <= 2650u) {
        return KEY_BACK;
    }
    if (adc >= 2670u && adc <= 3050u) {
        return KEY_ENTER;
    }
    return KEY_UNKNOWN;
}

const char *keys_name(key_id_t key)
{
    switch (key) {
    case KEY_NONE:     return "NONE";
    case KEY_LEFT:     return "PREV";
    case KEY_RIGHT:    return "NEXT";
    case KEY_VOL_UP:   return "VOL+";
    case KEY_VOL_DOWN: return "VOL-";
    case KEY_BACK:     return "BACK";
    case KEY_ENTER:    return "PLAY";
    default:           return "UNK";
    }
}
