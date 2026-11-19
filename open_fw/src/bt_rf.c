/*
 * Smartlink SL6801 Clean-Room Bluetooth RF, eFuse Trim & Modem Driver
 * (`0x40010000` / `0x40011000` / `0x40099000`)
 *
 * Implements verified RF CRU reset, eFuse trim readout, 330-entry (0x00..0x149)
 * RF LUT programming, RF analog register configuration, AON XTAL freq_drift
 * trim, and BT modem calibration tables with zero stock FIRM calls.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

#define EFUSE_RST_ID 0x58u
#define EFUSE_CLK_ID 0x1Bu

static void rf_delay(uint32_t n)
{
    for (volatile uint32_t i = n * 200u; i != 0u; i--) {
        __asm__ volatile ("nop");
    }
}

uint32_t bt_rf_efuse_trim(void)
{
    rom_rst_release(EFUSE_RST_ID);
    rom_clk_enable(EFUSE_CLK_ID);
    if ((EFUSE_WORD(3u) >> 31) == 0u) {
        return 4u;
    }
    return (EFUSE_WORD(1u) >> 14) & 0xFu;
}

static void rf_lut_write(uint32_t addr, uint32_t val)
{
    RF_LUT_DATA = val;
    RF_LUT_CTRL = (RF_LUT_CTRL & ~0x1FFu) | (addr & 0x1FFu);
    RF_LUT_CTRL |= (1u << 11);
    RF_LUT_CTRL |= (1u << 16);
    while (RF_LUT_CTRL & (1u << 16)) {
    }
}

static void rf_lut_populate_defaults(void)
{
    static const uint8_t bank0_id0[26] = {
        1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
        17u, 17u, 17u, 49u, 49u, 49u, 49u, 81u, 113u, 113u,
        241u, 115u, 115u, 243u, 243u, 255u,
    };
    static const int8_t bank2_id6[26] = {
        -23, -20, -16, -13, -10, -7, -4, -1, 2, 5,
        -1, 2, 5, -1, 2, 5, 9, 6, 5, 9,
        3, 4, 6, 1, 4, -4,
    };
    static const uint8_t bank4_id13[27] = {
        6u, 6u, 6u, 5u, 4u, 2u, 1u, 0u, 2u, 1u,
        2u, 1u, 0u, 0u, 0u, 8u, 8u, 8u, 16u, 16u,
        16u, 24u, 24u, 24u, 24u, 24u, 24u,
    };
    static const uint8_t bank5_id16[27] = {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u,
        2u, 2u, 2u, 3u, 4u, 2u, 3u, 4u, 2u, 3u,
        4u, 2u, 3u, 4u, 6u, 9u, 15u,
    };

    /* 0x00..0x19: id 0..2 */
    for (uint32_t i = 0u; i < 26u; i++) {
        uint32_t w = ((uint32_t)bank0_id0[i] << 24) | (1u << 12) | 1u;
        rf_lut_write(0x00u + i, w);
    }

    /* 0x1A..0x33: id 3..5 */
    for (uint32_t i = 0u; i < 26u; i++) {
        uint32_t w = (7u << 24) | (1u << 12) | 1u;
        rf_lut_write(0x1Au + i, w);
    }

    /* 0x34..0x4D: id 6..9 */
    for (uint32_t i = 0u; i < 26u; i++) {
        uint32_t f6 = (uint32_t)(bank2_id6[i]) & 0x3Fu;
        uint32_t w  = (f6 << 24) | (1u << 20) | (1u << 10) | 1u;
        rf_lut_write(0x34u + i, w);
    }

    /* 0x4E..0x67: id 10..11 */
    for (uint32_t i = 0u; i < 26u; i++) {
        uint32_t w = (1u << 10) | 1u;
        rf_lut_write(0x4Eu + i, w);
    }

    /* 0x68..0x82: id 12..15 */
    for (uint32_t i = 0u; i < 27u; i++) {
        uint32_t f12 = (i == 0u) ? 0u : (i == 1u) ? 2u : 3u;
        uint32_t w   = (f12 << 21) | ((uint32_t)bank4_id13[i] << 16) | (1u << 8) | 1u;
        rf_lut_write(0x68u + i, w);
    }

    /* 0x83..0x9D: id 16..23 */
    for (uint32_t i = 0u; i < 27u; i++) {
        uint32_t w = ((uint32_t)bank5_id16[i] << 28) | (1u << 10) | 1u;
        if (i == 0u) {
            w |= (1u << 26) | (1u << 25) | (1u << 24) | (1u << 22) | (1u << 20);
        }
        rf_lut_write(0x83u + i, w);
    }

    /* 0x9E..0xF3: id 24..25 (86-channel 2.4 GHz synth int/frac) */
    for (uint32_t ch = 0u; ch < 86u; ch++) {
        uint32_t pll_int  = (2400u + ch) / 12u;
        uint32_t pll_frac = (((2400u + ch) % 12u) << 21) / 12u;
        rf_lut_write(0x9Eu + ch, (pll_int << 21) | pll_frac);
    }

    /* 0xF4..0x149: id 26..29 VCO defaults */
    for (uint32_t ch = 0u; ch < 86u; ch++) {
        rf_lut_write(0xF4u + ch, 0x01010101u);
    }
}

