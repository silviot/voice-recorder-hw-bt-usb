/* Shared helpers for the headphones-mode host tests (test_hp_sink.c, test_hp_tx.c): tone generation and a
 * least-squares tone fit for SNR. SPDX-License-Identifier: MIT */
#ifndef HP_TEST_UTIL_H
#define HP_TEST_UTIL_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static inline int16_t tone_sample(double f, double fs, double amp, uint64_t n)
{
    return (int16_t)lrint(amp * 32767.0 * sin(2.0 * M_PI * f * (double)n / fs));
}

/* SNR (dB) of x[0..n) (stride samples apart) against the best-fit A cos + B sin + C at frequency f. */
static inline double tone_snr(const int16_t *x, size_t n, size_t stride, double f, double fs, double *amp_out)
{
    double scc = 0, sss = 0, ssc = 0, sc = 0, ss = 0, s1 = (double)n, ycc = 0, yss = 0, y1 = 0;
    for (size_t i = 0; i < n; i++) {
        double c = cos(2.0 * M_PI * f * (double)i / fs), s = sin(2.0 * M_PI * f * (double)i / fs), y = x[i * stride];
        scc += c * c; sss += s * s; ssc += s * c; sc += c; ss += s; ycc += y * c; yss += y * s; y1 += y;
    }
    /* 3x3 normal equations [scc ssc sc; ssc sss ss; sc ss s1] [A B C]' = [ycc yss y1]' (Cramer) */
    double m[3][3] = { { scc, ssc, sc }, { ssc, sss, ss }, { sc, ss, s1 } }, r[3] = { ycc, yss, y1 }, sol[3];
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    for (int k = 0; k < 3; k++) {
        double t[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i][j] = (j == k) ? r[i] : m[i][j];
        sol[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                  t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double c = cos(2.0 * M_PI * f * (double)i / fs), s = sin(2.0 * M_PI * f * (double)i / fs);
        double fit = sol[0] * c + sol[1] * s + sol[2], e = x[i * stride] - fit;
        sig += (fit - sol[2]) * (fit - sol[2]);
        err += e * e;
    }
    if (amp_out) *amp_out = sqrt(sol[0] * sol[0] + sol[1] * sol[1]) / 32767.0;
    return err > 0 ? 10.0 * log10(sig / err) : 200.0;
}
#endif
