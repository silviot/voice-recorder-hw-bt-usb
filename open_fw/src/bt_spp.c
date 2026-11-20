/*
 * Smartlink SL6801 OpenFW Clean-Room Classic BR/EDR L2CAP + SDP + RFCOMM Engine
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_bt_spp.h"

#define L2CAP_CID_SIG    0x0001u
#define L2CAP_CID_SDP    0x0040u
#define L2CAP_CID_RFCOMM 0x0041u
#define L2CAP_PSM_SDP    0x0001u
#define L2CAP_PSM_RFCOMM 0x0003u
#define L2CAP_MTU_MAX    672u
#define RFCOMM_CH1_DLCI  2u

static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint16_t rd_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static inline void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static inline void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static inline void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

static uint8_t rfcomm_fcs(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0xFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint8_t)((crc >> 1) ^ 0xE0u) : (uint8_t)(crc >> 1);
        }
    }
    return (uint8_t)(crc ^ 0xFFu);
}

static int queue_l2cap_pdu(bt_spp_t *spp, uint16_t remote_cid, const uint8_t *payload, uint16_t len)
{
    if (!spp || spp->tx_count >= BT_SPP_TX_Q_CAP || (4u + len) > BT_SPP_TX_MAX_PDU) {
        return -1;
    }
    uint8_t *dst = spp->tx_pdus[spp->tx_tail];
    wr_le16(&dst[0], len);
    wr_le16(&dst[2], remote_cid);
    if (len > 0u && payload) {
        mem_copy(&dst[4], payload, len);
    }
    spp->tx_lens[spp->tx_tail] = (uint16_t)(4u + len);
    spp->tx_tail = (uint8_t)((spp->tx_tail + 1u) % BT_SPP_TX_Q_CAP);
    spp->tx_count++;
    return 0;
}

static void notify_rfcomm_closed(bt_spp_t *spp)
{
    if (spp && spp->ch1_open) {
        spp->ch1_open     = 0u;
        spp->ch1_msc_sent = 0u;
        if (spp->on_disc) {
            spp->on_disc(spp->on_disc_user);
        }
    }
}

static uint8_t next_sig_id(bt_spp_t *spp)
{
    uint8_t id = spp->next_sig_id++;
    if (spp->next_sig_id == 0u) {
        spp->next_sig_id = 1u;
    }
    return id;
}

static void send_l2cap_sig(bt_spp_t *spp, uint8_t code, uint8_t id, const uint8_t *body, uint16_t body_len)
{
    uint8_t pdu[32];
    if ((4u + body_len) > sizeof(pdu)) {
        return;
    }
    pdu[0] = code;
    pdu[1] = id;
    wr_le16(&pdu[2], body_len);
    if (body_len > 0u && body) {
        mem_copy(&pdu[4], body, body_len);
    }
    (void)queue_l2cap_pdu(spp, L2CAP_CID_SIG, pdu, (uint16_t)(4u + body_len));
}

static void send_l2cap_cfg_req(bt_spp_t *spp, uint16_t remote_cid)
{
    uint8_t opt[8];
    wr_le16(&opt[0], remote_cid);
    wr_le16(&opt[2], 0u);
    opt[4] = 0x01u; /* MTU option */
    opt[5] = 0x02u;
    wr_le16(&opt[6], L2CAP_MTU_MAX);
    send_l2cap_sig(spp, 0x04u, next_sig_id(spp), opt, 8u);
}

