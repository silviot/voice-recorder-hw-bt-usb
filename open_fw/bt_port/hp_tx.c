/* Headphones-mode transmit path: pacer, packetiser, accounting, position (docs/BT_A2DP_DESIGN.md rev 3 §4.3-4.6).
 * See hp_tx.h. SPDX-License-Identifier: MIT */
#include "hp_tx.h"
#include <string.h>

#define US_PER_S 1000000ull
#define HP_REC_FLUSHED 3u   /* left audio_tx_q by a flush: finished, not heard */

static inline uint32_t umin32(uint32_t a, uint32_t b) { return a < b ? a : b; }
/* wrap-safe a - b clamped at 0 */
static inline uint32_t pos_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0 ? a - b : 0u; }

/* audio_mtu is ESP_A2D_CONNECTION_STATE_EVT's: the peer L2CAP MTU less the 12-byte RTP header (bta_av_aact.c) and
 * less the 1-byte SBC media payload header (btc_av.c BTC_AV_AUDIO_MTU_RESERVE), the bound the send path checks
 * (btc_a2d_src_audio_mtu_check: data_len <= mtu). So the SBC frames may fill it exactly. */
uint32_t hp_tx_fpp(uint32_t audio_mtu, uint32_t frame_len)
{
    if (frame_len == 0u || audio_mtu < frame_len) {
        return 0u;
    }
    return umin32(HP_FPP_MAX, audio_mtu / frame_len);
}

uint64_t hp_tx_credit_max(uint32_t tick_ms, uint32_t credit_ticks, uint32_t rate, uint32_t max_pkt, uint32_t fpp,
                          uint32_t spf)
{
    uint64_t a = (uint64_t)credit_ticks * tick_ms * rate / 1000u;
    uint64_t b = (uint64_t)max_pkt * fpp * spf;
    return a > b ? a : b;
}

uint32_t hp_tx_tick_for_headroom(uint32_t tick_ms, uint32_t max_pkt, uint32_t fpp, uint32_t spf, uint32_t rate)
{
    /* max_pkt * fpp * spf * 1000 / tick >= 1.2 * rate  <=>  max_pkt * fpp * spf * 10000 >= 12 * rate * tick */
    while (tick_ms > 1u && (uint64_t)max_pkt * fpp * spf * 10000u < 12ull * rate * tick_ms) {
        tick_ms--;
    }
    return tick_ms;
}

static void apply_tun(hp_tx_tun_t *o, const hp_tx_tun_t *in)
{
    o->tick_ms = (in && in->tick_ms) ? in->tick_ms : HP_TICK_MS;
    o->txq_cap = (in && in->txq_cap) ? in->txq_cap : HP_TXQ_PKT_CAP;
    o->max_pkt_per_tick = (in && in->max_pkt_per_tick) ? in->max_pkt_per_tick : HP_MAX_PKT_PER_TICK;
    o->credit_ticks = (in && in->credit_ticks) ? in->credit_ticks : HP_CREDIT_TICKS;
    o->stall_ms = (in && in->stall_ms) ? in->stall_ms : HP_STALL_MS;
    o->lat_ms = (in && in->lat_ms) ? in->lat_ms : HP_LAT_MS;
    o->speech_bp = (in && in->speech_bp != 0xFFu) ? in->speech_bp : HP_SPEECH_BP;
    o->flush_drops_txq = (in && in->flush_drops_txq != 0xFFu) ? in->flush_drops_txq : 1u;
}

void hp_tx_init(hp_tx_t *t, const hp_tx_ops_t *ops, hp_sink_t *sink, const hp_tx_tun_t *tun)
{
    memset(t, 0, sizeof(*t));
    t->ops = *ops;
    t->sink = sink;
    apply_tun(&t->tun, tun);
    if (t->ops.txq_stat) {
        t->ops.txq_stat(t->ops.ctx, &t->last_st);
    }
}

