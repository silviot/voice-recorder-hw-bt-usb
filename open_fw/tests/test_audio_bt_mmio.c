/*
 * MMIO-Hooked Host Unit Tests for Audio DMA, WAV Recorder & BT RF/Modem Drivers
 *
 * Compiles the real ARM driver sources (audio_dma.c, recorder.c, bt_rf.c,
 * wav.c, exfat.c, rtc.c, wdt_timer.c) natively under -DOPENFW_HOST_MMIO and
 * verifies them against deterministic virtual SL6801 Codec, DMAC CH1,
 * eFuse, RF LUT/ANA/BB, RWBT Exchange Memory, and exFAT disk models.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "audio_dsp.h"
#include "openfw_fs.h"
#include "openfw_wav.h"

extern void DMAC_CH1_IRQHandler(void);

volatile uint32_t g_systick_ms = 0u;
static void (*g_dmac_ch0_cb)(void) = NULL;

void openfw_exp_set_dmac_ch0_irq(void (*cb)(void))
{
    if (!cb) {
        nvic_disable_irq(DMAC_IRQ_BASE);
        openfw_mmio_wr32(DMAC_CH_IRQ_EN_ADDR(0u), 0u);
        openfw_mmio_clr32(DMAC_CH_CTL_ADDR(0u), DMAC_CTL_CH_EN);
        openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(0u),
                         openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(0u)));
        nvic_clear_pending(DMAC_IRQ_BASE);
        g_dmac_ch0_cb = NULL;
    } else {
        g_dmac_ch0_cb = cb;
    }
}

/* ============================================================================
 * Virtual SL6801 SoC State & MMIO / DMAC / ROM Hooks
 * ============================================================================ */
static struct {
    /* Core / Timer / NVIC / CRU / SYSCTL / GPIO / AON RTC */
    uint64_t us_time;
    uint32_t dsb_count;
    uint32_t nvic_iser[4];
    uint32_t nvic_icpr[4];

    uint32_t cru_rst_set1;
    uint32_t cru_rst_stat1;
    uint32_t cru_clk_en_audio;
    uint32_t cru_clk_src_gate;
    uint32_t cru_apll_ctrl;
    uint32_t cru_apll_frac;
    uint32_t cru_bt_rf_wrap;
    uint32_t sysctl_codec_sram;
    uint32_t gpio_mux[4][4];

    uint32_t aon_clk_cfg;
    uint32_t aon_clk_en;
    uint32_t aon_pwr_ctl0;
    uint32_t aon_rtc_ctrl;
    uint32_t rtc_date;
    uint32_t rtc_time;

    /* Audio Codec + Codec SRAM + DMAC */
    uint32_t codec_regs[256]; /* 0x40009000..0x400093FC */
    uint32_t codec_sram[128]; /* 0x40009400..0x400095FC */
    uint32_t dmac_regs[8][16];
    void    *dmac_buf[8];
    uint32_t dmac_len[8];
    uint32_t dmac_reg_calls[8];

    /* eFuse + ROM CRU Hooks */
    uint32_t efuse_words[16];
    uint32_t rom_rst_last_id;
    uint32_t rom_rst_calls;
    uint32_t rom_clk_last_id;
    uint32_t rom_clk_calls;

    /* BT RF BB / ANA / LUT + RWBT Exchange Memory */
    uint32_t rf_bb[256];      /* 0x40010000..0x400103FC (index 0 = RF_BB_CTRL) */
    uint32_t rf_ana[64];      /* 0x40011000..0x400110FC */
    uint32_t rf_lut_ctrl;
    uint32_t rf_lut_data;
    uint32_t rf_lut[512];
    uint32_t rf_lut_writes[512];
    uint16_t rwbt_em[0x4000]; /* 0x40030000..0x40037FFE (32 KiB) */
    uint32_t rf_vco_cal_steps;
    uint32_t rf_rx_captures;
    uint32_t bpf_sweep_mask;

    /* exFAT Image Backing Memory */
    uint8_t *sd_buf;
    uint32_t sd_sectors;
} g_soc;

static void soc_reset(void)
{
    uint8_t *saved_buf = g_soc.sd_buf;
    uint32_t saved_sec = g_soc.sd_sectors;
    memset(&g_soc, 0, sizeof(g_soc));
    g_soc.sd_buf       = saved_buf;
    g_soc.sd_sectors   = saved_sec;
    g_soc.aon_pwr_ctl0 = (1u << 28);
    g_soc.aon_rtc_ctrl = (1u << 4) | (1u << 8);
    for (uint32_t i = 0u; i < 128u; i++) {
        g_soc.codec_sram[i] = 0xDEADBEEFu;
    }
}

void openfw_host_dmac_register_ptr(uint32_t ch, void *buf, uint32_t len)
{
    assert(ch < 8u);
    g_soc.dmac_buf[ch] = buf;
    g_soc.dmac_len[ch] = len;
    g_soc.dmac_reg_calls[ch]++;
}

