/* Host tests for bt_port/bt_ctrl.c against tests/fake_rom.c (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M1, step 3).
 *
 * Bluedroid side: every reply bt_ctrl delivers is checked against the framing checks Bluedroid applies before parsing
 * (replicated below, cited), and the controller.c start-up replies are fed to the REAL hci_packet_parser.c functions
 * (linked in through tests/bt_ctrl_parser_shim.c). Parser assert() failures are caught (SIGABRT -> siglongjmp).
 * SPDX-License-Identifier: MIT */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_ctrl.h"
#include "bt_ctrl_evt.h"
#include "bt_ctrl_parser_shim.h"
#include "fake_rom.h"

static int g_fail, g_checks;
static int t_quiet;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond) && !t_quiet) {                                                                                    \
            g_fail++;                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

/* ------------------------------------------------------------------------------------------------------------- */
/* fake platform                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------- */
static int t_crit, t_crit_max;
static bool t_stop;
static uint32_t t_wake_ctrl, t_wake_isr, t_wake_dlv, t_faults, t_fault_last;
static void p_enter(void) { t_crit++; if (t_crit > t_crit_max) t_crit_max = t_crit; }
/* Optional hook run after each lock release (outside the lock): another task popping a note in that gap. */
static void (*t_exit_hook)(void);
static void p_exit(void)
{
    t_crit--;
    if (t_crit == 0 && t_exit_hook != NULL) {
        void (*h)(void) = t_exit_hook;
        t_exit_hook = NULL;                  /* not re-entered by the hook's own enter()/exit() */
        h();
        t_exit_hook = h;
    }
}
static void p_wake_ctrl(void) { t_wake_ctrl++; }
static void p_wake_isr(void) { t_wake_isr++; }
/* Plan §3 rule (e): wake_dlv and fault are FreeRTOS calls, never made while the ROM lock (PRIMASK) is held. */
static void p_wake_dlv(void)
{
    t_wake_dlv++;
    CHECK(fake_rom.lock_depth == 0u, "wake_dlv() called with the ROM lock held");
}
static bool p_stop(void) { return t_stop; }
static void p_fault(uint32_t code)
{
    t_faults++;
    t_fault_last = code;
    CHECK(fake_rom.lock_depth == 0u, "fault() posted with the ROM lock held");
}
static const bt_ctrl_platform_t k_plat = { p_enter, p_exit, p_wake_ctrl, p_wake_isr, p_wake_dlv, p_stop, p_fault };

static const uint8_t k_addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };

/* ------------------------------------------------------------------------------------------------------------- */
/* fake Bluedroid host side                                                                                       */
/* ------------------------------------------------------------------------------------------------------------- */
#define MAX_PKTS 8192u
typedef struct {
    uint16_t len;
    uint8_t d[1100];
} pkt_t;
static pkt_t *t_rx;
static uint32_t t_rx_n, t_rx_rd;
static int t_recv_fail;
static int t_recv_ok_left = -1;     /* >= 0: that many more deliveries succeed, then notify_host_recv fails */
static uint32_t t_send_avail, t_recv_calls;
static void (*t_cb_hook)(void);

/* Bluedroid's checks before an inbound packet is processed (silent drops otherwise):
 *   hci_hal_h4.c:462-466 len >= 1; :470-483 type 0x3E dropped; :484-491 type in {2,3,4}; :492-503 preamble;
 *   :504-523 length field == remaining; hci_layer.c:581 CC params >= 3, :614 CS params >= 4;
 *   hci_hal_h4.c:668-672,703 LE meta event never; packet_fragmenter.c:221-226 PB 10 start / 01 continuation;
 *   0C33 host ACL length 1021 (contract §7.1). */
static int h4_rx_valid(const uint8_t *d, uint16_t len)
{
    if (len < 1u) return 0;
    if (d[0] == BT_H4_EVT) {
        if (len < 3u) return 0;
        if (d[2] != len - 3u) return 0;
        if (d[1] == BT_HCI_EVT_LE) return 0;
        if (d[1] == BT_HCI_EVT_CC && d[2] < 3u) return 0;
        if (d[1] == BT_HCI_EVT_CS && d[2] < 4u) return 0;
        if ((d[1] == BT_HCI_EVT_CC && d[3] == 0u) || (d[1] == BT_HCI_EVT_CS && d[4] == 0u)) return 0; /* window 1 */
        return 1;
    }
    if (d[0] == BT_H4_ACL) {
        if (len < 5u) return 0;
        uint16_t dl = (uint16_t)(d[3] | (d[4] << 8));
        uint8_t pb = (uint8_t)((d[2] >> 4) & 3u);
        return dl == len - 5u && dl <= 1021u && (pb == 1u || pb == 2u);
    }
    return 0;
}

static int h_recv(uint8_t *d, uint16_t len)
{
    t_recv_calls++;
    CHECK(bt_ctrl_dlv_busy(), "notify_host_recv called without dlv_busy");
    CHECK(t_crit == 0, "host callback called inside a critical section");
    if (t_cb_hook != NULL) {
        void (*h)(void) = t_cb_hook;
        t_cb_hook = NULL;
        h();
    }
    if (t_recv_fail > 0) {
        t_recv_fail--;
        return -1;
    }
    if (t_recv_ok_left == 0) {
        return -1;
    }
    if (t_recv_ok_left > 0) {
        t_recv_ok_left--;
    }
    CHECK(h4_rx_valid(d, len), "invalid H4 packet delivered (type %02X len %u)", d[0], len);
    if (t_rx_n < MAX_PKTS) {
        t_rx[t_rx_n].len = len;
        memcpy(t_rx[t_rx_n].d, d, len < sizeof(t_rx[0].d) ? len : sizeof(t_rx[0].d));
        t_rx_n++;
    }
    return 0;
}

static void h_send_avail(void)
{
    t_send_avail++;
    CHECK(bt_ctrl_dlv_busy(), "notify_host_send_available called outside bt_dlv");
    CHECK(t_crit == 0, "send-available called inside a critical section");
}
static const bt_ctrl_host_cb_t k_host = { h_send_avail, h_recv };

static const pkt_t *next_pkt(void)
{
    return t_rx_rd < t_rx_n ? &t_rx[t_rx_rd++] : NULL;
}

