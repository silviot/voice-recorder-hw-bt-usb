/*
 * Cassini OpenFW — Headset Button Probe: log formatting, change detection & summary
 *
 * Pure C (no MMIO, no libc) so it is unit-tested on the host (tests/test_hsprobe.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw_hsprobe.h"

static const uint32_t k_prompt_ms[HSP_PROMPT_COUNT] = { 5000u, 12000u, 16000u, 23000u, 27000u };
static const char     k_phase_name[HSP_NPHASE] = { 'A', 'B', 'C' };

/* =========================================================================
 * Line builder & bounded text buffer
 * ========================================================================= */
void hsp_line_reset(hsp_line_t *l)
{
    l->n    = 0u;
    l->s[0] = '\0';
}

void hsp_line_chr(hsp_line_t *l, char c)
{
    if (l->n + 1u < (uint32_t)sizeof(l->s)) {
        l->s[l->n++] = c;
        l->s[l->n]   = '\0';
    }
}

void hsp_line_str(hsp_line_t *l, const char *s)
{
    while (s && *s) {
        hsp_line_chr(l, *s++);
    }
}

void hsp_line_u32(hsp_line_t *l, uint32_t v)
{
    char tmp[10];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < 10);
    while (n > 0) {
        hsp_line_chr(l, tmp[--n]);
    }
}

void hsp_line_i32(hsp_line_t *l, int32_t v)
{
    if (v < 0) {
        hsp_line_chr(l, '-');
        hsp_line_u32(l, (uint32_t)0u - (uint32_t)v);
    } else {
        hsp_line_u32(l, (uint32_t)v);
    }
}

void hsp_line_hex_n(hsp_line_t *l, uint32_t v, unsigned digits)
{
    static const char hx[] = "0123456789ABCDEF";
    if (digits == 0u || digits > 8u) {
        digits = 8u;
    }
    for (unsigned i = digits; i > 0u; i--) {
        hsp_line_chr(l, hx[(v >> ((i - 1u) * 4u)) & 0xFu]);
    }
}

void hsp_line_hex(hsp_line_t *l, uint32_t v)
{
    hsp_line_str(l, "0x");
    hsp_line_hex_n(l, v, 8u);
}

void hsp_text_init(hsp_text_t *t, char *buf, uint32_t cap, uint32_t reserve)
{
    t->buf           = buf;
    t->cap           = cap;
    t->len           = 0u;
    t->reserve       = (reserve < cap) ? reserve : 0u;
    t->dropped_lines = 0u;
}

int hsp_text_line(hsp_text_t *t, const hsp_line_t *l, int use_reserve)
{
    uint32_t limit = use_reserve ? t->cap : (t->cap - t->reserve);
    uint32_t need  = l->n + 1u;
    if (!t->buf || t->len + need > limit) {
        t->dropped_lines++;
        return -1;
    }
    for (uint32_t i = 0u; i < l->n; i++) {
        t->buf[t->len++] = l->s[i];
    }
    t->buf[t->len++] = '\n';
    return 0;
}

/* =========================================================================
 * Schedule helpers
 * ========================================================================= */
uint8_t hsp_prompt_at(uint32_t t_ms)
{
    for (uint32_t i = 0u; i < HSP_PROMPT_COUNT; i++) {
        if (t_ms >= k_prompt_ms[i] && t_ms < k_prompt_ms[i] + HSP_PROMPT_MS) {
            return (uint8_t)(i + 1u);
        }
    }
    return 0u;
}

uint32_t hsp_prompt_start_ms(uint8_t idx)
{
    return (idx >= 1u && idx <= HSP_PROMPT_COUNT) ? k_prompt_ms[idx - 1u] : 0u;
}

uint8_t hsp_phase_at(uint32_t t_ms)
{
    if (t_ms < HSP_T_PHASE_B) return 0u;
    if (t_ms < HSP_T_PHASE_C) return 1u;
    return 2u;
}

/* =========================================================================
 * State
 * ========================================================================= */
static void zero_bytes(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0u; i < n; i++) {
        b[i] = 0u;
    }
}

