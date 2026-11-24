/* bt_ctrl_cmds: HCI opcode policy table and reply packers for bt_ctrl (plan M1 "Opcode policy";
 * table = docs/BT_CTRL_OPCODE_TABLE.md §8, verbatim). Pure functions, no state beyond the effective table
 * computed once by bt_ctrl_cmds_init().
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_CMDS_H
#define BT_CTRL_CMDS_H

#include <stdbool.h>
#include <stdint.h>

/* ---- table vocabulary (BT_CTRL_OPCODE_TABLE.md §1, §8) ---- */
enum { BTC_M = 0, BTC_O = 1 };
enum { BTC_FWD = 0, BTC_SYN = 1, BTC_REJ = 2, BTC_LOCAL = 3 };
enum { BTC_CC = 0, BTC_CS = 1, BTC_NONE = 2 };
enum { BTC_ECHO_NONE = 0, BTC_ECHO_BDADDR = 1, BTC_ECHO_HANDLE = 2 };
enum { BTC_EXP_NONE = 0, BTC_EXP_V_A, BTC_EXP_V_B, BTC_EXP_V_C, BTC_EXP_G_A, BTC_EXP_G_B, BTC_EXP_G_C };

typedef struct {
    uint16_t op;
    uint8_t  cls, act, gated, rsp;
    uint8_t  plen;          /* Parameter_Total_Length of the initial reply (CC: 4 + return bytes; CS: 4) */
    uint8_t  echo;
    uint16_t rom_req, rom_rsp;
    uint8_t  so, sm;        /* 1002 octet / mask; so = 0xFF: none */
    uint8_t  exp;
} bt_ctrl_cmd_row_t;

/* ---- synthesized constants (BT_CTRL_OPCODE_TABLE.md §8 tail) ---- */
#define BT_CTRL_H2C_ACL_LEN          1021u   /* 1005 HC_ACL_Data_Packet_Length */
#define BT_CTRL_H2C_ACL_CREDITS      4u      /* 1005 HC_Total_Num_ACL_Data_Packets; <= EM TX pool (Exp 1 C) */
#define BT_CTRL_C2H_ACL_LEN          1021u   /* enforced from 0C33 */
#define BT_CTRL_C2H_ACL_CREDITS      4u      /* enforced from 0C33 */
#define BT_CTRL_HCI_VERSION          0x06u
#define BT_CTRL_HCI_REVISION         0x0000u
#define BT_CTRL_LMP_VERSION          0x06u
#define BT_CTRL_MANUFACTURER         0x0060u /* RivieraWaves; must never be 0x000F (Broadcom) */
#define BT_CTRL_LMP_SUBVER           0x0000u /* Exp 1 A: replace with ROM LMP_version_res value */
#define BT_CTRL_EXT_FEAT_MAX_PAGE    1u
#define BT_CTRL_FEAT_P0_ROM          { 0x7Fu, 0xFEu, 0xCDu, 0xFAu, 0xDBu, 0xFDu, 0x7Bu, 0x87u }
#define BT_CTRL_FEAT_P0_AND_MASK     { 0x1Fu, 0x06u, 0xC4u, 0x0Au, 0x98u, 0x1Du, 0x19u, 0x85u }
#define BT_CTRL_FEAT_P0_EIR_BYTE     6u      /* cleared unless 0x0C52's effective action is FWD */
#define BT_CTRL_FEAT_P0_EIR_MASK     0x01u
#define BT_CTRL_FEAT_P0_INQRSSI_BYTE 3u      /* stays clear until 0x0C45 + 0x22/0x2F packers pass G-C */
#define BT_CTRL_FEAT_P0_INQRSSI_MASK 0x40u
#define BT_CTRL_FEAT_P1_SSP_HOST     0x01u
#define BT_CTRL_NAME_LEN             248u
#define BT_CTRL_SUPPORTED_CMDS_LEN   64u

/* Exp 1 gate list: opcodes whose gate passed on silicon, e.g. -DBT_CTRL_EXP1_PASSED_LIST=0x0C1Cu,0x0C1Eu.
 * (BT_CTRL_OPCODE_TABLE.md §8 names one macro per opcode; a list is used because an X-macro cannot test whether a
 * per-opcode macro is defined.) A row whose effective action becomes FWD must have rom_req, rom_rsp and a FWD
 * payload layout, else bt_ctrl_cmds_init() reports a validation error. */
#ifndef BT_CTRL_EXP1_PASSED_LIST
#define BT_CTRL_EXP1_PASSED_LIST 0x0000u
#endif

/* The headphones (A2DP source, initiator) rows: Inquiry, Inquiry Cancel, Create Connection, Create Connection Cancel
 * (docs/BT_A2DP_DESIGN.md §7, milestone H2). Still gated: the default list is empty, so release images refuse them.
 * Host tests and bench images opt in with -DBT_CTRL_EXP1_PASSED_LIST=BT_CTRL_HP_GATE_LIST (make test-bt-ctrl runs both
 * builds; make BT=1 BENCH=1 HP_GATES=1 for silicon Exp 4a/4b). Each row flips to the default list only after its
 * silicon experiment passed (one commit each, citing docs/BT_HW_RESULTS.md). 040F, 080B and 080D stay REJ (§7). */
