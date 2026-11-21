/*
 * Smartlink SL6801 Bare-Metal Stereo Audio DAC0+DAC1 Headphone + DMAC CH0 TX Driver
 *
 * Implements verified Audio PLL (24.576 / 22.5792 MHz), eFuse DAC0/DAC1
 * DC-offset trims, pop-free analog power-up/down, and double-buffered
 * DMAC CH0 (NVIC IRQ 60) stereo 16-bit FIFO playback.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_exp.h"

#define DAC_DMA_CH       0u
#define DAC_SLOT_FRAMES  1024u
#define DAC_SLOT_SAMPLES (DAC_SLOT_FRAMES * 2u)

#define EFUSE_RST_ID     0x58u
#define EFUSE_CLK_ID     0x1Bu

/*
 * Built-in front speaker PA enable on PB30 (verified from HW_76 pen133_2.0.65):
 *   ON  = PB30 mux=0, pull-up enabled  (rom_gpio_config(0x0001F008u))
 *   OFF = PB30 mux=0, pull-up disabled (rom_gpio_config(0x0001F000u))
 */
#define AUDIO_PA_GPIO_ON   0x0001F008u
#define AUDIO_PA_GPIO_OFF  0x0001F000u

/* Level zero mutes. Levels 1..31 span -48 dB to unity in 1.6 dB steps. */
static const uint16_t pcm_gain_q15[32] = {
    0, 130, 157, 189, 227, 273, 328, 394,
    474, 569, 685, 823, 990, 1190, 1430, 1720,
    2068, 2486, 2988, 3593, 4320, 5193, 6244, 7507,
    9025, 10851, 13045, 15684, 18856, 22670, 27255, 32768,
};

static int16_t g_dac_buf[AUDIO_DAC_QUEUE_SLOTS][DAC_SLOT_SAMPLES] __attribute__((aligned(4)));
static volatile uint8_t  g_dac_running   = 0u;
static volatile uint8_t  g_tx_active     = 0u;
static volatile uint8_t  g_active_slot   = 0u;
static uint8_t           g_write_slot;
static volatile uint32_t g_slot_bytes[AUDIO_DAC_QUEUE_SLOTS];
static uint32_t          g_pcm_gain_q15;
static int8_t            g_pa_state      = -1;

void audio_pa_set(int enable)
{
    int8_t want = enable ? 1 : 0;
    if (g_pa_state == want) {
        return;
    }
    rom_gpio_config(want ? AUDIO_PA_GPIO_ON : AUDIO_PA_GPIO_OFF);
    g_pa_state = want;
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
    case 96000u:  return 9u;
    case 88200u:  return 10u;
    case 192000u: return 11u;
    case 176400u: return 12u;
    default:      return UINT32_MAX;
    }
}

void audio_pll_set(uint32_t sample_rate)
{
    uint32_t is_48k  = ((sample_rate % 11025u) != 0u) ? 1u : 0u;
    uint32_t int_div = is_48k ? 3u : 2u;
    uint32_t frac    = is_48k ? 0x03126u : 0x186C2u;

    while (openfw_mmio_rd32(CRU_APLL_FRAC_ADDR) & (1u << 11)) {
    }
    openfw_mmio_wr32(CRU_APLL_CTRL_ADDR,
                     (openfw_mmio_rd32(CRU_APLL_CTRL_ADDR) & 0xFC1F0000u) |
                     0x3000u | (int_div << 5));
    openfw_mmio_wr32(CRU_APLL_FRAC_ADDR,
                     (openfw_mmio_rd32(CRU_APLL_FRAC_ADDR) & 0x00003800u) |
                     0x80000400u | (frac << 14));
    openfw_mmio_set32(CRU_APLL_CTRL_ADDR, 0xC0000000u);
    while (!(openfw_mmio_rd32(CRU_APLL_CTRL_ADDR) & (1u << 28))) {
    }
}

static void dmac_arm_tx_slot(uint8_t slot)
{
    uint32_t len_bytes = g_slot_bytes[slot];
    g_active_slot = slot;
    g_tx_active   = 1u;
    OPENFW_HOST_DMAC_BUF(DAC_DMA_CH, g_dac_buf[slot], len_bytes);
    /* Clear CH_EN=0 before re-programming so single-shot DMAC sees a 0->1 rising edge */
    openfw_mmio_wr32(DMAC_CH_CTL_ADDR(DAC_DMA_CH), DMAC_CTL_DAC_TX_BASE);
    openfw_mmio_wr32(DMAC_CH_SRC_ADDR(DAC_DMA_CH), (uint32_t)(uintptr_t)g_dac_buf[slot]);
    openfw_mmio_wr32(DMAC_CH_DST_ADDR(DAC_DMA_CH), CODEC_DAC_TX_FIFO_ADDR);
    openfw_mmio_wr32(DMAC_CH_XFER_CNT_ADDR(DAC_DMA_CH), 0xFF000000u | (len_bytes & 0x3FFFFu));
    openfw_mmio_wr32(DMAC_CH_CTL_ADDR(DAC_DMA_CH), DMAC_CTL_DAC_TX_BASE | DMAC_CTL_CH_EN);
}

