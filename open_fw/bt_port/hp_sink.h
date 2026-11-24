/* Headphones-mode PCM sink (docs/BT_A2DP_DESIGN.md §3.3-3.4): lock-free SPSC ring at the source rate and channel
 * count, producer-side flush index, resampler to the A2DP connection rate, mono <-> stereo, gain and mute.
 * Pure C, no RTOS or Bluedroid dependency (host-tested by tests/test_hp_sink.c, TSan stress included).
 *
 * Threads: ONE producer (app task: hp_sink_set_format/write/flush/queued/tap_*), ONE consumer (BTC tick:
 * hp_sink_set_out_rate/pull). Indices are free-running uint32_t frame counters; payload stores are ordered before
 * index stores (release), and the consumer re-checks the flush generation after copying a chunk (seqlock), so it
 * never consumes a slot the producer rewrote after a flush.
 *
 * The optional "tap" is a second, producer-owned reader for output mode BOTH (audio_out.c): it lets the app feed
 * the DAC from the same ring, delayed so that both outputs play the same audio at about the same time.
 * SPDX-License-Identifier: MIT */
#ifndef HP_SINK_H
#define HP_SINK_H
#include <stdint.h>

/* Shared-field access: __atomic builtins (Cortex-M4: LDR/STR, DMB for acquire/release/fence; no libcalls for
 * <= 32-bit objects). HP_SINK_DMB is kept as the full barrier name used by hp_tx.c. */
#define HP_LD(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define HP_LDA(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define HP_ST(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define HP_STR(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define HP_FENCE()    __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define HP_SINK_DMB() HP_FENCE()

#define HP_SINK_CHUNK   64u   /* consumer copy granule (source frames) */
#define HP_RS_HIST      32u   /* resampler history per channel (>= 24 taps + lookahead) */

typedef enum { HP_RS_COPY = 0, HP_RS_X3, HP_RS_GEN } hp_rs_mode_t;

typedef struct {
    hp_rs_mode_t mode;
    uint32_t src, dst;
    uint64_t step_q32;        /* src / dst in Q32 (GEN) */
    uint64_t frac_q32;        /* fractional input position (GEN) */
    uint32_t ahead;           /* inputs pushed beyond the current base index (GEN) */
    uint8_t  x3_phase;        /* next output phase 0..2 (X3); 3 = need an input */
    uint8_t  ch;              /* channels in the history (ring channel count) */
    uint8_t  down;            /* GEN uses the downsampling table */
    uint32_t widx;
    int16_t  hist[2][2u * HP_RS_HIST];
} hp_rs_t;

typedef struct {
    /* storage (set once) */
    int16_t *buf;
    uint32_t samples;         /* int16 capacity, power of two */
    uint32_t cap;             /* producer: frames at the producer's channel count (samples / p_ch) */
    /* shared indices */
    uint32_t wr;         /* producer */
    uint32_t rd;         /* consumer */
    uint32_t flush_wr;   /* producer: everything before it is dropped */
    uint32_t flush_gen;  /* producer: bumped after flush_wr / fmt_* */
    uint32_t c_gen;      /* consumer: last flush_gen applied */
    uint32_t c_held;     /* consumer: frames copied out of the ring but not yet resampled */
    uint32_t fmt_rate;   /* producer -> consumer, published with a flush */
    uint8_t  fmt_ch;
    uint8_t  mute;
    uint16_t gain_q15;   /* 0..32768 */
    /* producer-only */
    uint32_t p_rate;
    uint8_t  p_ch;
    uint8_t  tap_on;
    uint32_t tap_rd;
    uint32_t ch_mismatch;
    /* consumer-only */
    uint32_t c_rate, c_out_rate, c_cap;
    uint8_t  c_ch;
    uint32_t c_seen_gen;
    uint32_t chunk_pos, chunk_len;
    int16_t  chunk[HP_SINK_CHUNK * 2u];
    uint32_t torn_chunks;         /* chunks discarded because a flush raced the copy */
    hp_rs_t  rs;
} hp_sink_t;

/* Storage: buf_bytes / 2 samples rounded down to a power of two (16 KiB: 4096 stereo or 8192 mono frames). */
void     hp_sink_init(hp_sink_t *s, int16_t *buf, uint32_t buf_bytes);

/* ---- producer (app task) ---- */
/* New file: source rate and channel count (1 or 2). Drops everything queued; the consumer applies it at its next
 * pull (resampler reset). */
void     hp_sink_set_format(hp_sink_t *s, uint32_t src_rate, uint8_t ch);
/* Copies min(n, free) frames; returns frames accepted. ch must equal the ring's channel count, or be 2 with a mono
 * ring (downmix (L+R)/2); anything else returns 0 and counts ch_mismatch. */
uint32_t hp_sink_write(hp_sink_t *s, const int16_t *pcm, uint32_t n, uint8_t ch);
/* Drop everything not yet consumed, at once from the producer's point of view (§3.3). */
void     hp_sink_flush(hp_sink_t *s);
/* Source-rate frames not yet resampled (ring + consumer chunk). */
uint32_t hp_sink_queued(const hp_sink_t *s);
uint32_t hp_sink_free(const hp_sink_t *s);
void     hp_sink_set_mute(hp_sink_t *s, int muted);
void     hp_sink_set_gain_q15(hp_sink_t *s, uint16_t gain_q15);
uint32_t hp_sink_src_rate(const hp_sink_t *s);
uint8_t  hp_sink_channels(const hp_sink_t *s);
/* Tap (output mode BOTH): a second reader the producer owns. While on, ring space is only freed once both readers
 * passed it. avail = wr - tap_rd. */
void     hp_sink_tap_enable(hp_sink_t *s, int on);
uint32_t hp_sink_tap_avail(const hp_sink_t *s);
/* Copy up to max frames from the tap position as interleaved stereo without consuming them. */
uint32_t hp_sink_tap_peek(const hp_sink_t *s, int16_t *out_lr, uint32_t max);
void     hp_sink_tap_advance(hp_sink_t *s, uint32_t n);

/* ---- consumer (BTC tick) ---- */
/* Apply a pending flush / format change now; returns the flush generation the consumer is at (the position epoch,
 * §4.4). */
uint32_t hp_sink_sync(hp_sink_t *s);
/* Connection (output) rate; resets the resampler. */
void     hp_sink_set_out_rate(hp_sink_t *s, uint32_t conn_rate);
/* Produce n output frames at the connection rate, out_ch (1 or 2) interleaved, gain and mute applied; out must hold
 * n * 2 samples. Returns the frames that carry real audio; the rest of out (an underrun) is zero-filled. Mute counts
 * as real (the ring drains, so the position advances as with the DAC). *src_used (may be NULL) receives the source
 * frames taken from the ring for this call: the unit of the position math (§4.4). */
uint32_t hp_sink_pull(hp_sink_t *s, int16_t *out, uint32_t n, uint8_t out_ch, uint32_t *src_used);

/* ---- resampler (exposed for tests) ---- */
void     hp_rs_reset(hp_rs_t *rs, uint32_t src, uint32_t dst, uint8_t ch);
/* Resample: consumes from in (n_in frames, rs->ch interleaved) and writes up to n_out frames (rs->ch interleaved).
 * *used_in returns the inputs consumed. Returns outputs produced. */
uint32_t hp_rs_process(hp_rs_t *rs, const int16_t *in, uint32_t n_in, uint32_t *used_in, int16_t *out, uint32_t n_out);
#endif
