/*
 * Smartlink SL6801 Clean-Room Bluetooth RF, Silicon VCO/BPF Calibration,
 * eFuse Trim & Modem Driver (`0x40010000` / `0x40011000` / `0x40099000`)
 *
 * Implements verified RF CRU reset, eFuse trim readout, 86-channel VCO
 * band/cap calibration (`0x00D528A0`), 3 dB RX BPF/LPF corner calibration
 * (`0x00D53B00`) in pure fixed-point integer math (zero libm/float), 330-entry
 * (`0x00..0x149`) RF LUT programming, and BT modem bring-up (`0x00D54D38`).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

#define EFUSE_RST_ID 0x58u
#define EFUSE_CLK_ID 0x1Bu

static const uint8_t g_rf_lut_base[30] = {
    0x00u, 0x00u, 0x00u, 0x1Au, 0x1Au, 0x1Au, 0x34u, 0x34u, 0x34u, 0x34u,
    0x4Eu, 0x4Eu,        0x68u, 0x68u, 0x68u, 0x68u,
    0x83u, 0x83u, 0x83u, 0x83u, 0x83u, 0x83u, 0x83u, 0x83u,
    0x9Eu, 0x9Eu,        0xF4u, 0xF4u, 0xF4u, 0xF4u,
};

static const uint8_t g_rf_lut_shift[30] = {
    24u, 12u,  0u, 24u, 12u,  0u, 24u, 20u, 10u,  0u,
    10u,  0u,      21u, 16u,  8u,  0u,
    28u, 26u, 25u, 24u, 22u, 20u, 10u,  0u,
    21u,  0u,       0u,  8u, 16u, 24u,
};

static const uint32_t g_rf_lut_mask[30] = {
    0xFFu, 0xFFFu, 0xFFFu, 0x07u, 0xFFFu, 0xFFFu, 0x3Fu, 0x0Fu, 0x3FFu, 0x3FFu,
    0x3FFu, 0x3FFu,        0x03u, 0x1Fu,  0x7Fu,  0x7Fu,
    0x0Fu, 0x01u, 0x01u, 0x01u, 0x03u, 0x03u, 0x3FFu, 0x3FFu,
    0x1FFu, 0x1FFFFFu,     0xFFu, 0x7Fu, 0xFFu, 0x7Fu,
};

/* Quarter-wave Q10 sine table: round(1024 * sin(2 * pi * i / 1024)), i=0..256 */
static const int16_t g_sin_q257[257] = {
       0,    6,   13,   19,   25,   31,   38,   44,   50,   57,   63,   69,   75,   82,   88,   94,
     100,  107,  113,  119,  125,  132,  138,  144,  150,  156,  163,  169,  175,  181,  187,  194,
     200,  206,  212,  218,  224,  230,  237,  243,  249,  255,  261,  267,  273,  279,  285,  291,
     297,  303,  309,  315,  321,  327,  333,  339,  345,  351,  357,  363,  369,  374,  380,  386,
     392,  398,  403,  409,  415,  421,  426,  432,  438,  443,  449,  455,  460,  466,  472,  477,
     483,  488,  494,  499,  505,  510,  516,  521,  526,  532,  537,  543,  548,  553,  558,  564,
     569,  574,  579,  584,  590,  595,  600,  605,  610,  615,  620,  625,  630,  635,  640,  645,
     650,  654,  659,  664,  669,  674,  678,  683,  688,  692,  697,  702,  706,  711,  715,  720,
     724,  729,  733,  737,  742,  746,  750,  755,  759,  763,  767,  771,  775,  779,  784,  788,
     792,  796,  799,  803,  807,  811,  815,  819,  822,  826,  830,  834,  837,  841,  844,  848,
     851,  855,  858,  862,  865,  868,  872,  875,  878,  882,  885,  888,  891,  894,  897,  900,
     903,  906,  909,  912,  915,  917,  920,  923,  926,  928,  931,  934,  936,  939,  941,  944,
     946,  948,  951,  953,  955,  958,  960,  962,  964,  966,  968,  970,  972,  974,  976,  978,
     980,  982,  983,  985,  987,  989,  990,  992,  993,  995,  996,  998,  999, 1000, 1002, 1003,
    1004, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1016, 1017, 1018, 1018,
    1019, 1020, 1020, 1021, 1021, 1022, 1022, 1022, 1023, 1023, 1023, 1024, 1024, 1024, 1024, 1024,
    1024,
};

