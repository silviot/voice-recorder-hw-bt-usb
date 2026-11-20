/*
 * Cassini OpenFW — Live 128-pt FFT Mel Spectrogram (144x24 RGB565)
 *
 * Freestanding Cortex-M4F single-precision implementation with zero libm
 * dependencies: precomputed Hann window, 64-entry radix-2 FFT twiddle table,
 * 24-row Mel bin range table, IEEE-754 fast log2 approximation, and 64-step
 * magma RGB565 colormap LUT.
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_ui.h"

/* 128-point Hann window: 0.5 * (1 - cos(2*pi*i / 127)) */
static const float k_hann[UI_SPEC_FFT_N] = {
    0.0000000f, 0.0006118f, 0.0024457f, 0.0054971f, 0.0097588f, 0.0152201f, 0.0218677f, 0.0296854f,
    0.0386541f, 0.0487517f, 0.0599537f, 0.0722325f, 0.0855581f, 0.0998979f, 0.1152168f, 0.1314774f,
    0.1486398f, 0.1666621f, 0.1855000f, 0.2051076f, 0.2254369f, 0.2464381f, 0.2680597f, 0.2902490f,
    0.3129516f, 0.3361119f, 0.3596732f, 0.3835780f, 0.4077677f, 0.4321831f, 0.4567644f, 0.4814515f,
    0.5061841f, 0.5309015f, 0.5555433f, 0.5800491f, 0.6043591f, 0.6284137f, 0.6521540f, 0.6755220f,
    0.6984604f, 0.7209132f, 0.7428254f, 0.7641433f, 0.7848149f, 0.8047894f, 0.8240181f, 0.8424538f,
    0.8600515f, 0.8767681f, 0.8925627f, 0.9073967f, 0.9212336f, 0.9340397f, 0.9457837f, 0.9564367f,
    0.9659728f, 0.9743686f, 0.9816035f, 0.9876598f, 0.9925227f, 0.9961804f, 0.9986238f, 0.9998470f,
    0.9998470f, 0.9986238f, 0.9961804f, 0.9925227f, 0.9876598f, 0.9816035f, 0.9743686f, 0.9659728f,
    0.9564367f, 0.9457837f, 0.9340397f, 0.9212336f, 0.9073967f, 0.8925627f, 0.8767681f, 0.8600515f,
    0.8424538f, 0.8240181f, 0.8047894f, 0.7848149f, 0.7641433f, 0.7428254f, 0.7209132f, 0.6984604f,
    0.6755220f, 0.6521540f, 0.6284137f, 0.6043591f, 0.5800491f, 0.5555433f, 0.5309015f, 0.5061841f,
    0.4814515f, 0.4567644f, 0.4321831f, 0.4077677f, 0.3835780f, 0.3596732f, 0.3361119f, 0.3129516f,
    0.2902490f, 0.2680597f, 0.2464381f, 0.2254369f, 0.2051076f, 0.1855000f, 0.1666621f, 0.1486398f,
    0.1314774f, 0.1152168f, 0.0998979f, 0.0855581f, 0.0722325f, 0.0599537f, 0.0487517f, 0.0386541f,
    0.0296854f, 0.0218677f, 0.0152201f, 0.0097588f, 0.0054971f, 0.0024457f, 0.0006118f, 0.0000000f,
};

/* 64-entry radix-2 FFT twiddle factors: W_128^k = cos(2*pi*k/128) - j*sin(2*pi*k/128) */
static const float k_twiddle_re[UI_SPEC_FFT_N / 2u] = {
    1.0000000f,  0.9987955f,  0.9951847f,  0.9891765f,  0.9807853f,  0.9700313f,  0.9569403f,  0.9415441f,
    0.9238795f,  0.9039893f,  0.8819213f,  0.8577286f,  0.8314696f,  0.8032075f,  0.7730105f,  0.7409511f,
    0.7071068f,  0.6715590f,  0.6343933f,  0.5956993f,  0.5555702f,  0.5141027f,  0.4713967f,  0.4275551f,
    0.3826834f,  0.3368899f,  0.2902847f,  0.2429802f,  0.1950903f,  0.1467305f,  0.0980171f,  0.0490677f,
    0.0000000f, -0.0490677f, -0.0980171f, -0.1467305f, -0.1950903f, -0.2429802f, -0.2902847f, -0.3368899f,
   -0.3826834f, -0.4275551f, -0.4713967f, -0.5141027f, -0.5555702f, -0.5956993f, -0.6343933f, -0.6715590f,
   -0.7071068f, -0.7409511f, -0.7730105f, -0.8032075f, -0.8314696f, -0.8577286f, -0.8819213f, -0.9039893f,
   -0.9238795f, -0.9415441f, -0.9569403f, -0.9700313f, -0.9807853f, -0.9891765f, -0.9951847f, -0.9987955f,
};

