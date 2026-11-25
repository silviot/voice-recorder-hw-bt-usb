/* Headphones-mode transmit path (docs/BT_A2DP_DESIGN.md rev 3 §4.3-4.6): credit pacer, SBC packetiser, stack-queue
 * accounting, the real-frame position (packet-record FIFO, flush epochs, rewind snapshot) and the credit-stall
 * watchdog. Pure C: time, buffers, send and the stack's queue counters come through hp_tx_ops_t, so
 * tests/test_hp_tx.c runs it on virtual time against a fake stack. On target bt_port/hp_tx_bd.c binds the ops to
 * Bluedroid and hp_tx_tick() runs in BTC context on an osi_alarm.
 *
 * Units (every name says which): *_pcm / *_src = PCM frames (one sample per channel; *_src at the SOURCE rate),
 * *_sbc = SBC frames (blocks x subbands connection-rate PCM frames, 128 for the A2DP default 16 x 8).
 *
 * Threads: hp_tx_init/set_cfg/start/stop/tick run in BTC context (serialised with the stack's own A2DP state).
 * The app task calls only hp_tx_app_* and hp_tx_request_flush/drop, which read a published snapshot or post requests
 * through single-writer atomic fields. hp_tx_on_nocp is called from one other context (bt_ctrl task).
 * SPDX-License-Identifier: MIT */
#ifndef HP_TX_H
#define HP_TX_H
#include <stdint.h>
#include "hp_sbc.h"
#include "hp_sink.h"

/* Defaults (§4.3; bench-overridable through hp_tx_tun_t). */
#define HP_TICK_MS            20u
#define HP_MAX_BITPOOL        53u   /* advertised max (SEP caps): the A2DP high-quality value for 48 kHz joint stereo */
#define HP_TXQ_PKT_CAP        4u    /* = MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ + 1 (bt_port_prefix.h, Bluedroid patch 0034) */
#define HP_MAX_PKT_PER_TICK   2u
#define HP_CREDIT_TICKS       4u    /* 80 ms: a BTC tick held up < 80 ms by higher-priority tasks loses no air time
                                       * (silicon 2026-11-25: with 2, 3.3 % of the stream was lost to late ticks) */
#define HP_LAT_MS             150u  /* used until the sink sends a delay report */
#define HP_STALL_MS           800u
#define HP_SPEECH_BP          0u    /* per-packet bitpool for sources <= 16 kHz (0 = always max); was 24 (CPU), the
                                       * encoder costs about the same at 53 (qemu-arm count: +14 %) */
#define HP_FPP_MAX            15u   /* 4-bit frame count in the A2DP SBC media payload header */
#define HP_DOWN_PKTS_MAX      10u   /* BTA a2d_list <= 1, L2CAP <= 5, controller <= 4 credits (rev 3) */
#define HP_LINK_END_PKTS      4u    /* link end: the ROM reports up to 4 never-aired packets as completed (§4.4) */
#define HP_REC_MAX            32u   /* packet records: in flight (<= 14) + acked ones kept for lat_ms (overflow drops
                                       * the oldest acked one: lat_term reads short, counted in rec_overflow) */

/* send() results */
#define HP_SEND_OK     0
#define HP_SEND_FAIL   1   /* refused (should not happen after can_send): the buffer is still ours */

typedef struct {
    uint32_t enq_pkts, enq_sbc, deq_pkts, deq_sbc, flushed_pkts, flushed_sbc, qlen;
} hp_txq_stat_t;

typedef struct {
    int64_t  (*now_us)(void *ctx);
    /* STARTED and not flushing: the checks the stack's send path makes (§4.3 step 4, rev 3). BTC context. */
    int      (*can_send)(void *ctx);
    /* Media buffer with room for len payload bytes; *data receives where to write them. NULL = no memory. */
    void    *(*alloc)(void *ctx, uint32_t len, uint8_t **data);
    void     (*free)(void *ctx, void *buf);
    /* Hand a filled buffer (len bytes, nframes SBC frames, RTP timestamp ts) to the stack. HP_SEND_*. */
    int      (*send)(void *ctx, void *buf, uint32_t len, uint16_t nframes, uint32_t ts);
    void     (*txq_stat)(void *ctx, hp_txq_stat_t *st);   /* Bluedroid patch 0034 counters */
    uint32_t (*txq_drop)(void *ctx);                      /* patch 0034: drop audio_tx_q, SBC frames dropped */
    void     (*event)(void *ctx, int evt);                /* HP_TX_EVT_*, from the tick (BTC) */
    void    *ctx;
} hp_tx_ops_t;