int hp_tx_set_cfg(hp_tx_t *t, const hp_sbc_cfg_t *cfg, uint32_t audio_mtu)
{
    if (cfg == NULL || hp_sbc_frame_len(cfg, cfg->max_bp) == 0u || hp_sbc_frame_len(cfg, cfg->max_bp) > audio_mtu) {
        return -1;
    }
    t->cfg_next = *cfg;
    t->audio_mtu = audio_mtu;
    t->cfg_pending = 1u;
    return 0;
}

uint32_t hp_tx_lat_ms(const hp_tx_t *t)
{
    uint32_t l = HP_LD(&t->lat_ms);
    return l ? l : t->tun.lat_ms;
}

/* ------------------------------------------------------------------------------------------------ records */

static inline hp_rec_t *rec_at(hp_tx_t *t, uint32_t i) { return &t->rec[i % HP_REC_MAX]; }

static void lose(hp_tx_t *t, uint32_t epoch, uint32_t real_src, uint32_t cur)
{
    if (epoch != cur || real_src == 0u) {
        return;   /* older epochs no longer count (the player flushed them) */
    }
    if (t->lost_epoch != cur) {
        t->lost_epoch = cur;
        t->lost_src = 0u;
    }
    t->lost_src += real_src;
}

static void rec_push(hp_tx_t *t, uint32_t real_src, uint32_t nf, uint32_t epoch)
{
    if (t->rt - t->rh == HP_REC_MAX) {
        t->st.rec_overflow++;   /* only acked records can be this old: drop the oldest (lat_term reads short) */
        t->rh++;
    }
    hp_rec_t *r = rec_at(t, t->rt++);
    r->real_src = (uint16_t)umin32(real_src, 0xFFFFu);
    r->nf = (uint8_t)nf;
    r->state = HP_REC_TXQ;
    r->epoch = epoch;
    r->acked_us = 0u;
}

/* Fold the stack's counters (patch 0034) and NOCPs into our counters and the packet records. audio_tx_q is FIFO and
 * only we enqueue (from this tick), so between two folds BTU dequeues from its head and a flush frees what is left:
 * the oldest TXQ records went downstream, the next ones were flushed. */
static void fold(hp_tx_t *t, int64_t now, uint32_t cur)
{
    hp_txq_stat_t s;
    memset(&s, 0, sizeof(s));
    if (t->ops.txq_stat) {
        t->ops.txq_stat(t->ops.ctx, &s);
    }
    uint32_t d_deq = s.deq_sbc - t->last_st.deq_sbc, d_fl = s.flushed_sbc - t->last_st.flushed_sbc;
    t->deq_sbc += d_deq;
    t->flushed_sbc += d_fl;
    t->last_st = s;
    if (s.qlen > t->st.txq_max) {
        t->st.txq_max = s.qlen;
    }
    for (uint32_t i = t->rh; i != t->rt && (d_deq || d_fl); i++) {
        hp_rec_t *r = rec_at(t, i);
        if (r->state != HP_REC_TXQ) {
            continue;
        }
        if (d_deq >= r->nf) {
            r->state = HP_REC_DOWN;
            d_deq -= r->nf;
        } else if (d_fl >= r->nf) {
            r->state = HP_REC_FLUSHED;
            d_fl -= r->nf;
            lose(t, r->epoch, r->real_src, cur);
        } else {
            break;
        }
    }
    uint32_t n = HP_LD(&t->nocp_pkts), dn = n - t->nocp_seen;
    t->nocp_seen = n;
    if (dn) {
        uint32_t before = t->acked_sbc;
        t->acked_sbc += t->fpp * dn;
        if ((int32_t)(t->acked_sbc - t->deq_sbc) > 0) {
            t->acked_sbc = t->deq_sbc;   /* never past what BTA handed to L2CAP (signalling NOCPs make it early) */
        }
        uint32_t frames = t->acked_sbc - before;
        if (frames) {
            if (t->started && now > t->last_ack_us) {
                uint32_t gap_ms = (uint32_t)((now - t->last_ack_us) / 1000);
                if (gap_ms > t->st.nocp_gap_max_ms) {
                    t->st.nocp_gap_max_ms = gap_ms;   /* longest wait for an ack with packets downstream (§3.5) */
                }
            }
            t->last_ack_us = now;
        }
        for (uint32_t i = t->rh; i != t->rt && frames; i++) {
            hp_rec_t *r = rec_at(t, i);
            if (r->state != HP_REC_DOWN) {
                continue;
            }
            if (frames < r->nf) {
                break;
            }
            r->state = HP_REC_ACKED;
            r->acked_us = (uint32_t)now;
            frames -= r->nf;
        }
    }
    /* drop finished records: flushed ones, and acked ones older than lat_ms */
    const uint32_t lat_us = hp_tx_lat_ms(t) * 1000u;
    while (t->rh != t->rt) {
        hp_rec_t *r = rec_at(t, t->rh);
        if (r->state == HP_REC_FLUSHED || (r->state == HP_REC_ACKED && (uint32_t)now - r->acked_us >= lat_us)) {
            t->rh++;
        } else {
            break;
        }
    }
}

