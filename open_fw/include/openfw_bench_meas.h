/* Bench measurement counters for "sync while recording" (docs/BT_SYNC_SPEED_AND_RECORDING.md F0, stage S2).
 *
 * Bench images only (make BT=1 BENCH=1 -> OPENFW_BT_BENCH_IMAGE=1): src/bench_meas.c is linked and the hooks in
 * src/recorder.c and src/xrec_proto.c are compiled in. Every other image (BT=0, release BT=1, HCI_PROBE) compiles the
 * hooks out; including this header there changes nothing (types and prototypes only).
 *
 * Per recording (reset at recorder_start):
 *   - DMA blocks completed (audio_get_frames, 2048 frames each) vs the us_timer time since capture start: drift.
 *   - Ring overruns (audio_get_overruns: blocks the DMA ISR dropped), the zero blocks recorder_poll wrote for them
 *     (capped at 2 per poll) and the frames written to the WAV: acct = (dma - ov + comp) * 2048 - wav. At REC stop
 *     (the ring drained) acct must be exactly 0 with VOR off; while recording it is the ring backlog (0..3 blocks).
 *   - Largest gap between two recorder_poll() calls (the app-loop iteration gap the ring must cover: 384 ms with the
 *     4-slot ring), the longest recorder_poll() itself, the longest SD write of one block (exfat_write) and the
 *     longest WAV checkpoint (header + directory entry).
 *   - GETS payload bytes sent while recording; the lowest 1 s idle CPU while recording (fed by the bench run).
 * Since boot: GETS payload bytes and frames, the last 1 s CPU window (idle / app / BTC_TASK permille).
 *
 * Export: +XREC:DBG (xrec_proto.c, authenticated, bench images only), a "BENCH rec ..." log line at every REC stop
 * (recorder.c), "BENCH meas ..." lines in phone-mode bench runs and the tail of SD:/BTBENCH.LOG (bt_bench_fw.c).
 * Pure logic, no hardware access: host-tested in tests/test_bench_meas.c (make test-bt-port).
 * SPDX-License-Identifier: MIT */
#ifndef OPENFW_BENCH_MEAS_H
#define OPENFW_BENCH_MEAS_H

#include <stdint.h>

#if defined(OPENFW_BT_BENCH_IMAGE) && OPENFW_BT_BENCH_IMAGE
#define OPENFW_BENCH_MEAS 1
#else
#define OPENFW_BENCH_MEAS 0
#endif

#define BENCH_MEAS_BLOCK_FRAMES 2048u      /* frames per DMA block (128 ms at 16 kHz) */
#define BENCH_MEAS_RATE_HZ      16000u
#define BENCH_MEAS_HIST         8u         /* finished recordings kept */
#define BENCH_MEAS_NONE16       0xFFFFu    /* idle_min_pm: no CPU window during the recording */
#define BENCH_MEAS_LINE_MAX     480u       /* longest formatted line (fits one XREC reply) */

typedef struct {
    uint32_t seq;           /* recording number since boot (1..), 0 = none yet */
    char     file[16];      /* the first WAV file of the recording (REC_NNNN.wav) */
    uint8_t  active;        /* recording now */
    uint8_t  ch, gain, dsp, vor;
    uint8_t  polled;        /* last_us valid */
    uint8_t  ended;         /* capture stopped: the elapsed time is final */
    uint32_t last_us;       /* us_timer (low 32 bits) at the last poll / start */
    uint32_t el_ms;         /* time since capture start: ms + us remainder (32-bit only, no 64-bit division) */
    uint32_t el_us_rem;
    uint32_t dma_blk;       /* DMA blocks completed (at the last poll / at stop) */
    uint32_t ov;            /* ring overruns (blocks dropped by the ISR) */
    uint32_t comp;          /* zero blocks written for them */
    uint32_t wav_frames;    /* frames written to the WAV (zero blocks included) */
    uint32_t gap_max_us;    /* largest gap between two recorder_poll() entries */
    uint32_t poll_max_us;   /* longest recorder_poll() */
    uint32_t wr_max_us;     /* longest exfat_write() of one block */
    uint32_t wr_n;          /* exfat_write() calls */
    uint32_t ckpt_max_us;   /* longest WAV checkpoint */
    uint32_t gets_b;        /* GETS payload bytes sent while recording */
    uint16_t idle_min_pm;   /* lowest 1 s idle while recording (BENCH_MEAS_NONE16: none) */
    uint16_t cpu_n;         /* 1 s CPU windows while recording */
} bench_meas_rec_t;

