/*
 * Smartlink SL6801 OpenFW Clean-Room Classic BR/EDR L2CAP + SDP + RFCOMM Engine
 *
 * Allocation-free server implementing:
 *   - L2CAP Signaling (CID 0x0001) with ACL packet reassembly
 *   - Classic SDP (PSM 0x0001) advertising SPP UUID 0x1101 on RFCOMM Channel 1
 *   - RFCOMM Multiplexer (PSM 0x0003, DLCI 0 + DLCI 2) with credit-based flow
 *
 * Designed to plug cleanly above BootROM RW-BT HCI/ACL transport and below the
 * +XREC v5 protocol handler, and 100% testable on host.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_BT_SPP_H
#define OPENFW_BT_SPP_H

#include <stdint.h>

#define BT_SPP_TX_Q_CAP   8u
#define BT_SPP_TX_MAX_PDU 680u
#define BT_SPP_RX_MAX_PDU 700u

typedef void (*bt_spp_data_cb_t)(void *user, const uint8_t *data, uint16_t len);
typedef void (*bt_spp_disc_cb_t)(void *user);

typedef struct {
    uint16_t remote_cid;
    uint16_t local_cid;
    uint16_t psm;
    uint8_t  open;
    uint8_t  cfg_rx_done;
    uint8_t  cfg_tx_done;
} bt_l2cap_ch_t;

typedef struct {
    uint16_t conhdl;
    uint8_t  next_sig_id;

    /* L2CAP RX reassembly buffer */
    uint16_t rx_expected;
    uint16_t rx_cid;
    uint16_t rx_pos;
    uint8_t  rx_buf[BT_SPP_RX_MAX_PDU];

    /* Channels: SDP (PSM 0x0001 -> CID 0x0040), RFCOMM (PSM 0x0003 -> CID 0x0041) */
    bt_l2cap_ch_t sdp;
    bt_l2cap_ch_t rfcomm;

    /* RFCOMM DLCI 0 & DLCI 2 (Channel 1) state */
    uint8_t  mux_open;
    uint8_t  ch1_open;
    uint8_t  ch1_msc_sent;
    uint8_t  credit_flow;
    uint16_t rfcomm_mtu;
    uint8_t  peer_credits;
    uint8_t  local_credits;

    /* TX ACL queue */
    uint8_t  tx_head;
    uint8_t  tx_tail;
    uint8_t  tx_count;
    uint16_t tx_lens[BT_SPP_TX_Q_CAP];
    uint8_t  tx_pdus[BT_SPP_TX_Q_CAP][BT_SPP_TX_MAX_PDU];

    bt_spp_data_cb_t on_data;
    void            *on_data_user;
    bt_spp_disc_cb_t on_disc;
    void            *on_disc_user;
} bt_spp_t;

void     bt_spp_init(bt_spp_t *spp, bt_spp_data_cb_t on_data, void *user);
void     bt_spp_set_disconnect_cb(bt_spp_t *spp, bt_spp_disc_cb_t cb, void *user);
void     bt_spp_reset_link(bt_spp_t *spp, uint16_t conhdl);
void     bt_spp_on_acl_rx(bt_spp_t *spp, uint16_t conhdl, uint8_t pb_flag,
                          const uint8_t *payload, uint16_t len);
int      bt_spp_is_open(const bt_spp_t *spp);
int      bt_spp_send(bt_spp_t *spp, const uint8_t *data, uint16_t len);
uint16_t bt_spp_pop_acl_tx(bt_spp_t *spp, uint16_t *out_conhdl,
                           uint8_t *out_pdu, uint16_t max_len);

#endif /* OPENFW_BT_SPP_H */
