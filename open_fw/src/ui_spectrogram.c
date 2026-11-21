/*
 * Cassini OpenFW — Live 128-pt Real FFT Mel Spectrogram (144x24 RGB565)
 *
 * Freestanding Cortex-M4F single-precision implementation with zero libm
 * dependencies: DC-removed Hann window (pre-scaled by 1/32768), 64-pt complex
 * FFT with real-FFT unpack, pre-baked Mel bin inverse counts, IEEE-754 fast
 * log2 approximation, 64-step magma RGB565 colormap LUT, and in-buffer
 * scrolling cyan markers visible directly to LVGL.
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_ui.h"

#define SPEC_FFT_HALF (UI_SPEC_FFT_N / 2u) /* 64 */
#define HANN_SCALE(v) ((v) * (1.0f / 32768.0f))

/* 128-point Hann window pre-scaled by 1/32768: (0.5/32768) * (1 - cos(2*pi*i / 127)) */
static const float k_hann_scaled[UI_SPEC_FFT_N] = {
    HANN_SCALE(0.0000000f), HANN_SCALE(0.0006118f), HANN_SCALE(0.0024457f), HANN_SCALE(0.0054971f),
    HANN_SCALE(0.0097588f), HANN_SCALE(0.0152201f), HANN_SCALE(0.0218677f), HANN_SCALE(0.0296854f),
    HANN_SCALE(0.0386541f), HANN_SCALE(0.0487517f), HANN_SCALE(0.0599537f), HANN_SCALE(0.0722325f),
    HANN_SCALE(0.0855581f), HANN_SCALE(0.0998979f), HANN_SCALE(0.1152168f), HANN_SCALE(0.1314774f),
    HANN_SCALE(0.1486398f), HANN_SCALE(0.1666621f), HANN_SCALE(0.1855000f), HANN_SCALE(0.2051076f),
    HANN_SCALE(0.2254369f), HANN_SCALE(0.2464381f), HANN_SCALE(0.2680597f), HANN_SCALE(0.2902490f),
    HANN_SCALE(0.3129516f), HANN_SCALE(0.3361119f), HANN_SCALE(0.3596732f), HANN_SCALE(0.3835780f),
    HANN_SCALE(0.4077677f), HANN_SCALE(0.4321831f), HANN_SCALE(0.4567644f), HANN_SCALE(0.4814515f),
    HANN_SCALE(0.5061841f), HANN_SCALE(0.5309015f), HANN_SCALE(0.5555433f), HANN_SCALE(0.5800491f),
    HANN_SCALE(0.6043591f), HANN_SCALE(0.6284137f), HANN_SCALE(0.6521540f), HANN_SCALE(0.6755220f),
    HANN_SCALE(0.6984604f), HANN_SCALE(0.7209132f), HANN_SCALE(0.7428254f), HANN_SCALE(0.7641433f),
    HANN_SCALE(0.7848149f), HANN_SCALE(0.8047894f), HANN_SCALE(0.8240181f), HANN_SCALE(0.8424538f),
    HANN_SCALE(0.8600515f), HANN_SCALE(0.8767681f), HANN_SCALE(0.8925627f), HANN_SCALE(0.9073967f),
    HANN_SCALE(0.9212336f), HANN_SCALE(0.9340397f), HANN_SCALE(0.9457837f), HANN_SCALE(0.9564367f),
    HANN_SCALE(0.9659728f), HANN_SCALE(0.9743686f), HANN_SCALE(0.9816035f), HANN_SCALE(0.9876598f),
    HANN_SCALE(0.9925227f), HANN_SCALE(0.9961804f), HANN_SCALE(0.9986238f), HANN_SCALE(0.9998470f),
    HANN_SCALE(0.9998470f), HANN_SCALE(0.9986238f), HANN_SCALE(0.9961804f), HANN_SCALE(0.9925227f),
    HANN_SCALE(0.9876598f), HANN_SCALE(0.9816035f), HANN_SCALE(0.9743686f), HANN_SCALE(0.9659728f),
    HANN_SCALE(0.9564367f), HANN_SCALE(0.9457837f), HANN_SCALE(0.9340397f), HANN_SCALE(0.9212336f),
    HANN_SCALE(0.9073967f), HANN_SCALE(0.8925627f), HANN_SCALE(0.8767681f), HANN_SCALE(0.8600515f),
    HANN_SCALE(0.8424538f), HANN_SCALE(0.8240181f), HANN_SCALE(0.8047894f), HANN_SCALE(0.7848149f),
    HANN_SCALE(0.7641433f), HANN_SCALE(0.7428254f), HANN_SCALE(0.7209132f), HANN_SCALE(0.6984604f),
    HANN_SCALE(0.6755220f), HANN_SCALE(0.6521540f), HANN_SCALE(0.6284137f), HANN_SCALE(0.6043591f),
    HANN_SCALE(0.5800491f), HANN_SCALE(0.5555433f), HANN_SCALE(0.5309015f), HANN_SCALE(0.5061841f),
    HANN_SCALE(0.4814515f), HANN_SCALE(0.4567644f), HANN_SCALE(0.4321831f), HANN_SCALE(0.4077677f),
    HANN_SCALE(0.3835780f), HANN_SCALE(0.3596732f), HANN_SCALE(0.3361119f), HANN_SCALE(0.3129516f),
    HANN_SCALE(0.2902490f), HANN_SCALE(0.2680597f), HANN_SCALE(0.2464381f), HANN_SCALE(0.2254369f),
    HANN_SCALE(0.2051076f), HANN_SCALE(0.1855000f), HANN_SCALE(0.1666621f), HANN_SCALE(0.1486398f),
    HANN_SCALE(0.1314774f), HANN_SCALE(0.1152168f), HANN_SCALE(0.0998979f), HANN_SCALE(0.0855581f),
    HANN_SCALE(0.0722325f), HANN_SCALE(0.0599537f), HANN_SCALE(0.0487517f), HANN_SCALE(0.0386541f),
    HANN_SCALE(0.0296854f), HANN_SCALE(0.0218677f), HANN_SCALE(0.0152201f), HANN_SCALE(0.0097588f),
    HANN_SCALE(0.0054971f), HANN_SCALE(0.0024457f), HANN_SCALE(0.0006118f), HANN_SCALE(0.0000000f),
};

