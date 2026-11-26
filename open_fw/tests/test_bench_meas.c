/* Host tests for the F0 bench measurement counters (src/bench_meas.c, include/openfw_bench_meas.h).
 * make test-bt-port. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "openfw_bench_meas.h"
#include "openfw_xrec.h"

static int g_checks, g_fails;
#define CHECK(c, ...)                                                                                                 \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(c)) {                                                                                                   \
            g_fails++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static bench_meas_t m;

static const char *field(const char *line, const char *key, char sep)
{
    static char v[64];
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = line;
    size_t kl = strlen(pat);
    while (p != NULL && *p != '\0') {
        if (strncmp(p, pat, kl) == 0) {
            size_t i = 0;
            p += kl;
            while (p[i] != '\0' && p[i] != sep && i + 1 < sizeof(v)) {
                v[i] = p[i];
                i++;
            }
            v[i] = '\0';
            return v;
        }
        p = strchr(p, sep);
        if (p != NULL) {
            p++;
        }
    }
    return "(missing)";
}

/* A clean 10 s mono recording: one poll per 128 ms block, us clock starting near the 32-bit wrap. */
static void t_clean(void)
{
    printf("[1] clean recording: exact accounting, drift within one block, wrap-safe clock\n");
    bench_meas_reset(&m);
    uint32_t t = 0xFFFFFFFFu - 3000000u;   /* wraps 3 s into the recording */
    bench_meas_rec_start(&m, "REC_0007.wav", 1u, 20u, 2u, 0u, t);
    CHECK(m.cur.active && m.cur.seq == 1u && strcmp(m.cur.file, "REC_0007.wav") == 0, "started");
    uint32_t blk = 0u;
    for (int i = 0; i < 78; i++) {       /* 78 blocks = 9.984 s */
        t += 128000u;
        blk++;
        bench_meas_poll_enter(&m, t + 500u);
        bench_meas_write(&m, 2048u, 3000u + (uint32_t)i, i == 40 ? 9000u : 0u);
        bench_meas_poll_exit(&m, t + 500u, t + 4000u, blk);
    }
    t += 100000u;                          /* stop 100 ms into the next block */
    bench_meas_capture_end(&m, t);
    bench_meas_poll_enter(&m, t + 2000u);  /* recorder_stop's drain: must not move the clock */
    bench_meas_poll_exit(&m, t + 2000u, t + 2500u, blk);
    bench_meas_rec_stop(&m, blk, 0u);
    const bench_meas_rec_t *r = bench_meas_hist(&m, 0u);
    CHECK(r != NULL && !m.cur.active && m.hist_n == 1u, "archived");
    CHECK(bench_meas_acct(r) == 0, "acct %d", bench_meas_acct(r));
    CHECK(r->el_ms == 9984u + 100u, "elapsed %u ms", r->el_ms);
    CHECK(bench_meas_clk_frames(r) == (9984u + 100u) * 16u, "clock frames %u", bench_meas_clk_frames(r));
    CHECK(bench_meas_drift(r) == -1600, "drift %d (the 100 ms block in flight)", bench_meas_drift(r));
    CHECK(r->gap_max_us == 128500u, "gap %u (start -> first poll)", r->gap_max_us);
    CHECK(r->poll_max_us == 3500u && r->wr_max_us == 3077u && r->ckpt_max_us == 9000u && r->wr_n == 78u,
          "poll %u wr %u ckpt %u n %u", r->poll_max_us, r->wr_max_us, r->ckpt_max_us, r->wr_n);
    CHECK(r->idle_min_pm == BENCH_MEAS_NONE16 && r->cpu_n == 0u, "no CPU window");
    char line[BENCH_MEAS_LINE_MAX];
    bench_meas_format_rec(r, ' ', line, sizeof(line));
    CHECK(strcmp(field(line, "seq", ' '), "1") == 0 && strcmp(field(line, "file", ' '), "REC_0007.wav") == 0 &&
          strcmp(field(line, "acct", ' '), "0") == 0 && strcmp(field(line, "drift", ' '), "-1600") == 0 &&
          strcmp(field(line, "idle_min_pm", ' '), "-") == 0 && strcmp(field(line, "rec", ' '), "0") == 0 &&
          strcmp(field(line, "wav_smp", ' '), "159744") == 0 && strcmp(field(line, "lost", ' '), "0") == 0,
          "line '%s'", line);
}

