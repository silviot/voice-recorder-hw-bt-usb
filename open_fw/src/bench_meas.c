/* Bench measurement counters (include/openfw_bench_meas.h). Bench images only (make BT=1 BENCH=1); host-tested in
 * tests/test_bench_meas.c. Pure logic: the callers pass the clocks and the audio counters.
 * SPDX-License-Identifier: MIT */
#include "openfw_bench_meas.h"

bench_meas_t g_bench_meas;

static void zero(void *p, uint32_t n)
{
    uint8_t *d = (uint8_t *)p;
    while (n-- > 0u) {
        *d++ = 0u;
    }
}

void bench_meas_reset(bench_meas_t *m)
{
    zero(m, (uint32_t)sizeof(*m));
}

/* ms + us remainder accumulator (32-bit only, no 64-bit division) */
static void ms_add(uint32_t *ms, uint32_t *us_rem, uint32_t dt_us)
{
    *ms += dt_us / 1000u;
    *us_rem += dt_us % 1000u;
    if (*us_rem >= 1000u) {
        *ms += *us_rem / 1000u;
        *us_rem %= 1000u;
    }
}

static void elapsed_add(bench_meas_rec_t *r, uint32_t now_us)
{
    if (r->ended) {
        return;
    }
    if (r->polled) {
        ms_add(&r->el_ms, &r->el_us_rem, now_us - r->last_us);
    }
    r->last_us = now_us;
    r->polled = 1u;
}

void bench_meas_rec_start(bench_meas_t *m, const char *file, uint8_t ch, uint8_t gain, uint8_t dsp, uint8_t vor,
                          uint32_t now_us)
{
    uint32_t seq = m->cur.seq + 1u;
    if (m->cur.active) {
        seq = m->cur.seq;    /* start without a stop (should not happen): reuse the number */
    }
    zero(&m->cur, (uint32_t)sizeof(m->cur));
    m->cur.seq = seq;
    for (uint32_t i = 0u; file != 0 && i + 1u < sizeof(m->cur.file) && file[i] != '\0'; i++) {
        m->cur.file[i] = file[i];
    }
    m->cur.active = 1u;
    m->cur.ch = ch;
    m->cur.gain = gain;
    m->cur.dsp = dsp;
    m->cur.vor = vor;
    m->cur.idle_min_pm = BENCH_MEAS_NONE16;
    m->cur.last_enter_us = now_us;
    elapsed_add(&m->cur, now_us);
}

void bench_meas_poll_enter(bench_meas_t *m, uint32_t now_us)
{
    bench_meas_rec_t *r = &m->cur;
    if (!r->active) {
        return;
    }
    uint32_t gap = now_us - r->last_enter_us;
    if (gap > r->gap_max_us) {
        r->gap_max_us = gap;
    }
    r->last_enter_us = now_us;
}

void bench_meas_poll_exit(bench_meas_t *m, uint32_t enter_us, uint32_t exit_us, uint32_t dma_blk)
{
    bench_meas_rec_t *r = &m->cur;
    if (!r->active) {
        return;
    }
    uint32_t d = exit_us - enter_us;
    if (d > r->poll_max_us) {
        r->poll_max_us = d;
    }
    elapsed_add(r, exit_us);   /* the clock side at the moment dma_blk is read (a long poll counts on both sides) */
    r->dma_blk = dma_blk;
}

void bench_meas_dma_stamp(bench_meas_t *m, uint32_t blk, uint32_t blk_us)
{
    bench_meas_rec_t *r = &m->cur;
    if (!r->active || blk == 0u) {
        return;
    }
    if (!r->b_valid) {
        r->b_valid = 1u;
        r->b_first_blk = blk;
        r->b_last_blk = blk;
        r->b_last_us = blk_us;
        return;
    }
    if (blk == r->b_last_blk) {
        return;
    }
    ms_add(&r->b_el_ms, &r->b_el_us_rem, blk_us - r->b_last_us);
    r->b_last_blk = blk;
    r->b_last_us = blk_us;
}

void bench_meas_sd_err(bench_meas_t *m, const char *tag)
{
    static const char mdto[] = "SDW_MDTO";
    uint32_t i = 0u;
    while (tag != 0 && mdto[i] != '\0' && tag[i] == mdto[i]) {
        i++;
    }
    uint32_t is_mdto = (tag != 0 && mdto[i] == '\0' && tag[i] == '\0') ? 1u : 0u;
    m->sd_err_total++;
    m->sd_mdto_total += is_mdto;
    if (m->cur.active) {
        m->cur.sd_err++;
        m->cur.sd_mdto += is_mdto;
    }
}