/* 64-entry radix-2 FFT twiddle factors: W_128^k = cos(2*pi*k/128) - j*sin(2*pi*k/128).
 * Even indices W_128^(2k) = W_64^k drive the 64-pt complex FFT; indices 1..31 unpack real FFT. */
static const float k_twiddle_re[SPEC_FFT_HALF] = {
    1.0000000f,  0.9987955f,  0.9951847f,  0.9891765f,  0.9807853f,  0.9700313f,  0.9569403f,  0.9415441f,
    0.9238795f,  0.9039893f,  0.8819213f,  0.8577286f,  0.8314696f,  0.8032075f,  0.7730105f,  0.7409511f,
    0.7071068f,  0.6715590f,  0.6343933f,  0.5956993f,  0.5555702f,  0.5141027f,  0.4713967f,  0.4275551f,
    0.3826834f,  0.3368899f,  0.2902847f,  0.2429802f,  0.1950903f,  0.1467305f,  0.0980171f,  0.0490677f,
    0.0000000f, -0.0490677f, -0.0980171f, -0.1467305f, -0.1950903f, -0.2429802f, -0.2902847f, -0.3368899f,
   -0.3826834f, -0.4275551f, -0.4713967f, -0.5141027f, -0.5555702f, -0.5956993f, -0.6343933f, -0.6715590f,
   -0.7071068f, -0.7409511f, -0.7730105f, -0.8032075f, -0.8314696f, -0.8577286f, -0.8819213f, -0.9039893f,
   -0.9238795f, -0.9415441f, -0.9569403f, -0.9700313f, -0.9807853f, -0.9891765f, -0.9951847f, -0.9987955f,
};

static const float k_twiddle_im[SPEC_FFT_HALF] = {
    0.0000000f, -0.0490677f, -0.0980171f, -0.1467305f, -0.1950903f, -0.2429802f, -0.2902847f, -0.3368899f,
   -0.3826834f, -0.4275551f, -0.4713967f, -0.5141027f, -0.5555702f, -0.5956993f, -0.6343933f, -0.6715590f,
   -0.7071068f, -0.7409511f, -0.7730105f, -0.8032075f, -0.8314696f, -0.8577286f, -0.8819213f, -0.9039893f,
   -0.9238795f, -0.9415441f, -0.9569403f, -0.9700313f, -0.9807853f, -0.9891765f, -0.9951847f, -0.9987955f,
   -1.0000000f, -0.9987955f, -0.9951847f, -0.9891765f, -0.9807853f, -0.9700313f, -0.9569403f, -0.9415441f,
   -0.9238795f, -0.9039893f, -0.8819213f, -0.8577286f, -0.8314696f, -0.8032075f, -0.7730105f, -0.7409511f,
   -0.7071068f, -0.6715590f, -0.6343933f, -0.5956993f, -0.5555702f, -0.5141027f, -0.4713967f, -0.4275551f,
   -0.3826834f, -0.3368899f, -0.2902847f, -0.2429802f, -0.1950903f, -0.1467305f, -0.0980171f, -0.0490677f,
};

