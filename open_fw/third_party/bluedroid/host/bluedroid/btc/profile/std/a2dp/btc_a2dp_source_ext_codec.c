/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common/bt_target.h"
#include "common/bt_trace.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "osi/allocator.h"
#include "osi/fixed_queue.h"
#include "stack/a2d_api.h"
#include "bta/bta_av_api.h"
#include "bta/bta_av_ci.h"
#include "btc/btc_manage.h"
#include "btc/btc_common.h"
#include "btc_av_co.h"
#include "btc_a2dp.h"
#include "btc_a2dp_control.h"
#include "btc_a2dp_source.h"
#include "btc_av.h"
#include "esp_a2dp_api.h"
#include <assert.h>

#if (BTC_AV_SRC_INCLUDED == TRUE) && (BTC_AV_EXT_CODEC == TRUE)

#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
/* open_fw patch 0034: the app paces the stream itself (bt_port/hp_tx.c) and keeps audio_tx_q short (packets wait in
 * our PCM ring, where a seek can still drop them); bt_port_prefix.h sets 3, so "length > 3" admits at most 4. */
#ifndef MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ
#define MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ     (27)
#endif
/* Queue accounting for the app's position and stall math (docs/BT_A2DP_DESIGN.md §4.4). Cumulative, wrap-safe;
 * frame counts are SBC frames from each buffer's number_frame (layer_specific once enqueued). One writer per
 * counter: enq_*, flushed_* and rej_pkts in BTC (send, tx flush, ext_txq_drop), deq_* in BTU (readbuf). 32-bit
 * aligned stores are single-copy atomic on Cortex-M4. */
static struct {
    volatile uint32_t enq_pkts, enq_sbc, deq_pkts, deq_sbc, flushed_pkts, flushed_sbc, rej_pkts;
} s_openfw_xq;
#else
#define MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ     (27)
#endif

enum {
    BTC_A2DP_SOURCE_STATE_OFF = 0,
    BTC_A2DP_SOURCE_STATE_ON = 1,
};

typedef struct {
    BOOLEAN                 stream_started;
    BOOLEAN                 tx_flush;
    fixed_queue_t          *audio_tx_q;
} a2dp_source_local_param_t;

static int btc_a2dp_source_state = BTC_A2DP_SOURCE_STATE_OFF;
#if A2D_DYNAMIC_MEMORY == FALSE
static a2dp_source_local_param_t a2dp_source_local_param;
#else
static a2dp_source_local_param_t *a2dp_source_local_param_ptr;
#define a2dp_source_local_param (*a2dp_source_local_param_ptr)
#endif ///A2D_DYNAMIC_MEMORY == FALSE

static inline void btc_aa_cb_to_app(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    esp_a2d_cb_t btc_aa_cb = (esp_a2d_cb_t)btc_profile_cb_get(BTC_PID_A2DP);
    if (btc_aa_cb) {
        btc_aa_cb(event, param);
    }
}

static void btc_a2dp_source_tx_flush(void)
{
    while (!fixed_queue_is_empty(a2dp_source_local_param.audio_tx_q)) {
#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
        BT_HDR *p_buf = (BT_HDR *)fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0);
        if (p_buf == NULL) {
            break;   /* BTU's readbuf took the last one between the two calls */
        }
        s_openfw_xq.flushed_pkts++;
        s_openfw_xq.flushed_sbc += p_buf->layer_specific;
        osi_free(p_buf);
#else
        osi_free(fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0));
#endif
    }
}

static void btc_a2dp_source_tx_stop(void)
{
    if (a2dp_source_local_param.stream_started == TRUE) {
        a2dp_source_local_param.stream_started = FALSE;
        /* ack to command */
        btc_a2dp_control_command_ack(ESP_A2D_MEDIA_CTRL_ACK_SUCCESS);
    }

    /* flush tx queue  */
    btc_a2dp_source_tx_flush();
    a2dp_source_local_param.tx_flush = FALSE;
}