/* Overruns: 5 dropped blocks, 2 written as zeros: lost 3 -> acct exact, lost visible. */
static void t_overruns(void)
{
    printf("[2] overruns, compensation cap, sub-ms clock remainder\n");
    bench_meas_reset(&m);
    bench_meas_rec_start(&m, "REC_0001.wav", 2u, 31u, 3u, 0u, 1000u);
    bench_meas_poll_enter(&m, 1333u);                      /* 333 us remainder accumulates */
    bench_meas_overrun(&m, 5u, 2u);
    bench_meas_write(&m, 2048u, 1u, 0u);                    /* zero block 1 */
    bench_meas_write(&m, 2048u, 1u, 0u);                    /* zero block 2 */
    for (int i = 0; i < 3; i++) {
        bench_meas_write(&m, 2048u, 1u, 0u);                /* the 3 surviving ring blocks */
    }
    bench_meas_poll_exit(&m, 1333u, 1400u, 8u);             /* 8 DMA blocks: 5 dropped + 3 kept */
    bench_meas_poll_enter(&m, 1999u);
    bench_meas_capture_end(&m, 2000u);
    bench_meas_rec_stop(&m, 8u, 5u);
    const bench_meas_rec_t *r = bench_meas_hist(&m, 0u);
    CHECK(r->ov == 5u && r->comp == 2u, "ov %u comp %u", r->ov, r->comp);
    CHECK(bench_meas_acct(r) == 0, "acct %d", bench_meas_acct(r));
    CHECK(r->el_ms == 1u && bench_meas_clk_frames(r) == 16u, "1 ms -> 16 frames (%u, %u)", r->el_ms,
          bench_meas_clk_frames(r));
    char line[BENCH_MEAS_LINE_MAX];
    bench_meas_format_rec(r, ',', line, sizeof(line));
    CHECK(strcmp(field(line, "lost", ','), "3") == 0 && strcmp(field(line, "ch", ','), "2") == 0 &&
          strcmp(field(line, "gain", ','), "31") == 0 && strcmp(field(line, "dsp", ','), "3") == 0,
          "line '%s'", line);
    /* an ISR overrun after the last poll: the stop count wins, acct shows the missing block */
    bench_meas_rec_start(&m, "REC_0002.wav", 1u, 20u, 3u, 0u, 0u);
    bench_meas_poll_enter(&m, 10u);
    bench_meas_write(&m, 2048u, 1u, 0u);
    bench_meas_poll_exit(&m, 10u, 20u, 1u);
    bench_meas_capture_end(&m, 30u);
    bench_meas_rec_stop(&m, 3u, 1u);
    r = bench_meas_hist(&m, 0u);
    CHECK(r->seq == 2u && r->ov == 1u && bench_meas_acct(r) == 2048, "late overrun: seq %u ov %u acct %d", r->seq,
          r->ov, bench_meas_acct(r));
    CHECK(bench_meas_hist(&m, 1u)->seq == 1u && bench_meas_hist(&m, 2u) == NULL, "history order");
}