static uint32_t pending_pkts(void)
{
    return t_rx_n - t_rx_rd;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* parser guard                                                                                                   */
/* ------------------------------------------------------------------------------------------------------------- */
static sigjmp_buf t_jmp;
static volatile sig_atomic_t t_in_parse;
static void on_abrt(int sig)
{
    if (t_in_parse) {
        siglongjmp(t_jmp, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#define PARSE_OK(expr)                                                                                                \
    do {                                                                                                              \
        t_in_parse = 1;                                                                                               \
        if (sigsetjmp(t_jmp, 1) == 0) {                                                                               \
            expr;                                                                                                     \
            t_in_parse = 0;                                                                                           \
        } else {                                                                                                      \
            t_in_parse = 0;                                                                                           \
            shim_free_last();                                                                                         \
            CHECK(0, "Bluedroid parser asserted on: %s", #expr);                                                      \
        }                                                                                                             \
    } while (0)
#define PARSE_ASSERTS(expr)                                                                                           \
    do {                                                                                                              \
        volatile int asserted_ = 0;                                                                                          \
        t_in_parse = 1;                                                                                               \
        if (sigsetjmp(t_jmp, 1) == 0) {                                                                               \
            expr;                                                                                                     \
        } else {                                                                                                      \
            asserted_ = 1;                                                                                            \
            shim_free_last();                                                                                         \
        }                                                                                                             \
        t_in_parse = 0;                                                                                               \
        CHECK(asserted_, "expected a Bluedroid parser assert on: %s", #expr);                                         \
    } while (0)

/* ------------------------------------------------------------------------------------------------------------- */
/* harness helpers                                                                                                */
/* ------------------------------------------------------------------------------------------------------------- */
static void setup(void)
{
    fake_rom_reset();
    fake_rom.stop_flag = &t_stop;
    t_stop = false;
    t_crit = t_crit_max = 0;
    t_wake_ctrl = t_wake_isr = t_wake_dlv = t_faults = t_fault_last = 0u;
    t_rx_n = t_rx_rd = 0u;
    t_recv_fail = 0;
    t_recv_ok_left = -1;
    t_send_avail = t_recv_calls = 0u;
    t_cb_hook = NULL;
    CHECK(bt_ctrl_init(&fake_rom_if, &k_plat, k_addr) == 0, "opcode/event table validation failed");
    CHECK(bt_ctrl_open() == 0, "open");
    CHECK(bt_ctrl_register_host_callback(&k_host) == 0, "register_host_callback must return ESP_OK");
}

static void pump(void)
{
    for (int i = 0; i < 4000; i++) {
        uint32_t more = bt_ctrl_run();
        int d = bt_ctrl_dlv_run();
        if (more == 0u && d == BT_CTRL_DLV_IDLE && fake_rom_queue_len() == 0u) {
            /* one more pass: a delivery may have freed space bt_ctrl was waiting for */
            if (bt_ctrl_run() == 0u && bt_ctrl_dlv_run() == BT_CTRL_DLV_IDLE && fake_rom_queue_len() == 0u) {
                return;
            }
        }
    }
    CHECK(0, "pump did not quiesce");
}

static uint16_t h4_cmd(uint8_t *buf, uint16_t op, const uint8_t *prm, uint8_t plen)
{
    buf[0] = BT_H4_CMD;
    buf[1] = (uint8_t)op;
    buf[2] = (uint8_t)(op >> 8);
    buf[3] = plen;
    if (plen != 0u) {
        memcpy(&buf[4], prm, plen);
    }
    return (uint16_t)(4u + plen);
}

static void send_cmd(uint16_t op, const uint8_t *prm, uint8_t plen)
{
    uint8_t buf[300];
    uint16_t n = h4_cmd(buf, op, prm, plen);
    CHECK(bt_ctrl_check_send_available(), "no TX slot for command %04X", op);
    bt_ctrl_send(buf, n);
}

static void send_acl(uint16_t handle, uint8_t pb, uint16_t len, uint8_t seed)
{
    static uint8_t buf[1100];
    buf[0] = BT_H4_ACL;
    uint16_t hdr = (uint16_t)(handle | ((uint16_t)pb << 12));
    buf[1] = (uint8_t)hdr;
    buf[2] = (uint8_t)(hdr >> 8);
    buf[3] = (uint8_t)len;
    buf[4] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) {
        buf[5 + i] = (uint8_t)(seed + i);
    }
    CHECK(bt_ctrl_check_send_available(), "no TX slot for ACL");
    bt_ctrl_send(buf, (uint16_t)(5u + len));
}

static void host_nocp(uint16_t handle, uint16_t count)
{
    uint8_t p[5] = { 1, (uint8_t)handle, (uint8_t)(handle >> 8), (uint8_t)count, (uint8_t)(count >> 8) };
    send_cmd(0x0C35u, p, 5u);
}

static int is_reply(const pkt_t *p, uint16_t op)
{
    if (p == NULL || p->d[0] != BT_H4_EVT) return 0;
    if (p->d[1] == BT_HCI_EVT_CC) return (uint16_t)(p->d[4] | (p->d[5] << 8)) == op;
    if (p->d[1] == BT_HCI_EVT_CS) return (uint16_t)(p->d[5] | (p->d[6] << 8)) == op;
    return 0;
}

/* Send one command, pump, and return its (single) reply. */
static const pkt_t *roundtrip(uint16_t op, const uint8_t *prm, uint8_t plen)
{
    uint32_t before = t_rx_n;
    send_cmd(op, prm, plen);
    pump();
    const pkt_t *r = NULL;
    uint32_t replies = 0u;
    for (uint32_t i = before; i < t_rx_n; i++) {
        if (is_reply(&t_rx[i], op)) {
            r = &t_rx[i];
            replies++;
        }
    }
    CHECK(replies == 1u, "opcode %04X: %u replies (want exactly 1)", op, replies);
    t_rx_rd = t_rx_n;
    return r;
}

static uint8_t status_of(const pkt_t *p)
{
    return p->d[1] == BT_HCI_EVT_CC ? p->d[6] : p->d[3];
}

/* controller.c start_up() order (device/controller.c:251-478), each reply through the real parsers. */
static void bring_up(void)
{
    const pkt_t *r;
    uint8_t one = 1u;
    r = roundtrip(0x0C03u, NULL, 0u);
    if (r) PARSE_OK(shim_parse_generic(r->d, r->len));
    if (r) CHECK(status_of(r) == 0u, "reset status");
    r = roundtrip(0x1005u, NULL, 0u);
    uint16_t acl_len = 0, acl_cnt = 0, sco_cnt = 0;
    uint8_t sco_len = 0;
    if (r) PARSE_OK(shim_parse_buffer_size(r->d, r->len, &acl_len, &acl_cnt, &sco_len, &sco_cnt));
    CHECK(acl_len == 1021u && acl_cnt == BT_CTRL_H2C_ACL_CREDITS && sco_len == 0u && sco_cnt == 0u, "1005 content");
    r = roundtrip(0x0C31u, &one, 1u);
    if (r) PARSE_OK(shim_parse_generic(r->d, r->len));
    uint8_t hbs[7] = { 0xFD, 0x03, 0xFF, 0x04, 0x00, 0x0A, 0x00 };   /* controller.c:274-288 values */
    r = roundtrip(0x0C33u, hbs, 7u);
    if (r) PARSE_OK(shim_parse_generic(r->d, r->len));
    r = roundtrip(0x1001u, NULL, 0u);
    uint8_t hv = 0, lv = 0;
    uint16_t hr = 0, man = 0xFFFF, sub = 0;
    if (r) PARSE_OK(shim_parse_local_version(r->d, r->len, &hv, &hr, &lv, &man, &sub));
    CHECK(man == 0x0060u && man != 0x000Fu && hv == 6u && lv == 6u, "1001 content");
    r = roundtrip(0x1009u, NULL, 0u);
    uint8_t a[6] = { 0 };
    if (r) PARSE_OK(shim_parse_bd_addr(r->d, r->len, a));
    int rev_ok = 1;   /* STREAM_TO_BDADDR stores the wire (LSB-first) address reversed */
    for (int i = 0; i < 6; i++) rev_ok &= a[i] == k_addr[5 - i];
    CHECK(rev_ok, "1009 address");
    r = roundtrip(0x1002u, NULL, 0u);
    uint8_t sc[64] = { 0 };
    if (r) PARSE_OK(shim_parse_supported_cmds(r->d, r->len, sc));
    /* 0C84 stays clear (D:1027). 041C is FWD since the phone-path promotion: Bluedroid then reads the peer's page 1
     * (btm_acl.c:1286) and learns SSP host support (btm_sec_set_peer_sec_caps) instead of marking it legacy. */
    CHECK((sc[45] & 0x80u) == 0u && (sc[2] & 0x40u) != 0u, "1002: 0C84 bit clear, 041C bit set");
    uint8_t page = 0, max_page = 0, pages[3][8];
    uint8_t pg = 0u;
    r = roundtrip(0x1004u, &pg, 1u);
    if (r) PARSE_OK(shim_parse_ext_features(r->d, r->len, &page, &max_page, pages));
    CHECK(page == 0u && max_page == 1u, "1004 page 0 header");
    CHECK((pages[0][6] & 0x08u) != 0u, "page 0 must keep SSP (bit 51) so controller.c sends 0C56");
    CHECK((pages[0][0] & 0x20u) == 0u && (pages[0][0] & 0x80u) == 0u, "no role switch / sniff");
    r = roundtrip(0x0C56u, &one, 1u);
    if (r) PARSE_OK(shim_parse_generic(r->d, r->len));
    if (r) CHECK(status_of(r) == 0u, "0C56 status");
    /* controller.c:349-361: pages 1..max_page while page < 3; max_page = 1 so page 2 (SC, 0C7A) is never read. */
    pg = 1u;
    r = roundtrip(0x1004u, &pg, 1u);
    if (r) PARSE_OK(shim_parse_ext_features(r->d, r->len, &page, &max_page, pages));
    CHECK(page == 1u && max_page == 1u && pages[1][0] == BT_CTRL_FEAT_P1_SSP_HOST, "1004 page 1 (SSP host bit)");
    CHECK((pages[1][0] & 0x08u) == 0u, "no SC host support on page 1");
    uint8_t mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x9F, 0x00, 0x20 };
    r = roundtrip(0x0C01u, mask, 8u);
    if (r) PARSE_OK(shim_parse_generic(r->d, r->len));
    if (r) CHECK(status_of(r) == 0u, "0C01 status");
}

static void link_up(uint32_t link)
{
    fake_rom_link(link, true);
}

static bt_ctrl_debug_t dbg(void)
{
    bt_ctrl_debug_t d;
    bt_ctrl_get_debug(&d);
    return d;
}

static bt_ctrl_stats_t stats(void)
{
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    return s;
}

static void shutdown_and_check(void)
{
    bt_ctrl_shutdown_begin();
    bt_ctrl_run();
    CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_OK, "shutdown status %d", bt_ctrl_shutdown_status());
    CHECK(fake_rom.kept_at_stop == 0u, "bt_ctrl still owned %u ROM messages at stop()", fake_rom.kept_at_stop);
    CHECK(fake_rom.em_rx_held_at_stop == 0u, "bt_ctrl still held %u EM RX buffers at stop()",
          fake_rom.em_rx_held_at_stop);
    CHECK(fake_rom.errors == 0u, "fake ROM contract error: %s", fake_rom.first_error);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 1. table validation                                                                                            */
/* ------------------------------------------------------------------------------------------------------------- */
static void test_table(void)
{
    setup();
    uint32_t n = 0;
    const bt_ctrl_cmd_row_t *rows = bt_ctrl_cmds_table(&n);
    CHECK(n == 78u, "table has %u rows (doc: 78 Classic opcodes)", n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t eff = bt_ctrl_cmds_eff(&rows[i]);
        CHECK(!(rows[i].cls == BTC_M && eff == BTC_REJ), "class M row %04X is REJ", rows[i].op);
        if (eff == BTC_FWD) {
            CHECK(bt_ctrl_evt_reply_opcode(rows[i].rom_rsp) == rows[i].op, "FWD %04X reply id unmapped", rows[i].op);
            CHECK(bt_ctrl_cmds_fwd_layout(rows[i].op, NULL), "FWD %04X has no ROM layout", rows[i].op);
        }
    }
    CHECK(bt_ctrl_cmds_lookup(0xFC57u) != NULL && bt_ctrl_cmds_lookup(0xFC57u)->act == BTC_REJ, "vendor OGF REJ");
    CHECK(bt_ctrl_cmds_lookup(0x0C5Fu) == NULL, "0C5F is not linked: closed-table default");
    CHECK(shim_l2cap_host_fc_acl_bufs() == BT_CTRL_C2H_ACL_CREDITS, "bt_port_prefix.h L2CAP_HOST_FC_ACL_BUFS mismatch");
    CHECK(shim_l2cap_host_fc_acl_len() == BT_CTRL_C2H_ACL_LEN, "bt_port_prefix.h L2CAP_HOST_FC_ACL_DATA_LEN mismatch");
    uint8_t p0[8];
    bt_ctrl_cmds_features_p0(p0);
    static const uint8_t want[8] = { 0x1F, 0x06, 0xC4, 0x0A, 0x98, 0x1D, 0x18, 0x85 };   /* §3.6, no-EIR variant */
    CHECK(memcmp(p0, want, 8u) == 0, "page 0 features");
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 2. golden vector per opcode-table row                                                                          */
/* ------------------------------------------------------------------------------------------------------------- */
static const uint8_t k_peer[6] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 };

static uint8_t gen_params(uint16_t op, uint8_t *p)
{
    memset(p, 0, 255);
    switch (op) {
    case 0x0C03: case 0x1005: case 0x1001: case 0x1009: case 0x1002: case 0x0C14: case 0x0402: case 0x0404:
    case 0x0C57: case 0x1003: case 0x0C58:
        return 0;
    case 0x0C31: p[0] = 1; return 1;
    case 0x0C33: { static const uint8_t h[7] = { 0xFD, 0x03, 0xFF, 0x04, 0x00, 0x0A, 0x00 }; memcpy(p, h, 7); return 7; }
    case 0x1004: p[0] = 0; return 1;
    case 0x0C56: p[0] = 1; return 1;
    case 0x0C01: memset(p, 0xFF, 8); return 8;
    case 0x0C18: p[0] = 0x00; p[1] = 0x20; return 2;
    case 0x0C24: p[0] = 0x0C; p[1] = 0x02; p[2] = 0x5A; return 3;
    case 0x080F: return 2;
    case 0x0C13: memcpy(p, "open_fw", 7); return 248;
    case 0x0C1A: p[0] = 3; return 1;
    case 0x0C05: p[0] = 1; p[1] = 0; return 2;
    case 0x0C35: p[0] = 1; p[1] = 0x80; p[2] = 0; p[3] = 0; p[4] = 0; return 5;
    case 0x0C12: memcpy(p, k_peer, 6); p[6] = 1; return 7;
    case 0x0406: p[0] = 0x80; p[1] = 0x00; p[2] = 0x13; return 3;
    case 0x0409: memcpy(p, k_peer, 6); p[6] = 1; return 7;
    case 0x040A: memcpy(p, k_peer, 6); p[6] = 0x0F; return 7;
    case 0x040B: memcpy(p, k_peer, 6); memset(p + 6, 0x5C, 16); return 22;
    case 0x040D: memcpy(p, k_peer, 6); p[6] = 4; memset(p + 7, '0', 16); return 23;
    case 0x042B: memcpy(p, k_peer, 6); p[6] = 1; p[7] = 0; p[8] = 3; return 9;
    case 0x0405: memcpy(p, k_peer, 6); p[6] = 0x18; p[7] = 0xCC; return 13;
    case 0x0419: case 0x042E: memcpy(p, k_peer, 6); return 10;
    case 0x0430: memcpy(p, k_peer, 6); return 38;
    case 0x0434: memcpy(p, k_peer, 6); p[6] = 0x18; return 7;
    case 0x040C: case 0x040E: case 0x042C: case 0x042D: case 0x041A: case 0x0408: case 0x042F: case 0x0433:
        memcpy(p, k_peer, 6); return 6;
    case 0x080B: memcpy(p, k_peer, 6); p[6] = 1; return 7;
    case 0x0401: p[0] = 0x33; p[1] = 0x8B; p[2] = 0x9E; p[3] = 8; return 5;
    case 0x0403: return 9;
    case 0x0C52: return 241;
    case 0x0C1C: case 0x0C1E: return 4;
    case 0x0C3A: p[0] = 1; p[1] = 0x33; p[2] = 0x8B; p[3] = 0x9E; return 4;
    case 0x0C3F: return 10;
    case 0x0807: p[0] = 0x80; return 20;
    case 0x0803: p[0] = 0x80; return 10;
    case 0x0801: case 0x0805: p[0] = 0x80; return 6;
    default:                                     /* first parameter is a connection handle */
        p[0] = 0x80; p[1] = 0x00; p[2] = 0x11; p[3] = 0x22; return 4;
    }
}

/* Expected reply computed from the documents, independently of bt_ctrl_cmds.c. */
static uint16_t golden(const bt_ctrl_cmd_row_t *r, const uint8_t *prm, uint8_t plen, uint8_t *out)
{
    uint16_t op = r->op;
    uint8_t lo = (uint8_t)op, hi = (uint8_t)(op >> 8);
    memset(out, 0, 260);
    switch (op) {     /* class M and SYN rows: literal vectors (BT_CTRL_OPCODE_TABLE §3, §4) */
    case 0x1005: { static const uint8_t g[] = { 4, 0x0E, 11, 1, 0x05, 0x10, 0, 0xFD, 0x03, 0, 4, 0, 0, 0 };
        memcpy(out, g, sizeof(g)); return sizeof(g); }
    case 0x1001: { static const uint8_t g[] = { 4, 0x0E, 12, 1, 0x01, 0x10, 0, 6, 0, 0, 6, 0x60, 0, 0, 0 };
        memcpy(out, g, sizeof(g)); return sizeof(g); }
    case 0x1009: { static const uint8_t g[] = { 4, 0x0E, 10, 1, 0x09, 0x10, 0 };
        memcpy(out, g, sizeof(g)); memcpy(out + 7, k_addr, 6); return 13; }
    case 0x1002: { static const uint8_t g[] = { 4, 0x0E, 68, 1, 0x02, 0x10, 0 };
        memcpy(out, g, sizeof(g));
        uint8_t *b = out + 7;  /* supported by the effective FWD/SYN/LOCAL rows of the default build */
        b[0] = 0x20; b[1] = 0xBF;
        /* the headphones rows (BT_CTRL_HP_GATE_LIST) are set only in a build that passes their gates */
        static const struct { uint16_t op; uint8_t bit; } hp[] = { { 0x0401, 0x01 }, { 0x0402, 0x02 },
                                                                   { 0x0405, 0x10 }, { 0x0408, 0x80 } };
        for (size_t k = 0; k < sizeof(hp) / sizeof(hp[0]); k++) {
            if (bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(hp[k].op)) == BTC_FWD) b[0] |= hp[k].bit;
        } b[2] = 0xF9; b[3] = 0x01; b[5] = 0xD0; b[6] = 0x81; b[7] = 0xA3; b[9] = 0x02;
        b[10] = 0xE0; b[14] = 0xD8; b[15] = 0x02; b[17] = 0x40; b[18] = 0x80; b[19] = 0x03; b[20] = 0x08;
        return 7 + 64; }
    case 0x1004: { static const uint8_t g[] = { 4, 0x0E, 14, 1, 0x04, 0x10, 0, 0, 1,
                                                0x1F, 0x06, 0xC4, 0x0A, 0x98, 0x1D, 0x18, 0x85 };
        memcpy(out, g, sizeof(g)); return sizeof(g); }
    case 0x0C14: { static const uint8_t g[] = { 4, 0x0E, 252, 1, 0x14, 0x0C, 0 };
        memcpy(out, g, sizeof(g)); return 7 + 248; }   /* name cache empty after Reset */
    case 0x0C12: { static const uint8_t g[] = { 4, 0x0E, 6, 1, 0x12, 0x0C, 0, 0, 0 };
        memcpy(out, g, sizeof(g)); return sizeof(g); }
    case 0x0C35:
        return 0;
    default:
        break;
    }
    uint8_t eff = bt_ctrl_cmds_eff(r);
    uint8_t status = eff == BTC_REJ ? 0x01 : 0x00;
    if (eff == BTC_FWD && op == 0x0402) status = 0x0C;   /* ROM 0x3F2BC with no inquiry running (map §2) */
    if (eff == BTC_FWD && op == 0x0408) status = 0x02;   /* ROM 0x32734: no link for the address (map §2) */
    if (r->rsp == BTC_CS) {
        uint8_t g[7] = { 4, 0x0F, 4, status, 1, lo, hi };
        memcpy(out, g, 7);
        return 7;
    }
    out[0] = 4; out[1] = 0x0E; out[2] = r->plen; out[3] = 1; out[4] = lo; out[5] = hi; out[6] = status;
    uint8_t echo = r->echo == BTC_ECHO_BDADDR ? 6 : r->echo == BTC_ECHO_HANDLE ? 2 : 0;
    if (echo > plen) echo = plen;
    memcpy(out + 7, prm, echo);                    /* REJ echo; FWD: the fake ROM echoes BD_ADDR like the ROM */
    return (uint16_t)(3u + r->plen);
}

static void test_golden_vectors(void)
{
    uint32_t n = 0;
    const bt_ctrl_cmd_row_t *rows = bt_ctrl_cmds_table(&n);
    static const bt_ctrl_cmd_row_t vendor = { 0xFC57u, BTC_O, BTC_REJ, BTC_REJ, BTC_CC, 4u, BTC_ECHO_NONE, 0, 0,
                                              0xFF, 0, BTC_EXP_NONE };
    for (uint32_t i = 0; i <= n; i++) {
        const bt_ctrl_cmd_row_t *r = i < n ? &rows[i] : &vendor;
        setup();
        link_up(0);
        uint8_t one = 1;
        if (r->op != 0x0C03u) {
            (void)roundtrip(0x0C03u, NULL, 0u);
            (void)roundtrip(0x0C31u, &one, 1u);
        }
        uint8_t prm[255], want[260];
        uint8_t plen = gen_params(r->op, prm);
        uint16_t wlen = golden(r, prm, plen, want);
        uint32_t before = t_rx_n;
        uint32_t cmds_before = fake_rom.n_cmds;
        fake_rom.page_hold = true;                     /* 0405: the CS only; the page outcome is test_hp_connect's */
        send_cmd(r->op, prm, plen);
        pump();
        uint32_t got = t_rx_n - before;
        if (wlen == 0u) {
            CHECK(got == 0u, "%04X must produce no reply (got %u packets)", r->op, got);
            CHECK(fake_rom.n_cmds == cmds_before, "%04X must not reach the ROM", r->op);
            continue;
        }
        CHECK(got == 1u, "%04X: %u packets (want 1)", r->op, got);
        if (got < 1u) continue;
        const pkt_t *p = &t_rx[before];
        CHECK(p->len == wlen && memcmp(p->d, want, wlen) == 0, "%04X golden vector mismatch (len %u want %u)",
              r->op, p->len, wlen);
        if (p->len != wlen || memcmp(p->d, want, wlen) != 0) {
            printf("   got :");
            for (uint16_t k = 0; k < p->len && k < 24; k++) printf(" %02X", p->d[k]);
            printf("\n   want:");
            for (uint16_t k = 0; k < wlen && k < 24; k++) printf(" %02X", want[k]);
            printf("\n");
        }
        CHECK(p->d[2] == (r->rsp == BTC_CS ? 4u : r->plen), "%04X reply length vs table plen", r->op);
        uint8_t eff = bt_ctrl_cmds_eff(r);
        if (eff == BTC_FWD) {
            CHECK(fake_rom.n_cmds == cmds_before + 1u && fake_rom.cmds[cmds_before].id == r->rom_req,
                  "%04X not forwarded as KE id %04X", r->op, r->rom_req);
            if (fake_rom.n_cmds == cmds_before + 1u) {
                const fake_cmd_rec_t *c = &fake_rom.cmds[cmds_before];
                uint8_t min = 0;
                CHECK(bt_ctrl_cmds_fwd_layout(r->op, &min), "%04X FWD without a layout", r->op);
                CHECK(c->len == (plen > min ? plen : min) && memcmp(c->data, prm, plen) == 0,
                      "%04X ROM payload", r->op);
                uint16_t dst_task = (uint16_t)(c->dst & 0xFFu);
                CHECK(dst_task == BT_ROM_MSG_DEST(r->rom_req), "%04X ROM destination task %04X", r->op, c->dst);
            }
        } else {
            CHECK(fake_rom.n_cmds == cmds_before, "%04X (non-FWD) reached the ROM", r->op);
        }
        if (r->cls == BTC_M) {
            CHECK(status_of(p) == 0u, "class M %04X status %02X", r->op, status_of(p));
            switch (r->op) {   /* mandatory-success proof with the real Bluedroid parsers */
            case 0x1005: { uint16_t a, b, d; uint8_t c; PARSE_OK(shim_parse_buffer_size(p->d, p->len, &a, &b, &c, &d)); break; }
            case 0x1001: { uint8_t a, c; uint16_t b, d, e; PARSE_OK(shim_parse_local_version(p->d, p->len, &a, &b, &c, &d, &e)); break; }
            case 0x1009: { uint8_t a[6]; PARSE_OK(shim_parse_bd_addr(p->d, p->len, a)); break; }
            case 0x1002: { uint8_t s[64]; PARSE_OK(shim_parse_supported_cmds(p->d, p->len, s)); break; }
            case 0x1004: { uint8_t pg, mp, f[3][8]; PARSE_OK(shim_parse_ext_features(p->d, p->len, &pg, &mp, f));
                           CHECK(pg == 0u, "page_number"); break; }
            default: PARSE_OK(shim_parse_generic(p->d, p->len)); break;
            }
        }
        bt_ctrl_stats_t s = stats();
        CHECK(s.cmd_unknown == 0u, "%04X hit the unknown-opcode default", r->op);
        CHECK(fake_rom.errors == 0u, "%04X fake ROM error: %s", r->op, fake_rom.first_error);
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 3. start-up through the real parsers + parameter rules                                                         */
/* ------------------------------------------------------------------------------------------------------------- */
static void test_startup_and_param_rules(void)
{
    setup();
    bring_up();
    bt_ctrl_stats_t s = stats();
    CHECK(s.c2h_config_mismatch == 0u && s.cmd_unknown == 0u && s.invariant == 0u, "start-up counters");
    CHECK(dbg().c2h_fc == 1u && dbg().c2h_credits == 4u && dbg().c2h_len == 1021u, "0C31/0C33 applied");
    /* page 2 is never requested by controller.c (max_page = 1); if it were, the parser must not assert. */
    uint8_t pg = 2, page, max_page, pages[3][8];
    const pkt_t *r = roundtrip(0x1004u, &pg, 1u);
    if (r) { CHECK(status_of(r) == 0x12u && r->d[2] == 14u, "page 2: s=0x12, full length");
             PARSE_OK(shim_parse_ext_features(r->d, r->len, &page, &max_page, pages)); }
    /* name cache: 0C13 FWD success updates 0C14 */
    uint8_t name[248] = { 0 };
    memcpy(name, "Recorder", 8);
    r = roundtrip(0x0C13u, name, 248u);
    r = roundtrip(0x0C14u, NULL, 0u);
    if (r) CHECK(r->len == 255u && memcmp(&r->d[7], name, 248u) == 0, "0C14 returns the 0C13 name");
    /* 0C05: exact only for (01,00) and (00) */
    uint8_t ef[3] = { 0x01, 0x00, 0x00 };
    r = roundtrip(0x0C05u, ef, 1u);
    if (r) CHECK(status_of(r) == 0x01u, "0C05 (01) alone is REJ");
    ef[0] = 0;
    r = roundtrip(0x0C05u, ef, 1u);
    if (r) CHECK(status_of(r) == 0x00u, "0C05 (00) clear is SYN");
    /* 0C33 mismatch: still s=0 (class M), min() enforced, counter */
    uint8_t hbs[7] = { 0x64, 0x00, 0xFF, 0x08, 0x00, 0x0A, 0x00 };
    r = roundtrip(0x0C33u, hbs, 7u);
    if (r) CHECK(status_of(r) == 0u, "0C33 mismatch status");
    CHECK(dbg().c2h_len == 100u && dbg().c2h_credits == 4u && stats().c2h_config_mismatch == 1u, "0C33 min()");
    /* unknown opcode: CS 0x01 + counter; parser-negative: a short 1005 reply asserts in Bluedroid */
    r = roundtrip(0x0C5Fu, NULL, 0u);
    if (r) CHECK(r->d[1] == BT_HCI_EVT_CS && status_of(r) == 0x01u, "unknown opcode -> CS 0x01");
    CHECK(stats().cmd_unknown == 1u && stats().unknown_opcode_last == 0x0C5Fu, "unknown counter");
    printf("  (an expected Bluedroid parser assert message follows)\n");
    fflush(stdout);
    uint8_t bad[7] = { 4, 0x0E, 4, 1, 0x05, 0x10, 0 };
    uint16_t a, b, d;
    uint8_t c;
    PARSE_ASSERTS(shim_parse_buffer_size(bad, 7u, &a, &b, &c, &d));
    /* FWD before the controller runs (no Reset yet) is answered locally */
    setup();
    uint8_t one = 1;
    r = roundtrip(0x0C1Au, &one, 1u);
    if (r) CHECK(status_of(r) == BT_HCI_ERR_CMD_DISALLOWED, "FWD before Reset");
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 4. TX admission                                                                                                */
/* ------------------------------------------------------------------------------------------------------------- */
static void test_tx_admission(void)
{
    /* empty queue, check-then-no-send, full ring, send-available via bt_dlv */
    setup();
    bring_up();
    link_up(0);
    CHECK(bt_ctrl_check_send_available(), "empty ring must admit");
    for (int i = 0; i < 100; i++) (void)bt_ctrl_check_send_available();   /* pure query: no reservation */
    CHECK(dbg().tx_free == 3u, "check reserved something");
    pump();
    CHECK(dbg().tx_free == 3u && pending_pkts() == 0u, "check without send leaked");
    uint32_t sa0 = t_send_avail;
    send_acl(0x80, 2, 100, 1);
    send_acl(0x80, 1, 100, 2);
    send_acl(0x80, 1, 100, 3);
    CHECK(!bt_ctrl_check_send_available(), "full ring must refuse");
    pump();
    CHECK(bt_ctrl_check_send_available() && dbg().tx_free == 3u, "ring drained");
    CHECK(t_send_avail == sa0 + 1u, "exactly one send-available after full -> not full (got %u)", t_send_avail - sa0);
    CHECK(fake_rom.n_acl == 3u && fake_rom.acl[0].len == 100u && fake_rom.acl[0].pb_bc == 2u &&
          fake_rom.acl[1].pb_bc == 1u && fake_rom.acl[2].data[0] == 3u, "ACL reached the ROM intact and in order");
    /* send on a full ring = contract violation, never a silent overwrite */
    send_acl(0x80, 2, 10, 4);
    send_acl(0x80, 2, 10, 5);
    send_acl(0x80, 2, 10, 6);
    uint8_t buf[16];
    uint16_t n = h4_cmd(buf, 0x0C1Au, (const uint8_t *)"\x03", 1);
    bt_ctrl_send(buf, n);
    CHECK(stats().tx_contract_violation == 1u && t_faults == 1u && t_fault_last == BT_CTRL_FAULT_HOST_CONTRACT,
          "send on a full ring");
    pump();
    /* malformed / SCO H4 from the host */
    uint8_t sco[4] = { 0x03, 0x80, 0x00, 0x00 };
    bt_ctrl_send(sco, 4);
    uint8_t shortc[4] = { 0x01, 0x1A, 0x0C, 0x05 };
    bt_ctrl_send(shortc, 4);
    CHECK(stats().tx_malformed == 2u, "malformed H4 counted");
    pump();
    CHECK(dbg().tx_free == 3u && fake_rom.errors == 0u, "no slot leaked by rejected packets");
    shutdown_and_check();

    /* zero command credits: a second ordinary command waits for the ROM reply to the first */
    setup();
    bring_up();
    fake_rom.hold_replies = true;
    uint8_t one = 1, cod[3] = { 1, 2, 3 };
    send_cmd(0x0C1Au, &one, 1);
    pump();
    CHECK(dbg().rom_cmd_credits == 0u && dbg().fwd_pending == 1u && dbg().evt_resv == 1u, "credit consumed");
    uint32_t cmds_held = fake_rom.n_cmds;
    send_cmd(0x0C24u, cod, 3);
    pump();
    CHECK(dbg().tx_free == 2u && fake_rom.n_cmds == cmds_held, "0C24 must wait for a ROM command credit");
    fake_rom.hold_replies = false;
    fake_rom_release_held();
    pump();
    const pkt_t *p1 = next_pkt(), *p2 = next_pkt();
    CHECK(is_reply(p1, 0x0C1Au) && is_reply(p2, 0x0C24u), "replies in command order");
    CHECK(dbg().rom_cmd_credits == 1u && dbg().tx_free == 3u, "credit restored");
    shutdown_and_check();

    /* zero ACL credits with a queued command: the command must still go out */
    setup();
    bring_up();
    link_up(0);
    fake_rom.auto_ack_acl = false;
    for (int i = 0; i < 4; i++) {
        send_acl(0x80, 2, 50, (uint8_t)i);
        pump();
    }
    CHECK(dbg().h2c_inflight == 4u && fake_rom.acl_in_air == 4u, "4 ACL in flight");
    send_acl(0x80, 2, 50, 9);           /* over the 1005 window: stays in the ring */
    pump();
    CHECK(dbg().tx_free == 2u && fake_rom.n_acl == 4u, "5th ACL credit-blocked");
    const pkt_t *r = roundtrip(0x0C1Au, &one, 1);
    CHECK(r != NULL && status_of(r) == 0u, "command overtook the credit-blocked ACL");
    CHECK(dbg().tx_free == 2u, "ACL still blocked");
    fake_rom_ack_acl(4);
    pump();
    const pkt_t *nocp = next_pkt();
    CHECK(nocp != NULL && nocp->d[1] == BT_HCI_EVT_NOCP && nocp->d[3] == 1u && nocp->d[4] == 0x80u &&
          nocp->d[6] == 4u, "NOCP (0x80, 4) forwarded to the host");
    CHECK(fake_rom.n_acl == 5u && dbg().h2c_inflight == 1u && dbg().tx_free == 3u, "credit return resumed TX ACL");
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 5. RX: saturation, events with zero ACL credits, credit return, fragmentation                                  */
/* ------------------------------------------------------------------------------------------------------------- */
static void inject_conn_req(uint8_t tag)
{
    uint8_t p[10] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, tag, 0x0C, 0x02, 0x5A, 0x01 };
    fake_rom_inject(0xE457u, p, 10);
}

static void test_rx_paths(void)
{
    /* event saturation: general area 6 slots, 4 deferred (KEEP), then overflow -> fault */
    setup();
    bring_up();
    uint32_t mark = t_rx_n;
    t_recv_fail = 1000000;                       /* bt_dlv cannot deliver */
    for (int i = 0; i < 12; i++) inject_conn_req((uint8_t)i);
    for (int i = 0; i < 20; i++) { bt_ctrl_run(); (void)bt_ctrl_dlv_run(); }
    bt_ctrl_debug_t d = dbg();
    CHECK(d.evt_used == BT_CTRL_EVT_SLOTS - BT_CTRL_EVT_RESERVED && d.evt_deferred == BT_CTRL_EVT_DEFER,
          "event ring %u used, %u deferred", d.evt_used, d.evt_deferred);
    CHECK(stats().evt_overflow == 2u && t_faults == 1u && t_fault_last == BT_CTRL_FAULT_EVT_OVERFLOW,
          "overflow counted (%u), one fault posted (first: overflow)", stats().evt_overflow);
    CHECK(stats().fault_site == BT_CTRL_SITE_EVT_OVERFLOW && stats().fault_a == 0xE457u &&
          stats().fault_b == BT_CTRL_EVT_DEFER, "fault site %u a=0x%X b=%u, want overflow E457/%u",
          stats().fault_site, stats().fault_a, stats().fault_b, BT_CTRL_EVT_DEFER);
    CHECK(fake_rom_msgs_owned_by_ctrl() == BT_CTRL_EVT_DEFER, "deferred events are KEEP'd ROM messages");
    /* the general area is full and bt_dlv is stuck: a command (FWD and SYN) still gets its reserved reply slot */
    uint8_t one = 1u;
    send_cmd(0x0C1Au, &one, 1u);
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().evt_used == BT_CTRL_EVT_SLOTS - BT_CTRL_EVT_RESERVED + 1u && dbg().rom_cmd_credits == 1u,
          "FWD reply took a reserved slot");
    send_cmd(0x1005u, NULL, 0u);
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().evt_used == BT_CTRL_EVT_SLOTS && dbg().tx_free == BT_CTRL_TX_SLOTS, "SYN reply took the last slot");
    send_cmd(0x1001u, NULL, 0u);
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().tx_free == BT_CTRL_TX_SLOTS - 1u, "no reply slot: the command waits in the ring");
    t_recv_fail = 0;
    pump();
    uint32_t got = 0;
    int last = -1, ordered = 1;
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) {
        if (p->d[1] == 0x04u) { got++; if (p->d[8] <= last) ordered = 0; last = p->d[8]; }
    }
    CHECK(got == 10u && ordered, "10 events delivered in order (%u)", got);
    t_rx_rd = mark;
    uint32_t r1a = 0, r1005 = 0, r1001 = 0;
    while ((p = next_pkt()) != NULL) { r1a += is_reply(p, 0x0C1Au); r1005 += is_reply(p, 0x1005u); r1001 += is_reply(p, 0x1001u); }
    CHECK(r1a == 1u && r1005 == 1u && r1001 == 1u, "every command answered exactly once after the stall");
    CHECK(fake_rom_msgs_owned_by_ctrl() == 0u, "deferred messages freed");
    shutdown_and_check();

    /* ACL: window 4 x 1021, 8 EM RX buffers; events flow with zero credits; 0C35 resumes ACL */
    setup();
    bring_up();
    link_up(0);
    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) data[i] = (uint8_t)(i * 7);
    for (int i = 0; i < 8; i++) CHECK(fake_rom_inject_acl(0x80, 2, data, 200) == 0, "inject %d", i);
    CHECK(fake_rom_inject_acl(0x80, 2, data, 200) == -1, "9th RX: EM pool exhausted (baseband flow-stop)");
    pump();
    uint32_t acl = 0;
    while ((p = next_pkt()) != NULL) acl += p->d[0] == BT_H4_ACL;
    d = dbg();
    CHECK(acl == 4u && d.c2h_outstanding == 4u && d.held_acl == 4u && d.acl_used == 0u,
          "4 ACL delivered, 4 held (acl %u held %u)", acl, d.held_acl);
    CHECK(stats().acl_rx_credit_blocked != 0u, "credit gate engaged");
    inject_conn_req(1);
    fake_rom_inject(0xE478u, (const uint8_t *)"\x80\x00\x40\x1F", 4);
    pump();
    uint32_t evts = 0;
    acl = 0;
    while ((p = next_pkt()) != NULL) { evts += p->d[0] == BT_H4_EVT; acl += p->d[0] == BT_H4_ACL; }
    CHECK(evts == 2u && acl == 0u, "events delivered with zero ACL credits and an empty ACL ring");
    host_nocp(0x80, 4);
    pump();
    acl = 0;
    while ((p = next_pkt()) != NULL) { acl += p->d[0] == BT_H4_ACL; CHECK(p->d[0] != BT_H4_EVT || p->d[1] != 0x0E,
                                                                         "0C35 produced a reply"); }
    CHECK(acl == 4u && dbg().held_acl == 0u && fake_rom_em_rx_in_use() == 0u, "credit return resumed ACL");
    CHECK(stats().c2h_returned == 4u && stats().c2h_invalid == 0u, "0C35 accounted");
    host_nocp(0x80, 4);
    host_nocp(0x81, 1);
    pump();
    CHECK(stats().c2h_invalid == 1u && stats().c2h_returned == 8u, "unknown handle 0C35 counted, not answered");
    CHECK(pending_pkts() == 0u, "0C35 never answered");
    shutdown_and_check();

    /* fragmentation: credit checked before any fragment state changes */
    setup();
    bring_up();
    link_up(1);
    uint8_t hbs1[7] = { 0xFD, 0x03, 0xFF, 0x01, 0x00, 0x0A, 0x00 };   /* window of 1 */
    (void)roundtrip(0x0C33u, hbs1, 7u);
    CHECK(fake_rom_inject_acl(0x81, 2, data, 1024) == 0, "inject 1024");
    pump();
    p = next_pkt();
    CHECK(p != NULL && p->len == 5u + 1021u && ((p->d[2] >> 4) & 3u) == 2u && memcmp(&p->d[5], data, 1021) == 0,
          "first fragment 1021 B, PB=10");
    CHECK(next_pkt() == NULL && dbg().held_acl == 1u && dbg().c2h_outstanding == 1u, "second fragment waits");
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().held_acl == 1u && pending_pkts() == 0u && stats().acl_rx_frags == 1u,
          "no fragment state advanced without a credit");
    host_nocp(0x81, 1);
    pump();
    p = next_pkt();
    CHECK(p != NULL && p->len == 5u + 3u && ((p->d[2] >> 4) & 3u) == 1u && memcmp(&p->d[5], data + 1021, 3) == 0,
          "continuation 3 B, PB=01");
    CHECK(dbg().held_acl == 0u && fake_rom_em_rx_in_use() == 0u, "EM RX freed after the last fragment");
    /* ROM PB 00 start is normalised to 10 (packet_fragmenter.c drops a PB=00 start) */
    host_nocp(0x81, 1);
    pump();
    fake_rom_inject_acl(0x81, 0, data, 10);
    pump();
    p = next_pkt();
    CHECK(p != NULL && ((p->d[2] >> 4) & 3u) == 2u && stats().acl_rx_pb_fixed == 1u, "PB 00 -> 10");
    host_nocp(0x81, 1);
    pump();
    shutdown_and_check();

    /* bt_dlv failure: entry kept, retried, fault after N, exactly-once delivery */
    setup();
    bring_up();
    t_recv_fail = (int)BT_CTRL_DLV_FAULT_AFTER + 2;
    inject_conn_req(7);
    for (int i = 0; i < 30; i++) { bt_ctrl_run(); (void)bt_ctrl_dlv_run(); }
    CHECK(t_faults == 1u && t_fault_last == BT_CTRL_FAULT_DLV, "one BT fault after N failures");
    uint32_t c = 0;
    while ((p = next_pkt()) != NULL) c += p->d[1] == 0x04u;
    CHECK(c == 1u && stats().dlv_fail == BT_CTRL_DLV_FAULT_AFTER + 2u, "delivered exactly once after retries");
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 6. 0C35 at zero command credits (plan M1 test through bt_ctrl)                                                 */
/* ------------------------------------------------------------------------------------------------------------- */
static void test_0c35_zero_cmd_credits(void)
{
    setup();
    bring_up();
    link_up(0);
    uint8_t data[64] = { 0 };
    for (int i = 0; i < 6; i++) fake_rom_inject_acl(0x80, 2, data, 64);
    pump();
    t_rx_rd = t_rx_n;
    CHECK(dbg().c2h_outstanding == 4u && dbg().held_acl == 2u, "window exhausted");
    fake_rom.hold_replies = true;
    uint8_t one = 1, cod[3] = { 1, 2, 3 };
    send_cmd(0x0C1Au, &one, 1);
    pump();
    send_cmd(0x0C24u, cod, 3);          /* ordinary command queued ahead of the credit return */
    pump();
    CHECK(dbg().rom_cmd_credits == 0u && dbg().tx_free == 2u, "zero command credits, command queued");
    uint32_t cmds = fake_rom.n_cmds;
    host_nocp(0x80, 4);
    pump();
    uint32_t acl = 0, cc = 0;
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) { acl += p->d[0] == BT_H4_ACL; cc += p->d[0] == BT_H4_EVT; }
    CHECK(acl == 2u && cc == 0u, "0C35 processed at zero command credits; inbound ACL resumed; no reply");
    CHECK(fake_rom.n_cmds == cmds && dbg().rom_cmd_credits == 0u && dbg().tx_free == 2u,
          "0C35 used no ROM command credit and did not reach the ROM; 0C24 still waits");
    fake_rom.hold_replies = false;
    fake_rom_release_held();
    pump();
    host_nocp(0x80, 2);
    pump();
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 7. NOCP accounting, disconnect, IRQ multiplicity                                                               */
/* ------------------------------------------------------------------------------------------------------------- */
static void test_nocp_disconnect_irq(void)
{
    setup();
    bring_up();
    link_up(0);
    link_up(1);
    fake_rom.auto_ack_acl = false;
    send_acl(0x80, 2, 10, 0);
    send_acl(0x81, 2, 10, 0);
    send_acl(0x80, 2, 10, 0);
    pump();
    CHECK(dbg().h2c_inflight == 3u, "inflight 3");
    fake_rom_ack_acl(1);
    pump();
    /* Disconnection Complete for 0x80: inflight for 0x80 released, pending NOCP dropped */
    uint8_t disc[5] = { 0x00, 0x00, 0x80, 0x00, 0x13 };
    fake_rom_link(0, false);
    fake_rom_inject(0xE458u, disc, 5);
    pump();
    CHECK(dbg().h2c_inflight == 1u && dbg().nocp_pending == 0u, "disconnect released 0x80 (inflight %u)",
          dbg().h2c_inflight);
    const pkt_t *p;
    int saw_disc = 0;
    while ((p = next_pkt()) != NULL) {
        if (p->d[1] == 0x05u) { saw_disc = 1; CHECK(p->d[2] == 4u && p->d[4] == 0x80u && p->d[6] == 0x13u, "0x05"); }
    }
    CHECK(saw_disc, "Disconnection Complete delivered");
    /* stale NOCP for the released handle is clamped, not forwarded */
    fake_rom_ack_acl(2);
    pump();
    uint32_t nocp_h80 = 0;
    while ((p = next_pkt()) != NULL) if (p->d[1] == BT_HCI_EVT_NOCP && p->d[4] == 0x80u) nocp_h80++;
    CHECK(nocp_h80 == 0u && stats().nocp_excess == 1u && dbg().h2c_inflight == 0u, "stale NOCP clamped");
    /* ACL for a disconnected handle is dropped, never wedged */
    send_acl(0x80, 2, 10, 0);
    pump();
    CHECK(dbg().tx_free == 3u && stats().acl_tx_dropped == 1u, "stale-handle ACL dropped");
    /* IRQ: coalescing, loop-until-clear, unknown bits, budget */
    uint32_t isr0 = t_wake_isr;
    fake_rom_raise_irq(BT_ROM_IRQ_CSCNT);
    fake_rom_raise_irq(BT_ROM_IRQ_CSCNT);
    fake_rom_raise_irq(BT_ROM_IRQ_CSCNT);
    CHECK(t_wake_isr == isr0 + 3u, "top half wakes bt_ctrl from ISR");
    bt_ctrl_stats_t s0 = stats();
    bt_ctrl_run();
    bt_ctrl_stats_t s1 = stats();
    CHECK(fake_rom.irq_count[0] == 3u && s1.bh_serviced[0] == s0.bh_serviced[0] + 1u, "3 IRQs coalesced into 1");
    fake_rom.irq_rearm = 2;
    fake_rom_raise_irq(BT_ROM_IRQ_RX | (1u << 5));
    bt_ctrl_run();
    bt_ctrl_stats_t s2 = stats();
    CHECK(s2.bh_serviced[1] == s1.bh_serviced[1] + 3u && s2.bh_loops_max >= 3u, "bottom half looped until clear");
    CHECK(s2.bh_unhandled[5] == s1.bh_unhandled[5] + 1u, "unknown IRQ bit counted");
    fake_rom.irq_rearm = 100;
    fake_rom_raise_irq(BT_ROM_IRQ_KE_TIMER);
    CHECK(bt_ctrl_run() == 1u && stats().bh_budget_exhausted >= 1u, "bounded bottom half reports more work");
    fake_rom.irq_rearm = 0;
    pump();
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 8. teardown / restart ordering with traffic outstanding                                                        */
/* ------------------------------------------------------------------------------------------------------------- */
static void during_delivery_teardown(void)
{
    CHECK(bt_ctrl_dlv_busy(), "dlv busy inside callback");
    bt_ctrl_shutdown_begin();
    CHECK(bt_ctrl_open() == -1, "open must refuse while not OFF / dlv busy");
}

static void test_teardown_restart(void)
{
    for (int cycle = 0; cycle < 50; cycle++) {
        if (cycle == 0) setup();
        else {
            CHECK(bt_ctrl_open() == 0, "reopen cycle %d", cycle);
            fake_rom.calls = 0;
        }
        bring_up();
        link_up(0);
        fake_rom.auto_ack_acl = false;
        uint8_t data[300];
        memset(data, cycle, sizeof(data));
        /* traffic outstanding in every direction */
        for (int i = 0; i < 6; i++) fake_rom_inject_acl(0x80, 2, data, 300);          /* 4 delivered-ish, 2 held */
        send_acl(0x80, 2, 100, 1);
        send_acl(0x80, 2, 100, 2);
        fake_rom.hold_replies = true;
        uint8_t one = 1;
        send_cmd(0x0C1Au, &one, 1);
        for (int i = 0; i < 3; i++) bt_ctrl_run();                                     /* bt_dlv not run */
        for (int i = 0; i < 8; i++) inject_conn_req((uint8_t)i);
        for (int i = 0; i < 3; i++) bt_ctrl_run();
        send_acl(0x80, 2, 100, 3);
        fake_rom_raise_irq(BT_ROM_IRQ_RX);
        bt_ctrl_debug_t d = dbg();
        CHECK(d.held_acl != 0u && d.evt_used != 0u && d.fwd_pending == 1u && fake_rom_msgs_owned_by_ctrl() != 0u,
              "traffic outstanding before teardown (held %u evt %u)", d.held_acl, d.evt_used);
        uint32_t sess = d.session;
        uint32_t rx_before = t_rx_n, sa_before = t_send_avail;
        bt_ctrl_shutdown_begin();
        d = dbg();
        CHECK(d.session == sess + 1u && !d.admission_open && !d.dlv_open, "quiesced and admission closed first");
        CHECK(!bt_ctrl_check_send_available(), "admission closed");
        uint32_t dropped = stats().tx_dropped_closed;
        uint8_t buf[8];
        bt_ctrl_send(buf, h4_cmd(buf, 0x0C1Au, &one, 1));
        CHECK(stats().tx_dropped_closed == dropped + 1u && t_faults == 0u, "late send dropped, not a fault");
        CHECK(bt_ctrl_dlv_run() == BT_CTRL_DLV_IDLE && t_rx_n == rx_before && t_send_avail == sa_before,
              "no delivery or send-available after quiesce");
        fake_rom.call_log_n = 0u;
        uint32_t log0 = 0u;
        bt_ctrl_run();
        CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_OK, "stop status");
        CHECK(fake_rom.kept_at_stop == 0u && fake_rom.em_rx_held_at_stop == 0u, "all bt_ctrl ROM objects released"
              " before stop (%u msgs, %u EM)", fake_rom.kept_at_stop, fake_rom.em_rx_held_at_stop);
        /* order: releases ... stop -> irq_enable(0), nothing after */
        uint32_t n = fake_rom.call_log_n;
        CHECK(n >= log0 + 2u && fake_rom.call_log[n - 2u] == FR_STOP && fake_rom.call_log[n - 1u] == FR_IRQ_EN &&
              fake_rom.irq_enabled == 0u, "stop then IRQ mask last");
        for (uint32_t k = log0; k + 2u < n; k++) {
            CHECK(fake_rom.call_log[k] == FR_EM_RX_FREE || fake_rom.call_log[k] == FR_MSG_FREE,
                  "only releases before stop (call %u = %u)", k, fake_rom.call_log[k]);
        }
        d = dbg();
        CHECK(d.state == 0u && d.evt_used == 0u && d.acl_used == 0u && d.tx_free == 3u && d.held_acl == 0u,
              "rings freed");
        CHECK(bt_ctrl_run() == 0u && fake_rom.call_log_n == n, "no ROM call after teardown");
        /* late upcall from the stopped controller incarnation */
        uint8_t ev[10] = { 0 };
        uint32_t late = stats().late_upcalls;
        fake_rom_deliver_stale(0xE457u, ev, 10);
        CHECK(stats().late_upcalls == late + 1u, "late callback rejected");
        CHECK(fake_rom.errors == 0u, "fake ROM error: %s", fake_rom.first_error);
        fake_rom.hold_replies = false;
        fake_rom.auto_ack_acl = true;
        t_rx_rd = t_rx_n = 0;
    }

    /* stop failure is reported, not ignored */
    CHECK(bt_ctrl_open() == 0, "reopen");
    bring_up();
    fake_rom.stop_result = 1;
    bt_ctrl_shutdown_begin();
    bt_ctrl_run();
    CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_ERR, "stop failure reported");
    fake_rom.stop_result = 0;

    /* teardown while bt_dlv is inside a host callback; pending send-available dropped */
    CHECK(bt_ctrl_open() == 0, "reopen 2");
    bring_up();
    inject_conn_req(1);
    inject_conn_req(2);
    bt_ctrl_run();
    t_cb_hook = during_delivery_teardown;
    uint32_t before = t_rx_n;
    CHECK(bt_ctrl_dlv_run() == BT_CTRL_DLV_IDLE, "dlv stops after teardown inside the callback");
    CHECK(t_rx_n == before + 1u, "the in-flight delivery completes, nothing after it");
    CHECK(!bt_ctrl_dlv_busy(), "dlv not busy after return");
    bt_ctrl_run();
    CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_OK && fake_rom.kept_at_stop == 0u, "teardown completed");

    /* real Reset in the middle of traffic: undelivered ACL of the old state is dropped by bt_dlv */
    CHECK(bt_ctrl_open() == 0, "reopen 3");
    bring_up();
    link_up(0);
    uint8_t data[32] = { 0 };
    fake_rom_inject_acl(0x80, 2, data, 32);
    fake_rom_inject_acl(0x80, 2, data, 32);
    bt_ctrl_run();                       /* ACL in ring, not delivered */
    CHECK(dbg().acl_used == 2u, "ACL queued");
    send_cmd(0x0C03u, NULL, 0);
    pump();
    uint32_t acl = 0;
    const pkt_t *p;
    int reset_cc = 0;
    while ((p = next_pkt()) != NULL) { acl += p->d[0] == BT_H4_ACL; reset_cc |= is_reply(p, 0x0C03u); }
    CHECK(acl == 0u && reset_cc && stats().dlv_stale == 2u, "pre-Reset ACL dropped, Reset CC delivered");
    CHECK(fake_rom.starts >= 2u && dbg().c2h_outstanding == 0u && dbg().c2h_fc == 0u, "controller really reset");
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 9. bt_stop expiry between the check and the ROM call: exactly that call completes, then none                    */
/* ------------------------------------------------------------------------------------------------------------- */
static void scenario_traffic(void)
{
    bring_up();
    link_up(0);
    uint8_t data[200];
    memset(data, 0x5A, sizeof(data));
    fake_rom_inject_acl(0x80, 2, data, 200);
    fake_rom_inject_acl(0x80, 2, data, 200);
    send_acl(0x80, 2, 100, 1);
    uint8_t one = 1;
    send_cmd(0x0C1Au, &one, 1);
    inject_conn_req(3);
    fake_rom_raise_irq(BT_ROM_IRQ_CSCNT | BT_ROM_IRQ_KE_TIMER);
    pump();
    host_nocp(0x80, 2);
    pump();
}