static void audio_dac_dmac_ch0_irq(void)
{
    uint32_t st = openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(DAC_DMA_CH));
    openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(DAC_DMA_CH), st);

    if (!g_dac_running) {
        g_tx_active = 0u;
        return;
    }

    if (st & DMAC_IRQ_TC) {
        uint8_t done = g_active_slot;
        uint8_t next = (done + 1u) % AUDIO_DAC_QUEUE_SLOTS;
        g_slot_bytes[done] = 0u;
        if (g_slot_bytes[next]) {
            dmac_arm_tx_slot(next);
            OPENFW_DSB();
        } else {
            openfw_mmio_clr32(DMAC_CH_CTL_ADDR(DAC_DMA_CH), DMAC_CTL_CH_EN);
            g_tx_active = 0u;
        }
    }
}

void audio_dac_set_volume(uint8_t ana_vol_0_31, uint16_t dig_vol_0_511)
{
    uint32_t av = (ana_vol_0_31 > 31u) ? 31u : (uint32_t)ana_vol_0_31;
    uint32_t dv = (dig_vol_0_511 > 0x1FFu) ? 0x1FFu : (uint32_t)dig_vol_0_511;

    /* Apply the user level to both PCM channels, including a true silent level. */
    g_pcm_gain_q15 = pcm_gain_q15[av];
    openfw_mmio_rmw32(CODEC_DAC0_VOL_ADDR, 0x1FFu, dv);
    openfw_mmio_rmw32(CODEC_DAC1_VOL_ADDR, 0x1FFu, dv);
    if (openfw_mmio_rd32(CODEC_DAC0_PWR_ADDR) & (1u << 29)) {
        openfw_mmio_rmw32(CODEC_DAC01_ANA_VOL_ADDR, 0x001F0000u, av << 16);
    }
    if (openfw_mmio_rd32(CODEC_DAC1_PWR_ADDR) & (1u << 29)) {
        openfw_mmio_rmw32(CODEC_DAC01_ANA_VOL_ADDR, 0x1F000000u, av << 24);
    }
}

void audio_dac_set_mute(int muted)
{
    if (muted) {
        openfw_mmio_set32(CODEC_DAC0_MUTE_ADDR, (1u << 16));
        openfw_mmio_set32(CODEC_DAC1_MUTE_ADDR, (1u << 16));
    } else {
        openfw_mmio_clr32(CODEC_DAC0_MUTE_ADDR, (1u << 16));
        openfw_mmio_clr32(CODEC_DAC1_MUTE_ADDR, (1u << 16));
    }
}

