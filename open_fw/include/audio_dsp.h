/*
 * Clean-Room Audio DSP & Voice-Operated Recording (VOR) Module for Cassini OpenFW
 *
 * Implements phase-safe capture conditioning (AUDIO_DSP_ASR_RAW),
 * clean-room broadcast voice clarity (AUDIO_DSP_VOICE_CLARITY), bypass
 * diagnostics (AUDIO_DSP_BYPASS), and opt-in pre-roll VOR gating.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_AUDIO_DSP_H
#define OPENFW_AUDIO_DSP_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_DSP_ASR_RAW       = 0, /* 75 Hz HPF + safety limiter (default for Parakeet/Whistle) */
    AUDIO_DSP_VOICE_CLARITY = 1, /* 350 Hz HPF + 2.2 kHz +9 dB shelf + 2:1 comp + limiter     */
    AUDIO_DSP_BYPASS        = 2, /* Raw ADC samples, no processing                            */
} audio_dsp_profile_t;

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} audio_biquad_t;

typedef struct {
    audio_dsp_profile_t profile;
    uint32_t sample_rate;
    uint8_t  channels;          /* 1 (mono) or 2 (interleaved stereo) */
    uint8_t  beamform_mono;     /* Optional broadside (L+R)*0.5 downmix in VOICE_CLARITY */
    audio_biquad_t hpf[2];
    audio_biquad_t shelf[2];
    float comp_env;             /* Linear peak envelope for compressor */
    float lim_env;              /* Linear peak envelope for safety limiter */
    uint32_t clip_events;       /* Counter of frames engaging safety limiter */
    float    rms_dbfs;          /* Last-frame RMS level (for VU / telemetry) */
} audio_dsp_t;

void audio_dsp_init(audio_dsp_t *dsp, audio_dsp_profile_t profile,
                    uint32_t sample_rate, uint8_t channels);
void audio_dsp_reset(audio_dsp_t *dsp);
void audio_dsp_set_beamform(audio_dsp_t *dsp, int enabled);

/* In-place processing of interleaved int16 PCM (num_frames = samples per channel). */
void audio_dsp_process_i16(audio_dsp_t *dsp, int16_t *pcm, size_t num_frames);

/* ============================================================================
 * Phase 4 Clean-Room VOR (Voice-Operated Recording, Opt-In Only)
 * ============================================================================ */
#define AUDIO_VOR_PREROLL_FRAMES  3200u /* 200 ms @ 16 kHz */
#define AUDIO_VOR_PREROLL_SAMPLES (AUDIO_VOR_PREROLL_FRAMES * 2u)

typedef void (*audio_vor_emit_fn_t)(void *user, const int16_t *pcm, size_t num_frames);

typedef struct {
    uint8_t  enabled;
    uint8_t  channels;
    uint8_t  gate_open;
    uint8_t  reserved;
    uint32_t sample_rate;
    uint32_t hangover_frames;
    uint32_t hangover_rem;
    float    open_thresh_dbfs;
    float    close_thresh_dbfs;
    float    min_zcr;
    float    max_zcr;
    int16_t  preroll_buf[AUDIO_VOR_PREROLL_SAMPLES];
    uint32_t preroll_cap_frames;
    uint32_t preroll_wr;
    uint32_t preroll_fill;
} audio_vor_t;

void audio_vor_init(audio_vor_t *vor, uint32_t sample_rate, uint8_t channels, int enabled);
void audio_vor_reset(audio_vor_t *vor);
void audio_vor_feed(audio_vor_t *vor, const int16_t *pcm, size_t num_frames,
                    audio_vor_emit_fn_t emit_cb, void *user);

#endif /* OPENFW_AUDIO_DSP_H */