static void rf_simulate_rx_capture(void)
{
    uint32_t em_off = (g_soc.rf_ana[0x0Cu >> 2] >> 16) & 0x7FFFu;
    assert(em_off + 256u * sizeof(uint16_t) <= sizeof(g_soc.rwbt_em));

    /*
     * TX tone step=0x0F (low tone) writes cos(2*pi*16/1024)=1019 at em[2],
     * while step=0x5F (high tone) writes cos(2*pi*96/1024)=851 at em[2].
     * Keep low tone unattenuated and apply programmable BPF roll-off from
     * RF_ANA+0xA4[28:24] so k=18 yields -3.0 dB (724/1024).
     */
    int16_t tx_c1 = (int16_t)g_soc.rwbt_em[(em_off >> 1) + 2u];
    int32_t scale = 1024;
    if (tx_c1 <= 950) {
        uint32_t k = (g_soc.rf_ana[0xA4u >> 2] >> 24) & 0x1Fu;
        scale = 364 + (int32_t)k * 20;
        g_soc.bpf_sweep_mask |= (1u << k);
    }

    for (uint32_t n = 0u; n < 128u; n++) {
        int32_t ti = (int16_t)g_soc.rwbt_em[(em_off >> 1) + 2u * n];
        int32_t tq = (int16_t)g_soc.rwbt_em[(em_off >> 1) + 2u * n + 1u];
        int32_t ri = (ti * scale) >> 10;
        int32_t rq = (tq * scale) >> 10;
        g_soc.rwbt_em[2u * n]      = (uint16_t)(ri & 0x0FFFu);
        g_soc.rwbt_em[2u * n + 1u] = (uint16_t)(rq & 0x0FFFu);
    }
    g_soc.rf_rx_captures++;
}

uint32_t openfw_mmio_rd32(uint32_t addr)
{
    if (addr == US_TIMER_HI_ADDR) {
        return (uint32_t)(g_soc.us_time >> 32);
    }
    if (addr == US_TIMER_LO_ADDR) {
        return (uint32_t)(g_soc.us_time & 0xFFFFFFFFu);
    }

    if (addr >= NVIC_ISER_ADDR(0u) && addr <= NVIC_ISER_ADDR(3u)) {
        return g_soc.nvic_iser[(addr - NVIC_ISER_ADDR(0u)) >> 2];
    }

    if (addr == AON_CLK_CFG_ADDR)  return g_soc.aon_clk_cfg;
    if (addr == AON_CLK_STAT_ADDR) return 0u;
    if (addr == AON_CLK_EN_ADDR)   return g_soc.aon_clk_en;
    if (addr == AON_PWR_CTL0_ADDR) return g_soc.aon_pwr_ctl0;
    if (addr == AON_RTC_CTRL_ADDR) return g_soc.aon_rtc_ctrl;
    if (addr == RTC_DATE_ADDR)     return g_soc.rtc_date;
    if (addr == RTC_TIME_ADDR)     return g_soc.rtc_time;

    if (addr == CRU_RST_SET1_ADDR)     return g_soc.cru_rst_set1;
    if (addr == CRU_RST_STAT1_ADDR)    return g_soc.cru_rst_stat1;
    if (addr == CRU_CLK_EN_AUDIO_ADDR) return g_soc.cru_clk_en_audio;
    if (addr == CRU_CLK_SRC_GATE_ADDR) return g_soc.cru_clk_src_gate;
    if (addr == CRU_APLL_CTRL_ADDR)    return g_soc.cru_apll_ctrl;
    if (addr == CRU_APLL_FRAC_ADDR)    return g_soc.cru_apll_frac;
    if (addr == CRU_BT_RF_WRAP_ADDR)   return g_soc.cru_bt_rf_wrap;
    if (addr == SYSCTL_CODEC_SRAM_ADDR) return g_soc.sysctl_codec_sram;

    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u) {
        uint32_t port = (addr - GPIO_BASE) >> 6;
        uint32_t off  = (addr - GPIO_BASE) & 0x3Fu;
        if (off < 0x10u) {
            return g_soc.gpio_mux[port][off >> 2];
        }
    }

    if (addr >= AUDIO_CODEC_BASE && addr < CODEC_SRAM_START) {
        return g_soc.codec_regs[(addr - AUDIO_CODEC_BASE) >> 2];
    }
    if (addr >= CODEC_SRAM_START && addr < CODEC_SRAM_END) {
        assert((g_soc.sysctl_codec_sram & (1u << 2)) != 0u);
        return g_soc.codec_sram[(addr - CODEC_SRAM_START) >> 2];
    }

    if (addr >= DMAC_BASE && addr < DMAC_BASE + 0x200u) {
        uint32_t ch  = (addr - DMAC_BASE) >> 6;
        uint32_t off = (addr - DMAC_BASE) & 0x3Fu;
        return g_soc.dmac_regs[ch][off >> 2];
    }

    if (addr >= EFUSE_WORD_ADDR(0u) && addr <= EFUSE_WORD_ADDR(15u)) {
        return g_soc.efuse_words[(addr - EFUSE_WORD_ADDR(0u)) >> 2];
    }

    if (addr >= RF_BB_BASE && addr < RF_BB_BASE + 0x400u) {
        return g_soc.rf_bb[(addr - RF_BB_BASE) >> 2];
    }

    if (addr == RF_LUT_CTRL_ADDR) return g_soc.rf_lut_ctrl;
    if (addr == RF_LUT_DATA_ADDR) return g_soc.rf_lut_data;

    if (addr >= RF_ANA_BASE && addr < RF_ANA_BASE + 0x100u) {
        return g_soc.rf_ana[(addr - RF_ANA_BASE) >> 2];
    }

    return 0u;
}

