/* bt_ctrl_evt: see bt_ctrl_evt.h.
 * SPDX-License-Identifier: MIT */
#include "bt_ctrl_evt.h"
#include "bt_ctrl_cmds.h"
#include <string.h>

enum { FIX_NONE = 0, FIX_S_H, FIX_S_H_B, FIX_CONN_CMP, FIX_REM_FEAT, FIX_REM_VER, FIX_USER_CONF };

typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint8_t  code;      /* event code (EVT) */
    uint16_t opcode;    /* CC/CS */
    uint8_t  out_len;   /* CC: ROM bytes incl. status; EVT: HCI parameter length */
    uint8_t  need;      /* minimum ROM param_len the packer reads */
    uint8_t  fix;
} evt_row_t;

/* bt_hci.c:741-850. ROM reply structs are naturally aligned (BT_ROM_INTERFACE §5.1); the FIX_* layouts are the
 * legacy de-padding, unverified on silicon beyond what the legacy chain exercised (Exp 1 stage B/C, V-x rows). */
static const evt_row_t k_evt[] = {
    /* Command Complete (status first, then return parameters) */
    { 0xE079u, BT_CTRL_EVK_CC, 0u, 0x0C01u, 1u, 1u, FIX_NONE },
    { 0xE083u, BT_CTRL_EVK_CC, 0u, 0x0C13u, 1u, 1u, FIX_NONE },
    { 0xE08Au, BT_CTRL_EVK_CC, 0u, 0x0C1Au, 1u, 1u, FIX_NONE },
    { 0xE094u, BT_CTRL_EVK_CC, 0u, 0x0C24u, 1u, 1u, FIX_NONE },
    { 0xE0B1u, BT_CTRL_EVK_CC, 0u, 0x0C56u, 1u, 1u, FIX_NONE },
    { 0xE0C0u, BT_CTRL_EVK_CC, 0u, 0x1009u, 7u, 7u, FIX_NONE },
    { 0xE06Au, BT_CTRL_EVK_CC, 0u, 0x040Bu, 7u, 7u, FIX_NONE },
    { 0xE06Bu, BT_CTRL_EVK_CC, 0u, 0x040Cu, 7u, 7u, FIX_NONE },
    { 0xE06Cu, BT_CTRL_EVK_CC, 0u, 0x040Du, 7u, 7u, FIX_NONE },
    { 0xE06Du, BT_CTRL_EVK_CC, 0u, 0x040Eu, 7u, 7u, FIX_NONE },
    { 0xE06Fu, BT_CTRL_EVK_CC, 0u, 0x042Bu, 7u, 7u, FIX_NONE },
    { 0xE070u, BT_CTRL_EVK_CC, 0u, 0x042Cu, 7u, 7u, FIX_NONE },
    { 0xE071u, BT_CTRL_EVK_CC, 0u, 0x042Du, 7u, 7u, FIX_NONE },
    /* Command Status (param[0] = status) */
    { 0xE0C9u, BT_CTRL_EVK_CS, 0u, 0x0409u, 0u, 1u, FIX_NONE },
    { 0xE43Bu, BT_CTRL_EVK_CS, 0u, 0x0409u, 0u, 1u, FIX_NONE },
    { 0xE0CAu, BT_CTRL_EVK_CS, 0u, 0x040Au, 0u, 1u, FIX_NONE },
    { 0xE43Cu, BT_CTRL_EVK_CS, 0u, 0x040Au, 0u, 1u, FIX_NONE },
    { 0xE43Au, BT_CTRL_EVK_CS, 0u, 0x0406u, 0u, 1u, FIX_NONE },
    /* Events */
    { 0xE0D2u, BT_CTRL_EVK_EVT, 0x03u, 0u, 11u, 13u, FIX_CONN_CMP },
    { 0xE456u, BT_CTRL_EVK_EVT, 0x03u, 0u, 11u, 13u, FIX_CONN_CMP },
    { 0xE457u, BT_CTRL_EVK_EVT, 0x04u, 0u, 10u, 10u, FIX_NONE },
    { 0xE458u, BT_CTRL_EVK_EVT, 0x05u, 0u,  4u,  5u, FIX_S_H_B },
    { 0xE459u, BT_CTRL_EVK_EVT, 0x06u, 0u,  3u,  4u, FIX_S_H },
    { 0xE45Au, BT_CTRL_EVK_EVT, 0x08u, 0u,  4u,  5u, FIX_S_H_B },
    { 0xE45Du, BT_CTRL_EVK_EVT, 0x0Bu, 0u, 11u, 12u, FIX_REM_FEAT },
    { 0xE45Eu, BT_CTRL_EVK_EVT, 0x0Cu, 0u,  8u, 10u, FIX_REM_VER },
    { 0xE464u, BT_CTRL_EVK_EVT, 0x16u, 0u,  6u,  6u, FIX_NONE },
    { 0xE465u, BT_CTRL_EVK_EVT, 0x17u, 0u,  6u,  6u, FIX_NONE },
    { 0xE466u, BT_CTRL_EVK_EVT, 0x18u, 0u, 23u, 23u, FIX_NONE },
    { 0xE468u, BT_CTRL_EVK_EVT, 0x1Bu, 0u,  3u,  3u, FIX_NONE },
    { 0xE472u, BT_CTRL_EVK_EVT, 0x31u, 0u,  6u,  6u, FIX_NONE },
    { 0xE473u, BT_CTRL_EVK_EVT, 0x32u, 0u,  9u,  9u, FIX_NONE },
    { 0xE474u, BT_CTRL_EVK_EVT, 0x33u, 0u, 10u, 12u, FIX_USER_CONF },
    { 0xE477u, BT_CTRL_EVK_EVT, 0x36u, 0u,  7u,  7u, FIX_NONE },
    { 0xE478u, BT_CTRL_EVK_EVT, 0x38u, 0u,  4u,  4u, FIX_NONE },
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

uint16_t bt_ctrl_evt_pack(uint16_t id, const uint8_t *param, uint16_t param_len, uint8_t *out,
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
    case FIX_USER_CONF:                 /* bt_hci.c:841-848: u32 numeric value moved from +8 to +6 */
        memcpy(&o[0], &param[0], 6u);
        memcpy(&o[6], &param[8], 4u);
        break;
    default:
        memcpy(o, param, r->out_len);
        break;
    }
    out[0] = BT_H4_EVT;
    out[1] = r->code;
    out[2] = r->out_len;
    return (uint16_t)(3u + r->out_len);
}

uint16_t bt_ctrl_evt_pack_nocp(uint8_t *out, uint8_t n, const uint16_t *handles, const uint16_t *counts)
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