static void test_bt_stop_sweep(void)
{
    setup();
    scenario_traffic();
    uint32_t total = fake_rom.calls;
    CHECK(fake_rom.errors == 0u && total > 50u, "baseline scenario (%u ROM calls)", total);
    printf("  bt_stop sweep over %u ROM-call expiry points\n", total);
    uint32_t bad = 0;
    for (uint32_t k = 1; k <= total; k++) {
        t_quiet = 1;
        setup();
        fake_rom.stop_at_call = k;
        scenario_traffic();
        for (int i = 0; i < 3; i++) { bt_ctrl_run(); (void)bt_ctrl_dlv_run(); }
        fake_rom_raise_irq(BT_ROM_IRQ_RX);
        bt_ctrl_run();
        bt_ctrl_shutdown_begin();
        bt_ctrl_run();
        t_quiet = 0;
        if (fake_rom.stop_fired_at != k || !fake_rom.stop_call_completed || fake_rom.calls_after_stop != 0u ||
            stats().parked != 1u || bt_ctrl_shutdown_status() != BT_CTRL_STOP_PARKED || fake_rom.errors != 0u) {
            bad++;
            if (bad < 4) {
                printf("  k=%u fired=%u completed=%u after=%u parked=%u err=%s\n", k, fake_rom.stop_fired_at,
                       fake_rom.stop_call_completed, fake_rom.calls_after_stop, stats().parked, fake_rom.first_error);
            }
        }
    }
    CHECK(bad == 0u, "bt_stop sweep: %u of %u expiry points violated 'exactly one call completes, then none'", bad,
          total);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 10. faults: allocation refusal, class M failure, ROM fault upcall, lock depth                                  */
/* ------------------------------------------------------------------------------------------------------------- */
static void test_faults(void)
{
    setup();
    bring_up();
    fake_rom.heap_budget = fake_rom.heap_used;   /* next bt_ctrl allocation is refused */
    uint8_t one = 1;
    const pkt_t *r = roundtrip(0x0C1Au, &one, 1);
    if (r) CHECK(status_of(r) == BT_HCI_ERR_MEM_FULL, "refused allocation -> local CC 0x07");
    CHECK(stats().cmd_alloc_refused == 1u && t_fault_last == BT_CTRL_FAULT_INVARIANT, "class M local failure faults");
    CHECK(stats().fault_site == BT_CTRL_SITE_CLASS_M_LOCAL && stats().fault_a == 0x0C1Au &&
          stats().fault_b == BT_HCI_ERR_MEM_FULL, "fault site %u a=0x%X b=0x%X, want class-M local 0C1A/07",
          stats().fault_site, stats().fault_a, stats().fault_b);
    fake_rom.heap_budget = 8192;
    fake_rom_set_reply_status(0xE01Cu, 0x0C);
    uint8_t mask[8] = { 0 };
    r = roundtrip(0x0C01u, mask, 8u);
    if (r) CHECK(status_of(r) == 0x0Cu, "class M FWD status passed through unchanged");
    CHECK(stats().mandatory_fwd_fail == 1u, "class M nonzero ROM status is an invariant violation");
    /* ROM-internal allocation failure -> on_fault -> BT fault (outside the upcall) */
    setup();
    bring_up();
    uint8_t cod[3] = { 0 };
    send_cmd(0x0C24u, cod, 3);
    fake_rom.heap_budget = fake_rom.heap_used + 24u;   /* bt_ctrl's 3-byte request (16 + 8) fits; the reply does not */
    pump();
    CHECK(stats().rom_faults == 1u && t_fault_last == BT_CTRL_FAULT_ROM, "ROM on_fault -> BT fault");
    CHECK(stats().fault_site == BT_CTRL_SITE_ROM_FAULT && stats().fault_a == 1u, "fault site %u a=%u, want ROM/1",
          stats().fault_site, stats().fault_a);
    /* ROM calling TASK_HCI with its lock held */
    setup();
    bring_up();
    fake_rom.lock_depth = 1;
    inject_conn_req(1);
    uint32_t wd = t_wake_dlv;
    bt_ctrl_run();
    CHECK(stats().lock_violations >= 1u && t_wake_dlv == wd, "lock held: violation counted, no FreeRTOS wake");
    fake_rom.lock_depth = 0;
    pump();
    CHECK(t_crit == 0 && t_crit_max == 1, "critical sections balanced and never nested");
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 11. review regressions (one block per finding / surviving mutant)                                               */
/* ------------------------------------------------------------------------------------------------------------- */
static void inject_disc(uint16_t h, uint8_t reason)
{
    uint8_t p[5] = { 0x00, 0x00, (uint8_t)h, (uint8_t)(h >> 8), reason };
    fake_rom_inject(0xE458u, p, 5);
}

static void inject_conn_cmp(uint16_t h)
{
    /* ROM layout (FIX_CONN_CMP): status, pad, u16 handle, pad, BD_ADDR[6], link type, encryption */
    uint8_t p[13] = { 0x00, 0x00, (uint8_t)h, (uint8_t)(h >> 8), 0x00, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0x01, 0x00 };
    fake_rom_inject(0xE456u, p, 13);
}

static int is_evt(const pkt_t *p, uint8_t code)
{
    return p != NULL && p->d[0] == BT_H4_EVT && p->d[1] == code;
}

/* bt_dlv order across the two RX rings: arrival order, events never behind credit-gated ACL (findings 1 and 9). */
static void test_rx_order(void)
{
    uint8_t data[64];
    memset(data, 0x33, sizeof(data));
    const pkt_t *p1, *p2;

    /* (a) an event that arrived before an ACL packet is delivered first (kills "ACL ring first") */
    setup();
    bring_up();
    link_up(0);
    t_rx_rd = t_rx_n;
    inject_conn_req(0x41);
    bt_ctrl_run();                               /* bt_dlv not run: both rings fill */
    fake_rom_inject_acl(0x80, 2, data, 64);
    bt_ctrl_run();
    CHECK(dbg().evt_used == 1u && dbg().acl_used == 1u, "(a) both rings hold one entry (evt %u acl %u)",
          dbg().evt_used, dbg().acl_used);
    (void)bt_ctrl_dlv_run();
    p1 = next_pkt();
    p2 = next_pkt();
    CHECK(is_evt(p1, 0x04u) && p1->d[8] == 0x41u && p2 != NULL && p2->d[0] == BT_H4_ACL,
          "(a) event that arrived first is delivered first");
    host_nocp(0x80, 1);
    pump();
    shutdown_and_check();

    /* (b) ACL already in its ring, then the peer drops the link: the ACL reaches the host BEFORE Disconnection
     *     Complete (Bluedroid drops ACL for a handle whose LCB is already released) */
    setup();
    bring_up();
    link_up(0);
    t_rx_rd = t_rx_n;
    fake_rom_inject_acl(0x80, 2, data, 64);
    bt_ctrl_run();
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    bt_ctrl_run();
    CHECK(dbg().evt_used == 1u && dbg().acl_used == 1u, "(b) both rings hold one entry");
    (void)bt_ctrl_dlv_run();
    p1 = next_pkt();
    p2 = next_pkt();
    CHECK(p1 != NULL && p1->d[0] == BT_H4_ACL && is_evt(p2, BT_HCI_EVT_DISC),
          "(b) ACL delivered before the later Disconnection Complete");
    pump();
    shutdown_and_check();

    /* (c) ACL_RX and Disconnection Complete in the same kernel run (ACL held during the upcall, copied after it) */
    setup();
    bring_up();
    link_up(0);
    t_rx_rd = t_rx_n;
    fake_rom_inject_acl(0x80, 2, data, 64);
    fake_rom_inject_acl(0x80, 1, data, 32);
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    pump();
    p1 = next_pkt();
    p2 = next_pkt();
    const pkt_t *p3 = next_pkt();
    CHECK(p1 != NULL && p1->d[0] == BT_H4_ACL && p2 != NULL && p2->d[0] == BT_H4_ACL && is_evt(p3, BT_HCI_EVT_DISC),
          "(c) same-run ACL kept ahead of Disconnection Complete");
    shutdown_and_check();

    /* (d) with zero host credits the held ACL does not hold the event back (plan M1 RX dispatcher (1)) */
    setup();
    bring_up();
    link_up(0);
    for (int i = 0; i < 5; i++) fake_rom_inject_acl(0x80, 2, data, 16);
    pump();
    t_rx_rd = t_rx_n;
    CHECK(dbg().c2h_outstanding == 4u && dbg().held_acl == 1u, "(d) window exhausted, one ACL held");
    inject_conn_req(0x42);
    pump();
    p1 = next_pkt();
    CHECK(is_evt(p1, 0x04u) && next_pkt() == NULL && dbg().held_acl == 1u, "(d) event not blocked by held ACL");
    host_nocp(0x80, 4);
    pump();
    host_nocp(0x80, 1);
    pump();
    shutdown_and_check();

    /* (e) ACL arriving after an event that is still deferred (not yet in the event ring) waits for it */
    setup();
    bring_up();
    link_up(0);
    t_rx_rd = t_rx_n;
    for (int i = 0; i < 7; i++) inject_conn_req((uint8_t)i);   /* 6 queued, #6 deferred */
    fake_rom_inject_acl(0x80, 2, data, 16);
    bt_ctrl_run();
    CHECK(dbg().evt_deferred == 1u, "(e) one event deferred");
    (void)bt_ctrl_dlv_run();                     /* bt_dlv drains the event ring before bt_ctrl runs again */
    pump();
    int i6 = -1, ia = -1, k = 0;
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) {
        if (is_evt(p, 0x04u) && p->d[8] == 6u) i6 = k;
        if (p->d[0] == BT_H4_ACL) ia = k;
        k++;
    }
    CHECK(i6 >= 0 && ia > i6, "(e) ACL delivered after the older deferred event (event %d, ACL %d)", i6, ia);
    host_nocp(0x80, 1);
    pump();
    shutdown_and_check();

    /* (f) a reserved reply queued ahead of an older (previously deferred) event in the event ring: an ACL that arrived
     *     between them still waits for the older event, not just for the ring head */
    setup();
    bring_up();
    link_up(0);
    t_rx_rd = t_rx_n;
    for (int i = 0; i < 7; i++) inject_conn_req((uint8_t)i);
    bt_ctrl_run();                               /* 6 queued, #6 deferred */
    fake_rom_inject_acl(0x80, 2, data, 16);      /* arrives after #6 */
    send_cmd(0x1005u, NULL, 0u);                 /* its CC takes a reserved slot: newer than the ACL */
    bt_ctrl_run();
    t_recv_ok_left = 2;
    (void)bt_ctrl_dlv_run();                     /* #0, #1 delivered */
    t_recv_ok_left = -1;
    bt_ctrl_run();                               /* #6 joins the ring behind the CC; the ACL enters its ring */
    CHECK(dbg().evt_deferred == 0u && dbg().acl_used == 1u, "(f) setup: deferred event queued, ACL in its ring");
    pump();
    i6 = ia = -1;
    k = 0;
    while ((p = next_pkt()) != NULL) {
        if (is_evt(p, 0x04u) && p->d[8] == 6u) i6 = k;
        if (p->d[0] == BT_H4_ACL) ia = k;
        k++;
    }
    CHECK(i6 >= 0 && ia > i6, "(f) ACL delivered after the older event behind the reply (event %d, ACL %d)", i6, ia);
    host_nocp(0x80, 1);
    pump();
    shutdown_and_check();
}

/* Findings 2 and 6: nothing posted while the ROM lock is held; the pending fault and the bt_dlv wake survive. */
static void test_lock_held_end_of_run(void)
{
    setup();
    bring_up();
    fake_rom.lock_depth = 1;                     /* upcall with the lock held AND the lock leaked past run() */
    inject_conn_req(1);
    uint32_t wd = t_wake_dlv, f0 = t_faults, lv0 = stats().lock_violations;
    bt_ctrl_run();
    CHECK(t_faults == f0 && t_wake_dlv == wd && stats().lock_violations > lv0, "lock held: nothing posted");
    CHECK(dbg().evt_used == 1u, "event queued while the lock was held");
    fake_rom.lock_depth = 0;
    bt_ctrl_run();                               /* no new work: only what was withheld */
    CHECK(t_wake_dlv == wd + 1u, "withheld bt_dlv wake delivered once the lock is free");
    CHECK(t_faults == f0 + 1u && t_fault_last == BT_CTRL_FAULT_INVARIANT, "lock violation posted once the lock is free");
    pump();
    shutdown_and_check();
}

/* Finding 3: a ROM fault on the teardown path fails the stop; the next session does not inherit it. */
static void test_fault_on_stop(void)
{
    setup();
    bring_up();
    fake_rom.fault_on_stop = true;
    bt_ctrl_shutdown_begin();
    bt_ctrl_run();
    CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_ERR && stats().stop_faults == 1u, "fault during stop -> STOP_ERR");
    fake_rom.fault_on_stop = false;
    uint32_t f0 = t_faults;
    CHECK(bt_ctrl_open() == 0, "reopen");
    bring_up();
    CHECK(t_faults == f0, "next session posted the previous session's fault (%u)", t_faults - f0);
    shutdown_and_check();
}

/* Finding 4: deferred ROM events released by HCI Reset / teardown are counted. */
static void test_reset_deferred_events(void)
{
    setup();
    bring_up();
    for (int i = 0; i < 10; i++) inject_conn_req((uint8_t)i);
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().evt_used == BT_CTRL_EVT_SLOTS - BT_CTRL_EVT_RESERVED && dbg().evt_deferred == BT_CTRL_EVT_DEFER,
          "general area full, defer list full");
    send_cmd(0x0C03u, NULL, 0u);
    bt_ctrl_run();
    CHECK(stats().evt_dropped_reset == BT_CTRL_EVT_DEFER && fake_rom.kept_at_stop == 0u,
          "deferred events dropped by Reset are counted (%u)", stats().evt_dropped_reset);
    uint32_t conn = 0, reset_cc = 0;
    pump();
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) { conn += is_evt(p, 0x04u); reset_cc += is_reply(p, 0x0C03u); }
    CHECK(conn == 6u && reset_cc == 1u, "ring events kept and delivered before the Reset CC (%u)", conn);
    /* teardown with deferred events */
    for (int i = 0; i < 10; i++) inject_conn_req((uint8_t)i);
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().evt_deferred == BT_CTRL_EVT_DEFER, "deferred again");
    shutdown_and_check();
    CHECK(stats().evt_dropped_stop == BT_CTRL_EVT_DEFER, "deferred events released by teardown are counted");
}

/* Finding 5: a failed Reset (stop or start) leaves IRQ 26 masked. */
static void test_reset_failure_masks_irq(void)
{
    setup();
    bring_up();
    CHECK(fake_rom.irq_enabled == 1u, "IRQ on after bring-up");
    fake_rom.stop_result = 1;
    const pkt_t *r = roundtrip(0x0C03u, NULL, 0u);
    if (r) CHECK(status_of(r) == BT_HCI_ERR_HW_FAILURE, "re-Reset with stop failure -> 0x03");
    CHECK(fake_rom.irq_enabled == 0u && dbg().rom_running == 0u, "IRQ 26 masked after the failed stop");
    fake_rom.stop_result = 0;
    r = roundtrip(0x0C03u, NULL, 0u);
    if (r) CHECK(status_of(r) == 0u && fake_rom.irq_enabled == 1u, "Reset recovers");
    fake_rom.start_result = 1;
    r = roundtrip(0x0C03u, NULL, 0u);
    if (r) CHECK(status_of(r) == BT_HCI_ERR_HW_FAILURE, "start failure -> 0x03");
    CHECK(fake_rom.irq_enabled == 0u, "IRQ 26 masked after the failed start");
    fake_rom.start_result = 0;
    r = roundtrip(0x0C03u, NULL, 0u);
    if (r) CHECK(status_of(r) == 0u && fake_rom.irq_enabled == 1u, "Reset recovers again");
    shutdown_and_check();
}

/* Finding 7: a forwarded LC command whose LC instance was freed under it is answered locally. */
static void test_fwd_orphan(void)
{
    setup();
    bring_up();
    link_up(0);
    uint8_t dp[3] = { 0x80, 0x00, 0x13 };
    fake_rom.lc_swallow = true;
    send_cmd(0x0406u, dp, 3u);
    pump();
    CHECK(dbg().fwd_pending == 1u && dbg().rom_cmd_credits == 0u && pending_pkts() == 0u, "0406 swallowed by the ROM");
    fake_rom.lc_swallow = false;
    fake_rom_link(0, false);
    inject_disc(0x80, 0x08);
    pump();
    const pkt_t *p1 = next_pkt(), *p2 = next_pkt();
    CHECK(is_evt(p1, BT_HCI_EVT_DISC) && is_reply(p2, 0x0406u) && p2->d[1] == BT_HCI_EVT_CS &&
          status_of(p2) == BT_HCI_ERR_UNKNOWN_CONN, "Disconnection Complete, then CS 0406 status 0x02");
    CHECK(dbg().fwd_pending == 0u && dbg().rom_cmd_credits == 1u && dbg().evt_resv == 0u &&
          stats().fwd_orphaned == 1u, "command window released");
    uint8_t one = 1u;
    const pkt_t *r = roundtrip(0x0C1Au, &one, 1u);
    CHECK(r != NULL && status_of(r) == 0u, "next command not wedged");
    /* the ROM's own reply, when it comes after the Disconnection Complete, is used and not duplicated */
    link_up(0);
    inject_conn_cmp(0x80);                       /* a new link on 0x80 (a dead handle is answered locally) */
    pump();
    t_rx_rd = t_rx_n;
    fake_rom.hold_replies = true;
    send_cmd(0x0406u, dp, 3u);
    pump();
    fake_rom.hold_replies = false;
    fake_rom_link(0, false);
    inject_disc(0x80, 0x16);
    fake_rom_release_held();                     /* ROM queue: Disconnection Complete, then CS 0406 */
    pump();
    uint32_t cs = 0;
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) {
        if (is_reply(p, 0x0406u)) { cs++; CHECK(status_of(p) == 0u, "ROM reply status kept"); }
    }
    CHECK(cs == 1u && stats().fwd_orphaned == 1u && stats().evt_unsolicited_reply == 0u, "exactly one 0406 reply");
    CHECK(dbg().fwd_pending == 0u && dbg().rom_cmd_credits == 1u, "window released by the ROM reply");
    shutdown_and_check();
}

/* Finding 8: host ACL queued for a handle that disconnects is flushed, never sent on the reused handle. */
static void test_stale_tx_acl_flush(void)
{
    setup();
    bring_up();
    link_up(0);
    fake_rom.auto_ack_acl = false;
    uint32_t n0 = fake_rom.n_acl, d0 = stats().acl_tx_dropped;
    send_acl(0x80, 2, 20, 1);                    /* READY, not yet drained */
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    fake_rom_link(0, true);                      /* the ROM hands 0x80 to an incoming connection in the same run */
    inject_conn_cmp(0x80);
    pump();
    CHECK(fake_rom.n_acl == n0 && dbg().h2c_inflight == 0u && stats().acl_tx_dropped == d0 + 1u &&
          dbg().tx_free == BT_CTRL_TX_SLOTS, "stale ACL flushed by Disconnection Complete, not sent to the new link");
    /* ACL the host sent for the old link after the disconnect, before the handle was reused */
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    pump();
    send_acl(0x80, 2, 20, 2);
    fake_rom_link(0, true);
    inject_conn_cmp(0x80);
    pump();
    CHECK(fake_rom.n_acl == n0 && stats().acl_tx_dropped == d0 + 2u, "stale ACL flushed by the new Connection Complete");
    send_acl(0x80, 2, 20, 3);                    /* after Connection Complete: the new link's data goes out */
    pump();
    CHECK(fake_rom.n_acl == n0 + 1u && fake_rom.acl[n0].data[0] == 3u && dbg().h2c_inflight == 1u, "new link ACL sent");
    fake_rom_ack_acl(1);
    pump();
    /* the ROM reassigns 0x80 before the new link's Connection Complete exists (setup still in progress): only the
     * Disconnection Complete flush keeps the old link's data off the new peer */
    uint32_t n1 = fake_rom.n_acl, d1 = stats().acl_tx_dropped;
    send_acl(0x80, 2, 20, 4);
    inject_disc(0x80, 0x13);                     /* conhdl_to_task(0x80) stays valid: handle already reused */
    pump();
    CHECK(fake_rom.n_acl == n1 && stats().acl_tx_dropped == d1 + 1u && dbg().h2c_inflight == 0u,
          "stale ACL flushed by Disconnection Complete alone");
    shutdown_and_check();
}

