/* hp_tx on Bluedroid's external-codec A2DP source (see hp_tx_bd.h). Built like vendored code (it includes BTC
 * internals for Bluedroid patch 0034's accessors). SPDX-License-Identifier: MIT */
#include "hp_tx_bd.h"
#include <string.h>
#include "esp_a2dp_api.h"
#include "esp_timer.h"
#include "osi/alarm.h"
#include "btc_a2dp_source.h"
#include "btc_av.h"
#include "bt_ctrl.h"
#include "stack/hcimsgs.h"

#if !((BTC_AV_SRC_INCLUDED == TRUE) && (BTC_AV_EXT_CODEC == TRUE))
#error "hp_tx_bd.c needs the external-codec A2DP source (CONFIG_BT_A2DP_USE_EXTERNAL_CODEC)"
#endif

static struct {
    hp_tx_t      tx;
    osi_alarm_t *alarm;
    void       (*on_event)(int evt);
    uint32_t     stop_req;      /* app -> BTC (HP_LD/HP_ST): stop at the next tick */
    uint32_t     tick_stopped;  /* BTC -> app: the alarm is cancelled and no tick can run */
    uint32_t     acl_handle;    /* NOCP filter (bt_ctrl task reads) */
    uint16_t     conn_hdl;
    uint16_t     audio_mtu;
    uint8_t      inited;
} s_bd;

static int64_t bd_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static int bd_can_send(void *ctx)
{
    (void)ctx;
    return btc_av_is_started() && btc_a2dp_source_ext_can_send();
}

static void *bd_alloc(void *ctx, uint32_t len, uint8_t **data)
{
    (void)ctx;
    esp_a2d_audio_buff_t *b = len <= 0xFFFFu ? esp_a2d_audio_buff_alloc((uint16_t)len) : NULL;
    if (b == NULL) {
        return NULL;
    }
    *data = b->data;
    return b;
}

static void bd_free(void *ctx, void *buf)
{
    (void)ctx;
    esp_a2d_audio_buff_free((esp_a2d_audio_buff_t *)buf);
}

static int bd_send(void *ctx, void *buf, uint32_t len, uint16_t nframes, uint32_t ts)
{
    (void)ctx;
    esp_a2d_audio_buff_t *b = (esp_a2d_audio_buff_t *)buf;
    b->number_frame = nframes;
    b->timestamp = ts;
    b->data_len = (uint16_t)len;
    /* On a refusal btc_a2d_src_audio_data_send restores the fields and the buffer is still ours (btc_av.c). */
    return esp_a2d_source_audio_data_send(s_bd.conn_hdl, b) == ESP_OK ? HP_SEND_OK : HP_SEND_FAIL;
}

static void bd_txq_stat(void *ctx, hp_txq_stat_t *st)
{
    (void)ctx;
    btc_a2dp_source_ext_txq_stat_t s;
    btc_a2dp_source_ext_txq_stat(&s);
    st->enq_pkts = s.enq_pkts;
    st->enq_sbc = s.enq_sbc;
    st->deq_pkts = s.deq_pkts;
    st->deq_sbc = s.deq_sbc;
    st->flushed_pkts = s.flushed_pkts;
    st->flushed_sbc = s.flushed_sbc;
    st->qlen = s.qlen;
}

static uint32_t bd_txq_drop(void *ctx)
{
    (void)ctx;
    return btc_a2dp_source_ext_txq_drop();
}

static void bd_stop_tick(void)
{
    /* BTC context: cancel and delivery run on the same task, so no tick can be mid-run after this (rev 3 R-10) */
    osi_alarm_cancel(s_bd.alarm);
    HP_STR(&s_bd.tick_stopped, 1u);
}

static void bd_event(void *ctx, int evt)
{
    (void)ctx;
    if (evt == HP_TX_EVT_STALL) {
        bd_stop_tick();   /* hp_tx already took the rewind snapshot and rebased */
    }
    if (s_bd.on_event) {
        s_bd.on_event(evt);
    }
}

static void bd_nocp(uint16_t handle, uint32_t pkts)
{
    if (handle == HP_LD(&s_bd.acl_handle)) {
        hp_tx_on_nocp(&s_bd.tx, pkts);
    }
}

/* osi_alarm callback: BTC context (bt_esp_timer shim, patches 0005/0006). */
static void bd_tick(void *arg)
{
    (void)arg;
    if (HP_LDA(&s_bd.stop_req)) {
        HP_ST(&s_bd.stop_req, 0u);
        bd_stop_tick();
        hp_tx_stop(&s_bd.tx, HP_STOP_SUSPEND);
        return;
    }
    hp_tx_tick(&s_bd.tx);
}

