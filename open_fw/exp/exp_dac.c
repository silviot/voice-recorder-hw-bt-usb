/*
 * Experiment 5 (Audio DAC0+DAC1 Stereo Headphone TX + DMAC CH0 IRQ 60):
 * Configures Audio PLL, stereo 16-bit DAC0 (L) + DAC1 (R), eFuse DC trims,
 * and DMAC CH0 double-buffered TX to play a 440 Hz (L) / 660 Hz (R) tone for
 * 2 seconds at `arg` Hz (default 16000; supports 16000 / 24000 / 48000 / 44100).
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

#define CODEC_DAC2_ANA_VOL_ADDR  (AUDIO_CODEC_BASE + 0x02Cu)
#define CODEC_DAC01_ANA_VOL_ADDR (AUDIO_CODEC_BASE + 0x030u)
#define CODEC_DAC_ANA_CFG0_ADDR  (AUDIO_CODEC_BASE + 0x040u)
#define CODEC_DAC_ANA_CFG1_ADDR  (AUDIO_CODEC_BASE + 0x04Cu)
#define CODEC_DAC0_PWR_ADDR      (AUDIO_CODEC_BASE + 0x050u)
#define CODEC_DAC1_PWR_ADDR      (AUDIO_CODEC_BASE + 0x054u)
#define CODEC_DAC2_PWR_ADDR      (AUDIO_CODEC_BASE + 0x058u)
#define CODEC_DAC0_DC_ADDR       (AUDIO_CODEC_BASE + 0x064u)
#define CODEC_DAC1_DC_ADDR       (AUDIO_CODEC_BASE + 0x068u)
#define CODEC_DAC_FIFO_CTRL_ADDR (AUDIO_CODEC_BASE + 0x108u)
#define CODEC_DAC_TX_FIFO_ADDR   (AUDIO_CODEC_BASE + 0x10Cu)
#define CODEC_DAC_DIG_CFG_ADDR   (AUDIO_CODEC_BASE + 0x110u)
#define CODEC_DAC_FIFO_RST_ADDR  (AUDIO_CODEC_BASE + 0x114u)
#define CODEC_DAC0_DIG_EN_ADDR   (AUDIO_CODEC_BASE + 0x130u)
#define CODEC_DAC1_DIG_EN_ADDR   (AUDIO_CODEC_BASE + 0x150u)
#define CODEC_DAC2_DIG_EN_ADDR   (AUDIO_CODEC_BASE + 0x170u)
#define DMAC_CTL_DAC_TX_BASE     0x002812A0u

#define DAC_BUF_FRAMES 512u
#define DAC_BUF_BYTES  (DAC_BUF_FRAMES * 4u)

static const int16_t k_sin64[64] = {
        0,  1176,  2341,  3483,  4592,  5656,  6667,  7613,
     8485,  9276,  9977, 10583, 11086, 11482, 11768, 11942,
    12000, 11942, 11768, 11482, 11086, 10583,  9977,  9276,
     8485,  7613,  6667,  5656,  4592,  3483,  2341,  1176,
        0, -1176, -2341, -3483, -4592, -5656, -6667, -7613,
    -8485, -9276, -9977,-10583,-11086,-11482,-11768,-11942,
   -12000,-11942,-11768,-11482,-11086,-10583, -9977, -9276,
    -8485, -7613, -6667, -5656, -4592, -3483, -2341, -1176,
};

static uint32_t g_dac_buf[2][DAC_BUF_FRAMES] __attribute__((aligned(4)));
static volatile uint32_t g_dac_irq_cnt   = 0u;
static volatile uint8_t  g_dac_buf_idx   = 0u;
static volatile uint8_t  g_dac_refill    = 0u;
static volatile uint8_t  g_dac_running   = 0u;

static inline void dmac_arm_tx(const void *src_buf)
{
    REG32(DMAC_CH_SRC_ADDR(0u))      = (uint32_t)(uintptr_t)src_buf;
    REG32(DMAC_CH_DST_ADDR(0u))      = CODEC_DAC_TX_FIFO_ADDR;
    REG32(DMAC_CH_XFER_CNT_ADDR(0u)) = 0xFF000000u | (DAC_BUF_BYTES & 0x3FFFFu);
    REG32(DMAC_CH_CTL_ADDR(0u))      = (REG32(DMAC_CH_CTL_ADDR(0u)) & ~DMAC_CTL_RELOAD) | DMAC_CTL_CH_EN;
}

static void dac_dmac_ch0_isr(void)
{
    uint32_t st = REG32(DMAC_CH_IRQ_STAT_ADDR(0u));
    REG32(DMAC_CH_IRQ_STAT_ADDR(0u)) = st;
    if (!g_dac_running) {
        return;
    }
    if (st & DMAC_IRQ_TC) {
        uint8_t done = g_dac_buf_idx;
        g_dac_buf_idx ^= 1u;
        dmac_arm_tx(g_dac_buf[g_dac_buf_idx]);
        __asm__ volatile ("dsb" ::: "memory");
        g_dac_refill = (uint8_t)(done + 1u);
        g_dac_irq_cnt++;
    }
}

static uint32_t dac_rate_code(uint32_t sample_rate)
{
    switch (sample_rate) {
    case 8000u:   return 0u;
    case 12000u:  return 1u;
    case 11025u:  return 2u;
    case 16000u:  return 3u;
    case 24000u:  return 4u;
    case 22050u:  return 5u;
    case 32000u:  return 6u;
    case 48000u:  return 7u;
    case 44100u:  return 8u;
    default:      return 3u;
    }
}

static void dac_pll_set(uint32_t sample_rate)
{
    uint32_t is_48k  = ((sample_rate % 8000u) == 0u) ? 1u : 0u;
    uint32_t int_div = is_48k ? 3u : 2u;
    uint32_t frac    = is_48k ? 0x03126u : 0x186C2u;
    while (REG32(CRU_APLL_FRAC_ADDR) & (1u << 11)) {
    }
    REG32(CRU_APLL_CTRL_ADDR) = (REG32(CRU_APLL_CTRL_ADDR) & 0xFC1F0000u) | 0x3000u | (int_div << 5);
    REG32(CRU_APLL_FRAC_ADDR) = (REG32(CRU_APLL_FRAC_ADDR) & 0x00003800u) | 0x80000400u | (frac << 14);
    REG32(CRU_APLL_CTRL_ADDR) |= 0xC0000000u;
    while (!(REG32(CRU_APLL_CTRL_ADDR) & (1u << 28))) {
    }
}

static void dac_set_vol(uint8_t ana_vol, uint16_t dig_vol)
{
    uint32_t av = (ana_vol > 31u) ? 31u : ana_vol;
    uint32_t dv = (dig_vol > 0x1FFu) ? 0x1FFu : dig_vol;
    REG32(CODEC_DAC0_VOL_ADDR) = (REG32(CODEC_DAC0_VOL_ADDR) & ~0x1FFu) | dv;
    REG32(CODEC_DAC1_VOL_ADDR) = (REG32(CODEC_DAC1_VOL_ADDR) & ~0x1FFu) | dv;
    if (REG32(CODEC_DAC0_PWR_ADDR) & (1u << 29)) {
        REG32(CODEC_DAC01_ANA_VOL_ADDR) = (REG32(CODEC_DAC01_ANA_VOL_ADDR) & ~0x001F0000u) | (av << 16);
    }
    if (REG32(CODEC_DAC1_PWR_ADDR) & (1u << 29)) {
        REG32(CODEC_DAC01_ANA_VOL_ADDR) = (REG32(CODEC_DAC01_ANA_VOL_ADDR) & ~0x1F000000u) | (av << 24);
    }
}

static void fill_tone_buf(uint32_t *dst, uint32_t sample_rate,
                          uint32_t *ph_l, uint32_t *ph_r,
                          uint32_t step_l, uint32_t step_r,
                          uint32_t *frame_idx, uint32_t total_frames)
{
    uint32_t fade_len = sample_rate / 20u; /* 50 ms ramp */
    if (fade_len == 0u) fade_len = 1u;
    for (uint32_t i = 0u; i < DAC_BUF_FRAMES; i++) {
        uint32_t fn = (*frame_idx)++;
        int32_t sl = k_sin64[(*ph_l >> 10) & 63u];
        int32_t sr = k_sin64[(*ph_r >> 10) & 63u];
        *ph_l += step_l;
        *ph_r += step_r;

        if (fn >= total_frames) {
            sl = 0;
            sr = 0;
        } else if (fn < fade_len) {
            sl = (sl * (int32_t)fn) / (int32_t)fade_len;
            sr = (sr * (int32_t)fn) / (int32_t)fade_len;
        } else if (fn + fade_len > total_frames) {
            int32_t rem = (int32_t)(total_frames - fn);
            sl = (sl * rem) / (int32_t)fade_len;
            sr = (sr * rem) / (int32_t)fade_len;
        }
        dst[i] = ((uint32_t)(uint16_t)(int16_t)sr << 16) | (uint16_t)(int16_t)sl;
    }
}

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint32_t rate = (arg == 24000u || arg == 48000u || arg == 44100u || arg == 16000u)
                  ? arg : 16000u;

    api->wdt_feed();
    rom_rst_release(0x58u);
    rom_clk_enable(0x1Bu);
    uint32_t w2 = REG32(EFUSE_WORD_ADDR(2u));
    uint32_t w3 = REG32(EFUSE_WORD_ADDR(3u));
    uint32_t d0_dc = ((w3 << 3) & 0x78u) | (w2 >> 29);
    uint32_t d1_dc = (w3 >> 4) & 0x7Fu;

    /* 1. CRU codec rst 0x25 + clk 0x13 + global enable + clear codec SRAM */
    REG32(CRU_RST_SET1_ADDR)  |= (1u << 5);
    REG32(CRU_RST_STAT1_ADDR) |= (1u << 5);
    while (!(REG32(CRU_RST_STAT1_ADDR) & (1u << 5))) {
    }
    REG32(CRU_CLK_EN_AUDIO_ADDR) |= 1u;
    REG32(CODEC_GLB_ADDR) |= 1u;
    api->mdelay(20u);

    REG32(CODEC_DAC0_VOL_ADDR) |= 0x1FFu;
    REG32(CODEC_DAC1_VOL_ADDR) |= 0x1FFu;
    REG32(CODEC_DAC2_VOL_ADDR) |= 0x1FFu;

    REG32(CRU_RST_SET1_ADDR)  |= 1u;
    REG32(CRU_RST_STAT1_ADDR) |= 1u;
    while (!(REG32(CRU_RST_STAT1_ADDR) & 1u)) {
    }
    REG32(SYSCTL_CODEC_SRAM_ADDR) |= (1u << 2);
    for (uint32_t a = CODEC_SRAM_START; a < CODEC_SRAM_END; a += 4u) {
        REG32(a) = 0u;
    }
    REG32(SYSCTL_CODEC_SRAM_ADDR) &= ~(1u << 2);
    REG32(CRU_RST_STAT1_ADDR) &= ~1u;
    REG32(CRU_RST_SET1_ADDR)  &= ~1u;

    REG32(CODEC_GLB_ADDR) = (REG32(CODEC_GLB_ADDR) & ~0x000FF000u) | 0x00055000u;
    REG32(CODEC_DAC0_MUTE_ADDR) &= ~(1u << 16);
    REG32(CODEC_DAC1_MUTE_ADDR) &= ~(1u << 16);
    REG32(CODEC_DAC2_MUTE_ADDR) &= ~(1u << 16);

    /* 2. APLL + digital DAC0+DAC1 16-bit DMA */
    dac_pll_set(rate);
    REG32(CODEC_ANA_CTRL0_ADDR) |= 0x80000022u;
    REG32(CODEC_DAC_DIG_CFG_ADDR) &= ~0x03FFFFFFu;
    REG32(CODEC_GLB_ADDR) = (REG32(CODEC_GLB_ADDR) & ~0xF00u) | 0x2u | (dac_rate_code(rate) << 8);
    REG32(CODEC_DAC_FIFO_CTRL_ADDR) |= 0x84u;
    REG32(CODEC_DAC_FIFO_RST_ADDR)  |= 0x2u;
    for (uint32_t t = 1000000u; t > 0u; t--) {
        if (!(REG32(CODEC_DAC_FIFO_RST_ADDR) & 0x2u)) break;
    }
    REG32(CODEC_DAC0_DIG_EN_ADDR) |= 0x3u;
    REG32(CODEC_DAC1_DIG_EN_ADDR) |= 0x3u;
    REG32(CODEC_DAC2_DIG_EN_ADDR) &= ~0x3u;

    /* 3. Analog DAC0+DAC1 power-up + eFuse DC-offset + anti-pop */
    REG32(CODEC_DAC01_ANA_VOL_ADDR) |= 0x11u;
    REG32(CODEC_DAC_ANA_CFG0_ADDR)  |= 0x200u;
    REG32(CODEC_DAC_ANA_CFG1_ADDR)  |= 0x30000u;
    REG32(CODEC_DAC_ANA_CFG0_ADDR)   = (REG32(CODEC_DAC_ANA_CFG0_ADDR) & ~0x30u) | 0x20u;
    REG32(CODEC_DAC_ANA_CFG1_ADDR)  |= 0x100000u;
    REG32(CODEC_DAC_ANA_CFG0_ADDR)  |= 0x800u;
    REG32(CODEC_DAC0_PWR_ADDR)      |= 0x8000u;
    REG32(CODEC_DAC1_PWR_ADDR)      |= 0x8000u;
    REG32(CODEC_DAC0_DC_ADDR)        = d0_dc;
    REG32(CODEC_DAC1_DC_ADDR)        = d1_dc;
    api->mdelay(10u);
    REG32(CODEC_DAC0_PWR_ADDR)      |= 0x30000000u;
    REG32(CODEC_DAC1_PWR_ADDR)      |= 0x30000000u;
    REG32(CODEC_DAC_ANA_CFG1_ADDR)  |= 0x30u;
    REG32(CODEC_DAC_ANA_CFG1_ADDR)  |= 0x03u;
    api->mdelay(10u);
    REG32(CODEC_DAC0_PWR_ADDR)      |= 0x0FFFu;
    REG32(CODEC_DAC1_PWR_ADDR)      |= 0x0FFFu;
    api->mdelay(10u);
    REG32(CODEC_DAC_ANA_CFG1_ADDR)  &= ~0x03u;
    dac_set_vol(20u, 0x1FFu);

    /* 4. Prepare 2 s 440 Hz (L) / 660 Hz (R) tone & DMAC CH0 */
    uint32_t ph_l = 0u, ph_r = 0u, frame_idx = 0u;
    uint32_t step_l = (440u * 64u * 1024u) / rate;
    uint32_t step_r = (660u * 64u * 1024u) / rate;
    uint32_t total_frames = rate * 2u;

    fill_tone_buf(g_dac_buf[0], rate, &ph_l, &ph_r, step_l, step_r, &frame_idx, total_frames);
    fill_tone_buf(g_dac_buf[1], rate, &ph_l, &ph_r, step_l, step_r, &frame_idx, total_frames);

    REG32(CRU_RST_SET1_ADDR)  |= (1u << 1);
    REG32(CRU_RST_STAT1_ADDR) |= (1u << 1);
    while (!(REG32(CRU_RST_STAT1_ADDR) & (1u << 1))) {
    }

    REG32(DMAC_CH_CTL_ADDR(0u))      = DMAC_CTL_DAC_TX_BASE;
    REG32(DMAC_CH_IRQ_STAT_ADDR(0u)) = REG32(DMAC_CH_IRQ_STAT_ADDR(0u));
    REG32(DMAC_CH_IRQ_EN_ADDR(0u))   = 0x00080001u;

    g_dac_irq_cnt = 0u;
    g_dac_buf_idx = 0u;
    g_dac_refill  = 0u;
    g_dac_running = 1u;

    api->set_dmac_ch0_irq(dac_dmac_ch0_isr);
    nvic_clear_pending(DMAC_IRQ_BASE);
    nvic_enable_irq(DMAC_IRQ_BASE);

    uint64_t us0 = api->us_timer_now();
    dmac_arm_tx(g_dac_buf[0]);

    uint32_t ms0 = api->millis();
    uint32_t need_irqs = (total_frames + DAC_BUF_FRAMES - 1u) / DAC_BUF_FRAMES;
    while (g_dac_irq_cnt < need_irqs && (uint32_t)(api->millis() - ms0) < 2500u) {
        api->wdt_feed();
        uint8_t rf = g_dac_refill;
        if (rf != 0u) {
            g_dac_refill = 0u;
            fill_tone_buf(g_dac_buf[rf - 1u], rate, &ph_l, &ph_r, step_l, step_r, &frame_idx, total_frames);
        } else {
            OPENFW_WFI();
        }
    }

    uint64_t us1 = api->us_timer_now();
    g_dac_running = 0u;
    api->set_dmac_ch0_irq(0);

    /* 5. Clean anti-pop shutdown */
    REG32(CODEC_DAC01_ANA_VOL_ADDR) &= ~0x1F1F0000u;
    for (uint32_t i = 0u; i < 10u; i++) {
        if ((REG32(CODEC_DAC01_ANA_VOL_ADDR) & 0xE0000000u) == 0u) break;
        api->mdelay(1u);
    }
    api->mdelay(10u);
    REG32(CODEC_DAC0_PWR_ADDR)      &= ~0x0FFFu;
    REG32(CODEC_DAC1_PWR_ADDR)      &= ~0x0FFFu;
    REG32(CODEC_DAC2_PWR_ADDR)      &= ~0x0FFFu;
    REG32(CODEC_DAC_ANA_CFG1_ADDR)  &= ~0x430u;
    REG32(CODEC_DAC0_PWR_ADDR)      &= ~0x30000000u;
    REG32(CODEC_DAC1_PWR_ADDR)      &= ~0x30000000u;
    REG32(CODEC_DAC2_PWR_ADDR)      &= ~0x30000000u;
    REG32(CODEC_DAC_ANA_CFG0_ADDR)  &= ~0x200u;
    REG32(CODEC_DAC2_ANA_VOL_ADDR)  &= ~0x001F0000u;
    REG32(CODEC_DAC_FIFO_CTRL_ADDR) &= ~0x80u;
    REG32(CODEC_DAC0_DIG_EN_ADDR)   &= ~0x3u;
    REG32(CODEC_DAC1_DIG_EN_ADDR)   &= ~0x3u;
    REG32(CODEC_GLB_ADDR)           &= ~0x2u;

    uint32_t irqs         = g_dac_irq_cnt;
    uint32_t samples_sent = irqs * DAC_BUF_FRAMES;
    uint32_t elapsed_ms   = (uint32_t)(us1 - us0) / 1000u;
    uint32_t actual_hz    = elapsed_ms ? ((samples_sent * 1000u) / elapsed_ms) : 0u;

    api->log_puts("[exp_dac] rate=");
    api->log_u32(rate);
    api->log_puts(" hp_det=");
    api->log_u32((uint32_t)api->keys_read_hp_detect());
    api->log_puts(" d0_dc=");
    api->log_u32(d0_dc);
    api->log_puts(" d1_dc=");
    api->log_u32(d1_dc);
    api->log_puts(" irqs=");
    api->log_u32(irqs);
    api->log_puts(" samples=");
    api->log_u32(samples_sent);
    api->log_puts(" ms=");
    api->log_u32(elapsed_ms);
    api->log_puts(" actual_hz=");
    api->log_u32(actual_hz);
    api->log_putc('\n');
    return irqs;
}