static void handle_l2cap_sig(bt_spp_t *spp, const uint8_t *buf, uint16_t len)
{
    uint16_t off = 0u;
    while ((off + 4u) <= len) {
        uint8_t  code = buf[off];
        uint8_t  id   = buf[off + 1u];
        uint16_t dlen = rd_le16(&buf[off + 2u]);
        off = (uint16_t)(off + 4u);
        if ((off + dlen) > len) {
            break;
        }
        const uint8_t *d = &buf[off];
        off = (uint16_t)(off + dlen);

        switch (code) {
        case 0x02u: /* Connection Request */
            if (dlen >= 4u) {
                uint16_t psm  = rd_le16(&d[0]);
                uint16_t scid = rd_le16(&d[2]);
                uint8_t  rsp[8];
                if (psm == L2CAP_PSM_SDP) {
                    spp->sdp.psm        = psm;
                    spp->sdp.remote_cid = scid;
                    spp->sdp.local_cid  = L2CAP_CID_SDP;
                    spp->sdp.open       = 1u;
                    wr_le16(&rsp[0], L2CAP_CID_SDP);
                    wr_le16(&rsp[2], scid);
                    wr_le16(&rsp[4], 0u);
                    wr_le16(&rsp[6], 0u);
                    send_l2cap_sig(spp, 0x03u, id, rsp, 8u);
                    send_l2cap_cfg_req(spp, scid);
                } else if (psm == L2CAP_PSM_RFCOMM) {
                    spp->rfcomm.psm        = psm;
                    spp->rfcomm.remote_cid = scid;
                    spp->rfcomm.local_cid  = L2CAP_CID_RFCOMM;
                    spp->rfcomm.open       = 1u;
                    wr_le16(&rsp[0], L2CAP_CID_RFCOMM);
                    wr_le16(&rsp[2], scid);
                    wr_le16(&rsp[4], 0u);
                    wr_le16(&rsp[6], 0u);
                    send_l2cap_sig(spp, 0x03u, id, rsp, 8u);
                    send_l2cap_cfg_req(spp, scid);
                } else {
                    wr_le16(&rsp[0], 0u);
                    wr_le16(&rsp[2], scid);
                    wr_le16(&rsp[4], 0x0002u); /* PSM not supported */
                    wr_le16(&rsp[6], 0u);
                    send_l2cap_sig(spp, 0x03u, id, rsp, 8u);
                }
            }
            break;

        case 0x04u: /* Configuration Request */
            if (dlen >= 4u) {
                uint16_t dcid = rd_le16(&d[0]);
                bt_l2cap_ch_t *ch = (dcid == L2CAP_CID_SDP) ? &spp->sdp :
                                    (dcid == L2CAP_CID_RFCOMM) ? &spp->rfcomm : 0;
                uint8_t rsp[12];
                uint16_t rlen = 6u;
                wr_le16(&rsp[0], ch ? ch->remote_cid : 0u);
                wr_le16(&rsp[2], 0u);
                wr_le16(&rsp[4], ch ? 0u : 0x0002u);
                if (ch) {
                    ch->cfg_rx_done = 1u;
                    uint16_t opt_off = 4u;
                    while ((opt_off + 2u) <= dlen) {
                        uint8_t ot = d[opt_off];
                        uint8_t ol = d[opt_off + 1u];
                        if ((opt_off + 2u + ol) > dlen) {
                            break;
                        }
                        if ((ot & 0x7Fu) == 0x01u && ol == 2u) {
                            uint16_t mtu = rd_le16(&d[opt_off + 2u]);
                            if (mtu < 48u) {
                                mtu = 48u;
                            }
                            if (dcid == L2CAP_CID_RFCOMM && mtu < L2CAP_MTU_MAX) {
                                uint16_t cap = (uint16_t)(mtu - 5u);
                                if (cap < spp->rfcomm_mtu) {
                                    spp->rfcomm_mtu = cap;
                                }
                            }
                            rsp[6] = 0x01u;
                            rsp[7] = 0x02u;
                            wr_le16(&rsp[8], mtu);
                            rlen = 10u;
                        }
                        opt_off = (uint16_t)(opt_off + 2u + ol);
                    }
                }
                send_l2cap_sig(spp, 0x05u, id, rsp, rlen);
            }
            break;

        case 0x05u: /* Configuration Response */
            if (dlen >= 4u) {
                uint16_t scid = rd_le16(&d[0]);
                if (scid == L2CAP_CID_SDP) {
                    spp->sdp.cfg_tx_done = 1u;
                } else if (scid == L2CAP_CID_RFCOMM) {
                    spp->rfcomm.cfg_tx_done = 1u;
                }
            }
            break;

        case 0x06u: /* Disconnection Request */
            if (dlen >= 4u) {
                uint16_t dcid = rd_le16(&d[0]);
                uint16_t scid = rd_le16(&d[2]);
                uint8_t rsp[4];
                wr_le16(&rsp[0], dcid);
                wr_le16(&rsp[2], scid);
                send_l2cap_sig(spp, 0x07u, id, rsp, 4u);
                if (dcid == L2CAP_CID_SDP) {
                    mem_zero(&spp->sdp, sizeof(spp->sdp));
                } else if (dcid == L2CAP_CID_RFCOMM) {
                    notify_rfcomm_closed(spp);
                    mem_zero(&spp->rfcomm, sizeof(spp->rfcomm));
                    spp->mux_open    = 0u;
                    spp->credit_flow = 0u;
                }
            }
            break;

        case 0x08u: /* Echo Request */
            send_l2cap_sig(spp, 0x09u, id, 0, 0u);
            break;

        case 0x0Au: /* Information Request */
            if (dlen >= 2u) {
                uint16_t info_type = rd_le16(&d[0]);
                uint8_t rsp[12];
                wr_le16(&rsp[0], info_type);
                if (info_type == 0x0002u) { /* Extended Features */
                    wr_le16(&rsp[2], 0u);
                    mem_zero(&rsp[4], 4u);
                    send_l2cap_sig(spp, 0x0Bu, id, rsp, 8u);
                } else if (info_type == 0x0003u) { /* Fixed Channels */
                    wr_le16(&rsp[2], 0u);
                    mem_zero(&rsp[4], 8u);
                    rsp[4] = 0x02u; /* CID 0x0001 Signaling */
                    send_l2cap_sig(spp, 0x0Bu, id, rsp, 12u);
                } else {
                    wr_le16(&rsp[2], 0x0001u); /* Not supported */
                    send_l2cap_sig(spp, 0x0Bu, id, rsp, 4u);
                }
            }
            break;

        default:
            break;
        }
    }
}