/* Credit-blocked inbound ACL (g.held[]) must never be delivered after Disconnection Complete or onto a reused handle,
 * and ACL_RX for a dead or out-of-range handle must be dropped immediately. */
static void test_stale_rx_acl_flush(void)
{
    uint8_t d_a[1024], d_b[64];
    memset(d_a, 0xAA, sizeof(d_a));
    memset(d_b, 0xBB, sizeof(d_b));
    const pkt_t *p;

    /* (a) credit-blocked packets on 0x80 dropped once Disconnection Complete is delivered; never injected onto a
     *     new connection reusing 0x80 when the host returns the earlier credits */
    setup();
    bring_up();
    link_up(0);
    for (int i = 0; i < 6; i++) CHECK(fake_rom_inject_acl(0x80, 2, d_a, 64) == 0, "inject A %d", i);
    pump();
    t_rx_rd = t_rx_n;
    CHECK(dbg().c2h_outstanding == 4u && dbg().held_acl == 2u, "4 delivered, 2 held");
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    pump();
    p = next_pkt();
    CHECK(is_evt(p, BT_HCI_EVT_DISC) && next_pkt() == NULL, "Disconnection Complete delivered");
    CHECK(dbg().held_acl == 0u && fake_rom_em_rx_in_use() == 0u,
          "stale held ACL freed once Disconnection Complete crossed to host (held %u em %u)",
          dbg().held_acl, fake_rom_em_rx_in_use());
    fake_rom_link(0, true);
    inject_conn_cmp(0x80);
    host_nocp(0x80, 4);
    pump();
    uint32_t stale_acl = 0u, saw_conn = 0u;
    while ((p = next_pkt()) != NULL) {
        stale_acl += p->d[0] == BT_H4_ACL;
        saw_conn += is_evt(p, 0x03u);
    }
    CHECK(saw_conn == 1u && stale_acl == 0u, "no stale Peer A ACL delivered onto Peer B (stale %u)", stale_acl);
    shutdown_and_check();

    /* (b) partially delivered >1021 B packet: continuation fragment dropped on Disconnection Complete */
    setup();
    bring_up();
    link_up(0);
    uint8_t hbs1[7] = { 0xFD, 0x03, 0xFF, 0x01, 0x00, 0x0A, 0x00 };
    (void)roundtrip(0x0C33u, hbs1, 7u);
    CHECK(fake_rom_inject_acl(0x80, 2, d_a, 1024) == 0, "inject 1024");
    pump();
    p = next_pkt();
    CHECK(p != NULL && p->d[0] == BT_H4_ACL && dbg().held_acl == 1u, "first fragment delivered, continuation held");
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    pump();
    p = next_pkt();
    CHECK(is_evt(p, BT_HCI_EVT_DISC) && dbg().held_acl == 0u && fake_rom_em_rx_in_use() == 0u,
          "continuation fragment dropped on disconnect");
    host_nocp(0x80, 1);
    pump();
    CHECK(next_pkt() == NULL, "no orphan continuation fragment delivered after disconnect");
    shutdown_and_check();

    /* (c) non-head held entry for 0x80 behind live 0x81, plus late ACL_RX on dead 0x80 and out-of-range 0x87 */
    setup();
    bring_up();
    link_up(0);
    link_up(1);
    for (int i = 0; i < 5; i++) CHECK(fake_rom_inject_acl(0x81, 2, d_b, 32) == 0, "inject B %d", i);
    CHECK(fake_rom_inject_acl(0x80, 2, d_a, 32) == 0, "inject A behind B");
    pump();
    t_rx_rd = t_rx_n;
    CHECK(dbg().held_acl == 2u, "B#5 and A#1 held");
    fake_rom_link(0, false);
    inject_disc(0x80, 0x13);
    pump();
    p = next_pkt();
    CHECK(is_evt(p, BT_HCI_EVT_DISC) && dbg().held_acl == 1u, "A#1 swept while B#5 kept (held %u)", dbg().held_acl);
    CHECK(fake_rom_inject_acl(0x80, 2, d_a, 32) == 0, "late ACL_RX on dead 0x80");
    CHECK(fake_rom_inject_acl(0x87, 2, d_a, 32) == 0, "ACL_RX on out-of-range 0x87");
    pump();
    CHECK(dbg().held_acl == 1u && fake_rom_em_rx_in_use() == 1u, "dead/out-of-range ACL_RX dropped at upcall");
    host_nocp(0x81, 4);
    pump();
    p = next_pkt();
    CHECK(p != NULL && p->d[0] == BT_H4_ACL && (p->d[1] | ((p->d[2] & 0x0Fu) << 8)) == 0x81u && p->d[5] == 0xBBu &&
          next_pkt() == NULL, "only live 0x81 packet delivered");
    host_nocp(0x81, 1);
    pump();
    shutdown_and_check();
}

/* Unsolicited ROM CC/CS replies (e.g. a second late command reply after g.fwd_late_op was overwritten or expired)
 * must be counted and consumed, never forwarded with Num_HCI_Command_Packets=1 to Bluedroid. */
static void test_unsolicited_reply_dropped(void)
{
    setup();
    bring_up();
    link_up(0);
    t_rx_rd = t_rx_n;
    /* Direct unsolicited CC (0xE088 -> 0x0C18) with no command pending */
    uint8_t cc_ok[2] = { 0x00, 0x00 };
    fake_rom_inject(0xE088u, cc_ok, 1u);
    pump();
    CHECK(stats().evt_unsolicited_reply == 1u && pending_pkts() == 0u,
          "unsolicited CC must be dropped, not delivered (%u pkts)", pending_pkts());
    /* Late CS (0xE0CB -> 0x0419) arriving while another command (0x0C1A) is in flight */
    fake_rom.hold_replies = true;
    uint8_t one = 1u;
    send_cmd(0x0C1Au, &one, 1u);
    pump();
    CHECK(dbg().fwd_pending == 1u && dbg().rom_cmd_credits == 0u, "0C1A in flight");
    fake_rom_inject(0xE0CBu, cc_ok, 2u);
    pump();
    CHECK(stats().evt_unsolicited_reply == 2u && pending_pkts() == 0u && dbg().fwd_pending == 1u,
          "unsolicited CS while 0C1A in flight must not reach host (%u pkts)", pending_pkts());
    fake_rom.hold_replies = false;
    fake_rom_release_held();
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0C1Au) && next_pkt() == NULL, "real 0C1A reply delivered intact");
    shutdown_and_check();
}

/* 0x0C13 (Write Local Name): a spec-valid 248-byte non-NUL UTF-8 name must still give ROM handler 0x3ED00 a NUL
 * guard byte at m[248] so its 249-byte scan never reads adjacent g_bt_heap metadata (BT_ROM_HCI_MAP.md §2). */
static void test_0c13_rom_nul_guard(void)
{
    setup();
    bring_up();
    uint8_t full_name[250];
    memset(full_name, 'A', sizeof(full_name));
    uint32_t c0 = fake_rom.n_cmds;
    const pkt_t *r = roundtrip(0x0C13u, full_name, 248u);
    CHECK(r != NULL && status_of(r) == 0u && fake_rom.n_cmds == c0 + 1u, "0C13 248 B accepted");
    CHECK(fake_rom.cmds[c0].len >= 249u && fake_rom.cmds[c0].data[248] == 0u,
          "0C13 KE msg must have NUL guard at offset 248 (len %u, byte248 0x%02X)",
          fake_rom.cmds[c0].len, fake_rom.cmds[c0].data[248]);
    /* Oversize H4 parameter (> 248 B) must also clamp copied name to 248 B, preserving m[248] == 0 */
    c0 = fake_rom.n_cmds;
    r = roundtrip(0x0C13u, full_name, 250u);
    CHECK(r != NULL && fake_rom.n_cmds == c0 + 1u && fake_rom.cmds[c0].len >= 249u && fake_rom.cmds[c0].data[248] == 0u,
          "0C13 >248 B clamped before guard byte");
    shutdown_and_check();
}

/* If bt_stop latched before bt_ctrl_shutdown_begin(), bt_ctrl_run() must still run do_stop() to report
 * BT_CTRL_STOP_PARKED and return to ST_OFF instead of stranding BT_CTRL_STOP_PENDING. */
static void test_parked_shutdown_completes(void)
{
    setup();
    bring_up();
    t_stop = true;
    fake_rom_raise_irq(BT_ROM_IRQ_CSCNT);
    uint32_t c0 = fake_rom.calls;
    (void)bt_ctrl_run();
    CHECK(dbg().parked == 1u && fake_rom.calls == c0, "parked on bt_stop without ROM calls");
    bt_ctrl_shutdown_begin();
    CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_PENDING, "pending right after shutdown_begin");
    (void)bt_ctrl_run();
    CHECK(bt_ctrl_shutdown_status() == BT_CTRL_STOP_PARKED && dbg().state == 0u && fake_rom.calls == c0,
          "do_stop completed as STOP_PARKED with 0 ROM calls (status %d, state %u)",
          bt_ctrl_shutdown_status(), dbg().state);
}

/* Finding 10: send-available only on a full -> not-full transition. */
static void test_send_avail_only_when_full(void)
{
    setup();
    bring_up();
    link_up(0);
    uint32_t sa0 = t_send_avail;
    uint8_t one = 1u;
    send_acl(0x80, 2, 10, 1);
    pump();
    send_cmd(0x0C1Au, &one, 1u);
    pump();
    CHECK(t_send_avail == sa0, "no send-available while the ring was never full (%u)", t_send_avail - sa0);
    send_acl(0x80, 2, 10, 2);
    send_acl(0x80, 2, 10, 3);                    /* 2 of 3 slots */
    pump();
    CHECK(t_send_avail == sa0 && dbg().tx_free == BT_CTRL_TX_SLOTS, "2 of 3 slots drained: no send-available");
    shutdown_and_check();
}

/* Finding 11: a nonzero status on a forwarded class-M command posts a BT fault. */
static void test_class_m_fwd_fault(void)
{
    setup();
    bring_up();
    fake_rom_set_reply_status(0xE01Cu, 0x0C);
    uint32_t f0 = t_faults, inv0 = stats().invariant;
    uint8_t mask[8] = { 0 };
    const pkt_t *r = roundtrip(0x0C01u, mask, 8u);
    if (r) CHECK(status_of(r) == 0x0Cu, "status passed through");
    CHECK(t_faults == f0 + 1u && t_fault_last == BT_CTRL_FAULT_INVARIANT && stats().invariant == inv0 + 1u &&
          stats().mandatory_fwd_fail == 1u, "class M FWD failure posted as an invariant fault");
    CHECK(stats().fault_site == BT_CTRL_SITE_CLASS_M_STATUS && stats().fault_a == 0x0C01u && stats().fault_b == 0x0Cu,
          "fault site %u a=0x%X b=0x%X, want class-M status 0C01/0C", stats().fault_site, stats().fault_a,
          stats().fault_b);
    fake_rom_set_reply_status(0xE01Cu, 0x00);
    shutdown_and_check();
}

/* Finding 12: a slot bt_dlv freed goes to the oldest deferred event; a new event neither overtakes nor overflows. */
static void test_deferred_order_after_free(void)
{
    setup();
    bring_up();
    t_rx_rd = t_rx_n;
    for (int i = 0; i < 10; i++) inject_conn_req((uint8_t)i);
    for (int i = 0; i < 5; i++) bt_ctrl_run();
    CHECK(dbg().evt_used == 6u && dbg().evt_deferred == 4u && stats().evt_overflow == 0u, "6 queued, 4 deferred");
    t_recv_ok_left = 1;
    (void)bt_ctrl_dlv_run();                     /* exactly one event delivered */
    t_recv_ok_left = -1;
    CHECK(dbg().evt_used == 5u, "one slot freed");
    inject_conn_req(10);
    bt_ctrl_run();
    CHECK(stats().evt_overflow == 0u && t_faults == 0u, "no overflow although a slot was free (%u)",
          stats().evt_overflow);
    pump();
    int last = -1, ordered = 1;
    uint32_t got = 0;
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) {
        if (is_evt(p, 0x04u)) { got++; if (p->d[8] != (uint8_t)(last + 1)) ordered = 0; last = p->d[8]; }
    }
    CHECK(got == 11u && ordered, "11 events in injection order (%u, ordered %d)", got, ordered);
    shutdown_and_check();
}

/* Finding 13: NOCP uses a reserved slot while the general area is full and bt_dlv is stuck. */
static void test_nocp_reserved_slot(void)
{
    setup();
    bring_up();
    link_up(0);
    fake_rom.auto_ack_acl = false;
    send_acl(0x80, 2, 10, 1);
    pump();
    CHECK(dbg().h2c_inflight == 1u, "one ACL in flight");
    for (int i = 0; i < 6; i++) inject_conn_req((uint8_t)i);
    for (int i = 0; i < 5; i++) bt_ctrl_run();   /* bt_dlv stuck: general area full */
    CHECK(dbg().evt_used == BT_CTRL_EVT_SLOTS - BT_CTRL_EVT_RESERVED, "general area full");
    fake_rom_ack_acl(1);
    bt_ctrl_run();
    CHECK(dbg().nocp_pending == 0u && dbg().evt_used == BT_CTRL_EVT_SLOTS - BT_CTRL_EVT_RESERVED + 1u &&
          dbg().h2c_inflight == 0u, "NOCP took a reserved slot (pending %u used %u)", dbg().nocp_pending,
          dbg().evt_used);
    pump();
    shutdown_and_check();
}

/* Finding 14: process_acl error paths. */
static void test_acl_tx_error_paths(void)
{
    /* (a) ACL descriptor allocation refused: the EM TX buffer goes back, the packet waits and goes out later */
    setup();
    bring_up();
    link_up(0);
    uint32_t n0 = fake_rom.n_acl, ref0 = stats().cmd_alloc_refused;
    fake_rom.heap_budget = fake_rom.heap_used;
    send_acl(0x80, 2, 30, 7);
    pump();
    CHECK(stats().cmd_alloc_refused > ref0 && fake_rom_em_tx_in_use() == 0u && fake_rom.n_acl == n0 &&
          dbg().tx_free == BT_CTRL_TX_SLOTS - 1u, "refused descriptor: EM TX buffer freed, ACL kept");
    fake_rom.heap_budget = 8192u;
    pump();
    CHECK(fake_rom.n_acl == n0 + 1u && fake_rom.acl[n0].data[0] == 7u && dbg().tx_free == BT_CTRL_TX_SLOTS,
          "ACL delivered once memory is back");
    CHECK(fake_rom.errors == 0u, "fake ROM error: %s", fake_rom.first_error);
    shutdown_and_check();
    /* (b) EM TX allocation failure with a credit is an invariant violation */
    setup();
    bring_up();
    link_up(0);
    fake_rom.em_tx_fail = true;
    send_acl(0x80, 2, 10, 1);
    bt_ctrl_run();
    CHECK(stats().acl_tx_em_fail == 1u && t_faults == 1u && t_fault_last == BT_CTRL_FAULT_INVARIANT,
          "EM TX failure with a credit -> invariant fault");
    CHECK(stats().fault_site == BT_CTRL_SITE_EM_TX_ALLOC && stats().fault_a == 0x80u && stats().fault_b == 0u,
          "fault site %u a=0x%X b=%u, want EM TX alloc on 0x80 with 0 in flight", stats().fault_site,
          stats().fault_a, stats().fault_b);
    fake_rom.em_tx_fail = false;
    pump();
    CHECK(fake_rom.n_acl == 1u && dbg().tx_free == BT_CTRL_TX_SLOTS, "ACL sent after EM TX recovers");
    shutdown_and_check();
}

/* Finding 15: HCI Reset drops host ACL and 0C35 queued behind it. */
static void test_reset_drops_queued_tx(void)
{
    setup();
    bring_up();
    link_up(0);
    uint8_t data[16] = { 0 };
    fake_rom_inject_acl(0x80, 2, data, 16);
    pump();
    CHECK(dbg().c2h_outstanding == 1u, "one C2H credit outstanding before Reset");
    uint32_t n0 = fake_rom.n_acl, noack0 = stats().tx_noack;
    send_cmd(0x0C03u, NULL, 0u);
    send_acl(0x80, 2, 10, 1);
    host_nocp(0x80, 1);
    pump();
    CHECK(fake_rom.n_acl == n0 && stats().tx_noack == noack0 && dbg().tx_free == BT_CTRL_TX_SLOTS,
          "pre-Reset ACL and 0C35 dropped (acl %u noack %u)", fake_rom.n_acl - n0, stats().tx_noack - noack0);
    shutdown_and_check();
}

/* Finding 16: connection-handle error paths. */
static void test_handle_errors(void)
{
    setup();
    bring_up();
    uint8_t dp[3] = { 0x85, 0x00, 0x13 };
    const pkt_t *r = roundtrip(0x0406u, dp, 3u);
    if (r) CHECK(r->d[1] == BT_HCI_EVT_CS && status_of(r) == BT_HCI_ERR_UNKNOWN_CONN, "0406 unknown handle -> CS 0x02");
    link_up(0);
    fake_rom.conhdl_type_override = 2;           /* live handle that is not an ACL link */
    uint32_t n0 = fake_rom.n_acl, d0 = stats().acl_tx_dropped;
    send_acl(0x80, 2, 10, 1);
    pump();
    CHECK(stats().acl_tx_dropped == d0 + 1u && fake_rom.n_acl == n0, "ACL on a non-ACL handle dropped");
    fake_rom.conhdl_type_override = -1;
    shutdown_and_check();
}

/* Finding 17: 0C31 with flow control off. */
static void test_c2h_fc_off(void)
{
    setup();
    bring_up();
    link_up(0);
    uint8_t zero = 0u;
    (void)roundtrip(0x0C31u, &zero, 1u);
    CHECK(dbg().c2h_fc == 0u, "0C31(0) turns the C2H credit gate off");
    uint8_t data[50];
    memset(data, 0x44, sizeof(data));
    for (int i = 0; i < 6; i++) fake_rom_inject_acl(0x80, 2, data, 50);
    pump();
    uint32_t acl = 0;
    const pkt_t *p;
    while ((p = next_pkt()) != NULL) acl += p->d[0] == BT_H4_ACL;
    CHECK(acl == 6u && dbg().c2h_outstanding == 0u && dbg().held_acl == 0u, "no credit gate: %u of 6 delivered", acl);
    uint32_t inv0 = stats().c2h_invalid;
    host_nocp(0x80, 1);
    pump();
    CHECK(stats().c2h_invalid == inv0 + 1u, "0C35 with flow control off counted invalid");
    shutdown_and_check();
}


/* ------------------------------------------------------------------------------------------------------------- */
/* 11. phone path: event packers, scripted remote peer, dead handles, forwarded-command deadline                   */
/* ------------------------------------------------------------------------------------------------------------- */
static void hexdump(const char *tag, const uint8_t *d, uint16_t n)
{
    printf("   %s:", tag);
    for (uint16_t k = 0; k < n && k < 40; k++) printf(" %02X", d[k]);
    printf(n > 40 ? " ...\n" : "\n");
}

static int same(const uint8_t *got, uint16_t glen, const uint8_t *want, uint16_t wlen)
{
    return glen == wlen && memcmp(got, want, wlen) == 0;
}

/* Golden vectors for every ROM reply/event id bt_ctrl_evt.c packs beyond the legacy set: ROM param bytes as the ROM
 * sends them (naturally aligned struct, pad bytes 0xEE) -> exact H4 bytes, hand-derived from the stock packers
 * (docs/BT_ROM_HCI_MAP.md §9), not from bt_ctrl_evt.c. */
typedef struct {
    uint16_t id;
    uint8_t rom[24];
    uint8_t rom_len;
    uint8_t h4[30];
    uint8_t h4_len;
} evt_vec_t;

#define A6 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6
#define Q4(x) x, x, x, x
static const evt_vec_t k_evt_vecs[] = {
    { 0xE0D0, { 0x00 }, 1, { 4, 0x01, 1, 0x00 }, 4 },
    { 0xE0D1, { 0x05, A6, 0x01, 0x00, 0x00, 0x0C, 0x02, 0x5A, 0xEE, 0x34, 0x12 }, 16,
      { 4, 0x02, 15, 0x01, A6, 0x01, 0x00, 0x00, 0x0C, 0x02, 0x5A, 0x34, 0x12 }, 18 },
    /* E0D6 (map §3, ROM senders only): {num, bdaddr, psrm, rsvd, cod@9, clkoff@0xC, rssi@0xE} = wire 0x22, num -> 1 */
    { 0xE0D6, { 0x00, A6, 0x01, 0x00, 0x18, 0x04, 0x24, 0x34, 0x12, 0xC4 }, 15,
      { 4, 0x22, 15, 0x01, A6, 0x01, 0x00, 0x18, 0x04, 0x24, 0x34, 0x12, 0xC4 }, 18 },
    { 0xE45B, { 0x00, 0xEE, 0x80, 0x00 }, 4, { 4, 0x09, 3, 0x00, 0x80, 0x00 }, 6 },
    { 0xE45C, { 0x00, 0xEE, 0x80, 0x00, 0x01 }, 5, { 4, 0x0A, 4, 0x00, 0x80, 0x00, 0x01 }, 7 },
    { 0xE45F, { 0x00, 0xEE, 0x80, 0x00, 0x01, 0x02, 0xEE, 0xEE, Q4(0x11), Q4(0x22), Q4(0x33), Q4(0x44) }, 24,
      { 4, 0x0D, 21, 0x00, 0x80, 0x00, 0x01, 0x02, Q4(0x11), Q4(0x22), Q4(0x33), Q4(0x44) }, 24 },
    { 0xE0D4, { 0x07 }, 1, { 4, 0x10, 1, 0x07 }, 4 },
    { 0xE460, { 0x80, 0x00 }, 2, { 4, 0x11, 2, 0x80, 0x00 }, 5 },
    { 0xE461, { 0x00, A6, 0x01 }, 8, { 4, 0x12, 8, 0x00, A6, 0x01 }, 11 },
    { 0xE463, { 0x00, 0xEE, 0x80, 0x00, 0x02, 0xEE, 0x20, 0x03 }, 8, { 4, 0x14, 6, 0x00, 0x80, 0x00, 0x02, 0x20, 0x03 }, 9 },
    { 0xE467, { 0x01 }, 1, { 4, 0x1A, 1, 0x01 }, 4 },
    { 0xE469, { 0x00, 0xEE, 0x80, 0x00, 0x34, 0x12 }, 6, { 4, 0x1C, 5, 0x00, 0x80, 0x00, 0x34, 0x12 }, 8 },
    { 0xE46A, { 0x00, 0xEE, 0x80, 0x00, 0x18, 0xCC }, 6, { 4, 0x1D, 5, 0x00, 0x80, 0x00, 0x18, 0xCC }, 8 },
    { 0xE46B, { 0x80, 0x00 }, 2, { 4, 0x1E, 2, 0x80, 0x00 }, 5 },
    { 0xE46C, { 0x00, 0xEE, 0x80, 0x00, 0x00, 0x01, 0x02, 0xEE, Q4(0x11), Q4(0x22), Q4(0x33), Q4(0x44) }, 24,
      { 4, 0x21, 22, 0x00, 0x80, 0x00, 0x00, 0x01, 0x02, Q4(0x11), Q4(0x22), Q4(0x33), Q4(0x44) }, 25 },
    { 0xE46D, { 0x00, 0xEE, 0x80, 0x00, 0x01, 0x02, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8 }, 14,
      { 4, 0x23, 13, 0x00, 0x80, 0x00, 0x01, 0x02, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8 }, 16 },
    { 0xE471, { 0x00, 0xEE, 0x80, 0x00 }, 4, { 4, 0x30, 3, 0x00, 0x80, 0x00 }, 6 },
    { 0xE475, { A6 }, 6, { 4, 0x34, 6, A6 }, 9 },
    { 0xE476, { A6 }, 6, { 4, 0x35, 6, A6 }, 9 },
    { 0xE479, { 0x80, 0x00 }, 2, { 4, 0x39, 2, 0x80, 0x00 }, 5 },
    { 0xE47A, { A6, 0xEE, 0xEE, 0x40, 0xE2, 0x01, 0x00 }, 12, { 4, 0x3B, 10, A6, 0x40, 0xE2, 0x01, 0x00 }, 13 },
    { 0xE47B, { A6, 0x02 }, 7, { 4, 0x3C, 7, A6, 0x02 }, 10 },
    { 0xE47C, { A6, 0x01, 0, 0, 0, 0, 0, 0, 0x80 }, 14, { 4, 0x3D, 14, A6, 0x01, 0, 0, 0, 0, 0, 0, 0x80 }, 17 },
    /* legacy-packed events on the phone path (bt_hci.c:763-848), same method */
    { 0xE456, { 0x00, 0xEE, 0x80, 0x00, 0xEE, A6, 0x01, 0x00 }, 13, { 4, 0x03, 11, 0x00, 0x80, 0x00, A6, 0x01, 0x00 }, 14 },
    { 0xE45A, { 0x00, 0xEE, 0x80, 0x00, 0x01 }, 5, { 4, 0x08, 4, 0x00, 0x80, 0x00, 0x01 }, 7 },
    { 0xE459, { 0x05, 0xEE, 0x80, 0x00 }, 4, { 4, 0x06, 3, 0x05, 0x80, 0x00 }, 6 },
    { 0xE474, { A6, 0xEE, 0xEE, 0x40, 0xE2, 0x01, 0x00 }, 12, { 4, 0x33, 10, A6, 0x40, 0xE2, 0x01, 0x00 }, 13 },
    { 0xE45E, { 0x00, 0xEE, 0x80, 0x00, 0x0A, 0xEE, 0x0F, 0x00, 0x34, 0x12 }, 10,
      { 4, 0x0C, 8, 0x00, 0x80, 0x00, 0x0A, 0x0F, 0x00, 0x34, 0x12 }, 11 },
    /* command replies (stock reply tables; CC generic packer FW 0xD5860C, CS FW 0xD59189) */
    { 0xE42E, { 0x00, 0xEE, 0x80, 0x00 }, 4, { 4, 0x0E, 6, 1, 0x28, 0x0C, 0x00, 0x80, 0x00 }, 9 },
    { 0xE431, { 0x0C, 0xEE, 0x81, 0x00 }, 4, { 4, 0x0E, 6, 1, 0x37, 0x0C, 0x0C, 0x81, 0x00 }, 9 },
    { 0xE06E, { 0x12, A6 }, 7, { 4, 0x0E, 10, 1, 0x1A, 0x04, 0x12, A6 }, 13 },
    { 0xE076, { 0x00, A6 }, 7, { 4, 0x0E, 10, 1, 0x34, 0x04, 0x00, A6 }, 13 },
    { 0xE069, { 0x02, A6 }, 7, { 4, 0x0E, 10, 1, 0x08, 0x04, 0x02, A6 }, 13 },
    { 0xE066, { 0x0C }, 1, { 4, 0x0E, 4, 1, 0x02, 0x04, 0x0C }, 7 },
    { 0xE088, { 0x12 }, 1, { 4, 0x0E, 4, 1, 0x18, 0x0C, 0x12 }, 7 },
    { 0xE08C, { 0x00 }, 1, { 4, 0x0E, 4, 1, 0x1C, 0x0C, 0x00 }, 7 },
    { 0xE08E, { 0x00 }, 1, { 4, 0x0E, 4, 1, 0x1E, 0x0C, 0x00 }, 7 },
    { 0xE0A2, { 0x12 }, 1, { 4, 0x0E, 4, 1, 0x3A, 0x0C, 0x12 }, 7 },
    { 0xE0A9, { 0x00 }, 1, { 4, 0x0E, 4, 1, 0x45, 0x0C, 0x00 }, 7 },
    { 0xE0AF, { 0x00 }, 1, { 4, 0x0E, 4, 1, 0x52, 0x0C, 0x00 }, 7 },
    { 0xE0C7, { 0x00 }, 1, { 4, 0x0F, 4, 0x00, 1, 0x01, 0x04 }, 7 },
    { 0xE0C8, { 0x09 }, 1, { 4, 0x0F, 4, 0x09, 1, 0x05, 0x04 }, 7 },
    { 0xE0CB, { 0x00 }, 1, { 4, 0x0F, 4, 0x00, 1, 0x19, 0x04 }, 7 },
    { 0xE43D, { 0x11 }, 1, { 4, 0x0F, 4, 0x11, 1, 0x0F, 0x04 }, 7 },
    { 0xE43E, { 0x00 }, 1, { 4, 0x0F, 4, 0x00, 1, 0x11, 0x04 }, 7 },
    { 0xE43F, { 0x12 }, 1, { 4, 0x0F, 4, 0x12, 1, 0x13, 0x04 }, 7 },
    { 0xE441, { 0x00 }, 1, { 4, 0x0F, 4, 0x00, 1, 0x1B, 0x04 }, 7 },
    { 0xE442, { 0x1A }, 1, { 4, 0x0F, 4, 0x1A, 1, 0x1C, 0x04 }, 7 },
    { 0xE443, { 0x0C }, 1, { 4, 0x0F, 4, 0x0C, 1, 0x1D, 0x04 }, 7 },
    { 0xE444, { 0x00 }, 1, { 4, 0x0F, 4, 0x00, 1, 0x1F, 0x04 }, 7 },
};
#undef Q4