static void mux_stat_reset(hsp_mux_stat_t *m)
{
    zero_bytes(m, (uint32_t)sizeof(*m));
    m->min1 = 32767;
    m->min2 = 32767;
    m->max1 = -32768;
    m->max2 = -32768;
}

void hsp_init(hsp_state_t *st, char *buf, uint32_t cap, const uint32_t gpio_mask[HSP_NPORT])
{
    zero_bytes(st, (uint32_t)sizeof(*st));
    hsp_text_init(&st->txt, buf, cap, HSP_LOG_RESERVE);
    st->pb11_last = -1;
    st->mux_cur   = 0xFFu;
    for (uint32_t p = 0u; p < HSP_NPORT; p++) {
        st->gpio_mask[p] = gpio_mask ? gpio_mask[p] : 0xFFFFFFFFu;
    }
    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        st->ch[c].min = 0xFFFFu;
        for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
            st->ch[c].min_ph[ph] = 0xFFFFu;
        }
    }
    mux_stat_reset(&st->win);
    for (uint32_t m = 0u; m < HSP_NMUX; m++) {
        mux_stat_reset(&st->mux[m]);
    }
}

static void line_t(hsp_line_t *l, uint32_t t_ms)
{
    hsp_line_reset(l);
    hsp_line_u32(l, t_ms);
    hsp_line_chr(l, ' ');
}

int hsp_comment(hsp_state_t *st, const char *s)
{
    hsp_line_t l;
    hsp_line_reset(&l);
    hsp_line_str(&l, s);
    return hsp_text_line(&st->txt, &l, 0);
}

void hsp_set_phase(hsp_state_t *st, uint32_t t_ms, uint8_t phase, const char *desc)
{
    st->phase = (phase < HSP_NPHASE) ? phase : (uint8_t)(HSP_NPHASE - 1u);
    hsp_line_t l;
    line_t(&l, t_ms);
    hsp_line_str(&l, "PHASE ");
    hsp_line_chr(&l, k_phase_name[st->phase]);
    if (desc && desc[0]) {
        hsp_line_chr(&l, ' ');
        hsp_line_str(&l, desc);
    }
    (void)hsp_text_line(&st->txt, &l, 0);
}

void hsp_mark(hsp_state_t *st, uint32_t t_ms, const char *tag, uint32_t val)
{
    hsp_line_t l;
    line_t(&l, t_ms);
    hsp_line_str(&l, tag);
    hsp_line_chr(&l, ' ');
    hsp_line_u32(&l, val);
    (void)hsp_text_line(&st->txt, &l, 0);
}

/* =========================================================================
 * SARADC change / edge detection
 * ========================================================================= */
static void chan_edge_line(hsp_state_t *st, uint32_t t_ms, char kind, uint32_t c,
                           uint16_t v, int32_t ref, uint32_t len_ms, int with_len)
{
    hsp_chan_t *ch = &st->ch[c];
    if (ch->edge_lines >= HSP_CH_EDGE_BUDGET) {
        ch->suppressed++;
        return;
    }
    ch->edge_lines++;
    hsp_line_t l;
    line_t(&l, t_ms);
    hsp_line_chr(&l, kind);
    hsp_line_u32(&l, c);
    hsp_line_chr(&l, ' ');
    hsp_line_u32(&l, v);
    hsp_line_str(&l, " ref=");
    hsp_line_i32(&l, ref);
    if (with_len) {
        hsp_line_str(&l, " len=");
        hsp_line_u32(&l, len_ms);
    }
    (void)hsp_text_line(&st->txt, &l, 0);
}