/* Complete SPP Service Record (80-byte attributes payload) */
static const uint8_t g_sdp_spp_attrs[80] = {
    0x09u, 0x00u, 0x00u, 0x0Au, 0x00u, 0x01u, 0x00u, 0x01u,             /* ServiceRecordHandle */
    0x09u, 0x00u, 0x01u, 0x35u, 0x03u, 0x19u, 0x11u, 0x01u,             /* ServiceClassIDList = SPP 0x1101 */
    0x09u, 0x00u, 0x04u, 0x35u, 0x0Cu,                                  /* ProtocolDescriptorList */
    0x35u, 0x03u, 0x19u, 0x01u, 0x00u,                                  /* L2CAP */
    0x35u, 0x05u, 0x19u, 0x00u, 0x03u, 0x08u, 0x01u,                    /* RFCOMM Ch 1 */
    0x09u, 0x00u, 0x05u, 0x35u, 0x03u, 0x19u, 0x10u, 0x02u,             /* BrowseGroupList = Public */
    0x09u, 0x00u, 0x06u, 0x35u, 0x09u,                                  /* LanguageBaseAttributeIDList */
    0x09u, 0x65u, 0x6Eu, 0x09u, 0x00u, 0x6Au, 0x09u, 0x01u, 0x00u,      /* en, UTF-8, base 0x0100 */
    0x09u, 0x00u, 0x09u, 0x35u, 0x08u, 0x35u, 0x06u, 0x19u, 0x11u, 0x01u, 0x09u, 0x01u, 0x02u,
    0x09u, 0x01u, 0x00u, 0x25u, 0x07u, 'C', 'a', 's', 's', 'i', 'n', 'i', /* ServiceName */
};

static const uint8_t g_bt_base_tail[12] = {
    0x00u, 0x00u, 0x10u, 0x00u, 0x80u, 0x00u, 0x00u, 0x80u, 0x5Fu, 0x9Bu, 0x34u, 0xFBu,
};

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int sdp_uuid_known(uint16_t u16)
{
    static const uint16_t known[4] = { 0x1101u, 0x0100u, 0x0003u, 0x1002u };
    for (unsigned k = 0u; k < 4u; k++) {
        if (u16 == known[k]) {
            return 1;
        }
    }
    return 0;
}

