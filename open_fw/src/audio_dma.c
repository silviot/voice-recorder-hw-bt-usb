/*
 * Smartlink SL6801 Bare-Metal Audio ADC (AMIC1, 16 kHz / 16-bit) + DMAC CH1 Driver
 *
 * Configures Audio PLL (24.576 MHz), AMIC1 differential PGA on PB12/PB13,
 * 16 kHz mono 16-bit ADC capture, and DMAC Channel 1 (NVIC IRQ 61) ping-pong
 * single-shot transfers for real-time audio telemetry and recording.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

#define AUDIO_DMA_CH        1u
#define AUDIO_FRAME_SAMPLES 2048u /* 128 ms @ 16 kHz mono 16-bit */
#define AUDIO_FRAME_BYTES   (AUDIO_FRAME_SAMPLES * sizeof(int16_t))

static int16_t g_pcm_buf[2][AUDIO_FRAME_SAMPLES] __attribute__((aligned(4)));
static volatile uint8_t  g_active_buf = 0u;
static volatile uint8_t  g_running    = 0u;
static volatile uint8_t  g_peak_pct   = 0u;
static volatile uint32_t g_frames     = 0u;

static inline void dmac_arm_rx(void *dst_buf, uint32_t len_bytes)
{
    DMAC_CH_SRC(AUDIO_DMA_CH)      = CODEC_ADC_RX_FIFO_ADDR;
    DMAC_CH_DST(AUDIO_DMA_CH)      = (uint32_t)(uintptr_t)dst_buf;
    DMAC_CH_XFER_CNT(AUDIO_DMA_CH) = 0xFF000000u | (len_bytes & 0x3FFFFu);
    DMAC_CH_CTL(AUDIO_DMA_CH)      = (DMAC_CH_CTL(AUDIO_DMA_CH) & ~DMAC_CTL_RELOAD)
                                   | DMAC_CTL_CH_EN;
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
    uint32_t st = DMAC_CH_IRQ_STAT(AUDIO_DMA_CH);
    DMAC_CH_IRQ_STAT(AUDIO_DMA_CH) = st;

    if (!g_running) {
        return;
    }

    if (st & DMAC_IRQ_TC) {
        uint8_t done_idx = g_active_buf;
        uint8_t next_idx = (uint8_t)(done_idx ^ 1u);
        g_active_buf = next_idx;
        dmac_arm_rx(g_pcm_buf[next_idx], AUDIO_FRAME_BYTES);

        g_peak_pct = compute_ac_peak_pct(g_pcm_buf[done_idx], AUDIO_FRAME_SAMPLES);
        g_frames++;
    }
}