static void chan_sample(hsp_state_t *st, uint32_t t_ms, uint32_t c, uint16_t v)
{
    hsp_chan_t *ch = &st->ch[c];
    uint8_t     ph = st->phase;

    if (v < ch->min) ch->min = v;
    if (v > ch->max) ch->max = v;
    if (v < ch->min_ph[ph]) ch->min_ph[ph] = v;
    if (v > ch->max_ph[ph]) ch->max_ph[ph] = v;

    if (!ch->have) {
        ch->have        = 1u;
        ch->last_logged = v;
        ch->ref_q4      = (int32_t)v << 4;
        return; /* first value is reported by the initial snapshot */
    }

    /* 1. Change events (> ±HSP_CHG_TH vs. the last logged value) */
    int32_t d = (int32_t)v - (int32_t)ch->last_logged;
    if (d > HSP_CHG_TH || d < -HSP_CHG_TH) {
        ch->last_logged = v;
        if (ch->events < HSP_CH_EVT_BUDGET) {
            ch->events++;
            hsp_line_t l;
            line_t(&l, t_ms);
            hsp_line_chr(&l, 'K');
            hsp_line_u32(&l, c);
            hsp_line_chr(&l, ' ');
            hsp_line_u32(&l, v);
            (void)hsp_text_line(&st->txt, &l, 0);
        } else {
            ch->suppressed++;
        }
    }

    /* 2. Large drop / rise vs. a slow baseline (hysteresis TH / TH/2) */
    int32_t ref = ch->ref_q4 >> 4;
    int32_t vi  = (int32_t)v;
    if (ch->edge == 0u) {
        if (vi + HSP_EDGE_TH <= ref) {
            ch->edge     = 1u;
            ch->edge_len = 0u;
            ch->drops[ph]++;
            chan_edge_line(st, t_ms, 'D', c, v, ref, 0u, 0);
        } else if (vi >= ref + HSP_EDGE_TH) {
            ch->edge     = 2u;
            ch->edge_len = 0u;
            ch->rises[ph]++;
            chan_edge_line(st, t_ms, 'U', c, v, ref, 0u, 0);
        } else {
            ch->ref_q4 += (((int32_t)v << 4) - ch->ref_q4) >> 4;
        }
    } else {
        ch->edge_len++;
        int back = (ch->edge == 1u) ? (vi + HSP_EDGE_TH / 2 > ref)
                                    : (vi < ref + HSP_EDGE_TH / 2);
        if (back) {
            chan_edge_line(st, t_ms, 'E', c, v, ref, ch->edge_len * HSP_SAMPLE_MS, 1);
            ch->edge = 0u;
        } else if (ch->edge_len >= HSP_EDGE_LONG_SMP) {
            /* Stuck for 3 s: treat as a level shift (e.g. plug event) and re-baseline. */
            chan_edge_line(st, t_ms, 'R', c, v, ref, ch->edge_len * HSP_SAMPLE_MS, 1);
            ch->edge   = 0u;
            ch->ref_q4 = (int32_t)v << 4;
        }
    }
}

static void gpio_sample(hsp_state_t *st, uint32_t t_ms, const uint32_t gpio[HSP_NPORT])
{
    if (!st->gpio_have) {
        for (uint32_t p = 0u; p < HSP_NPORT; p++) {
            st->gpio_last[p] = gpio[p];
        }
        st->gpio_have = 1u;
        return;
    }
    for (uint32_t p = 0u; p < HSP_NPORT; p++) {
        uint32_t x = (gpio[p] ^ st->gpio_last[p]) & st->gpio_mask[p];
        st->gpio_last[p] = gpio[p];
        if (x == 0u) {
            continue;
        }
        uint32_t newly_noisy = 0u;
        for (uint32_t b = 0u; b < 32u; b++) {
            if (x & (1u << b)) {
                if (st->gpio_toggles[p][b] < 255u) {
                    st->gpio_toggles[p][b]++;
                }
                if (st->gpio_toggles[p][b] > HSP_GPIO_NOISY) {
                    newly_noisy |= (1u << b);
                }
            }
        }
        st->gpio_changed_bits[p] |= x & ~newly_noisy;
        hsp_line_t l;
        if (st->gpio_events < HSP_GPIO_EVT_BUDGET) {
            st->gpio_events++;
            line_t(&l, t_ms);
            hsp_line_chr(&l, 'G');
            hsp_line_chr(&l, (char)('A' + p));
            hsp_line_chr(&l, ' ');
            hsp_line_hex_n(&l, gpio[p], 8u);
            hsp_line_str(&l, " x=");
            hsp_line_hex_n(&l, x, 8u);
            (void)hsp_text_line(&st->txt, &l, 0);
        } else {
            st->gpio_suppressed++;
        }
        if (newly_noisy) {
            st->gpio_mask[p] &= ~newly_noisy;
            st->gpio_changed_bits[p] &= ~newly_noisy;
            line_t(&l, t_ms);
            hsp_line_str(&l, "GM");
            hsp_line_chr(&l, (char)('A' + p));
            hsp_line_str(&l, " noisy=");
            hsp_line_hex_n(&l, newly_noisy, 8u);
            (void)hsp_text_line(&st->txt, &l, 0);
        }
    }
}

