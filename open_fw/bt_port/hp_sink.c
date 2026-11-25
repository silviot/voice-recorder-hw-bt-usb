/* Headphones-mode PCM sink: SPSC ring, flush index, resampler (docs/BT_A2DP_DESIGN.md §3.3-3.4). See hp_sink.h.
 * SPDX-License-Identifier: MIT */
#include "hp_sink.h"
#include <string.h>
#include "hp_rs_taps.h"

_Static_assert((HP_RS_HIST & (HP_RS_HIST - 1u)) == 0u, "history must be a power of two");
_Static_assert(HP_RS_HIST >= HP_RS3_TAPS && HP_RS_HIST >= HP_RSG_TAPS, "history shorter than the filters");

static inline int16_t sat16(int64_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* ------------------------------------------------------------------------------------------------ resampler */

/* Exact 3x: one output = sum_j g[j] * x[m - j] over 24 taps, rounded and saturated to Q0. A 32-bit accumulator is
 * exact: every partial sum is bounded by sum|g| * 32768 <= 58580 * 32768 = 1.92e9 (hp_rs_taps.h phase 1; test_hp_sink
 * checks the bound), so this equals the int64 reference bit for bit (HP_SELFTEST_GOLDEN unchanged). On the Cortex-M4
 * the taps go two at a time through SMLADX (g[j] * x[m-j] + g[j+1] * x[m-j-1] from one word of each): 12 dual MACs
 * instead of 24 MACs plus 64-bit bookkeeping, about a quarter of the BTC tick's per-frame instructions (silicon
 * 2026-11-25, qemu-arm instruction count: 208 -> ~60 per output). */
static inline int16_t x3_dot(const int16_t *g, const int16_t *h)
{
    int32_t acc = 1 << 14;
#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP
    typedef int32_t pair_t __attribute__((aligned(2), may_alias));   /* one LDR (unaligned is fine on the M4) */
    _Static_assert(HP_RS3_TAPS == 24, "x3_dot is unrolled for 24 taps");
    /* taps j, j+1: lo g[j] hi g[j+1] against lo x[m-j-1] hi x[m-j]; unrolled (-Os keeps loops rolled) */
#define X3_PAIR(j) acc = __builtin_arm_smladx(*(const pair_t *)&g[j], *(const pair_t *)&h[-(j) - 1], acc)
    X3_PAIR(0);  X3_PAIR(2);  X3_PAIR(4);  X3_PAIR(6);  X3_PAIR(8);  X3_PAIR(10);
    X3_PAIR(12); X3_PAIR(14); X3_PAIR(16); X3_PAIR(18); X3_PAIR(20); X3_PAIR(22);
#undef X3_PAIR
    return (int16_t)__builtin_arm_ssat(acc >> 15, 16);
#else
    for (uint32_t j = 0; j < HP_RS3_TAPS; j++) {
        acc += (int32_t)g[j] * h[-(int32_t)j];
    }
    acc >>= 15;
    return (int16_t)(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
#endif
}

void hp_rs_reset(hp_rs_t *rs, uint32_t src, uint32_t dst, uint8_t ch)
{
    memset(rs, 0, sizeof(*rs));
    rs->src = src;
    rs->dst = dst;
    rs->ch = (ch == 1u) ? 1u : 2u;
    rs->x3_phase = 3u;
    if (src == 0u || dst == 0u || src == dst) {
        rs->mode = HP_RS_COPY;
    } else if (3u * src == dst) {
        rs->mode = HP_RS_X3;
    } else {
        rs->mode = HP_RS_GEN;
        rs->step_q32 = ((uint64_t)src << 32) / dst;
        rs->down = (src > dst) ? 1u : 0u;
    }
}

static inline void rs_push(hp_rs_t *rs, const int16_t *x)
{
    uint32_t w = rs->widx;
    for (uint32_t c = 0; c < rs->ch; c++) {
        rs->hist[c][w] = x[c];
        rs->hist[c][w + HP_RS_HIST] = x[c];
    }
    rs->widx = (w + 1u) & (HP_RS_HIST - 1u);
}

uint32_t hp_rs_process(hp_rs_t *rs, const int16_t *in, uint32_t n_in, uint32_t *used_in, int16_t *out, uint32_t n_out)
{
    uint32_t used = 0, made = 0;
    const uint32_t ch = rs->ch;

    if (rs->mode == HP_RS_COPY) {
        made = n_in < n_out ? n_in : n_out;
        memcpy(out, in, (size_t)made * ch * sizeof(int16_t));
        *used_in = made;
        return made;
    }
    if (rs->mode == HP_RS_X3 && ch == 1u) {
        /* mono (the recorder's format): locals instead of the state struct in the per-output loop */
        uint32_t phase = rs->x3_phase, w = rs->widx;
        int16_t *hist = rs->hist[0];
        while (made < n_out) {
            if (phase >= 3u) {
                if (used == n_in) {
                    break;
                }
                int16_t x = in[used++];
                hist[w] = x;
                hist[w + HP_RS_HIST] = x;
                w = (w + 1u) & (HP_RS_HIST - 1u);
                phase = 0u;
            }
            out[made++] = x3_dot(hp_rs3_taps[phase], &hist[w + HP_RS_HIST - 1u]);
            phase++;
        }
        rs->x3_phase = (uint8_t)phase;
        rs->widx = w;
        *used_in = used;
        return made;
    }
    if (rs->mode == HP_RS_X3) {
        while (made < n_out) {
            if (rs->x3_phase >= 3u) {
                if (used == n_in) {
                    break;
                }
                rs_push(rs, in + (size_t)used * ch);
                used++;
                rs->x3_phase = 0u;
            }
            const int16_t *g = hp_rs3_taps[rs->x3_phase];
            for (uint32_t c = 0; c < ch; c++) {
                const int16_t *h = &rs->hist[c][rs->widx + HP_RS_HIST - 1u];   /* x[m], then x[m - j] */
                out[(size_t)made * ch + c] = x3_dot(g, h);
            }
            made++;
            rs->x3_phase++;
        }
        *used_in = used;
        return made;
    }
    /* HP_RS_GEN: output at input time n + mu from the newest 16 inputs x[n-7 .. n+8] (8 inputs lookahead). */
    const int16_t (*tab)[HP_RSG_TAPS] = rs->down ? hp_rsg_down_taps : hp_rsg_up_taps;
    while (made < n_out) {
        while (rs->ahead < 8u) {
            if (used == n_in) {
                *used_in = used;
                return made;
            }
            rs_push(rs, in + (size_t)used * ch);
            used++;
            rs->ahead++;
        }
        uint32_t frac = (uint32_t)rs->frac_q32;
        uint32_t p = frac >> 26;                       /* 64 phases */
        int32_t  f = (int32_t)((frac >> 10) & 0xFFFFu); /* position between phase p and p + 1, Q16 */
        const int16_t *t0 = tab[p], *t1 = tab[p + 1u];
        int32_t coef[HP_RSG_TAPS];
        for (uint32_t j = 0; j < HP_RSG_TAPS; j++) {
            coef[j] = t0[j] + (int32_t)(((int32_t)(t1[j] - t0[j]) * f) >> 16);
        }
        for (uint32_t c = 0; c < ch; c++) {
            const int16_t *x = &rs->hist[c][rs->widx + HP_RS_HIST - HP_RSG_TAPS];   /* x[n-7] .. x[n+8] */
            int64_t acc = 0;
            for (uint32_t j = 0; j < HP_RSG_TAPS; j++) {
                acc += coef[j] * x[j];
            }
            out[(size_t)made * ch + c] = sat16((acc + (1 << 14)) >> 15);
        }
        made++;
        uint64_t pos = (rs->frac_q32 & 0xFFFFFFFFull) + rs->step_q32;
        rs->ahead -= (uint32_t)(pos >> 32);
        rs->frac_q32 = pos & 0xFFFFFFFFull;
    }
    *used_in = used;
    return made;
}

/* ------------------------------------------------------------------------------------------------ ring */

void hp_sink_init(hp_sink_t *s, int16_t *buf, uint32_t buf_bytes)
{
    memset(s, 0, sizeof(*s));
    uint32_t samples = buf_bytes / 2u, n = 1u;
    while (n * 2u <= samples) {
        n *= 2u;
    }
    s->buf = (buf && samples >= 2u) ? buf : 0;
    s->samples = s->buf ? n : 0u;
    s->cap = s->samples / 2u;
    s->c_cap = s->cap;
    s->gain_q15 = 32768u;
    s->p_ch = 2u;
    s->fmt_ch = 2u;
    s->c_ch = 2u;
    hp_rs_reset(&s->rs, 0, 0, 2u);
}

/* Shared fields use __atomic loads/stores (plain LDR/STR + DMB on Cortex-M4; visible to TSan on the host). The ring
 * slots are accessed with relaxed atomics too: a consumer copy that races a post-flush write is detected by the
 * generation re-check (seqlock) and discarded, so it must not be a C data race. */
static inline int16_t slot_ld(const int16_t *p) { return HP_LD(p); }
static inline void slot_st(int16_t *p, int16_t v) { HP_ST(p, v); }

/* Read index the headset path is at, as the producer sees it: max(rd, flush_wr). */
static uint32_t bt_read_index(const hp_sink_t *s)
{
    uint32_t r = HP_LDA(&s->rd), f = HP_LD(&s->flush_wr);
    return ((int32_t)(f - r) > 0) ? f : r;
}

/* Oldest frame still needed by a reader (producer view). */
static uint32_t oldest(const hp_sink_t *s)
{
    uint32_t o = bt_read_index(s);
    if (s->tap_on && (int32_t)(s->tap_rd - o) < 0) {
        o = s->tap_rd;
    }
    return o;
}

static void publish_flush(hp_sink_t *s)
{
    uint32_t w = HP_LD(&s->wr);
    HP_ST(&s->flush_wr, w);
    s->tap_rd = w;
    HP_STR(&s->flush_gen, HP_LD(&s->flush_gen) + 1u);   /* flush_wr / fmt_* visible with the new generation */
    HP_FENCE();                                          /* ... and before any post-flush slot write (seqlock) */
}

void hp_sink_set_format(hp_sink_t *s, uint32_t src_rate, uint8_t ch)
{
    s->p_rate = src_rate;
    s->p_ch = (ch == 1u) ? 1u : 2u;
    s->cap = s->samples / s->p_ch;   /* 16 KiB: 4096 stereo or 8192 mono frames (§3.3) */
    HP_ST(&s->fmt_rate, src_rate);
    HP_ST(&s->fmt_ch, s->p_ch);
    publish_flush(s);
}

void hp_sink_flush(hp_sink_t *s)
{
    publish_flush(s);
}

uint32_t hp_sink_write(hp_sink_t *s, const int16_t *pcm, uint32_t n, uint8_t ch)
{
    if (!s->buf || !pcm || n == 0u) {
        return 0u;
    }
    const uint8_t rch = s->p_ch;
    if (!(ch == rch || (ch == 2u && rch == 1u))) {
        s->ch_mismatch++;
        return 0u;
    }
    uint32_t w = HP_LD(&s->wr);
    uint32_t used = w - oldest(s);
    uint32_t space = used >= s->cap ? 0u : s->cap - used;
    uint32_t k = n < space ? n : space;
    const uint32_t mask = s->cap - 1u;
    for (uint32_t i = 0; i < k; i++) {
        int16_t *d = &s->buf[((w + i) & mask) * rch];
        if (rch == 2u) {
            slot_st(&d[0], pcm[2u * i]);
            slot_st(&d[1], pcm[2u * i + 1u]);
        } else if (ch == 1u) {
            slot_st(&d[0], pcm[i]);
        } else {
            slot_st(&d[0], (int16_t)(((int32_t)pcm[2u * i] + pcm[2u * i + 1u]) >> 1));
        }
    }
    HP_STR(&s->wr, w + k);   /* payload before index */
    return k;
}

uint32_t hp_sink_queued(const hp_sink_t *s)
{
    uint32_t q = HP_LD(&s->wr) - bt_read_index(s);
    if (HP_LD(&s->c_gen) == HP_LD(&s->flush_gen)) {
        q += HP_LD(&s->c_held);
    }
    return q;
}

uint32_t hp_sink_free(const hp_sink_t *s)
{
    uint32_t used = HP_LD(&s->wr) - oldest(s);
    return used >= s->cap ? 0u : s->cap - used;
}

void hp_sink_set_mute(hp_sink_t *s, int muted)
{
    HP_ST(&s->mute, (uint8_t)(muted ? 1u : 0u));
}

void hp_sink_set_gain_q15(hp_sink_t *s, uint16_t g)
{
    HP_ST(&s->gain_q15, (uint16_t)(g > 32768u ? 32768u : g));
}

uint32_t hp_sink_src_rate(const hp_sink_t *s)
{
    return s->p_rate;
}

uint8_t hp_sink_channels(const hp_sink_t *s)
{
    return s->p_ch;
}

void hp_sink_tap_enable(hp_sink_t *s, int on)
{
    s->tap_rd = HP_LD(&s->wr);
    s->tap_on = on ? 1u : 0u;
}

uint32_t hp_sink_tap_avail(const hp_sink_t *s)
{
    return s->tap_on ? HP_LD(&s->wr) - s->tap_rd : 0u;
}

uint32_t hp_sink_tap_peek(const hp_sink_t *s, int16_t *out_lr, uint32_t max)
{
    if (!s->tap_on || !s->buf) {
        return 0u;
    }
    uint32_t avail = HP_LD(&s->wr) - s->tap_rd, k = avail < max ? avail : max;
    const uint32_t mask = s->cap - 1u;
    for (uint32_t i = 0; i < k; i++) {
        const int16_t *src = &s->buf[((s->tap_rd + i) & mask) * s->p_ch];
        int16_t l = slot_ld(&src[0]);
        out_lr[2u * i] = l;
        out_lr[2u * i + 1u] = (s->p_ch == 2u) ? slot_ld(&src[1]) : l;
    }
    return k;
}

void hp_sink_tap_advance(hp_sink_t *s, uint32_t n)
{
    uint32_t avail = HP_LD(&s->wr) - s->tap_rd;
    s->tap_rd += n < avail ? n : avail;
}

/* ------------------------------------------------------------------------------------------------ consumer */

static void consumer_reset_rs(hp_sink_t *s)
{
    hp_rs_reset(&s->rs, s->c_rate, s->c_out_rate, s->c_ch);
}

void hp_sink_set_out_rate(hp_sink_t *s, uint32_t conn_rate)
{
    s->c_out_rate = conn_rate;
    consumer_reset_rs(s);
}

/* Apply a pending flush / format change. */
static void consumer_sync(hp_sink_t *s)
{
    uint32_t g = HP_LDA(&s->flush_gen);
    if (g == s->c_seen_gen) {
        return;
    }
    uint32_t f = HP_LD(&s->flush_wr), r = HP_LD(&s->rd);
    s->c_rate = HP_LD(&s->fmt_rate);
    s->c_ch = HP_LD(&s->fmt_ch);
    s->c_cap = s->samples / s->c_ch;
    if ((int32_t)(f - r) > 0) {
        HP_STR(&s->rd, f);
    }
    s->chunk_pos = s->chunk_len = 0u;
    HP_ST(&s->c_held, 0u);
    consumer_reset_rs(s);
    s->c_seen_gen = g;
    HP_ST(&s->c_gen, g);
}

uint32_t hp_sink_sync(hp_sink_t *s)
{
    consumer_sync(s);
    return s->c_seen_gen;
}

/* Refill the chunk from the ring. 0 = ring empty. */
static int consumer_refill(hp_sink_t *s)
{
    for (;;) {
        consumer_sync(s);
        uint32_t w = HP_LDA(&s->wr);
        uint32_t r = HP_LD(&s->rd), avail = w - r;
        if (avail == 0u || (int32_t)avail < 0) {
            return 0;
        }
        uint32_t k = avail < HP_SINK_CHUNK ? avail : HP_SINK_CHUNK;
        const uint32_t mask = s->c_cap - 1u, ch = s->c_ch;
        for (uint32_t i = 0; i < k; i++) {
            const int16_t *src = &s->buf[((r + i) & mask) * ch];
            s->chunk[i * ch] = slot_ld(&src[0]);
            if (ch == 2u) {
                s->chunk[i * ch + 1u] = slot_ld(&src[1]);
            }
        }
        HP_FENCE();   /* seqlock reader: slot loads before the generation re-check */
        if (HP_LD(&s->flush_gen) != s->c_seen_gen) {
            s->torn_chunks++;   /* a flush raced the copy: the slots may hold post-flush data; drop and resync */
            continue;
        }
        HP_STR(&s->rd, r + k);   /* slots read before the producer may reuse them */
        s->chunk_pos = 0u;
        s->chunk_len = k;
        HP_ST(&s->c_held, k);
        return 1;
    }
}

uint32_t hp_sink_pull(hp_sink_t *s, int16_t *out, uint32_t n, uint8_t out_ch, uint32_t *src_used)
{
    uint32_t made = 0, consumed = 0;
    out_ch = (out_ch == 1u) ? 1u : 2u;
    if (s->buf) {
        consumer_sync(s);
        while (made < n) {
            if (s->chunk_pos >= s->chunk_len && !consumer_refill(s)) {
                break;
            }
            uint32_t used = 0;
            const uint32_t ch = s->c_ch;
            made += hp_rs_process(&s->rs, &s->chunk[s->chunk_pos * ch], s->chunk_len - s->chunk_pos, &used,
                                  out + (size_t)made * ch, n - made);
            s->chunk_pos += used;
            consumed += used;
            HP_ST(&s->c_held, s->chunk_len - s->chunk_pos);
        }
    }
    const uint32_t ch = s->c_ch;
    /* channel mapping in place (out holds n * 2 samples) */
    if (ch == 1u && out_ch == 2u) {
        for (uint32_t i = made; i-- > 0u;) {
            out[2u * i] = out[2u * i + 1u] = out[i];
        }
    } else if (ch == 2u && out_ch == 1u) {
        for (uint32_t i = 0; i < made; i++) {
            out[i] = (int16_t)(((int32_t)out[2u * i] + out[2u * i + 1u]) >> 1);
        }
    }
    const uint32_t real_samples = made * out_ch;
    const uint16_t gain = HP_LD(&s->gain_q15);
    if (HP_LD(&s->mute)) {
        memset(out, 0, (size_t)real_samples * sizeof(int16_t));
    } else if (gain < 32768u) {
        const int32_t g = gain;
        for (uint32_t i = 0; i < real_samples; i++) {
            out[i] = (int16_t)(((int32_t)out[i] * g + (1 << 14)) >> 15);
        }
    }
    memset(out + real_samples, 0, (size_t)(n - made) * out_ch * sizeof(int16_t));
    if (src_used) {
        *src_used = consumed;
    }
    return made;
}
