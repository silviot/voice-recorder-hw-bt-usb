/* Host test for Bluedroid patch 0034 (docs/BT_A2DP_DESIGN.md §5.2, §8.1, milestone H0 gate): the patched
 * external-codec source queue (btc_a2dp_source_ext_codec.c) linked against a small in-test fixed_queue and stubs for
 * its BTC/BTA neighbours. Checks the queue cap of 4 (prefix MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ = 3), every counter
 * (enqueue, readbuf, tx flush on suspend, ext_txq_drop), frame counts taken from each buffer's number_frame, the
 * drop's return value, can_send, and that nothing is enqueued while flushing or before start-up.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/bt_target.h"
#include "osi/allocator.h"
#include "osi/fixed_queue.h"
#include "btc_a2dp_source.h"
#include "btc/btc_manage.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- in-test fixed_queue (single-threaded) ---- */
struct fixed_queue_t { void *v[64]; size_t h, n; };
fixed_queue_t *fixed_queue_new(size_t capacity) { (void)capacity; return calloc(1, sizeof(fixed_queue_t)); }
void fixed_queue_free(fixed_queue_t *q, fixed_queue_free_cb cb)
{
    while (q->n) { if (cb) cb(q->v[q->h]); q->h = (q->h + 1) % 64; q->n--; }
    free(q);
}
bool fixed_queue_is_empty(fixed_queue_t *q) { return q->n == 0; }
size_t fixed_queue_length(fixed_queue_t *q) { return q ? q->n : 0; }
bool fixed_queue_enqueue(fixed_queue_t *q, void *d, uint32_t t) { (void)t; if (q->n == 64) return false; q->v[(q->h + q->n++) % 64] = d; return true; }
void *fixed_queue_dequeue(fixed_queue_t *q, uint32_t t)
{
    (void)t;
    if (!q->n) return NULL;
    void *d = q->v[q->h]; q->h = (q->h + 1) % 64; q->n--;
    return d;
}
void osi_free_func(void *p) { free(p); }

/* ---- neighbours ---- */
static int g_ready, g_acks;
void bta_av_ci_src_data_ready(tBTA_AV_CHNL chnl) { (void)chnl; g_ready++; }
void *btc_profile_cb_get(btc_pid_t pid) { (void)pid; return NULL; }
bool btc_a2dp_control_init(void) { return true; }
void btc_a2dp_control_cleanup(void) {}
void btc_a2dp_control_command_ack(int status) { (void)status; g_acks++; }
BOOLEAN bta_av_co_audio_set_pref_mcc(tBTA_AV_HNDL h, tBTC_AV_CODEC_INFO *m) { (void)h; (void)m; return TRUE; }

static BT_HDR *pkt(uint16_t nframes)
{
    BT_HDR *b = calloc(1, sizeof(BT_HDR) + 16);
    b->layer_specific = nframes;   /* btc_a2d_src_audio_data_send stores number_frame here */
    return b;
}

static btc_a2dp_source_ext_txq_stat_t st(void)
{
    btc_a2dp_source_ext_txq_stat_t s;
    btc_a2dp_source_ext_txq_stat(&s);
    return s;
}

int main(void)
{
    BT_HDR *p = pkt(10);
    CHECK(!btc_a2dp_source_ext_can_send(), "can_send before start-up");
    CHECK(!btc_a2dp_source_enqueue_audio_frame(p), "enqueue before start-up refused");
    CHECK(st().qlen == 0 && st().enq_pkts == 0, "nothing counted");
    CHECK(btc_a2dp_source_startup(), "startup");
    CHECK(btc_a2dp_source_ext_can_send(), "can_send after start-up");

    /* cap: 4 packets admitted, the 5th refused (the app keeps its buffer) */
    for (int i = 0; i < 4; i++) CHECK(btc_a2dp_source_enqueue_audio_frame(pkt(10)), "enqueue %d", i);
    CHECK(!btc_a2dp_source_enqueue_audio_frame(p), "5th refused (cap 4)");
    btc_a2dp_source_ext_txq_stat_t s = st();
    CHECK(s.qlen == 4 && s.enq_pkts == 4 && s.enq_sbc == 40 && s.rej_pkts == 1, "after cap: q %u enq %u/%u rej %u",
          s.qlen, s.enq_pkts, s.enq_sbc, s.rej_pkts);
    CHECK(g_ready == 4, "data_ready per enqueue (%d)", g_ready);

    /* BTU reads two */
    for (int i = 0; i < 2; i++) { BT_HDR *b = btc_a2dp_source_audio_readbuf(); CHECK(b && b->layer_specific == 10, "readbuf"); free(b); }
    s = st();
    CHECK(s.deq_pkts == 2 && s.deq_sbc == 20 && s.qlen == 2, "deq %u/%u q %u", s.deq_pkts, s.deq_sbc, s.qlen);

    /* different frame counts are summed from number_frame */
    CHECK(btc_a2dp_source_enqueue_audio_frame(pkt(7)), "enqueue 7");
    /* ext_txq_drop: returns the SBC frames dropped and counts them as flushed */
    uint32_t d = btc_a2dp_source_ext_txq_drop();
    s = st();
    CHECK(d == 27 && s.flushed_pkts == 3 && s.flushed_sbc == 27 && s.qlen == 0, "drop %u flushed %u/%u", d,
          s.flushed_pkts, s.flushed_sbc);
    CHECK(btc_a2dp_source_ext_txq_drop() == 0, "drop of an empty queue");

    /* local suspend: tx_flush set -> queue freed and counted; enqueue refused while flushing; readbuf NULL */
    CHECK(btc_a2dp_source_enqueue_audio_frame(pkt(11)), "enqueue 11");
    btc_a2dp_source_set_tx_flush(TRUE);
    s = st();
    CHECK(s.flushed_pkts == 4 && s.flushed_sbc == 38 && s.qlen == 0, "suspend flush counted: %u/%u", s.flushed_pkts, s.flushed_sbc);
    CHECK(!btc_a2dp_source_enqueue_audio_frame(p), "refused while flushing");
    CHECK(!btc_a2dp_source_ext_can_send(), "can_send false while flushing");
    CHECK(btc_a2dp_source_audio_readbuf() == NULL, "readbuf NULL while flushing");
    tBTA_AV_SUSPEND sus = { 0 };
    sus.status = BTA_AV_SUCCESS;
    btc_a2dp_source_on_suspended(&sus);   /* clears tx_flush */
    CHECK(btc_a2dp_source_ext_can_send(), "can_send after the suspend completes");
    CHECK(btc_a2dp_source_enqueue_audio_frame(pkt(10)), "enqueue after suspend completes");
    s = st();
    /* identity: enq = deq + flushed + queued */
    CHECK(s.enq_sbc == s.deq_sbc + s.flushed_sbc + 10 && s.enq_pkts == s.deq_pkts + s.flushed_pkts + s.qlen,
          "accounting identity");

    btc_a2dp_source_shutdown();
    CHECK(st().qlen == 0, "qlen 0 after shutdown");
    free(p);
    printf("test_xsrc_stats: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