static int sdp_uuid_match(const uint8_t *pat, uint16_t pat_len)
{
    if (!pat || pat_len < 2u) {
        return 0;
    }
    uint16_t off = 0u;
    uint16_t seq_len = 0u;
    if (pat[0] == 0x35u) {
        seq_len = pat[1];
        off = 2u;
    } else if (pat[0] == 0x36u && pat_len >= 3u) {
        seq_len = rd_be16(&pat[1]);
        off = 3u;
    } else {
        return 0;
    }
    if ((uint32_t)off + seq_len > pat_len) {
        return 0;
    }

    uint16_t seq_end = (uint16_t)(off + seq_len);
    uint16_t cnt = 0u;
    while (off < seq_end) {
        uint8_t  de  = pat[off];
        uint16_t u16 = 0u;
        if (de == 0x19u) {
            if ((off + 3u) > seq_end) {
                return 0;
            }
            u16 = rd_be16(&pat[off + 1u]);
            off = (uint16_t)(off + 3u);
        } else if (de == 0x1Au) {
            if ((off + 5u) > seq_end || rd_be16(&pat[off + 1u]) != 0x0000u) {
                return 0;
            }
            u16 = rd_be16(&pat[off + 3u]);
            off = (uint16_t)(off + 5u);
        } else if (de == 0x1Cu) {
            if ((off + 17u) > seq_end || rd_be16(&pat[off + 1u]) != 0x0000u ||
                !bytes_eq(&pat[off + 5u], g_bt_base_tail, 12u)) {
                return 0;
            }
            u16 = rd_be16(&pat[off + 3u]);
            off = (uint16_t)(off + 17u);
        } else {
            return 0;
        }
        if (!sdp_uuid_known(u16)) {
            return 0;
        }
        cnt++;
    }
    return (cnt > 0u) ? 1 : 0;
}

static void handle_sdp(bt_spp_t *spp, const uint8_t *buf, uint16_t len)
{
    if (!spp->sdp.open || len < 5u) {
        return;
    }
    uint8_t  pdu_id = buf[0];
    uint16_t tid    = rd_be16(&buf[1]);
    uint16_t plen   = rd_be16(&buf[3]);
    if ((5u + plen) > len) {
        return;
    }
    const uint8_t *p = &buf[5];
    uint8_t rsp[128];

    if (pdu_id == 0x02u) { /* ServiceSearchRequest */
        int match = sdp_uuid_match(p, plen);
        rsp[0] = 0x03u;
        wr_be16(&rsp[1], tid);
        uint16_t cnt = match ? 1u : 0u;
        wr_be16(&rsp[3], (uint16_t)(5u + cnt * 4u));
        wr_be16(&rsp[5], cnt);
        wr_be16(&rsp[7], cnt);
        if (match) {
            wr_be32(&rsp[9], 0x00010001u);
        }
        rsp[9u + cnt * 4u] = 0x00u; /* ContinuationState = 0 */
        (void)queue_l2cap_pdu(spp, spp->sdp.remote_cid, rsp, (uint16_t)(10u + cnt * 4u));
    } else if (pdu_id == 0x04u) { /* ServiceAttributeRequest */
        uint32_t hdl = (plen >= 4u) ? rd_be32(p) : 0u;
        rsp[0] = 0x05u;
        wr_be16(&rsp[1], tid);
        if (hdl == 0x00010001u) {
            uint16_t list_len = (uint16_t)(2u + sizeof(g_sdp_spp_attrs));
            wr_be16(&rsp[3], (uint16_t)(3u + list_len));
            wr_be16(&rsp[5], list_len);
            rsp[7] = 0x35u;
            rsp[8] = (uint8_t)sizeof(g_sdp_spp_attrs);
            mem_copy(&rsp[9], g_sdp_spp_attrs, (uint32_t)sizeof(g_sdp_spp_attrs));
            rsp[9u + sizeof(g_sdp_spp_attrs)] = 0x00u;
            (void)queue_l2cap_pdu(spp, spp->sdp.remote_cid, rsp, (uint16_t)(10u + sizeof(g_sdp_spp_attrs)));
        } else {
            wr_be16(&rsp[3], 5u);
            wr_be16(&rsp[5], 2u);
            rsp[7] = 0x35u;
            rsp[8] = 0x00u;
            rsp[9] = 0x00u;
            (void)queue_l2cap_pdu(spp, spp->sdp.remote_cid, rsp, 10u);
        }
    } else if (pdu_id == 0x06u) { /* ServiceSearchAttributeRequest */
        int match = sdp_uuid_match(p, plen);
        rsp[0] = 0x07u;
        wr_be16(&rsp[1], tid);
        if (match) {
            uint16_t inner = (uint16_t)(2u + sizeof(g_sdp_spp_attrs));
            uint16_t outer = (uint16_t)(2u + inner);
            wr_be16(&rsp[3], (uint16_t)(3u + outer));
            wr_be16(&rsp[5], outer);
            rsp[7]  = 0x35u;
            rsp[8]  = (uint8_t)inner;
            rsp[9]  = 0x35u;
            rsp[10] = (uint8_t)sizeof(g_sdp_spp_attrs);
            mem_copy(&rsp[11], g_sdp_spp_attrs, (uint32_t)sizeof(g_sdp_spp_attrs));
            rsp[11u + sizeof(g_sdp_spp_attrs)] = 0x00u;
            (void)queue_l2cap_pdu(spp, spp->sdp.remote_cid, rsp, (uint16_t)(12u + sizeof(g_sdp_spp_attrs)));
        } else {
            wr_be16(&rsp[3], 5u);
            wr_be16(&rsp[5], 2u);
            rsp[7] = 0x35u;
            rsp[8] = 0x00u;
            rsp[9] = 0x00u;
            (void)queue_l2cap_pdu(spp, spp->sdp.remote_cid, rsp, 10u);
        }
    }
}