/* 6-bit reversal LUT for 64-point complex FFT */
static const uint8_t k_bitrev64[SPEC_FFT_HALF] = {
     0u, 32u, 16u, 48u,  8u, 40u, 24u, 56u,
     4u, 36u, 20u, 52u, 12u, 44u, 28u, 60u,
     2u, 34u, 18u, 50u, 10u, 42u, 26u, 58u,
     6u, 38u, 22u, 54u, 14u, 46u, 30u, 62u,
     1u, 33u, 17u, 49u,  9u, 41u, 25u, 57u,
     5u, 37u, 21u, 53u, 13u, 45u, 29u, 61u,
     3u, 35u, 19u, 51u, 11u, 43u, 27u, 59u,
     7u, 39u, 23u, 55u, 15u, 47u, 31u, 63u,
};

/*
 * 24 Mel-scale frequency bin ranges (inclusive) for 16 kHz / 128-pt FFT,
 * with pre-baked 1.0f / count to avoid per-row float division.
 * Row 0 = top (highest freq, ~7.2..8.0 kHz), Row 23 = bottom (lowest freq, ~125..210 Hz).
 */
typedef struct {
    uint8_t lo;
    uint8_t hi;
    float   inv_count;
} mel_bin_range_t;

static const mel_bin_range_t k_mel_bins[UI_SPEC_H] = {
    { 57u, 63u, 1.0f / 7.0f }, /* row  0: 7186.6..8000.0 Hz */
    { 52u, 57u, 1.0f / 6.0f }, /* row  1: 6449.3..7186.6 Hz */
    { 46u, 52u, 1.0f / 7.0f }, /* row  2: 5780.9..6449.3 Hz */
    { 41u, 46u, 1.0f / 6.0f }, /* row  3: 5175.0..5780.9 Hz */
    { 37u, 41u, 1.0f / 5.0f }, /* row  4: 4625.7..5175.0 Hz */
    { 33u, 37u, 1.0f / 5.0f }, /* row  5: 4127.8..4625.7 Hz */
    { 29u, 33u, 1.0f / 5.0f }, /* row  6: 3676.5..4127.8 Hz */
    { 26u, 29u, 1.0f / 4.0f }, /* row  7: 3267.3..3676.5 Hz */
    { 23u, 26u, 1.0f / 4.0f }, /* row  8: 2896.4..3267.3 Hz */
    { 20u, 23u, 1.0f / 4.0f }, /* row  9: 2560.2..2896.4 Hz */
    { 18u, 20u, 1.0f / 3.0f }, /* row 10: 2255.4..2560.2 Hz */
    { 16u, 18u, 1.0f / 3.0f }, /* row 11: 1979.1..2255.4 Hz */
    { 14u, 16u, 1.0f / 3.0f }, /* row 12: 1728.6..1979.1 Hz */
    { 12u, 14u, 1.0f / 3.0f }, /* row 13: 1501.6..1728.6 Hz */
    { 10u, 12u, 1.0f / 3.0f }, /* row 14: 1295.7..1501.6 Hz */
    {  9u, 10u, 1.0f / 2.0f }, /* row 15: 1109.2..1295.7 Hz */
    {  8u,  9u, 1.0f / 2.0f }, /* row 16:  940.0..1109.2 Hz */
    {  6u,  8u, 1.0f / 3.0f }, /* row 17:  786.7.. 940.0 Hz */
    {  5u,  6u, 1.0f / 2.0f }, /* row 18:  647.7.. 786.7 Hz */
    {  4u,  5u, 1.0f / 2.0f }, /* row 19:  521.7.. 647.7 Hz */
    {  3u,  4u, 1.0f / 2.0f }, /* row 20:  407.5.. 521.7 Hz */
    {  2u,  3u, 1.0f / 2.0f }, /* row 21:  303.9.. 407.5 Hz */
    {  2u,  2u, 1.0f        }, /* row 22:  210.1.. 303.9 Hz */
    {  1u,  1u, 1.0f        }, /* row 23:  125.0.. 210.1 Hz */
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

/*
 * In-place 128-point real FFT magnitude-squared via a 64-point complex FFT.
 * Input:  re[k] = x[2k], im[k] = x[2k + 1] for k = 0..63.
 * Output: re[k] = |X[k]|^2 for k = 0..63 (reusing re[] for mag2[]).
 */
static void rfft128_mag2_f32(float *re, float *im)
{
    /* 1. 6-bit reversal permutation */
    for (uint32_t i = 0u; i < SPEC_FFT_HALF; i++) {
        uint32_t j = k_bitrev64[i];
        if (i < j) {
            float tr = re[i];
            float ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }

    /* 2. Stage 1 (half=1, len=2): twiddle W=1 (multiply-free) */
    for (uint32_t base = 0u; base < SPEC_FFT_HALF; base += 2u) {
        float ur = re[base],      ui = im[base];
        float vr = re[base + 1u], vi = im[base + 1u];
        re[base]      = ur + vr;
        im[base]      = ui + vi;
        re[base + 1u] = ur - vr;
        im[base + 1u] = ui - vi;
    }

    /* 3. Stage 2 (half=2, len=4): twiddles W=1 and W=-j (multiply-free) */
    for (uint32_t base = 0u; base < SPEC_FFT_HALF; base += 4u) {
        float ur = re[base],      ui = im[base];
        float vr = re[base + 2u], vi = im[base + 2u];
        re[base]      = ur + vr;
        im[base]      = ui + vi;
        re[base + 2u] = ur - vr;
        im[base + 2u] = ui - vi;

        ur = re[base + 1u]; ui = im[base + 1u];
        /* Multiply by -j: (vr + j*vi) * (-j) = vi - j*vr */
        vr = im[base + 3u]; vi = -re[base + 3u];
        re[base + 1u] = ur + vr;
        im[base + 1u] = ui + vi;
        re[base + 3u] = ur - vr;
        im[base + 3u] = ui - vi;
    }

    /* 4. Stages 3..6 (half=4, 8, 16, 32): W_64^(k*(32/half)) = W_128^(k*(64/half)) */
    for (uint32_t half = 4u, step = 16u; half < SPEC_FFT_HALF; half <<= 1, step >>= 1) {
        uint32_t len = half << 1;
        for (uint32_t base = 0u; base < SPEC_FFT_HALF; base += len) {
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

    /* 5. Unpack 128-pt real spectrum bins 0..63 directly into re[0..63] as |X[k]|^2 */
    float dc = re[0] + im[0];
    re[0] = dc * dc;
    re[32] = re[32] * re[32] + im[32] * im[32];

    for (uint32_t k = 1u; k < 32u; k++) {
        uint32_t m = SPEC_FFT_HALF - k;
        float zk_r = re[k], zk_i = im[k];
        float zm_r = re[m], zm_i = im[m];

        float er  = 0.5f * (zk_r + zm_r);
        float ei  = 0.5f * (zk_i - zm_i);
        float or_ = 0.5f * (zk_i + zm_i);
        float oi  = 0.5f * (zm_r - zk_r);

        float wr  = k_twiddle_re[k];
        float wi  = k_twiddle_im[k];
        float tr  = wr * or_ - wi * oi;
        float ti  = wr * oi  + wi * or_;

        float xk_r = er + tr;
        float xk_i = ei + ti;
        float xm_r = er - tr;
        float xm_i = ei - ti;

        re[k] = xk_r * xk_r + xk_i * xk_i;
        re[m] = xm_r * xm_r + xm_i * xm_i;
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
    float    re[SPEC_FFT_HALF];
    float    im[SPEC_FFT_HALF];
    uint32_t i, r, c, n;

    if (!sp) {
        return;
    }

    /* Scroll spectrogram left by 1 pixel using 32-bit aligned word ops */
    for (r = 0u; r < UI_SPEC_H; r++) {
        uint16_t *row = sp->pixels[r];
        if (((uintptr_t)row & 3u) == 0u) {
            uint32_t *w = (uint32_t *)(void *)row;
            uint32_t prev = w[0];
            for (c = 0u; c < (UI_SPEC_W / 2u) - 1u; c++) {
                uint32_t next = w[c + 1u];
                w[c] = (prev >> 16) | (next << 16);
                prev = next;
            }
            row[UI_SPEC_W - 2u] = (uint16_t)(prev >> 16);
        } else {
            for (c = 0u; c < UI_SPEC_W - 1u; c++) {
                row[c] = row[c + 1u];
            }
        }
    }
    for (c = 0u; c < UI_SPEC_W - 1u; c++) {
        sp->markers[c] = sp->markers[c + 1u];
    }
    sp->markers[UI_SPEC_W - 1u] = 0u;

    /* Extract up to 128 mono samples, subtract DC mean, apply pre-scaled Hann window,
     * and pack even/odd samples into re[0..63] / im[0..63] for 64-pt complex FFT. */
    n = (pcm != (const int16_t *)0 && frames > 0u)
            ? ((frames > UI_SPEC_FFT_N) ? UI_SPEC_FFT_N : frames)
            : 0u;

    float mean = 0.0f;
    if (n > 0u) {
        int32_t acc = 0;
        if (channels >= 2u) {
            for (i = 0u; i < n; i++) {
                acc += (int32_t)pcm[2u * i] + (int32_t)pcm[2u * i + 1u];
            }
            mean = (float)acc * (0.5f / (float)n);
        } else {
            for (i = 0u; i < n; i++) {
                acc += (int32_t)pcm[i];
            }
            mean = (float)acc / (float)n;
        }
    }

    for (i = 0u; i < SPEC_FFT_HALF; i++) {
        uint32_t i0 = i * 2u;
        uint32_t i1 = i0 + 1u;
        float s0 = 0.0f;
        float s1 = 0.0f;
        if (i0 < n) {
            float raw0 = (channels >= 2u)
                       ? (0.5f * ((float)pcm[2u * i0] + (float)pcm[2u * i0 + 1u]))
                       : (float)pcm[i0];
            s0 = (raw0 - mean) * k_hann_scaled[i0];
        }
        if (i1 < n) {
            float raw1 = (channels >= 2u)
                       ? (0.5f * ((float)pcm[2u * i1] + (float)pcm[2u * i1 + 1u]))
                       : (float)pcm[i1];
            s1 = (raw1 - mean) * k_hann_scaled[i1];
        }
        re[i] = s0;
        im[i] = s1;
    }

    /* 128-pt real FFT -> magnitude-squared bins 0..63 in re[0..63] */
    rfft128_mag2_f32(re, im);

    /* Map 64 bins into 24 Mel rows and look up magma RGB565 */
    for (r = 0u; r < UI_SPEC_H; r++) {
        uint32_t lo  = k_mel_bins[r].lo;
        uint32_t hi  = k_mel_bins[r].hi;
        float    sum = 0.0f;
        uint32_t idx;
        uint16_t col;

        for (i = lo; i <= hi; i++) {
            sum += re[i];
        }
        float avg = sum * k_mel_bins[r].inv_count;

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

    /* Complete the right half of any recently dropped cyan marker notch as it scrolls left */
    if (sp->markers[UI_SPEC_W - 2u]) {
        sp->pixels[0][UI_SPEC_W - 1u] = UI_COL_CYAN;
        sp->pixels[1][UI_SPEC_W - 1u] = UI_COL_CYAN;
    }
    if (sp->markers[UI_SPEC_W - 3u]) {
        sp->pixels[0][UI_SPEC_W - 1u] = UI_COL_CYAN;
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
    const uint32_t col = UI_SPEC_W - 1u;
    sp->markers[col] = 1u;

    /* Render 1x24 vertical cyan marker line + left half of top notch directly into pixels
     * so markers scroll naturally inside sp->pixels and appear in LVGL. */
    for (uint32_t r = 0u; r < UI_SPEC_H; r++) {
        sp->pixels[r][col] = UI_COL_CYAN;
    }
    sp->pixels[0][col - 2u] = UI_COL_CYAN;
    sp->pixels[0][col - 1u] = UI_COL_CYAN;
    sp->pixels[1][col - 1u] = UI_COL_CYAN;
}

void ui_spec_blit(const ui_spectrogram_t *sp, int16_t x, int16_t y)
{
    if (!sp) {
        return;
    }

    /* Cyan marker lines & top notches are already baked into sp->pixels */
    lcd_blit_rgb565((uint16_t)x, (uint16_t)y, UI_SPEC_W, UI_SPEC_H, &sp->pixels[0][0]);
}