uint32_t hp_tx_stack_sbc(const hp_tx_t *t)
{
    return pos_diff(t->sent_sbc, t->deq_sbc + t->flushed_sbc);
}

uint32_t hp_tx_down_sbc(const hp_tx_t *t)
{
    return umin32(pos_diff(t->deq_sbc, t->acked_sbc), HP_DOWN_PKTS_MAX * t->fpp);
}

typedef struct {
    uint32_t inflight, lat_term, last_real_ack, link_end;
} pos_t;

static pos_t position(hp_tx_t *t, int64_t now, uint32_t cur)
{
    pos_t p = { 0, 0, 0, 0 };
    const uint32_t lat_us = hp_tx_lat_ms(t) * 1000u, now32 = (uint32_t)now;
    uint32_t acked_in_window = 0, tail_acked = 0;
    for (uint32_t i = t->rt; i != t->rh;) {   /* newest first, for the link-end add-back */
        hp_rec_t *r = rec_at(t, --i);
        if (r->epoch != cur) {
            continue;
        }
        if (r->state == HP_REC_TXQ || r->state == HP_REC_DOWN) {
            p.inflight += r->real_src;
        } else if (r->state == HP_REC_ACKED && now32 - r->acked_us < lat_us) {
            acked_in_window += r->real_src;
            if (r->real_src && (p.last_real_ack == 0u || (int32_t)(r->acked_us - p.last_real_ack) > 0)) {
                p.last_real_ack = r->acked_us ? r->acked_us : 1u;
            }
            if (tail_acked < HP_LINK_END_PKTS) {
                p.link_end += r->real_src;
                tail_acked++;
            }
        }
    }
    uint32_t src = t->sink->c_rate ? t->sink->c_rate : t->conn_rate;
    p.lat_term = umin32((uint32_t)((uint64_t)hp_tx_lat_ms(t) * src / 1000u), acked_in_window);
    return p;
}

static void publish(hp_tx_t *t, int64_t now, uint32_t cur)
{
    pos_t p = position(t, now, cur);
    uint32_t seq = HP_LD(&t->pub_seq);
    HP_ST(&t->pub_seq, seq + 1u);   /* odd: update in progress */
    HP_FENCE();
    HP_ST(&t->pub_epoch, cur);
    HP_ST(&t->pub_inflight_src, p.inflight);
    HP_ST(&t->pub_lost_src, t->lost_epoch == cur ? t->lost_src : 0u);
    HP_ST(&t->pub_lat_src, p.lat_term);
    HP_ST(&t->pub_last_real_ack_us, p.last_real_ack);
    HP_ST(&t->pub_acked_sbc, t->acked_sbc);
    HP_STR(&t->pub_seq, seq + 2u);
}

static void freeze(hp_tx_t *t, uint32_t cur, uint32_t value, uint32_t on)
{
    uint32_t seq = HP_LD(&t->pub_seq);
    HP_ST(&t->pub_seq, seq + 1u);
    HP_FENCE();
    HP_ST(&t->pub_frozen_epoch, cur);
    HP_ST(&t->pub_rewind_src, value);
    HP_ST(&t->pub_frozen, on);
    HP_STR(&t->pub_seq, seq + 2u);
}