static void rfcomm_send_ctrl(bt_spp_t *spp, uint8_t dlci, uint8_t cr, uint8_t ctrl)
{
    uint8_t frame[4];
    frame[0] = (uint8_t)((dlci << 2) | (cr ? 0x02u : 0x00u) | 0x01u);
    frame[1] = ctrl;
    frame[2] = 0x01u; /* length = 0, EA = 1 */
    frame[3] = rfcomm_fcs(frame, 3u);
    (void)queue_l2cap_pdu(spp, spp->rfcomm.remote_cid, frame, 4u);
}

static int rfcomm_send_uih(bt_spp_t *spp, uint8_t dlci, uint8_t pf, uint8_t credits,
                           const uint8_t *info, uint16_t info_len)
{
    if (!spp || spp->tx_count >= BT_SPP_TX_Q_CAP) {
        return -1;
    }
    uint16_t len_bytes = (info_len <= 127u) ? 1u : 2u;
    uint16_t pf_bytes  = pf ? 1u : 0u;
    uint16_t rf_len    = (uint16_t)(2u + len_bytes + pf_bytes + info_len + 1u);
    if ((4u + rf_len) > BT_SPP_TX_MAX_PDU) {
        return -1;
    }

    uint8_t *dst = spp->tx_pdus[spp->tx_tail];
    wr_le16(&dst[0], rf_len);
    wr_le16(&dst[2], spp->rfcomm.remote_cid);

    uint8_t *rf  = &dst[4];
    uint16_t pos = 0u;
    rf[pos++] = (uint8_t)((dlci << 2) | 0x01u); /* Responder UIH: CR=0, EA=1 */
    rf[pos++] = pf ? 0xFFu : 0xEFu;
    if (info_len <= 127u) {
        rf[pos++] = (uint8_t)((info_len << 1) | 0x01u);
    } else {
        rf[pos++] = (uint8_t)((info_len & 0x7Fu) << 1);
        rf[pos++] = (uint8_t)(info_len >> 7);
    }
    if (pf) {
        rf[pos++] = credits;
    }
    if (info_len > 0u && info) {
        mem_copy(&rf[pos], info, info_len);
    }
    /* RFCOMM UIH FCS covers ONLY the 2 bytes (Address, Control) */
    rf[pos + info_len] = rfcomm_fcs(rf, 2u);

    spp->tx_lens[spp->tx_tail] = (uint16_t)(4u + rf_len);
    spp->tx_tail = (uint8_t)((spp->tx_tail + 1u) % BT_SPP_TX_Q_CAP);
    spp->tx_count++;
    return 0;
}

