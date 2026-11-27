/* bt_ctrl_evt: see bt_ctrl_evt.h.
 * SPDX-License-Identifier: MIT */
#include "bt_ctrl_evt.h"
#include "bt_ctrl_cmds.h"
#include "bt_rom_if.h"
#include <string.h>

enum {
    FIX_NONE = 0, FIX_S_H, FIX_S_H_B, FIX_CONN_CMP, FIX_REM_FEAT, FIX_REM_VER, FIX_USER_CONF, FIX_CC_S_H, FIX_S_H_W,
    FIX_MODE, FIX_EXT_FEAT, FIX_QOS, FIX_FLOW, FIX_RNR, FIX_INQ_RES, FIX_INQ_RSSI, FIX_INQ_EIR,
};

typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint8_t  code;      /* event code (EVT) */
    uint16_t opcode;    /* CC/CS */
    uint8_t  out_len;   /* CC: HCI return bytes incl. status; EVT: HCI parameter length */
    uint8_t  need;      /* minimum ROM param_len the packer reads */
    uint8_t  fix;
} evt_row_t;

/* Sources (docs/BT_ROM_HCI_MAP.md §1 "Stock reply table" and §9 "Event packers"):
 *   L = legacy bt_hci.c:741-850, the packers the legacy phone mode runs on silicon today;
 *   S = stock reply tables RAM 0x8178E0 (LM, E065..E0D4) / 0x817628 (LC, E427..E47C) and the stock packer each entry
 *       points to (FW 0xD5860C generic CC, 0xD59189 generic CS, 0xD591E0..0xD596AC events), static only.
 * ROM reply structs are naturally aligned (BT_ROM_INTERFACE §5.1): every FIX_* below is the stock packer's de-padding.
 *   R = read from the ROM senders only (docs/BT_ROM_HCI_MAP.md §3): E0D6/E0D7, the inquiry results of modes 1/2. The
 *       stock firmware has no packer for them and never sent 0C45; they stay unreachable while page-0 bit 30 is masked
 *       (bt_ctrl_cmds_features_p0), and are packed here so a peer-independent probe of 0C45 has them.
 * Not packed (counted in rom_unknown_ids if they ever arrive): E46E/E46F (0x2C/0x2D synchronous links: no SCO/eSCO in
 * the build), E470 (0x2E sniff subrating: no sniff), E454/E455 (stock-synthesised TASK_HCI -> TASK_HCI replies, never
 * sent by the ROM). */