#define HP_TX_EVT_STALL  1   /* acked_sbc stuck for stall_ms with frames downstream (§4.6), or send() refused for
                              * stall_ms; the tick has stopped */

/* hp_tx_stop reasons */
#define HP_STOP_SUSPEND  0   /* local / remote SUSPEND, CLOSE, shutdown */
#define HP_STOP_LINK     1   /* link loss or stall: the snapshot adds back the last HP_LINK_END_PKTS acked packets */

typedef struct {   /* tunables (§10.2 bench keys); zero fields take the defaults */
    uint16_t tick_ms, txq_cap, max_pkt_per_tick, credit_ticks, stall_ms, lat_ms;
    uint8_t  speech_bp;      /* 0xFF = use the default; 0 = always the max bitpool */
    uint8_t  flush_drops_txq;/* 0xFF = default (1) */
} hp_tx_tun_t;

typedef struct {
    uint32_t pkts, frames_sbc, pad_pcm, real_pcm, pad_pkts, can_send_no, alloc_fail, tx_lost_sbc, txq_max;
    uint32_t stalls, flush_drop_sbc, ticks, tick_late, max_burst, rec_overflow;
    uint32_t nocp_gap_max_ms;   /* longest gap between two acks while frames were downstream (bench: Exp 4c) */
} hp_tx_stats_t;

typedef struct {           /* one media packet, in send order */
    uint16_t real_src;     /* source-rate frames taken from the ring (pads and silence count 0) */
    uint8_t  nf;           /* SBC frames */
    uint8_t  state;        /* HP_REC_* */
    uint32_t epoch;        /* sink flush generation the PCM came from */
    uint32_t acked_us;     /* low 32 bits of now_us when acked */
} hp_rec_t;
#define HP_REC_TXQ   0u    /* in audio_tx_q */
#define HP_REC_DOWN  1u    /* handed to BTA / L2CAP / controller */
#define HP_REC_ACKED 2u    /* NOCP-completed (heard after lat_ms) */

typedef struct {
    hp_tx_ops_t ops;
    hp_sink_t  *sink;
    hp_tx_tun_t tun;          /* effective values */
    /* configuration (BTC) */
    hp_sbc_cfg_t cfg, cfg_next;
    uint8_t  cfg_valid, cfg_pending;
    uint32_t audio_mtu, conn_rate, spf;   /* spf = PCM frames per SBC frame */
    uint32_t fpp, frame_len_max, tick_ms_eff;
    /* pacer (BTC) */
    uint8_t  started, send_failing, stalled;   /* send_failing: send() refused since send_fail_us (stall after
                                                 * stall_ms) */
    int64_t  send_fail_us;
    int64_t  last_us;
    uint64_t credit;          /* PCM frames x 1e6 (exact integer credit) */
    uint64_t credit_max;
    uint32_t ts;              /* RTP timestamp, samples */
    /* accounting, SBC frames, cumulative (BTC) */
    hp_txq_stat_t last_st;
    uint32_t sent_sbc, deq_sbc, flushed_sbc, acked_sbc;
    uint32_t nocp_seen;
    int64_t  last_ack_us;
    uint32_t seen_flush_req, seen_drop_req;
    /* packet records: [rh, rt) in send order; acked ones stay for lat_ms (BTC) */
    hp_rec_t rec[HP_REC_MAX];
    uint32_t rh, rt;
    uint32_t lost_src;        /* current-epoch real frames that finished without being heard (flush, rebase, loss) */
    uint32_t lost_epoch;
    /* app <-> BTC (HP_LD/HP_ST, hp_sink.h) */
    uint32_t nocp_pkts;       /* hp_tx_on_nocp (bt_ctrl task) */
    uint32_t flush_req;       /* app: hp_tx_request_flush */
    uint32_t drop_req;        /* app: hp_tx_request_drop (aout_stop) */
    uint32_t lat_ms;          /* bt_hp: delay report / 10, 0 = default */
    /* published snapshot for the app: BTC writes under a sequence count, hp_tx_app_* read it */
    uint32_t pub_seq;
    uint32_t pub_epoch;       /* sink generation the values below belong to */
    uint32_t pub_inflight_src;/* current-epoch real frames sent but not yet acked */
    uint32_t pub_lost_src;
    uint32_t pub_lat_src;     /* lat_term (§4.4) */
    uint32_t pub_last_real_ack_us; /* 0 = none within lat_ms */
    uint32_t pub_frozen;      /* rewind snapshot valid while the sink generation is pub_frozen_epoch */
    uint32_t pub_frozen_epoch, pub_rewind_src;
    uint32_t pub_acked_sbc;
    hp_tx_stats_t st;
} hp_tx_t;