void openfw_mmio_wr32(uint32_t addr, uint32_t val)
{
    if (addr >= NVIC_ISER_ADDR(0u) && addr <= NVIC_ISER_ADDR(3u)) {
        g_soc.nvic_iser[(addr - NVIC_ISER_ADDR(0u)) >> 2] |= val;
        return;
    }
    if (addr >= NVIC_ICER_ADDR(0u) && addr <= NVIC_ICER_ADDR(3u)) {
        g_soc.nvic_iser[(addr - NVIC_ICER_ADDR(0u)) >> 2] &= ~val;
        return;
    }
    if (addr >= NVIC_ICPR_ADDR(0u) && addr <= NVIC_ICPR_ADDR(3u)) {
        g_soc.nvic_icpr[(addr - NVIC_ICPR_ADDR(0u)) >> 2] = val;
        return;
    }

    if (addr == AON_CLK_CFG_ADDR)  { g_soc.aon_clk_cfg = val & 0x7FFFFFFFu; return; }
    if (addr == AON_CLK_EN_ADDR)   { g_soc.aon_clk_en = val; return; }
    if (addr == AON_PWR_CTL0_ADDR) { g_soc.aon_pwr_ctl0 = val; return; }
    if (addr == AON_RTC_CTRL_ADDR) { g_soc.aon_rtc_ctrl = val; return; }
    if (addr == RTC_DATE_ADDR)     { g_soc.rtc_date = val; return; }
    if (addr == RTC_TIME_ADDR)     { g_soc.rtc_time = val; return; }

    if (addr == CRU_RST_SET1_ADDR)     { g_soc.cru_rst_set1 = val; return; }
    if (addr == CRU_RST_STAT1_ADDR) {
        g_soc.cru_rst_stat1 = val;
        if ((val & (1u << 2)) == 0u) {
            memset(g_soc.rf_ana, 0, sizeof(g_soc.rf_ana));
            memset(g_soc.rf_bb, 0, sizeof(g_soc.rf_bb));
            g_soc.rf_lut_ctrl = 0u;
            g_soc.rf_lut_data = 0u;
        }
        return;
    }
    if (addr == CRU_CLK_EN_AUDIO_ADDR) { g_soc.cru_clk_en_audio = val; return; }
    if (addr == CRU_CLK_SRC_GATE_ADDR) { g_soc.cru_clk_src_gate = val; return; }
    if (addr == CRU_APLL_FRAC_ADDR)    { g_soc.cru_apll_frac = val; return; }
    if (addr == CRU_BT_RF_WRAP_ADDR)   { g_soc.cru_bt_rf_wrap = val; return; }
    if (addr == CRU_APLL_CTRL_ADDR) {
        g_soc.cru_apll_ctrl = val;
        if ((val & 0xC0000000u) == 0xC0000000u) {
            g_soc.cru_apll_ctrl |= (1u << 28); /* Audio PLL locks */
        }
        return;
    }
    if (addr == SYSCTL_CODEC_SRAM_ADDR) { g_soc.sysctl_codec_sram = val; return; }

    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u) {
        uint32_t port = (addr - GPIO_BASE) >> 6;
        uint32_t off  = (addr - GPIO_BASE) & 0x3Fu;
        if (off < 0x10u) {
            g_soc.gpio_mux[port][off >> 2] = val;
        }
        return;
    }

    if (addr >= AUDIO_CODEC_BASE && addr < CODEC_SRAM_START) {
        g_soc.codec_regs[(addr - AUDIO_CODEC_BASE) >> 2] = val;
        return;
    }
    if (addr >= CODEC_SRAM_START && addr < CODEC_SRAM_END) {
        assert((g_soc.sysctl_codec_sram & (1u << 2)) != 0u);
        g_soc.codec_sram[(addr - CODEC_SRAM_START) >> 2] = val;
        return;
    }

    if (addr >= DMAC_BASE && addr < DMAC_BASE + 0x200u) {
        uint32_t ch  = (addr - DMAC_BASE) >> 6;
        uint32_t off = (addr - DMAC_BASE) & 0x3Fu;
        if (off == 0x24u) {
            g_soc.dmac_regs[ch][off >> 2] &= ~val; /* W1C IRQ status */
        } else {
            g_soc.dmac_regs[ch][off >> 2] = val;
        }
        return;
    }

    if (addr >= RF_BB_BASE && addr < RF_BB_BASE + 0x400u) {
        g_soc.rf_bb[(addr - RF_BB_BASE) >> 2] = val;
        return;
    }

    if (addr == RF_LUT_DATA_ADDR) {
        g_soc.rf_lut_data = val;
        return;
    }
    if (addr == RF_LUT_CTRL_ADDR) {
        if ((val & (1u << 16)) != 0u) {
            uint32_t lut_addr = val & 0x1FFu;
            if ((val & (1u << 11)) != 0u) {
                g_soc.rf_lut[lut_addr] = g_soc.rf_lut_data;
                g_soc.rf_lut_writes[lut_addr]++;
            } else {
                g_soc.rf_lut_data = g_soc.rf_lut[lut_addr];
            }
            g_soc.rf_lut_ctrl = val & ~(1u << 16);
        } else {
            g_soc.rf_lut_ctrl = val;
        }
        return;
    }

    if (addr >= RF_ANA_BASE && addr < RF_ANA_BASE + 0x100u) {
        uint32_t off = addr - RF_ANA_BASE;
        if (off == 0x10u && (val & (1u << 31)) != 0u) {
            rf_simulate_rx_capture();
            g_soc.rf_ana[off >> 2] = val & ~(1u << 31);
            return;
        }
        if (off == 0x80u && (val & (1u << 16)) != 0u) {
            g_soc.rf_ana[off >> 2] = val & ~(1u << 16);
            return;
        }
        if (off == 0xACu && (val & (1u << 8)) != 0u) {
            g_soc.rf_ana[off >> 2] = val & ~(1u << 8);
            return;
        }
        if (off == 0xC0u && (val & (1u << 8)) != 0u) {
            g_soc.rf_ana[off >> 2] = val & ~(1u << 8);
            g_soc.rf_vco_cal_steps++;
            uint32_t ni   = (g_soc.rf_lut[0x9Eu] >> 21) & 0x1FFu;
            uint32_t nf   = g_soc.rf_lut[0x9Eu] & 0x1FFFFFu;
            uint32_t band = (ni >= 200u) ? ((ni - 200u) & 0xFFu) : 1u;
            uint32_t cap  = ((nf >> 16) + ni * 3u) & 0x7Fu;
            g_soc.rf_ana[0xC4u >> 2] = cap;
            g_soc.rf_ana[0xCCu >> 2] = band << 12;
            return;
        }
        g_soc.rf_ana[off >> 2] = val;
    }
}

