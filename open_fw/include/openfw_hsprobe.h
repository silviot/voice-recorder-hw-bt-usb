/*
 * Cassini OpenFW — Headset Button Probe (diagnostic Settings entry)
 *
 * A 30-second, user-run experiment that answers one hardware question:
 * is the 3.5 mm jack's 4th (mic / headset-button) contact wired to anything
 * the SL6801 can read?  It samples every 10 ms:
 *   - PB11 (headphone detect) digital level,
 *   - all 10 SARADC/KADC channel data registers (all channel enables set),
 *   - the raw input registers of GPIO ports A..D (auto-masking noisy pins),
 *   - codec ADC statistics with MICBIAS on while cycling the ADC ch2 input mux,
 * in three phases:
 *   A  0..10 s  idle codec (MICBIAS off), PB11 = GPIO input
 *   B 10..20 s  codec capture running (MICBIAS on), PB11 = GPIO input
 *   C 20..30 s  codec capture running (MICBIAS on), PB11 = analog mux 15
 * and writes a bounded plain-text log to /HSPROBE.LOG on the SD card.
 *
 * The log formatting / change detection / summary logic (hsprobe_log.c) is
 * pure C with no MMIO so it can be unit-tested on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_HSPROBE_H
#define OPENFW_HSPROBE_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Tunables
 * ------------------------------------------------------------------------- */
#define HSP_NCH             10u     /* SARADC channels 0..9 */
#define HSP_NPORT           4u      /* GPIO ports A..D */
#define HSP_NPHASE          3u
#define HSP_NMUX            16u     /* codec ADC ch2 mux values 0..15 */
#define HSP_LOG_CAP         16384u  /* RAM text buffer (bytes) */
#define HSP_LOG_RESERVE     3072u   /* bytes kept free for summary + self-check */
#define HSP_CHG_TH          40      /* log SARADC change when |v - last_logged| > 40 LSB */
#define HSP_EDGE_TH         300     /* large drop / rise vs. slow baseline (LSB) */
#define HSP_EDGE_LONG_SMP   300u    /* re-baseline after 3 s stuck in a drop/rise */
#define HSP_CH_EVT_BUDGET   48u     /* max K lines per channel */
#define HSP_CH_EDGE_BUDGET  24u     /* max D/U lines per channel */
#define HSP_GPIO_EVT_BUDGET 64u     /* max G lines in total */
#define HSP_GPIO_NOISY      16u     /* toggles before a GPIO bit is auto-masked */
#define HSP_A_EVT_BUDGET    48u     /* max codec block event lines */
#define HSP_A_P2P_EVT       12000   /* codec block peak-to-peak event threshold */
#define HSP_A_STEP_EVT      3000    /* codec block mean step event threshold */

#define HSP_SAMPLE_MS       10u
#define HSP_T_PHASE_B       10000u
#define HSP_T_PHASE_C       20000u
#define HSP_T_END           30000u
#define HSP_MUX_WINDOW_MS   1250u   /* 16 mux values x 1.25 s = phases B + C */
#define HSP_PROMPT_COUNT    5u
#define HSP_PROMPT_MS       1500u

/* ---------------------------------------------------------------------------
 * Bounded text log (one '\n'-terminated line per entry, never split)
 * ------------------------------------------------------------------------- */
typedef struct {
    char     *buf;
    uint32_t  cap;
    uint32_t  len;
    uint32_t  reserve;
    uint32_t  dropped_lines;
} hsp_text_t;

typedef struct {
    char     s[192];
    uint32_t n;
} hsp_line_t;

void hsp_text_init(hsp_text_t *t, char *buf, uint32_t cap, uint32_t reserve);
/* Append one line (+'\n'). use_reserve=0 keeps `reserve` bytes free. 0 = ok, -1 = dropped. */
int  hsp_text_line(hsp_text_t *t, const hsp_line_t *l, int use_reserve);

void hsp_line_reset(hsp_line_t *l);
void hsp_line_str(hsp_line_t *l, const char *s);
void hsp_line_chr(hsp_line_t *l, char c);
void hsp_line_u32(hsp_line_t *l, uint32_t v);
void hsp_line_i32(hsp_line_t *l, int32_t v);
void hsp_line_hex(hsp_line_t *l, uint32_t v);      /* "0x%08X" */
void hsp_line_hex_n(hsp_line_t *l, uint32_t v, unsigned digits); /* "%0*X", no prefix */

/* ---------------------------------------------------------------------------
 * Probe analysis state
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t last_logged;
    uint16_t min;
    uint16_t max;
    uint8_t  have;
    uint8_t  edge;           /* 0 = none, 1 = in drop, 2 = in rise */
    int32_t  ref_q4;         /* slow baseline (EMA, x16) */
    uint32_t edge_len;
    uint16_t events;
    uint16_t suppressed;
    uint16_t edge_lines;
    uint16_t drops[HSP_NPHASE];
    uint16_t rises[HSP_NPHASE];
    uint16_t min_ph[HSP_NPHASE];
    uint16_t max_ph[HSP_NPHASE];
} hsp_chan_t;