static int16_t trig_sin1024(uint32_t idx)
{
    idx &= 1023u;
    if (idx <= 256u) {
        return g_sin_q257[idx];
    }
    if (idx < 512u) {
        return g_sin_q257[512u - idx];
    }
    if (idx <= 768u) {
        return (int16_t)(-g_sin_q257[idx - 512u]);
    }
    return (int16_t)(-g_sin_q257[1024u - idx]);
}

static int16_t trig_cos1024(uint32_t idx)
{
    return trig_sin1024(idx + 256u);
}

static uint32_t int_log2_q8(uint64_t v)
{
    static const uint16_t lut16[17] = {
        0u, 22u, 44u, 63u, 82u, 100u, 118u, 134u, 150u,
        165u, 179u, 193u, 207u, 220u, 232u, 244u, 256u,
    };
    if (v <= 1u) {
        return 0u;
    }
    uint32_t msb = 0u;
    uint64_t t   = v;
    while (t >= (1ULL << 32)) {
        t >>= 32;
        msb += 32u;
    }
    uint32_t hi32 = (uint32_t)t;
    while (hi32 >= 256u) {
        hi32 >>= 8;
        msb += 8u;
    }
    while (hi32 > 1u) {
        hi32 >>= 1;
        msb++;
    }
    uint32_t frac4 = (msb >= 4u) ? (uint32_t)((v >> (msb - 4u)) & 0xFu)
                                 : (uint32_t)((v << (4u - msb)) & 0xFu);
    uint32_t rem   = (msb >= 8u) ? (uint32_t)((v >> (msb - 8u)) & 0xFu) : 0u;
    uint32_t a     = lut16[frac4];
    uint32_t b     = lut16[frac4 + 1u];
    return (msb << 8) + a + (((b - a) * rem) >> 4);
}

#define RF_POLL_TIMEOUT_ITERS 200000u

static void rf_poll_clear32(uint32_t addr, uint32_t mask)
{
    for (uint32_t i = 0u; i < RF_POLL_TIMEOUT_ITERS; i++) {
        if ((openfw_mmio_rd32(addr) & mask) == 0u) {
            return;
        }
    }
}

static void rf_delay(uint32_t n)
{
    if (n > 0u) {
        udelay(n * 20u);
    }
}

uint32_t bt_rf_efuse_trim(void)
{
    rom_rst_release(EFUSE_RST_ID);
    rom_clk_enable(EFUSE_CLK_ID);
    if ((openfw_mmio_rd32(EFUSE_WORD_ADDR(3u)) >> 31) == 0u) {
        return 4u;
    }
    return (openfw_mmio_rd32(EFUSE_WORD_ADDR(1u)) >> 14) & 0xFu;
}

static uint32_t rf_lut_read_raw(uint32_t addr)
{
    openfw_mmio_rmw32(RF_LUT_CTRL_ADDR, 0x1FFu, addr & 0x1FFu);
    openfw_mmio_clr32(RF_LUT_CTRL_ADDR, (1u << 11));
    openfw_mmio_set32(RF_LUT_CTRL_ADDR, (1u << 16));
    rf_poll_clear32(RF_LUT_CTRL_ADDR, (1u << 16));
    return openfw_mmio_rd32(RF_LUT_DATA_ADDR);
}

static void rf_lut_write_raw(uint32_t addr, uint32_t val)
{
    openfw_mmio_wr32(RF_LUT_DATA_ADDR, val);
    openfw_mmio_rmw32(RF_LUT_CTRL_ADDR, 0x1FFu, addr & 0x1FFu);
    openfw_mmio_set32(RF_LUT_CTRL_ADDR, (1u << 11));
    openfw_mmio_set32(RF_LUT_CTRL_ADDR, (1u << 16));
    rf_poll_clear32(RF_LUT_CTRL_ADDR, (1u << 16));
}

static uint32_t rf_lut_read_field(uint32_t id, uint32_t idx)
{
    return (rf_lut_read_raw(g_rf_lut_base[id] + idx) >> g_rf_lut_shift[id]) & g_rf_lut_mask[id];
}

static void rf_lut_write_field(uint32_t id, uint32_t idx, uint32_t val)
{
    uint32_t addr = g_rf_lut_base[id] + idx;
    uint32_t sh   = g_rf_lut_shift[id];
    uint32_t m    = g_rf_lut_mask[id];
    uint32_t cur  = rf_lut_read_raw(addr);
    rf_lut_write_raw(addr, (cur & ~(m << sh)) | ((val & m) << sh));
}

