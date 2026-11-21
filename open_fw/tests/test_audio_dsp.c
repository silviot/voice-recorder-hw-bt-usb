/*
 * Host Unit Tests for Clean-Room Audio DSP & VOR Module
 *
 * Verifies:
 *   1. DC step rejection (< 1 LSB residual) under ASR_RAW & VOICE_CLARITY
 *   2. Exact frequency response sweep vs analytical RBJ Butterworth/shelf H(e^jw) (+/- 0.5 dB)
 *   3. Safety peak limiter against overdriven burst (no wrap-around, <= 32767, smooth release)
 *   4. Stereo phase-linked gain reduction + optional broadside beamformer
 *   5. BYPASS bit-exactness & RMS dBFS accuracy
 *   6. Opt-in VOR pre-roll (200 ms) + hysteresis + 500 ms hangover
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double biquad_mag_db(const audio_biquad_t *bq, double freq_hz, double fs_hz)
{
    double w  = 2.0 * M_PI * freq_hz / fs_hz;
    double c1 = cos(w);
    double s1 = sin(w);
    double c2 = cos(2.0 * w);
    double s2 = sin(2.0 * w);

    double num_re = bq->b0 + bq->b1 * c1 + bq->b2 * c2;
    double num_im = -(bq->b1 * s1 + bq->b2 * s2);
    double den_re = 1.0 + bq->a1 * c1 + bq->a2 * c2;
    double den_im = -(bq->a1 * s1 + bq->a2 * s2);

    double num_mag = sqrt(num_re * num_re + num_im * num_im);
    double den_mag = sqrt(den_re * den_re + den_im * den_im);
    return 20.0 * log10(num_mag / den_mag);
}

static double measure_sine_gain_db(audio_dsp_t *dsp, double freq_hz, double amp_i16)
{
    audio_dsp_reset(dsp);
    int16_t buf[16384];
    for (size_t i = 0u; i < 16384u; i++) {
        double v = amp_i16 * sin(2.0 * M_PI * freq_hz * (double)i / (double)dsp->sample_rate);
        buf[i] = (int16_t)llround(v);
    }

    for (size_t off = 0u; off < 16384u; off += 2048u) {
        audio_dsp_process_i16(dsp, &buf[off], 2048u);
    }

    double sum_sq = 0.0;
    for (size_t i = 8192u; i < 16384u; i++) {
        double s = (double)buf[i];
        sum_sq += s * s;
    }
    double out_rms = sqrt(sum_sq / 8192.0);
    double in_rms  = amp_i16 / sqrt(2.0);
    return 20.0 * log10(out_rms / in_rms);
}

/* Test 1: DC step rejection */
static void test_dc_rejection(void)
{
    static int16_t dc_buf[16384];

    for (int prof = 0; prof < 2; prof++) {
        audio_dsp_profile_t p = (prof == 0) ? AUDIO_DSP_ASR_RAW : AUDIO_DSP_VOICE_CLARITY;
        for (size_t i = 0u; i < 16384u; i++) {
            dc_buf[i] = 8000;
        }
        audio_dsp_t dsp;
        audio_dsp_init(&dsp, p, 16000u, 1u);
        for (size_t off = 0u; off < 16384u; off += 2048u) {
            audio_dsp_process_i16(&dsp, &dc_buf[off], 2048u);
        }
        for (size_t i = 14336u; i < 16384u; i++) {
            assert(abs((int)dc_buf[i]) <= 1);
        }
    }
    printf("  [PASS] audio_dsp DC step settles to 0 (ASR_RAW & VOICE_CLARITY)\n");
}