/* Everything downstream counts as finished (§4.4 rebase); records still in flight were not heard. */
static void rebase(hp_tx_t *t, uint32_t cur)
{
    for (uint32_t i = t->rh; i != t->rt; i++) {
        hp_rec_t *r = rec_at(t, i);
        if (r->state == HP_REC_TXQ || r->state == HP_REC_DOWN) {
            lose(t, r->epoch, r->real_src, cur);
        }
    }
    t->rh = t->rt;
    t->acked_sbc = t->deq_sbc;
    t->nocp_seen = HP_LD(&t->nocp_pkts);
}

uint32_t hp_tx_start(hp_tx_t *t)
{
    int64_t now = t->ops.now_us(t->ops.ctx);
    if (t->cfg_pending) {
        if (hp_sbc_init(&t->cfg_next) != 0) {
            return 0u;
        }
        t->cfg = t->cfg_next;
        t->cfg_valid = 1u;
        t->cfg_pending = 0u;
    }
    if (!t->cfg_valid) {
        return 0u;
    }
    uint32_t cur = hp_sink_sync(t->sink);
    fold(t, now, cur);   /* stop-time flushes, counted with the previous period's fpp */
    t->spf = hp_sbc_pcm_per_frame(&t->cfg);
    t->conn_rate = t->cfg.rate;
    t->frame_len_max = hp_sbc_frame_len(&t->cfg, t->cfg.max_bp);
    t->fpp = hp_tx_fpp(t->audio_mtu, t->frame_len_max);
    if (t->fpp == 0u) {
        return 0u;
    }
    t->tick_ms_eff = hp_tx_tick_for_headroom(t->tun.tick_ms, t->tun.max_pkt_per_tick, t->fpp, t->spf, t->conn_rate);
    t->credit_max = hp_tx_credit_max(t->tick_ms_eff, t->tun.credit_ticks, t->conn_rate, t->tun.max_pkt_per_tick,
                                     t->fpp, t->spf) * US_PER_S;
    t->credit = (uint64_t)t->fpp * t->spf * US_PER_S;   /* first packet at once */
    t->last_us = now;
    t->last_ack_us = now;
    rebase(t, cur);
    hp_sink_set_out_rate(t->sink, t->conn_rate);
    t->seen_flush_req = HP_LD(&t->flush_req);
    t->seen_drop_req = HP_LD(&t->drop_req);
    t->send_failing = 0u;
    t->stalled = 0u;
    t->started = 1u;
    publish(t, now, cur);
    return t->tick_ms_eff;
}

void hp_tx_stop(hp_tx_t *t, int reason)
{
    int64_t now = t->ops.now_us(t->ops.ctx);
    uint32_t cur = hp_sink_sync(t->sink);
    if (t->cfg_valid && t->fpp) {
        fold(t, now, cur);
    }
    /* Rewind snapshot (§4.4): what the player has not heard, frozen until its next flush. */
    pos_t p = position(t, now, cur);
    uint32_t snap = hp_sink_queued(t->sink) + p.inflight + (t->lost_epoch == cur ? t->lost_src : 0u) + p.lat_term;
    if (reason == HP_STOP_LINK) {
        snap += p.link_end;   /* the ROM's link end reports never-aired packets as completed */
    }
    freeze(t, cur, snap, 1u);
    t->started = 0u;
    rebase(t, cur);
    publish(t, now, cur);
}

void hp_tx_on_nocp(hp_tx_t *t, uint32_t npkts)
{
    HP_ST(&t->nocp_pkts, HP_LD(&t->nocp_pkts) + npkts);   /* single writer (bt_ctrl task) */
}

void hp_tx_set_delay_report(hp_tx_t *t, uint32_t d)
{
    HP_ST(&t->lat_ms, (d + 5u) / 10u);
}

/* Bitpool for the next packet: the max, or the speech bitpool for sources <= 16 kHz (§3.5). */
uint8_t hp_tx_packet_bitpool(const hp_tx_t *t)
{
    uint8_t bp = t->cfg.max_bp;
    if (t->tun.speech_bp && t->sink->c_rate && t->sink->c_rate <= 16000u && t->tun.speech_bp < bp) {
        bp = t->tun.speech_bp < t->cfg.min_bp ? t->cfg.min_bp : t->tun.speech_bp;
    }
    return bp;
}