static void rf_lut_populate(const uint8_t tx_band[86], const uint8_t tx_cap[86],
                            const uint8_t rx_band[86], const uint8_t rx_cap[86])
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

    for (uint32_t i = 0u; i < 26u; i++) {
        rf_lut_write_raw(0x00u + i, ((uint32_t)bank0_id0[i] << 24) | (1u << 12) | 1u);
    }
    for (uint32_t i = 0u; i < 26u; i++) {
        rf_lut_write_raw(0x1Au + i, (7u << 24) | (1u << 12) | 1u);
    }
    for (uint32_t i = 0u; i < 26u; i++) {
        uint32_t f6 = (uint32_t)(bank2_id6[i]) & 0x3Fu;
        rf_lut_write_raw(0x34u + i, (f6 << 24) | (1u << 20) | (1u << 10) | 1u);
    }
    for (uint32_t i = 0u; i < 26u; i++) {
        rf_lut_write_raw(0x4Eu + i, (1u << 10) | 1u);
    }
    for (uint32_t i = 0u; i < 27u; i++) {
        uint32_t f12 = (i == 0u) ? 0u : (i == 1u) ? 2u : 3u;
        rf_lut_write_raw(0x68u + i, (f12 << 21) | ((uint32_t)bank4_id13[i] << 16) | (1u << 8) | 1u);
    }
    for (uint32_t i = 0u; i < 27u; i++) {
        uint32_t w = ((uint32_t)bank5_id16[i] << 28) | (1u << 10) | 1u;
        if (i == 0u) {
            w |= (1u << 26) | (1u << 25) | (1u << 24) | (1u << 22) | (1u << 20);
        }
        rf_lut_write_raw(0x83u + i, w);
    }
    for (uint32_t ch = 0u; ch < 86u; ch++) {
        uint32_t pll_int  = (2400u + ch) / 12u;
        uint32_t pll_frac = (((2400u + ch) % 12u) << 21) / 12u;
        rf_lut_write_raw(0x9Eu + ch, (pll_int << 21) | pll_frac);
    }
    for (uint32_t ch = 0u; ch < 86u; ch++) {
        if (tx_band && tx_cap && rx_band && rx_cap) {
            uint32_t w = ((uint32_t)tx_band[ch] & 0xFFu)
                       | (((uint32_t)tx_cap[ch] & 0x7Fu) << 8)
                       | (((uint32_t)rx_band[ch] & 0xFFu) << 16)
                       | (((uint32_t)rx_cap[ch] & 0x7Fu) << 24);
            rf_lut_write_raw(0xF4u + ch, w);
        } else {
            rf_lut_write_raw(0xF4u + ch, 0x01010101u);
        }
    }
}

static void rf_cru_reset(void)
{
    openfw_mmio_clr32(CRU_BT_RF_WRAP_ADDR, (1u << 0));
    openfw_mmio_clr32(CRU_RST_STAT1_ADDR, (1u << 2));
    openfw_mmio_clr32(CRU_RST_SET1_ADDR, (1u << 2));
    openfw_mmio_set32(CRU_CLK_SRC_GATE_ADDR, (1u << 4));
    openfw_mmio_set32(CRU_RST_SET1_ADDR, (1u << 2));
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, (1u << 2));
    openfw_mmio_set32(CRU_BT_RF_WRAP_ADDR, (1u << 0));
    rf_delay(1u);
}

static void bt_rf_cal_mode_reset(void)
{
    rf_cru_reset();
    openfw_mmio_set32(RF_BB_CTRL_ADDR, (1u << 26));
    openfw_mmio_clr32(RF_ANA_BASE + 0x20u, 0x1u);
    openfw_mmio_set32(RF_ANA_BASE + 0x2Cu, 0x4u);
    openfw_mmio_set32(RF_ANA_BASE + 0x40u, 0x20u);
    openfw_mmio_set32(RF_ANA_BASE + 0x64u, (1u << 19) | (1u << 18) | (1u << 20));
    openfw_mmio_set32(RF_ANA_BASE + 0x80u, (1u << 17));
    openfw_mmio_set32(RF_ANA_BASE + 0x84u, (1u << 18) | (1u << 19));
    openfw_mmio_set32(RF_ANA_BASE + 0xA0u, (1u << 31) | (1u << 27));
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, (1u << 9) | (1u << 16));
    openfw_mmio_set32(RF_ANA_BASE + 0xB0u, (1u << 24));
    openfw_mmio_set32(RF_ANA_BASE + 0xB8u, (1u << 28));
    openfw_mmio_set32(RF_ANA_BASE + 0xC0u, (1u << 9));
    openfw_mmio_clr32(RF_LUT_CTRL_ADDR, 0xFFF00000u);
    openfw_mmio_clr32(RF_BB_BASE + 0x200u, 0x420u);
    openfw_mmio_rmw32(AON_CLK_CFG_ADDR, 0xFF0u, 0x4C0u);
}

