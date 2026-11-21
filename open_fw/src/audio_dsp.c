/*
 * Clean-Room Audio DSP & Voice-Operated Recording (VOR) Implementation
 *
 * Uses pure single-precision IEEE-754 float math (hardware FPU on Cortex-M4F)
 * with zero libm / heap dependencies so open_fw.elf remains 100% freestanding.
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio_dsp.h"

#define DSP_PI      3.14159265f
#define DSP_TWO_PI  6.28318531f
#define DSP_LN2     0.69314718f
#define DSP_LOG2E   1.44269504f
#define DSP_LOG10_2 0.301029996f
#define DSP_SQRT2_2 0.70710678f

/* -3.0 dBFS knee and -0.5 dBFS ceiling for transparent safety peak limiter */
#define LIM_KNEE_LIN  0.70794578f
#define LIM_CEIL_LIN  0.94406088f
#define LIM_SPAN_LIN  (LIM_CEIL_LIN - LIM_KNEE_LIN)
/* -24.0 dBFS threshold and +6.0 dB makeup gain for 2:1 VOICE_CLARITY compressor */
#define COMP_THRESH_LIN 0.06309573f
#define COMP_MAKEUP_LIN 1.99526231f
/* sqrt(COMP_THRESH_LIN) * COMP_MAKEUP_LIN = 10^(-6/20) */
#define COMP_SCALE_K    0.50118723f

#define VOR_FADE_BUF_MAX 256u /* >= 5 ms @ 48 kHz (240 frames) */

static inline float dsp_fabsf(float x)
{
    return (x < 0.0f) ? -x : x;
}

static inline float dsp_rsqrtf(float x)
{
    union {
        float    f;
        uint32_t u;
    } conv;
    conv.f = x;
    conv.u = 0x5F375A86u - (conv.u >> 1);
    float y = conv.f;
    y = y * (1.5f - 0.5f * x * y * y);
    return y;
}

static float dsp_sqrtf(float x)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
#if defined(__ARM_FP) && ((__ARM_FP & 0x4) != 0)
    float res;
    __asm__ ("vsqrt.f32 %0, %1" : "=t"(res) : "t"(x));
    return res;
#else
    union {
        float    f;
        uint32_t u;
    } conv;
    conv.f = x;
    conv.u = (conv.u >> 1) + 0x1FC00000u;
    float y = conv.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
#endif
}

static float dsp_pow2f(float x)
{
    if (x < -126.0f) {
        return 0.0f;
    }
    if (x > 127.0f) {
        x = 127.0f;
    }
    /* Center the polynomial on zero. Envelope exponents sit just below
     * zero, where a small coefficient error compounds over thousands of samples. */
    int32_t k = (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
    float f = x - (float)k;
    float u = f * DSP_LN2;
    /* Degree-5 Taylor polynomial for |u| <= ln(2)/2. */
    float p = 1.0f + u * (1.0f + u * (0.5f + u * (0.16666667f +
              u * (0.04166667f + u * 0.00833333f))));
    union {
        float    f;
        uint32_t u;
    } scale;
    scale.u = (uint32_t)(k + 127) << 23;
    return p * scale.f;
}

static float dsp_log2f(float x)
{
    if (x <= 1.0e-30f) {
        return -100.0f;
    }
    union {
        float    f;
        uint32_t u;
    } v;
    v.f = x;
    int32_t exp_val = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u; /* Mantissa m in [1, 2) */
    float m  = v.f;
    float t  = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float ln_m = 2.0f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * 0.14285714f)));
    return (float)exp_val + ln_m * DSP_LOG2E;
}

static float dsp_lin_to_dbfs(float lin)
{
    if (lin <= 1.5849e-5f) {
        return -96.0f;
    }
    float db = 20.0f * dsp_log2f(lin) * DSP_LOG10_2;
    if (db < -96.0f) {
        return -96.0f;
    }
    if (db > 0.0f) {
        return 0.0f;
    }
    return db;
}