int audio_dac_start(uint32_t sample_rate, uint8_t ana_vol_0_31)
{
    uint32_t rate_code = dac_rate_code(sample_rate);
    if (rate_code == UINT32_MAX) {
        return -1;
    }
    if (g_dac_running) {
        audio_dac_stop();
    }

    wdt_feed();

    /* 1. CRU codec rst 0x25 + clk 0x13 + global enable + 20ms + clear codec SRAM */
    openfw_mmio_set32(CRU_RST_SET1_ADDR, (1u << 5));
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, (1u << 5));
    while (!(openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & (1u << 5))) {
    }
    openfw_mmio_set32(CRU_CLK_EN_AUDIO_ADDR, 1u);
    openfw_mmio_set32(CODEC_GLB_ADDR, 1u);
    mdelay(20u);
    wdt_feed();

    openfw_mmio_rmw32(CODEC_DAC0_VOL_ADDR, 0x1FFu, 0x1FFu);
    openfw_mmio_rmw32(CODEC_DAC1_VOL_ADDR, 0x1FFu, 0x1FFu);
    openfw_mmio_rmw32(CODEC_DAC2_VOL_ADDR, 0x1FFu, 0x1FFu);

    openfw_mmio_set32(CRU_RST_SET1_ADDR, 1u);
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, 1u);
    while (!(openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & 1u)) {
    }
    openfw_mmio_set32(SYSCTL_CODEC_SRAM_ADDR, (1u << 2));
    for (uint32_t a = CODEC_SRAM_START; a < CODEC_SRAM_END; a += 4u) {
        openfw_mmio_wr32(a, 0u);
    }
    openfw_mmio_clr32(SYSCTL_CODEC_SRAM_ADDR, (1u << 2));
    openfw_mmio_clr32(CRU_RST_STAT1_ADDR, 1u);
    openfw_mmio_clr32(CRU_RST_SET1_ADDR, 1u);

    openfw_mmio_rmw32(CODEC_GLB_ADDR, 0x000FF000u, 0x00055000u);
    openfw_mmio_clr32(CODEC_DAC0_MUTE_ADDR, (1u << 16));
    openfw_mmio_clr32(CODEC_DAC1_MUTE_ADDR, (1u << 16));
    openfw_mmio_clr32(CODEC_DAC2_MUTE_ADDR, (1u << 16));

    /* 2. Audio PLL & digital DAC0+DAC1 16-bit DMA */
    audio_pll_set(sample_rate);
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x80000022u);
    openfw_mmio_clr32(CODEC_DAC_DIG_CFG_ADDR, 0x03FFFFFFu);
    openfw_mmio_rmw32(CODEC_GLB_ADDR, 0xF02u, 0x2u | (rate_code << 8));
    openfw_mmio_set32(CODEC_DAC_FIFO_CTRL_ADDR, 0x84u);
    openfw_mmio_set32(CODEC_DAC_FIFO_RST_ADDR, 0x2u);
    for (uint32_t t = 1000000u; t > 0u; t--) {
        if (!(openfw_mmio_rd32(CODEC_DAC_FIFO_RST_ADDR) & 0x2u)) {
            break;
        }
    }
    openfw_mmio_set32(CODEC_DAC0_DIG_EN_ADDR, 0x3u);
    openfw_mmio_set32(CODEC_DAC1_DIG_EN_ADDR, 0x3u);
    openfw_mmio_clr32(CODEC_DAC2_DIG_EN_ADDR, 0x3u);

    /* 3. Analog DAC0+DAC1 power-up + eFuse DC-offset trims + anti-pop */
    openfw_mmio_set32(CODEC_DAC01_ANA_VOL_ADDR, 0x11u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG0_ADDR, 0x200u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG1_ADDR, 0x30000u);
    openfw_mmio_rmw32(CODEC_DAC_ANA_CFG0_ADDR, 0x30u, 0x20u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG1_ADDR, 0x100000u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG0_ADDR, 0x800u);
    openfw_mmio_set32(CODEC_DAC0_PWR_ADDR, 0x8000u);
    openfw_mmio_set32(CODEC_DAC1_PWR_ADDR, 0x8000u);

    rom_rst_release(EFUSE_RST_ID);
    rom_clk_enable(EFUSE_CLK_ID);
    uint32_t w2 = openfw_mmio_rd32(EFUSE_WORD_ADDR(2u));
    uint32_t w3 = openfw_mmio_rd32(EFUSE_WORD_ADDR(3u));
    openfw_mmio_wr32(CODEC_DAC0_DC_ADDR, ((w3 << 3) & 0x78u) | (w2 >> 29));
    openfw_mmio_wr32(CODEC_DAC1_DC_ADDR, (w3 >> 4) & 0x7Fu);

    mdelay(10u);
    wdt_feed();
    openfw_mmio_set32(CODEC_DAC0_PWR_ADDR, 0x30000000u);
    openfw_mmio_set32(CODEC_DAC1_PWR_ADDR, 0x30000000u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG1_ADDR, 0x30u);
    openfw_mmio_set32(CODEC_DAC_ANA_CFG1_ADDR, 0x03u);
    mdelay(10u);
    openfw_mmio_set32(CODEC_DAC0_PWR_ADDR, 0x0FFFu);
    openfw_mmio_set32(CODEC_DAC1_PWR_ADDR, 0x0FFFu);
    mdelay(10u);
    wdt_feed();
    openfw_mmio_clr32(CODEC_DAC_ANA_CFG1_ADDR, 0x03u);
    audio_dac_set_volume(ana_vol_0_31, 0x1FFu);

    /* 4. Configure DMAC CH0 TX + NVIC IRQ 60 */
    g_tx_active     = 0u;
    g_active_slot   = 0u;
    g_write_slot = 0u;
    for (unsigned i = 0; i < AUDIO_DAC_QUEUE_SLOTS; i++) g_slot_bytes[i] = 0u;

    openfw_mmio_set32(CRU_RST_SET1_ADDR, (1u << 1));
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, (1u << 1));
    while (!(openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & (1u << 1))) {
    }
    openfw_mmio_wr32(DMAC_CH_CTL_ADDR(DAC_DMA_CH), DMAC_CTL_DAC_TX_BASE);
    openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(DAC_DMA_CH),
                     openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(DAC_DMA_CH)));
    openfw_mmio_wr32(DMAC_CH_IRQ_EN_ADDR(DAC_DMA_CH), DMAC_AUDIO_IRQ_EN); /* TC_IE */

    openfw_exp_set_dmac_ch0_irq(audio_dac_dmac_ch0_irq);
    nvic_clear_pending(DMAC_IRQ_BASE + DAC_DMA_CH);
    nvic_enable_irq(DMAC_IRQ_BASE + DAC_DMA_CH);

    g_dac_running = 1u;
    return 0;
}