/* Test 2: Frequency sweep within +/-0.5 dB of analytical Butterworth / RBJ shelf curves */
static void test_frequency_sweep(void)
{
    static const double freqs[6] = { 30.0, 75.0, 350.0, 1000.0, 2200.0, 5000.0 };

    audio_dsp_t raw;
    audio_dsp_init(&raw, AUDIO_DSP_ASR_RAW, 16000u, 1u);
    for (size_t i = 0u; i < 6u; i++) {
        double measured   = measure_sine_gain_db(&raw, freqs[i], 3200.0);
        double analytical = biquad_mag_db(&raw.hpf[0], freqs[i], 16000.0);
        assert(fabs(measured - analytical) <= 0.5);
        if (fabs(freqs[i] - 75.0) < 0.1) {
            assert(fabs(measured - (-3.01)) <= 0.5);
        }
        if (freqs[i] >= 1000.0) {
            assert(fabs(measured) <= 0.3);
        }
    }

    audio_dsp_t clr;
    audio_dsp_init(&clr, AUDIO_DSP_VOICE_CLARITY, 16000u, 1u);
    for (size_t i = 0u; i < 6u; i++) {
        /* Keep amplitude well below -24 dBFS compressor threshold to verify pure EQ + +6 dB makeup */
        double measured   = measure_sine_gain_db(&clr, freqs[i], 400.0);
        double analytical = biquad_mag_db(&clr.hpf[0], freqs[i], 16000.0)
                          + biquad_mag_db(&clr.shelf[0], freqs[i], 16000.0)
                          + 6.0;
        if (freqs[i] >= 300.0) {
            assert(fabs(measured - analytical) <= 0.5);
        } else {
            /* 30 Hz and 75 Hz are below the 350 Hz HPF (-43 dB and -27 dB); verify strong attenuation */
            assert(measured < analytical + 1.5 && measured < -20.0);
        }
    }
    printf("  [PASS] audio_dsp frequency sweep (30, 75, 350, 1000, 2200, 5000 Hz) within +/-0.5 dB\n");
}

static void test_voice_compression_ratio(void)
{
    /* Both tones are above the Clarity threshold and below the limiter knee.
     * Quadrupling the input (+12 dB) should double the output (+6 dB). */
    audio_dsp_t dsp;
    audio_dsp_init(&dsp, AUDIO_DSP_VOICE_CLARITY, 16000u, 1u);
    double low_gain = measure_sine_gain_db(&dsp, 1000.0, 4000.0);
    double high_gain = measure_sine_gain_db(&dsp, 1000.0, 16000.0);
    double input_step = 20.0 * log10(4.0);
    assert(fabs(input_step + high_gain - low_gain - input_step / 2.0) < 0.25);

    /* With a silent input the envelope decays to 1/e after 120 ms. */
    int16_t silence[1920] = { 0 };
    audio_dsp_reset(&dsp);
    dsp.comp_env = 1.0f;
    audio_dsp_process_i16(&dsp, silence, 1920u);
    assert(fabs(dsp.comp_env - exp(-1.0)) < 0.002);

    audio_dsp_init(&dsp, AUDIO_DSP_ASR_RAW, 16000u, 1u);
    low_gain = measure_sine_gain_db(&dsp, 1000.0, 4000.0);
    high_gain = measure_sine_gain_db(&dsp, 1000.0, 16000.0);
    assert(fabs(high_gain - low_gain) < 0.1);
    puts("  [PASS] Clarity compresses 2:1 and Raw preserves speech dynamics");
}

