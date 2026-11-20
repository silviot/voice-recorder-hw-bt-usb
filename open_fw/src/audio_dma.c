/*
 * Smartlink SL6801 Bare-Metal Audio ADC (AMIC1 mono / AMIC1+AMIC2 stereo, 16 kHz / 16-bit) + DMAC CH1
 *
 * Configures Audio PLL (24.576 MHz), AMIC1 differential PGA (PB12/PB13) and
 * optional AMIC2 differential PGA (PB14/PB15), 16 kHz 16-bit ADC capture,
 * and DMAC Channel 1 (NVIC IRQ 61) into a 4-slot (32 KiB) lock-free ring buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

#define AUDIO_DMA_CH           1u
#define AUDIO_RING_SLOTS       4u
#define AUDIO_FRAMES_PER_SLOT  2048u /* 128 ms @ 16 kHz */
#define AUDIO_MAX_SLOT_SAMPLES (AUDIO_FRAMES_PER_SLOT * 2u)

static int16_t g_pcm_buf[AUDIO_RING_SLOTS][AUDIO_MAX_SLOT_SAMPLES] __attribute__((aligned(4)));
static volatile uint8_t  g_dma_slot      = 0u;
static volatile uint8_t  g_running       = 0u;
static volatile uint8_t  g_channels      = 1u;
static volatile uint8_t  g_peak_pct      = 0u;
static volatile uint32_t g_frame_samples = AUDIO_FRAMES_PER_SLOT;
static volatile uint32_t g_frame_bytes   = AUDIO_FRAMES_PER_SLOT * sizeof(int16_t);
static volatile uint32_t g_frames        = 0u;
static volatile uint32_t g_wr_idx        = 0u;
static volatile uint32_t g_rd_idx        = 0u;
static volatile uint32_t g_overruns      = 0u;

static inline void dmac_arm_rx(void *dst_buf, uint32_t len_bytes)
{
    OPENFW_HOST_DMAC_BUF(AUDIO_DMA_CH, dst_buf, len_bytes);
    /* Clear CH_EN=0 before re-programming so single-shot DMAC sees a 0->1 rising edge */
    openfw_mmio_clr32(DMAC_CH_CTL_ADDR(AUDIO_DMA_CH), DMAC_CTL_CH_EN);
    openfw_mmio_wr32(DMAC_CH_SRC_ADDR(AUDIO_DMA_CH), CODEC_ADC_RX_FIFO_ADDR);
    openfw_mmio_wr32(DMAC_CH_DST_ADDR(AUDIO_DMA_CH), (uint32_t)(uintptr_t)dst_buf);
    openfw_mmio_wr32(DMAC_CH_XFER_CNT_ADDR(AUDIO_DMA_CH), 0xFF000000u | (len_bytes & 0x3FFFFu));
    openfw_mmio_rmw32(DMAC_CH_CTL_ADDR(AUDIO_DMA_CH), DMAC_CTL_RELOAD, DMAC_CTL_CH_EN);
}

static uint8_t compute_ac_peak_pct(const int16_t *samples, uint32_t count)
{
    int32_t min_s = samples[0];
    int32_t max_s = samples[0];
    for (uint32_t i = 1u; i < count; i++) {
        int32_t s = (int32_t)samples[i];
        if (s < min_s) {
            min_s = s;
        }
        if (s > max_s) {
            max_s = s;
        }
    }
    /* Half peak-to-peak removes any ADC DC offset */
    uint32_t ac_amp = (uint32_t)(max_s - min_s) >> 1;
    uint32_t pct    = (ac_amp * 100u) / 32767u;
    return (pct > 100u) ? 100u : (uint8_t)pct;
}

void DMAC_CH1_IRQHandler(void)
{
    uint32_t st = openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(AUDIO_DMA_CH));
    openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(AUDIO_DMA_CH), st);

    if (!g_running) {
        return;
    }

    if (st & DMAC_IRQ_TC) {
        uint8_t done_slot = g_dma_slot;
        uint8_t next_slot = (uint8_t)((done_slot + 1u) & (AUDIO_RING_SLOTS - 1u));
        g_dma_slot = next_slot;

        /* Reserve next_slot exclusively for DMAC: keep at most SLOTS-1 queued */
        if ((g_wr_idx - g_rd_idx) >= (AUDIO_RING_SLOTS - 1u)) {
            g_rd_idx++;
            g_overruns++;
        }

        dmac_arm_rx(g_pcm_buf[next_slot], g_frame_bytes);
        OPENFW_DSB();

        g_wr_idx++;
        g_peak_pct = compute_ac_peak_pct(g_pcm_buf[done_slot], g_frame_samples);
        g_frames++;
    }
}