int audio_dac_write_stereo(const int16_t *interleaved_lr, uint32_t num_frames)
{
    if (!g_dac_running || !interleaved_lr || num_frames == 0u) {
        return 0;
    }

    uint32_t primask;
    /* Single producer. The IRQ releases a slot only after its DMA completes. */
    uint8_t slot = g_write_slot;
    if (g_slot_bytes[slot]) {
        return 0;
    }

    uint32_t frames  = (num_frames > DAC_SLOT_FRAMES) ? DAC_SLOT_FRAMES : num_frames;
    uint32_t samples = frames * 2u;
    int16_t *dst     = g_dac_buf[slot];
    for (uint32_t i = 0u; i < samples; i++) {
        dst[i] = (int16_t)(((int32_t)interleaved_lr[i] * (int32_t)g_pcm_gain_q15) / 32768);
    }

    OPENFW_IRQ_SAVE(primask);
    if (g_dac_running) {
        g_slot_bytes[slot] = frames * 4u;
        g_write_slot = (slot + 1u) % AUDIO_DAC_QUEUE_SLOTS;
        OPENFW_DSB();
        if (!g_tx_active) {
            dmac_arm_tx_slot(slot);
        }
    } else {
        frames = 0u;
    }
    OPENFW_IRQ_RESTORE(primask);

    return (int)frames;
}

int audio_dac_is_busy(void)
{
    return g_tx_active;
}

void audio_dac_stop(void)
{
    if (!g_dac_running) {
        return;
    }
    g_dac_running   = 0u;
    g_tx_active     = 0u;


    /* Shut down built-in speaker PA first for pop-free teardown */
    audio_pa_set(0);
    mdelay(5u);

    openfw_exp_set_dmac_ch0_irq(0);

    openfw_mmio_clr32(CODEC_DAC01_ANA_VOL_ADDR, 0x1F1F0000u);
    for (uint32_t i = 0u; i < 10u; i++) {
        if ((openfw_mmio_rd32(CODEC_DAC01_ANA_VOL_ADDR) & 0xE0000000u) == 0u) {
            break;
        }
        mdelay(1u);
    }
    mdelay(10u);
    wdt_feed();

    openfw_mmio_clr32(CODEC_DAC0_PWR_ADDR, 0x0FFFu);
    openfw_mmio_clr32(CODEC_DAC1_PWR_ADDR, 0x0FFFu);
    openfw_mmio_clr32(CODEC_DAC2_PWR_ADDR, 0x0FFFu);
    openfw_mmio_clr32(CODEC_DAC_ANA_CFG1_ADDR, 0x430u);
    openfw_mmio_clr32(CODEC_DAC0_PWR_ADDR, 0x30000000u);
    openfw_mmio_clr32(CODEC_DAC1_PWR_ADDR, 0x30000000u);
    openfw_mmio_clr32(CODEC_DAC2_PWR_ADDR, 0x30000000u);
    openfw_mmio_clr32(CODEC_DAC_ANA_CFG0_ADDR, 0x200u);
    openfw_mmio_clr32(CODEC_DAC2_ANA_VOL_ADDR, 0x001F0000u);
    openfw_mmio_clr32(CODEC_DAC_FIFO_CTRL_ADDR, 0x80u);
    openfw_mmio_clr32(CODEC_DAC0_DIG_EN_ADDR, 0x3u);
    openfw_mmio_clr32(CODEC_DAC1_DIG_EN_ADDR, 0x3u);
    openfw_mmio_clr32(CODEC_GLB_ADDR, 0x2u);
}