void hsp_sample(hsp_state_t *st, uint32_t t_ms, const uint16_t v[HSP_NCH], int pb11,
                const uint32_t gpio[HSP_NPORT])
{
    uint8_t ph = st->phase;
    st->samples++;
    st->phase_samples[ph]++;

    if (pb11 >= 0) {
        int lvl = pb11 ? 1 : 0;
        if (lvl == 0) {
            st->pb11_low_samples[ph]++;
        }
        if (st->pb11_last >= 0 && lvl != st->pb11_last) {
            st->pb11_changes[ph]++;
            hsp_line_t l;
            line_t(&l, t_ms);
            hsp_line_str(&l, "P ");
            hsp_line_u32(&l, (uint32_t)lvl);
            (void)hsp_text_line(&st->txt, &l, 0);
        }
        st->pb11_last = (int8_t)lvl;
    }

    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        chan_sample(st, t_ms, c, (uint16_t)(v[c] & 0x0FFFu));
    }

    if (gpio) {
        gpio_sample(st, t_ms, gpio);
    }
}

void hsp_snapshot(hsp_state_t *st, uint32_t t_ms, const uint16_t v[HSP_NCH], int pb11)
{
    hsp_line_t l;
    line_t(&l, t_ms);
    hsp_line_str(&l, "S ");
    if (pb11 >= 0) {
        hsp_line_u32(&l, pb11 ? 1u : 0u);
    } else {
        hsp_line_chr(&l, '-');
    }
    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, (uint32_t)(v[c] & 0x0FFFu));
    }
    (void)hsp_text_line(&st->txt, &l, 0);
}

/* =========================================================================
 * Codec ADC statistics
 * ========================================================================= */
static int32_t div_round(int32_t sum, uint32_t n)
{
    if (n == 0u) {
        return 0;
    }
    return (sum >= 0) ? (int32_t)(((uint32_t)sum + n / 2u) / n)
                      : -(int32_t)(((uint32_t)(-sum) + n / 2u) / n);
}

static void line_mmm(hsp_line_t *l, const char *tag, int32_t mn, int32_t mx, int32_t mean)
{
    hsp_line_str(l, tag);
    hsp_line_i32(l, mn);
    hsp_line_chr(l, '/');
    hsp_line_i32(l, mx);
    hsp_line_chr(l, '/');
    hsp_line_i32(l, mean);
}

void hsp_codec_window_begin(hsp_state_t *st, uint32_t t_ms, uint8_t mux)
{
    (void)t_ms;
    st->mux_cur     = (uint8_t)(mux & 0x0Fu);
    st->a_have_prev = 0u;
    mux_stat_reset(&st->win);
}

