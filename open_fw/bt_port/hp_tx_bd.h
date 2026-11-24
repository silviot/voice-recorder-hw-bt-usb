/* Binds the headphones transmit path (hp_tx.c) to Bluedroid's external-codec A2DP source: esp_a2d_audio_buff_alloc /
 * esp_a2d_source_audio_data_send, patch 0034's queue counters, an osi_alarm tick in BTC context, bt_ctrl's NOCP
 * observer (docs/BT_A2DP_DESIGN.md §4.3-4.6, §5.4-5.5). The headphones session (bt_hp, milestone H5) drives it from
 * its A2DP callback, which Bluedroid runs in BTC context, so start/stop/cfg are serialised with the tick.
 * SPDX-License-Identifier: MIT */
#ifndef HP_TX_BD_H
#define HP_TX_BD_H
#include <stdint.h>
#include "hp_tx.h"

/* After esp_a2d_source_init (BTC or app task, before any connect): binds ops, creates the tick alarm. 0 = ok. */
int      hp_tx_bd_init(hp_sink_t *sink, const hp_tx_tun_t *tun, void (*on_event)(int evt));
/* App task, teardown (§5.5 step 1, rev 3 R-10): ask the tick to stop itself in BTC context (cancel + rewind
 * snapshot + rebase) and wait up to timeout_ms for it, sleeping with sleep_ms(1). 0 = stopped; -1 = bound overrun
 * (a teardown fault, never a spin). */
int      hp_tx_bd_stop_wait(uint32_t timeout_ms, void (*sleep_ms)(uint32_t));
/* Before esp_a2d_source_deinit, after hp_tx_bd_stopped or hp_tx_bd_stop_wait: frees the tick alarm. */
void     hp_tx_bd_deinit(void);
/* ESP_A2D_CONNECTION_STATE_EVT (CONNECTED): stream handle and audio MTU. */
void     hp_tx_bd_set_conn(uint16_t conn_hdl, uint16_t audio_mtu);
/* ACL handle of the headset link, for NOCP accounting (0xFFFF = none). */
void     hp_tx_bd_set_acl_handle(uint16_t acl_handle);
/* ESP_A2D_AUDIO_CFG_EVT: the SBC codec information element (4 bytes). 0 = ok. */
int      hp_tx_bd_on_audio_cfg(const uint8_t sbc_cie[4]);
/* A STARTED bt_hp asked for (ack of its START, §4.5): start pacing. BTC context. 0 = ok. */
int      hp_tx_bd_started(void);
/* SUSPEND (local or remote), CLOSE (HP_STOP_SUSPEND), link loss (HP_STOP_LINK): stop pacing, rewind snapshot.
 * BTC context (bt_hp's A2DP callback). */
void     hp_tx_bd_stopped(int reason);
/* ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT (1/10 ms). */
void     hp_tx_bd_delay_report(uint16_t delay_0p1ms);
hp_tx_t *hp_tx_bd(void);
#endif