typedef struct {
    bench_meas_rec_t cur;                     /* the running recording, else the last one */
    bench_meas_rec_t hist[BENCH_MEAS_HIST];   /* finished recordings: hist[(hist_n - 1 - i) % HIST] = i-th latest */
    uint32_t hist_n;                          /* recordings finished since boot */
    uint32_t gets_b_total;                    /* GETS payload bytes since boot */
    uint32_t gets_frames_total;
    uint16_t idle_pm, app_pm, btc_pm;         /* the last CPU window (permille of wall time) */
    uint16_t cpu_win_ms;                      /* its length; 0 = no window yet */
} bench_meas_t;

extern bench_meas_t g_bench_meas;            /* the firmware's instance (bench images) */

void bench_meas_reset(bench_meas_t *m);
/* recorder_start() succeeded: capture started at now_us. */
void bench_meas_rec_start(bench_meas_t *m, const char *file, uint8_t ch, uint8_t gain, uint8_t dsp, uint8_t vor,
                          uint32_t now_us);
/* recorder_poll() entry (gap, elapsed time) and exit (duration, DMA blocks so far). */
void bench_meas_poll_enter(bench_meas_t *m, uint32_t now_us);
void bench_meas_poll_exit(bench_meas_t *m, uint32_t enter_us, uint32_t exit_us, uint32_t dma_blk);
/* recorder_poll() saw `dropped` new overruns and wrote `comp` zero blocks for them. */
void bench_meas_overrun(bench_meas_t *m, uint32_t dropped, uint32_t comp);
/* One WAV block write: frames written (0 if it failed), its exfat_write() time, its checkpoint time (0 = none). */
void bench_meas_write(bench_meas_t *m, uint32_t frames, uint32_t wr_us, uint32_t ckpt_us);
/* recorder_stop(): right after audio_capture_stop() (the elapsed time stops here; the final drain does not count). */
void bench_meas_capture_end(bench_meas_t *m, uint32_t now_us);
/* recorder_stop() after the ring was drained: final DMA block and overrun counts. Archives the recording. */
void bench_meas_rec_stop(bench_meas_t *m, uint32_t dma_blk, uint32_t ov_total);
/* GETS payload bytes of one frame sent. */
void bench_meas_gets(bench_meas_t *m, uint32_t bytes);
/* One CPU window (permille of wall time; win_ms its length). */
void bench_meas_cpu(bench_meas_t *m, uint32_t idle_pm, uint32_t app_pm, uint32_t btc_pm, uint32_t win_ms);

/* i-th latest finished recording (0 = latest), NULL if there is none. */
const bench_meas_rec_t *bench_meas_hist(const bench_meas_t *m, uint32_t i);
/* (dma - ov + comp) * 2048 - wav, in frames. */
int32_t  bench_meas_acct(const bench_meas_rec_t *r);
/* frames expected from the clock since capture start (16 kHz). */
uint32_t bench_meas_clk_frames(const bench_meas_rec_t *r);
/* dma * 2048 - clock frames: 0..-2048 (the block in flight) plus any clock / sample-rate mismatch. */
int32_t  bench_meas_drift(const bench_meas_rec_t *r);

/* "k=v" fields joined by sep (no leading or trailing sep), NUL-terminated, truncated at cap. Returns the length.
 * rec: seq file rec ch gain dsp vor t_ms dma_smp clk_smp drift wav_smp ov comp lost acct gap_max_us poll_max_us
 *      wr_max_us wr_n ckpt_max_us gets_b idle_min_pm cpu_n
 * cpu: gets_tot gets_fr idle_pm app_pm btc_pm cpu_ms */
uint32_t bench_meas_format_rec(const bench_meas_rec_t *r, char sep, char *buf, uint32_t cap);
uint32_t bench_meas_format_cpu(const bench_meas_t *m, char sep, char *buf, uint32_t cap);

#endif /* OPENFW_BENCH_MEAS_H */