BOOLEAN btc_a2dp_source_enqueue_audio_frame(BT_HDR *p_buf)
{
    if (btc_a2dp_source_state != BTC_A2DP_SOURCE_STATE_ON) {
        APPL_TRACE_WARNING("%s source not start up", __func__);
        return FALSE;
    }

    if (a2dp_source_local_param.tx_flush) {
        APPL_TRACE_WARNING("%s try to send data when tx flush enable", __func__);
        return FALSE;
    }

    if (fixed_queue_length(a2dp_source_local_param.audio_tx_q) > MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ) {
        APPL_TRACE_WARNING("%s audio tx queue overflow: %d", __func__, fixed_queue_length(a2dp_source_local_param.audio_tx_q));
#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
        s_openfw_xq.rej_pkts++;
#endif
        return FALSE;
    }

#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
    /* Counted before the enqueue: BTU may dequeue (and count deq) as soon as the buffer is in the queue. The queue
     * is unbounded (QUEUE_SIZE_MAX), so with an infinite timeout the enqueue fails only on a node allocation
     * failure (patch 0009 restores the slot); the caller keeps the buffer then (ESP_FAIL), so undo the count. */
    uint16_t openfw_nf = p_buf->layer_specific;
    s_openfw_xq.enq_pkts++;
    s_openfw_xq.enq_sbc += openfw_nf;
    if (!fixed_queue_enqueue(a2dp_source_local_param.audio_tx_q, p_buf, FIXED_QUEUE_MAX_TIMEOUT)) {
        s_openfw_xq.enq_pkts--;
        s_openfw_xq.enq_sbc -= openfw_nf;
        s_openfw_xq.rej_pkts++;
        return FALSE;
    }
#else
    fixed_queue_enqueue(a2dp_source_local_param.audio_tx_q, p_buf, FIXED_QUEUE_MAX_TIMEOUT);
#endif
    bta_av_ci_src_data_ready(BTA_AV_CHNL_AUDIO);
    return TRUE;
}

void btc_source_report_delay_value(UINT16 delay_value)
{
    esp_a2d_cb_param_t param;

    if (btc_a2dp_source_state != BTC_A2DP_SOURCE_STATE_ON){
        return;
    }

    param.a2d_report_delay_value_stat.delay_value = delay_value;

    btc_aa_cb_to_app(ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT, &param);
}

BOOLEAN btc_a2dp_source_start_audio_req(void)
{
    a2dp_source_local_param.stream_started = TRUE;
    return TRUE;
}

BT_HDR *btc_a2dp_source_audio_readbuf(void)
{
    if (btc_a2dp_source_state != BTC_A2DP_SOURCE_STATE_ON || a2dp_source_local_param.tx_flush){
        return NULL;
    }
#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
    BT_HDR *p_buf = (BT_HDR *)fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0);
    if (p_buf != NULL) {
        s_openfw_xq.deq_pkts++;
        s_openfw_xq.deq_sbc += p_buf->layer_specific;
    }
    return p_buf;
#else
    return fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0);
#endif
}

void btc_a2dp_source_set_tx_flush(BOOLEAN enable)
{
    a2dp_source_local_param.tx_flush = enable;
    if (enable) {
        btc_a2dp_source_tx_flush();
    }
}

void btc_a2dp_source_on_suspended(tBTA_AV_SUSPEND *p_av)
{
    /* check for status failures */
    if (p_av == NULL) {
        return;
    }

    if (p_av->status != BTA_AV_SUCCESS) {
        if (p_av->initiator == TRUE) {
            btc_a2dp_control_command_ack(ESP_A2D_MEDIA_CTRL_ACK_FAILURE);
        }
        return;
    }

    /* stop tx, ack to cmd, flush tx queue */
    btc_a2dp_source_tx_stop();
}

void btc_a2dp_source_on_stopped(tBTA_AV_SUSPEND *p_av)
{
    /* allow using this api for other than suspend */
    if (p_av != NULL) {
        if (p_av->status != BTA_AV_SUCCESS) {
            if (p_av->initiator) {
                btc_a2dp_control_command_ack(ESP_A2D_MEDIA_CTRL_ACK_FAILURE);
            }
            return;
        }
    }

    /* stop tx, ack to cmd, flush tx queue */
    btc_a2dp_source_tx_stop();
}

void btc_a2dp_source_on_idle(void)
{
    /* stop tx, ack to cmd, flush tx queue */
    btc_a2dp_source_tx_stop();
}

bool btc_a2dp_source_is_task_shutting_down(void)
{
    /* always return false, remove this api when internal codec is remove */
    return false;
}