void hsp_codec_block(hsp_state_t *st, uint32_t t_ms, const int16_t *pcm, uint32_t frames,
                     uint8_t channels)
{
    if (st->mux_cur == 0xFFu || !pcm || frames == 0u || channels == 0u) {
        return;
    }
    int32_t mn1 = 32767, mx1 = -32768, mn2 = 32767, mx2 = -32768;
    int32_t s1 = 0, s2 = 0;
    for (uint32_t i = 0u; i < frames; i++) {
        int32_t a = pcm[i * channels];
        int32_t b = (channels >= 2u) ? pcm[i * channels + 1u] : 0;
        if (a < mn1) mn1 = a;
        if (a > mx1) mx1 = a;
        if (b < mn2) mn2 = b;
        if (b > mx2) mx2 = b;
        s1 += a;
        s2 += b;
    }
    int32_t m1 = div_round(s1, frames);
    int32_t m2 = div_round(s2, frames);
    st->a_blocks++;

    hsp_mux_stat_t *w = &st->win;
    if (mn1 < w->min1) w->min1 = (int16_t)mn1;
    if (mx1 > w->max1) w->max1 = (int16_t)mx1;
    if (mn2 < w->min2) w->min2 = (int16_t)mn2;
    if (mx2 > w->max2) w->max2 = (int16_t)mx2;
    if (mx1 - mn1 > w->p2p1_max) w->p2p1_max = mx1 - mn1;
    if (mx2 - mn2 > w->p2p2_max) w->p2p2_max = mx2 - mn2;
    if (w->n + frames <= 60000u) {
        w->sum1 += s1;
        w->sum2 += s2;
        w->n    += frames;
    }

    int ev = (mx1 - mn1 >= HSP_A_P2P_EVT) || (mx2 - mn2 >= HSP_A_P2P_EVT);
    if (st->a_have_prev) {
        int32_t d1 = m1 - st->a_prev_mean1;
        int32_t d2 = m2 - st->a_prev_mean2;
        if (d1 >= HSP_A_STEP_EVT || d1 <= -HSP_A_STEP_EVT ||
            d2 >= HSP_A_STEP_EVT || d2 <= -HSP_A_STEP_EVT) {
            ev = 1;
        }
    }
    st->a_prev_mean1 = m1;
    st->a_prev_mean2 = m2;
    st->a_have_prev  = 1u;

    if (ev) {
        w->events++;
        if (st->a_events < HSP_A_EVT_BUDGET) {
            st->a_events++;
            hsp_line_t l;
            line_t(&l, t_ms);
            hsp_line_str(&l, "AE mux=");
            hsp_line_u32(&l, st->mux_cur);
            line_mmm(&l, " c1=", mn1, mx1, m1);
            line_mmm(&l, " c2=", mn2, mx2, m2);
            (void)hsp_text_line(&st->txt, &l, 0);
        } else {
            st->a_suppressed++;
        }
    }
}

void hsp_codec_window_end(hsp_state_t *st, uint32_t t_ms)
{
    if (st->mux_cur == 0xFFu) {
        return;
    }
    hsp_mux_stat_t *w = &st->win;
    hsp_mux_stat_t *m = &st->mux[st->mux_cur];
    hsp_line_t l;
    line_t(&l, t_ms);
    hsp_line_str(&l, "A mux=");
    hsp_line_u32(&l, st->mux_cur);
    hsp_line_str(&l, " n=");
    hsp_line_u32(&l, w->n);
    if (w->n > 0u) {
        int32_t m1 = div_round(w->sum1, w->n);
        int32_t m2 = div_round(w->sum2, w->n);
        line_mmm(&l, " c1=", w->min1, w->max1, m1);
        line_mmm(&l, " c2=", w->min2, w->max2, m2);
        hsp_line_str(&l, " p2p=");
        hsp_line_i32(&l, w->p2p1_max);
        hsp_line_chr(&l, '/');
        hsp_line_i32(&l, w->p2p2_max);

        if (w->min1 < m->min1) m->min1 = w->min1;
        if (w->max1 > m->max1) m->max1 = w->max1;
        if (w->min2 < m->min2) m->min2 = w->min2;
        if (w->max2 > m->max2) m->max2 = w->max2;
        if (w->p2p1_max > m->p2p1_max) m->p2p1_max = w->p2p1_max;
        if (w->p2p2_max > m->p2p2_max) m->p2p2_max = w->p2p2_max;
        m->sum1 += m1;
        m->sum2 += m2;
        m->windows++;
    }
    m->events = (uint16_t)(m->events + w->events);
    (void)hsp_text_line(&st->txt, &l, 0);
    st->mux_cur = 0xFFu;
}