static void rf_cru_reset(void)
{
    CRU_BT_RF_WRAP   &= ~(1u << 0);
    CRU_RST_STAT1    &= ~(1u << 2);
    CRU_RST_SET1     &= ~(1u << 2);
    CRU_CLK_SRC_GATE |=  (1u << 4);
    CRU_RST_SET1     |=  (1u << 2);
    CRU_RST_STAT1    |=  (1u << 2);
    CRU_BT_RF_WRAP   |=  (1u << 0);
    rf_delay(1u);
}

void bt_rf_init(uint32_t freq_drift, uint32_t vco_cal_10a4)
{
    uint32_t trim = bt_rf_efuse_trim();
    rf_cru_reset();

    RF_BB_CTRL |= (1u << 26);
    REG32(RF_ANA_BASE + 0x20u) &= ~(1u << 0);
    REG32(RF_ANA_BASE + 0x84u) |=  (1u << 19);
    REG32(RF_ANA_BASE + 0x40u) |=  (1u << 5) | ((trim & 0xFu) << 16);
    REG32(RF_ANA_BASE + 0xACu) |=  (1u << 16);
    RF_LUT_CTRL = (RF_LUT_CTRL & ~0xFFF00000u) | 0x04000000u;

    rf_lut_populate_defaults();

    REG32(RF_ANA_BASE + 0xA0u) = (REG32(RF_ANA_BASE + 0xA0u) & ~0x71040000u) | 0x30000000u;
    REG32(RF_ANA_BASE + 0xA4u) = (REG32(RF_ANA_BASE + 0xA4u) & ~0xFF03F370u)
                               | 0xE0000B40u | ((vco_cal_10a4 & 0x1Fu) << 24);
    REG32(RF_ANA_BASE + 0xA8u) = (REG32(RF_ANA_BASE + 0xA8u) & ~0x00000FF0u) | 0x00000450u
                               | ((REG32(RF_BB_BASE + 0x120u) & 0x1FFFu) ? 0x100u : 0u);

    REG32(RF_ANA_BASE + 0x60u) = (REG32(RF_ANA_BASE + 0x60u) & ~0x7F037C07u) | 0x35023407u;
    REG32(RF_ANA_BASE + 0x80u) = (REG32(RF_ANA_BASE + 0x80u) & ~0x00000007u) | 0x00000004u;

    REG32(RF_ANA_BASE + 0x88u) = (REG32(RF_ANA_BASE + 0x88u) & ~0xF0003F00u) | 0x60002D00u;
    REG32(RF_ANA_BASE + 0x84u) = (REG32(RF_ANA_BASE + 0x84u) & ~0xF0000000u) | 0x60000000u;
    REG32(RF_ANA_BASE + 0x20u) &= ~(1u << 21);
    REG32(RF_ANA_BASE + 0x20u) |=  (1u << 21);
    REG32(RF_ANA_BASE + 0x24u) &= ~0xF0000000u;

    REG32(RF_ANA_BASE + 0xC0u) = (REG32(RF_ANA_BASE + 0xC0u) & ~0x7Fu) | 0x00000229u;
    REG32(RF_ANA_BASE + 0xC0u) |= (1u << 8);
    while (REG32(RF_ANA_BASE + 0xC0u) & (1u << 8)) {
    }
    REG32(RF_ANA_BASE + 0xC0u) &= ~(1u << 9);
    REG32(RF_ANA_BASE + 0xE4u) = (REG32(RF_ANA_BASE + 0xE4u) & ~0x3FE00000u) | 0x00E00000u;
    AON_CLK_CFG = (AON_CLK_CFG & ~0x00000FF0u) | (((freq_drift > 0xFFu) ? 0x4Cu : freq_drift) << 4);
    REG32(RF_ANA_BASE + 0xB8u) &= ~0x60000000u;
    REG32(RF_ANA_BASE + 0xBCu) = 0x1E1D0303u;

    REG32(RF_ANA_BASE + 0x80u) = (REG32(RF_ANA_BASE + 0x80u) & ~0x7F00u) | 0x00021A00u;
    REG32(RF_ANA_BASE + 0x80u) |= (1u << 16);
    while (REG32(RF_ANA_BASE + 0x80u) & (1u << 16)) {
    }
    REG32(RF_ANA_BASE + 0x80u) &= ~(1u << 17);
    REG32(RF_ANA_BASE + 0xACu) = (REG32(RF_ANA_BASE + 0xACu) & ~0x1Fu) | 0x00000212u;
    REG32(RF_ANA_BASE + 0xACu) |= (1u << 8);
    while (REG32(RF_ANA_BASE + 0xACu) & (1u << 8)) {
    }
    REG32(RF_ANA_BASE + 0xACu) &= ~(1u << 9);

    REG32(RF_ANA_BASE + 0x40u) &= ~0x00007F00u;
    REG32(RF_ANA_BASE + 0x80u) = (REG32(RF_ANA_BASE + 0x80u) & ~0x7FF00000u) | 0x36000000u;
    REG32(RF_ANA_BASE + 0xA0u) = (REG32(RF_ANA_BASE + 0xA0u) & ~0x0000FF07u) | 0x00006000u;
    REG32(RF_ANA_BASE + 0xACu) = (REG32(RF_ANA_BASE + 0xACu) & ~0xFFFC0000u) | 0x0A140000u;
    REG32(RF_ANA_BASE + 0xB0u) = (REG32(RF_ANA_BASE + 0xB0u) & ~0x7E7F7F7Fu) | 0x3A033100u;

    RF_BB_CTRL &= ~(1u << 26);
    rf_delay(100u);
}

