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
/* -24.0 dBFS threshold and +6.0 dB makeup gain for 2:1 VOICE_CLARITY compressor */
#define COMP_THRESH_LIN 0.06309573f
#define COMP_MAKEUP_LIN 1.99526231f

static inline float dsp_fabsf(float x)
{
    return (x < 0.0f) ? -x : x;
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
    int32_t k = (int32_t)x;
    if ((float)k > x) {
        k -= 1;
    }
    float f = x - (float)k;
    float u = f * DSP_LN2;
    /* Degree-5 Taylor polynomial on u in [0, ln2) -> max error < 1.5e-4 */
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
    float alpha = sw / (2.0f * DSP_SQRT2_2);
    float a0    = 1.0f + alpha;
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
    float span   = LIM_CEIL_LIN - LIM_KNEE_LIN;
    float excess = env - LIM_KNEE_LIN;
    float target = LIM_KNEE_LIN + span * (excess / (excess + span));
    float gain   = target / env;
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

    const float lim_att  = dsp_exp_decay(0.001f, dsp->sample_rate);
    const float lim_rel  = dsp_exp_decay(0.080f, dsp->sample_rate);
    const float comp_att = dsp_exp_decay(0.005f, dsp->sample_rate);
    const float comp_rel = dsp_exp_decay(0.120f, dsp->sample_rate);
    float lim_env        = dsp->lim_env;
    float comp_env       = dsp->comp_env;

    for (size_t i = 0u; i < num_frames; i++) {
        float l = (float)pcm[i * (size_t)ch] * inv_i16;
        float r = (ch == 2u) ? ((float)pcm[i * 2u + 1u] * inv_i16) : 0.0f;

        if (ch == 2u && dsp->profile == AUDIO_DSP_VOICE_CLARITY && dsp->beamform_mono) {
            float mono = 0.5f * (l + r);
            l = mono;
            r = mono;
        }

        l = biquad_step(&dsp->hpf[0], l);
        if (ch == 2u) {
            r = biquad_step(&dsp->hpf[1], r);
        }

        if (dsp->profile == AUDIO_DSP_VOICE_CLARITY) {
            l = biquad_step(&dsp->shelf[0], l);
            if (ch == 2u) {
                r = biquad_step(&dsp->shelf[1], r);
            }

            float cpeak = dsp_fabsf(l);
            if (ch == 2u) {
                float ar = dsp_fabsf(r);
                if (ar > cpeak) {
                    cpeak = ar;
                }
            }
            float cc = (cpeak > comp_env) ? comp_att : comp_rel;
            comp_env = cc * comp_env + (1.0f - cc) * cpeak;

            float cgain = 1.0f;
            if (comp_env > COMP_THRESH_LIN) {
                cgain = dsp_sqrtf(COMP_THRESH_LIN / comp_env);
            }
            float total_c = cgain * COMP_MAKEUP_LIN;
            l *= total_c;
            if (ch == 2u) {
                r *= total_c;
            }
        }

        float lpeak = dsp_fabsf(l);
        if (ch == 2u) {
            float ar = dsp_fabsf(r);
            if (ar > lpeak) {
                lpeak = ar;
            }
        }
        float lc = (lpeak > lim_env) ? lim_att : lim_rel;
        lim_env = lc * lim_env + (1.0f - lc) * lpeak;

        float peak_eff = (lpeak > lim_env) ? lpeak : lim_env;
        float lgain = limiter_gain_for_env(peak_eff, &dsp->clip_events);
        l *= lgain;
        if (ch == 2u) {
            r *= lgain;
        }

        int16_t ol = float_to_i16(l);
        pcm[i * (size_t)ch] = ol;
        float fl = (float)ol * inv_i16;
        sum_sq += fl * fl;

        if (ch == 2u) {
            int16_t or = float_to_i16(r);
            pcm[i * 2u + 1u] = or;
            float fr = (float)or * inv_i16;
            sum_sq += fr * fr;
        }
    }

    dsp->lim_env  = lim_env;
    dsp->comp_env = comp_env;
    dsp->rms_dbfs = dsp_lin_to_dbfs(dsp_sqrtf(sum_sq / (float)(num_frames * (size_t)ch)));
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

static void vor_flush_preroll(audio_vor_t *vor, audio_vor_emit_fn_t emit_cb, void *user)
{
    if (vor->preroll_fill == 0u || !emit_cb) {
        return;
    }
    uint32_t cap   = vor->preroll_cap_frames;
    uint32_t fill  = vor->preroll_fill;
    uint32_t start = (vor->preroll_wr >= fill) ? (vor->preroll_wr - fill) : (cap + vor->preroll_wr - fill);
    uint8_t  ch    = vor->channels;

    if (start + fill <= cap) {
        emit_cb(user, &vor->preroll_buf[start * (uint32_t)ch], fill);
    } else {
        uint32_t first = cap - start;
        emit_cb(user, &vor->preroll_buf[start * (uint32_t)ch], first);
        emit_cb(user, &vor->preroll_buf[0], fill - first);
    }
    vor->preroll_wr   = 0u;
    vor->preroll_fill = 0u;
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
        uint32_t zc  = 0u;
        int16_t prev = blk[0];
        for (size_t i = 0u; i < n; i++) {
            float s = (float)blk[i * (size_t)ch] * inv_i16;
            if (ch == 2u) {
                float sr = (float)blk[i * 2u + 1u] * inv_i16;
                s = 0.5f * (s + sr);
            }
            sum_sq += s * s;
            int16_t cur = blk[i * (size_t)ch];
            if ((cur >= 0 && prev < 0) || (cur < 0 && prev >= 0)) {
                zc++;
            }
            prev = cur;
        }

        float rms_db = dsp_lin_to_dbfs(dsp_sqrtf(sum_sq / (float)n));
        float zcr    = (n > 1u) ? ((float)zc / (float)(n - 1u)) : 0.0f;
        int speech_zcr = (zcr >= vor->min_zcr && zcr <= vor->max_zcr) ? 1 : 0;
        int trigger = (rms_db >= vor->open_thresh_dbfs &&
                       (speech_zcr || rms_db >= (vor->open_thresh_dbfs + 12.0f)));

        if (vor->gate_open) {
            emit_cb(user, blk, n);
            if (rms_db >= vor->close_thresh_dbfs) {
                vor->hangover_rem = vor->hangover_frames;
            } else if (vor->hangover_rem > (uint32_t)n) {
                vor->hangover_rem -= (uint32_t)n;
            } else {
                vor->hangover_rem = 0u;
                vor->gate_open    = 0u;
            }
        } else if (trigger) {
            vor->gate_open    = 1u;
            vor->hangover_rem = vor->hangover_frames;
            vor_flush_preroll(vor, emit_cb, user);
            emit_cb(user, blk, n);
        } else {
            vor_push_preroll(vor, blk, n);
        }

        pos += n;
    }
}