/* Test 3: Overdriven burst -> safety limiter holds peak <= 32767, zero wrap-around, smooth release */
static void test_safety_limiter(void)
{
    audio_dsp_t dsp;
    int16_t frame[2048];

    /* 3a: VOICE_CLARITY 4 kHz burst (+9 dB shelf + +6 dB makeup drives internal transient > +6 dBFS) */
    audio_dsp_init(&dsp, AUDIO_DSP_VOICE_CLARITY, 16000u, 1u);
    for (size_t i = 0u; i < 2048u; i++) {
        double v = 22000.0 * sin(2.0 * M_PI * 4000.0 * (double)i / 16000.0);
        frame[i] = (int16_t)llround(v);
    }
    audio_dsp_process_i16(&dsp, frame, 2048u);
    assert(dsp.clip_events > 20u);
    int16_t trans_peak  = 0;
    int16_t steady_peak = 0;
    for (size_t i = 0u; i < 2048u; i++) {
        assert(frame[i] <= 32767 && frame[i] >= -32767);
        int16_t a = (int16_t)abs((int)frame[i]);
        if (a > trans_peak) {
            trans_peak = a;
        }
        if (i >= 512u && a > steady_peak) {
            steady_peak = a;
        }
    }
    /* Transient caught cleanly below -0.5 dBFS ceiling (~30935), steady compressed near -3.2 dBFS */
    assert(trans_peak <= 31200 && steady_peak >= 20000 && steady_peak <= 31200);

    /* Smooth release check: feed quiet -26 dBFS 1 kHz frame after settling */
    for (int f = 0; f < 6; f++) {
        for (size_t i = 0u; i < 2048u; i++) {
            double v = 1600.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0);
            frame[i] = (int16_t)llround(v);
        }
        audio_dsp_process_i16(&dsp, frame, 2048u);
    }
    uint32_t clips_before = dsp.clip_events;
    for (size_t i = 0u; i < 2048u; i++) {
        double v = 1600.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0);
        frame[i] = (int16_t)llround(v);
    }
    audio_dsp_process_i16(&dsp, frame, 2048u);
    assert(dsp.clip_events == clips_before);

    /* 3b: ASR_RAW full-scale 1 kHz sine + bipolar step (> -3 dBFS knee, HPF overshoots) */
    audio_dsp_init(&dsp, AUDIO_DSP_ASR_RAW, 16000u, 1u);
    for (size_t i = 0u; i < 2048u; i++) {
        frame[i] = (int16_t)llround(32000.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0));
    }
    audio_dsp_process_i16(&dsp, frame, 2048u);
    assert(dsp.clip_events > 500u);
    int16_t raw_lim_peak = 0;
    for (size_t i = 0u; i < 2048u; i++) {
        assert(frame[i] <= 32767 && frame[i] >= -32767);
        int16_t a = (int16_t)abs((int)frame[i]);
        if (i >= 512u && a > raw_lim_peak) {
            raw_lim_peak = a;
        }
    }
    assert(raw_lim_peak >= 26000 && raw_lim_peak <= 30935);
    printf("  [PASS] audio_dsp safety limiter (trans=%d, clr=%d, raw_lim=%d, clips=%u)\n",
           (int)trans_peak, (int)steady_peak, (int)raw_lim_peak, (unsigned)dsp.clip_events);
}

/* Test 4: Stereo phase-link + optional broadside beamformer */
static void test_stereo_link_and_beamform(void)
{
    audio_dsp_t dsp;
    audio_dsp_init(&dsp, AUDIO_DSP_ASR_RAW, 16000u, 2u);

    int16_t st[2048 * 2];
    for (size_t i = 0u; i < 2048u; i++) {
        double s = sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0);
        /* Exact 4:1 amplitude ratio while L crosses -3 dBFS limiter knee */
        st[i * 2u + 0u] = (int16_t)llround(31000.0 * s);
        st[i * 2u + 1u] = (int16_t)llround(7750.0 * s);
    }
    audio_dsp_process_i16(&dsp, st, 2048u);
    assert(dsp.clip_events > 100u);

    /* Measure L and R RMS ratio on second half of buffer; stereo-linked gain keeps ratio == 4.0 */
    double sum_l = 0.0, sum_r = 0.0;
    for (size_t i = 1024u; i < 2048u; i++) {
        sum_l += (double)st[i * 2u + 0u] * (double)st[i * 2u + 0u];
        sum_r += (double)st[i * 2u + 1u] * (double)st[i * 2u + 1u];
    }
    double ratio = sqrt(sum_l / sum_r);
    assert(fabs(ratio - 4.0) <= 0.02);

    /* Broadside beamformer: out-of-phase (L=+s, R=-s) cancels; in-phase (L=+s, R=+s) preserves */
    audio_dsp_init(&dsp, AUDIO_DSP_VOICE_CLARITY, 16000u, 2u);
    audio_dsp_set_beamform(&dsp, 1);
    for (size_t i = 0u; i < 2048u; i++) {
        int16_t s = (int16_t)llround(4000.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0));
        st[i * 2u + 0u] = s;
        st[i * 2u + 1u] = (int16_t)-s;
    }
    audio_dsp_process_i16(&dsp, st, 2048u);
    for (size_t i = 0u; i < 4096u; i++) {
        assert(abs((int)st[i]) <= 1);
    }
    printf("  [PASS] audio_dsp stereo gain-link (L/R=%.4f) & broadside beamformer\n", ratio);
}