static void audio_amic1_16k_init(uint8_t gain_0_31)
{
    if (gain_0_31 > 31u) {
        gain_0_31 = 31u;
    }

    /* 1. CRU: release codec reset 0x25 and enable codec clock 0x13 */
    CRU_RST_SET1  |= (1u << 5);
    CRU_RST_STAT1 |= (1u << 5);
    while (!(CRU_RST_STAT1 & (1u << 5))) {
    }
    CRU_CLK_EN_AUDIO |= 1u;

    /* 2. Global codec enable + 20 ms settle + clear codec SRAM (0x40009400..0x400095FF) */
    CODEC_GLB |= 1u;
    mdelay(20u);

    CODEC_DAC0_VOL = (CODEC_DAC0_VOL & ~0x1FFu) | 0x1FFu;
    CODEC_DAC1_VOL = (CODEC_DAC1_VOL & ~0x1FFu) | 0x1FFu;
    CODEC_DAC2_VOL = (CODEC_DAC2_VOL & ~0x1FFu) | 0x1FFu;

    CRU_RST_SET1  |= 1u;
    CRU_RST_STAT1 |= 1u;
    while (!(CRU_RST_STAT1 & 1u)) {
    }
    SYSCTL_CODEC_SRAM |= (1u << 2);
    for (uint32_t a = CODEC_SRAM_START; a < CODEC_SRAM_END; a += 4u) {
        REG32(a) = 0u;
    }
    SYSCTL_CODEC_SRAM &= ~(1u << 2);
    CRU_RST_STAT1 &= ~1u;
    CRU_RST_SET1  &= ~1u;

    CODEC_GLB = (CODEC_GLB & ~0x000FF000u) | 0x00055000u;
    CODEC_DAC0_MUTE &= ~(1u << 16);
    CODEC_DAC1_MUTE &= ~(1u << 16);
    CODEC_DAC2_MUTE &= ~(1u << 16);

    /* 3. Program Audio PLL = 24.576 MHz & wait for lock */
    while (CRU_APLL_FRAC & (1u << 11)) {
    }
    CRU_APLL_CTRL = (CRU_APLL_CTRL & 0xFC1F0000u) | 0x3060u;
    CRU_APLL_FRAC = (CRU_APLL_FRAC & 0x00003800u) | 0x80000400u | (0x3126u << 14);
    CRU_APLL_CTRL |= 0xC0000000u;
    while (!(CRU_APLL_CTRL & (1u << 28))) {
    }

    /* 4. PB12 & PB13 = Analog (mux 15) for differential AMIC1 input */
    gpio_set_mux(GPIO_PORT_B, 12u, 15u);
    gpio_set_mux(GPIO_PORT_B, 13u, 15u);

    /* 5. Analog core + AMIC1 differential PGA + MICBIAS0/1 */
    CODEC_ANA_CTRL0 |= 0x80000022u;
    CODEC_ANA_CTRL0 |= 0x40000000u;
    CODEC_ANA_MISC  &= ~0x1Fu;
    CODEC_AMIC01_PGA |= 0x20430000u;
    mdelay(2u);
    CODEC_AMIC01_PGA = (CODEC_AMIC01_PGA & ~0x1F000000u)
                     | (((uint32_t)gain_0_31 & 0x1Fu) << 24);
    CODEC_ANA_CTRL0 |= 0x03000000u;

    /* 6. ADC digital: 16 kHz (3), 16-bit + DMA req, AMIC1 (ch1) only, RUN */
    CODEC_ADC_DIG_CTRL &= ~0x2u;
    CODEC_ADC_DIG_CTRL |=  0x1u;
    for (uint32_t ch = 0u; ch < 4u; ch++) {
        CODEC_ADC_CH_GAIN(ch) &= ~0x1FFu;
    }
    CODEC_ADC_DIG_CTRL   = (CODEC_ADC_DIG_CTRL & ~0xF00u) | (3u << 8);
    CODEC_ADC_FIFO_CTRL |= 0x84u; /* bit 2 = 16-bit, bit 7 = RX FIFO & DMA req EN */
    CODEC_ADC_CH_MUX     = (CODEC_ADC_CH_MUX & ~0x000D0D0Du) | 0x00000D00u;
    CODEC_ADC_CH_CTRL(0u) &= ~0xBu;
    CODEC_ADC_CH_CTRL(1u)  = (CODEC_ADC_CH_CTRL(1u) & ~0x8u) | 0x1u;
    CODEC_ADC_CH_CTRL(2u) &= ~0xBu;
    CODEC_ADC_CH_CTRL(3u) &= ~0xBu;
    CODEC_ADC_DIG_CTRL    |= 0x2u;
}

static void dmac_audio_rx_init(void)
{
    /* Release DMAC reset 0x21 (bank 1 bit 1) */
    CRU_RST_SET1  |= (1u << 1);
    CRU_RST_STAT1 |= (1u << 1);
    while (!(CRU_RST_STAT1 & (1u << 1))) {
    }

    DMAC_CH_CTL(AUDIO_DMA_CH)      = DMAC_CTL_ADC_RX_BASE;
    DMAC_CH_IRQ_STAT(AUDIO_DMA_CH) = DMAC_CH_IRQ_STAT(AUDIO_DMA_CH);
    DMAC_CH_IRQ_EN(AUDIO_DMA_CH)   = 0x00080001u; /* TC_IE=1 */

    nvic_clear_pending(DMAC_IRQ_BASE + AUDIO_DMA_CH);
    nvic_enable_irq(DMAC_IRQ_BASE + AUDIO_DMA_CH);
}

int audio_capture_start(uint8_t gain_0_31)
{
    if (g_running) {
        return 0;
    }

    g_active_buf = 0u;
    g_peak_pct   = 0u;
    g_frames     = 0u;

    audio_amic1_16k_init(gain_0_31);
    dmac_audio_rx_init();

    g_running = 1u;
    dmac_arm_rx(g_pcm_buf[0], AUDIO_FRAME_BYTES);
    return 0;
}

void audio_capture_stop(void)
{
    if (!g_running) {
        return;
    }
    g_running  = 0u;
    g_peak_pct = 0u;

    nvic_disable_irq(DMAC_IRQ_BASE + AUDIO_DMA_CH);
    DMAC_CH_CTL(AUDIO_DMA_CH)     &= ~DMAC_CTL_CH_EN;
    DMAC_CH_IRQ_EN(AUDIO_DMA_CH)   = 0u;
    DMAC_CH_IRQ_STAT(AUDIO_DMA_CH) = DMAC_CH_IRQ_STAT(AUDIO_DMA_CH);
    nvic_clear_pending(DMAC_IRQ_BASE + AUDIO_DMA_CH);

    CODEC_ADC_DIG_CTRL    &= ~0x3u;
    CODEC_ADC_FIFO_CTRL   &= ~0x80u;
    CODEC_ADC_CH_CTRL(1u) &= ~0x1u;
    CODEC_ANA_CTRL0       &= ~0x03000000u;
}

uint8_t audio_get_peak_pct(void)
{
    return g_peak_pct;
}

uint32_t audio_get_frames(void)
{
    return g_frames;
}