static void test_event_packers(void)
{
    uint8_t out[BT_CTRL_EVT_MAX_H4];
    bt_ctrl_evt_info_t info;
    for (size_t i = 0; i < sizeof(k_evt_vecs) / sizeof(k_evt_vecs[0]); i++) {
        const evt_vec_t *v = &k_evt_vecs[i];
        memset(out, 0xCD, sizeof(out));
        uint16_t n = bt_ctrl_evt_pack(v->id, v->rom, v->rom_len, out, &info);
        CHECK(same(out, n, v->h4, v->h4_len), "ROM id %04X golden H4 mismatch", v->id);
        if (!same(out, n, v->h4, v->h4_len)) { hexdump("got ", out, n); hexdump("want", v->h4, v->h4_len); }
        CHECK(h4_rx_valid(out, n), "ROM id %04X packs an H4 packet Bluedroid would drop", v->id);
        if (v->rom_len > 1u) {
            CHECK(bt_ctrl_evt_pack(v->id, v->rom, (uint16_t)(v->rom_len - 1u), out, &info) == 0u &&
                  info.kind == BT_CTRL_EVK_SHORT, "ROM id %04X: one byte short must be SHORT", v->id);
        }
    }
    /* Remote Name Request Complete: full 255 B, and a short name zero-filled (stock copies 255 B verbatim) */
    uint8_t rnr[255];
    memset(rnr, 0, sizeof(rnr));
    const uint8_t a6[6] = { A6 };
    memcpy(&rnr[1], a6, 6);
    memcpy(&rnr[7], "Pixel 9", 7);
    memset(&rnr[200], 0x77, 55);                  /* name bytes at the end are kept */
    uint16_t n = bt_ctrl_evt_pack(0xE0D3, rnr, 255, out, &info);
    CHECK(n == 258u && out[0] == 4 && out[1] == 0x07 && out[2] == 255 && memcmp(&out[3], rnr, 255) == 0,
          "E0D3 full -> event 0x07, 255 B verbatim");
    n = bt_ctrl_evt_pack(0xE0D3, rnr, 14, out, &info);
    int zero_tail = 1;
    for (int k = 17; k < 258; k++) zero_tail &= out[k] == 0;
    CHECK(n == 258u && memcmp(&out[3], rnr, 14) == 0 && zero_tail, "E0D3 short: name zero-filled to 248");
    CHECK(bt_ctrl_evt_pack(0xE0D3, rnr, 6, out, &info) == 0u && info.kind == BT_CTRL_EVK_SHORT, "E0D3 < 7 B SHORT");
    /* E0D7 Extended Inquiry Result: E0D6's 15 bytes + eir[240] verbatim = 0x2F (255 B), num forced to 1, a short
     * message zero-filled */
    uint8_t eir[256];
    for (int k = 0; k < 256; k++) eir[k] = (uint8_t)(k * 7 + 1);
    eir[0] = 0;
    n = bt_ctrl_evt_pack(0xE0D7, eir, 256, out, &info);
    CHECK(n == 258u && out[1] == 0x2F && out[2] == 255 && out[3] == 1 && memcmp(&out[4], &eir[1], 254) == 0 &&
          h4_rx_valid(out, n), "E0D7 -> event 0x2F, 255 B verbatim, num 1");
    n = bt_ctrl_evt_pack(0xE0D7, eir, 20, out, &info);
    zero_tail = 1;
    for (int k = 3 + 20; k < 258; k++) zero_tail &= out[k] == 0;
    CHECK(n == 258u && memcmp(&out[4], &eir[1], 19) == 0 && zero_tail, "E0D7 short: EIR zero-filled");
    CHECK(bt_ctrl_evt_pack(0xE0D7, eir, 14, out, &info) == 0u && info.kind == BT_CTRL_EVK_SHORT, "E0D7 < 15 B SHORT");
    /* not packed on purpose (no stock packer / feature compiled out): counted as unknown, never mis-packed */
    const uint16_t unknown[] = { 0xE46E, 0xE46F, 0xE470, 0xE454, 0xE455 };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        CHECK(bt_ctrl_evt_pack(unknown[i], rnr, 255, out, &info) == 0u && info.kind == BT_CTRL_EVK_UNKNOWN,
              "%04X must stay unknown", unknown[i]);
    }
    /* every reply id in the opcode table (FWD and gated rows) maps back to its own opcode */
    uint32_t rows_n = 0;
    const bt_ctrl_cmd_row_t *rows = bt_ctrl_cmds_table(&rows_n);
    for (uint32_t i = 0; i < rows_n; i++) {
        if (rows[i].rom_rsp != 0u) {
            CHECK(bt_ctrl_evt_reply_opcode(rows[i].rom_rsp) == rows[i].op, "%04X reply id %04X not packed back",
                  rows[i].op, rows[i].rom_rsp);
        }
    }
}

/* ---- scripted phone ---- */
static const uint8_t k_phone[6] = { 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };

static void phone_cfg(fake_peer_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->addr, k_phone, 6);
    c->cod[0] = 0x0C; c->cod[1] = 0x02; c->cod[2] = 0x5A;   /* smartphone */
    c->io_cap = 0x01;                                       /* DisplayYesNo */
    c->auth_req = 0x03;                                     /* MITM, dedicated bonding */
    c->numeric = 123456u;                                   /* 0x0001E240 */
    c->user_confirms = true;
    c->auto_encrypt = true;
    c->host_feat_notify = true;
    c->key_type = 0x05;                                     /* authenticated combination key P-192 */
    static const uint8_t f0[8] = { 0xBF, 0xFE, 0xCF, 0xFE, 0xDB, 0xFF, 0x7B, 0x87 };   /* bit 63 ext, bit 51 SSP */
    static const uint8_t f1[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };                        /* SSP host */
    memcpy(c->features, f0, 8);
    memcpy(c->features_p1, f1, 8);
    c->max_page = 2;
    c->lmp_ver = 0x0A;
    c->manuf = 0x000F;
    c->subver = 0x1234;
    c->clk_off = 0x4321;
    memcpy(c->name, "Pixel", 5);
}

/* Next delivered packet must be exactly `want`. */
static void expect_pkt(const uint8_t *want, uint16_t n, const char *what)
{
    const pkt_t *p = next_pkt();
    int ok = p != NULL && same(p->d, p->len, want, n);
    CHECK(ok, "phone path: %s", what);
    if (!ok) {
        if (p != NULL) hexdump("got ", p->d, p->len); else printf("   got : (nothing)\n");
        hexdump("want", want, n);
    }
}

static void expect_cs(uint16_t op, uint8_t status, const char *what)
{
    uint8_t w[7] = { 4, 0x0F, 4, status, 1, (uint8_t)op, (uint8_t)(op >> 8) };
    expect_pkt(w, 7, what);
}

static void expect_cc_addr(uint16_t op, uint8_t status, const char *what)
{
    uint8_t w[13] = { 4, 0x0E, 10, 1, (uint8_t)op, (uint8_t)(op >> 8), status };
    memcpy(&w[7], k_phone, 6);
    expect_pkt(w, 13, what);
}

static void expect_addr_evt(uint8_t code, const char *what)
{
    uint8_t w[9] = { 4, code, 6 };
    memcpy(&w[3], k_phone, 6);
    expect_pkt(w, 9, what);
}

static void expect_s_h(uint8_t code, uint8_t status, uint16_t h, const uint8_t *tail, uint8_t tail_len,
                       const char *what)
{
    uint8_t w[32] = { 4, code, (uint8_t)(3u + tail_len), status, (uint8_t)h, (uint8_t)(h >> 8) };
    if (tail_len) memcpy(&w[6], tail, tail_len);
    expect_pkt(w, (uint16_t)(6u + tail_len), what);
}

static void cmd_addr(uint16_t op, const uint8_t *tail, uint8_t tail_len)
{
    uint8_t p[32];
    memcpy(p, k_phone, 6);
    if (tail_len) memcpy(&p[6], tail, tail_len);
    send_cmd(op, p, (uint8_t)(6u + tail_len));
    pump();
}

static void cmd_h(uint16_t op, uint16_t h, const uint8_t *tail, uint8_t tail_len)
{
    uint8_t p[32] = { (uint8_t)h, (uint8_t)(h >> 8) };
    if (tail_len) memcpy(&p[2], tail, tail_len);
    send_cmd(op, p, (uint8_t)(2u + tail_len));
    pump();
}

/* Incoming connection: Connection Request, 0409 (remain slave: Bluedroid's choice without local role switch,
 * l2c_link.c:96-98, and the legacy accept at bt_hci.c:1592-1595), CS, Connection Complete. */
static void phone_connects(void)
{
    CHECK(fake_peer_page(0) == 0, "page");
    pump();
    uint8_t creq[13] = { 4, 0x04, 10, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x0C, 0x02, 0x5A, 0x01 };
    expect_pkt(creq, 13, "Connection Request (E457 -> 0x04)");
    uint8_t role = 0x01;
    cmd_addr(0x0409u, &role, 1);
    expect_cs(0x0409u, 0x00, "0409 CS (E0C9)");
    uint8_t cc[14] = { 4, 0x03, 11, 0x00, 0x80, 0x00, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x01, 0x00 };
    expect_pkt(cc, 14, "Connection Complete (E456 -> 0x03)");
    CHECK(fake_peer.connected && fake_peer.host_accept_role == 0x01, "peer connected, host stayed slave");
}

/* btm_acl_created (btm_acl.c:435-436, 474) + btm_read_remote_features_complete (1286) + name (btm_sec.c:5002). */
static void phone_link_setup(const fake_peer_cfg_t *c)
{
    cmd_h(0x041Fu, 0x80, NULL, 0);
    expect_cs(0x041Fu, 0, "041F CS (E444)");
    uint8_t off[2] = { 0x21, 0x43 };
    expect_s_h(0x1C, 0, 0x80, off, 2, "Read Clock Offset Complete (E469 -> 0x1C)");
    cmd_h(0x041Du, 0x80, NULL, 0);
    expect_cs(0x041Du, 0, "041D CS (E443)");
    uint8_t ver[5] = { 0x0A, 0x0F, 0x00, 0x34, 0x12 };
    expect_s_h(0x0C, 0, 0x80, ver, 5, "Read Remote Version Complete (E45E -> 0x0C)");
    cmd_h(0x041Bu, 0x80, NULL, 0);
    expect_cs(0x041Bu, 0, "041B CS (E441)");
    expect_s_h(0x0B, 0, 0x80, c->features, 8, "Read Remote Features Complete (E45D -> 0x0B)");
    uint8_t pg = 1;
    cmd_h(0x041Cu, 0x80, &pg, 1);
    expect_cs(0x041Cu, 0, "041C CS (E442)");
    uint8_t ext[10] = { 0x01, 0x02 };
    memcpy(&ext[2], c->features_p1, 8);
    expect_s_h(0x23, 0, 0x80, ext, 10, "Read Remote Ext Features Complete (E46D -> 0x23)");
    uint8_t rnr_tail[4] = { 0x01, 0x00, 0x00, 0x00 };       /* R1, reserved, clock offset 0 (btm_sec.c:5002) */
    cmd_addr(0x0419u, rnr_tail, 4);
    expect_cs(0x0419u, 0, "0419 CS (E0CB)");
    uint8_t hf[17] = { 4, 0x3D, 14, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };
    memcpy(&hf[9], c->features_p1, 8);
    expect_pkt(hf, 17, "Remote Host Supported Features Notification (E47C -> 0x3D)");
    static uint8_t name[258];
    memset(name, 0, sizeof(name));
    name[0] = 4; name[1] = 0x07; name[2] = 255; name[3] = 0x00;
    memcpy(&name[4], k_phone, 6);
    memcpy(&name[10], "Pixel", 5);
    expect_pkt(name, 258, "Remote Name Request Complete (E0D3 -> 0x07, 255 B)");
}

/* SSP numeric comparison as the responder (the phone authenticates us), Bluedroid's replies: no key (040C),
 * DisplayYesNo + MITM (042B), user accepts (042C). Then the phone encrypts. */
static void phone_pairs_us(void)
{
    fake_peer_authenticate();
    pump();
    expect_addr_evt(0x17, "Link Key Request (E465 -> 0x17)");
    cmd_addr(0x040Cu, NULL, 0);
    expect_cc_addr(0x040Cu, 0, "040C CC (E06B)");
    uint8_t iorsp[12] = { 4, 0x32, 9, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x01, 0x00, 0x03 };
    expect_pkt(iorsp, 12, "IO Capability Response (E473 -> 0x32), responder order: response first");
    expect_addr_evt(0x31, "IO Capability Request (E472 -> 0x31)");
    uint8_t io[3] = { 0x01, 0x00, 0x03 };                  /* DisplayYesNo, no OOB, MITM (plan M4 policy) */
    cmd_addr(0x042Bu, io, 3);
    expect_cc_addr(0x042Bu, 0, "042B CC (E06F)");
    CHECK(fake_peer.host_io_cap == 0x01 && fake_peer.host_auth_req == 0x03, "ROM received DisplayYesNo + MITM");
    uint8_t uc[13] = { 4, 0x33, 10, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x40, 0xE2, 0x01, 0x00 };
    expect_pkt(uc, 13, "User Confirmation Request 123456 (E474 -> 0x33, u32 moved +8 -> +6)");
    cmd_addr(0x042Cu, NULL, 0);
    expect_cc_addr(0x042Cu, 0, "042C CC (E070)");
    uint8_t spc[10] = { 4, 0x36, 7, 0x00, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };
    expect_pkt(spc, 10, "Simple Pairing Complete (E477 -> 0x36)");
    uint8_t lkn[26] = { 4, 0x18, 23, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };
    memcpy(&lkn[9], fake_peer.key, 16);
    lkn[25] = 0x05;
    expect_pkt(lkn, 26, "Link Key Notification (E466 -> 0x18)");
    uint8_t on = 1;
    expect_s_h(0x08, 0, 0x80, &on, 1, "Encryption Change on (E45A -> 0x08)");
    CHECK(fake_peer.has_key && fake_peer.authenticated && fake_peer.encrypted && fake_peer.pairings == 1u,
          "paired, authenticated, encrypted");
}

/* SSP as the initiator (Bluedroid's 0411 from btm_sec_execute_procedure, btm_sec.c:5606), fresh pairing. */
static void we_pair_phone(void)
{
    cmd_h(0x0411u, 0x80, NULL, 0);
    expect_cs(0x0411u, 0, "0411 CS (E43E)");
    expect_addr_evt(0x17, "Link Key Request after 0411");
    cmd_addr(0x040Cu, NULL, 0);
    expect_cc_addr(0x040Cu, 0, "040C CC");
    expect_addr_evt(0x31, "IO Capability Request first (initiator order)");
    uint8_t io[3] = { 0x01, 0x00, 0x03 };
    cmd_addr(0x042Bu, io, 3);
    expect_cc_addr(0x042Bu, 0, "042B CC");
    uint8_t iorsp[12] = { 4, 0x32, 9, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x01, 0x00, 0x03 };
    expect_pkt(iorsp, 12, "IO Capability Response (peer's LMP io_capability_res)");
    uint8_t uc[13] = { 4, 0x33, 10, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x40, 0xE2, 0x01, 0x00 };
    expect_pkt(uc, 13, "User Confirmation Request");
}

static void test_phone_path(void)
{
    fake_peer_cfg_t c;
    phone_cfg(&c);
    setup();
    bring_up();
    uint8_t scan = 0x03;
    (void)roundtrip(0x0C1Au, &scan, 1);
    fake_peer_init(&c);
    t_rx_rd = t_rx_n;

    /* (1) first connection, link set-up reads, responder pairing, peer-started encryption */
    phone_connects();
    phone_link_setup(&c);
    phone_pairs_us();
    uint8_t first_key[16];
    memcpy(first_key, fake_peer.key, 16);

    /* (2) ACL host -> phone under load: 20 L2CAP frames, never more than 1005's 4 credits in the ROM */
    fake_rom.auto_ack_acl = false;
    uint32_t sent = 0, acked = 0, n0 = fake_rom.n_acl, max_inflight = 0, nocp_total = 0;
    for (int it = 0; it < 200 && (sent < 20u || acked < 20u); it++) {
        while (sent < 20u && bt_ctrl_check_send_available()) {
            send_acl(0x80, 2, (uint16_t)(8u + sent * 50u), (uint8_t)sent);
            sent++;
        }
        bt_ctrl_run();
        (void)bt_ctrl_dlv_run();
        if (dbg().h2c_inflight > max_inflight) max_inflight = dbg().h2c_inflight;
        if (it % 3 == 2) acked += fake_rom_ack_acl(2);
        pump();
        const pkt_t *p;
        while ((p = next_pkt()) != NULL) {
            CHECK(p->d[0] == BT_H4_EVT && p->d[1] == BT_HCI_EVT_NOCP && p->d[3] == 1 && p->d[4] == 0x80 && p->d[5] == 0,
                  "only NOCP events for 0x80 during host ACL");
            nocp_total += (uint32_t)(p->d[6] | (p->d[7] << 8));
        }
    }
    CHECK(sent == 20u && acked == 20u && nocp_total == 20u && fake_rom.n_acl == n0 + 20u,
          "20 frames to air, 20 completed, NOCP total %u", nocp_total);
    CHECK(max_inflight == BT_CTRL_H2C_ACL_CREDITS && stats().acl_tx_credit_blocked != 0u,
          "credit gate engaged at %u in flight", max_inflight);
    int in_order = 1;
    for (uint32_t i = 0; i < 20u; i++) {
        const fake_acl_rec_t *r = &fake_rom.acl[n0 + i];
        in_order &= r->handle == 0x80 && r->len == 8u + i * 50u && r->data[0] == (uint8_t)i && r->pb_bc == 2;
    }
    CHECK(in_order, "host ACL reached the air in order with exact lengths");

    /* (3) ACL phone -> host under load: 12 frames, host window 4, credits returned with 0C35 */
    uint8_t l2[600];
    uint32_t injected = 0, got = 0, max_out = 0;
    int data_ok = 1;
    for (int it = 0; it < 200 && got < 12u; it++) {
        while (injected < 12u && fake_rom_em_rx_in_use() < BT_ROM_EM_RX_BUFS) {
            for (int k = 0; k < 600; k++) l2[k] = (uint8_t)(injected * 3u + (uint32_t)k);
            if (fake_peer_send_acl(l2, (uint16_t)(40u + injected * 40u)) != 0) break;
            injected++;
        }
        pump();
        if (dbg().c2h_outstanding > max_out) max_out = dbg().c2h_outstanding;
        const pkt_t *p;
        uint32_t batch = 0;
        while ((p = next_pkt()) != NULL) {
            if (p->d[0] != BT_H4_ACL) continue;
            uint16_t dl = (uint16_t)(p->d[3] | (p->d[4] << 8));
            data_ok &= dl == 40u + got * 40u && p->d[5] == (uint8_t)(got * 3u) && ((p->d[2] >> 4) & 3u) == 2u;
            got++;
            batch++;
        }
        if (batch) host_nocp(0x80, (uint16_t)batch);
        pump();
    }
    CHECK(got == 12u && data_ok, "12 phone frames delivered in order (%u)", got);
    CHECK(max_out <= BT_CTRL_C2H_ACL_CREDITS && stats().c2h_excess == 0u && dbg().c2h_outstanding == 0u,
          "host window respected (max %u outstanding)", max_out);

    /* (4) disconnect while host ACL is queued: 4 in the ROM, 2 waiting in the TX ring, then 0406 */
    uint32_t drop0 = stats().acl_tx_dropped;
    for (int i = 0; i < 4; i++) { send_acl(0x80, 2, 30, (uint8_t)(0x40 + i)); pump(); }
    send_acl(0x80, 2, 30, 0x50);
    send_acl(0x80, 2, 30, 0x51);
    pump();
    CHECK(dbg().h2c_inflight == 4u && dbg().tx_free == 1u, "4 in flight, 2 queued (%u, %u)", dbg().h2c_inflight,
          dbg().tx_free);
    uint8_t reason = 0x13;
    cmd_h(0x0406u, 0x80, &reason, 1);                     /* the command overtakes the credit-blocked ACL */
    expect_cs(0x0406u, 0, "0406 CS (E43A)");
    uint8_t r16 = 0x16;
    expect_s_h(0x05, 0, 0x80, &r16, 1, "Disconnection Complete (E458 -> 0x05)");
    CHECK(pending_pkts() == 0u, "nothing else after the Disconnection Complete");
    CHECK(dbg().h2c_inflight == 0u && dbg().tx_free == BT_CTRL_TX_SLOTS && stats().acl_tx_dropped == drop0 + 2u,
          "queued ACL flushed, in-flight credits released (dropped %u)", stats().acl_tx_dropped - drop0);
    fake_rom_ack_acl(4);                                 /* the baseband's late completions for the dead link */
    pump();
    CHECK(pending_pkts() == 0u && dbg().nocp_pending == 0u, "no NOCP for a disconnected handle");
    send_acl(0x80, 2, 30, 0x60);                         /* host had not seen the Disconnection Complete yet */
    pump();
    CHECK(stats().acl_tx_dead_handle == 1u && dbg().h2c_inflight == 0u, "late ACL for the dead handle dropped");
    uint32_t cmds0 = fake_rom.n_cmds;
    cmd_h(0x041Du, 0x80, NULL, 0);
    expect_cs(0x041Du, BT_HCI_ERR_UNKNOWN_CONN, "LC command for the dead handle: CS 0x02, not forwarded");
    CHECK(fake_rom.n_cmds == cmds0 && stats().cmd_dead_handle == 1u, "dead-handle command never reached the ROM");

    /* (5) re-connect with the stored key, the phone authenticates and encrypts */
    fake_rom.auto_ack_acl = true;
    phone_connects();
    fake_peer_authenticate();
    pump();
    expect_addr_evt(0x17, "Link Key Request on re-connect");
    cmd_addr(0x040Bu, first_key, 16);                    /* Bluedroid's stored key (btm_sec.c:5082) */
    expect_cc_addr(0x040Bu, 0, "040B CC (E06A)");
    uint8_t on = 1;
    expect_s_h(0x08, 0, 0x80, &on, 1, "Encryption Change after re-auth with the stored key");
    CHECK(fake_peer.auth_ok == 2u && fake_peer.pairings == 1u && memcmp(fake_peer.host_key, first_key, 16) == 0,
          "re-connect authenticated with the stored key, no new pairing");

    /* (6) we authenticate (0411) and encrypt (0413) with the stored key */
    cmd_h(0x0411u, 0x80, NULL, 0);
    expect_cs(0x0411u, 0, "0411 CS");
    expect_addr_evt(0x17, "Link Key Request after 0411");
    cmd_addr(0x040Bu, first_key, 16);
    expect_cc_addr(0x040Bu, 0, "040B CC");
    expect_s_h(0x06, 0, 0x80, NULL, 0, "Authentication Complete (E459 -> 0x06)");
    cmd_h(0x0413u, 0x80, &on, 1);
    expect_cs(0x0413u, 0, "0413 CS (E43F)");
    expect_s_h(0x08, 0, 0x80, &on, 1, "Encryption Change (E45A) after 0413");

    /* (7) the phone forgot us: we initiate, fresh numeric comparison, accepted */
    fake_peer_disconnect(0x13);
    pump();
    uint8_t r13 = 0x13;
    expect_s_h(0x05, 0, 0x80, &r13, 1, "remote Disconnection Complete");
    phone_connects();
    fake_peer.has_key = false;
    we_pair_phone();
    cmd_addr(0x042Cu, NULL, 0);
    expect_cc_addr(0x042Cu, 0, "042C CC");
    uint8_t spc[10] = { 4, 0x36, 7, 0x00, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };
    expect_pkt(spc, 10, "Simple Pairing Complete");
    const pkt_t *lk = next_pkt();
    CHECK(is_evt(lk, 0x18) && lk->len == 26u, "Link Key Notification");
    expect_s_h(0x06, 0, 0x80, NULL, 0, "Authentication Complete after initiator pairing");

    /* (8) initiator pairing refused locally: user rejects (042D), then IO capability refused (0434) */
    fake_peer.has_key = false;
    we_pair_phone();
    cmd_addr(0x042Du, NULL, 0);
    expect_cc_addr(0x042Du, 0, "042D CC (E071)");
    uint8_t spf[10] = { 4, 0x36, 7, 0x05, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };
    expect_pkt(spf, 10, "Simple Pairing Complete, authentication failure");
    expect_s_h(0x06, 0x05, 0x80, NULL, 0, "Authentication Complete 0x05");
    cmd_h(0x0411u, 0x80, NULL, 0);
    expect_cs(0x0411u, 0, "0411 CS");
    expect_addr_evt(0x17, "Link Key Request");
    cmd_addr(0x040Cu, NULL, 0);
    expect_cc_addr(0x040Cu, 0, "040C CC");
    expect_addr_evt(0x31, "IO Capability Request");
    uint8_t rej = 0x18;                                  /* Pairing Not Allowed (plan M4: outside the pairing window) */
    cmd_addr(0x0434u, &rej, 1);
    expect_cc_addr(0x0434u, 0, "0434 CC (E076)");
    uint8_t sp18[10] = { 4, 0x36, 7, 0x18, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };
    expect_pkt(sp18, 10, "Simple Pairing Complete 0x18");
    expect_s_h(0x06, 0x18, 0x80, NULL, 0, "Authentication Complete 0x18");

    /* (9) name request for an absent device and RNR cancel */
    uint8_t absent[10] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01, 0, 0, 0 };
    send_cmd(0x0419u, absent, 10);
    pump();
    expect_cs(0x0419u, 0, "0419 CS for an absent device");
    const pkt_t *rn = next_pkt();
    CHECK(is_evt(rn, 0x07) && rn->len == 258u && rn->d[3] == 0x04 && memcmp(&rn->d[4], absent, 6) == 0,
          "RNR complete with page timeout");
    cmd_addr(0x041Au, NULL, 0);
    expect_cc_addr(0x041Au, 0x12, "041A CC (E06E) with no RNR running");

    /* (10) the phone hangs up */
    fake_peer_disconnect(0x13);
    pump();
    expect_s_h(0x05, 0, 0x80, &r13, 1, "remote Disconnection Complete");
    bt_ctrl_stats_t s = stats();
    CHECK(s.rom_unknown_ids == 0u && s.rom_short == 0u && s.evt_unsolicited_reply == 0u && s.cmd_unknown == 0u &&
          s.invariant == 0u && t_faults == 0u, "phone path: no unknown/short ROM message, no unsolicited reply, no fault");
    CHECK(pending_pkts() == 0u, "phone path: every packet accounted for");
    CHECK(fake_rom.errors == 0u, "fake ROM: %s", fake_rom.first_error);
    fake_peer_init(NULL);
    shutdown_and_check();

    /* (11) a phone that rejects the connection attempt by the host is not on this path; 040A after a Connection
     * Request (legacy bt_hci.c:1585-1590) -> CS + Connection Complete with the reject reason */
    setup();
    bring_up();
    fake_peer_init(&c);
    CHECK(fake_peer_page(1) == 0, "page");
    pump();
    t_rx_rd = t_rx_n;
    uint8_t lim = 0x0D;
    cmd_addr(0x040Au, &lim, 1);
    expect_cs(0x040Au, 0, "040A CS (E0CA)");
    uint8_t ccr[14] = { 4, 0x03, 11, 0x0D, 0x00, 0x00, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x01, 0x00 };
    expect_pkt(ccr, 14, "Connection Complete 0x0D (E0D2 -> 0x03)");
    fake_peer_init(NULL);
    shutdown_and_check();
}