static void t_live_cpu_gets(void)
{
    printf("[3] live values, CPU windows, GETS bytes, history ring\n");
    bench_meas_reset(&m);
    char line[BENCH_MEAS_LINE_MAX];
    bench_meas_format_cpu(&m, ',', line, sizeof(line));
    CHECK(strcmp(line, "gets_tot=0,gets_fr=0,idle_pm=-,app_pm=-,btc_pm=-,cpu_ms=0,take_us=0") == 0, "empty cpu '%s'", line);
    bench_meas_gets(&m, 640u);                              /* not recording: total only */
    bench_meas_cpu(&m, 800u, 100u, 50u, 1000u);
    bench_meas_rec_start(&m, "REC_0003.wav", 1u, 20u, 0u, 1u, 0u);
    bench_meas_gets(&m, 640u);
    bench_meas_gets(&m, 100u);
    bench_meas_cpu(&m, 300u, 200u, 400u, 1001u);
    bench_meas_cpu(&m, 450u, 200u, 300u, 999u);
    bench_meas_cpu(&m, 2000u, 0u, 0u, 0u);                  /* clamped */
    CHECK(m.cur.gets_b == 740u && m.gets_b_total == 1380u && m.gets_frames_total == 3u, "gets %u %u %u",
          m.cur.gets_b, m.gets_b_total, m.gets_frames_total);
    CHECK(m.cur.idle_min_pm == 300u && m.cur.cpu_n == 3u && m.idle_pm == 1000u && m.cpu_win_ms == 1u,
          "idle min %u n %u last %u ms %u", m.cur.idle_min_pm, m.cur.cpu_n, m.idle_pm, m.cpu_win_ms);
    bench_meas_format_rec(&m.cur, ',', line, sizeof(line));
    CHECK(strcmp(field(line, "rec", ','), "1") == 0 && strcmp(field(line, "vor", ','), "1") == 0 &&
          strcmp(field(line, "idle_min_pm", ','), "300") == 0 && strcmp(field(line, "gets_b", ','), "740") == 0,
          "live '%s'", line);
    for (int i = 0; i < 12; i++) {                          /* the ring keeps the latest BENCH_MEAS_HIST */
        bench_meas_capture_end(&m, 5u);
        bench_meas_rec_stop(&m, 0u, 0u);
        bench_meas_rec_start(&m, "REC_0004.wav", 1u, 20u, 0u, 0u, 0u);
    }
    CHECK(m.hist_n == 12u && bench_meas_hist(&m, 0u)->seq == 12u &&
          bench_meas_hist(&m, BENCH_MEAS_HIST - 1u)->seq == 12u - BENCH_MEAS_HIST + 1u &&
          bench_meas_hist(&m, BENCH_MEAS_HIST) == NULL, "ring");
    /* hooks are no-ops while not recording */
    bench_meas_capture_end(&m, 5u);
    bench_meas_rec_stop(&m, 0u, 0u);
    uint32_t n = m.hist_n;
    bench_meas_poll_enter(&m, 1u);
    bench_meas_overrun(&m, 1u, 1u);
    bench_meas_write(&m, 1u, 1u, 1u);
    bench_meas_rec_stop(&m, 1u, 1u);
    CHECK(m.hist_n == n && m.cur.ov == 0u && m.cur.wr_n == 0u, "idle hooks ignored");
}

/* Worst-case line length: every field at its widest; the XREC reply must fit XREC_REPLY_MAX. */
static void t_widths(void)
{
    printf("[4] worst-case widths and truncation\n");
    bench_meas_reset(&m);
    bench_meas_rec_t *r = &m.cur;
    memset(r, 0xFF, sizeof(*r));
    memcpy(r->file, "ABCDEFGHIJKLMNO", 16u);   /* max 15-char file field */
    r->idle_min_pm = 65534u;                   /* widest non-BENCH_MEAS_NONE16 uint16_t */
    r->dma_blk = 0x1FFFFFu;                     /* dma_smp = 4294965248 (10 digits) */
    r->ov = 4000000000u;                        /* 10-digit ov, 10-digit comp, and 10-digit lost simultaneously */
    r->comp = 1000000000u;
    r->wav_frames = 3219126272u;                /* 10-digit wav_smp; acct = -2147483648 (11 chars) */
    r->el_ms = 134217600u;                      /* clk_smp = 2147481600 (10 digits); drift = -2147483648 (11 chars) */
    r->el_us_rem = 0u;
    r->b_el_ms = 134217600u;                    /* bclk_smp = 2147481600 (10 digits); bdrift = -2147483648 (11 chars) */
    r->b_el_us_rem = 0u;
    r->b_first_blk = 0u;
    r->b_last_blk = 0x1FFFFFu;
    m.take_max_us = 0xFFFFFFFFu;
    m.sd_err_total = 0xFFFFFFFFu;
    m.gets_b_total = m.gets_frames_total = 0xFFFFFFFFu;
    m.idle_pm = m.app_pm = m.btc_pm = 0xFFFFu;
    m.cpu_win_ms = 0xFFFFu;
    char line[1024];
    uint32_t a = bench_meas_format_rec(r, ',', line, sizeof(line));
    uint32_t b = bench_meas_format_cpu(&m, ',', line, sizeof(line));
    uint32_t reply = (uint32_t)strlen("+XREC:DBG,LIVE,") + a + 1u + b + 2u;
    printf("    rec %u B, cpu %u B, LIVE reply %u B\n", a, b, reply);
    CHECK(a + 1u + b < BENCH_MEAS_LINE_MAX, "line fits BENCH_MEAS_LINE_MAX (%u vs %u)", a + 1u + b,
          BENCH_MEAS_LINE_MAX);
    CHECK(reply <= XREC_REPLY_MAX, "reply fits XREC_REPLY_MAX (%u vs %u)", reply, XREC_REPLY_MAX);
    char small[20];
    uint32_t s = bench_meas_format_rec(r, ',', small, sizeof(small));
    CHECK(s == 19u && small[19] == '\0' && strncmp(small, "seq=4294967295,file", 19) == 0, "truncated '%s'", small);
    CHECK(bench_meas_format_rec(r, ',', small, 0u) == 0u, "cap 0");
    /* a file name with separators cannot break the k=v format */
    bench_meas_rec_start(&m, "A B,C=D", 1u, 0u, 0u, 0u, 0u);
    bench_meas_format_rec(&m.cur, ',', line, sizeof(line));
    CHECK(strcmp(field(line, "file", ','), "A_B_C_D") == 0, "sanitised '%s'", line);
}