#define BT_CTRL_HP_GATE_LIST 0x0401u, 0x0402u, 0x0405u, 0x0408u

/* HCI status codes used by bt_ctrl */
#define BT_HCI_OK                    0x00u
#define BT_HCI_ERR_UNKNOWN_CMD       0x01u
#define BT_HCI_ERR_UNKNOWN_CONN      0x02u
#define BT_HCI_ERR_HW_FAILURE        0x03u
#define BT_HCI_ERR_MEM_FULL          0x07u
#define BT_HCI_ERR_CMD_DISALLOWED    0x0Cu
#define BT_HCI_ERR_INVALID_PARAMS    0x12u
#define BT_HCI_ERR_LMP_TIMEOUT       0x22u   /* forwarded command not answered by the ROM within the deadline */

#define BT_H4_CMD   0x01u
#define BT_H4_ACL   0x02u
#define BT_H4_EVT   0x04u
#define BT_HCI_EVT_CC    0x0Eu
#define BT_HCI_EVT_CS    0x0Fu
#define BT_HCI_EVT_NOCP  0x13u
#define BT_HCI_EVT_DISC  0x05u
#define BT_HCI_EVT_LE    0x3Eu
#define BT_CTRL_OP_HOST_NUM_COMPLETED 0x0C35u
#define BT_CTRL_OP_RESET              0x0C03u

/* Returns the number of validation errors in the (effective) table; 0 is required. Idempotent. */
int bt_ctrl_cmds_init(void);

const bt_ctrl_cmd_row_t *bt_ctrl_cmds_table(uint32_t *count);
/* Row for an opcode; vendor OGF 0x3F maps to a shared REJ CC(4) row; NULL = not in the closed table. */
const bt_ctrl_cmd_row_t *bt_ctrl_cmds_lookup(uint16_t opcode);
/* Effective action after the Exp 1 gate list (BTC_FWD/SYN/REJ/LOCAL). */
uint8_t bt_ctrl_cmds_eff(const bt_ctrl_cmd_row_t *row);
/* ROM payload size for a FWD row (legacy g_openfw_cmd_tbl min_len, bt_hci.c:882-904); 0 = no verified layout, or a
 * verified zero-length layout (bt_ctrl_cmds_fwd_layout tells the two apart). */
uint8_t bt_ctrl_cmds_fwd_min_len(uint16_t opcode);
/* true if `opcode` has a verified ROM payload layout; *min_len (may be NULL) gets its size, 0 for a parameterless KE
 * message (0402 -> E001, sent by the stock firmware with ke_msg_send_basic: docs/BT_ROM_HCI_MAP.md §1, §2). */
bool bt_ctrl_cmds_fwd_layout(uint16_t opcode, uint8_t *min_len);
const uint8_t *bt_ctrl_cmds_supported(void);           /* 64-byte 1002 bitmap */
void bt_ctrl_cmds_features_p0(uint8_t out[8]);          /* masked LMP page 0 as reported by 1004 page 0 */

/* ---- H4 event packers (out must hold 258 bytes); return the H4 length including the 0x04 type byte ---- */
uint16_t bt_ctrl_pack_cc(uint8_t *out, uint16_t opcode, uint8_t status, const uint8_t *ret, uint8_t ret_len);
uint16_t bt_ctrl_pack_cs(uint8_t *out, uint16_t opcode, uint8_t status);

/* State the SYN replies read (owned by bt_ctrl). */
typedef struct {
    const uint8_t *bd_addr;      /* 6 bytes, wire order */
    const uint8_t *local_name;   /* 248 bytes */
    uint8_t ssp_host;            /* last 0C56 enabled SSP with s = 0 */
    uint8_t reset_status;        /* 0C03 only */
} bt_ctrl_cmd_ctx_t;

/* Build the local reply for `row` (effective action SYN or REJ, or a FWD row answered locally with `status`):
 *   SYN: the synthesized content (status from the row rules; 0C05/1004 apply their parameter rules)
 *   REJ / local failure: status `fail_status`, full reply length, echo per row, zeros elsewhere.
 * Returns the H4 length, 0 if the row needs no reply (0C35). */
uint16_t bt_ctrl_cmds_reply_syn(const bt_ctrl_cmd_row_t *row, const uint8_t *params, uint8_t plen,
                                const bt_ctrl_cmd_ctx_t *ctx, uint8_t *out);
uint16_t bt_ctrl_cmds_reply_fail(const bt_ctrl_cmd_row_t *row, uint16_t opcode, const uint8_t *params, uint8_t plen,
                                 uint8_t fail_status, uint8_t *out);

#endif /* BT_CTRL_CMDS_H */