static const evt_row_t k_evt[] = {
    /* Command Complete (status first, then return parameters) */
    { 0xE079u, BT_CTRL_EVK_CC, 0u, 0x0C01u, 1u, 1u, FIX_NONE },      /* L */
    { 0xE083u, BT_CTRL_EVK_CC, 0u, 0x0C13u, 1u, 1u, FIX_NONE },      /* L */
    { 0xE08Au, BT_CTRL_EVK_CC, 0u, 0x0C1Au, 1u, 1u, FIX_NONE },      /* L */
    { 0xE094u, BT_CTRL_EVK_CC, 0u, 0x0C24u, 1u, 1u, FIX_NONE },      /* L */
    { 0xE0B1u, BT_CTRL_EVK_CC, 0u, 0x0C56u, 1u, 1u, FIX_NONE },      /* L */
    { 0xE0C0u, BT_CTRL_EVK_CC, 0u, 0x1009u, 7u, 7u, FIX_NONE },      /* L */
    { 0xE06Au, BT_CTRL_EVK_CC, 0u, 0x040Bu, 7u, 7u, FIX_NONE },      /* L */
    { 0xE06Bu, BT_CTRL_EVK_CC, 0u, 0x040Cu, 7u, 7u, FIX_NONE },      /* L */
    { 0xE06Cu, BT_CTRL_EVK_CC, 0u, 0x040Du, 7u, 7u, FIX_NONE },      /* L */
    { 0xE06Du, BT_CTRL_EVK_CC, 0u, 0x040Eu, 7u, 7u, FIX_NONE },      /* L */
    { 0xE06Fu, BT_CTRL_EVK_CC, 0u, 0x042Bu, 7u, 7u, FIX_NONE },      /* L */
    { 0xE070u, BT_CTRL_EVK_CC, 0u, 0x042Cu, 7u, 7u, FIX_NONE },      /* L */
    { 0xE071u, BT_CTRL_EVK_CC, 0u, 0x042Du, 7u, 7u, FIX_NONE },      /* L */
    { 0xE066u, BT_CTRL_EVK_CC, 0u, 0x0402u, 1u, 1u, FIX_NONE },      /* S */
    { 0xE069u, BT_CTRL_EVK_CC, 0u, 0x0408u, 7u, 7u, FIX_NONE },      /* S */
    { 0xE06Eu, BT_CTRL_EVK_CC, 0u, 0x041Au, 7u, 7u, FIX_NONE },      /* S */
    { 0xE076u, BT_CTRL_EVK_CC, 0u, 0x0434u, 7u, 7u, FIX_NONE },      /* S */
    { 0xE088u, BT_CTRL_EVK_CC, 0u, 0x0C18u, 1u, 1u, FIX_NONE },      /* S */
    { 0xE08Cu, BT_CTRL_EVK_CC, 0u, 0x0C1Cu, 1u, 1u, FIX_NONE },      /* S */
    { 0xE08Eu, BT_CTRL_EVK_CC, 0u, 0x0C1Eu, 1u, 1u, FIX_NONE },      /* S */
    { 0xE0A2u, BT_CTRL_EVK_CC, 0u, 0x0C3Au, 1u, 1u, FIX_NONE },      /* S */
    { 0xE0A9u, BT_CTRL_EVK_CC, 0u, 0x0C45u, 1u, 1u, FIX_NONE },      /* S */
    { 0xE0AFu, BT_CTRL_EVK_CC, 0u, 0x0C52u, 1u, 1u, FIX_NONE },      /* S */
    { 0xE42Eu, BT_CTRL_EVK_CC, 0u, 0x0C28u, 3u, 4u, FIX_CC_S_H },    /* S: {s, pad, u16 h} -> s, h (FW 0xD5902C) */
    { 0xE431u, BT_CTRL_EVK_CC, 0u, 0x0C37u, 3u, 4u, FIX_CC_S_H },    /* S: same (FW 0xD5905C) */
    /* Command Status (param[0] = status; stock generic CS packer FW 0xD59189) */
    { 0xE0C9u, BT_CTRL_EVK_CS, 0u, 0x0409u, 0u, 1u, FIX_NONE },      /* L */
    { 0xE43Bu, BT_CTRL_EVK_CS, 0u, 0x0409u, 0u, 1u, FIX_NONE },      /* L */
    { 0xE0CAu, BT_CTRL_EVK_CS, 0u, 0x040Au, 0u, 1u, FIX_NONE },      /* L */
    { 0xE43Cu, BT_CTRL_EVK_CS, 0u, 0x040Au, 0u, 1u, FIX_NONE },      /* L */
    { 0xE43Au, BT_CTRL_EVK_CS, 0u, 0x0406u, 0u, 1u, FIX_NONE },      /* L */
    { 0xE0C7u, BT_CTRL_EVK_CS, 0u, 0x0401u, 0u, 1u, FIX_NONE },      /* S */
    { 0xE0C8u, BT_CTRL_EVK_CS, 0u, 0x0405u, 0u, 1u, FIX_NONE },      /* S */
    { 0xE0CBu, BT_CTRL_EVK_CS, 0u, 0x0419u, 0u, 1u, FIX_NONE },      /* S */
    { 0xE43Du, BT_CTRL_EVK_CS, 0u, 0x040Fu, 0u, 1u, FIX_NONE },      /* S */
    { 0xE43Eu, BT_CTRL_EVK_CS, 0u, 0x0411u, 0u, 1u, FIX_NONE },      /* S */
    { 0xE43Fu, BT_CTRL_EVK_CS, 0u, 0x0413u, 0u, 1u, FIX_NONE },      /* S */
    { 0xE441u, BT_CTRL_EVK_CS, 0u, 0x041Bu, 0u, 1u, FIX_NONE },      /* S */
    { 0xE442u, BT_CTRL_EVK_CS, 0u, 0x041Cu, 0u, 1u, FIX_NONE },      /* S */
    { 0xE443u, BT_CTRL_EVK_CS, 0u, 0x041Du, 0u, 1u, FIX_NONE },      /* S */
    { 0xE444u, BT_CTRL_EVK_CS, 0u, 0x041Fu, 0u, 1u, FIX_NONE },      /* S */
    /* Events */
    { 0xE0D0u, BT_CTRL_EVK_EVT, 0x01u, 0u,  1u,  1u, FIX_NONE },      /* S: Inquiry Complete */
    { 0xE0D1u, BT_CTRL_EVK_EVT, 0x02u, 0u, 15u, 16u, FIX_INQ_RES },   /* S: Inquiry Result (mode 0), map §3 */
    { 0xE0D6u, BT_CTRL_EVK_EVT, 0x22u, 0u, 15u, 15u, FIX_INQ_RSSI },  /* R: Inquiry Result with RSSI (mode 1/2) */
    { 0xE0D7u, BT_CTRL_EVK_EVT, 0x2Fu, 0u, 255u, 15u, FIX_INQ_EIR },  /* R: Extended Inquiry Result (mode 2 + EIR) */
    { 0xE0D2u, BT_CTRL_EVK_EVT, 0x03u, 0u, 11u, 13u, FIX_CONN_CMP },  /* L */
    { 0xE456u, BT_CTRL_EVK_EVT, 0x03u, 0u, 11u, 13u, FIX_CONN_CMP },  /* L */
    { 0xE457u, BT_CTRL_EVK_EVT, 0x04u, 0u, 10u, 10u, FIX_NONE },      /* L */
    { 0xE458u, BT_CTRL_EVK_EVT, 0x05u, 0u,  4u,  5u, FIX_S_H_B },     /* L */
    { 0xE459u, BT_CTRL_EVK_EVT, 0x06u, 0u,  3u,  4u, FIX_S_H },       /* L */
    { 0xE0D3u, BT_CTRL_EVK_EVT, 0x07u, 0u, 255u, 7u, FIX_RNR },       /* S: Remote Name Request Complete */
    { 0xE45Au, BT_CTRL_EVK_EVT, 0x08u, 0u,  4u,  5u, FIX_S_H_B },     /* L */
    { 0xE45Bu, BT_CTRL_EVK_EVT, 0x09u, 0u,  3u,  4u, FIX_S_H },       /* S: Change Conn Link Key Complete */
    { 0xE45Cu, BT_CTRL_EVK_EVT, 0x0Au, 0u,  4u,  5u, FIX_S_H_B },     /* S: Master Link Key Complete */
    { 0xE45Du, BT_CTRL_EVK_EVT, 0x0Bu, 0u, 11u, 12u, FIX_REM_FEAT },  /* L */
    { 0xE45Eu, BT_CTRL_EVK_EVT, 0x0Cu, 0u,  8u, 10u, FIX_REM_VER },   /* L */
    { 0xE45Fu, BT_CTRL_EVK_EVT, 0x0Du, 0u, 21u, 24u, FIX_QOS },       /* S: QoS Setup Complete */
    { 0xE0D4u, BT_CTRL_EVK_EVT, 0x10u, 0u,  1u,  1u, FIX_NONE },      /* S: Hardware Error */
    { 0xE460u, BT_CTRL_EVK_EVT, 0x11u, 0u,  2u,  2u, FIX_NONE },      /* S: Flush Occurred */
    { 0xE461u, BT_CTRL_EVK_EVT, 0x12u, 0u,  8u,  8u, FIX_NONE },      /* S: Role Change */
    { 0xE463u, BT_CTRL_EVK_EVT, 0x14u, 0u,  6u,  8u, FIX_MODE },      /* S: Mode Change */
    { 0xE464u, BT_CTRL_EVK_EVT, 0x16u, 0u,  6u,  6u, FIX_NONE },      /* L */
    { 0xE465u, BT_CTRL_EVK_EVT, 0x17u, 0u,  6u,  6u, FIX_NONE },      /* L */
    { 0xE466u, BT_CTRL_EVK_EVT, 0x18u, 0u, 23u, 23u, FIX_NONE },      /* L */
    { 0xE467u, BT_CTRL_EVK_EVT, 0x1Au, 0u,  1u,  1u, FIX_NONE },      /* S: Data Buffer Overflow */
    { 0xE468u, BT_CTRL_EVK_EVT, 0x1Bu, 0u,  3u,  3u, FIX_NONE },      /* L */
    { 0xE469u, BT_CTRL_EVK_EVT, 0x1Cu, 0u,  5u,  6u, FIX_S_H_W },     /* S: Read Clock Offset Complete */
    { 0xE46Au, BT_CTRL_EVK_EVT, 0x1Du, 0u,  5u,  6u, FIX_S_H_W },     /* S: Conn Packet Type Changed */
    { 0xE46Bu, BT_CTRL_EVK_EVT, 0x1Eu, 0u,  2u,  2u, FIX_NONE },      /* S: QoS Violation */
    { 0xE46Cu, BT_CTRL_EVK_EVT, 0x21u, 0u, 22u, 24u, FIX_FLOW },      /* S: Flow Specification Complete */
    { 0xE46Du, BT_CTRL_EVK_EVT, 0x23u, 0u, 13u, 14u, FIX_EXT_FEAT },  /* S: Read Remote Ext Features Complete */
    { 0xE471u, BT_CTRL_EVK_EVT, 0x30u, 0u,  3u,  4u, FIX_S_H },       /* S: Encryption Key Refresh Complete */
    { 0xE472u, BT_CTRL_EVK_EVT, 0x31u, 0u,  6u,  6u, FIX_NONE },      /* L */
    { 0xE473u, BT_CTRL_EVK_EVT, 0x32u, 0u,  9u,  9u, FIX_NONE },      /* L */
    { 0xE474u, BT_CTRL_EVK_EVT, 0x33u, 0u, 10u, 12u, FIX_USER_CONF }, /* L */
    { 0xE475u, BT_CTRL_EVK_EVT, 0x34u, 0u,  6u,  6u, FIX_NONE },      /* S: User Passkey Request */
    { 0xE476u, BT_CTRL_EVK_EVT, 0x35u, 0u,  6u,  6u, FIX_NONE },      /* S: Remote OOB Data Request */
    { 0xE477u, BT_CTRL_EVK_EVT, 0x36u, 0u,  7u,  7u, FIX_NONE },      /* L */
    { 0xE478u, BT_CTRL_EVK_EVT, 0x38u, 0u,  4u,  4u, FIX_NONE },      /* L */
    { 0xE479u, BT_CTRL_EVK_EVT, 0x39u, 0u,  2u,  2u, FIX_NONE },      /* S: Enhanced Flush Complete */
    { 0xE47Au, BT_CTRL_EVK_EVT, 0x3Bu, 0u, 10u, 12u, FIX_USER_CONF }, /* S: User Passkey Notification (u32 +8 -> +6) */
    { 0xE47Bu, BT_CTRL_EVK_EVT, 0x3Cu, 0u,  7u,  7u, FIX_NONE },      /* S: Keypress Notification */
    { 0xE47Cu, BT_CTRL_EVK_EVT, 0x3Du, 0u, 14u, 14u, FIX_NONE },      /* S: Remote Host Supported Features */
};
#define N_EVT ((uint32_t)(sizeof(k_evt) / sizeof(k_evt[0])))