/* A long poll (SD stall): blocks that complete during it count against the clock at the same moment. */
static void t_live_consistent(void)
{
    printf("[5] live drift from a consistent snapshot (clock and DMA blocks both at poll exit)\n");
    bench_meas_reset(&m);
    bench_meas_rec_start(&m, "REC_0010.wav", 1u, 20u, 2u, 0u, 0u);
    bench_meas_poll_enter(&m, 128000u);
    bench_meas_poll_exit(&m, 128000u, 428000u, 3u);       /* 300 ms poll: 2 more blocks completed in it */
    int32_t d = bench_meas_drift(&m.cur);
    CHECK(d <= 0 && d >= -2048, "live drift %d within 0..-2048", d);
    CHECK(m.cur.el_ms == 428u && m.cur.poll_max_us == 300000u && m.cur.gap_max_us == 128000u,
          "el %u poll %u gap %u", m.cur.el_ms, m.cur.poll_max_us, m.cur.gap_max_us);
    bench_meas_poll_enter(&m, 500000u);                    /* gap entry to entry, not exit to entry */
    bench_meas_poll_exit(&m, 500000u, 501000u, 3u);
    CHECK(m.cur.gap_max_us == 372000u, "gap %u", m.cur.gap_max_us);
}

/* Block clock: the ISR stamps resolve a sub-block loss; start-up offsets cancel; wrap-safe. */
static void t_block_clock(void)
{
    printf("[6] block clock: sub-block sample loss, wrap, no stamps\n");
    bench_meas_reset(&m);
    char line[BENCH_MEAS_LINE_MAX];
    bench_meas_rec_start(&m, "REC_0011.wav", 1u, 20u, 2u, 0u, 7u);
    bench_meas_dma_stamp(&m, 0u, 5u);                      /* no block yet: ignored */
    bench_meas_format_rec(&m.cur, ' ', line, sizeof(line));
    CHECK(strcmp(field(line, "bdrift", ' '), "-") == 0 && strcmp(field(line, "bclk_smp", ' '), "-") == 0,
          "no stamps '%s'", line);
    uint32_t us = 0xFFFFFFFFu - 200000u;                   /* wraps between the stamps */
    bench_meas_dma_stamp(&m, 2u, us);                      /* the first block this recording saw: reference */
    bench_meas_dma_stamp(&m, 2u, us);                      /* same block again: no change */
    for (uint32_t b = 3u; b <= 600u; b++) {
        bench_meas_dma_stamp(&m, b, us + (b - 2u) * 128000u);
    }
    CHECK(bench_meas_bdrift(&m.cur) == 0 && bench_meas_bclk_frames(&m.cur) == 598u * 2048u, "clean bdrift %d bclk %u",
          bench_meas_bdrift(&m.cur), bench_meas_bclk_frames(&m.cur));
    us += 598u * 128000u;
    bench_meas_dma_stamp(&m, 601u, us + 128000u + 1000u);  /* 1 ms (16 samples) the DMA did not capture */
    bench_meas_dma_stamp(&m, 604u, us + 4u * 128000u + 1000u);   /* polls may skip blocks */
    CHECK(bench_meas_bdrift(&m.cur) == -16, "16-sample loss: bdrift %d", bench_meas_bdrift(&m.cur));
    bench_meas_dma_stamp(&m, 605u, us + 5u * 128000u + 1000u + 63u);   /* 63 us = 1 sample (16 kHz) */
    CHECK(bench_meas_bdrift(&m.cur) == -17, "1 more sample: bdrift %d", bench_meas_bdrift(&m.cur));
    bench_meas_capture_end(&m, us);
    bench_meas_rec_stop(&m, 605u, 0u);
    bench_meas_format_rec(bench_meas_hist(&m, 0u), ' ', line, sizeof(line));
    CHECK(strcmp(field(line, "bdrift", ' '), "-17") == 0, "line '%s'", line);
    bench_meas_dma_stamp(&m, 700u, 1u);                    /* not recording: ignored */
    CHECK(bench_meas_hist(&m, 0u)->b_last_blk == 605u && !m.cur.active, "idle stamp ignored");
}