int hp_tx_bd_init(hp_sink_t *sink, const hp_tx_tun_t *tun, void (*on_event)(int evt))
{
    if (s_bd.inited) {
        return 0;
    }
    static const hp_tx_ops_t ops = {
        .now_us = bd_now_us, .can_send = bd_can_send, .alloc = bd_alloc, .free = bd_free, .send = bd_send,
        .txq_stat = bd_txq_stat, .txq_drop = bd_txq_drop, .event = bd_event, .ctx = NULL,
    };
    memset(&s_bd, 0, sizeof(s_bd));
    s_bd.acl_handle = 0xFFFFu;
    s_bd.conn_hdl = ESP_A2D_INVALID_CONN_HDL;
    s_bd.on_event = on_event;
    s_bd.tick_stopped = 1u;
    hp_tx_init(&s_bd.tx, &ops, sink, tun);
    s_bd.alarm = osi_alarm_new("hp_tx", bd_tick, NULL, 0);
    if (s_bd.alarm == NULL) {
        return -1;
    }
    bt_ctrl_set_nocp_observer(bd_nocp);
    s_bd.inited = 1u;
    return 0;
}

int hp_tx_bd_stop_wait(uint32_t timeout_ms, void (*sleep_ms)(uint32_t))
{
    if (!s_bd.inited || HP_LDA(&s_bd.tick_stopped)) {
        return 0;
    }
    HP_STR(&s_bd.stop_req, 1u);
    for (uint32_t waited = 0; waited < timeout_ms; waited++) {
        if (HP_LDA(&s_bd.tick_stopped)) {
            return 0;
        }
        sleep_ms(1);
    }
    return HP_LDA(&s_bd.tick_stopped) ? 0 : -1;
}

void hp_tx_bd_deinit(void)
{
    if (!s_bd.inited) {
        return;
    }
    bt_ctrl_set_nocp_observer(NULL);
    osi_alarm_free(s_bd.alarm);   /* after hp_tx_bd_stopped (BTC) or hp_tx_bd_stop_wait (app): no tick in flight */
    s_bd.alarm = NULL;
    s_bd.inited = 0u;
}

void hp_tx_bd_set_conn(uint16_t conn_hdl, uint16_t audio_mtu)
{
    s_bd.conn_hdl = conn_hdl;
    s_bd.audio_mtu = audio_mtu;
}

void hp_tx_bd_set_acl_handle(uint16_t acl_handle)
{
    HP_ST(&s_bd.acl_handle, (uint32_t)acl_handle);
}

int hp_tx_bd_on_audio_cfg(const uint8_t sbc_cie[4])
{
    hp_sbc_cfg_t cfg;
    if (hp_sbc_cfg_from_cie(sbc_cie, &cfg) != 0) {
        return -1;
    }
    return hp_tx_set_cfg(&s_bd.tx, &cfg, s_bd.audio_mtu);
}

int hp_tx_bd_started(void)
{
    if (!s_bd.inited) {
        return -1;
    }
    uint32_t tick_ms = hp_tx_start(&s_bd.tx);
    if (tick_ms == 0u) {
        return -1;
    }
    HP_ST(&s_bd.stop_req, 0u);
    HP_ST(&s_bd.tick_stopped, 0u);
    return osi_alarm_set_periodic(s_bd.alarm, tick_ms) == OSI_ALARM_ERR_PASS ? 0 : -1;
}

void hp_tx_bd_stopped(int reason)
{
    if (!s_bd.inited) {
        return;
    }
    bd_stop_tick();
    hp_tx_stop(&s_bd.tx, reason);
}

void hp_tx_bd_delay_report(uint16_t delay_0p1ms)
{
    hp_tx_set_delay_report(&s_bd.tx, delay_0p1ms);
}

hp_tx_t *hp_tx_bd(void)
{
    return &s_bd.tx;
}

void hp_tx_bd_close_acl(uint16_t acl_handle)
{
    /* design §4.6 (rev 3 R-4): 0406 through Bluedroid's HCI layer (hci_layer transmit_command: a locked queue drained by
     * hciT, the one producer bt_ctrl allows), never straight to bt_ctrl. An HCI command is not held back by the zero
     * ACL credits of a stalled link, where an AVDTP/L2CAP close would wait behind the stuck data. BTA_DmCloseACL (the
     * design's choice) is declared by this Bluedroid but not implemented (live log). BTM learns about the link end
     * from the Disconnection Complete as usual. */
    (void)btsnd_hcic_disconnect(acl_handle, HCI_ERR_PEER_USER);
}