void bt_modem_init(uint32_t clk6_hz)
{
    uint32_t div = (clk6_hz <= 16000000u) ? 1u : 2u;
    uint32_t mhz = (clk6_hz / 1000000u) / div;
    RF_BB_CTRL = (RF_BB_CTRL & ~0x073F0000u) | ((div - 1u) << 24) | (mhz << 16);

    REG32(RF_BB_BASE + 0x298u) = 0x2D2B2927u;
    REG32(RF_BB_BASE + 0x29Cu) = 0x3533312Fu;
    REG32(RF_BB_BASE + 0x2A0u) = 0x3D3B3937u;
    REG32(RF_BB_BASE + 0x2A4u) = 0x4543413Fu;
    REG32(RF_BB_BASE + 0x2A8u) = 0x4D4B4947u;
    REG32(RF_BB_BASE + 0x2ACu) = 0x5553514Fu;
    REG32(RF_BB_BASE + 0x2B0u) = 0x00005957u;

    REG32(RF_BB_BASE + 0x01Cu) = 0x00000001u;
    REG32(RF_BB_BASE + 0x100u) = 0x00000080u;
    REG32(RF_BB_BASE + 0x120u) = 0x800703E8u;
    REG32(RF_BB_BASE + 0x2F0u) = 0x0022C3E8u;
    REG32(RF_BB_BASE + 0x270u) = 0x9A805E1Bu;
    REG32(RF_BB_BASE + 0x274u) = 0x00F00341u;
    REG32(RF_BB_BASE + 0x280u) = 0x0EEDE000u;
    REG32(RF_BB_BASE + 0x20Cu) = 0x18601111u;
    REG32(RF_BB_BASE + 0x290u) = (REG32(RF_BB_BASE + 0x290u) & ~0x003F3F00u) | 0x00393F00u;
    REG32(RF_BB_BASE + 0x21Cu) = 0x000CB88Au;
    REG32(RF_BB_BASE + 0x2C0u) = 0x600C6020u;
    REG32(RF_BB_BASE + 0x2C4u) = 0x01303779u;
    REG32(RF_BB_BASE + 0x108u) = 0x14360422u;
    REG32(RF_BB_BASE + 0x2C8u) = 0x031CC1F4u;
    REG32(RF_BB_BASE + 0x2B8u) = 0x001350C0u;
    REG32(RF_BB_BASE + 0x2F8u) = 0x00013765u;
    REG32(RF_BB_BASE + 0x228u) = (REG32(RF_BB_BASE + 0x228u) & ~0x00003F00u) | 0x00001C00u;
    REG32(RF_BB_BASE + 0x2F4u) = (REG32(RF_BB_BASE + 0x2F4u) & ~0x00003F00u) | 0x00002200u;
    REG32(RF_BB_BASE + 0x2FCu) = (REG32(RF_BB_BASE + 0x2FCu) & ~0x0003F000u) | 0x0002C000u;
    REG32(RF_BB_BASE + 0x200u) = (REG32(RF_BB_BASE + 0x200u) & ~0x00008000u) | 0x00002000u;
    REG32(RF_BB_BASE + 0x30Cu) = 0x00353B73u;
}