static void t_sd_take(void)
{
    printf("[7] SD error counters (whole recording, since boot) and the CPU take maximum\n");
    bench_meas_reset(&m);
    bench_meas_sd_err(&m, "SDW_MDTO");                     /* before any recording: totals only */
    bench_meas_rec_start(&m, "REC_0012.wav", 1u, 20u, 2u, 0u, 0u);
    bench_meas_sd_err(&m, "SDW_MDTO");
    bench_meas_sd_err(&m, "SDW_MFAIL");
    bench_meas_sd_err(&m, "SDW_MDTOX");
    bench_meas_sd_err(&m, "SDW_MDT");
    bench_meas_sd_err(&m, NULL);
    CHECK(m.cur.sd_err == 5u && m.cur.sd_mdto == 1u && m.sd_err_total == 6u && m.sd_mdto_total == 2u,
          "rec %u/%u total %u/%u", m.cur.sd_err, m.cur.sd_mdto, m.sd_err_total, m.sd_mdto_total);
    char line[BENCH_MEAS_LINE_MAX];
    bench_meas_format_rec(&m.cur, ',', line, sizeof(line));
    CHECK(strcmp(field(line, "sd_err", ','), "5") == 0 && strcmp(field(line, "sd_mdto", ','), "1") == 0, "line '%s'",
          line);
    bench_meas_capture_end(&m, 1u);
    bench_meas_rec_stop(&m, 0u, 0u);
    bench_meas_rec_start(&m, "REC_0013.wav", 1u, 20u, 2u, 0u, 0u);
    CHECK(m.cur.sd_err == 0u && m.cur.sd_mdto == 0u && m.sd_err_total == 6u, "reset per recording");
    /* SD write-path counters: only while recording, per recording, on their own "BENCH rec" suffix */
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_BCI1);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_BCI1);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_BCIM);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_RETRY);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_RETRY);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_RETRY);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_REINIT);
    bench_meas_sd_wr(&m, 99u);
    bench_meas_format_sdw(&m.cur, ' ', line, sizeof(line));
    CHECK(strcmp(line, "sd_bci1=2 sd_bcim=1 sd_retry=3 sd_reinit=1") == 0, "sdw '%s'", line);
    {
        bench_meas_rec_t wide;
        memset(&wide, 0xFF, sizeof(wide));
        char sdw[BENCH_MEAS_SDW_LINE_MAX];
        uint32_t n = bench_meas_format_sdw(&wide, ' ', sdw, sizeof(sdw));
        CHECK(n + 1u < BENCH_MEAS_SDW_LINE_MAX && n == strlen(sdw), "sdw width %u", n);
    }
    bench_meas_capture_end(&m, 2u);
    bench_meas_rec_stop(&m, 0u, 0u);
    bench_meas_sd_wr(&m, BENCH_MEAS_SDW_RETRY);                /* not recording: ignored */
    CHECK(bench_meas_hist(&m, 0u)->sd_retry == 3u, "idle sd_wr ignored");
    bench_meas_rec_start(&m, "REC_0014.wav", 1u, 20u, 2u, 0u, 0u);
    CHECK(m.cur.sd_bci1 == 0u && m.cur.sd_retry == 0u && m.cur.sd_reinit == 0u, "sd_wr reset per recording");
    bench_meas_cpu_take(&m, 120u);
    bench_meas_cpu_take(&m, 90u);
    bench_meas_format_cpu(&m, ',', line, sizeof(line));
    CHECK(strcmp(field(line, "take_us", ','), "120") == 0,
          "cpu '%s'", line);
}

int main(void)
{
    t_clean();
    t_overruns();
    t_live_cpu_gets();
    t_widths();
    t_live_consistent();
    t_block_clock();
    t_sd_take();
    printf("test_bench_meas: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