static void rfcomm_send_msc(bt_spp_t *spp, uint8_t dlci)
{
    uint8_t msc[4];
    msc[0] = 0xE3u; /* MSC command */
    msc[1] = 0x05u; /* len=2, EA=1 */
    msc[2] = (uint8_t)((dlci << 2) | 0x03u);
    msc[3] = 0x8Du; /* FC=0, RTC=1, RTR=1, DV=1 */
    (void)rfcomm_send_uih(spp, 0u, 0u, 0u, msc, 4u);
}

static void handle_rfcomm_mux(bt_spp_t *spp, const uint8_t *msg, uint16_t len)
{
    if (len < 2u) {
        return;
    }
    uint8_t mtype = msg[0];
    uint8_t mlen  = (uint8_t)(msg[1] >> 1);
    if ((2u + mlen) > len) {
        return;
    }

    if ((mtype & 0x02u) == 0u) {
        return; /* Only respond to MUX commands (C/R=1) */
    }

    uint8_t rsp[24];
    if (mlen + 2u > sizeof(rsp)) {
        return;
    }
    mem_copy(rsp, msg, (uint32_t)(mlen + 2u));
    rsp[0] = (uint8_t)(mtype & ~0x02u); /* Convert command -> response (C/R=0) */

    if ((mtype & 0xFCu) == 0x80u && mlen >= 8u) { /* PN (0x83 -> 0x81) */
        uint16_t max_mtu = (spp->rfcomm_mtu >= 23u && spp->rfcomm_mtu <= 660u) ? spp->rfcomm_mtu : 660u;
        uint16_t req_mtu = rd_le16(&rsp[6]);
        if (req_mtu == 0u || req_mtu > max_mtu) {
            req_mtu = max_mtu;
            wr_le16(&rsp[6], max_mtu);
        } else if (req_mtu < 23u) {
            req_mtu = 23u;
            wr_le16(&rsp[6], 23u);
        }
        spp->rfcomm_mtu = req_mtu;
        if ((rsp[3] & 0xF0u) == 0xF0u) {
            rsp[3] = (uint8_t)((rsp[3] & 0x0Fu) | 0xE0u); /* Grant credit-based flow */
            spp->credit_flow   = 1u;
            spp->peer_credits  = (uint8_t)(rsp[9] & 0x07u);
            spp->local_credits = 7u;
            rsp[9]             = 7u;
        } else {
            spp->credit_flow = 0u;
        }
    }
    (void)rfcomm_send_uih(spp, 0u, 0u, 0u, rsp, (uint16_t)(mlen + 2u));
}