static void apply_pga_gain(uint8_t gain_0_31, uint8_t channels)
{
    if (gain_0_31 > 31u) {
        gain_0_31 = 31u;
    }
    openfw_mmio_rmw32(CODEC_AMIC01_PGA_ADDR, 0x1F000000u,
                      ((uint32_t)gain_0_31 & 0x1Fu) << 24);
    if (channels == 2u) {
        uint32_t g2 = (uint32_t)gain_0_31 * 3u;
        if (g2 > 127u) {
            g2 = 127u;
        }
        openfw_mmio_rmw32(CODEC_AMIC2_CTRL_ADDR, 0x00007F00u, g2 << 8);
    }
}

static void audio_amic_16k_init(uint8_t gain_0_31, uint8_t channels)
{
    if (gain_0_31 > 31u) {
        gain_0_31 = 31u;
    }

    /* 1. CRU: release codec reset 0x25 and enable codec clock 0x13 */
    openfw_mmio_set32(CRU_RST_SET1_ADDR, (1u << 5));
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, (1u << 5));
    while (!(openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & (1u << 5))) {
    }
    openfw_mmio_set32(CRU_CLK_EN_AUDIO_ADDR, 1u);

    /* 2. Global codec enable + 20 ms settle + clear codec SRAM (0x40009400..0x400095FF) */
    openfw_mmio_set32(CODEC_GLB_ADDR, 1u);
    mdelay(20u);

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

    /* 3. Program Audio PLL = 24.576 MHz & wait for lock */
    while (openfw_mmio_rd32(CRU_APLL_FRAC_ADDR) & (1u << 11)) {
    }
    openfw_mmio_wr32(CRU_APLL_CTRL_ADDR,
                     (openfw_mmio_rd32(CRU_APLL_CTRL_ADDR) & 0xFC1F0000u) | 0x3060u);
    openfw_mmio_wr32(CRU_APLL_FRAC_ADDR,
                     (openfw_mmio_rd32(CRU_APLL_FRAC_ADDR) & 0x00003800u) | 0x80000400u | (0x3126u << 14));
    openfw_mmio_set32(CRU_APLL_CTRL_ADDR, 0xC0000000u);
    while (!(openfw_mmio_rd32(CRU_APLL_CTRL_ADDR) & (1u << 28))) {
    }

    /* 4. PB12/PB13 = AMIC1 analog (mux 15); PB14/PB15 = AMIC2 analog when stereo */
    gpio_set_mux(GPIO_PORT_B, 12u, 15u);
    gpio_set_mux(GPIO_PORT_B, 13u, 15u);
    if (channels == 2u) {
        gpio_set_mux(GPIO_PORT_B, 14u, 15u);
        gpio_set_mux(GPIO_PORT_B, 15u, 15u);
    }

    /* 5. Analog core + AMIC1 (and optional AMIC2) differential PGA + MICBIAS0/1 */
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x80000022u);
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x40000000u);
    openfw_mmio_clr32(CODEC_ANA_MISC_ADDR, 0x1Fu);
    openfw_mmio_set32(CODEC_AMIC01_PGA_ADDR, 0x20430000u);
    if (channels == 2u) {
        openfw_mmio_set32(CODEC_AMIC2_CTRL_ADDR, 0x00010043u);
    } else {
        openfw_mmio_clr32(CODEC_AMIC2_CTRL_ADDR, 0x00010043u);
    }
    mdelay(2u);
    apply_pga_gain(gain_0_31, channels);
    openfw_mmio_set32(CODEC_ANA_CTRL0_ADDR, 0x03000000u);

    /* 6. ADC digital: 16 kHz (3), 16-bit + DMA req, AMIC1 (ch1) or AMIC1+AMIC2 (ch1+ch2), RUN */
    openfw_mmio_clr32(CODEC_ADC_DIG_CTRL_ADDR, 0x2u);
    openfw_mmio_set32(CODEC_ADC_DIG_CTRL_ADDR, 0x1u);
    for (uint32_t ch = 0u; ch < 4u; ch++) {
        openfw_mmio_clr32(CODEC_ADC_CH_GAIN_ADDR(ch), 0x1FFu);
    }
    openfw_mmio_rmw32(CODEC_ADC_DIG_CTRL_ADDR, 0xF00u, (3u << 8));
    openfw_mmio_set32(CODEC_ADC_FIFO_CTRL_ADDR, 0x84u); /* bit 2 = 16-bit, bit 7 = RX FIFO & DMA req EN */
    openfw_mmio_rmw32(CODEC_ADC_CH_MUX_ADDR, 0x000D0D0Du,
                      (channels == 2u) ? 0x000D0D00u : 0x00000D00u);
    openfw_mmio_clr32(CODEC_ADC_CH_CTRL_ADDR(0u), 0xBu);
    openfw_mmio_rmw32(CODEC_ADC_CH_CTRL_ADDR(1u), 0xBu, 0x9u); /* bit0=EN | bit3=FIFO_EN (Q16 verified) */
    if (channels == 2u) {
        openfw_mmio_rmw32(CODEC_ADC_CH_CTRL_ADDR(2u), 0xBu, 0x9u);
    } else {
        openfw_mmio_clr32(CODEC_ADC_CH_CTRL_ADDR(2u), 0xBu);
    }
    openfw_mmio_clr32(CODEC_ADC_CH_CTRL_ADDR(3u), 0xBu);
    openfw_mmio_set32(CODEC_ADC_DIG_CTRL_ADDR, 0x2u);
}