static void rf_pll_ldo_en(uint32_t trim)
{
    openfw_mmio_clr32(RF_ANA_BASE + 0x40u, 0x4u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x40u, 0xF0000u, (trim & 0xFu) << 16);
    openfw_mmio_set32(RF_ANA_BASE + 0x40u, 0x20u);
    rf_delay(1u);
}

static void rf_xtal_en(void)
{
    openfw_mmio_set32(RF_ANA_BASE + 0x2Cu, 0x5u);
    rf_delay(1u);
}

static void rf_synth_en(void)
{
    openfw_mmio_set32(RF_ANA_BASE + 0xB0u, (1u << 24));
    openfw_mmio_set32(RF_ANA_BASE + 0xB8u, (1u << 28));
    for (int b = 15; b >= 1; b--) {
        openfw_mmio_set32(RF_ANA_BASE + 0xB4u, (1u << (uint32_t)b));
    }
    rf_delay(10u);
}

static void rf_vco_cal_step(int16_t n_int, uint32_t n_frac)
{
    /* Fields 24 (bits [29:21]) and 25 (bits [20:0]) share LUT entry 0x9E */
    rf_lut_write_raw(0x9Eu, (((uint32_t)n_int & 0x1FFu) << 21) | (n_frac & 0x1FFFFFu));
    openfw_mmio_set32(RF_ANA_BASE + 0xC0u, (1u << 9));
    openfw_mmio_clr32(RF_ANA_BASE + 0xC0u, 0x7Fu);
    openfw_mmio_set32(RF_ANA_BASE + 0xC0u, (1u << 8));
    rf_poll_clear32(RF_ANA_BASE + 0xC0u, (1u << 8));
    openfw_mmio_set32(RF_ANA_BASE + 0xB4u, 0x1u);
    rf_delay(1u);
    openfw_mmio_clr32(RF_ANA_BASE + 0xB4u, 0x1u);
    rf_delay(1u);
    openfw_mmio_wr32(RF_ANA_BASE + 0xBCu, 0x1E1D1303u);
    rf_delay(50u);
}

static void bt_rf_vco_cal(uint32_t trim, uint8_t tx_band[86], uint8_t tx_cap[86],
                          uint8_t rx_band[86], uint8_t rx_cap[86])
{
    rf_pll_ldo_en(trim);
    rf_xtal_en();
    rf_synth_en();

    for (uint32_t ch = 0u; ch < 86u; ch++) {
        int16_t  ni = (int16_t)(200u + (ch / 12u));
        uint32_t nf = ((ch % 12u) << 21) / 12u;
        rf_vco_cal_step(ni, nf);
        tx_cap[ch]  = (uint8_t)(openfw_mmio_rd32(RF_ANA_BASE + 0xC4u) & 0x7Fu);
        tx_band[ch] = (uint8_t)(openfw_mmio_rd32(RF_ANA_BASE + 0xCCu) >> 12);
    }
    for (uint32_t ch = 0u; ch < 86u; ch++) {
        int16_t  ni = (int16_t)(200u + (ch / 12u) + 7u);
        uint32_t nf = ((ch % 12u) << 21) / 12u;
        rf_vco_cal_step(ni, nf);
        rx_cap[ch]  = (uint8_t)(openfw_mmio_rd32(RF_ANA_BASE + 0xC4u) & 0x7Fu);
        rx_band[ch] = (uint8_t)(openfw_mmio_rd32(RF_ANA_BASE + 0xCCu) >> 12);
    }
}