uint16_t openfw_mmio_rd16(uint32_t addr)
{
    if (addr >= RWBT_EM_BASE && addr < RWBT_EM_BASE + 0x8000u) {
        assert((addr & 1u) == 0u);
        return g_soc.rwbt_em[(addr - RWBT_EM_BASE) >> 1];
    }
    return 0u;
}

void openfw_mmio_wr16(uint32_t addr, uint16_t val)
{
    if (addr >= RWBT_EM_BASE && addr < RWBT_EM_BASE + 0x8000u) {
        assert((addr & 1u) == 0u);
        g_soc.rwbt_em[(addr - RWBT_EM_BASE) >> 1] = val;
    }
}

uint8_t openfw_mmio_rd8(uint32_t addr)               { (void)addr; return 0u; }
void    openfw_mmio_wr8(uint32_t addr, uint8_t val)  { (void)addr; (void)val; }
void    openfw_mmio_nop(void)                        { g_soc.us_time += 20u; }
void    openfw_mmio_dsb(void)                        { g_soc.dsb_count++; }
void    openfw_mmio_wfi(void)                        { }

/* BootROM CRU hooks used by bt_rf.c */
void rom_rst_release(uint32_t rst_id)
{
    g_soc.rom_rst_last_id = rst_id;
    g_soc.rom_rst_calls++;
}

void rom_clk_enable(uint32_t clk_id)
{
    g_soc.rom_clk_last_id = clk_id;
    g_soc.rom_clk_calls++;
}

/* ============================================================================
 * Helpers: 1 kHz Sine Generator & Image-Backed SD Block Device
 * ============================================================================ */
static void fill_sine_1khz_frame(int16_t *dst, uint32_t samples, int32_t dc_offset, int32_t ac_amp)
{
    /* 16 kHz / 1 kHz = 16 samples per cycle, Q14 exact sine table */
    static const int16_t sin16_q14[16] = {
        0, 6270, 11585, 15137, 16384, 15137, 11585, 6270,
        0, -6270, -11585, -15137, -16384, -15137, -11585, -6270,
    };
    for (uint32_t i = 0u; i < samples; i++) {
        int32_t v = dc_offset + ((sin16_q14[i & 15u] * ac_amp) / 16384);
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
    }
}

static void sim_dma_deliver_frame(int32_t dc_offset, int32_t ac_amp, int16_t *copy_out)
{
    assert((g_soc.dmac_regs[1][0] & DMAC_CTL_CH_EN) != 0u);
    assert((g_soc.nvic_iser[1] & (1u << 29)) != 0u);
    assert(g_soc.dmac_buf[1] != NULL);
    assert(g_soc.dmac_len[1] == 4096u);

    int16_t *dst = (int16_t *)g_soc.dmac_buf[1];
    fill_sine_1khz_frame(dst, 2048u, dc_offset, ac_amp);
    if (copy_out) {
        memcpy(copy_out, dst, 4096u);
    }
    g_soc.dmac_regs[1][9] |= DMAC_IRQ_TC;
    DMAC_CH1_IRQHandler();
}

static void sim_dma_deliver_stereo_frame(int32_t dc_l, int32_t amp_l,
                                         int32_t dc_r, int32_t amp_r)
{
    assert((g_soc.dmac_regs[1][0] & DMAC_CTL_CH_EN) != 0u);
    assert((g_soc.nvic_iser[1] & (1u << 29)) != 0u);
    assert(g_soc.dmac_buf[1] != NULL);
    assert(g_soc.dmac_len[1] == 8192u);

    static const int16_t sin16_q14[16] = {
        0, 6270, 11585, 15137, 16384, 15137, 11585, 6270,
        0, -6270, -11585, -15137, -16384, -15137, -11585, -6270,
    };
    int16_t *dst = (int16_t *)g_soc.dmac_buf[1];
    for (uint32_t i = 0u; i < 2048u; i++) {
        int32_t l = dc_l + ((sin16_q14[i & 15u] * amp_l) / 16384);
        int32_t r = dc_r + ((sin16_q14[i & 15u] * amp_r) / 16384);
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        dst[i * 2u + 0u] = (int16_t)l;
        dst[i * 2u + 1u] = (int16_t)r;
    }
    g_soc.dmac_regs[1][9] |= DMAC_IRQ_TC;
    DMAC_CH1_IRQHandler();
}

static int mem_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba > g_soc.sd_sectors || count > (g_soc.sd_sectors - lba)) {
        return -1;
    }
    memcpy(buf, &g_soc.sd_buf[lba * 512u], (size_t)count * 512u);
    return 0;
}

static int mem_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    if (lba > g_soc.sd_sectors || count > (g_soc.sd_sectors - lba)) {
        return -1;
    }
    memcpy(&g_soc.sd_buf[lba * 512u], buf, (size_t)count * 512u);
    return 0;
}

/* ============================================================================
 * Suite 1: Audio ADC AMIC1 + DMAC CH1 Ring Capture Tests
 * ============================================================================ */