void hp_tx_tick(hp_tx_t *t)
{
    if (!t->started || t->stalled) {
        return;
    }
    int64_t now = t->ops.now_us(t->ops.ctx);
    t->st.ticks++;
    int64_t el = now - t->last_us;
    int64_t clamp = (int64_t)t->tun.credit_ticks * t->tick_ms_eff * 1000;
    t->last_us = now;
    if (el < 0) {
        el = 0;
    }
    if (el > clamp) {
        el = clamp;   /* elapsed clamp (§4.3 step 1) */
        t->st.tick_late++;
    }
    t->credit += (uint64_t)el * t->conn_rate;
    if (t->credit > t->credit_max) {
        t->credit = t->credit_max;   /* credit clamp (step 2) */
    }

    /* Seek (flush) or end of file (drop) from the app: the ring is already dropped; audio_tx_q too if asked (§4.5). */
    uint32_t fr = HP_LDA(&t->flush_req), dr = HP_LDA(&t->drop_req);
    if (fr != t->seen_flush_req || dr != t->seen_drop_req) {
        int drop = (dr != t->seen_drop_req) || t->tun.flush_drops_txq;
        t->seen_flush_req = fr;
        t->seen_drop_req = dr;
        if (drop && t->ops.txq_drop) {
            uint32_t d = t->ops.txq_drop(t->ops.ctx);
            t->ts -= d * t->spf;   /* RTP timestamps stay contiguous */
            t->st.flush_drop_sbc += d;
        }
    }
    uint32_t cur = hp_sink_sync(t->sink);
    fold(t, now, cur);

    uint32_t qlen = t->last_st.qlen;
    uint32_t room = qlen >= t->tun.txq_cap ? 0u : t->tun.txq_cap - qlen;
    const uint64_t need = (uint64_t)t->fpp * t->spf * US_PER_S;
    const uint8_t out_ch = hp_sbc_channels(&t->cfg);
    uint32_t n = 0;
    while (n < t->tun.max_pkt_per_tick && t->credit >= need && room > 0u) {
        /* rev 3: check and allocate before touching PCM, so a refusal never strands audio */
        if (t->ops.can_send && !t->ops.can_send(t->ops.ctx)) {
            t->st.can_send_no++;
            break;
        }
        uint8_t *data = NULL;
        void *buf = t->ops.alloc(t->ops.ctx, t->fpp * t->frame_len_max, &data);
        if (buf == NULL) {
            t->st.alloc_fail++;   /* nothing was pulled from the ring: no audio lost, the next tick retries */
            break;
        }
        const uint8_t bp = hp_tx_packet_bitpool(t);
        uint32_t len = 0, real_src = 0, epoch = t->sink->c_seen_gen;
        for (uint32_t f = 0; f < t->fpp; f++) {
            uint32_t used = 0;
            uint32_t r = hp_sink_pull(t->sink, hp_sbc_pcm_buf(), t->spf, out_ch, &used);
            if (t->sink->c_seen_gen != epoch) {   /* a flush landed inside this packet: only the new part counts */
                epoch = t->sink->c_seen_gen;
                real_src = 0u;
            }
            real_src += used;
            t->st.real_pcm += r;
            t->st.pad_pcm += t->spf - r;
            len += hp_sbc_encode(bp, data + len);
        }
        int rc = t->ops.send(t->ops.ctx, buf, len, (uint16_t)t->fpp, t->ts);
        if (rc != HP_SEND_OK) {
            /* should not happen after can_send (heap node allocation, handle mismatch): count it, keep its real frames
             * as unheard (a rewind replays them) and go on at the next tick; a refusal that lasts stall_ms is
             * handled as a stall below (rewind snapshot, event): on an already STARTED stream no further STARTED
             * would come to resume a paused tick (audit r3 item 2) */
            t->ops.free(t->ops.ctx, buf);
            t->st.tx_lost_sbc += t->fpp;
            lose(t, epoch, real_src, epoch);
            if (!t->send_failing) {
                t->send_failing = 1u;
                t->send_fail_us = now;
            }
            break;
        }
        t->send_failing = 0u;
        t->ts += t->fpp * t->spf;
        t->credit -= need;
        t->sent_sbc += t->fpp;
        rec_push(t, real_src, t->fpp, epoch);
        t->st.pkts++;
        t->st.frames_sbc += t->fpp;
        if (real_src == 0u) {
            t->st.pad_pkts++;
        } else if (HP_LD(&t->pub_frozen)) {
            freeze(t, 0u, 0u, 0u);   /* playing again after a stop: the live position takes over (live log) */
        }
        n++;
        room--;
    }
    if (n > t->st.max_burst) {
        t->st.max_burst = n;
    }
    cur = t->sink->c_seen_gen;

    /* Credit-stall watchdog (§4.6): stop (rewind snapshot) and tell the session. */
    if (hp_tx_down_sbc(t) == 0u) {
        t->last_ack_us = now;
    }
    if ((hp_tx_down_sbc(t) != 0u && now - t->last_ack_us > (int64_t)t->tun.stall_ms * 1000) ||
        (t->send_failing && now - t->send_fail_us > (int64_t)t->tun.stall_ms * 1000)) {
        t->st.stalls++;
        hp_tx_stop(t, HP_STOP_LINK);
        t->stalled = 1u;
        if (t->ops.event) {
            t->ops.event(t->ops.ctx, HP_TX_EVT_STALL);
        }
        return;
    }
    publish(t, now, cur);
}