static void rf_synth_tune_mhz(uint32_t freq_mhz)
{
    openfw_mmio_set32(RF_ANA_BASE + 0xC0u, (1u << 9));
    openfw_mmio_rmw32(RF_ANA_BASE + 0xC0u, 0x7Fu, (freq_mhz - 0x960u) & 0x7Fu);
    openfw_mmio_set32(RF_ANA_BASE + 0xC0u, (1u << 8));
    rf_poll_clear32(RF_ANA_BASE + 0xC0u, (1u << 8));
    openfw_mmio_set32(RF_ANA_BASE + 0xB4u, 0x1u);
    rf_delay(1u);
    openfw_mmio_clr32(RF_ANA_BASE + 0xB4u, 0x1u);
    rf_delay(50u);
}

static void rf_tx_tone_start(uint32_t step, uint32_t em_off)
{
    openfw_mmio_clr32(RF_ANA_BASE + 0x0Cu, (1u << 31));
    openfw_mmio_set32(RF_ANA_BASE + 0xA0u, (1u << 20));
    uint32_t phase = 0u;
    for (uint32_t i = 0u; i < 1024u; i++) {
        openfw_mmio_wr16(RWBT_EM_BASE + em_off + 4u * i,      (uint16_t)trig_cos1024(phase));
        openfw_mmio_wr16(RWBT_EM_BASE + em_off + 4u * i + 2u, (uint16_t)trig_sin1024(phase));
        phase += (step + 1u);
    }
    openfw_mmio_set32(RF_ANA_BASE + 0x00u, (1u << 14));
    rf_lut_write_field(6u, 0u, (uint32_t)(-32));
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_lut_write_field(3u, 0u, 1u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x0Cu, 0x7FFF0000u, (em_off << 16) & 0x7FFF0000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x0Cu, 0xFFFFu, 0x0FFCu);
    openfw_mmio_set32(RF_ANA_BASE + 0x0Cu, (1u << 31));
    openfw_mmio_clr32(RF_ANA_BASE + 0xA0u, (1u << 20));
    rf_delay(101u);
}