void bench_meas_overrun(bench_meas_t *m, uint32_t dropped, uint32_t comp)
{
    if (!m->cur.active) {
        return;
    }
    m->cur.ov += dropped;
    m->cur.comp += comp;
}

void bench_meas_write(bench_meas_t *m, uint32_t frames, uint32_t wr_us, uint32_t ckpt_us)
{
    bench_meas_rec_t *r = &m->cur;
    if (!r->active) {
        return;
    }
    r->wav_frames += frames;
    r->wr_n++;
    if (wr_us > r->wr_max_us) {
        r->wr_max_us = wr_us;
    }
    if (ckpt_us > r->ckpt_max_us) {
        r->ckpt_max_us = ckpt_us;
    }
}

void bench_meas_capture_end(bench_meas_t *m, uint32_t now_us)
{
    bench_meas_rec_t *r = &m->cur;
    if (!r->active) {
        return;
    }
    elapsed_add(r, now_us);
    r->ended = 1u;
}

void bench_meas_rec_stop(bench_meas_t *m, uint32_t dma_blk, uint32_t ov_total)
{
    bench_meas_rec_t *r = &m->cur;
    if (!r->active) {
        return;
    }
    r->ended = 1u;
    r->dma_blk = dma_blk;
    if (ov_total > r->ov) {
        r->ov = ov_total;   /* the ISR's count is authoritative (overruns after the last poll) */
    }
    r->active = 0u;
    m->hist[m->hist_n % BENCH_MEAS_HIST] = *r;
    m->hist_n++;
}

void bench_meas_gets(bench_meas_t *m, uint32_t bytes)
{
    m->gets_b_total += bytes;
    m->gets_frames_total++;
    if (m->cur.active) {
        m->cur.gets_b += bytes;
    }
}

static uint16_t pm16(uint32_t v)
{
    return (uint16_t)(v > 1000u ? 1000u : v);
}

void bench_meas_cpu(bench_meas_t *m, uint32_t idle_pm, uint32_t app_pm, uint32_t btc_pm, uint32_t win_ms)
{
    m->idle_pm = pm16(idle_pm);
    m->app_pm = pm16(app_pm);
    m->btc_pm = pm16(btc_pm);
    m->cpu_win_ms = (uint16_t)(win_ms > 0xFFFFu ? 0xFFFFu : (win_ms == 0u ? 1u : win_ms));
    if (m->cur.active) {
        if (m->idle_pm < m->cur.idle_min_pm) {
            m->cur.idle_min_pm = m->idle_pm;
        }
        if (m->cur.cpu_n < 0xFFFFu) {
            m->cur.cpu_n++;
        }
    }
}

void bench_meas_cpu_take(bench_meas_t *m, uint32_t take_us)
{
    if (take_us > m->take_max_us) {
        m->take_max_us = take_us;
    }
}

const bench_meas_rec_t *bench_meas_hist(const bench_meas_t *m, uint32_t i)
{
    if (i >= m->hist_n || i >= BENCH_MEAS_HIST) {
        return 0;
    }
    return &m->hist[(m->hist_n - 1u - i) % BENCH_MEAS_HIST];
}

int32_t bench_meas_acct(const bench_meas_rec_t *r)
{
    uint32_t blocks = r->dma_blk - r->ov + r->comp;   /* modular: exact while the true value fits */
    return (int32_t)(blocks * BENCH_MEAS_BLOCK_FRAMES - r->wav_frames);
}

uint32_t bench_meas_clk_frames(const bench_meas_rec_t *r)
{
    return r->el_ms * (BENCH_MEAS_RATE_HZ / 1000u) + r->el_us_rem * (BENCH_MEAS_RATE_HZ / 1000u) / 1000u;
}

int32_t bench_meas_drift(const bench_meas_rec_t *r)
{
    return (int32_t)(r->dma_blk * BENCH_MEAS_BLOCK_FRAMES - bench_meas_clk_frames(r));
}

uint32_t bench_meas_bclk_frames(const bench_meas_rec_t *r)
{
    return r->b_el_ms * (BENCH_MEAS_RATE_HZ / 1000u) + r->b_el_us_rem * (BENCH_MEAS_RATE_HZ / 1000u) / 1000u;
}

int32_t bench_meas_bdrift(const bench_meas_rec_t *r)
{
    if (!r->b_valid) {
        return 0;
    }
    return (int32_t)((r->b_last_blk - r->b_first_blk) * BENCH_MEAS_BLOCK_FRAMES - bench_meas_bclk_frames(r));
}

/* ---- formatting ---- */
typedef struct {
    char    *buf;
    uint32_t cap, len;
    char     sep;
    uint8_t  first;
} fmt_t;

static void fc(fmt_t *f, char c)
{
    if (f->len + 1u < f->cap) {
        f->buf[f->len++] = c;
        f->buf[f->len] = '\0';
    }
}