/* Forwarded-command deadline: a class-O command the ROM consumes silently is answered 0x22 at 7 s (before
 * Bluedroid's 8 s timeout); the late ROM reply is consumed; class-M commands are never answered by the deadline. */
static uint32_t t_now_ms;
static uint32_t clk(void) { return t_now_ms; }

static void test_fwd_deadline(void)
{
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 0xFFFFF000u;                              /* wraps during the test */
    bring_up();
    link_up(0);
    fake_rom.hold_replies = true;                        /* LC consumes it: no reply for now */
    uint8_t h[2] = { 0x80, 0x00 };
    send_cmd(0x041Du, h, 2);
    pump();
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS - 1u;
    pump();
    CHECK(dbg().fwd_pending == 1u && pending_pkts() == 0u, "still waiting at deadline - 1 ms");
    t_now_ms += 1u;
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x041Du) && p->d[1] == BT_HCI_EVT_CS && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT,
          "CS 0x22 at the deadline");
    CHECK(dbg().fwd_pending == 0u && dbg().rom_cmd_credits == 1u && dbg().evt_resv == 0u && stats().fwd_deadline == 1u,
          "window released");
    uint8_t one = 1;
    send_cmd(0x0C1Au, &one, 1);                          /* the next command is not wedged (its reply is held too) */
    pump();
    fake_rom.hold_replies = false;
    fake_rom_release_held();                             /* late 041D CS, then 0C1A CC */
    pump();
    uint32_t r41d = 0, r1a = 0;
    while ((p = next_pkt()) != NULL) { r41d += is_reply(p, 0x041Du); r1a += is_reply(p, 0x0C1Au); }
    CHECK(r41d == 0u && r1a == 1u && stats().fwd_late_reply == 1u && stats().evt_unsolicited_reply == 0u,
          "late ROM reply consumed once, 0C1A answered");
    /* class M (0C1A is M) is never answered by the deadline */
    fake_rom.hold_replies = true;
    send_cmd(0x0C1Au, &one, 1);
    pump();
    t_now_ms += 3u * BT_CTRL_FWD_DEADLINE_MS;
    pump();
    CHECK(dbg().fwd_pending == 1u && pending_pkts() == 0u && stats().fwd_deadline == 1u, "class M keeps waiting");
    fake_rom.hold_replies = false;
    fake_rom_release_held();
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x0C1Au) && status_of(p) == 0u, "class M reply delivered");
    shutdown_and_check();
}

/* The host sends the SAME opcode again after a deadline reply (review finding: replies are matched on the opcode alone,
 * so the first command's late reply was delivered as the second's, and the second's own reply swallowed as "late").
 * The second command waits in the TX ring until the late reply was consumed, then gets its own (distinct) status. */
static uint32_t rom_cmds_with(uint16_t id)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < fake_rom.n_cmds; i++) {
        n += fake_rom.cmds[i].id == id;
    }
    return n;
}

static void test_fwd_late_same_opcode(void)
{
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 0x1000u;
    bring_up();
    link_up(0);
    link_up(1);
    uint8_t h80[2] = { 0x80, 0x00 }, h81[2] = { 0x81, 0x00 };
    /* A: 041D for 0x80, consumed without a reply until after its deadline */
    fake_rom.hold_replies = true;
    send_cmd(0x041Du, h80, 2);
    pump();
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x041Du) && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT, "A answered 0x22 at the deadline");
    /* B: the same opcode (other link) while A's late reply can still come: not forwarded yet */
    t_now_ms += 100u;
    send_cmd(0x041Du, h81, 2);
    pump();
    CHECK(rom_cmds_with(0xE407u) == 1u && pending_pkts() == 0u && dbg().fwd_pending == 0u,
          "B held in the TX ring (ROM saw %u x 041D)", rom_cmds_with(0xE407u));
    CHECK(stats().fwd_late_held == 1u, "fwd_late_held %u", stats().fwd_late_held);
    /* A's late reply (status 0x00) arrives, then B is forwarded and the ROM answers it 0x0C */
    fake_rom.hold_replies = false;
    fake_rom_set_reply_status(0xE407u, 0x00);
    fake_rom_release_held();
    fake_rom_set_reply_status(0xE407u, 0x0C);
    pump();
    uint32_t n41d = 0u;
    uint8_t st41d = 0xFFu;
    while ((p = next_pkt()) != NULL) {
        if (is_reply(p, 0x041Du)) {
            n41d++;
            st41d = status_of(p);
        }
    }
    CHECK(n41d == 1u && st41d == 0x0Cu, "B got exactly its own reply (%u replies, status 0x%02X, want 0x0C)", n41d,
          st41d);
    CHECK(rom_cmds_with(0xE407u) == 2u && stats().fwd_late_reply == 1u && stats().evt_unsolicited_reply == 0u,
          "late reply consumed once, B forwarded after it");
    /* the late reply never comes: B waits at most BT_CTRL_FWD_DEADLINE_MS from A's deadline, and B's own deadline
     * runs from when it was first held (inside the host's 8 s command timeout) */
    fake_rom.hold_replies = true;
    send_cmd(0x041Du, h80, 2);
    pump();
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x041Du) && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT, "C answered 0x22 at the deadline");
    t_now_ms += 1000u;
    send_cmd(0x041Du, h81, 2);
    pump();
    CHECK(rom_cmds_with(0xE407u) == 3u, "D held");
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS - 1000u;
    pump();
    CHECK(rom_cmds_with(0xE407u) == 4u && stats().fwd_late_expired == 1u && dbg().fwd_pending == 1u,
          "D forwarded once the late reply is given up (expired %u)", stats().fwd_late_expired);
    t_now_ms += 1000u;
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x041Du) && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT && stats().fwd_deadline == 3u,
          "D's deadline counted from when it was held");
    /* an LC command's late reply dies with its link: the same opcode is forwarded at once after the disconnect */
    inject_disc(0x81u, 0x13u);
    pump();
    while (next_pkt() != NULL) {
    }
    CHECK(stats().fwd_late_dropped == 1u, "late reply given up at the disconnect (%u)", stats().fwd_late_dropped);
    send_cmd(0x041Du, h80, 2);
    pump();
    CHECK(rom_cmds_with(0xE407u) == 5u && dbg().fwd_pending == 1u, "forwarded at once after the link went");
    fake_rom.hold_replies = false;
    fake_rom_release_held();
    pump();
    while (next_pkt() != NULL) {
    }
    /* host dispatches next same-opcode command at +0 ms upon receiving 0x22: when the prior late reply never arrives,
     * the held command must still get a non-zero execution window in the ROM instead of timing out in Pass 1 (0 ms) */
    fake_rom.hold_replies = true;
    send_cmd(0x041Du, h80, 2);
    pump();
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x041Du) && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT, "E answered 0x22 at deadline");
    send_cmd(0x041Du, h80, 2);                   /* sent at +0 ms (same tick as 0x22) */
    pump();
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;         /* prior late reply expires -> F forwarded now */
    pump();
    CHECK(dbg().fwd_pending == 1u && pending_pkts() == 0u,
          "held command sent at +0 ms must not 0-ms-timeout in the pass that forwards it");
    t_now_ms += 20u;
    fake_rom.hold_replies = false;
    fake_rom_set_reply_status(0xE407u, 0x00);
    fake_rom_release_held();
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x041Du) && status_of(p) == 0x00u, "F completed with real ROM status within its window");
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 12. fuzzing                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------- */
static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void check_invariants(void)
{
    bt_ctrl_debug_t d = dbg();
    CHECK(d.evt_used <= BT_CTRL_EVT_SLOTS && d.acl_used <= BT_CTRL_ACL_RX_SLOTS && d.held_acl <= BT_CTRL_HELD_ACL &&
          d.h2c_inflight <= BT_CTRL_H2C_ACL_CREDITS + 0u && d.evt_resv <= 1u && d.tx_free <= BT_CTRL_TX_SLOTS,
          "invariant: evt %u acl %u held %u inflight %u resv %u", d.evt_used, d.acl_used, d.held_acl, d.h2c_inflight,
          d.evt_resv);
    CHECK(!d.c2h_fc || d.c2h_outstanding <= d.c2h_credits, "c2h window exceeded");
}

static const uint16_t k_rom_ids[] = { 0xE0D2, 0xE456, 0xE457, 0xE458, 0xE459, 0xE45A, 0xE45D, 0xE45E, 0xE462, 0xE464,
                                      0xE465, 0xE466, 0xE468, 0xE472, 0xE473, 0xE474, 0xE477, 0xE478, 0xE079, 0xE0C9,
                                      0xE43A, 0xE06A, 0x1234, 0xE47D, 0xE0D0, 0xE0D1, 0xE0D3, 0xE0D4, 0xE45B, 0xE45C,
                                      0xE45F, 0xE460, 0xE461, 0xE463, 0xE467, 0xE469, 0xE46A, 0xE46B, 0xE46C, 0xE46D,
                                      0xE471, 0xE475, 0xE476, 0xE479, 0xE47A, 0xE47B, 0xE47C, 0xE42E, 0xE431, 0xE06E,
                                      0xE076, 0xE0CB, 0xE441, 0xE443, 0xE0D6, 0xE46E };

static void return_all_c2h(void)
{
    /* the host returns every outstanding credit, one 0C35 per handle */
    for (uint16_t h = 0x80; h <= 0x86; h++) {
        if (bt_ctrl_check_send_available()) host_nocp(h, 0xFFFF);
        pump();
    }
}

static void test_fuzz_rx(void)
{
    for (uint32_t seed = 1; seed <= 6; seed++) {
        rng_state = 0x9E3779B9u * seed;
        setup();
        bring_up();
        link_up(0); link_up(1); link_up(2);
        uint8_t buf[1024];
        for (int it = 0; it < 4000; it++) {
            for (uint32_t k = 0; k < sizeof(buf); k++) buf[k] = (uint8_t)rnd();
            switch (rnd() % 9) {
            case 0: case 1: {
                uint16_t id = k_rom_ids[rnd() % (sizeof(k_rom_ids) / sizeof(k_rom_ids[0]))];
                uint16_t len = (uint16_t)(rnd() % 40);
                if (id == 0xE47D) {               /* raw descriptor with a NULL buffer: must be rejected */
                    bt_rom_acl_desc_t dd; memset(&dd, 0, sizeof(dd)); dd.length = 10;
                    fake_rom_inject(id, (const uint8_t *)&dd, (uint16_t)sizeof(dd));
                } else {
                    fake_rom_inject(id, buf, len);
                }
                break;
            }
            case 2: case 3:
                (void)fake_rom_inject_acl((uint16_t)(0x80 + rnd() % 4), (uint8_t)(rnd() % 16), buf,
                                          (uint16_t)(rnd() % 1025));
                break;
            case 4: {
                if (bt_ctrl_check_send_available()) {
                    uint8_t p[1 + 4 * 3];
                    uint8_t nh = (uint8_t)(rnd() % 4);
                    p[0] = nh;
                    for (int i = 0; i < 3; i++) { p[1 + 4 * i] = (uint8_t)(0x80 + rnd() % 4); p[2 + 4 * i] = 0;
                                                   p[3 + 4 * i] = (uint8_t)(rnd() % 3); p[4 + 4 * i] = 0; }
                    send_cmd(0x0C35u, p, (uint8_t)(rnd() % 4 == 0 ? rnd() % 13 : 1u + 4u * nh));
                }
                break;
            }
            case 5: t_recv_fail = (int)(rnd() % 3); (void)bt_ctrl_dlv_run(); t_recv_fail = 0; break;
            case 6: fake_rom_raise_irq(rnd()); break;
            case 7: (void)bt_ctrl_dlv_run(); break;
            default: (void)bt_ctrl_run(); break;
            }
            check_invariants();
            if (fake_rom.errors != 0u) break;
        }
        t_recv_fail = 0;
        pump();
        return_all_c2h();
        pump();
        CHECK(fake_rom.errors == 0u, "RX fuzz seed %u: %s", seed, fake_rom.first_error);
        CHECK(stats().cmd_unknown == 0u, "RX fuzz: unknown opcode");
        shutdown_and_check();
    }
}

static void test_fuzz_tx(void)
{
    uint32_t n = 0;
    const bt_ctrl_cmd_row_t *rows = bt_ctrl_cmds_table(&n);
    for (uint32_t seed = 1; seed <= 6; seed++) {
        rng_state = 0x85EBCA6Bu * seed + 1u;
        setup();
        fake_rom.inq_auto_complete = true;   /* HP gate build: a fuzzed 0401 must not SAVE every later 0419 for good */
        bring_up();
        link_up(0); link_up(1);
        uint32_t expect_replies = 0, got_replies = 0;
        uint32_t rx0 = t_rx_n;
        static uint8_t buf[1100];
        for (int it = 0; it < 4000; it++) {
            uint32_t a = rnd() % 10;
            if (a < 6 && bt_ctrl_check_send_available()) {
                uint32_t kind = rnd() % 20;
                uint16_t len;
                if (kind < 13) {                  /* command */
                    uint16_t op;
                    uint32_t pick = rnd() % 10;
                    op = pick < 8 ? rows[rnd() % n].op : pick == 8 ? (uint16_t)(0xFC00 | (rnd() & 0x3FF))
                                                                   : (uint16_t)rnd();
                    if (op == 0x0C03u && rnd() % 4 != 0) op = 0x0C1Au;   /* fewer resets */
                    uint8_t plen = (uint8_t)(rnd() % 256);
                    if (rnd() % 2) plen = (uint8_t)(rnd() % 12);
                    for (uint32_t k = 0; k < plen; k++) buf[4 + k] = (uint8_t)rnd();
                    if (rnd() % 2) { buf[4] = (uint8_t)(0x80 + rnd() % 3); buf[5] = 0; }
                    len = h4_cmd(buf, op, buf + 4, plen);
                    bool malformed = rnd() % 25 == 0;
                    if (malformed) len = (uint16_t)(len - 1u);
                    else if (op != 0x0C35u) expect_replies++;
                } else if (kind < 19) {           /* ACL */
                    uint16_t dl = (uint16_t)(rnd() % 1022);
                    if (rnd() % 30 == 0) dl = (uint16_t)(1022 + rnd() % 3);   /* oversize: host bug */
                    uint16_t hdr = (uint16_t)((0x80 + rnd() % 4) | ((rnd() % 4) << 12));
                    buf[0] = 2; buf[1] = (uint8_t)hdr; buf[2] = (uint8_t)(hdr >> 8);
                    buf[3] = (uint8_t)dl; buf[4] = (uint8_t)(dl >> 8);
                    len = (uint16_t)(5u + dl);
                    if (len > 1026) len = 1026;
                } else {                          /* garbage type */
                    buf[0] = (uint8_t)(rnd() | 0x08);
                    len = (uint16_t)(1 + rnd() % 30);
                }
                bt_ctrl_send(buf, len);
            } else if (a == 6) {
                (void)bt_ctrl_dlv_run();
            } else if (a == 7) {
                fake_rom_ack_acl(rnd() % 5);
            } else {
                (void)bt_ctrl_run();
            }
            check_invariants();
            if (fake_rom.errors != 0u) break;
        }
        pump();
        fake_rom_ack_acl(100);
        pump();
        for (uint32_t i = rx0; i < t_rx_n; i++) {
            got_replies += t_rx[i].d[0] == BT_H4_EVT && (t_rx[i].d[1] == BT_HCI_EVT_CC || t_rx[i].d[1] == BT_HCI_EVT_CS);
        }
        CHECK(got_replies == expect_replies, "TX fuzz seed %u: %u replies for %u well-formed commands", seed,
              got_replies, expect_replies);
        CHECK(dbg().tx_free == BT_CTRL_TX_SLOTS, "TX fuzz: ring not drained (%u free)", dbg().tx_free);
        CHECK(fake_rom.errors == 0u, "TX fuzz seed %u: %s", seed, fake_rom.first_error);
        return_all_c2h();
        shutdown_and_check();
    }
}

/* A peer detach right after a device -> peer ACL burst: every controller credit in flight at the ROM (no acks), more
 * in the TX ring. The ROM flushes the link's TX (NOCP for the flushed count, then E458, ROM 0x2A8B4); bt_ctrl drops
 * the queued ACL, owes no NOCP for the dead handle, keeps no credit, and the next link on the same handle sends at
 * once. */
static void test_detach_with_acl_in_flight(void)
{
    setup();
    bring_up();
    fake_peer_cfg_t c;
    phone_cfg(&c);
    fake_peer_init(&c);
    phone_connects();
    fake_rom.auto_ack_acl = false;                   /* the peer stops acking: everything stays "in air" */
    for (uint32_t i = 0u; i < BT_CTRL_H2C_ACL_CREDITS; i++) {
        send_acl(0x80, 2, 600, (uint8_t)i);
        pump();
    }
    send_acl(0x80, 2, 600, 0x55);                    /* credit-blocked in the TX ring */
    send_acl(0x80, 2, 600, 0x56);
    pump();
    CHECK(dbg().h2c_inflight == BT_CTRL_H2C_ACL_CREDITS && fake_rom_em_tx_in_use() == BT_CTRL_H2C_ACL_CREDITS &&
          dbg().tx_free == BT_CTRL_TX_SLOTS - 2u, "burst: %u in flight, %u EM TX used, %u TX slots free",
          dbg().h2c_inflight, fake_rom_em_tx_in_use(), dbg().tx_free);
    uint32_t f0 = t_faults;
    fake_peer_disconnect(0x13);
    pump();
    CHECK(fake_peer.tx_flushed == BT_CTRL_H2C_ACL_CREDITS && fake_rom_em_tx_in_use() == 0u, "ROM flush: %u flushed, "
          "%u EM TX still used", fake_peer.tx_flushed, fake_rom_em_tx_in_use());
    CHECK(dbg().h2c_inflight == 0u && dbg().nocp_pending == 0u && dbg().tx_free == BT_CTRL_TX_SLOTS,
          "after the detach: %u in flight, %u NOCP owed, %u TX slots free", dbg().h2c_inflight, dbg().nocp_pending,
          dbg().tx_free);
    CHECK(t_faults == f0 && stats().invariant == 0u, "detach with ACL in flight faulted (site %u a=0x%X b=0x%X)",
          stats().fault_site, stats().fault_a, stats().fault_b);
    bool disc = false;
    for (const pkt_t *p = next_pkt(); p != NULL; p = next_pkt()) {
        if (is_evt(p, 0x05)) disc = true;
    }
    CHECK(disc, "Disconnection Complete delivered");
    fake_rom.auto_ack_acl = true;
    phone_connects();                                /* same handle 0x80 */
    uint32_t n0 = fake_rom.n_acl;
    for (uint32_t i = 0u; i < BT_CTRL_H2C_ACL_CREDITS; i++) {
        send_acl(0x80, 2, 100, (uint8_t)(0x60u + i));
        pump();
    }
    CHECK(fake_rom.n_acl == n0 + BT_CTRL_H2C_ACL_CREDITS && stats().acl_tx_em_fail == 0u && t_faults == f0,
          "new link: %u of %u packets sent, EM fails %u", fake_rom.n_acl - n0, BT_CTRL_H2C_ACL_CREDITS,
          stats().acl_tx_em_fail);
    fake_peer_init(NULL);
    shutdown_and_check();
}

/* Silicon bench 2026-11-24 (BT=1 BENCH=1, CRC 0x3599): the pairing window reopened while the ROM still held the
 * laptop's dead link plus a second one; 0C1A answered 0x0C and bt_ctrl stopped BT (INVARIANT, class M). The ROM's
 * Write Scan Enable handler (0x3EAC4) refuses page scan (2, 3) with 0x0C whenever more than one ACL link is up: a
 * defined controller answer, so it is passed through, counted and noted, never a BT fault. */
static void test_scan_refused_two_links(void)
{
    setup();
    bring_up();
    uint8_t v = 0x03u;
    link_up(0);
    const pkt_t *r = roundtrip(0x0C1Au, &v, 1u);
    if (r) CHECK(status_of(r) == 0x00u, "one link: 0C1A 03 accepted (%02X)", status_of(r));
    link_up(1);
    uint32_t f0 = t_faults, inv0 = stats().invariant;
    r = roundtrip(0x0C1Au, &v, 1u);
    if (r) CHECK(status_of(r) == 0x0Cu, "two links: 0C1A 03 refused by the ROM, status passed through (%02X)",
                 status_of(r));
    CHECK(t_faults == f0 && stats().invariant == inv0 && stats().mandatory_fwd_fail == 0u,
          "0C1A refused with two links posted a BT fault (faults %u, site %u a=0x%X b=0x%X)", t_faults - f0,
          stats().fault_site, stats().fault_a, stats().fault_b);
    CHECK(stats().scan_refused == 1u, "scan_refused %u", stats().scan_refused);
    bt_ctrl_note_t n;
    int got = 0, refused = 0;
    while (bt_ctrl_note_pop(&n)) {
        got++;
        if (n.kind == BT_CTRL_NOTE_SCAN_REFUSED && n.status == 0x0Cu && n.reason == 0x03u) refused++;
    }
    CHECK(refused == 1, "scan refusal note (%d notes, %d refusals)", got, refused);
    v = 0x02u;                                       /* window closing: page scan only, still two links */
    r = roundtrip(0x0C1Au, &v, 1u);
    if (r) CHECK(status_of(r) == 0x0Cu, "two links: 0C1A 02 refused too (%02X)", status_of(r));
    CHECK(t_faults == f0 && stats().scan_refused == 2u, "0C1A 02 refused: no fault, counted");
    v = 0x01u;                                       /* inquiry scan only: never refused */
    r = roundtrip(0x0C1Au, &v, 1u);
    if (r) CHECK(status_of(r) == 0x00u, "0C1A 01 accepted with two links (%02X)", status_of(r));
    fake_rom_link(1, false);
    v = 0x03u;
    r = roundtrip(0x0C1Au, &v, 1u);
    if (r) CHECK(status_of(r) == 0x00u, "back to one link: 0C1A 03 accepted (%02X)", status_of(r));
    /* any other class-M failure (here 0C1A with an invalid value) is still a BT fault, with its site */
    fake_rom_set_reply_status(0xE02Cu, 0x12u);
    r = roundtrip(0x0C1Au, &v, 1u);
    CHECK(t_faults == f0 + 1u && stats().fault_site == BT_CTRL_SITE_CLASS_M_STATUS && stats().fault_a == 0x0C1Au &&
          stats().fault_b == 0x12u, "0C1A status 12: fault %u site %u a=0x%X b=0x%X", t_faults - f0,
          stats().fault_site, stats().fault_a, stats().fault_b);
    fake_rom_set_reply_status(0xE02Cu, 0x00u);
    shutdown_and_check();
}

/* bt_ctrl_note_pop() without the HCI trace notes of the trace build (test_hci_trace checks those) */
static int note_pop_nt(bt_ctrl_note_t *n)
{
    while (bt_ctrl_note_pop(n)) {
#if defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE
        if (n->kind == BT_CTRL_NOTE_TRACE) {
            continue;
        }
#endif
        return 1;
    }
    return 0;
}

static int note_line_is(const char *want)
{
    bt_ctrl_note_t n;
    char line[BT_CTRL_NOTE_LINE];
    if (!note_pop_nt(&n)) {
        printf("    no note, want '%s'\n", want);
        return 0;
    }
    uint16_t len = bt_ctrl_note_format(&n, line, sizeof(line));
    if (strcmp(line, want) != 0 || len != strlen(want)) {
        printf("    note '%s' (%u), want '%s'\n", line, len, want);
        return 0;
    }
    return 1;
}