/* ------------------------------------------------------------------------------------------------ app task */

void hp_tx_request_flush(hp_tx_t *t)
{
    hp_sink_flush(t->sink);
    HP_STR(&t->flush_req, HP_LD(&t->flush_req) + 1u);   /* single writer (app task) */
}

void hp_tx_request_drop(hp_tx_t *t)
{
    hp_sink_flush(t->sink);
    HP_STR(&t->drop_req, HP_LD(&t->drop_req) + 1u);
}

typedef struct {
    uint32_t epoch, inflight, lost, lat, last_real_ack, frozen, frozen_epoch, rewind;
} pub_t;

static void read_pub(const hp_tx_t *t, pub_t *p)
{
    for (;;) {
        uint32_t s0 = HP_LDA(&t->pub_seq);
        p->epoch = HP_LD(&t->pub_epoch);
        p->inflight = HP_LD(&t->pub_inflight_src);
        p->lost = HP_LD(&t->pub_lost_src);
        p->lat = HP_LD(&t->pub_lat_src);
        p->last_real_ack = HP_LD(&t->pub_last_real_ack_us);
        p->frozen = HP_LD(&t->pub_frozen);
        p->frozen_epoch = HP_LD(&t->pub_frozen_epoch);
        p->rewind = HP_LD(&t->pub_rewind_src);
        HP_FENCE();
        if (!(s0 & 1u) && s0 == HP_LD(&t->pub_seq)) {
            return;
        }
    }
}

uint32_t hp_tx_app_queued_frames(const hp_tx_t *t)
{
    pub_t p;
    read_pub(t, &p);
    uint32_t gen = HP_LDA(&t->sink->flush_gen);
    if (p.frozen && p.frozen_epoch == gen) {
        return p.rewind;
    }
    uint32_t q = hp_sink_queued(t->sink);
    if (p.epoch == gen) {   /* values of an older epoch belong to audio the player already flushed */
        q += p.inflight + p.lost + p.lat;
    }
    return q;
}

int hp_tx_app_is_busy(const hp_tx_t *t)
{
    pub_t p;
    read_pub(t, &p);
    if (hp_sink_queued(t->sink) > 0u) {
        return 1;
    }
    if (p.epoch != HP_LDA(&t->sink->flush_gen)) {
        return 0;
    }
    if (p.inflight > 0u) {
        return 1;
    }
    if (p.last_real_ack == 0u) {
        return 0;
    }
    uint32_t now = (uint32_t)t->ops.now_us(t->ops.ctx);
    return now - p.last_real_ack < hp_tx_lat_ms(t) * 1000u ? 1 : 0;
}