static void fs(fmt_t *f, const char *s)
{
    while (*s != '\0') {
        fc(f, *s++);
    }
}

static void fu(fmt_t *f, uint32_t v)
{
    char b[10];
    int i = 0;
    do {
        b[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && i < 10);
    while (i > 0) {
        fc(f, b[--i]);
    }
}

static void fkey(fmt_t *f, const char *k)
{
    if (!f->first) {
        fc(f, f->sep);
    }
    f->first = 0u;
    fs(f, k);
    fc(f, '=');
}

static void fku(fmt_t *f, const char *k, uint32_t v)
{
    fkey(f, k);
    fu(f, v);
}

static void fki(fmt_t *f, const char *k, int32_t v)
{
    fkey(f, k);
    if (v < 0) {
        fc(f, '-');
        fu(f, (uint32_t)0 - (uint32_t)v);
    } else {
        fu(f, (uint32_t)v);
    }
}

static void fks(fmt_t *f, const char *k, const char *v)
{
    fkey(f, k);
    if (*v == '\0') {
        fc(f, '-');
    }
    for (; *v != '\0'; v++) {
        char c = *v;
        fc(f, (c == ' ' || c == ',' || c == '=' || (uint8_t)c < 0x20u || (uint8_t)c >= 0x7Fu) ? '_' : c);
    }
}

static void fmt_init(fmt_t *f, char sep, char *buf, uint32_t cap)
{
    f->buf = buf;
    f->cap = cap;
    f->len = 0u;
    f->sep = sep;
    f->first = 1u;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

uint32_t bench_meas_format_rec(const bench_meas_rec_t *r, char sep, char *buf, uint32_t cap)
{
    fmt_t f;
    fmt_init(&f, sep, buf, cap);
    if (cap == 0u) {
        return 0u;
    }
    uint32_t dma = r->dma_blk * BENCH_MEAS_BLOCK_FRAMES;
    fku(&f, "seq", r->seq);
    fks(&f, "file", r->file);
    fku(&f, "rec", r->active);
    fku(&f, "ch", r->ch);
    fku(&f, "gain", r->gain);
    fku(&f, "dsp", r->dsp);
    fku(&f, "vor", r->vor);
    fku(&f, "t_ms", r->el_ms);
    fku(&f, "dma_smp", dma);
    fku(&f, "clk_smp", bench_meas_clk_frames(r));
    fki(&f, "drift", bench_meas_drift(r));
    fku(&f, "wav_smp", r->wav_frames);
    fku(&f, "ov", r->ov);
    fku(&f, "comp", r->comp);
    fku(&f, "lost", r->ov - r->comp);
    fki(&f, "acct", bench_meas_acct(r));
    fku(&f, "gap_max_us", r->gap_max_us);
    fku(&f, "poll_max_us", r->poll_max_us);
    fku(&f, "wr_max_us", r->wr_max_us);
    fku(&f, "wr_n", r->wr_n);
    fku(&f, "ckpt_max_us", r->ckpt_max_us);
    fku(&f, "gets_b", r->gets_b);
    if (r->idle_min_pm == BENCH_MEAS_NONE16) {
        fks(&f, "idle_min_pm", "");
    } else {
        fku(&f, "idle_min_pm", r->idle_min_pm);
    }
    fku(&f, "cpu_n", r->cpu_n);
    if (r->b_valid) {
        fku(&f, "bclk_smp", bench_meas_bclk_frames(r));
        fki(&f, "bdrift", bench_meas_bdrift(r));
    } else {
        fks(&f, "bclk_smp", "");
        fks(&f, "bdrift", "");
    }
    fku(&f, "sd_err", r->sd_err);
    fku(&f, "sd_mdto", r->sd_mdto);
    return f.len;
}

uint32_t bench_meas_format_cpu(const bench_meas_t *m, char sep, char *buf, uint32_t cap)
{
    fmt_t f;
    fmt_init(&f, sep, buf, cap);
    if (cap == 0u) {
        return 0u;
    }
    fku(&f, "gets_tot", m->gets_b_total);
    fku(&f, "gets_fr", m->gets_frames_total);
    if (m->cpu_win_ms == 0u) {
        fks(&f, "idle_pm", "");
        fks(&f, "app_pm", "");
        fks(&f, "btc_pm", "");
    } else {
        fku(&f, "idle_pm", m->idle_pm);
        fku(&f, "app_pm", m->app_pm);
        fku(&f, "btc_pm", m->btc_pm);
    }
    fku(&f, "cpu_ms", m->cpu_win_ms);
    fku(&f, "take_us", m->take_max_us);
    return f.len;
}