static void rf_tx_cal_cfg(void)
{
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA0u, 0x70000000u, 0x30000000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA4u, 0xFF000370u, 0xED000340u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA8u, 0xFF3u, (3u << 4) | (1u << 6));
    openfw_mmio_rmw32(RF_BB_BASE + 0x120u, 0xE1FFFu, (3u << 17));
    rf_lut_write_field(2u, 0u, 1u);
    rf_lut_write_field(1u, 0u, 1u);
    rf_lut_write_field(5u, 0u, 1u);
    rf_lut_write_field(4u, 0u, 1u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_lut_write_field(9u, 0u, 1u);
    rf_lut_write_field(8u, 0u, 1u);
    rf_lut_write_field(11u, 0u, 1u);
    rf_lut_write_field(10u, 0u, 1u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_lut_write_field(6u, 0u, 0x20u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_lut_write_field(0u, 0u, 0x73u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_lut_write_field(3u, 0u, 7u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, 0x100u);
    rf_poll_clear32(RF_ANA_BASE + 0xACu, 0x100u);
}

static uint32_t dft_bin_mag_sq(uint32_t bin, const int16_t x[128])
{
    int32_t  re = 0, im = 0;
    uint32_t ph = 0u;
    for (uint32_t n = 0u; n < 128u; n++) {
        uint32_t idx = ph << 3;
        re += (int32_t)x[n] * (int32_t)trig_cos1024(idx);
        im -= (int32_t)x[n] * (int32_t)trig_sin1024(idx);
        ph = (ph + bin) & 127u;
    }
    int32_t r = re >> 14;
    int32_t i = im >> 14;
    return (uint32_t)(r * r) + (uint32_t)(i * i);
}

static uint32_t measure_rx_tone_log2_q8(uint32_t bin)
{
    int16_t ibuf[128], qbuf[128];
    openfw_mmio_clr32(RF_ANA_BASE + 0x10u, 0x7FFF0000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x10u, 0x7FFFu, 0x1FCu);
    openfw_mmio_set32(RF_ANA_BASE + 0x10u, 0x80008000u);
    rf_poll_clear32(RF_ANA_BASE + 0x10u, (1u << 31));
    rf_delay(1u);

    for (uint32_t n = 0u; n < 128u; n++) {
        int16_t iv = (int16_t)(openfw_mmio_rd16(RWBT_EM_BASE + 4u * n) & 0x0FFFu);
        int16_t qv = (int16_t)(openfw_mmio_rd16(RWBT_EM_BASE + 4u * n + 2u) & 0x0FFFu);
        if (iv & 0x800) {
            iv |= (int16_t)0xF000;
        }
        if (qv & 0x800) {
            qv |= (int16_t)0xF000;
        }
        ibuf[n] = iv;
        qbuf[n] = qv;
    }

    uint64_t pi = (uint64_t)dft_bin_mag_sq(bin, ibuf);
    uint64_t pq = (uint64_t)dft_bin_mag_sq(bin, qbuf);
    if (pi == 0u) {
        pi = 1u;
    }
    if (pq == 0u) {
        pq = 1u;
    }
    return int_log2_q8(pi * pq);
}

static uint32_t bt_rf_cal_10a4(uint32_t trim)
{
    rf_tx_tone_start(0x0Fu, 0x3000u);
    rf_pll_ldo_en(trim);
    rf_xtal_en();
    rf_synth_en();
    rf_synth_tune_mhz(0x98Au);

    openfw_mmio_rmw32(RF_ANA_BASE + 0xA0u,
                      (1u << 22) | (1u << 21) | (1u << 19) | (1u << 17) | (1u << 16),
                      (1u << 31) | (1u << 20));
    openfw_mmio_clr32(RF_ANA_BASE + 0x64u, 0x80u);
    rf_delay(1u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA0u, (1u << 18) | (1u << 24), (1u << 21));
    rf_delay(1u);
    openfw_mmio_set32(RF_ANA_BASE + 0xA0u, (1u << 22));
    if (rf_lut_read_field(0u, 0u) & 0xF0u) {
        openfw_mmio_set32(RF_ANA_BASE + 0x64u, 0x80u);
    }
    rf_delay(1u);
    openfw_mmio_clr32(RF_ANA_BASE + 0xA0u, (1u << 20));
    rf_delay(1u);
    openfw_mmio_set32(RF_ANA_BASE + 0xA0u, (1u << 19) | (1u << 17) | (1u << 16));
    rf_tx_cal_cfg();

    rf_pll_ldo_en(trim);
    rf_xtal_en();
    rf_synth_en();
    rf_synth_tune_mhz(0x98Au);

    openfw_mmio_set32(RF_ANA_BASE + 0x64u, (1u << 19) | (1u << 16) | 0x60u);
    rf_delay(1u);
    openfw_mmio_set32(RF_ANA_BASE + 0x64u, (1u << 19) | 0x1Fu);
    rf_delay(1u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x84u, 0x192u, (1u << 18) | 0x2Cu);
    rf_delay(4u);
    openfw_mmio_set32(RF_ANA_BASE + 0x84u, 0x180u);
    rf_delay(1u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x84u, 0xCu, 0x10u);
    rf_delay(1u);
    rf_synth_tune_mhz(0x9B4u);

    openfw_mmio_rmw32(RF_ANA_BASE + 0x80u, 0x7u, 4u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x60u, 0x7C007C00u, 0x30003000u);
    rf_lut_write_field(14u, 0u, 0u);
    rf_lut_write_field(15u, 0u, 0u);
    openfw_mmio_set32(RF_ANA_BASE + 0x80u, 0x10000u);
    rf_poll_clear32(RF_ANA_BASE + 0x80u, 0x10000u);
    rf_lut_write_field(23u, 0u, 0u);
    rf_lut_write_field(22u, 0u, 0u);
    openfw_mmio_set32(RF_ANA_BASE + 0x80u, 0x10000u);
    rf_poll_clear32(RF_ANA_BASE + 0x80u, 0x10000u);
    rf_lut_write_field(12u, 0u, 0u);
    rf_lut_write_field(13u, 0u, 6u);
    rf_lut_write_field(16u, 0u, 0u);
    openfw_mmio_set32(RF_ANA_BASE + 0x80u, 0x10000u);
    rf_poll_clear32(RF_ANA_BASE + 0x80u, 0x10000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x88u, 0xF0003F00u, 0x60002D00u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x84u, 0xF0000000u, 0x60000000u);
    rf_delay(1u);

    uint32_t l2_low = measure_rx_tone_log2_q8(2u);
    rf_tx_tone_start(0x5Fu, 0x3000u);
    rf_tx_cal_cfg();

    /* 3.0 dB roll-off => delta log2(pi * pq) = 0.6 * log2(10) = 1.9932 => Q8 = 510 */
    uint32_t best_k   = 0x0Du;
    uint32_t best_err = 0xFFFFFFFFu;
    for (uint32_t k = 0u; k < 32u; k++) {
        openfw_mmio_rmw32(RF_ANA_BASE + 0xA4u, 0x1F000000u, k << 24);
        uint32_t l2_hi = measure_rx_tone_log2_q8(12u);
        int32_t  drop  = (int32_t)l2_low - (int32_t)l2_hi;
        int32_t  diff  = drop - 510;
        uint32_t err   = (diff < 0) ? (uint32_t)(-diff) : (uint32_t)diff;
        if (err < best_err) {
            best_err = err;
            best_k   = k;
        }
    }
    return best_k;
}

static void bt_rf_init_internal(uint32_t freq_drift, uint32_t vco_cal_10a4,
                                const uint8_t tx_band[86], const uint8_t tx_cap[86],
                                const uint8_t rx_band[86], const uint8_t rx_cap[86])
{
    uint32_t trim = bt_rf_efuse_trim();
    rf_cru_reset();

    openfw_mmio_set32(RF_BB_CTRL_ADDR, (1u << 26));
    openfw_mmio_clr32(RF_ANA_BASE + 0x20u, (1u << 0));
    openfw_mmio_set32(RF_ANA_BASE + 0x84u, (1u << 19));
    openfw_mmio_rmw32(RF_ANA_BASE + 0x40u, 0xF0000u | (1u << 5), (1u << 5) | ((trim & 0xFu) << 16));
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, (1u << 16));
    openfw_mmio_rmw32(RF_LUT_CTRL_ADDR, 0xFFF00000u, 0x04000000u);

    rf_lut_populate(tx_band, tx_cap, rx_band, rx_cap);

    openfw_mmio_rmw32(RF_ANA_BASE + 0xA0u, 0x71040000u, 0x30000000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA4u, 0xFF03F370u,
                      0xE0000B40u | ((vco_cal_10a4 & 0x1Fu) << 24));
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA8u, 0x00000FF0u,
                      0x00000450u | ((openfw_mmio_rd32(RF_BB_BASE + 0x120u) & 0x1FFFu) ? 0x100u : 0u));

    openfw_mmio_rmw32(RF_ANA_BASE + 0x60u, 0x7F037C07u, 0x35023407u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x80u, 0x00000007u, 0x00000004u);

    openfw_mmio_rmw32(RF_ANA_BASE + 0x88u, 0xF0003F00u, 0x60002D00u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x84u, 0xF0000000u, 0x60000000u);
    openfw_mmio_clr32(RF_ANA_BASE + 0x20u, (1u << 21));
    openfw_mmio_set32(RF_ANA_BASE + 0x20u, (1u << 21));
    openfw_mmio_clr32(RF_ANA_BASE + 0x24u, 0xF0000000u);

    openfw_mmio_rmw32(RF_ANA_BASE + 0xC0u, 0x7Fu, 0x00000229u);
    openfw_mmio_set32(RF_ANA_BASE + 0xC0u, (1u << 8));
    rf_poll_clear32(RF_ANA_BASE + 0xC0u, (1u << 8));
    openfw_mmio_clr32(RF_ANA_BASE + 0xC0u, (1u << 9));
    openfw_mmio_rmw32(RF_ANA_BASE + 0xE4u, 0x3FE00000u, 0x00E00000u);
    openfw_mmio_rmw32(AON_CLK_CFG_ADDR, 0x00000FF0u,
                      (((freq_drift > 0xFFu) ? 0x4Cu : freq_drift) << 4));
    openfw_mmio_clr32(RF_ANA_BASE + 0xB8u, 0x60000000u);
    openfw_mmio_wr32(RF_ANA_BASE + 0xBCu, 0x1E1D0303u);

    openfw_mmio_rmw32(RF_ANA_BASE + 0x80u, 0x7F00u, 0x00021A00u);
    openfw_mmio_set32(RF_ANA_BASE + 0x80u, (1u << 16));
    rf_poll_clear32(RF_ANA_BASE + 0x80u, (1u << 16));
    openfw_mmio_clr32(RF_ANA_BASE + 0x80u, (1u << 17));
    openfw_mmio_rmw32(RF_ANA_BASE + 0xACu, 0x1Fu, 0x00000212u);
    openfw_mmio_set32(RF_ANA_BASE + 0xACu, (1u << 8));
    rf_poll_clear32(RF_ANA_BASE + 0xACu, (1u << 8));
    openfw_mmio_clr32(RF_ANA_BASE + 0xACu, (1u << 9));

    openfw_mmio_clr32(RF_ANA_BASE + 0x40u, 0x00007F00u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0x80u, 0x7FF00000u, 0x36000000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xA0u, 0x0000FF07u, 0x00006000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xACu, 0xFFFC0000u, 0x0A140000u);
    openfw_mmio_rmw32(RF_ANA_BASE + 0xB0u, 0x7E7F7F7Fu, 0x3A033100u);

    openfw_mmio_clr32(RF_BB_CTRL_ADDR, (1u << 26));
    rf_delay(100u);
}

void bt_rf_init(uint32_t freq_drift, uint32_t vco_cal_10a4)
{
    bt_rf_init_internal(freq_drift, vco_cal_10a4, 0, 0, 0, 0);
}

void bt_modem_init(uint32_t clk6_hz)
{
    uint32_t div = (clk6_hz <= 16000000u) ? 1u : 2u;
    uint32_t mhz = (clk6_hz / 1000000u) / div;
    openfw_mmio_rmw32(RF_BB_CTRL_ADDR, 0x073F0000u, ((div - 1u) << 24) | ((mhz & 0x3Fu) << 16));

    openfw_mmio_wr32(RF_BB_BASE + 0x298u, 0x2D2B2927u);
    openfw_mmio_wr32(RF_BB_BASE + 0x29Cu, 0x3533312Fu);
    openfw_mmio_wr32(RF_BB_BASE + 0x2A0u, 0x3D3B3937u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2A4u, 0x4543413Fu);
    openfw_mmio_wr32(RF_BB_BASE + 0x2A8u, 0x4D4B4947u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2ACu, 0x5553514Fu);
    openfw_mmio_wr32(RF_BB_BASE + 0x2B0u, 0x00005957u);

    openfw_mmio_wr32(RF_BB_BASE + 0x01Cu, 0x00000001u);
    openfw_mmio_wr32(RF_BB_BASE + 0x100u, 0x00000080u);
    openfw_mmio_wr32(RF_BB_BASE + 0x120u, 0x800703E8u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2F0u, 0x0022C3E8u);
    openfw_mmio_wr32(RF_BB_BASE + 0x270u, 0x9A805E1Bu);
    openfw_mmio_wr32(RF_BB_BASE + 0x274u, 0x00F00341u);
    openfw_mmio_wr32(RF_BB_BASE + 0x280u, 0x0EEDE000u);
    openfw_mmio_wr32(RF_BB_BASE + 0x20Cu, 0x18601111u);
    openfw_mmio_rmw32(RF_BB_BASE + 0x290u, 0x003F3F00u, 0x00393F00u);
    openfw_mmio_wr32(RF_BB_BASE + 0x21Cu, 0x000CB88Au);
    openfw_mmio_wr32(RF_BB_BASE + 0x2C0u, 0x600C6020u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2C4u, 0x01303779u);
    openfw_mmio_wr32(RF_BB_BASE + 0x108u, 0x14360422u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2C8u, 0x031CC1F4u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2B8u, 0x001350C0u);
    openfw_mmio_wr32(RF_BB_BASE + 0x2F8u, 0x00013765u);
    openfw_mmio_rmw32(RF_BB_BASE + 0x228u, 0x00003F00u, 0x00001C00u);
    openfw_mmio_rmw32(RF_BB_BASE + 0x2F4u, 0x00003F00u, 0x00002200u);
    openfw_mmio_rmw32(RF_BB_BASE + 0x2FCu, 0x0003F000u, 0x0002C000u);
    openfw_mmio_rmw32(RF_BB_BASE + 0x200u, 0x00008000u, 0x00002000u);
    openfw_mmio_wr32(RF_BB_BASE + 0x30Cu, 0x00353B73u);
}

uint32_t bt_rf_init_calibrated(uint32_t freq_drift, uint32_t clk6_hz)
{
    uint8_t tx_band[86], tx_cap[86], rx_band[86], rx_cap[86];

    bt_rf_cal_mode_reset();
    bt_rf_vco_cal(5u, tx_band, tx_cap, rx_band, rx_cap);

    bt_rf_cal_mode_reset();
    rf_lut_populate(tx_band, tx_cap, rx_band, rx_cap);
    uint32_t cal_10a4 = bt_rf_cal_10a4(5u);

    bt_rf_init_internal(freq_drift, cal_10a4, tx_band, tx_cap, rx_band, rx_cap);
    bt_modem_init(clk6_hz);
    return cal_10a4;
}