/* Init (BTC, or before the tick exists). tun may be NULL. */
void     hp_tx_init(hp_tx_t *t, const hp_tx_ops_t *ops, hp_sink_t *sink, const hp_tx_tun_t *tun);
/* Negotiated codec configuration (ESP_A2D_AUDIO_CFG_EVT) and the stream's audio MTU. Applied at the next
 * hp_tx_start (a reconfiguration needs a suspend, so fpp never changes inside a STARTED period). 0 = ok. */
int      hp_tx_set_cfg(hp_tx_t *t, const hp_sbc_cfg_t *cfg, uint32_t audio_mtu);
/* STARTED (one bt_hp asked for): rebase, fix fpp, start pacing. Returns the tick period to arm (ms; lowered from
 * tun.tick_ms if the headroom rule of §4.3 needs it), 0 = no valid configuration. */
uint32_t hp_tx_start(hp_tx_t *t);
/* SUSPEND / CLOSE / link loss / stall / shutdown: take the rewind snapshot, stop pacing, rebase. HP_STOP_*. */
void     hp_tx_stop(hp_tx_t *t, int reason);
/* One pacer tick (BTC). */
void     hp_tx_tick(hp_tx_t *t);
/* NOCP for the stream's ACL handle: npkts completed packets. One context. */
void     hp_tx_on_nocp(hp_tx_t *t, uint32_t npkts);
/* Sink delay report in 1/10 ms (0 = none: HP_LAT_MS). */
void     hp_tx_set_delay_report(hp_tx_t *t, uint32_t delay_0p1ms);

/* ---- app task ---- */
/* Seek: drops the ring at once (new flush epoch) and asks the tick to drop audio_tx_q if enabled (§4.5). */
void     hp_tx_request_flush(hp_tx_t *t);
/* End of file (aout_stop, rev 3 R-M7): like a flush, and audio_tx_q is always dropped. */
void     hp_tx_request_drop(hp_tx_t *t);
/* Source-rate frames written but not yet heard: ring + unheard real frames + latency term, or the frozen rewind
 * snapshot after a stop until the next flush (§4.4). 0 right after a flush. */
uint32_t hp_tx_app_queued_frames(const hp_tx_t *t);
/* Real (non-pad) audio still to be heard. */
int      hp_tx_app_is_busy(const hp_tx_t *t);
uint32_t hp_tx_lat_ms(const hp_tx_t *t);

/* ---- helpers (exposed for tests) ---- */
/* SBC frames per packet: as many as fit the connection's audio_mtu (it already excludes the RTP and SBC media headers),
 * at most HP_FPP_MAX; 0 = none fit. */
uint32_t hp_tx_fpp(uint32_t audio_mtu, uint32_t frame_len);
/* Bitpool the next packet is encoded at (the speech bitpool for sources <= 16 kHz, else the max; BTC / bench). */
uint8_t  hp_tx_packet_bitpool(const hp_tx_t *t);
uint64_t hp_tx_credit_max(uint32_t tick_ms, uint32_t credit_ticks, uint32_t conn_rate, uint32_t max_pkt,
                          uint32_t fpp, uint32_t spf);
/* Largest tick period (<= tick_ms) satisfying the headroom rule max_pkt * fpp * spf * 1000 / tick >= 1.2 * rate. */
uint32_t hp_tx_tick_for_headroom(uint32_t tick_ms, uint32_t max_pkt, uint32_t fpp, uint32_t spf, uint32_t rate);
/* Accounting (BTC; tests). */
uint32_t hp_tx_stack_sbc(const hp_tx_t *t);
uint32_t hp_tx_down_sbc(const hp_tx_t *t);
#endif