static const float k_twiddle_im[UI_SPEC_FFT_N / 2u] = {
    0.0000000f, -0.0490677f, -0.0980171f, -0.1467305f, -0.1950903f, -0.2429802f, -0.2902847f, -0.3368899f,
   -0.3826834f, -0.4275551f, -0.4713967f, -0.5141027f, -0.5555702f, -0.5956993f, -0.6343933f, -0.6715590f,
   -0.7071068f, -0.7409511f, -0.7730105f, -0.8032075f, -0.8314696f, -0.8577286f, -0.8819213f, -0.9039893f,
   -0.9238795f, -0.9415441f, -0.9569403f, -0.9700313f, -0.9807853f, -0.9891765f, -0.9951847f, -0.9987955f,
   -1.0000000f, -0.9987955f, -0.9951847f, -0.9891765f, -0.9807853f, -0.9700313f, -0.9569403f, -0.9415441f,
   -0.9238795f, -0.9039893f, -0.8819213f, -0.8577286f, -0.8314696f, -0.8032075f, -0.7730105f, -0.7409511f,
   -0.7071068f, -0.6715590f, -0.6343933f, -0.5956993f, -0.5555702f, -0.5141027f, -0.4713967f, -0.4275551f,
   -0.3826834f, -0.3368899f, -0.2902847f, -0.2429802f, -0.1950903f, -0.1467305f, -0.0980171f, -0.0490677f,
};

/* 7-bit reversal LUT for 128-point FFT */
static const uint8_t k_bitrev[UI_SPEC_FFT_N] = {
      0u,  64u,  32u,  96u,  16u,  80u,  48u, 112u,   8u,  72u,  40u, 104u,  24u,  88u,  56u, 120u,
      4u,  68u,  36u, 100u,  20u,  84u,  52u, 116u,  12u,  76u,  44u, 108u,  28u,  92u,  60u, 124u,
      2u,  66u,  34u,  98u,  18u,  82u,  50u, 114u,  10u,  74u,  42u, 106u,  26u,  90u,  58u, 122u,
      6u,  70u,  38u, 102u,  22u,  86u,  54u, 118u,  14u,  78u,  46u, 110u,  30u,  94u,  62u, 126u,
      1u,  65u,  33u,  97u,  17u,  81u,  49u, 113u,   9u,  73u,  41u, 105u,  25u,  89u,  57u, 121u,
      5u,  69u,  37u, 101u,  21u,  85u,  53u, 117u,  13u,  77u,  45u, 109u,  29u,  93u,  61u, 125u,
      3u,  67u,  35u,  99u,  19u,  83u,  51u, 115u,  11u,  75u,  43u, 107u,  27u,  91u,  59u, 123u,
      7u,  71u,  39u, 103u,  23u,  87u,  55u, 119u,  15u,  79u,  47u, 111u,  31u,  95u,  63u, 127u,
};

/*
 * 24 Mel-scale frequency bin ranges (inclusive) for 16 kHz / 128-pt FFT.
 * Row 0 = top (highest freq, ~7.2..8.0 kHz), Row 23 = bottom (lowest freq, ~125..210 Hz).
 */
typedef struct {
    uint8_t lo;
    uint8_t hi;
} mel_bin_range_t;

static const mel_bin_range_t k_mel_bins[UI_SPEC_H] = {
    { 57u, 63u }, /* row  0: 7186.6..8000.0 Hz */
    { 52u, 57u }, /* row  1: 6449.3..7186.6 Hz */
    { 46u, 52u }, /* row  2: 5780.9..6449.3 Hz */
    { 41u, 46u }, /* row  3: 5175.0..5780.9 Hz */
    { 37u, 41u }, /* row  4: 4625.7..5175.0 Hz */
    { 33u, 37u }, /* row  5: 4127.8..4625.7 Hz */
    { 29u, 33u }, /* row  6: 3676.5..4127.8 Hz */
    { 26u, 29u }, /* row  7: 3267.3..3676.5 Hz */
    { 23u, 26u }, /* row  8: 2896.4..3267.3 Hz */
    { 20u, 23u }, /* row  9: 2560.2..2896.4 Hz */
    { 18u, 20u }, /* row 10: 2255.4..2560.2 Hz */
    { 16u, 18u }, /* row 11: 1979.1..2255.4 Hz */
    { 14u, 16u }, /* row 12: 1728.6..1979.1 Hz */
    { 12u, 14u }, /* row 13: 1501.6..1728.6 Hz */
    { 10u, 12u }, /* row 14: 1295.7..1501.6 Hz */
    {  9u, 10u }, /* row 15: 1109.2..1295.7 Hz */
    {  8u,  9u }, /* row 16:  940.0..1109.2 Hz */
    {  6u,  8u }, /* row 17:  786.7.. 940.0 Hz */
    {  5u,  6u }, /* row 18:  647.7.. 786.7 Hz */
    {  4u,  5u }, /* row 19:  521.7.. 647.7 Hz */
    {  3u,  4u }, /* row 20:  407.5.. 521.7 Hz */
    {  2u,  3u }, /* row 21:  303.9.. 407.5 Hz */
    {  2u,  2u }, /* row 22:  210.1.. 303.9 Hz */
    {  1u,  1u }, /* row 23:  125.0.. 210.1 Hz */
};