typedef struct {
    int16_t  min1, max1, min2, max2;
    int32_t  p2p1_max, p2p2_max;
    int32_t  sum1, sum2;     /* window: sample sums; per-mux: sum of window means */
    uint32_t n;              /* window: frames; per-mux: unused */
    uint16_t events;
    uint16_t windows;
} hsp_mux_stat_t;

typedef struct {
    hsp_text_t     txt;
    hsp_chan_t     ch[HSP_NCH];
    uint8_t        phase;                 /* 0 = A, 1 = B, 2 = C */
    int8_t         pb11_last;             /* -1 = never sampled */
    uint16_t       pb11_changes[HSP_NPHASE];
    uint32_t       pb11_low_samples[HSP_NPHASE];
    uint32_t       phase_samples[HSP_NPHASE];
    uint32_t       gpio_last[HSP_NPORT];
    uint32_t       gpio_mask[HSP_NPORT];
    uint8_t        gpio_have;
    uint8_t        gpio_toggles[HSP_NPORT][32];
    uint32_t       gpio_changed_bits[HSP_NPORT]; /* union of non-noisy bits that changed */
    uint16_t       gpio_events;
    uint16_t       gpio_suppressed;
    uint32_t       samples;
    /* Codec ADC */
    uint8_t        mux_cur;               /* 0xFF = no window open */
    int32_t        a_prev_mean1, a_prev_mean2;
    uint8_t        a_have_prev;
    uint16_t       a_events;
    uint16_t       a_suppressed;
    uint32_t       a_blocks;
    hsp_mux_stat_t win;                   /* current window */
    hsp_mux_stat_t mux[HSP_NMUX];         /* per mux value, accumulated */
} hsp_state_t;

void hsp_init(hsp_state_t *st, char *buf, uint32_t cap, const uint32_t gpio_mask[HSP_NPORT]);
/* Header / free-form line (counts against the normal budget, not the reserve). */
int  hsp_comment(hsp_state_t *st, const char *s);
void hsp_set_phase(hsp_state_t *st, uint32_t t_ms, uint8_t phase, const char *desc);
void hsp_mark(hsp_state_t *st, uint32_t t_ms, const char *tag, uint32_t val);
/* One 10 ms sample. pb11 = raw PB11 input level (0/1) or -1 if not sampled. gpio may be NULL. */
void hsp_sample(hsp_state_t *st, uint32_t t_ms, const uint16_t v[HSP_NCH], int pb11,
                const uint32_t gpio[HSP_NPORT]);
void hsp_snapshot(hsp_state_t *st, uint32_t t_ms, const uint16_t v[HSP_NCH], int pb11);
/* Codec ADC: open a window for mux value `mux`, feed blocks of interleaved PCM, close window. */
void hsp_codec_window_begin(hsp_state_t *st, uint32_t t_ms, uint8_t mux);
void hsp_codec_block(hsp_state_t *st, uint32_t t_ms, const int16_t *pcm, uint32_t frames,
                     uint8_t channels);
void hsp_codec_window_end(hsp_state_t *st, uint32_t t_ms);
/* Summary + "END" line (uses the reserve). */
void hsp_summary(hsp_state_t *st, uint32_t t_end_ms, const char *end_reason);

/* Prompt schedule: returns 1..HSP_PROMPT_COUNT while a "press now" window is open, else 0. */
uint8_t  hsp_prompt_at(uint32_t t_ms);
uint32_t hsp_prompt_start_ms(uint8_t idx); /* idx 1..HSP_PROMPT_COUNT */
uint8_t  hsp_phase_at(uint32_t t_ms);

/* ---------------------------------------------------------------------------
 * Firmware entry (hsprobe.c) — modal, called from the Settings screen
 * ------------------------------------------------------------------------- */
#define HSP_RC_SAVED      0
#define HSP_RC_NO_SD      1
#define HSP_RC_SAVE_FAIL  2
#define HSP_RC_BUSY       3

typedef struct {
    void *vol;          /* exfat_vol_t * (mounted volume) */
    int   sd_mounted;
    /* UI hooks implemented by main.c. `poll` services the power button /
     * backlight and returns non-zero when the screen is visible. `render`
     * pushes the current probe state (secs left, phase, prompt, plug). */
    int  (*poll)(uint32_t now_ms);
    void (*render)(uint8_t secs_left, uint8_t phase, uint8_t prompt, int8_t plug);
} hsprobe_ctx_t;

/* Runs the 30 s probe (BACK aborts early, REC switch aborts), restores every
 * touched register, then writes /HSPROBE.LOG. Returns HSP_RC_*. */
int hsprobe_run(const hsprobe_ctx_t *ctx);

/* Exposed for host tests (BT=1 firmware images keep the log in the experiment output buffer instead, hsprobe.c). */
extern char g_hsp_log_buf[HSP_LOG_CAP];
uint32_t hsprobe_last_log_len(void);

#endif /* OPENFW_HSPROBE_H */