static void handle_rfcomm(bt_spp_t *spp, const uint8_t *buf, uint16_t len)
{
    if (!spp->rfcomm.open || len < 4u) {
        return;
    }
    uint8_t  addr = buf[0];
    uint8_t  ctrl = buf[1];
    uint8_t  dlci = (uint8_t)(addr >> 2);
    uint8_t  cr   = (uint8_t)((addr >> 1) & 1u);
    uint8_t  pf   = (uint8_t)((ctrl >> 4) & 1u);
    uint8_t  ftype = (uint8_t)(ctrl & 0xEFu);

    uint16_t hdr_len = 3u;
    uint16_t dlen;
    if (buf[2] & 0x01u) {
        dlen = (uint16_t)(buf[2] >> 1);
    } else {
        if (len < 5u) {
            return;
        }
        dlen    = (uint16_t)((buf[2] >> 1) | ((uint16_t)buf[3] << 7));
        hdr_len = 4u;
    }

    uint16_t credit_byte = (spp->credit_flow && ftype == 0xEFu && pf && dlci != 0u) ? 1u : 0u;
    if ((uint32_t)hdr_len + credit_byte + dlen + 1u > len) {
        return;
    }

    /* Validate RFCOMM FCS */
    uint8_t expected_fcs = rfcomm_fcs(buf, (ftype == 0xEFu) ? 2u : hdr_len);
    if (buf[hdr_len + credit_byte + dlen] != expected_fcs) {
        return;
    }

    if (ftype == 0x2Fu) { /* SABM */
        rfcomm_send_ctrl(spp, dlci, cr, 0x73u); /* UA */
        if (dlci == 0u) {
            spp->mux_open = 1u;
        } else if (dlci == RFCOMM_CH1_DLCI) {
            spp->ch1_open = 1u;
            if (!spp->ch1_msc_sent) {
                spp->ch1_msc_sent = 1u;
                rfcomm_send_msc(spp, dlci);
            }
        }
        return;
    }

    if (ftype == 0x43u) { /* DISC */
        rfcomm_send_ctrl(spp, dlci, cr, 0x73u);
        if (dlci == 0u) {
            notify_rfcomm_closed(spp);
            spp->mux_open    = 0u;
            spp->credit_flow = 0u;
        } else if (dlci == RFCOMM_CH1_DLCI) {
            notify_rfcomm_closed(spp);
        }
        return;
    }

    if (ftype == 0xEFu) { /* UIH */
        if (credit_byte && dlci == RFCOMM_CH1_DLCI) {
            uint8_t  given = buf[hdr_len];
            uint16_t sum   = (uint16_t)spp->peer_credits + given;
            spp->peer_credits = (sum > 255u) ? 255u : (uint8_t)sum;
        }
        const uint8_t *info = &buf[hdr_len + credit_byte];
        if (dlci == 0u) {
            handle_rfcomm_mux(spp, info, dlen);
        } else if (dlci == RFCOMM_CH1_DLCI && dlen > 0u) {
            if (spp->credit_flow) {
                if (spp->local_credits > 0u) {
                    spp->local_credits--;
                }
                if (spp->local_credits <= 2u) {
                    uint8_t grant = (uint8_t)(7u - spp->local_credits);
                    spp->local_credits = 7u;
                    (void)rfcomm_send_uih(spp, RFCOMM_CH1_DLCI, 1u, grant, 0, 0u);
                }
            }
            if (spp->on_data) {
                spp->on_data(spp->on_data_user, info, dlen);
            }
        }
    }
}

static void dispatch_l2cap_sdu(bt_spp_t *spp, uint16_t cid, const uint8_t *buf, uint16_t len)
{
    if (cid == L2CAP_CID_SIG) {
        handle_l2cap_sig(spp, buf, len);
    } else if (cid == L2CAP_CID_SDP) {
        handle_sdp(spp, buf, len);
    } else if (cid == L2CAP_CID_RFCOMM) {
        handle_rfcomm(spp, buf, len);
    }
}

void bt_spp_init(bt_spp_t *spp, bt_spp_data_cb_t on_data, void *user)
{
    if (!spp) {
        return;
    }
    mem_zero(spp, sizeof(*spp));
    spp->next_sig_id   = 1u;
    spp->rfcomm_mtu    = 660u;
    spp->credit_flow   = 0u;
    spp->peer_credits  = 0u;
    spp->local_credits = 7u;
    spp->on_data       = on_data;
    spp->on_data_user  = user;
}

void bt_spp_set_disconnect_cb(bt_spp_t *spp, bt_spp_disc_cb_t cb, void *user)
{
    if (!spp) {
        return;
    }
    spp->on_disc      = cb;
    spp->on_disc_user = user;
}

void bt_spp_reset_link(bt_spp_t *spp, uint16_t conhdl)
{
    if (!spp) {
        return;
    }
    notify_rfcomm_closed(spp);
    bt_spp_data_cb_t cb  = spp->on_data;
    void            *u   = spp->on_data_user;
    bt_spp_disc_cb_t dcb = spp->on_disc;
    void            *du  = spp->on_disc_user;
    bt_spp_init(spp, cb, u);
    spp->on_disc      = dcb;
    spp->on_disc_user = du;
    spp->conhdl       = conhdl;
}