/* Link notes for the session log: what the ROM reported about links, and how the host's 0406 was answered. */
static void test_link_notes(void)
{
    setup();
    bring_up();
    bt_ctrl_note_t n;
    while (bt_ctrl_note_pop(&n)) {
    }
    inject_conn_req(1);
    link_up(0);
    inject_conn_cmp(0x80);
    pump();
    uint8_t d[3] = { 0x80, 0x00, 0x13 };
    const pkt_t *r = roundtrip(0x0406u, d, 3u);       /* forwarded: the ROM's CS */
    if (r) CHECK(status_of(r) == 0x00u, "0406 CS %02X", status_of(r));
    inject_disc(0x80, 0x16);
    pump();
    r = roundtrip(0x0406u, d, 3u);                    /* dead handle: answered locally */
    if (r) CHECK(status_of(r) == BT_HCI_ERR_UNKNOWN_CONN, "0406 on a dead handle %02X", status_of(r));
    CHECK(note_line_is("BT HCI connection request"), "conn request note");
    CHECK(note_line_is("BT HCI connection complete h=0x0080 st=0x00"), "conn complete note");
    CHECK(note_line_is("BT HCI disconnect cmd h=0x0080 st=0x00 rom"), "0406 CS note");
    CHECK(note_line_is("BT HCI disconnection complete h=0x0080 st=0x00 reason=0x16"), "disconnection note");
    CHECK(note_line_is("BT HCI disconnect cmd h=0x0080 st=0x02 local"), "0406 local note");
    CHECK(!note_pop_nt(&n), "no further note");
#if !(defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE)   /* each event adds a trace note there: depth in test_hci_trace */
    uint32_t lost0 = stats().notes_lost;
    for (uint32_t i = 0u; i < BT_CTRL_NOTES + 3u; i++) {
        inject_disc(0x81, 0x08);
    }
    pump();
    CHECK(stats().notes_lost == lost0 + 3u, "notes lost %u, want 3", stats().notes_lost - lost0);
    uint32_t k = 0u;
    while (bt_ctrl_note_pop(&n)) {
        k++;
    }
    CHECK(k == BT_CTRL_NOTES, "%u notes kept, want %u", k, BT_CTRL_NOTES);
#endif
    char small[12];
    n.kind = BT_CTRL_NOTE_DISC;
    uint16_t len = bt_ctrl_note_format(&n, small, sizeof(small));
    CHECK(len == sizeof(small) - 1u && small[len] == '\0', "format truncates within the buffer (%u)", len);
    shutdown_and_check();
}

/* Silicon session 6: the prompt showed the laptop's address; no log said whether a name request failed or its answer
 * came late. The 0419 answer and every Remote Name Request Complete are noted (status, name length), and the newest
 * successful name is kept for the session (bt_ctrl_peer_name): BTM's own link set-up request is answered to BTM only. */
static void test_name_notes(void)
{
    setup();
    bring_up();
    bt_ctrl_note_t n;
    while (bt_ctrl_note_pop(&n)) {
    }
    uint8_t bda[6];
    char nm[BT_CTRL_PEER_NAME_MAX];
    CHECK(!bt_ctrl_peer_name(bda, nm, sizeof(nm)), "no name before any name complete");
    uint8_t rnr[10] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01, 0, 0, 0 };
    const pkt_t *r = roundtrip(0x0419u, rnr, 10u);
    if (r) CHECK(status_of(r) == 0x00u, "0419 CS %02X", status_of(r));
    fake_rom_set_reply_status(0xE00Cu, 0x0Cu);       /* ROM: a name request already running (env +0x217) */
    r = roundtrip(0x0419u, rnr, 10u);
    if (r) CHECK(status_of(r) == 0x0Cu, "second 0419 CS %02X", status_of(r));
    fake_rom_set_reply_status(0xE00Cu, 0x00u);
    static uint8_t ev[255];
    memset(ev, 0, sizeof(ev));
    ev[0] = 0x22u;                                   /* failed name complete */
    memcpy(&ev[1], rnr, 6u);
    fake_rom_inject(0xE0D3u, ev, 255u);
    pump();
    CHECK(!bt_ctrl_peer_name(bda, nm, sizeof(nm)), "a failed name complete keeps no name");
    ev[0] = 0x00u;                                   /* the late answer, with a name longer than the kept part */
    const char *longname = "martianbfg-with-a-very-long-host-name";
    memcpy(&ev[7], longname, strlen(longname));
    fake_rom_inject(0xE0D3u, ev, 255u);
    pump();
    static const uint8_t bda_msb[6] = { 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };   /* wire order reversed */
    CHECK(bt_ctrl_peer_name(bda, nm, sizeof(nm)) && memcmp(bda, bda_msb, 6u) == 0 &&
          strlen(nm) == BT_CTRL_PEER_NAME_MAX - 1u && strncmp(nm, longname, BT_CTRL_PEER_NAME_MAX - 1u) == 0,
          "peer name '%s'", nm);
    char small[6];
    CHECK(bt_ctrl_peer_name(bda, small, sizeof(small)) && strcmp(small, "marti") == 0, "cut to the caller's buffer");
    CHECK(note_line_is("BT HCI name request st=0x00 rom"), "0419 CS note");
    CHECK(note_line_is("BT HCI name request st=0x0C rom"), "0419 refused note");
    CHECK(note_line_is("BT HCI name complete st=0x22 len=0"), "failed name complete note");
    CHECK(note_line_is("BT HCI name complete st=0x00 len=37"), "name complete note");
    CHECK(!note_pop_nt(&n), "no further note");
    shutdown_and_check();
    CHECK(bt_ctrl_open() == 0, "reopen");
    bring_up();
    CHECK(!bt_ctrl_peer_name(bda, nm, sizeof(nm)), "a new session starts without the old name");
    shutdown_and_check();
}

/* Review of the session 6 fix: a task that pops the name note right when bt_ctrl releases the lock must already find
 * the name (bt_ctrl stores the name before it queues the note). */
static int t_gap_name_notes, t_gap_name_ok;
static void gap_pop_name(void)
{
    bt_ctrl_note_t n;
    while (bt_ctrl_note_pop(&n)) {
        if (n.kind == BT_CTRL_NOTE_NAME && n.status == 0u && n.reason != 0u) {
            uint8_t bda[6];
            char nm[BT_CTRL_PEER_NAME_MAX];
            t_gap_name_notes++;
            t_gap_name_ok += (bt_ctrl_peer_name(bda, nm, sizeof(nm)) && strcmp(nm, "zed") == 0) ? 1 : 0;
        }
    }
}
static void test_name_before_note(void)
{
    setup();
    bring_up();
    bt_ctrl_note_t n;
    while (bt_ctrl_note_pop(&n)) {
    }
    static uint8_t ev[255];
    memset(ev, 0, sizeof(ev));
    memcpy(&ev[1], (const uint8_t[6]){ 1, 2, 3, 4, 5, 6 }, 6u);
    memcpy(&ev[7], "zed", 3u);
    t_gap_name_notes = t_gap_name_ok = 0;
    fake_rom_inject(0xE0D3u, ev, 255u);
    t_exit_hook = gap_pop_name;
    pump();
    t_exit_hook = NULL;
    CHECK(t_gap_name_notes == 1 && t_gap_name_ok == 1, "name stored before its note (%d notes, %d with the name)",
          t_gap_name_notes, t_gap_name_ok);
    shutdown_and_check();
}

/* Session log lines (bt_xrec_fw.c): the fault line takes code, site and values from this session together (review:
 * the firmware printed bt_ctrl_rtos's per-boot first code next to this session's site), and the stats line carries
 * the silent-drop counters that tell "the ROM never sent E458" from "bt_ctrl dropped it". */
static void test_session_lines(void)
{
    setup();
    bring_up();
    char line[BT_CTRL_FAULT_LINE];
    uint16_t len = bt_ctrl_fault_format(line, sizeof(line));
    CHECK(strcmp(line, "BT bt_ctrl fault, first code 0 site 0 a=0x00000000 b=0x00000000") == 0 && len == strlen(line),
          "no fault: '%s'", line);
    uint8_t sco[4] = { 0x03, 0x80, 0x00, 0x00 };     /* session 1: HOST_CONTRACT (malformed H4) */
    bt_ctrl_send(sco, 4u);
    pump();
    (void)bt_ctrl_fault_format(line, sizeof(line));
    CHECK(strcmp(line, "BT bt_ctrl fault, first code 1 site 15 a=0x00000004 b=0x00000003") == 0, "session 1: '%s'",
          line);
    shutdown_and_check();
    /* session 2 of the same boot: no fault yet, then a class-M INVARIANT */
    CHECK(bt_ctrl_open() == 0, "reopen");
    bring_up();
    (void)bt_ctrl_fault_format(line, sizeof(line));
    CHECK(strcmp(line, "BT bt_ctrl fault, first code 0 site 0 a=0x00000000 b=0x00000000") == 0,
          "session 2 before its fault shows session 1's: '%s'", line);
    fake_rom_set_reply_status(0xE02Cu, 0x12u);
    uint8_t v = 0x03u;
    (void)roundtrip(0x0C1Au, &v, 1u);
    fake_rom_set_reply_status(0xE02Cu, 0x00u);
    (void)bt_ctrl_fault_format(line, sizeof(line));
    CHECK(strcmp(line, "BT bt_ctrl fault, first code 6 site 4 a=0x00000C1A b=0x00000012") == 0, "session 2: '%s'",
          line);
    char small[20];
    len = bt_ctrl_fault_format(small, sizeof(small));
    CHECK(len == sizeof(small) - 1u && small[len] == '\0', "fault line truncates within the buffer (%u)", len);
    char sl[BT_CTRL_STATS_LINE];
    len = bt_ctrl_stats_format(sl, sizeof(sl));
    bt_ctrl_stats_t s = stats();
    char want[BT_CTRL_STATS_LINE];
    snprintf(want, sizeof(want), "BT ctrl stats unk=%u short=%u late=%u unsol=%u fwd_to=%u late_reply=%u "
             "late_drop=%u scan_refused=%u notes_lost=%u", s.rom_unknown_ids, s.rom_short, s.late_upcalls,
             s.evt_unsolicited_reply, s.fwd_deadline, s.fwd_late_reply, s.fwd_late_dropped, s.scan_refused,
             s.notes_lost);
    CHECK(strcmp(sl, want) == 0 && len == strlen(want), "stats line '%s'", sl);
    shutdown_and_check();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* 13. headphones path (docs/BT_A2DP_DESIGN.md §7, §8.2, milestone H2): inquiry, inquiry cancel, create connection,  */
/*     create connection cancel, link view, ACL pacing. The FWD cases run in the BT_CTRL_HP_GATE_LIST build          */
/*     (make test-bt-ctrl builds both); the default build checks that the rows stay refused without touching the ROM.*/
/* ------------------------------------------------------------------------------------------------------------- */
static const uint8_t k_hp[6] = { 0x51, 0x52, 0x53, 0x54, 0x55, 0x56 };      /* the headset (wire order) */
static const uint8_t k_dev1[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };    /* a phone nearby */
static const uint8_t k_dev2[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };    /* a speaker that does not answer names */

static int hp_gated_on(void)
{
    return bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(0x0401u)) == BTC_FWD;
}

static void hp_cfg(fake_peer_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->addr, k_hp, 6);
    c->cod[0] = 0x18; c->cod[1] = 0x04; c->cod[2] = 0x24;   /* 0x240418: Audio+Rendering, AV / headphones */
    c->io_cap = 0x03;                                       /* NoInputNoOutput */
    c->key_type = 0x04;                                     /* unauthenticated combination (Just Works) */
    static const uint8_t f0[8] = { 0xBF, 0xFE, 0xCF, 0xFE, 0xDB, 0xFF, 0x7B, 0x87 };
    memcpy(c->features, f0, 8);
    c->features_p1[0] = 0x01;
    c->max_page = 1;
    c->lmp_ver = 0x09;
    c->manuf = 0x001D;
    c->clk_off = 0x2468;
    c->psrm = 1;
    c->rssi = -52;
    c->discoverable = true;
    memcpy(c->name, "HP-Fake", 7);
}

static void hp_devices(void)
{
    fake_dev_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.addr, k_dev1, 6);
    d.cod[0] = 0x0C; d.cod[1] = 0x02; d.cod[2] = 0x5A;     /* smartphone */
    d.psrm = 1;
    d.clk_off = 0x1111;
    d.answers_name = true;
    memcpy(d.name, "Phone-1", 7);
    CHECK(fake_rom_add_device(&d) == 0, "add device 1");
    memset(&d, 0, sizeof(d));
    memcpy(d.addr, k_dev2, 6);
    d.cod[0] = 0x14; d.cod[1] = 0x04; d.cod[2] = 0x24;     /* loudspeaker */
    d.psrm = 2;
    d.clk_off = 0x2222;
    CHECK(fake_rom_add_device(&d) == 0, "add device 2");
}

static uint8_t hp_create_params(uint8_t *p, const uint8_t *addr)
{
    memset(p, 0, 13);
    memcpy(p, addr, 6);
    p[6] = 0x18; p[7] = 0xCC;          /* DM1/DH1/DM3/DH3/DM5/DH5, as btm_acl builds it with EDR not excluded */
    p[8] = 0x01;                       /* page scan repetition mode R1 */
    p[11] = 0x80;                      /* clock offset invalid */
    p[12] = 0x00;                      /* allow role switch: no (page-0 bit 5 is masked) */
    return 13;
}

static const uint8_t k_giac_8[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };          /* GIAC, 10.24 s, unlimited */

static void hp_note_expect(uint8_t kind, uint8_t status, uint8_t local, const char *what)
{
    bt_ctrl_note_t n;
    int found = 0;
    while (bt_ctrl_note_pop(&n)) {
        if (n.kind == kind) {
            found = 1;
            CHECK(n.status == status && n.local == local, "%s note: st %02X local %u", what, n.status, n.local);
            break;
        }
    }
    CHECK(found, "%s note missing", what);
}

/* Both builds: the default refuses the rows locally (no ROM contact) and keeps their 1002 bits clear; 040F/080B/080D
 * and 0C45 stay refused in both (design §7). */
static void test_hp_gates(void)
{
    setup();
    bring_up();
    uint8_t prm[16];
    static const uint16_t hp_ops[] = { 0x0401u, 0x0402u, 0x0405u, 0x0408u };
    const uint8_t *bits = bt_ctrl_cmds_supported();
    static const uint8_t mask[] = { 0x01, 0x02, 0x10, 0x80 };
    for (size_t i = 0; i < 4; i++) {
        uint8_t eff = bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(hp_ops[i]));
        CHECK(eff == (hp_gated_on() ? BTC_FWD : BTC_REJ), "%04X effective action %u", hp_ops[i], eff);
        CHECK(((bits[0] & mask[i]) != 0) == (eff == BTC_FWD), "%04X 1002 bit follows the gate", hp_ops[i]);
    }
    if (!hp_gated_on()) {
        uint32_t c0 = fake_rom.n_cmds;
        const pkt_t *r = roundtrip(0x0401u, k_giac_8, 5);
        CHECK(r && r->d[1] == BT_HCI_EVT_CS && status_of(r) == 0x01u, "0401 REJ CS 0x01");
        r = roundtrip(0x0402u, NULL, 0);
        CHECK(r && r->d[1] == BT_HCI_EVT_CC && r->d[2] == 4 && status_of(r) == 0x01u, "0402 REJ CC(4) 0x01");
        r = roundtrip(0x0405u, prm, hp_create_params(prm, k_hp));
        CHECK(r && r->d[1] == BT_HCI_EVT_CS && status_of(r) == 0x01u, "0405 REJ CS 0x01");
        r = roundtrip(0x0408u, k_hp, 6);
        CHECK(r && r->d[2] == 10 && status_of(r) == 0x01u && memcmp(&r->d[7], k_hp, 6) == 0, "0408 REJ CC(10) echo");
        CHECK(fake_rom.n_cmds == c0, "gated rows reached the ROM");
    }
    CHECK(bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(0x040Fu)) == BTC_REJ &&
          bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(0x080Bu)) == BTC_REJ &&
          bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(0x080Du)) == BTC_REJ &&
          bt_ctrl_cmds_eff(bt_ctrl_cmds_lookup(0x0C45u)) == BTC_REJ, "040F/080B/080D/0C45 stay REJ");
    uint8_t p0[8];
    bt_ctrl_cmds_features_p0(p0);
    CHECK((p0[0] & 0x20u) == 0u && (p0[3] & 0x40u) == 0u, "no role switch, no RSSI inquiry advertised");
    bool zl = false;
    uint8_t ml = 0xFF;
    zl = bt_ctrl_cmds_fwd_layout(0x0402u, &ml);
    CHECK(zl && ml == 0u && bt_ctrl_cmds_fwd_min_len(0x0402u) == 0u, "0402 has a verified zero-length layout");
    CHECK(!bt_ctrl_cmds_fwd_layout(0x0403u, NULL), "0403 has no layout");
    shutdown_and_check();
}

/* Inquiry: results with the CoD and clock offset de-padded, 0405 refused 0x0C by the ROM while inquiring, a name
 * request SAVED behind the inquiry and answered after Inquiry Complete, notes, page scan off around it (design §6.2). */
static void test_hp_inquiry(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    setup();
    bring_up();
    fake_peer_init(&c);
    hp_devices();
    uint8_t v = 0x02;
    (void)roundtrip(0x0C1Au, &v, 1);
    CHECK(bt_ctrl_scan_enable() == 0x02u, "scan accessor: page scan");
    v = 0x00;
    (void)roundtrip(0x0C1Au, &v, 1);                         /* the session turns page scan off first */
    CHECK(bt_ctrl_scan_enable() == 0x00u && fake_rom.scan_enable == 0u, "page scan off before the inquiry");
    while (bt_ctrl_note_pop(&(bt_ctrl_note_t){0})) {}
    uint32_t c0 = fake_rom.n_cmds;
    send_cmd(0x0401u, k_giac_8, 5);
    pump();
    CHECK(fake_rom.n_cmds == c0 + 1u && fake_rom.cmds[c0].id == 0xE000u && fake_rom.cmds[c0].len == 5u &&
          memcmp(fake_rom.cmds[c0].data, k_giac_8, 5) == 0, "0401 -> E000, 5 B wire layout");
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0401u) && p->d[1] == BT_HCI_EVT_CS && status_of(p) == 0u, "0401 CS 0");
    static const uint8_t r_hp[] = { 4, 0x02, 15, 1, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x01, 0x00, 0x00,
                                    0x18, 0x04, 0x24, 0x68, 0x24 };
    static const uint8_t r_d1[] = { 4, 0x02, 15, 1, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x01, 0x00, 0x00,
                                    0x0C, 0x02, 0x5A, 0x11, 0x11 };
    static const uint8_t r_d2[] = { 4, 0x02, 15, 1, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x02, 0x00, 0x00,
                                    0x14, 0x04, 0x24, 0x22, 0x22 };
    p = next_pkt();
    CHECK(p && same(p->d, p->len, r_hp, sizeof(r_hp)), "inquiry result: headset (CoD 240418, clkoff 2468)");
    p = next_pkt();
    CHECK(p && same(p->d, p->len, r_d1, sizeof(r_d1)), "inquiry result: phone");
    p = next_pkt();
    CHECK(p && same(p->d, p->len, r_d2, sizeof(r_d2)), "inquiry result: speaker");
    CHECK(pending_pkts() == 0u && bt_ctrl_inquiry_active() == 1 && fake_rom.lm_state == 4u, "inquiry still running");
    hp_note_expect(BT_CTRL_NOTE_INQ_CMD, 0u, 0u, "inquiry cmd");

    /* 0405 while inquiring: the ROM refuses it at once (LM state 4 -> 0x0C, not SAVED) */
    uint8_t prm[16];
    const pkt_t *r = roundtrip(0x0405u, prm, hp_create_params(prm, k_hp));
    CHECK(r && r->d[1] == BT_HCI_EVT_CS && status_of(r) == 0x0Cu, "0405 during inquiry: CS 0x0C from the ROM");
    CHECK(fake_rom.page_refused == 1u && fake_rom.pages == 0u && !fake_peer.connected, "no page started");
    hp_note_expect(BT_CTRL_NOTE_PAGE_CMD, 0x0Cu, 0u, "create connection (refused)");

    /* 0419 while inquiring: SAVED (map §2), the one-command window stays shut until the inquiry ends */
    uint8_t rnr[10] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x01, 0x00, 0x00, 0x00 };
    send_cmd(0x0419u, rnr, 10);
    pump();
    CHECK(pending_pkts() == 0u && dbg().fwd_pending == 1u && fake_rom.saved == 1u, "0419 SAVED behind the inquiry");
    fake_rom_inquiry_end();
    pump();
    p = next_pkt();
    CHECK(is_evt(p, 0x01) && p->d[3] == 0u, "Inquiry Complete first");
    p = next_pkt();
    CHECK(is_reply(p, 0x0419u) && status_of(p) == 0u, "then the saved name request's CS");
    p = next_pkt();
    CHECK(is_evt(p, 0x07) && p->d[3] == 0u && memcmp(&p->d[4], k_dev1, 6) == 0 && memcmp(&p->d[10], "Phone-1", 8) == 0,
          "then its Remote Name Request Complete");
    CHECK(bt_ctrl_inquiry_active() == 0 && fake_rom.saved_reruns == 1u, "inquiry over, saved message re-run once");
    bt_ctrl_note_t n;
    int done_seen = 0;
    while (bt_ctrl_note_pop(&n)) {
        if (n.kind == BT_CTRL_NOTE_INQ_DONE) {
            done_seen = 1;
            CHECK(n.status == 0u && n.reason == 3u, "inquiry complete note: %u results", n.reason);
            char line[BT_CTRL_NOTE_LINE];
            bt_ctrl_note_format(&n, line, sizeof(line));
            CHECK(strcmp(line, "BT HCI inquiry complete st=0x00 results=3") == 0, "note line '%s'", line);
        }
    }
    CHECK(done_seen, "inquiry complete note");
    /* the speaker does not answer a name page: 0x04 */
    memcpy(rnr, k_dev2, 6);
    r = roundtrip(0x0419u, rnr, 10);
    p = next_pkt();
    CHECK(r == NULL || status_of(r) == 0u, "0419 CS 0");
    /* roundtrip consumed the CS; the name complete is in the same pump */
    CHECK(t_rx_n >= 2u && is_evt(&t_rx[t_rx_n - 1u], 0x07) && t_rx[t_rx_n - 1u].d[3] == 0x04u, "speaker: page timeout");
    (void)p;
    /* mode-1 inquiry result (E0D6 -> 0x22), as a 0C45 probe would see it (0C45 itself stays REJ) */
    fake_rom.inq_mode = 1u;
    t_rx_rd = t_rx_n;
    send_cmd(0x0401u, k_giac_8, 5);
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x0401u), "0401 CS (mode 1)");
    p = next_pkt();
    CHECK(is_evt(p, 0x22) && p->d[2] == 15 && p->d[3] == 1 && memcmp(&p->d[4], k_hp, 6) == 0 && p->d[12] == 0x18 &&
          (int8_t)p->d[17] == -52, "0x22 with RSSI -52");
    fake_rom_inquiry_end();
    pump();
    CHECK(stats().rom_unknown_ids == 0u && stats().rom_short == 0u && stats().fwd_deadline == 0u, "nothing dropped");
    shutdown_and_check();
}

/* Inquiry Cancel: zero-length E001; deferred CC; no Inquiry Complete after a cancel; refused when idle; a CC held past
 * the deadline is answered 0x22 and the late CC consumed. */
static void test_hp_inquiry_cancel(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 5000u;
    bring_up();
    fake_peer_init(&c);
    const pkt_t *r = roundtrip(0x0402u, NULL, 0);
    CHECK(r && r->d[1] == BT_HCI_EVT_CC && r->d[2] == 4 && status_of(r) == 0x0Cu, "0402 while idle: CC 0x0C");
    uint32_t c0 = fake_rom.n_cmds;
    (void)roundtrip(0x0401u, k_giac_8, 5);
    pump();
    t_rx_rd = t_rx_n;
    r = roundtrip(0x0402u, NULL, 0);
    CHECK(fake_rom.cmds[c0 + 1u].id == 0xE001u && fake_rom.cmds[c0 + 1u].len == 0u, "0402 -> E001 with no payload");
    CHECK(r && status_of(r) == 0u && bt_ctrl_inquiry_active() == 0 && fake_rom.inq_cancels == 1u, "0402 CC 0");
    fake_rom_inquiry_end();                                  /* state 0 already: nothing */
    pump();
    CHECK(pending_pkts() == 0u && fake_rom.inq_completes == 0u, "no Inquiry Complete after a cancel");
    /* the LD takes its time: CC held, then the deadline */
    fake_rom.inq_cancel_hold = true;
    (void)roundtrip(0x0401u, k_giac_8, 5);
    pump();
    t_rx_rd = t_rx_n;
    send_cmd(0x0402u, NULL, 0);
    pump();
    CHECK(pending_pkts() == 0u && dbg().fwd_pending == 1u && fake_rom.lm_state == 7u, "deferred CC pending");
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0402u) && p->d[2] == 4 && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT, "0402 answered 0x22 at 7 s");
    fake_rom_inquiry_end();                                  /* the LD stops: the late CC */
    pump();
    CHECK(pending_pkts() == 0u && stats().fwd_late_reply == 1u && fake_rom.inq_completes == 0u,
          "late CC consumed, no Inquiry Complete");
    /* the host's next inquiry is not wedged */
    r = roundtrip(0x0401u, k_giac_8, 5);
    CHECK(r && status_of(r) == 0u, "next inquiry starts");
    fake_rom_inquiry_end();
    pump();
    shutdown_and_check();
}

/* 0401 refusals: bad length (0x12), LM state 1 (SAVED -> 0x22 at the deadline, results arrive later as unsolicited
 * events and the late CS is consumed), LM state 2 with env +0x4b (0x3A), paging (0x0C). */
static void test_hp_inquiry_refusals(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 100u;
    bring_up();
    fake_peer_init(&c);
    uint8_t bad[5] = { 0x33, 0x8B, 0x9E, 0x00, 0x00 };
    const pkt_t *r = roundtrip(0x0401u, bad, 5);
    CHECK(r && status_of(r) == 0x12u, "0401 length 0: 0x12");
    hp_note_expect(BT_CTRL_NOTE_INQ_CMD, 0x12u, 0u, "inquiry (bad length)");
    fake_rom_set_lm_state(1u);
    send_cmd(0x0401u, k_giac_8, 5);
    pump();
    CHECK(pending_pkts() == 0u && fake_rom.saved == 1u, "0401 SAVED in LM state 1");
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0401u) && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT, "0x22 at the deadline");
    hp_note_expect(BT_CTRL_NOTE_INQ_CMD, BT_HCI_ERR_LMP_TIMEOUT, 1u, "inquiry (deadline)");
    fake_rom_set_lm_state(0u);                               /* the saved inquiry runs now */
    pump();
    p = next_pkt();
    CHECK(is_evt(p, 0x02), "its result reaches the host as an event");
    CHECK(stats().fwd_late_reply == 1u && stats().evt_unsolicited_reply == 0u, "late CS consumed");
    fake_rom_inquiry_end();
    pump();
    t_rx_rd = t_rx_n;
    fake_rom.lm_state2_err = true;
    fake_rom_set_lm_state(2u);
    r = roundtrip(0x0401u, k_giac_8, 5);
    CHECK(r && status_of(r) == 0x3Au, "LM state 2 + env 0x4b: 0x3A");
    fake_rom.lm_state2_err = false;
    fake_rom_set_lm_state(3u);
    r = roundtrip(0x0401u, k_giac_8, 5);
    CHECK(r && status_of(r) == 0x0Cu, "LM state 3 (paging): 0x0C");
    fake_rom_set_lm_state(0u);
    pump();
    shutdown_and_check();
}