/*
 * 64-step Magma RGB565 LUT interpolated across the 8 stops:
 * [0,0,4], [40,11,84], [101,21,110], [159,42,99],
 * [212,72,66], [245,125,21], [250,193,39], [252,255,164]
 */
static const uint16_t k_magma_lut[64] = {
    0x0000u, 0x0001u, 0x0802u, 0x0823u, 0x1025u, 0x1026u, 0x1827u, 0x1848u,
    0x2049u, 0x284Au, 0x286Au, 0x306Bu, 0x386Bu, 0x406Cu, 0x488Cu, 0x508Cu,
    0x508Du, 0x58ADu, 0x60ADu, 0x68ADu, 0x70CDu, 0x78EDu, 0x78EDu, 0x810Du,
    0x890Cu, 0x912Cu, 0x994Cu, 0x994Cu, 0xA16Bu, 0xA98Bu, 0xB1ABu, 0xB1AAu,
    0xB9CAu, 0xC1E9u, 0xCA09u, 0xCA28u, 0xD248u, 0xDA67u, 0xDAA7u, 0xDAC6u,
    0xE305u, 0xE325u, 0xEB44u, 0xEB83u, 0xF3A3u, 0xF3E2u, 0xF422u, 0xF463u,
    0xF4A3u, 0xF4C3u, 0xFD03u, 0xFD44u, 0xFD84u, 0xFDC4u, 0xFE04u, 0xFE46u,
    0xFE68u, 0xFEAAu, 0xFEEBu, 0xFF0Du, 0xFF4Fu, 0xFF91u, 0xFFD2u, 0xFFF4u,
};

/* Fast IEEE-754 single-precision log2 approximation (x > 0, no libm) */
static float fast_log2f(float x)
{
    union {
        float    f;
        uint32_t u;
    } v;
    v.f = x;
    int32_t exp = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    float   m   = (float)(v.u & 0x007FFFFFu) * (1.0f / 8388608.0f);
    return (float)exp + m * (1.3333333f - 0.3333333f * m);
}