bool btc_a2dp_source_startup(void)
{
    if (btc_a2dp_source_state != BTC_A2DP_SOURCE_STATE_OFF) {
        APPL_TRACE_ERROR("%s A2DP source already start up", __func__);
        return false;
    }

#if A2D_DYNAMIC_MEMORY == TRUE
    if ((a2dp_source_local_param_ptr = (a2dp_source_local_param_t *)osi_malloc(sizeof(a2dp_source_local_param_t))) == NULL) {
        APPL_TRACE_ERROR("%s malloc failed!", __func__);
        return false;
    }
    memset((void *)a2dp_source_local_param_ptr, 0, sizeof(a2dp_source_local_param_t));
#endif

    btc_a2dp_source_state = BTC_A2DP_SOURCE_STATE_ON;

    a2dp_source_local_param.audio_tx_q = fixed_queue_new(QUEUE_SIZE_MAX);
    if(a2dp_source_local_param.audio_tx_q == NULL) {
        goto error_exit;
    }

    btc_a2dp_control_init();

    return true;

error_exit:;
    APPL_TRACE_ERROR("%s A2DP source start up failed", __func__);

    btc_a2dp_source_state = BTC_A2DP_SOURCE_STATE_OFF;

#if A2D_DYNAMIC_MEMORY == TRUE
    osi_free(a2dp_source_local_param_ptr);
    a2dp_source_local_param_ptr = NULL;
#endif

    return false;
}

void btc_a2dp_source_shutdown(void)
{
    if (btc_a2dp_source_state != BTC_A2DP_SOURCE_STATE_ON) {
        APPL_TRACE_ERROR("%s A2DP source already shutdown", __func__);
        return;
    }
    btc_a2dp_source_state = BTC_A2DP_SOURCE_STATE_OFF;

    btc_a2dp_control_cleanup();

    fixed_queue_free(a2dp_source_local_param.audio_tx_q, osi_free_func);

    a2dp_source_local_param.audio_tx_q = NULL;
    a2dp_source_local_param.tx_flush = FALSE;
    a2dp_source_local_param.stream_started = FALSE;

#if A2D_DYNAMIC_MEMORY == TRUE
    osi_free(a2dp_source_local_param_ptr);
    a2dp_source_local_param_ptr = NULL;
#endif
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_source_set_pref_mcc
 **
 ** Description
 **
 ** Returns          TRUE if the preferred config is supported, FALSE otherwise
 **
 *******************************************************************************/
BOOLEAN btc_a2dp_source_set_pref_mcc(tBTA_AV_HNDL hndl, tBTC_AV_CODEC_INFO *pref_mcc)
{
    return bta_av_co_audio_set_pref_mcc(hndl, pref_mcc);
}

#if defined(OPENFW_BLUEDROID) && OPENFW_BLUEDROID
/* open_fw patch 0034 accessors (declared in btc_a2dp_source.h). */
void btc_a2dp_source_ext_txq_stat(btc_a2dp_source_ext_txq_stat_t *out)
{
    if (out == NULL) {
        return;
    }
    out->enq_pkts = s_openfw_xq.enq_pkts;
    out->enq_sbc = s_openfw_xq.enq_sbc;
    out->deq_pkts = s_openfw_xq.deq_pkts;
    out->deq_sbc = s_openfw_xq.deq_sbc;
    out->flushed_pkts = s_openfw_xq.flushed_pkts;
    out->flushed_sbc = s_openfw_xq.flushed_sbc;
    out->rej_pkts = s_openfw_xq.rej_pkts;
    out->qlen = (btc_a2dp_source_state == BTC_A2DP_SOURCE_STATE_ON && a2dp_source_local_param.audio_tx_q != NULL) ?
                (uint32_t)fixed_queue_length(a2dp_source_local_param.audio_tx_q) : 0u;
}

uint32_t btc_a2dp_source_ext_txq_drop(void)
{
    if (btc_a2dp_source_state != BTC_A2DP_SOURCE_STATE_ON || a2dp_source_local_param.audio_tx_q == NULL) {
        return 0;
    }
    uint32_t before = s_openfw_xq.flushed_sbc;
    btc_a2dp_source_tx_flush();
    return s_openfw_xq.flushed_sbc - before;
}

BOOLEAN btc_a2dp_source_ext_can_send(void)
{
    /* the checks btc_a2dp_source_enqueue_audio_frame makes, minus the queue cap (the app checks qlen itself) */
    return btc_a2dp_source_state == BTC_A2DP_SOURCE_STATE_ON && !a2dp_source_local_param.tx_flush &&
           a2dp_source_local_param.audio_tx_q != NULL;
}
#endif

#endif /* (BTC_AV_SRC_INCLUDED == TRUE) && (BTC_AV_EXT_CODEC == TRUE) */