static const evt_row_t *find(uint16_t id)
{
    for (uint32_t i = 0u; i < N_EVT; i++) {
        if (k_evt[i].id == id) {
            return &k_evt[i];
        }
    }
    return NULL;
}

uint16_t bt_ctrl_evt_reply_opcode(uint16_t id)
{
    const evt_row_t *r = find(id);
    return (r != NULL && (r->kind == BT_CTRL_EVK_CC || r->kind == BT_CTRL_EVK_CS)) ? r->opcode : 0u;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

BT_FAST uint16_t bt_ctrl_evt_pack(uint16_t id, const uint8_t *param, uint16_t param_len, uint8_t *out,
                                  bt_ctrl_evt_info_t *info)
{
    memset(info, 0, sizeof(*info));
    const evt_row_t *r = find(id);
    if (r == NULL) {
        info->kind = BT_CTRL_EVK_UNKNOWN;
        return 0u;
    }
    if (param == NULL || param_len < r->need) {
        info->kind = BT_CTRL_EVK_SHORT;
        return 0u;
    }
    info->kind = r->kind;
    info->opcode = r->opcode;
    info->status = param[0];
    if (r->kind == BT_CTRL_EVK_CC) {
        info->evt_code = BT_HCI_EVT_CC;
        if (r->fix == FIX_CC_S_H) {     /* {s, pad, u16 h} -> s, h (stock FW 0xD5902C / 0xD5905C) */
            info->handle = rd16(&param[2]);
            return bt_ctrl_pack_cc(out, r->opcode, param[0], &param[2], 2u);
        }
        return bt_ctrl_pack_cc(out, r->opcode, param[0], &param[1], (uint8_t)(r->out_len - 1u));
    }
    if (r->kind == BT_CTRL_EVK_CS) {
        info->evt_code = BT_HCI_EVT_CS;
        return bt_ctrl_pack_cs(out, r->opcode, param[0]);
    }
    info->evt_code = r->code;
    uint8_t *o = &out[3];
    switch (r->fix) {
    case FIX_S_H:                       /* status, pad, u16 handle */
    case FIX_S_H_B:                     /* status, pad, u16 handle, u8 */
        o[0] = param[0];
        o[1] = param[2];
        o[2] = param[3];
        if (r->fix == FIX_S_H_B) {
            o[3] = param[4];
        }
        info->handle = rd16(&param[2]);
        break;
    case FIX_CONN_CMP:                  /* bt_hci.c:763-772 */
        o[0] = param[0];
        o[1] = param[2];
        o[2] = param[3];
        memcpy(&o[3], &param[5], 6u);
        o[9] = param[11];
        o[10] = param[12];
        info->handle = rd16(&param[2]);
        break;
    case FIX_REM_FEAT:                  /* bt_hci.c:799-805 */
        o[0] = param[0];
        o[1] = param[2];
        o[2] = param[3];
        memcpy(&o[3], &param[4], 8u);
        info->handle = rd16(&param[2]);
        break;
    case FIX_REM_VER:                   /* bt_hci.c:807-816 */
        o[0] = param[0];
        o[1] = param[2];
        o[2] = param[3];
        o[3] = param[4];
        o[4] = param[6];
        o[5] = param[7];
        o[6] = param[8];
        o[7] = param[9];
        info->handle = rd16(&param[2]);
        break;
    case FIX_USER_CONF:                 /* bt_hci.c:841-848: u32 numeric value moved from +8 to +6 (also 0x3B) */
        memcpy(&o[0], &param[0], 6u);
        memcpy(&o[6], &param[8], 4u);
        break;
    case FIX_S_H_W:                     /* {s, pad, u16 h, u16 v} -> s, h, v (FW 0xD592D8 / 0xD592F4) */
        o[0] = param[0];
        memcpy(&o[1], &param[2], 4u);
        info->handle = rd16(&param[2]);
        break;
    case FIX_MODE:                      /* {s, pad, u16 h, mode, pad, u16 interval} (FW 0xD595B8) */
        o[0] = param[0];
        o[1] = param[2];
        o[2] = param[3];
        o[3] = param[4];
        o[4] = param[6];
        o[5] = param[7];
        info->handle = rd16(&param[2]);
        break;
    case FIX_EXT_FEAT:                  /* {s, pad, u16 h, page, max_page, feat[8]} (FW 0xD5938C) */
        o[0] = param[0];
        memcpy(&o[1], &param[2], 12u);
        info->handle = rd16(&param[2]);
        break;
    case FIX_QOS:                       /* {s, pad, u16 h, flags, type, pad[2], u32 x4} (FW 0xD59234) */
    case FIX_FLOW:                      /* {s, pad, u16 h, flags, dir, type, pad, u32 x4} (FW 0xD59324) */
        o[0] = param[0];
        o[1] = param[2];
        o[2] = param[3];
        o[3] = param[4];
        o[4] = param[5];
        if (r->fix == FIX_FLOW) {
            o[5] = param[6];
        }
        memcpy(&o[r->fix == FIX_FLOW ? 6u : 5u], &param[8], 16u);
        info->handle = rd16(&param[2]);
        break;
    case FIX_RNR: {                     /* {s, bdaddr, name[248]} = wire (FW 0xD5953C); a short name is zero-filled */
        uint16_t n = (uint16_t)(param_len - 7u);
        if (n > 248u) {
            n = 248u;
        }
        memcpy(o, param, 7u);
        memcpy(&o[7], &param[7], n);
        memset(&o[7u + n], 0, 248u - n);
        break;
    }
    case FIX_INQ_RES:                   /* mode 0: clkoff 0xE -> 0xD, one response per message (map §3) */
        o[0] = 1u;
        memcpy(&o[1], &param[1], 12u);
        o[13] = param[14];
        o[14] = param[15];
        break;
    case FIX_INQ_RSSI:                  /* {num, bdaddr, psrm, rsvd, cod[3], u16 clkoff@0xC, i8 rssi@0xE} = wire 0x22 */
        memcpy(o, param, 15u);
        o[0] = 1u;                      /* one response per message; the EC05 mode-0 path leaves num 0 (map §3) */
        break;
    case FIX_INQ_EIR: {                 /* as E0D6, then eir[240]@0xF = wire 0x2F; a short message is zero-filled */
        uint16_t n = param_len < 255u ? param_len : 255u;
        memcpy(o, param, n);
        memset(&o[n], 0, 255u - n);
        o[0] = 1u;
        break;
    }
    default:
        memcpy(o, param, r->out_len);
        break;
    }
    out[0] = BT_H4_EVT;
    out[1] = r->code;
    out[2] = r->out_len;
    return (uint16_t)(3u + r->out_len);
}

BT_FAST uint16_t bt_ctrl_evt_pack_nocp(uint8_t *out, uint8_t n, const uint16_t *handles, const uint16_t *counts)
{
    out[0] = BT_H4_EVT;
    out[1] = BT_HCI_EVT_NOCP;
    out[2] = (uint8_t)(1u + 4u * n);
    out[3] = n;
    for (uint32_t i = 0u; i < n; i++) {   /* interleaved (handle, count) pairs, as l2c_link.c:1547-1549 reads */
        out[4u + 4u * i] = (uint8_t)(handles[i] & 0xFFu);
        out[5u + 4u * i] = (uint8_t)(handles[i] >> 8);
        out[6u + 4u * i] = (uint8_t)(counts[i] & 0xFFu);
        out[7u + 4u * i] = (uint8_t)(counts[i] >> 8);
    }
    return (uint16_t)(4u + 4u * n);
}