static void dmac_audio_rx_init(void)
{
    /* Release DMAC reset 0x21 (bank 1 bit 1) */
    openfw_mmio_set32(CRU_RST_SET1_ADDR, (1u << 1));
    openfw_mmio_set32(CRU_RST_STAT1_ADDR, (1u << 1));
    while (!(openfw_mmio_rd32(CRU_RST_STAT1_ADDR) & (1u << 1))) {
    }

    openfw_mmio_wr32(DMAC_CH_CTL_ADDR(AUDIO_DMA_CH), DMAC_CTL_ADC_RX_BASE);
    openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(AUDIO_DMA_CH),
                     openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(AUDIO_DMA_CH)));
    openfw_mmio_wr32(DMAC_CH_IRQ_EN_ADDR(AUDIO_DMA_CH), 0x00080001u); /* TC_IE=1 */

    nvic_clear_pending(DMAC_IRQ_BASE + AUDIO_DMA_CH);
    nvic_enable_irq(DMAC_IRQ_BASE + AUDIO_DMA_CH);
}

int audio_capture_start_ex(uint8_t gain_0_31, uint8_t channels)
{
    if (g_running) {
        return 0;
    }

    uint8_t ch        = (channels == 2u) ? 2u : 1u;
    g_channels        = ch;
    g_frame_samples   = AUDIO_FRAMES_PER_SLOT * (uint32_t)ch;
    g_frame_bytes     = g_frame_samples * (uint32_t)sizeof(int16_t);
    g_dma_slot        = 0u;
    g_peak_pct        = 0u;
    g_frames          = 0u;
    g_wr_idx          = 0u;
    g_rd_idx          = 0u;
    g_overruns        = 0u;

    audio_amic_16k_init(gain_0_31, ch);
    dmac_audio_rx_init();

    g_running = 1u;
    dmac_arm_rx(g_pcm_buf[0], g_frame_bytes);
    return 0;
}

int audio_capture_start(uint8_t gain_0_31)
{
    return audio_capture_start_ex(gain_0_31, 1u);
}

void audio_capture_set_gain(uint8_t gain_0_31)
{
    if (g_running) {
        apply_pga_gain(gain_0_31, g_channels);
    }
}

void audio_capture_stop(void)
{
    if (!g_running) {
        return;
    }
    g_running  = 0u;
    g_peak_pct = 0u;

    nvic_disable_irq(DMAC_IRQ_BASE + AUDIO_DMA_CH);
    openfw_mmio_clr32(DMAC_CH_CTL_ADDR(AUDIO_DMA_CH), DMAC_CTL_CH_EN);
    openfw_mmio_wr32(DMAC_CH_IRQ_EN_ADDR(AUDIO_DMA_CH), 0u);
    openfw_mmio_wr32(DMAC_CH_IRQ_STAT_ADDR(AUDIO_DMA_CH),
                     openfw_mmio_rd32(DMAC_CH_IRQ_STAT_ADDR(AUDIO_DMA_CH)));
    nvic_clear_pending(DMAC_IRQ_BASE + AUDIO_DMA_CH);

    openfw_mmio_clr32(CODEC_ADC_DIG_CTRL_ADDR, 0x3u);
    openfw_mmio_clr32(CODEC_ADC_FIFO_CTRL_ADDR, 0x80u);
    openfw_mmio_clr32(CODEC_ADC_CH_CTRL_ADDR(1u), 0x1u);
    openfw_mmio_clr32(CODEC_ADC_CH_CTRL_ADDR(2u), 0x1u);
    openfw_mmio_clr32(CODEC_AMIC2_CTRL_ADDR, 0x00010043u);
    openfw_mmio_clr32(CODEC_ANA_CTRL0_ADDR, 0x03000000u);
}

uint8_t audio_get_channels(void)
{
    return g_channels;
}

const int16_t *audio_pop_frame(uint32_t *out_bytes)
{
    uint32_t primask;
    OPENFW_IRQ_SAVE(primask);

    if (g_rd_idx == g_wr_idx) {
        OPENFW_IRQ_RESTORE(primask);
        return 0;
    }

    uint32_t slot = g_rd_idx & (AUDIO_RING_SLOTS - 1u);
    g_rd_idx++;

    OPENFW_IRQ_RESTORE(primask);
    OPENFW_DSB();

    if (out_bytes) {
        *out_bytes = g_frame_bytes;
    }
    return g_pcm_buf[slot];
}

uint8_t audio_get_peak_pct(void)
{
    return g_peak_pct;
}

uint32_t audio_get_frames(void)
{
    return g_frames;
}

uint32_t audio_get_overruns(void)
{
    return g_overruns;
}