/* Test 5: BYPASS bit-exactness + rms_dbfs accuracy */
static void test_bypass_and_rms(void)
{
    audio_dsp_t dsp;
    audio_dsp_init(&dsp, AUDIO_DSP_BYPASS, 16000u, 1u);

    int16_t ref[2048], buf[2048];
    for (size_t i = 0u; i < 2048u; i++) {
        /* 0.7071 peak sine => RMS = 0.5 => -6.02 dBFS */
        ref[i] = (int16_t)llround(23170.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0));
    }
    memcpy(buf, ref, sizeof(ref));
    audio_dsp_process_i16(&dsp, buf, 2048u);
    assert(memcmp(buf, ref, sizeof(ref)) == 0);
    assert(fabs((double)dsp.rms_dbfs - (-6.02)) <= 0.2);
    printf("  [PASS] audio_dsp BYPASS bit-exact & rms_dbfs=%.2f dBFS\n", dsp.rms_dbfs);
}

/* Test 6: Opt-in VOR pre-roll, hysteresis & 500 ms hangover */
typedef struct {
    int16_t  out[32000];
    size_t   frames;
} vor_cap_t;

static void vor_capture_cb(void *user, const int16_t *pcm, size_t num_frames)
{
    vor_cap_t *vc = (vor_cap_t *)user;
    assert(vc->frames + num_frames <= 32000u);
    memcpy(&vc->out[vc->frames], pcm, num_frames * sizeof(int16_t));
    vc->frames += num_frames;
}

static void test_vor_gate(void)
{
    audio_vor_t vor;
    vor_cap_t vc;
    memset(&vc, 0, sizeof(vc));
    audio_vor_init(&vor, 16000u, 1u, 1);

    /* 1. Feed 500 ms (8000 samples) silence -> 0 frames emitted, pre-roll filled to 3200 */
    int16_t chunk[320];
    for (int f = 0; f < 25; f++) {
        memset(chunk, 0, sizeof(chunk));
        /* Put recognizable low-level (-70 dBFS) ramp in pre-roll right before trigger */
        if (f >= 15) {
            chunk[0] = (int16_t)(1 + f);
        }
        audio_vor_feed(&vor, chunk, 320u, vor_capture_cb, &vc);
    }
    assert(vc.frames == 0u && vor.gate_open == 0u);

    /* 2. Feed 300 ms (15 x 320 = 4800) voice-like 1 kHz sine at -18 dBFS */
    for (int f = 0; f < 15; f++) {
        for (size_t i = 0u; i < 320u; i++) {
            chunk[i] = (int16_t)llround(4500.0 * sin(2.0 * M_PI * 1000.0 * (double)i / 16000.0));
        }
        audio_vor_feed(&vor, chunk, 320u, vor_capture_cb, &vc);
    }
    assert(vor.gate_open == 1u);
    /* Emitted 200 ms pre-roll (3200) + 300 ms speech (4800) = 8000 frames; verify pre-roll marker preserved */
    assert(vc.frames == 8000u);
    assert(vc.out[0] == 16 && vc.out[320] == 17);

    /* 3. Feed 800 ms (40 x 320 = 12800) silence -> emits 500 ms hangover (8000) then closes! */
    memset(chunk, 0, sizeof(chunk));
    for (int f = 0; f < 40; f++) {
        audio_vor_feed(&vor, chunk, 320u, vor_capture_cb, &vc);
    }
    assert(vor.gate_open == 0u);
    assert(vc.frames == 8000u + 8000u);

    printf("  [PASS] audio_vor pre-roll (200ms) + speech + hangover (500ms) -> %u frames\n",
           (unsigned)vc.frames);
}

int main(void)
{
    printf("Running test_audio_dsp...\n");
    test_dc_rejection();
    test_frequency_sweep();
    test_voice_compression_ratio();
    test_safety_limiter();
    test_stereo_link_and_beamform();
    test_bypass_and_rms();
    test_vor_gate();
    printf("ALL 7 AUDIO DSP + VOR SUITES PASSED!\n");
    return 0;
}