static uint16_t hp_connect(uint16_t expect_h)
{
    uint8_t prm[16];
    uint32_t c0 = fake_rom.n_cmds;
    send_cmd(0x0405u, prm, hp_create_params(prm, k_hp));
    pump();
    CHECK(fake_rom.cmds[c0].id == 0xE004u && fake_rom.cmds[c0].len == 14u && memcmp(fake_rom.cmds[c0].data, prm, 13) == 0
          && fake_rom.cmds[c0].data[13] == 0u, "0405 -> E004, 13 wire bytes zero-padded to 14");
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0405u) && status_of(p) == 0u, "0405 CS 0");
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[2] == 11 && p->d[3] == 0u && (uint16_t)(p->d[4] | (p->d[5] << 8)) == expect_h &&
          memcmp(&p->d[6], k_hp, 6) == 0 && p->d[12] == 1u, "Connection Complete h=%04X", expect_h);
    CHECK(fake_peer.connected && fake_peer.we_paged && fake_peer.host_pkt_type == 0xCC18u &&
          fake_peer.host_allow_rs == 0u && fake_peer.host_psrm == 1u, "the peer saw our page parameters");
    return p ? (uint16_t)(p->d[4] | (p->d[5] << 8)) : 0u;
}

/* Create Connection as master to the headset: link view, set-up reads on the LC as master, refused link-policy
 * commands, a peer role switch and max-slots change, paced ACL with NOCP, a credit stall, disconnect, reconnect. */
static void test_hp_connect(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 1000u;
    bring_up();
    fake_peer_init(&c);
    uint16_t h = hp_connect(0x0080u);
    bt_ctrl_link_info_t li;
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.up && li.outgoing && li.role == BT_CTRL_ROLE_MASTER &&
          li.bda[0] == k_hp[5] && li.bda[5] == k_hp[0], "link view: up, outgoing, master, BD_ADDR MSB first");
    hp_note_expect(BT_CTRL_NOTE_PAGE_CMD, 0u, 0u, "create connection");
    /* Bluedroid's set-up reads as master (btm_acl.c), then what it sends that stays refused */
    uint8_t hh[4] = { (uint8_t)h, (uint8_t)(h >> 8), 0x01, 0x00 };
    const pkt_t *r = roundtrip(0x041Bu, hh, 2);
    CHECK(r && status_of(r) == 0u, "041B CS");
    r = roundtrip(0x041Du, hh, 2);
    CHECK(r && status_of(r) == 0u, "041D CS");
    r = roundtrip(0x041Cu, hh, 3);
    CHECK(r && status_of(r) == 0u, "041C CS");
    uint32_t c0 = fake_rom.n_cmds;
    uint8_t pt[4] = { (uint8_t)h, (uint8_t)(h >> 8), 0x18, 0xCC };
    r = roundtrip(0x040Fu, pt, 4);
    CHECK(r && r->d[1] == BT_HCI_EVT_CS && status_of(r) == 0x01u, "040F stays REJ (CS 0x01)");
    uint8_t lp[4] = { (uint8_t)h, (uint8_t)(h >> 8), 0x05, 0x00 };
    r = roundtrip(0x080Du, lp, 4);
    CHECK(r && r->d[2] == 6 && status_of(r) == 0x01u && r->d[7] == (uint8_t)h, "080D REJ CC(6) echoing the handle");
    uint8_t ls[4] = { (uint8_t)h, (uint8_t)(h >> 8), 0x00, 0x7D };
    r = roundtrip(0x0C37u, ls, 4);
    CHECK(r && r->d[2] == 6 && status_of(r) == 0x01u, "0C37 (master only) stays REJ");
    CHECK(fake_rom.n_cmds == c0, "refused link-policy commands never reached the ROM");
    /* the headset takes the master role and allows 5-slot packets */
    fake_peer_role_switch(BT_CTRL_ROLE_SLAVE);
    fake_peer_max_slots(5);
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_evt(p, 0x12) && p->d[2] == 8 && p->d[3] == 0u && memcmp(&p->d[4], k_hp, 6) == 0 && p->d[10] == 1u,
          "Role Change: slave");
    p = next_pkt();
    CHECK(is_evt(p, 0x1B) && p->d[2] == 3 && p->d[3] == (uint8_t)h && p->d[5] == 5u, "Max Slots Change: 5");
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.role == BT_CTRL_ROLE_SLAVE && li.role_changes == 1u &&
          li.max_slots == 5u, "link view follows the role change and slots");
    bt_ctrl_note_t n;
    int role_note = 0;
    while (bt_ctrl_note_pop(&n)) {
        if (n.kind == BT_CTRL_NOTE_ROLE) {
            role_note = 1;
            char line[BT_CTRL_NOTE_LINE];
            bt_ctrl_note_format(&n, line, sizeof(line));
            CHECK(n.handle == h && strcmp(line, "BT HCI role change h=0x0080 st=0x00 slave") == 0, "role note '%s'", line);
        }
    }
    CHECK(role_note, "role change note");

    /* media-like ACL: 600 B packets, air 30 kB/s, 20 ms ticks: one NOCP per completed packet batch, the 4-credit gate */
    fake_rom.auto_ack_acl = false;
    fake_rom.air_rate_Bps = 30000u;
    uint32_t sent = 0, nocp = 0, ticks = 0, max_infl = 0;
    for (; ticks < 200u && (sent < 24u || dbg().h2c_inflight != 0u); ticks++) {
        while (sent < 24u && bt_ctrl_check_send_available()) {
            send_acl(h, 2, 596, (uint8_t)sent);
            sent++;
        }
        pump();
        if (dbg().h2c_inflight > max_infl) max_infl = dbg().h2c_inflight;
        t_now_ms += 20u;
        (void)fake_rom_air_tick(20u);
        pump();
        const pkt_t *q;
        while ((q = next_pkt()) != NULL) {
            if (is_evt(q, BT_HCI_EVT_NOCP)) nocp += (uint32_t)(q->d[6] | (q->d[7] << 8));
        }
    }
    CHECK(sent == 24u && nocp == 24u && fake_rom.air_acked == 24u, "24 packets paced to air (%u NOCP)", nocp);
    CHECK(max_infl == BT_CTRL_H2C_ACL_CREDITS, "credit gate at %u", max_infl);
    CHECK(ticks >= 24u && ticks <= 30u, "pacing: %u ticks of 20 ms for 24 x 600 B at 30 kB/s", ticks);
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.acl_sent == 24u && li.acl_completed == 24u &&
          li.nocp_last_ms > 1000u && li.nocp_last_ms <= t_now_ms, "link view counts sent/completed (%u/%u)",
          li.acl_sent, li.acl_completed);
    /* credit stall: the baseband stops acknowledging, nothing detaches (design §4.6 watchdog input) */
    fake_rom.air_stall = true;
    for (int i = 0; i < 6; i++) {
        send_acl(h, 2, 596, (uint8_t)(100 + i));
        pump();
    }
    uint32_t done0 = li.acl_completed;
    for (int i = 0; i < 40; i++) {
        t_now_ms += 20u;
        (void)fake_rom_air_tick(20u);
        pump();
    }
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.acl_completed == done0 && li.up && dbg().h2c_inflight == 4u &&
          pending_pkts() == 0u, "stall: no NOCP for 800 ms, 4 in flight, link still up");
    /* the session's 0406 after the stall: the ROM flushes the air with a NOCP before the Disconnection Complete */
    uint8_t dc[3] = { (uint8_t)h, (uint8_t)(h >> 8), 0x13 };
    send_cmd(0x0406u, dc, 3);
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x0406u) && status_of(p) == 0u, "0406 CS");
    /* the ROM's flush NOCP (4) comes before the Disconnection Complete; the host reclaims those credits from the
     * Disconnection Complete itself, so bt_ctrl drops the NOCP it still owed (apply_event_side_effects) */
    while ((p = next_pkt()) != NULL && is_evt(p, BT_HCI_EVT_NOCP)) {}
    CHECK(is_evt(p, 0x05) && p->d[6] == 0x16u && fake_peer.tx_flushed == 4u, "Disconnection Complete 0x16, 4 flushed");
    fake_rom.air_stall = false;
    pump();
    while (next_pkt() != NULL) {}
    CHECK(bt_ctrl_link_info(h, &li) == 1 && !li.up && dbg().h2c_inflight == 0u, "link down, credits back");
    /* reconnect: a new record on the handle */
    h = hp_connect(0x0080u);
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.up && li.role == BT_CTRL_ROLE_MASTER && li.acl_sent == 0u &&
          li.role_changes == 0u, "fresh link view after the reconnect");
    /* the peer drops while we pause: link down */
    fake_peer_disconnect(0x13);
    pump();
    p = next_pkt();
    CHECK(is_evt(p, 0x05), "remote disconnect");
    shutdown_and_check();
}

/* Create Connection failures: page timeout, already connected (0x0B), LM state 1 SAVED -> 0x22 at the deadline, the
 * same opcode held while the late reply can still come (fwd_late_hold), and a role-switching headset. */
static void test_hp_connect_fail(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    c.ignores_pages = true;
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 77u;
    bring_up();
    fake_peer_init(&c);
    uint8_t prm[16];
    send_cmd(0x0405u, prm, hp_create_params(prm, k_hp));
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0405u) && status_of(p) == 0u, "CS 0");
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[3] == 0x04u && memcmp(&p->d[6], k_hp, 6) == 0, "Connection Complete 0x04 (page TO)");
    bt_ctrl_link_info_t li;
    CHECK(bt_ctrl_link_info(0x80, &li) == 1 && !li.up && fake_rom.page_timeouts == 1u, "no link after a page timeout");
    /* the headset in range now, role switch right after the page */
    hp_cfg(&c);
    c.role_switch_on_connect = true;
    c.max_slots_on_connect = 5;
    fake_peer_init(&c);
    (void)hp_connect(0x0080u);
    p = next_pkt();
    CHECK(is_evt(p, 0x12) && p->d[10] == 1u, "Role Change right after the page");
    p = next_pkt();
    CHECK(is_evt(p, 0x1B) && p->d[5] == 5u, "Max Slots 5");
    CHECK(bt_ctrl_link_info(0x80, &li) == 1 && li.outgoing && li.role == BT_CTRL_ROLE_SLAVE, "outgoing link, now slave");
    const pkt_t *r = roundtrip(0x0405u, prm, hp_create_params(prm, k_hp));
    CHECK(r && status_of(r) == 0x0Bu, "0405 to a connected peer: 0x0B");
    fake_peer_disconnect(0x13);
    pump();
    while (next_pkt() != NULL) {}
    /* LM state 1: SAVED, 0x22 at 7 s; a second 0405 waits for the late reply */
    hp_cfg(&c);
    fake_peer_init(&c);
    fake_rom_set_lm_state(1u);
    send_cmd(0x0405u, prm, hp_create_params(prm, k_hp));
    pump();
    CHECK(pending_pkts() == 0u && fake_rom.saved == 1u, "0405 SAVED in LM state 1");
    t_now_ms += BT_CTRL_FWD_DEADLINE_MS;
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x0405u) && status_of(p) == BT_HCI_ERR_LMP_TIMEOUT, "0405 answered 0x22 at the deadline");
    uint32_t e004 = rom_cmds_with(0xE004u);
    send_cmd(0x0405u, prm, hp_create_params(prm, k_hp));       /* the host retries */
    pump();
    CHECK(rom_cmds_with(0xE004u) == e004 && stats().fwd_late_held == 1u, "retry held while the late reply can come");
    fake_rom_set_lm_state(0u);                                /* the saved page runs: late CS (consumed) + link */
    pump();
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[3] == 0u, "the saved page's Connection Complete reaches the host");
    p = next_pkt();
    CHECK(is_reply(p, 0x0405u) && status_of(p) == 0x0Bu, "the held retry then gets its own reply (0x0B)");
    CHECK(stats().fwd_late_reply == 1u && stats().evt_unsolicited_reply == 0u, "late CS consumed once");
    CHECK(bt_ctrl_link_info(0x80, &li) == 1 && li.up && li.role == BT_CTRL_ROLE_UNKNOWN,
          "a link from a deadline-answered page has an unknown role");
    shutdown_and_check();
}

/* Role Change before Connection Complete (the slave asks for the switch during connection set-up, as TWS earbuds do):
 * the link view applies it to the paged address's link instead of dropping it; the 0405 packet types / role-switch
 * permission and the peer's page-0 features are kept for the linkinfo line (design §7, §10.2). */
static void test_hp_link_view_early_role(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    c.role_switch_before_connect = true;
    setup();
    bt_ctrl_set_clock(clk);
    t_now_ms = 500u;
    bring_up();
    fake_peer_init(&c);
    uint8_t prm[16];
    send_cmd(0x0405u, prm, hp_create_params(prm, k_hp));
    pump();
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0405u) && status_of(p) == 0u, "0405 CS 0");
    p = next_pkt();
    CHECK(is_evt(p, 0x12) && p->d[3] == 0u && p->d[10] == 1u, "Role Change (we: slave) before the Connection Complete");
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[3] == 0u, "Connection Complete");
    uint16_t h = p != NULL ? (uint16_t)(p->d[4] | (p->d[5] << 8)) : 0u;
    bt_ctrl_link_info_t li;
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.up && li.outgoing && li.role == BT_CTRL_ROLE_SLAVE &&
          li.role_changes == 1u, "link view after an early role change: role %u (want slave) changes %u", li.role,
          li.role_changes);
    CHECK(li.req_pkt_type == 0xCC18u && li.req_allow_rs == 0u, "0405 packet types 0x%04X allow_rs %u kept",
          li.req_pkt_type, li.req_allow_rs);
    CHECK(!li.rmt_feat_seen, "no remote features yet");
    uint8_t hh[2] = { (uint8_t)h, (uint8_t)(h >> 8) };
    send_cmd(0x041Bu, hh, 2);
    pump();
    p = next_pkt();
    CHECK(is_reply(p, 0x041Bu) && status_of(p) == 0u, "041B CS 0");
    p = next_pkt();
    CHECK(is_evt(p, 0x0B) && p->d[3] == 0u, "Read Remote Supported Features Complete");
    CHECK(bt_ctrl_link_info(h, &li) == 1 && li.rmt_feat_seen && li.rmt_feat0 == 0xBFu && li.rmt_feat3 == 0xFEu,
          "remote page-0 features kept: seen %u byte0 0x%02X byte3 0x%02X", li.rmt_feat_seen, li.rmt_feat0,
          li.rmt_feat3);
    /* a Role Change for an address with no link and no page pending changes nothing */
    fake_peer_disconnect(0x13);
    pump();
    while (next_pkt() != NULL) {}
    CHECK(bt_ctrl_link_info(h, &li) == 1 && !li.up, "link down");
    shutdown_and_check();
}

/* Create Connection Cancel: while paging (CC 0, then Connection Complete 0x02), after the page won (0x0B), with
 * nothing to cancel (0x02); the CC always echoes the address (CC(10)). */
static void test_hp_create_cancel(void)
{
    if (!hp_gated_on()) return;
    fake_peer_cfg_t c;
    hp_cfg(&c);
    setup();
    bring_up();
    fake_peer_init(&c);
    fake_rom.page_hold = true;
    uint8_t prm[16];
    const pkt_t *r = roundtrip(0x0405u, prm, hp_create_params(prm, k_hp));
    CHECK(r && status_of(r) == 0u && fake_rom.lm_state == 3u, "paging");
    uint32_t c0 = fake_rom.n_cmds;
    send_cmd(0x0408u, k_hp, 6);
    pump();
    CHECK(fake_rom.cmds[c0].id == 0xE005u && fake_rom.cmds[c0].len == 6u, "0408 -> E005 (6 B)");
    const pkt_t *p = next_pkt();
    CHECK(is_reply(p, 0x0408u) && p->d[2] == 10 && status_of(p) == 0u && memcmp(&p->d[7], k_hp, 6) == 0,
          "0408 CC 0 {s, addr}");
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[3] == 0x02u && memcmp(&p->d[6], k_hp, 6) == 0, "then Connection Complete 0x02");
    CHECK(fake_rom.lm_state == 0u && fake_rom.page_cancels == 1u && !fake_peer.connected, "page cancelled");
    /* race: the page completes before the cancel reaches the ROM */
    r = roundtrip(0x0405u, prm, hp_create_params(prm, k_hp));
    fake_rom_page_answer();
    send_cmd(0x0408u, k_hp, 6);
    pump();
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[3] == 0u, "Connection Complete 0 first");
    p = next_pkt();
    CHECK(is_reply(p, 0x0408u) && status_of(p) == 0x0Bu && memcmp(&p->d[7], k_hp, 6) == 0, "then 0408 CC 0x0B");
    bt_ctrl_link_info_t li;
    CHECK(bt_ctrl_link_info(0x80, &li) == 1 && li.up && li.outgoing, "the link stays (the host disconnects it)");
    /* nothing to cancel */
    r = roundtrip(0x0408u, k_dev1, 6);
    CHECK(r && status_of(r) == 0x02u && memcmp(&r->d[7], k_dev1, 6) == 0, "0408 with no page: 0x02");
    /* a held page that times out */
    fake_peer_disconnect(0x13);
    pump();
    while (next_pkt() != NULL) {}
    r = roundtrip(0x0405u, prm, hp_create_params(prm, k_hp));
    fake_rom_page_timeout();
    pump();
    p = next_pkt();
    CHECK(is_evt(p, 0x03) && p->d[3] == 0x04u, "held page times out: 0x04");
    shutdown_and_check();
}

/* An accepted incoming link (phone mode's path) gets its role from the 0409 Role parameter. */
static void test_hp_link_view_accept(void)
{
    fake_peer_cfg_t c;
    phone_cfg(&c);
    setup();
    bring_up();
    fake_peer_init(&c);
    CHECK(fake_peer_page(2) == 0, "page");
    pump();
    t_rx_rd = t_rx_n;
    uint8_t acc[7];
    memcpy(acc, k_phone, 6);
    acc[6] = 0x01;                                           /* remain slave */
    send_cmd(0x0409u, acc, 7);
    pump();
    bt_ctrl_link_info_t li;
    CHECK(bt_ctrl_link_info(0x82, &li) == 1 && li.up && !li.outgoing && li.role == BT_CTRL_ROLE_SLAVE,
          "accepted link: slave, incoming");
    CHECK(bt_ctrl_link_info(0x87, &li) == 0 && bt_ctrl_link_info(0x0001, &li) == 0, "non-ROM handles");
    shutdown_and_check();
}

#if defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE
/* HP_GATES bench images (silicon 2026-11-25): every event to the host is a TRACE link note, except NOCP and inquiry
 * results (too many) and successful CCs outside link control (set-up, reads). 64 notes deep. */
static uint32_t trace_drain(char lines[][BT_CTRL_NOTE_LINE], uint32_t max, uint32_t *other)
{
    bt_ctrl_note_t n;
    uint32_t k = 0u;
    *other = 0u;
    while (bt_ctrl_note_pop(&n)) {
        if (n.kind != BT_CTRL_NOTE_TRACE) {
            (*other)++;
            continue;
        }
        if (k < max) {
            (void)bt_ctrl_note_format(&n, lines[k], BT_CTRL_NOTE_LINE);
        }
        k++;
    }
    return k;
}

static void test_hci_trace(void)
{
    static char l[80][BT_CTRL_NOTE_LINE];
    uint32_t other;
    CHECK(BT_CTRL_NOTES == 64u, "BT_CTRL_NOTES %u in the trace build, want 64", BT_CTRL_NOTES);
    setup();
    bring_up();
    /* controller.c start-up: successful CCs of 0C03, 1005, ... (none link control): no trace */
    uint32_t k = trace_drain(l, 80u, &other);
    CHECK(k == 0u, "start-up traced %u events (first '%s')", k, k ? l[0] : "");
    /* NOCP and inquiry results: not traced */
    link_up(0);
    inject_conn_cmp(0x80);
    pump();
    (void)trace_drain(l, 80u, &other);
    fake_rom.auto_ack_acl = false;
    send_acl(0x80, 2, 20, 1);
    pump();
    uint32_t rx0 = t_rx_n;
    (void)fake_rom_ack_acl(1);
    pump();
    uint8_t ir[16] = { 0x01, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0x01, 0x00, 0x00, 0x18, 0x04, 0x24, 0x00, 0x00, 0x00 };
    fake_rom_inject(0xE0D1u, ir, 16u);
    pump();
    uint32_t nocp = 0u, inq = 0u;
    for (uint32_t i = rx0; i < t_rx_n; i++) {
        nocp += is_evt(&t_rx[i], BT_HCI_EVT_NOCP);
        inq += is_evt(&t_rx[i], 0x02u);
    }
    t_rx_rd = t_rx_n;
    CHECK(nocp == 1u && inq == 1u, "host saw NOCP %u, inquiry result %u (want 1, 1)", nocp, inq);
    k = trace_drain(l, 80u, &other);
    CHECK(k == 0u, "NOCP / inquiry result traced %u (first '%s')", k, k ? l[0] : "");
    /* a successful non-LC CC: not traced; a failed one: traced with its opcode and status */
    uint8_t one = 1u;
    (void)roundtrip(0x0C1Au, &one, 1u);
    k = trace_drain(l, 80u, &other);
    CHECK(k == 0u, "successful 0C1A CC traced (%u)", k);
    fake_rom_set_reply_status(0xE02Cu, 0x12u);
    (void)roundtrip(0x0C1Au, &one, 1u);
    fake_rom_set_reply_status(0xE02Cu, 0x00u);
    k = trace_drain(l, 80u, &other);
    CHECK(k == 1u && strcmp(l[0], "BT HCI trace ev=0x0E st=0x12 op=0x0C1A") == 0, "failed 0C1A CC: %u '%s'", k,
          k ? l[0] : "");
    /* link control: a successful CC (041A) and a CS (0419) are traced, then events from the ROM */
    uint8_t a[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    (void)roundtrip(0x041Au, a, 6u);
    uint8_t rnr[10] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01, 0, 0, 0 };
    (void)roundtrip(0x0419u, rnr, 10u);
    inject_conn_req(0x42);
    pump();
    inject_disc(0x80, 0x13);
    pump();
    t_rx_rd = t_rx_n;
    k = trace_drain(l, 80u, &other);
    CHECK(k == 4u, "LC traces %u, want 4", k);
    if (k == 4u) {
        CHECK(strcmp(l[0], "BT HCI trace ev=0x0E st=0x00 op=0x041A") == 0, "041A CC trace '%s'", l[0]);
        CHECK(strcmp(l[1], "BT HCI trace ev=0x0F st=0x00 op=0x0419") == 0, "0419 CS trace '%s'", l[1]);
        CHECK(strcmp(l[2], "BT HCI trace ev=0x04 st=0xB0") == 0, "Connection Request trace '%s'", l[2]);
        CHECK(strcmp(l[3], "BT HCI trace ev=0x05 st=0x00") == 0, "Disconnection Complete trace '%s'", l[3]);
    }
    CHECK(other >= 2u, "the regular link notes stay (%u)", other);
    /* 64 deep: a burst of 70 Encryption Change events (no regular note) keeps 64, 6 lost */
    inject_conn_cmp(0x80);
    pump();
    (void)trace_drain(l, 80u, &other);
    uint32_t lost0 = stats().notes_lost;
    for (int i = 0; i < 70; i++) {
        uint8_t ec[5] = { 0x00, 0x00, 0x80, 0x00, 0x01 };    /* {s, pad, h, enc} */
        fake_rom_inject(0xE45Au, ec, 5u);
        pump();
    }
    t_rx_rd = t_rx_n;
    k = trace_drain(l, 80u, &other);
    CHECK(k == 64u && other == 0u && stats().notes_lost - lost0 == 6u, "burst: %u traces + %u notes kept, %u lost", k, other,
          stats().notes_lost - lost0);
    shutdown_and_check();
}
#endif

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_abrt;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, NULL);
    t_rx = calloc(MAX_PKTS, sizeof(pkt_t));

    struct { const char *name; void (*fn)(void); } tests[] = {
        { "table", test_table },
        { "name_notes", test_name_notes },
        { "name_before_note", test_name_before_note },
        { "golden_vectors", test_golden_vectors },
        { "startup_and_param_rules", test_startup_and_param_rules },
        { "tx_admission", test_tx_admission },
        { "rx_paths", test_rx_paths },
        { "0c35_zero_cmd_credits", test_0c35_zero_cmd_credits },
        { "nocp_disconnect_irq", test_nocp_disconnect_irq },
        { "teardown_restart", test_teardown_restart },
        { "bt_stop_sweep", test_bt_stop_sweep },
        { "faults", test_faults },
        { "rx_order", test_rx_order },
        { "lock_held_end_of_run", test_lock_held_end_of_run },
        { "fault_on_stop", test_fault_on_stop },
        { "reset_deferred_events", test_reset_deferred_events },
        { "reset_failure_masks_irq", test_reset_failure_masks_irq },
        { "fwd_orphan", test_fwd_orphan },
        { "stale_tx_acl_flush", test_stale_tx_acl_flush },
        { "stale_rx_acl_flush", test_stale_rx_acl_flush },
        { "unsolicited_reply_dropped", test_unsolicited_reply_dropped },
        { "0c13_rom_nul_guard", test_0c13_rom_nul_guard },
        { "parked_shutdown_completes", test_parked_shutdown_completes },
        { "send_avail_only_when_full", test_send_avail_only_when_full },
        { "class_m_fwd_fault", test_class_m_fwd_fault },
        { "deferred_order_after_free", test_deferred_order_after_free },
        { "nocp_reserved_slot", test_nocp_reserved_slot },
        { "acl_tx_error_paths", test_acl_tx_error_paths },
        { "reset_drops_queued_tx", test_reset_drops_queued_tx },
        { "handle_errors", test_handle_errors },
        { "c2h_fc_off", test_c2h_fc_off },
        { "event_packers", test_event_packers },
        { "phone_path", test_phone_path },
        { "fwd_deadline", test_fwd_deadline },
        { "fwd_late_same_opcode", test_fwd_late_same_opcode },
        { "scan_refused_two_links", test_scan_refused_two_links },
        { "detach_with_acl_in_flight", test_detach_with_acl_in_flight },
        { "link_notes", test_link_notes },
        { "session_lines", test_session_lines },
        { "fuzz_rx", test_fuzz_rx },
        { "fuzz_tx", test_fuzz_tx },
        { "hp_gates", test_hp_gates },
        { "hp_inquiry", test_hp_inquiry },
        { "hp_inquiry_cancel", test_hp_inquiry_cancel },
        { "hp_inquiry_refusals", test_hp_inquiry_refusals },
        { "hp_connect", test_hp_connect },
        { "hp_connect_fail", test_hp_connect_fail },
        { "hp_create_cancel", test_hp_create_cancel },
        { "hp_link_view_accept", test_hp_link_view_accept },
        { "hp_link_view_early_role", test_hp_link_view_early_role },
#if defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE
        { "hci_trace", test_hci_trace },
#endif
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fail;
        tests[i].fn();
        printf("%-28s %s\n", tests[i].name, g_fail == before ? "ok" : "FAILED");
    }
    fake_rom_shutdown();
    free(t_rx);
    printf("test_bt_ctrl (%s%s): %d checks, %d failures\n", hp_gated_on() ? "BT_CTRL_HP_GATE_LIST" : "default gates",
#if defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE
           " + BT_CTRL_HCI_TRACE",
#else
           "",
#endif
           g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