void bt_spp_on_acl_rx(bt_spp_t *spp, uint16_t conhdl, uint8_t pb_flag,
                      const uint8_t *payload, uint16_t len)
{
    if (!spp || !payload || len == 0u) {
        return;
    }
    if (spp->conhdl == 0u) {
        spp->conhdl = conhdl;
    } else if (conhdl != spp->conhdl) {
        return; /* Drop ACL traffic from any other connection handle */
    }

    if (pb_flag == 0x02u || pb_flag == 0x00u) { /* L2CAP Start */
        if (len < 4u) {
            return;
        }
        uint16_t l2len = rd_le16(&payload[0]);
        uint16_t cid   = rd_le16(&payload[2]);
        uint16_t take  = (uint16_t)(len - 4u);
        if (l2len > BT_SPP_RX_MAX_PDU) {
            spp->rx_expected = 0u;
            return;
        }
        if (take >= l2len) {
            dispatch_l2cap_sdu(spp, cid, &payload[4], l2len);
            spp->rx_expected = 0u;
            return;
        }
        spp->rx_expected = l2len;
        spp->rx_cid      = cid;
        spp->rx_pos      = take;
        mem_copy(spp->rx_buf, &payload[4], take);
    } else if (pb_flag == 0x01u && spp->rx_expected > 0u) { /* L2CAP Continuation */
        uint16_t need = (uint16_t)(spp->rx_expected - spp->rx_pos);
        uint16_t take = (len < need) ? len : need;
        mem_copy(&spp->rx_buf[spp->rx_pos], payload, take);
        spp->rx_pos = (uint16_t)(spp->rx_pos + take);
        if (spp->rx_pos >= spp->rx_expected) {
            uint16_t cid = spp->rx_cid;
            uint16_t tot = spp->rx_expected;
            spp->rx_expected = 0u;
            dispatch_l2cap_sdu(spp, cid, spp->rx_buf, tot);
        }
    }
}

int bt_spp_is_open(const bt_spp_t *spp)
{
    return (spp && spp->rfcomm.open && spp->ch1_open) ? 1 : 0;
}

int bt_spp_send(bt_spp_t *spp, const uint8_t *data, uint16_t len)
{
    if (!bt_spp_is_open(spp) || (!data && len > 0u) || spp->rfcomm_mtu == 0u) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    uint16_t frames = (uint16_t)((len + spp->rfcomm_mtu - 1u) / spp->rfcomm_mtu);
    if ((spp->credit_flow && frames > spp->peer_credits) || (spp->tx_count + frames) > BT_SPP_TX_Q_CAP) {
        return -1;
    }
    uint16_t off = 0u;
    while (off < len) {
        uint16_t rem   = (uint16_t)(len - off);
        uint16_t chunk = (rem > spp->rfcomm_mtu) ? spp->rfcomm_mtu : rem;
        if (spp->credit_flow) {
            spp->peer_credits--;
        }
        if (rfcomm_send_uih(spp, RFCOMM_CH1_DLCI, 0u, 0u, data + off, chunk) != 0) {
            return -1;
        }
        off = (uint16_t)(off + chunk);
    }
    return 0;
}

uint16_t bt_spp_pop_acl_tx(bt_spp_t *spp, uint16_t *out_conhdl,
                           uint8_t *out_pdu, uint16_t max_len)
{
    if (!spp || spp->tx_count == 0u || !out_pdu) {
        return 0u;
    }
    uint16_t len = spp->tx_lens[spp->tx_head];
    if (len > max_len) {
        return 0u;
    }
    mem_copy(out_pdu, spp->tx_pdus[spp->tx_head], len);
    if (out_conhdl) {
        *out_conhdl = spp->conhdl;
    }
    spp->tx_head = (uint8_t)((spp->tx_head + 1u) % BT_SPP_TX_Q_CAP);
    spp->tx_count--;
    return len;
}