static void test_audio_dma(void)
{
    soc_reset();
    assert(audio_capture_start(24u) == 0);

    /* APLL locked (bit 28) & PB12/PB13 analog mux 15 */
    assert((g_soc.cru_apll_ctrl & (1u << 28)) != 0u);
    assert((g_soc.cru_apll_ctrl & 0xC0000000u) == 0xC0000000u);
    assert(((g_soc.gpio_mux[1][1] >> 16) & 0xFFu) == 0xFFu);

    /* Codec SRAM cleared and CPU SRAM access bit restored to 0 */
    assert((g_soc.sysctl_codec_sram & (1u << 2)) == 0u);
    for (uint32_t i = 0u; i < 128u; i++) {
        assert(g_soc.codec_sram[i] == 0u);
    }

    /* AMIC1 PGA gain [28:24] == 24 & DMAC CH1 armed */
    uint32_t pga = g_soc.codec_regs[(CODEC_AMIC01_PGA_ADDR - AUDIO_CODEC_BASE) >> 2];
    assert(((pga >> 24) & 0x1Fu) == 24u);
    assert(g_soc.dmac_buf[1] != NULL && g_soc.dmac_len[1] == 4096u);
    assert(g_soc.dmac_regs[1][1] == CODEC_ADC_RX_FIFO_ADDR);
    assert((g_soc.dmac_regs[1][3] & 0x3FFFFu) == 4096u);
    assert((g_soc.dmac_regs[1][0] & DMAC_CTL_CH_EN) != 0u);
    assert((g_soc.nvic_iser[1] & (1u << 29)) != 0u);

    /* Feed DC-biased (+/-16384 AC around +4000 DC) 1 kHz sine frame */
    void *slot0_ptr = g_soc.dmac_buf[1];
    sim_dma_deliver_frame(4000, 16384, NULL);
    assert(g_soc.dmac_regs[1][9] == 0u);
    assert(g_soc.dsb_count >= 1u);
    assert(g_soc.dmac_buf[1] != slot0_ptr);
    assert(audio_get_frames() == 1u);
    assert(audio_get_overruns() == 0u);
    assert(audio_get_peak_pct() == 50u);

    /* Feed second frame with distinct DC tag +4100 and check FIFO order */
    sim_dma_deliver_frame(4100, 16384, NULL);
    assert(audio_get_frames() == 2u);
    assert(audio_get_peak_pct() == 50u);

    uint32_t nbytes = 0u;
    const int16_t *f0 = audio_pop_frame(&nbytes);
    assert(f0 != NULL && nbytes == 4096u);
    assert(f0[0] == 4000 && f0[4] == (4000 + 16384) && f0[12] == (4000 - 16384));

    const int16_t *f1 = audio_pop_frame(&nbytes);
    assert(f1 != NULL && nbytes == 4096u && f1[0] == 4100);
    assert(audio_pop_frame(&nbytes) == NULL);

    /* Ring overrun test: > 3 unconsumed frames drops oldest (4200), keeping 4300, 4400, 4500 */
    sim_dma_deliver_frame(4200, 16384, NULL);
    sim_dma_deliver_frame(4300, 16384, NULL);
    sim_dma_deliver_frame(4400, 16384, NULL);
    assert(audio_get_overruns() == 0u);
    sim_dma_deliver_frame(4500, 16384, NULL);
    assert(audio_get_overruns() == 1u);

    const int16_t *r0 = audio_pop_frame(&nbytes);
    const int16_t *r1 = audio_pop_frame(&nbytes);
    const int16_t *r2 = audio_pop_frame(&nbytes);
    assert(r0 != NULL && r0[0] == 4300);
    assert(r1 != NULL && r1[0] == 4400);
    assert(r2 != NULL && r2[0] == 4500);
    assert(audio_pop_frame(&nbytes) == NULL);

    /* Stop capture and verify hardware quiesced */
    audio_capture_stop();
    assert((g_soc.nvic_iser[1] & (1u << 29)) == 0u);
    assert((g_soc.dmac_regs[1][0] & DMAC_CTL_CH_EN) == 0u);
    assert(g_soc.dmac_regs[1][8] == 0u);
    assert(audio_get_peak_pct() == 0u);
    assert((g_soc.codec_regs[(CODEC_ADC_DIG_CTRL_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x3u) == 0u);
    assert((g_soc.codec_regs[(CODEC_ADC_FIFO_CTRL_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x80u) == 0u);
    assert((g_soc.codec_regs[(CODEC_ANA_CTRL0_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x03000000u) == 0u);

    g_soc.dmac_regs[1][9] |= DMAC_IRQ_TC;
    DMAC_CH1_IRQHandler();
    assert(audio_get_frames() == 6u);
    printf("[OK] audio_dma MMIO + DMAC CH1 ring + AC VU\n");
}

/* ============================================================================
 * Suite 2: Real-Time WAV Recorder + exFAT Flush Tests
 * ============================================================================ */
static void test_recorder(const char *exfat_img_path)
{
    soc_reset();

    FILE *fp = fopen(exfat_img_path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, 0, SEEK_END) == 0);
    long sz = ftell(fp);
    assert(sz > 0 && (sz % 512L) == 0);
    assert(fseek(fp, 0, SEEK_SET) == 0);

    g_soc.sd_sectors = (uint32_t)(sz / 512L);
    g_soc.sd_buf     = (uint8_t *)malloc((size_t)sz);
    assert(g_soc.sd_buf != NULL);
    assert(fread(g_soc.sd_buf, 1u, (size_t)sz, fp) == (size_t)sz);
    fclose(fp);

    rtc_ensure_running();
    rtc_datetime_t now = { 2026u, 11u, 19u, 0u, 15u, 30u, 0u };
    rtc_set(&now);

    blkdev_t dev = {
        .read_sectors  = mem_read_sectors,
        .write_sectors = mem_write_sectors,
        .total_sectors = g_soc.sd_sectors,
        .ctx           = NULL,
    };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);

    /* Recording #1: BYPASS bit-exact 3 x 4096 B DMA frames -> /record/REC_0001.wav (12332 B) */
    recorder_set_gain(20u);
    recorder_set_dsp_profile(AUDIO_DSP_BYPASS);
    recorder_set_channels(1u);
    recorder_set_vor(0);
    assert(recorder_start(&vol, 1) == 0);
    assert(recorder_is_saving() == 1);
    assert(strcmp(recorder_get_filename(), "REC_0001.wav") == 0);

    static int16_t expected_pcm[3u * 2048u];
    for (uint32_t f = 0u; f < 3u; f++) {
        sim_dma_deliver_frame(100 * (int32_t)(f + 1u), 12000, &expected_pcm[f * 2048u]);
        recorder_poll();
    }
    assert(recorder_stop() == 0);
    assert(recorder_is_saving() == 0);

    exfat_entry_t ent1;
    assert(exfat_find_path(&vol, "/record/REC_0001.wav", &ent1) == 0);
    assert(ent1.size == 44u + 12288u);

    uint8_t wav1[44u + 12288u];
    uint32_t got1 = 0u;
    assert(exfat_read_file(&vol, &ent1, 0u, wav1, sizeof(wav1), &got1) == 0);
    assert(got1 == sizeof(wav1));

    wav_info_t info1;
    assert(wav_parse_header(wav1, &info1) == 0);
    assert(info1.sample_rate == 16000u && info1.channels == 1u && info1.bits_per_sample == 16u);
    assert(info1.block_align == 2u && info1.byte_rate == 32000u && info1.data_bytes == 12288u);
    assert(memcmp(wav1 + 44u, expected_pcm, 12288u) == 0);

    /* Recording #2: AUDIO_DSP_ASR_RAW strips +4000 DC offset in REC_0002.wav */
    recorder_set_dsp_profile(AUDIO_DSP_ASR_RAW);
    assert(recorder_start(&vol, 1) == 0);
    assert(recorder_is_saving() == 1);
    assert(strcmp(recorder_get_filename(), "REC_0002.wav") == 0);
    for (int f = 0; f < 4; f++) {
        sim_dma_deliver_frame(4000, 8000, NULL);
        recorder_poll();
    }
    assert(recorder_stop() == 0);

    exfat_entry_t ent2;
    assert(exfat_find_path(&vol, "/record/REC_0002.wav", &ent2) == 0);
    assert(ent2.size == 44u + 4u * 4096u);
    static uint8_t wav2[44u + 4u * 4096u];
    uint32_t got2 = 0u;
    assert(exfat_read_file(&vol, &ent2, 0u, wav2, sizeof(wav2), &got2) == 0);
    assert(got2 == sizeof(wav2));
    wav_info_t info2;
    assert(wav_parse_header(wav2, &info2) == 0);
    assert(info2.data_bytes == 4u * 4096u);
    const int16_t *pcm2 = (const int16_t *)(wav2 + 44u);
    int64_t dc_sum = 0;
    for (uint32_t i = 6144u; i < 8192u; i++) {
        dc_sum += pcm2[i];
    }
    assert(llabs(dc_sum / 2048) < 25);

    /* Recording #3: Dual-Mic Stereo AMIC1+AMIC2 (PB14/PB15, 2ch, 8192B DMA) -> REC_0003.wav */
    recorder_set_gain(22u);
    recorder_set_dsp_profile(AUDIO_DSP_ASR_RAW);
    recorder_set_channels(2u);
    assert(recorder_start(&vol, 1) == 0);
    assert(audio_get_channels() == 2u);
    /* Verify PB14/PB15 analog mux=15, CODEC_AMIC2_CTRL enable+gain (22*3=66), CH1+CH2 mux=0x000D0D00 */
    assert(((g_soc.gpio_mux[1][1] >> 24) & 0xFFu) == 0xFFu);
    uint32_t amic2 = g_soc.codec_regs[(CODEC_AMIC2_CTRL_ADDR - AUDIO_CODEC_BASE) >> 2];
    assert((amic2 & 0x00010043u) == 0x00010043u);
    assert(((amic2 >> 8) & 0x7Fu) == 66u);
    uint32_t ch_mux = g_soc.codec_regs[(CODEC_ADC_CH_MUX_ADDR - AUDIO_CODEC_BASE) >> 2];
    assert((ch_mux & 0x000D0D0Du) == 0x000D0D00u);

    sim_dma_deliver_stereo_frame(2000, 12000, -1500, 6000);
    recorder_poll();
    assert(recorder_stop() == 0);

    exfat_entry_t ent3;
    assert(exfat_find_path(&vol, "/record/REC_0003.wav", &ent3) == 0);
    assert(ent3.size == 44u + 8192u);
    uint8_t hdr3[44];
    uint32_t got3 = 0u;
    assert(exfat_read_file(&vol, &ent3, 0u, hdr3, sizeof(hdr3), &got3) == 0);
    assert(got3 == 44u);
    wav_info_t info3;
    assert(wav_parse_header(hdr3, &info3) == 0);
    assert(info3.sample_rate == 16000u && info3.channels == 2u && info3.block_align == 4u);
    assert(info3.byte_rate == 64000u && info3.data_bytes == 8192u);

    /* Restore default recorder config */
    recorder_set_gain(20u);
    recorder_set_dsp_profile(AUDIO_DSP_ASR_RAW);
    recorder_set_channels(1u);

    /* Flush updated sectors back to argv[1] so fsck.exfat -n validates the image */
    fp = fopen(exfat_img_path, "wb");
    assert(fp != NULL);
    assert(fwrite(g_soc.sd_buf, 1u, (size_t)sz, fp) == (size_t)sz);
    assert(fflush(fp) == 0);
    fclose(fp);

    free(g_soc.sd_buf);
    g_soc.sd_buf     = NULL;
    g_soc.sd_sectors = 0u;
    printf("[OK] recorder -> exFAT (REC_0001.wav=raw, REC_0002.wav=ASR_RAW, REC_0003.wav=2ch stereo)\n");
}

/* ============================================================================
 * Suite 4: Stereo Audio DAC0+DAC1 + DMAC CH0 TX + eFuse DC Trim Tests
 * ============================================================================ */
static void test_audio_dac(void)
{
    soc_reset();
    /* Test eFuse DAC0/DAC1 DC trim extraction: W2[31:29]=5, W3[3:0]=9 => 0x4D; W3[10:4]=0x2B */
    g_soc.efuse_words[2] = (5u << 29);
    g_soc.efuse_words[3] = 0x09u | (0x2Bu << 4);

    assert(audio_dac_start(16000u, 20u) == 0);
    assert(g_dmac_ch0_cb != NULL);
    assert((g_soc.cru_apll_ctrl & (1u << 28)) != 0u);
    assert((g_soc.cru_apll_ctrl & 0x00003FE0u) == 0x3060u); /* 24.576 MHz int=3 */
    assert(((g_soc.cru_apll_frac >> 14) & 0x1FFFFu) == 0x03126u);
    assert(((g_soc.codec_regs[(CODEC_GLB_ADDR - AUDIO_CODEC_BASE) >> 2] >> 8) & 0xFu) == 3u);
    assert(g_soc.codec_regs[(CODEC_DAC0_DC_ADDR - AUDIO_CODEC_BASE) >> 2] == 0x4Du);
    assert(g_soc.codec_regs[(CODEC_DAC1_DC_ADDR - AUDIO_CODEC_BASE) >> 2] == 0x2Bu);
    assert(((g_soc.codec_regs[(CODEC_DAC01_ANA_VOL_ADDR - AUDIO_CODEC_BASE) >> 2] >> 16) & 0x1Fu) == 20u);
    assert(((g_soc.codec_regs[(CODEC_DAC01_ANA_VOL_ADDR - AUDIO_CODEC_BASE) >> 2] >> 24) & 0x1Fu) == 20u);
    assert((g_soc.codec_regs[(CODEC_DAC0_DIG_EN_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x3u) == 0x3u);
    assert((g_soc.codec_regs[(CODEC_DAC1_DIG_EN_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x3u) == 0x3u);
    assert((g_soc.codec_regs[(CODEC_DAC2_DIG_EN_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x3u) == 0u);
    assert((g_soc.nvic_iser[1] & (1u << 28)) != 0u); /* IRQ 60 */

    /* Queue two stereo buffers -> double-buffered DMAC CH0 chain */
    int16_t sbuf[512];
    for (uint32_t i = 0u; i < 256u; i++) {
        sbuf[i * 2u + 0u] = (int16_t)(i * 10);
        sbuf[i * 2u + 1u] = (int16_t)(-(int32_t)i * 10);
    }
    assert(audio_dac_write_stereo(sbuf, 256u) == 256);
    assert(audio_dac_is_busy() == 1);
    assert((g_soc.dmac_regs[0][0] & (DMAC_CTL_CH_EN | 0x00FFFFFFu)) == (DMAC_CTL_DAC_TX_BASE | DMAC_CTL_CH_EN));
    assert(g_soc.dmac_regs[0][2] == CODEC_DAC_TX_FIFO_ADDR);
    assert((g_soc.dmac_regs[0][3] & 0x3FFFFu) == 256u * 4u);
    void *first_buf = g_soc.dmac_buf[0];

    /* Enqueue second buffer while first is active */
    assert(audio_dac_write_stereo(sbuf, 128u) == 128);
    assert(audio_dac_write_stereo(sbuf, 128u) == 0); /* Both slots occupied */

    /* Complete slot 0 -> seamlessly switches DMAC CH0 to slot 1 */
    g_soc.dmac_regs[0][9] |= DMAC_IRQ_TC;
    g_dmac_ch0_cb();
    assert(audio_dac_is_busy() == 1);
    assert(g_soc.dmac_buf[0] != first_buf);
    assert((g_soc.dmac_regs[0][3] & 0x3FFFFu) == 128u * 4u);

    /* Complete slot 1 -> idles cleanly */
    g_soc.dmac_regs[0][9] |= DMAC_IRQ_TC;
    g_dmac_ch0_cb();
    assert(audio_dac_is_busy() == 0);
    assert((g_soc.dmac_regs[0][0] & DMAC_CTL_CH_EN) == 0u);

    /* Volume clamp & mute */
    audio_dac_set_volume(99u, 600u);
    assert(((g_soc.codec_regs[(CODEC_DAC01_ANA_VOL_ADDR - AUDIO_CODEC_BASE) >> 2] >> 16) & 0x1Fu) == 31u);
    assert((g_soc.codec_regs[(CODEC_DAC0_VOL_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x1FFu) == 0x1FFu);
    audio_dac_set_mute(1);
    assert((g_soc.codec_regs[(CODEC_DAC0_MUTE_ADDR - AUDIO_CODEC_BASE) >> 2] & (1u << 16)) != 0u);
    audio_dac_set_mute(0);
    assert((g_soc.codec_regs[(CODEC_DAC0_MUTE_ADDR - AUDIO_CODEC_BASE) >> 2] & (1u << 16)) == 0u);

    /* 44.1 kHz family APLL verification */
    assert(audio_dac_start(44100u, 15u) == 0);
    assert((g_soc.cru_apll_ctrl & 0x00003FE0u) == 0x3040u); /* 22.5792 MHz int=2 */
    assert(((g_soc.cru_apll_frac >> 14) & 0x1FFFFu) == 0x186C2u);
    assert(((g_soc.codec_regs[(CODEC_GLB_ADDR - AUDIO_CODEC_BASE) >> 2] >> 8) & 0xFu) == 8u);

    audio_dac_stop();
    assert(g_dmac_ch0_cb == NULL);
    assert((g_soc.nvic_iser[1] & (1u << 28)) == 0u);
    assert((g_soc.codec_regs[(CODEC_DAC01_ANA_VOL_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x1F1F0000u) == 0u);
    assert((g_soc.codec_regs[(CODEC_DAC0_DIG_EN_ADDR - AUDIO_CODEC_BASE) >> 2] & 0x3u) == 0u);
    printf("[OK] audio_dac MMIO (APLL 16k/44.1k, eFuse DC, DMAC CH0 double-buf, pop-free stop)\n");
}

/* ============================================================================
 * Suite 3: BT RF eFuse Trim, 86-Channel VCO LUT & 32-Step BPF Calibration Tests
 * ============================================================================ */
static void test_bt_rf(void)
{
    soc_reset();

    /* eFuse bit 31 = 0 -> fallback trim 4 */
    g_soc.efuse_words[3] = 0x00000000u;
    g_soc.efuse_words[1] = (9u << 14);
    assert(bt_rf_efuse_trim() == 4u);
    assert(g_soc.rom_rst_last_id == 0x58u && g_soc.rom_rst_calls == 1u);
    assert(g_soc.rom_clk_last_id == 0x1Bu && g_soc.rom_clk_calls == 1u);

    /* eFuse bit 31 = 1 -> calibrated nibble [17:14] = 11 */
    g_soc.efuse_words[3] = (1u << 31);
    g_soc.efuse_words[1] = (11u << 14);
    assert(bt_rf_efuse_trim() == 11u);

    /* Full calibrated RF + Modem bring-up */
    uint32_t cal = bt_rf_init_calibrated(0x55u, 24000000u);
    assert(g_soc.rf_vco_cal_steps >= 172u); /* 86 TX + 86 RX VCO cal steps */

    /* Verify all 86 PLL & calibrated VCO band/cap LUT entries (0x9E..0xF3, 0xF4..0x149) */
    for (uint32_t ch = 0u; ch < 86u; ch++) {
        assert(g_soc.rf_lut_writes[0x9Eu + ch] >= 1u);
        assert(g_soc.rf_lut_writes[0xF4u + ch] >= 1u);

        uint32_t pll_int  = (2400u + ch) / 12u;
        uint32_t pll_frac = (((2400u + ch) % 12u) << 21) / 12u;
        assert(g_soc.rf_lut[0x9Eu + ch] == ((pll_int << 21) | pll_frac));

        uint32_t tx_ni   = 200u + (ch / 12u);
        uint32_t rx_ni   = tx_ni + 7u;
        uint32_t nf      = ((ch % 12u) << 21) / 12u;
        uint32_t tx_cap  = ((nf >> 16) + tx_ni * 3u) & 0x7Fu;
        uint32_t tx_band = (tx_ni - 200u) & 0xFFu;
        uint32_t rx_cap  = ((nf >> 16) + rx_ni * 3u) & 0x7Fu;
        uint32_t rx_band = (rx_ni - 200u) & 0xFFu;
        uint32_t expect  = tx_band | (tx_cap << 8) | (rx_band << 16) | (rx_cap << 24);
        assert(g_soc.rf_lut[0xF4u + ch] == expect);
    }

    /* Verify 32-step BPF calibration sweep across RF_ANA + RWBT_EM */
    assert(g_soc.bpf_sweep_mask == 0xFFFFFFFFu);
    assert(g_soc.rf_rx_captures == 33u);
    assert(cal == 18u);
    assert(((g_soc.rf_ana[0xA4u >> 2] >> 24) & 0x1Fu) == cal);

    /* Verify RF exit from config mode, eFuse trim applied, AON freq drift & Modem init */
    assert((g_soc.cru_bt_rf_wrap & 1u) == 1u);
    assert(((g_soc.rf_ana[0x40u >> 2] >> 16) & 0xFu) == 11u);
    assert(((g_soc.aon_clk_cfg >> 4) & 0xFFu) == 0x55u);
    assert((g_soc.rf_bb[0] & (1u << 26)) == 0u);
    assert((g_soc.rf_bb[0] & 0x073F0000u) == ((1u << 24) | (12u << 16)));
    assert(g_soc.rf_bb[0x298u >> 2] == 0x2D2B2927u);
    assert(g_soc.rf_bb[0x30Cu >> 2] == 0x00353B73u);
    printf("[OK] bt_rf MMIO (eFuse trim, 86ch VCO LUT, 32-step BPF cal=%u, modem)\n", cal);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test_rec_exfat.img>\n", argv[0]);
        return 2;
    }
    test_audio_dma();
    test_recorder(argv[1]);
    test_audio_dac();
    test_bt_rf();
    printf("ALL AUDIO/DAC/RECORDER/BT_RF MMIO SUITES PASSED\n");
    return 0;
}
