/* Force-included in every Bluedroid translation unit (stands in for the headers ESP-IDF pulls in transitively). SPDX-License-Identifier: MIT */
#pragma once
#include "sdkconfig.h"
#include "esp_attr.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif
#define CONFIG_FREERTOS_NUMBER_OF_CORES 1

/* ---- open_fw footprint switches (see third_party/bluedroid_patches/README.md) ---- */
#define OPENFW_A2DP_SOURCE_ONLY 1          /* patch 0001: no A2DP sink / SBC decoder */
#define BTC_MEDIA_AA_BUF_SIZE   1280       /* patch 0002: encoded frame buffer; upstream 4112. >= BT_HDR + offset + MTU(1008) */
#define BTA_JV_SDP_DB_SIZE      1500       /* upstream 4500: only used when WE discover a peer's SPP record (client role) */
#define BTA_JV_SDP_RAW_DATA_SIZE 600       /* upstream 1800 */
/* Controller->host ACL window (plan M1 "ACL credit window"): announced via Host Buffer Size (0C33) and enforced by
 * bt_ctrl, whose ACL RX ring holds exactly this many packets (static-asserted there). Upstream: 100 x 1691 B. */
#define L2CAP_HOST_FC_ACL_BUFS      4      /* upstream #ifndef override in bt_target.h */
#define L2CAP_HOST_FC_ACL_DATA_LEN  1021   /* patch 0004; = RW-BT controller ACL data length (DH5) */

/* Headphones mode, external-codec A2DP source (docs/BT_A2DP_DESIGN.md §4-5). With CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
 * bt_target.h no longer defines SBC_ENC_INCLUDED and falls back to FALSE, which compiles the encoder empty; the app
 * encodes SBC itself (bt_port/hp_sbc.c). The default rate makes bta_av_build_src_cfg prefer 48 kHz (SEID 0 offers
 * 44.1|48 kHz, SEID 1 44.1 kHz only). The ext-codec queue cap is overridable by patch 0034: "length > 3" admits a
 * 4th packet, so audio_tx_q holds at most 4 (= HP_TXQ_PKT_CAP, bt_port/hp_tx.h). */
#define SBC_ENC_INCLUDED                   TRUE
#define BTC_AV_SBC_DEFAULT_SAMP_FREQ       A2D_SBC_IE_SAMP_FREQ_48
#define MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ 3

/* btc_spp.c passes size_t* to esp_ringbuf's UBaseType_t* parameters (warning silenced for that file only). */
#include "FreeRTOS.h"
_Static_assert(sizeof(size_t) == sizeof(UBaseType_t), "btc_spp.c size_t*/UBaseType_t* aliasing needs equal sizes");