/* In-place 128-point radix-2 Cooley-Tukey FFT */
static void fft128_f32(float *re, float *im)
{
    uint32_t i;

    for (i = 0u; i < UI_SPEC_FFT_N; i++) {
        uint32_t j = k_bitrev[i];
        if (i < j) {
            float tr = re[i];
            float ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }

    for (uint32_t half = 1u, step = UI_SPEC_FFT_N >> 1;
         half < UI_SPEC_FFT_N;
         half <<= 1, step >>= 1) {
        uint32_t len = half << 1;
        for (uint32_t base = 0u; base < UI_SPEC_FFT_N; base += len) {
            for (uint32_t k = 0u; k < half; k++) {
                uint32_t tw_idx = k * step;
                float    wr     = k_twiddle_re[tw_idx];
                float    wi     = k_twiddle_im[tw_idx];
                uint32_t u      = base + k;
                uint32_t v      = u + half;
                float    tr     = wr * re[v] - wi * im[v];
                float    ti     = wr * im[v] + wi * re[v];
                re[v] = re[u] - tr;
                im[v] = im[u] - ti;
                re[u] = re[u] + tr;
                im[u] = im[u] + ti;
            }
        }
    }
}

void ui_spec_reset(ui_spectrogram_t *sp)
{
    uint32_t r, c;

    if (!sp) {
        return;
    }

    for (r = 0u; r < UI_SPEC_H; r++) {
        for (c = 0u; c < UI_SPEC_W; c++) {
            sp->pixels[r][c] = k_magma_lut[0];
        }
    }
    for (c = 0u; c < UI_SPEC_W; c++) {
        sp->markers[c] = 0u;
    }
    sp->col_count = 0u;
}

void ui_spec_push_pcm(ui_spectrogram_t *sp, const int16_t *pcm, uint32_t frames,
                      uint8_t channels, int vor_paused)
{
    float    re[UI_SPEC_FFT_N];
    float    im[UI_SPEC_FFT_N];
    float    mag2[UI_SPEC_FFT_N / 2u];
    uint32_t i, r, c, n;

    if (!sp) {
        return;
    }

    /* Scroll spectrogram left by 1 pixel */
    for (r = 0u; r < UI_SPEC_H; r++) {
        for (c = 0u; c < UI_SPEC_W - 1u; c++) {
            sp->pixels[r][c] = sp->pixels[r][c + 1u];
        }
    }
    for (c = 0u; c < UI_SPEC_W - 1u; c++) {
        sp->markers[c] = sp->markers[c + 1u];
    }
    sp->markers[UI_SPEC_W - 1u] = 0u;

    /* Extract up to 128 mono samples, normalize to [-1, 1], apply Hann window */
    n = (pcm != (const int16_t *)0 && frames > 0u)
            ? ((frames > UI_SPEC_FFT_N) ? UI_SPEC_FFT_N : frames)
            : 0u;

    for (i = 0u; i < n; i++) {
        float s;
        if (channels >= 2u) {
            s = 0.5f * ((float)pcm[2u * i] + (float)pcm[2u * i + 1u]);
        } else {
            s = (float)pcm[i];
        }
        re[i] = s * (1.0f / 32768.0f) * k_hann[i];
        im[i] = 0.0f;
    }
    for (; i < UI_SPEC_FFT_N; i++) {
        re[i] = 0.0f;
        im[i] = 0.0f;
    }

    /* 128-pt radix-2 FFT and 64 magnitude-squared bins */
    fft128_f32(re, im);
    for (i = 0u; i < UI_SPEC_FFT_N / 2u; i++) {
        mag2[i] = re[i] * re[i] + im[i] * im[i];
    }

    /* Map 64 bins into 24 Mel rows and look up magma RGB565 */
    for (r = 0u; r < UI_SPEC_H; r++) {
        uint32_t lo    = k_mel_bins[r].lo;
        uint32_t hi    = k_mel_bins[r].hi;
        float    sum   = 0.0f;
        uint32_t count = hi - lo + 1u;
        uint32_t idx;
        uint16_t col;

        for (i = lo; i <= hi; i++) {
            sum += mag2[i];
        }
        float avg = sum / (float)count;

        if (avg <= 1e-10f) {
            idx = 0u;
        } else {
            /* Full-scale Hann-windowed sine => mag2 ~= 1024 (30.103 dB); map [-64..0] dBFS to [0..63] */
            float db = 3.0103f * fast_log2f(avg) - 30.103f;
            int32_t scaled = (int32_t)(db + 64.0f);
            if (scaled < 0) {
                idx = 0u;
            } else if (scaled > 63) {
                idx = 63u;
            } else {
                idx = (uint32_t)scaled;
            }
        }

        col = k_magma_lut[idx];
        if (vor_paused) {
            col = (uint16_t)((col >> 1) & 0x7BEFu);
        }
        sp->pixels[r][UI_SPEC_W - 1u] = col;
    }

    if (sp->col_count < 0xFFFFu) {
        sp->col_count++;
    }
}

void ui_spec_drop_marker(ui_spectrogram_t *sp)
{
    if (!sp) {
        return;
    }
    sp->markers[UI_SPEC_W - 1u] = 1u;
}

void ui_spec_blit(const ui_spectrogram_t *sp, int16_t x, int16_t y)
{
    uint32_t col;

    if (!sp) {
        return;
    }

    lcd_blit_rgb565((uint16_t)x, (uint16_t)y, UI_SPEC_W, UI_SPEC_H, &sp->pixels[0][0]);

    for (col = 0u; col < UI_SPEC_W; col++) {
        int16_t mx, x0, x1;

        if (!sp->markers[col]) {
            continue;
        }

        mx = (int16_t)(x + (int16_t)col);
        /* 1x24 vertical cyan marker line */
        lcd_fill_rect((uint16_t)mx, (uint16_t)y, 1u, UI_SPEC_H, UI_COL_CYAN);

        /* Top downward triangle notch: row y (width 5), row y+1 (width 3), row y+2 (width 1) */
        x0 = (int16_t)(mx - 2);
        x1 = (int16_t)(mx + 2);
        if (x0 < x) {
            x0 = x;
        }
        if (x1 >= (int16_t)(x + (int16_t)UI_SPEC_W)) {
            x1 = (int16_t)(x + (int16_t)UI_SPEC_W - 1);
        }
        if (x1 >= x0) {
            lcd_fill_rect((uint16_t)x0, (uint16_t)y, (uint16_t)(x1 - x0 + 1), 1u, UI_COL_CYAN);
        }

        x0 = (int16_t)(mx - 1);
        x1 = (int16_t)(mx + 1);
        if (x0 < x) {
            x0 = x;
        }
        if (x1 >= (int16_t)(x + (int16_t)UI_SPEC_W)) {
            x1 = (int16_t)(x + (int16_t)UI_SPEC_W - 1);
        }
        if (x1 >= x0) {
            lcd_fill_rect((uint16_t)x0, (uint16_t)(y + 1), (uint16_t)(x1 - x0 + 1), 1u, UI_COL_CYAN);
        }
    }
}