static void dsp_sincosf(float rad, float *out_sin, float *out_cos)
{
    /* Wrap to [-pi, pi] */
    while (rad > DSP_PI) {
        rad -= DSP_TWO_PI;
    }
    while (rad < -DSP_PI) {
        rad += DSP_TWO_PI;
    }
    int negate_cos = 0;
    if (rad > 0.5f * DSP_PI) {
        rad = DSP_PI - rad;
        negate_cos = 1;
    } else if (rad < -0.5f * DSP_PI) {
        rad = -DSP_PI - rad;
        negate_cos = 1;
    }

    float x2 = rad * rad;
    float s  = rad * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.00019841f)));
    float c  = 1.0f - x2 * (0.5f - x2 * (0.04166667f - x2 * (0.00138889f - x2 * 0.00002480f)));
    if (negate_cos) {
        c = -c;
    }
    *out_sin = s;
    *out_cos = c;
}

static float dsp_exp_decay(float tau_sec, uint32_t sample_rate)
{
    if (tau_sec <= 0.0f || sample_rate == 0u) {
        return 0.0f;
    }
    float exponent = -1.0f / (tau_sec * (float)sample_rate);
    return dsp_pow2f(exponent * DSP_LOG2E);
}

static void biquad_set_unity(audio_biquad_t *bq)
{
    bq->b0 = 1.0f;
    bq->b1 = 0.0f;
    bq->b2 = 0.0f;
    bq->a1 = 0.0f;
    bq->a2 = 0.0f;
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

static void biquad_design_hpf(audio_biquad_t *bq, float fc_hz, uint32_t sample_rate)
{
    float max_fc = 0.45f * (float)sample_rate;
    if (fc_hz > max_fc) {
        fc_hz = max_fc;
    }
    if (fc_hz < 1.0f) {
        fc_hz = 1.0f;
    }

    if (sample_rate == 16000u && fc_hz > 74.5f && fc_hz < 75.5f) {
        /* Standard 75 Hz Butterworth HPF @ 16 kHz, Q=0.70710678 */
        bq->b0 =  0.9793881f;
        bq->b1 = -1.9587762f;
        bq->b2 =  0.9793881f;
        bq->a1 = -1.9583506f;
        bq->a2 =  0.9592018f;
        bq->z1 = 0.0f;
        bq->z2 = 0.0f;
        return;
    }
    if (sample_rate == 16000u && fc_hz > 349.5f && fc_hz < 350.5f) {
        /* Standard 350 Hz Butterworth HPF @ 16 kHz, Q=0.70710678 */
        bq->b0 =  0.9073629f;
        bq->b1 = -1.8147258f;
        bq->b2 =  0.9073629f;
        bq->a1 = -1.8062004f;
        bq->a2 =  0.8232512f;
        bq->z1 = 0.0f;
        bq->z2 = 0.0f;
        return;
    }

    float w0 = DSP_TWO_PI * fc_hz / (float)sample_rate;
    float sw = 0.0f, cw = 1.0f;
    dsp_sincosf(w0, &sw, &cw);
    float alpha  = sw / (2.0f * DSP_SQRT2_2);
    float a0     = 1.0f + alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = ((1.0f + cw) * 0.5f) * inv_a0;
    bq->b1 = (-(1.0f + cw)) * inv_a0;
    bq->b2 = bq->b0;
    bq->a1 = (-2.0f * cw) * inv_a0;
    bq->a2 = (1.0f - alpha) * inv_a0;
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

static void biquad_design_high_shelf(audio_biquad_t *bq, float fc_hz, float db_gain, uint32_t sample_rate)
{
    float max_fc = 0.45f * (float)sample_rate;
    if (fc_hz > max_fc) {
        fc_hz = max_fc;
    }
    if (fc_hz < 1.0f) {
        fc_hz = 1.0f;
    }

    float A  = dsp_pow2f((db_gain / 40.0f) * 3.32192809f); /* 10^(dBgain/40) */
    float w0 = DSP_TWO_PI * fc_hz / (float)sample_rate;
    float sw = 0.0f, cw = 1.0f;
    dsp_sincosf(w0, &sw, &cw);

    float alpha  = sw / (2.0f * DSP_SQRT2_2); /* S = 1.0f -> Butterworth shelf slope */
    float two_sqrtA_alpha = 2.0f * dsp_sqrtf(A) * alpha;
    float a0     = (A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha;
    float inv_a0 = 1.0f / a0;

    bq->b0 = (A * ((A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha)) * inv_a0;
    bq->b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw)) * inv_a0;
    bq->b2 = (A * ((A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha)) * inv_a0;
    bq->a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cw)) * inv_a0;
    bq->a2 = ((A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha) * inv_a0;
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

static inline float biquad_step(audio_biquad_t *bq, float x)
{
    float y  = bq->b0 * x + bq->z1;
    bq->z1   = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2   = bq->b2 * x - bq->a2 * y;
    return y;
}

static inline float limiter_gain_for_env(float env, uint32_t *clip_events)
{
    if (env <= LIM_KNEE_LIN) {
        return 1.0f;
    }
    float excess = env - LIM_KNEE_LIN;
    float gain   = (LIM_KNEE_LIN * LIM_SPAN_LIN + LIM_CEIL_LIN * excess)
                 / (env * (excess + LIM_SPAN_LIN));
    if (clip_events && gain < 0.999f) {
        (*clip_events)++;
    }
    return gain;
}

static inline int16_t float_to_i16(float s)
{
    float scaled = s * 32767.0f;
    if (scaled >= 32767.0f) {
        return 32767;
    }
    if (scaled <= -32767.0f) {
        return -32767;
    }
    return (scaled >= 0.0f) ? (int16_t)(scaled + 0.5f) : (int16_t)(scaled - 0.5f);
}

void audio_dsp_init(audio_dsp_t *dsp, audio_dsp_profile_t profile,
                    uint32_t sample_rate, uint8_t channels)
{
    if (!dsp) {
        return;
    }
    dsp->profile       = profile;
    dsp->sample_rate   = (sample_rate > 0u) ? sample_rate : 16000u;
    dsp->channels      = (channels == 2u) ? 2u : 1u;
    dsp->beamform_mono = 0u;
    dsp->lim_att       = dsp_exp_decay(0.001f, dsp->sample_rate);
    dsp->lim_rel       = dsp_exp_decay(0.080f, dsp->sample_rate);
    dsp->comp_att      = dsp_exp_decay(0.005f, dsp->sample_rate);
    dsp->comp_rel      = dsp_exp_decay(0.120f, dsp->sample_rate);
    dsp->comp_env      = 0.0f;
    dsp->lim_env       = 0.0f;
    dsp->clip_events   = 0u;
    dsp->rms_dbfs      = -96.0f;

    for (uint8_t ch = 0u; ch < 2u; ch++) {
        if (profile == AUDIO_DSP_ASR_RAW) {
            biquad_design_hpf(&dsp->hpf[ch], 75.0f, dsp->sample_rate);
            biquad_set_unity(&dsp->shelf[ch]);
        } else if (profile == AUDIO_DSP_VOICE_CLARITY) {
            biquad_design_hpf(&dsp->hpf[ch], 350.0f, dsp->sample_rate);
            biquad_design_high_shelf(&dsp->shelf[ch], 2200.0f, 9.0f, dsp->sample_rate);
        } else {
            biquad_set_unity(&dsp->hpf[ch]);
            biquad_set_unity(&dsp->shelf[ch]);
        }
    }
}

void audio_dsp_reset(audio_dsp_t *dsp)
{
    if (!dsp) {
        return;
    }
    for (uint8_t ch = 0u; ch < 2u; ch++) {
        dsp->hpf[ch].z1   = 0.0f;
        dsp->hpf[ch].z2   = 0.0f;
        dsp->shelf[ch].z1 = 0.0f;
        dsp->shelf[ch].z2 = 0.0f;
    }
    dsp->comp_env    = 0.0f;
    dsp->lim_env     = 0.0f;
    dsp->clip_events = 0u;
    dsp->rms_dbfs    = -96.0f;
}

void audio_dsp_set_beamform(audio_dsp_t *dsp, int enabled)
{
    if (dsp) {
        dsp->beamform_mono = enabled ? 1u : 0u;
    }
}

void audio_dsp_process_i16(audio_dsp_t *dsp, int16_t *pcm, size_t num_frames)
{
    if (!dsp || !pcm || num_frames == 0u) {
        return;
    }

    const uint8_t ch = (dsp->channels == 2u) ? 2u : 1u;
    const float inv_i16 = 1.0f / 32768.0f;
    float sum_sq = 0.0f;

    if (dsp->profile == AUDIO_DSP_BYPASS) {
        size_t total = num_frames * (size_t)ch;
        for (size_t i = 0u; i < total; i++) {
            float s = (float)pcm[i] * inv_i16;
            sum_sq += s * s;
        }
        dsp->rms_dbfs = dsp_lin_to_dbfs(dsp_sqrtf(sum_sq / (float)total));
        return;
    }

    const float lim_att  = dsp->lim_att;
    const float lim_rel  = dsp->lim_rel;
    const float comp_att = dsp->comp_att;
    const float comp_rel = dsp->comp_rel;
    float lim_env        = dsp->lim_env;
    float comp_env       = dsp->comp_env;
    uint32_t clips       = dsp->clip_events;

    if (dsp->profile == AUDIO_DSP_ASR_RAW) {
        if (ch == 1u) {
            for (size_t i = 0u; i < num_frames; i++) {
                float l = biquad_step(&dsp->hpf[0], (float)pcm[i] * inv_i16);
                float lpeak = dsp_fabsf(l);
                float lc = (lpeak > lim_env) ? lim_att : lim_rel;
                lim_env = lpeak + lc * (lim_env - lpeak);
                float peak_eff = (lpeak > lim_env) ? lpeak : lim_env;
                l *= limiter_gain_for_env(peak_eff, &clips);
                pcm[i] = float_to_i16(l);
                sum_sq += l * l;
            }
        } else {
            for (size_t i = 0u; i < num_frames; i++) {
                float l = biquad_step(&dsp->hpf[0], (float)pcm[i * 2u + 0u] * inv_i16);
                float r = biquad_step(&dsp->hpf[1], (float)pcm[i * 2u + 1u] * inv_i16);
                float al = dsp_fabsf(l);
                float ar = dsp_fabsf(r);
                float lpeak = (ar > al) ? ar : al;
                float lc = (lpeak > lim_env) ? lim_att : lim_rel;
                lim_env = lpeak + lc * (lim_env - lpeak);
                float peak_eff = (lpeak > lim_env) ? lpeak : lim_env;
                float lgain = limiter_gain_for_env(peak_eff, &clips);
                l *= lgain;
                r *= lgain;
                pcm[i * 2u + 0u] = float_to_i16(l);
                pcm[i * 2u + 1u] = float_to_i16(r);
                sum_sq += l * l + r * r;
            }
        }
    } else if (ch == 1u || dsp->beamform_mono) {
        /* Mono Clarity or dual-mic broadside beamformer: single biquad/comp/limiter pass */
        const float half_inv_i16 = 0.5f * inv_i16;
        for (size_t i = 0u; i < num_frames; i++) {
            float m = (ch == 2u)
                    ? ((float)((int32_t)pcm[i * 2u + 0u] + (int32_t)pcm[i * 2u + 1u]) * half_inv_i16)
                    : ((float)pcm[i] * inv_i16);
            m = biquad_step(&dsp->hpf[0], m);
            m = biquad_step(&dsp->shelf[0], m);

            float cpeak = dsp_fabsf(m);
            float cc = (cpeak > comp_env) ? comp_att : comp_rel;
            comp_env = cpeak + cc * (comp_env - cpeak);
            float total_c = (comp_env > COMP_THRESH_LIN)
                          ? (COMP_SCALE_K * dsp_rsqrtf(comp_env))
                          : COMP_MAKEUP_LIN;
            m *= total_c;

            float lpeak = dsp_fabsf(m);
            float lc = (lpeak > lim_env) ? lim_att : lim_rel;
            lim_env = lpeak + lc * (lim_env - lpeak);
            float peak_eff = (lpeak > lim_env) ? lpeak : lim_env;
            m *= limiter_gain_for_env(peak_eff, &clips);

            int16_t om = float_to_i16(m);
            if (ch == 2u) {
                pcm[i * 2u + 0u] = om;
                pcm[i * 2u + 1u] = om;
                sum_sq += 2.0f * m * m;
            } else {
                pcm[i] = om;
                sum_sq += m * m;
            }
        }
    } else {
        /* Stereo Clarity */
        for (size_t i = 0u; i < num_frames; i++) {
            float l = biquad_step(&dsp->hpf[0], (float)pcm[i * 2u + 0u] * inv_i16);
            float r = biquad_step(&dsp->hpf[1], (float)pcm[i * 2u + 1u] * inv_i16);
            l = biquad_step(&dsp->shelf[0], l);
            r = biquad_step(&dsp->shelf[1], r);

            float al = dsp_fabsf(l);
            float ar = dsp_fabsf(r);
            float cpeak = (ar > al) ? ar : al;
            float cc = (cpeak > comp_env) ? comp_att : comp_rel;
            comp_env = cpeak + cc * (comp_env - cpeak);
            float total_c = (comp_env > COMP_THRESH_LIN)
                          ? (COMP_SCALE_K * dsp_rsqrtf(comp_env))
                          : COMP_MAKEUP_LIN;
            l *= total_c;
            r *= total_c;

            float lal = dsp_fabsf(l);
            float lar = dsp_fabsf(r);
            float lpeak = (lar > lal) ? lar : lal;
            float lc = (lpeak > lim_env) ? lim_att : lim_rel;
            lim_env = lpeak + lc * (lim_env - lpeak);
            float peak_eff = (lpeak > lim_env) ? lpeak : lim_env;
            float lgain = limiter_gain_for_env(peak_eff, &clips);
            l *= lgain;
            r *= lgain;

            pcm[i * 2u + 0u] = float_to_i16(l);
            pcm[i * 2u + 1u] = float_to_i16(r);
            sum_sq += l * l + r * r;
        }
    }

    dsp->lim_env     = lim_env;
    dsp->comp_env    = comp_env;
    dsp->clip_events = clips;
    dsp->rms_dbfs    = dsp_lin_to_dbfs(dsp_sqrtf(sum_sq / (float)(num_frames * (size_t)ch)));
}

/* ============================================================================
 * Phase 4 Clean-Room VOR (Voice-Operated Recording)
 * ============================================================================ */
void audio_vor_init(audio_vor_t *vor, uint32_t sample_rate, uint8_t channels, int enabled)
{
    if (!vor) {
        return;
    }
    vor->enabled         = enabled ? 1u : 0u;
    vor->channels        = (channels == 2u) ? 2u : 1u;
    vor->sample_rate     = (sample_rate > 0u) ? sample_rate : 16000u;
    vor->hangover_frames = (vor->sample_rate * 500u) / 1000u; /* 500 ms hangover */
    vor->preroll_cap_frames = (vor->sample_rate * 200u) / 1000u; /* 200 ms pre-roll */
    if (vor->preroll_cap_frames > AUDIO_VOR_PREROLL_FRAMES) {
        vor->preroll_cap_frames = AUDIO_VOR_PREROLL_FRAMES;
    }
    if (vor->preroll_cap_frames == 0u) {
        vor->preroll_cap_frames = 1u;
    }
    vor->open_thresh_dbfs  = -34.0f;
    vor->close_thresh_dbfs = -40.0f;
    vor->min_zcr           = 0.002f;
    vor->max_zcr           = 0.48f;
    audio_vor_reset(vor);
}

void audio_vor_reset(audio_vor_t *vor)
{
    if (!vor) {
        return;
    }
    vor->gate_open    = 0u;
    vor->hangover_rem = 0u;
    vor->preroll_wr   = 0u;
    vor->preroll_fill = 0u;
}

static uint32_t vor_fade_len(const audio_vor_t *vor)
{
    uint32_t f = (vor->sample_rate * 5u) / 1000u; /* 5 ms fade-in/out */
    if (f < 1u) {
        f = 1u;
    }
    if (f > VOR_FADE_BUF_MAX) {
        f = VOR_FADE_BUF_MAX;
    }
    return f;
}

static void vor_push_preroll(audio_vor_t *vor, const int16_t *pcm, size_t num_frames)
{
    uint32_t cap = vor->preroll_cap_frames;
    uint8_t  ch  = vor->channels;
    for (size_t i = 0u; i < num_frames; i++) {
        uint32_t dst_idx = vor->preroll_wr * (uint32_t)ch;
        vor->preroll_buf[dst_idx] = pcm[i * (size_t)ch];
        if (ch == 2u) {
            vor->preroll_buf[dst_idx + 1u] = pcm[i * 2u + 1u];
        }
        vor->preroll_wr++;
        if (vor->preroll_wr >= cap) {
            vor->preroll_wr = 0u;
        }
        if (vor->preroll_fill < cap) {
            vor->preroll_fill++;
        }
    }
}

/* Flush pre-roll ring with a 5 ms fade-in at gate open; returns frames faded so far. */
static uint32_t vor_flush_preroll_fadein(audio_vor_t *vor, uint32_t fade_len,
                                         audio_vor_emit_fn_t emit_cb, void *user)
{
    if (vor->preroll_fill == 0u || !emit_cb) {
        return 0u;
    }
    uint32_t cap   = vor->preroll_cap_frames;
    uint32_t fill  = vor->preroll_fill;
    uint32_t start = (vor->preroll_wr >= fill) ? (vor->preroll_wr - fill) : (cap + vor->preroll_wr - fill);
    uint8_t  ch    = vor->channels;

    /* Apply linear 5 ms fade-in directly in preroll_buf before emitting */
    uint32_t nfade = (fill < fade_len) ? fill : fade_len;
    for (uint32_t k = 0u; k < nfade; k++) {
        uint32_t idx = ((start + k) % cap) * (uint32_t)ch;
        int32_t  num = (int32_t)k;
        int32_t  den = (int32_t)fade_len;
        vor->preroll_buf[idx] = (int16_t)(((int32_t)vor->preroll_buf[idx] * num) / den);
        if (ch == 2u) {
            vor->preroll_buf[idx + 1u] = (int16_t)(((int32_t)vor->preroll_buf[idx + 1u] * num) / den);
        }
    }

    if (start + fill <= cap) {
        emit_cb(user, &vor->preroll_buf[start * (uint32_t)ch], fill);
    } else {
        uint32_t first = cap - start;
        emit_cb(user, &vor->preroll_buf[start * (uint32_t)ch], first);
        emit_cb(user, &vor->preroll_buf[0], fill - first);
    }
    vor->preroll_wr   = 0u;
    vor->preroll_fill = 0u;
    return nfade;
}

static void vor_emit_with_fadein(const int16_t *blk, size_t n, uint8_t ch,
                                 uint32_t faded_so_far, uint32_t fade_len,
                                 audio_vor_emit_fn_t emit_cb, void *user)
{
    if (faded_so_far >= fade_len) {
        emit_cb(user, blk, n);
        return;
    }
    uint32_t rem = fade_len - faded_so_far;
    if (rem > (uint32_t)n) {
        rem = (uint32_t)n;
    }
    if (rem > VOR_FADE_BUF_MAX) {
        rem = VOR_FADE_BUF_MAX;
    }
    int16_t tmp[VOR_FADE_BUF_MAX * 2u];
    int32_t den = (int32_t)fade_len;
    for (uint32_t k = 0u; k < rem; k++) {
        int32_t num = (int32_t)(faded_so_far + k);
        tmp[k * (uint32_t)ch] = (int16_t)(((int32_t)blk[k * (uint32_t)ch] * num) / den);
        if (ch == 2u) {
            tmp[k * 2u + 1u] = (int16_t)(((int32_t)blk[k * 2u + 1u] * num) / den);
        }
    }
    emit_cb(user, tmp, rem);
    if ((size_t)rem < n) {
        emit_cb(user, blk + (size_t)rem * (size_t)ch, n - (size_t)rem);
    }
}

static void vor_emit_with_fadeout(const int16_t *blk, size_t n, uint8_t ch,
                                  uint32_t fade_len, audio_vor_emit_fn_t emit_cb, void *user)
{
    uint32_t tail = (n < (size_t)fade_len) ? (uint32_t)n : fade_len;
    if (tail > VOR_FADE_BUF_MAX) {
        tail = VOR_FADE_BUF_MAX;
    }
    size_t prefix = n - (size_t)tail;
    if (prefix > 0u) {
        emit_cb(user, blk, prefix);
    }
    int16_t tmp[VOR_FADE_BUF_MAX * 2u];
    const int16_t *src = blk + prefix * (size_t)ch;
    int32_t den = (int32_t)tail;
    for (uint32_t k = 0u; k < tail; k++) {
        int32_t num = (int32_t)(tail - 1u - k);
        tmp[k * (uint32_t)ch] = (int16_t)(((int32_t)src[k * (uint32_t)ch] * num) / den);
        if (ch == 2u) {
            tmp[k * 2u + 1u] = (int16_t)(((int32_t)src[k * 2u + 1u] * num) / den);
        }
    }
    emit_cb(user, tmp, tail);
}

void audio_vor_feed(audio_vor_t *vor, const int16_t *pcm, size_t num_frames,
                    audio_vor_emit_fn_t emit_cb, void *user)
{
    if (!pcm || num_frames == 0u || !emit_cb) {
        return;
    }
    if (!vor || !vor->enabled) {
        emit_cb(user, pcm, num_frames);
        return;
    }

    const uint8_t ch = vor->channels;
    const float inv_i16 = 1.0f / 32768.0f;
    const uint32_t fade_len = vor_fade_len(vor);
    size_t sub_max = (vor->sample_rate * 20u) / 1000u; /* 20 ms evaluation blocks */
    if (sub_max < 64u) {
        sub_max = 64u;
    }

    size_t pos = 0u;
    while (pos < num_frames) {
        size_t n = num_frames - pos;
        if (n > sub_max) {
            n = sub_max;
        }
        const int16_t *blk = pcm + pos * (size_t)ch;

        float sum_sq = 0.0f;
        uint32_t zc_l = 0u;
        uint32_t zc_r = 0u;
        if (ch == 2u) {
            int16_t prev_l = blk[0];
            int16_t prev_r = blk[1];
            for (size_t i = 0u; i < n; i++) {
                int16_t cl = blk[i * 2u + 0u];
                int16_t cr = blk[i * 2u + 1u];
                float sl = (float)cl * inv_i16;
                float sr = (float)cr * inv_i16;
                /* Phase-invariant stereo power avoids L/R cancellation */
                sum_sq += 0.5f * (sl * sl + sr * sr);
                if ((cl >= 0 && prev_l < 0) || (cl < 0 && prev_l >= 0)) {
                    zc_l++;
                }
                if ((cr >= 0 && prev_r < 0) || (cr < 0 && prev_r >= 0)) {
                    zc_r++;
                }
                prev_l = cl;
                prev_r = cr;
            }
        } else {
            int16_t prev = blk[0];
            for (size_t i = 0u; i < n; i++) {
                int16_t cur = blk[i];
                float s = (float)cur * inv_i16;
                sum_sq += s * s;
                if ((cur >= 0 && prev < 0) || (cur < 0 && prev >= 0)) {
                    zc_l++;
                }
                prev = cur;
            }
        }

        float rms_db = dsp_lin_to_dbfs(dsp_sqrtf(sum_sq / (float)n));
        float inv_nm1 = (n > 1u) ? (1.0f / (float)(n - 1u)) : 0.0f;
        float zcr_l = (float)zc_l * inv_nm1;
        int speech_zcr = (zcr_l >= vor->min_zcr && zcr_l <= vor->max_zcr) ? 1 : 0;
        if (!speech_zcr && ch == 2u) {
            float zcr_r = (float)zc_r * inv_nm1;
            speech_zcr = (zcr_r >= vor->min_zcr && zcr_r <= vor->max_zcr) ? 1 : 0;
        }
        int trigger = (rms_db >= vor->open_thresh_dbfs &&
                       (speech_zcr || rms_db >= (vor->open_thresh_dbfs + 12.0f)));

        if (vor->gate_open) {
            if (rms_db >= vor->close_thresh_dbfs) {
                emit_cb(user, blk, n);
                vor->hangover_rem = vor->hangover_frames;
            } else if (vor->hangover_rem > (uint32_t)n) {
                emit_cb(user, blk, n);
                vor->hangover_rem -= (uint32_t)n;
            } else {
                vor_emit_with_fadeout(blk, n, ch, fade_len, emit_cb, user);
                vor->hangover_rem = 0u;
                vor->gate_open    = 0u;
            }
        } else if (trigger) {
            vor->gate_open    = 1u;
            vor->hangover_rem = vor->hangover_frames;
            uint32_t faded = vor_flush_preroll_fadein(vor, fade_len, emit_cb, user);
            vor_emit_with_fadein(blk, n, ch, faded, fade_len, emit_cb, user);
        } else {
            vor_push_preroll(vor, blk, n);
        }

        pos += n;
    }
}