/* =========================================================================
 * Summary
 * ========================================================================= */
static void sum_line(hsp_state_t *st, const hsp_line_t *l)
{
    (void)hsp_text_line(&st->txt, l, 1);
}

void hsp_summary(hsp_state_t *st, uint32_t t_end_ms, const char *end_reason)
{
    hsp_line_t l;
    hsp_line_reset(&l);
    hsp_line_str(&l, "# ---- summary ----");
    sum_line(st, &l);

    hsp_line_reset(&l);
    hsp_line_str(&l, "SUM samples=");
    hsp_line_u32(&l, st->samples);
    for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
        hsp_line_chr(&l, ' ');
        hsp_line_chr(&l, k_phase_name[ph]);
        hsp_line_chr(&l, '=');
        hsp_line_u32(&l, st->phase_samples[ph]);
    }
    sum_line(st, &l);

    hsp_line_reset(&l);
    hsp_line_str(&l, "# SUM K<ch> min max events suppressed drops=A/B/C rises=A/B/C range=minA-maxA/minB-maxB/minC-maxC");
    sum_line(st, &l);
    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        const hsp_chan_t *ch = &st->ch[c];
        hsp_line_reset(&l);
        hsp_line_str(&l, "SUM K");
        hsp_line_u32(&l, c);
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, ch->have ? ch->min : 0u);
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, ch->have ? ch->max : 0u);
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, ch->events);
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, ch->suppressed);
        hsp_line_str(&l, " drops=");
        for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
            if (ph) hsp_line_chr(&l, '/');
            hsp_line_u32(&l, ch->drops[ph]);
        }
        hsp_line_str(&l, " rises=");
        for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
            if (ph) hsp_line_chr(&l, '/');
            hsp_line_u32(&l, ch->rises[ph]);
        }
        hsp_line_str(&l, " range=");
        for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
            if (ph) hsp_line_chr(&l, '/');
            if (ch->min_ph[ph] == 0xFFFFu) {
                hsp_line_chr(&l, '-');
            } else {
                hsp_line_u32(&l, ch->min_ph[ph]);
                hsp_line_chr(&l, '-');
                hsp_line_u32(&l, ch->max_ph[ph]);
            }
        }
        sum_line(st, &l);
    }

    hsp_line_reset(&l);
    hsp_line_str(&l, "SUM P11 changes=");
    for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
        if (ph) hsp_line_chr(&l, '/');
        hsp_line_u32(&l, st->pb11_changes[ph]);
    }
    hsp_line_str(&l, " low_pct=");
    for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
        if (ph) hsp_line_chr(&l, '/');
        uint32_t n = st->phase_samples[ph];
        hsp_line_u32(&l, n ? (st->pb11_low_samples[ph] * 100u + n / 2u) / n : 0u);
    }
    sum_line(st, &l);

    hsp_line_reset(&l);
    hsp_line_str(&l, "SUM GPIO changed=");
    for (uint32_t p = 0u; p < HSP_NPORT; p++) {
        if (p) hsp_line_chr(&l, '/');
        hsp_line_hex_n(&l, st->gpio_changed_bits[p], 8u);
    }
    hsp_line_str(&l, " mask=");
    for (uint32_t p = 0u; p < HSP_NPORT; p++) {
        if (p) hsp_line_chr(&l, '/');
        hsp_line_hex_n(&l, st->gpio_mask[p], 8u);
    }
    hsp_line_str(&l, " events=");
    hsp_line_u32(&l, st->gpio_events);
    hsp_line_str(&l, " suppressed=");
    hsp_line_u32(&l, st->gpio_suppressed);
    sum_line(st, &l);

    hsp_line_reset(&l);
    hsp_line_str(&l, "# SUM MUX<m> windows p2p=c1/c2 mean=c1/c2 c2range=min..max block_events");
    sum_line(st, &l);
    for (uint32_t m = 0u; m < HSP_NMUX; m++) {
        const hsp_mux_stat_t *ms = &st->mux[m];
        if (ms->windows == 0u) {
            continue;
        }
        hsp_line_reset(&l);
        hsp_line_str(&l, "SUM MUX");
        hsp_line_u32(&l, m);
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, ms->windows);
        hsp_line_str(&l, " p2p=");
        hsp_line_i32(&l, ms->p2p1_max);
        hsp_line_chr(&l, '/');
        hsp_line_i32(&l, ms->p2p2_max);
        hsp_line_str(&l, " mean=");
        hsp_line_i32(&l, div_round(ms->sum1, ms->windows));
        hsp_line_chr(&l, '/');
        hsp_line_i32(&l, div_round(ms->sum2, ms->windows));
        hsp_line_str(&l, " c2range=");
        hsp_line_i32(&l, ms->min2);
        hsp_line_str(&l, "..");
        hsp_line_i32(&l, ms->max2);
        hsp_line_chr(&l, ' ');
        hsp_line_u32(&l, ms->events);
        sum_line(st, &l);
    }
    hsp_line_reset(&l);
    hsp_line_str(&l, "SUM A blocks=");
    hsp_line_u32(&l, st->a_blocks);
    hsp_line_str(&l, " events=");
    hsp_line_u32(&l, st->a_events);
    hsp_line_str(&l, " suppressed=");
    hsp_line_u32(&l, st->a_suppressed);
    sum_line(st, &l);

    /* Candidate lines: channels with a plausible number of discrete press-like edges. */
    hsp_line_reset(&l);
    hsp_line_str(&l, "HINT candidates=");
    int any = 0;
    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        const hsp_chan_t *ch = &st->ch[c];
        uint32_t e = 0u;
        for (uint32_t ph = 0u; ph < HSP_NPHASE; ph++) {
            e += (uint32_t)ch->drops[ph] + (uint32_t)ch->rises[ph];
        }
        if (e >= 2u && e <= 40u) {
            if (any) hsp_line_chr(&l, ',');
            hsp_line_chr(&l, 'K');
            hsp_line_u32(&l, c);
            hsp_line_chr(&l, ':');
            hsp_line_u32(&l, e);
            any = 1;
        }
    }
    for (uint32_t p = 0u; p < HSP_NPORT; p++) {
        if (st->gpio_changed_bits[p]) {
            if (any) hsp_line_chr(&l, ',');
            hsp_line_chr(&l, 'G');
            hsp_line_chr(&l, (char)('A' + p));
            hsp_line_chr(&l, ':');
            hsp_line_hex_n(&l, st->gpio_changed_bits[p], 8u);
            any = 1;
        }
    }
    if (!any) {
        hsp_line_str(&l, "none");
    }
    hsp_line_str(&l, " (K1 = key ladder)");
    sum_line(st, &l);

    hsp_line_reset(&l);
    hsp_line_str(&l, "SUM LOG used=");
    hsp_line_u32(&l, st->txt.len);
    hsp_line_str(&l, " cap=");
    hsp_line_u32(&l, st->txt.cap);
    hsp_line_str(&l, " dropped_lines=");
    hsp_line_u32(&l, st->txt.dropped_lines);
    hsp_line_str(&l, " overflow=");
    hsp_line_u32(&l, st->txt.dropped_lines ? 1u : 0u);
    sum_line(st, &l);

    hsp_line_reset(&l);
    hsp_line_str(&l, "END t=");
    hsp_line_u32(&l, t_end_ms);
    hsp_line_str(&l, " reason=");
    hsp_line_str(&l, end_reason ? end_reason : "?");
    if (hsp_text_line(&st->txt, &l, 1) != 0 && st->txt.len > 0u) {
        /* Never lose the END marker: overwrite the tail if the reserve ran out. */
        uint32_t need = l.n + 1u;
        if (need <= st->txt.cap) {
            uint32_t cut = st->txt.cap - need;
            if (cut > st->txt.len) {
                cut = st->txt.len;
            }
            while (cut > 0u && st->txt.buf[cut - 1u] != '\n') {
                cut--; /* keep whole lines only */
            }
            st->txt.len = cut;
            (void)hsp_text_line(&st->txt, &l, 1);
        }
    }
}
