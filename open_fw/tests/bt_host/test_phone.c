/* Phone-mode end-to-end scenario for make test-bt-host (test_bt_host phone <exfat.img>): the real BT=1 XREC backend
 * (bt_port/bt_xrec_bluedroid.c + src/xrec_proto.c + src/exfat.c, platform tests/bt_host/host_xrec.c) on the real
 * Bluedroid (patches 0001-0027) + bt_port + bt_ctrl, against tests/fake_rom.c's scripted remote peer, which this file
 * extends with a laptop's host stack: L2CAP signalling, an SDP client, an RFCOMM client (TS 07.10 basic mode with
 * credit-based flow control) and the +XREC client side of tools/bt_sync.py.
 *
 * Steps (each one CHECKed; any failure stops the scenario):
 *   1. bt_xrec_start -> READY (SPP server on channel 1); pairing window opened -> discoverable (0C1A 0x03).
 *   2. Laptop A pages, authenticates: SSP numeric comparison (A DisplayYesNo) inside the window waits on the device's
 *      prompt (value + remote name "laptop-A"), the test answers ENTER (BTHX_SSP_ANSWER); the link key
 *      is notified, stored through bt_store and written to /bt_bonds.bin; A encrypts.
 *   3. SDP: ServiceSearchAttribute for 0x1101 finds "Cassini" on RFCOMM channel 1.
 *   4. RFCOMM: mux SABM, PN (CFC), DLC SABM, MSC -> ESP_SPP_SRV_OPEN, link authorized for XREC.
 *   5. XREC: bt_sync.py's native-SPP start (no HFP AT lines from the device), PING, PAIR with the window's PIN
 *      (window and discoverability close), LS, a multi-frame GETS of a 40000-byte file with congestion (the peer
 *      withholds RFCOMM credits and baseband acks mid-transfer), byte-exact. 5b: the same GETS with the session's
 *      in-flight window raised above BTC's ring (test hook), so esp_spp_write() returns ESP_ERR_NO_MEM against the real
 *      btc_spp and ESP_SPP_CONG_EVT(false) resumes it (otherwise only the session's own window is ever hit).
 *   6. A drops the link (supervision timeout): SPP closed, XREC authorization cleared.
 *   7. A reconnects with the stored key, window closed: no new pairing; RFCOMM straight to channel 1; CHAL/AUTH; GETS.
 *   8. bt_xrec_stop with a 300000-byte GETS outstanding and the peer stalled: graceful teardown inside the 2 s
 *      deadline, no hard path; heap / tasks / esp_timer back to the pre-start baseline.
 *   9. Restart: bonds from /bt_bonds.bin, XREC key from /cassini.key: A reconnects (stored key) and authenticates.
 *  10. Laptop B tries to pair with the window closed: numeric comparison rejected, no bond.
 *  11. Window open, laptop C (NoInputNoOutput): Just Works pairing completes in Bluedroid, the session removes the
 *      unauthenticated bond and drops the link; C never reaches XREC.
 *  12. A reconnects, then walks out of range: its LMP detach never completes (fake_peer.defer_detach); bt_xrec_stop
 *      ends gracefully although Bluedroid's deinit finds the link still allocated (patch 0027).
 *  13. C pairs Just Works again and BT stops while its link is still up: C's key never reaches /bt_bonds.bin.
 *  14. Restart; C reconnects with that key: never authorized for XREC.
 *  15. A stale Just Works bond of C planted in /bt_bonds.bin, the card refusing writes meanwhile: removed at start
 *      (and the removal written once the card recovers), C never authorized.
 *  16. Window open, laptop B pairs; the user presses BACK on the prompt (BTHX_SSP_ANSWER 0): negative reply, no bond,
 *      window still open, an ENTER right after it is swallowed (-3), one 1 s later finds no prompt (-1).
 *  17. BENCH autoconfirm (bt_xrec_set_bench_autoconfirm, as SD:/BTBENCH.RUN autoconfirm=1): laptop D's comparison is
 *      accepted without the user, MITM bond, the window's SSP part closes; D: CHAL/AUTH twice (distinct DRBG nonces),
 *      GETS.
 *  18. Restart with the session's entropy withheld (host model of a BT-mode entry whose mic collection credited
 *      nothing; host_xrec.c): the plan M2.3 gate refuses, nothing opens even with the bench override on, B is
 *      rejected without a prompt, D's CHAL fails closed (no nonce, link closed); a later credit gives the XREC PIN but
 *      the SSP part stays refused (the ROM P-192 key is only replaced at a controller start).
 *  19. Bench-style start: window opened right after bt_xrec_start, before READY (main.c bt_bench_begin, pair=1 at
 *      t=0; bench XREC PIN xrec_pin=246810): the gate is decided at READY and the SSP part opens; D: XREC PAIR with
 *      the bench PIN and a new key, CHAL/AUTH with it, LS, GETS of a small range; bench PIN off: DRBG PIN again.
 *  20. bt_xrec_stop: baseline restored, no fake-ROM contract error, no unknown opcode, no BT fault, no HCI timeout;
 *      the scenario's heap peak against a host regression ceiling.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "openfw.h"
#include "openfw_crypto.h"
#include "bt_ctrl.h"
#include "bt_store.h"
#include "fake_rom.h"
#include "host_env.h"
#include "host_xrec.h"

int bth_phone_scenario(const char *img_path);
void bt_xrec_test_set_inflight_max(uint32_t n);   /* bt_xrec_bluedroid.c test hook (0 = default) */

/* Host regression ceiling for the scenario's heap peak above the post-warm-up baseline: measured 189,170 B on x86-64
 * (2026-11-24) + ~18 %. NOT the target budget (plan §4: >= 8 KiB free of the BT=1 image's 118,264 B heap): on the host,
 * pointers, list nodes, alignment and every task stack (StackType_t is 8 bytes) are wider, so the target figure comes
 * from the firmware's "BT mem" log lines (bt_xrec_fw.c, BT_HW_RESULTS recipe), not from here. */
#define PHONE_HEAP_CEILING (224u * 1024u)

/* Bluedroid task supervisor (bt_port/bt_task_sup.h) in the scenario: firmware 1 s / 8 s / 20 s, here shorter. */
#define SUP_PERIOD_MS 200u
#define SUP_STALL_MS  1500u
#define SUP_HOLD_MS   4000u

static int p_fail, p_checks;
#define PCHECK(cond, ...)                                                                                             \
    do {                                                                                                              \
        p_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            p_fail++;                                                                                                 \
            bth_log("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            bth_log(__VA_ARGS__);                                                                                     \
            bth_log("\n");                                                                                            \
        }                                                                                                             \
    } while (0)

/* Pump the peer until cond holds or ms elapse (1 tick = 1 ms on the host port). */
#define WAIT(cond, ms)                                                                                                \
    ({                                                                                                                \
        int ok_ = 0;                                                                                                  \
        for (uint32_t t_ = 0u; t_ < (uint32_t)(ms); t_++) {                                                           \
            peer_poll();                                                                                              \
            if (cond) {                                                                                               \
                ok_ = 1;                                                                                              \
                break;                                                                                                \
            }                                                                                                         \
            vTaskDelay(1);                                                                                            \
        }                                                                                                             \
        ok_;                                                                                                          \
    })

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bthx_snap_t S(void)
{
    bthx_snap_t s;
    bthx_snap(&s);
    return s;
}

/* ============================================ RAM SD card ======================================================= */
#define CARD_SECTORS 4096u
static uint8_t s_disk[CARD_SECTORS * 512u];
static exfat_vol_t s_card;

static int disk_read(void *ctx, uint32_t lba, uint32_t n, void *buf)
{
    (void)ctx;
    if (lba + n > CARD_SECTORS) {
        return -1;
    }
    memcpy(buf, &s_disk[lba * 512u], n * 512u);
    return 0;
}

static int disk_write(void *ctx, uint32_t lba, uint32_t n, const void *buf)
{
    (void)ctx;
    if (lba + n > CARD_SECTORS) {
        return -1;
    }
    memcpy(&s_disk[lba * 512u], buf, n * 512u);
    return 0;
}

static const blkdev_t s_dev = { disk_read, disk_write, CARD_SECTORS, NULL };

static uint8_t file_byte(uint32_t seed, uint32_t i) { return (uint8_t)(i * 7u + (i >> 8) * 13u + seed * 101u); }

static int write_record(const char *name, uint32_t size, uint32_t seed)
{
    exfat_writer_t w;
    if (exfat_create_file(&s_card, "/record", name, &w) != 0) {
        return -1;
    }
    uint8_t buf[4096];
    for (uint32_t off = 0u; off < size;) {
        uint32_t n = size - off < sizeof(buf) ? size - off : (uint32_t)sizeof(buf);
        for (uint32_t i = 0u; i < n; i++) {
            buf[i] = file_byte(seed, off + i);
        }
        if (exfat_write(&w, buf, n) != 0) {
            return -1;
        }
        off += n;
    }
    return exfat_close_file(&w);
}

/* ============================================ baselines ========================================================= */
typedef struct {
    size_t bytes, blocks;
    UBaseType_t tasks;
    uint32_t timers;
    uint32_t seq;
} pbase_t;

static pbase_t psnap(void)
{
    pbase_t b;
    bth_heap_stats_t h;
    bth_heap_stats(&h);
    b.bytes = h.live_bytes;
    b.blocks = h.live_blocks;
    b.tasks = uxTaskGetNumberOfTasks();
    b.timers = bth_timers_in_use();
    b.seq = bth_heap_seq();
    return b;
}

static int psettle(const pbase_t *base, pbase_t *now)
{
    for (int i = 0; i < 400; i++) {
        *now = psnap();
        if (now->bytes == base->bytes && now->blocks == base->blocks && now->tasks == base->tasks &&
            now->timers == base->timers) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

static void controller_clean(const char *where)
{
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    bth_rom_lock();
    uint32_t rom_errors = fake_rom.errors;
    char first[160];
    memcpy(first, fake_rom.first_error, sizeof(first));
    bth_rom_unlock();
    PCHECK(rom_errors == 0u, "%s: fake ROM contract error: %s", where, first);
    PCHECK(s.cmd_unknown == 0u, "%s: %u unknown opcodes (last %04X)", where, s.cmd_unknown, s.unknown_opcode_last);
    PCHECK(s.mandatory_fwd_fail == 0u, "%s: mandatory FWD failed", where);
    PCHECK(s.tx_contract_violation == 0u && s.tx_malformed == 0u, "%s: host TX contract violation", where);
    PCHECK(s.invariant == 0u && s.lock_violations == 0u, "%s: bt_ctrl invariant/lock violation", where);
    PCHECK(s.rom_unknown_ids == 0u, "%s: %u ROM message ids without a packer", where, s.rom_unknown_ids);
    PCHECK(s.fwd_deadline == 0u && s.fwd_late_reply == 0u, "%s: forwarded command deadline hit (%u, late %u)", where,
           s.fwd_deadline, s.fwd_late_reply);
    PCHECK(s.acl_rx_bad == 0u && s.acl_rx_overflow == 0u && s.evt_overflow == 0u, "%s: RX path error", where);
    PCHECK(bth_ctrl_faults() == 0u, "%s: BT fault posted (first code %u)", where, bth_ctrl_first_fault());
    PCHECK(bth_hci_cmd_timeouts() == 0u, "%s: HCI command timeout (patch 0007)", where);
    PCHECK(bthx_hard_path_reason() == 0u, "%s: XREC hard path taken (reason %u)", where, bthx_hard_path_reason());
    PCHECK(bthx_deadline_fired() == 0u, "%s: teardown deadline latched", where);
}

/* ============================================ the peer's host stack ============================================== */
#define PEER_L2CAP_MTU 1013u
#define PEER_RFC_N1    1000u
#define PEER_CREDITS   10
#define LCID_SDP       0x0040u
#define LCID_RFC       0x0041u

typedef struct {
    uint16_t psm, lcid, rcid, rmtu, refused;
    uint8_t pending, connected, cfg_in, cfg_out, closed;
} l2ch_t;

#define STREAM_CAP (512u * 1024u)
static uint8_t s_stream[STREAM_CAP];
static fake_acl_rec_t s_acl_copy[FAKE_ROM_LOG_MAX];

static struct {
    uint8_t asm_buf[4096];
    uint32_t asm_len;
    uint8_t sig_id;
    l2ch_t sdp, rfc;
    uint32_t info_reqs, cmd_rejects, sig_unknown, bad_frames, acl_in, acl_out;
    /* SDP */
    uint8_t sdp_attr[2048];
    uint32_t sdp_attr_len;
    uint8_t sdp_cont[17], sdp_cont_len;
    int sdp_state;                 /* 0 idle, 1 waiting, 2 done, -1 error */
    uint16_t sdp_tid;
    /* RFCOMM */
    uint8_t mux_up, mux_dm, dlc_up, dlc_dm, pn_rsp, msc_cmd_rx, msc_rsp_rx, cfc;
    uint32_t dlc_disc_rx, mux_disc_rx, rpn_rx;
    uint8_t dlci;
    uint16_t n1;
    int tx_credits, rx_credits;
    bool withhold;
    uint32_t uih_rx, credit_grants, fcs_errors, at_lines;
    uint32_t stream_len, stream_rd;
} P;

static uint32_t peer_poll(void);

static void peer_reset_link_state(void)
{
    memset(&P, 0, sizeof(P));
}

static int l2_send(uint16_t cid, const uint8_t *p, uint16_t len)
{
    uint8_t f[1100];
    if (len + 4u > sizeof(f) || len + 4u > 1021u) {
        PCHECK(0, "peer frame too long (%u)", len);
        return -1;
    }
    put16(f, len);
    put16(&f[2], cid);
    memcpy(&f[4], p, len);
    for (int i = 0; i < 3000; i++) {
        bth_rom_lock();
        int r = fake_peer_send_acl(f, (uint16_t)(len + 4u));
        bool up = fake_peer.connected;
        bth_rom_unlock();
        bth_ctrl_kick();
        if (r == 0) {
            P.acl_out++;
            return 0;
        }
        if (!up) {
            return -1;
        }
        vTaskDelay(1);             /* all EM RX buffers held: bt_ctrl / Bluedroid return them */
    }
    PCHECK(0, "peer ACL could not be injected for 3 s");
    return -1;
}

/* ---- L2CAP signalling ---- */
static void sig_send(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t b[96];
    b[0] = code;
    b[1] = id;
    put16(&b[2], len);
    memcpy(&b[4], d, len);
    (void)l2_send(0x0001u, b, (uint16_t)(4u + len));
}

static l2ch_t *ch_by_lcid(uint16_t lcid)
{
    if (lcid == LCID_SDP) return &P.sdp;
    if (lcid == LCID_RFC) return &P.rfc;
    return NULL;
}

static void l2_connect(l2ch_t *c, uint16_t psm, uint16_t lcid)
{
    memset(c, 0, sizeof(*c));
    c->psm = psm;
    c->lcid = lcid;
    uint8_t d[4];
    put16(d, psm);
    put16(&d[2], lcid);
    sig_send(0x02u, ++P.sig_id, d, 4u);
}

static void l2_config(l2ch_t *c)
{
    uint8_t d[8];
    put16(d, c->rcid);
    put16(&d[2], 0u);
    d[4] = 0x01u;                   /* MTU option */
    d[5] = 2u;
    put16(&d[6], PEER_L2CAP_MTU);
    sig_send(0x04u, ++P.sig_id, d, 8u);
}

static void l2_disconnect(l2ch_t *c)
{
    uint8_t d[4];
    put16(d, c->rcid);
    put16(&d[2], c->lcid);
    sig_send(0x06u, ++P.sig_id, d, 4u);
}

static bool l2_open(const l2ch_t *c) { return c->connected && c->cfg_in && c->cfg_out && !c->closed; }

static void sig_in(const uint8_t *p, uint16_t n)
{
    while (n >= 4u) {
        uint8_t code = p[0], id = p[1];
        uint16_t len = rd16(&p[2]);
        const uint8_t *d = &p[4];
        if (4u + len > n) {
            P.bad_frames++;
            return;
        }
        l2ch_t *c;
        uint8_t r[16];
        switch (code) {
        case 0x03:                                   /* Connection Response */
            c = ch_by_lcid(rd16(&d[2]));
            if (c != NULL) {
                uint16_t result = rd16(&d[4]);
                if (result == 0u) {
                    c->rcid = rd16(d);
                    c->connected = 1u;
                    l2_config(c);
                } else if (result == 1u) {
                    c->pending = 1u;
                } else {
                    c->refused = result;
                    c->closed = 1u;
                }
            }
            break;
        case 0x04:                                   /* Configuration Request */
            c = ch_by_lcid(rd16(d));
            if (c != NULL) {
                for (uint16_t o = 4u; o + 2u <= len; o = (uint16_t)(o + 2u + d[o + 1u])) {
                    if ((d[o] & 0x7Fu) == 0x01u && d[o + 1u] == 2u) {
                        c->rmtu = rd16(&d[o + 2u]);
                    }
                }
                put16(r, c->rcid);
                put16(&r[2], 0u);
                put16(&r[4], 0u);                    /* success, no options */
                sig_send(0x05u, id, r, 6u);
                c->cfg_in = 1u;
            }
            break;
        case 0x05:                                   /* Configuration Response */
            c = ch_by_lcid(rd16(d));
            if (c != NULL) {
                PCHECK(rd16(&d[4]) == 0u, "L2CAP config of PSM %04X refused (%u)", c->psm, rd16(&d[4]));
                c->cfg_out = 1u;
            }
            break;
        case 0x06:                                   /* Disconnection Request */
            c = ch_by_lcid(rd16(d));
            memcpy(r, d, 4u);
            sig_send(0x07u, id, r, 4u);
            if (c != NULL) {
                c->closed = 1u;
            }
            break;
        case 0x07:                                   /* Disconnection Response */
            c = ch_by_lcid(rd16(&d[2]));
            if (c != NULL) {
                c->closed = 1u;
            }
            break;
        case 0x0A: {                                 /* Information Request */
            uint16_t type = rd16(d);
            P.info_reqs++;
            memset(r, 0, sizeof(r));
            put16(r, type);
            uint16_t rl = 4u;
            if (type == 1u) {
                put16(&r[4], 672u);
                rl = 6u;
            } else if (type == 2u) {
                rl = 8u;                             /* extended features: none (basic mode only) */
            } else if (type == 3u) {
                r[4] = 0x02u;                        /* fixed channels: signalling */
                rl = 12u;
            } else {
                put16(&r[2], 1u);                    /* not supported */
            }
            sig_send(0x0Bu, id, r, rl);
            break;
        }
        case 0x08:                                   /* Echo Request */
            sig_send(0x09u, id, r, 0u);
            break;
        case 0x0B:                                   /* Information Response */
        case 0x09:
            break;
        case 0x01:                                   /* Command Reject */
            P.cmd_rejects++;
            break;
        case 0x02:                                   /* Connection Request from the device: no services here */
            put16(r, 0u);
            put16(&r[2], rd16(&d[2]));
            put16(&r[4], 2u);
            put16(&r[6], 0u);
            sig_send(0x03u, id, r, 8u);
            break;
        default:
            P.sig_unknown++;
            break;
        }
        p += 4u + len;
        n = (uint16_t)(n - 4u - len);
    }
}

/* ---- SDP client ---- */
static void sdp_request(void)
{
    uint8_t q[48];
    uint16_t i = 5u;
    static const uint8_t pattern[] = { 0x35, 0x03, 0x19, 0x11, 0x01 };            /* SerialPort 0x1101 */
    static const uint8_t attrs[] = { 0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF };  /* all attributes */
    q[0] = 0x06u;                                                                 /* ServiceSearchAttributeRequest */
    put16(&q[1], (uint16_t)(((P.sdp_tid >> 8) | (P.sdp_tid << 8)) & 0xFFFFu));   /* big endian on the wire */
    memcpy(&q[i], pattern, sizeof(pattern));
    i = (uint16_t)(i + sizeof(pattern));
    q[i++] = 0x03u;                                                               /* max attribute bytes (BE) */
    q[i++] = 0xF0u;
    memcpy(&q[i], attrs, sizeof(attrs));
    i = (uint16_t)(i + sizeof(attrs));
    q[i++] = P.sdp_cont_len;
    memcpy(&q[i], P.sdp_cont, P.sdp_cont_len);
    i = (uint16_t)(i + P.sdp_cont_len);
    q[3] = (uint8_t)((i - 5u) >> 8);
    q[4] = (uint8_t)(i - 5u);
    P.sdp_state = 1;
    (void)l2_send(P.sdp.rcid, q, i);
}

static void sdp_in(const uint8_t *p, uint16_t n)
{
    if (n < 5u || p[0] != 0x07u) {
        P.sdp_state = -1;
        return;
    }
    uint16_t cnt = (uint16_t)((p[5] << 8) | p[6]);
    if (7u + cnt + 1u > n || P.sdp_attr_len + cnt > sizeof(P.sdp_attr)) {
        P.sdp_state = -1;
        return;
    }
    memcpy(&P.sdp_attr[P.sdp_attr_len], &p[7], cnt);
    P.sdp_attr_len += cnt;
    P.sdp_cont_len = p[7u + cnt];
    if (P.sdp_cont_len > 16u || 8u + cnt + P.sdp_cont_len > n) {
        P.sdp_state = -1;
        return;
    }
    memcpy(P.sdp_cont, &p[8u + cnt], P.sdp_cont_len);
    P.sdp_tid++;
    if (P.sdp_cont_len != 0u) {
        sdp_request();
    } else {
        P.sdp_state = 2;
    }
}

static bool mem_has(const uint8_t *hay, uint32_t n, const void *needle, uint32_t m)
{
    for (uint32_t i = 0u; i + m <= n; i++) {
        if (memcmp(&hay[i], needle, m) == 0) {
            return true;
        }
    }
    return false;
}

/* RFCOMM channel from the ProtocolDescriptorList: UUID16 0x0003 followed by a uint8 element. */
static int sdp_rfcomm_channel(void)
{
    for (uint32_t i = 0u; i + 5u <= P.sdp_attr_len; i++) {
        const uint8_t *q = &P.sdp_attr[i];
        if (q[0] == 0x19u && q[1] == 0x00u && q[2] == 0x03u && q[3] == 0x08u) {
            return q[4];
        }
    }
    return -1;
}

/* ---- RFCOMM client (initiator) ---- */
static uint8_t rfc_fcs(const uint8_t *p, int n)
{
    uint8_t crc = 0xFFu;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint8_t)((crc >> 1) ^ 0xE0u) : (uint8_t)(crc >> 1);
        }
    }
    return (uint8_t)(0xFFu - crc);
}

/* cr: the frame's C/R bit (initiator: 1 on commands and UIH, 0 on responses). credit >= 0: UIH with P/F + credit. */
static void rfc_frame(uint8_t dlci, uint8_t ctrl, bool cr, const uint8_t *info, uint16_t len, int credit)
{
    uint8_t f[1100];
    int i = 0;
    bool uih = ctrl == 0xEFu;
    f[i++] = (uint8_t)((dlci << 2) | (cr ? 0x02u : 0u) | 0x01u);
    f[i++] = (uint8_t)(ctrl | (credit >= 0 ? 0x10u : 0u));
    if (len <= 127u) {
        f[i++] = (uint8_t)((len << 1) | 1u);
    } else {
        f[i++] = (uint8_t)((len & 0x7Fu) << 1);
        f[i++] = (uint8_t)(len >> 7);
    }
    int hdr = i;
    if (credit >= 0) {
        f[i++] = (uint8_t)credit;
    }
    if (len != 0u) {
        memcpy(&f[i], info, len);
    }
    i += len;
    f[i] = rfc_fcs(f, uih ? 2 : hdr);
    i++;
    (void)l2_send(P.rfc.rcid, f, (uint16_t)i);
}

static void mux_send(uint8_t cmd, bool cr, const uint8_t *v, uint8_t len)
{
    uint8_t m[16];
    m[0] = (uint8_t)((cmd << 2) | (cr ? 0x02u : 0u) | 0x01u);
    m[1] = (uint8_t)((len << 1) | 1u);
    memcpy(&m[2], v, len);
    rfc_frame(0u, 0xEFu, true, m, (uint16_t)(2u + len), -1);
}

static void rfc_grant(void)
{
    if (P.withhold || !P.dlc_up || !P.cfc || P.rx_credits > PEER_CREDITS / 2) {
        return;
    }
    int g = PEER_CREDITS - P.rx_credits;
    P.rx_credits += g;
    P.credit_grants++;
    rfc_frame(P.dlci, 0xEFu, true, NULL, 0u, g);
}

static void mux_in(const uint8_t *d, uint16_t n)
{
    while (n >= 2u) {
        uint8_t t = d[0], cmd = (uint8_t)(t >> 2);
        bool cr = (t & 0x02u) != 0u;
        uint8_t len = (uint8_t)(d[1] >> 1);
        const uint8_t *v = &d[2];
        if (2u + len > n) {
            P.bad_frames++;
            return;
        }
        switch (cmd) {
        case 0x20:                                     /* PN */
            if (!cr && len >= 8u) {
                P.pn_rsp = 1u;
                P.n1 = rd16(&v[4]);
                P.cfc = (uint8_t)((v[1] >> 4) == 0x0Eu);
                P.tx_credits = P.cfc ? (v[7] & 0x07) : 0;
            }
            break;
        case 0x38:                                     /* MSC */
            if (cr) {
                P.msc_cmd_rx = 1u;
                mux_send(0x38u, false, v, len);
            } else {
                P.msc_rsp_rx = 1u;
            }
            break;
        case 0x24: {                                   /* RPN: answer with the device's values or 9600 8N1 */
            P.rpn_rx++;
            if (cr) {
                uint8_t rv[8] = { v[0], 0x03, 0x03, 0x00, 0x11, 0x13, 0xFF, 0x3F };
                if (len == 8u) {
                    memcpy(rv, v, 8u);
                }
                mux_send(0x24u, false, rv, 8u);
            }
            break;
        }
        case 0x14:                                     /* RLS */
        case 0x08:                                     /* Test */
        case 0x28:                                     /* FCon */
        case 0x18:                                     /* FCoff */
            if (cr) {
                mux_send(cmd, false, v, len);
            }
            break;
        case 0x04:                                     /* NSC */
            P.bad_frames++;
            break;
        default:
            if (cr) {
                uint8_t nsc = t;
                mux_send(0x04u, false, &nsc, 1u);
            }
            break;
        }
        d += 2u + len;
        n = (uint16_t)(n - 2u - len);
    }
}

static void stream_append(const uint8_t *d, uint16_t n)
{
    if (P.stream_rd > STREAM_CAP / 2u) {
        memmove(s_stream, &s_stream[P.stream_rd], P.stream_len - P.stream_rd);
        P.stream_len -= P.stream_rd;
        P.stream_rd = 0u;
    }
    if (P.stream_len + n > STREAM_CAP) {
        PCHECK(0, "peer stream buffer overrun");
        return;
    }
    memcpy(&s_stream[P.stream_len], d, n);
    P.stream_len += n;
}

static void rfc_in(const uint8_t *p, uint16_t n)
{
    if (n < 4u) {
        P.bad_frames++;
        return;
    }
    uint8_t dlci = (uint8_t)(p[0] >> 2), ctrl = p[1], type = (uint8_t)(ctrl & 0xEFu);
    bool pf = (ctrl & 0x10u) != 0u;
    uint16_t len;
    int h;
    if (p[2] & 1u) {
        len = (uint16_t)(p[2] >> 1);
        h = 3;
    } else {
        len = (uint16_t)((p[2] >> 1) | (p[3] << 7));
        h = 4;
    }
    int credit_bytes = (type == 0xEFu && pf && dlci != 0u && P.cfc) ? 1 : 0;
    if ((uint32_t)h + (uint32_t)credit_bytes + len + 1u != n) {
        P.bad_frames++;
        return;
    }
    if (p[n - 1u] != rfc_fcs(p, type == 0xEFu ? 2 : h)) {
        P.fcs_errors++;
        return;
    }
    switch (type) {
    case 0x63:                                         /* UA */
        if (dlci == 0u) {
            P.mux_up = 1u;
        } else if (dlci == P.dlci) {
            P.dlc_up = 1u;
        }
        break;
    case 0x0F:                                         /* DM */
        if (dlci == 0u) {
            P.mux_dm = 1u;
        } else if (dlci == P.dlci) {
            P.dlc_dm = 1u;
        }
        break;
    case 0x43:                                         /* DISC */
        rfc_frame(dlci, 0x73u, false, NULL, 0u, -1);   /* UA (F) */
        if (dlci == 0u) {
            P.mux_up = 0u;
            P.mux_disc_rx++;
        } else {
            P.dlc_up = 0u;
            P.dlc_disc_rx++;
        }
        break;
    case 0xEF:                                         /* UIH */
        if (dlci == 0u) {
            mux_in(&p[h], len);
        } else if (dlci == P.dlci) {
            if (credit_bytes) {
                P.tx_credits += p[h];
            }
            if (len != 0u) {
                P.uih_rx++;
                P.rx_credits--;
                if (P.rx_credits < 0) {
                    PCHECK(0, "device sent UIH data without a credit");
                }
                stream_append(&p[h + credit_bytes], len);
            }
            rfc_grant();
        }
        break;
    default:
        P.bad_frames++;
        break;
    }
}

static void l2_frame_in(const uint8_t *f, uint32_t n)
{
    uint16_t cid = rd16(&f[2]);
    const uint8_t *p = &f[4];
    uint16_t len = (uint16_t)(n - 4u);
    if (cid == 0x0001u) {
        sig_in(p, len);
    } else if (cid == P.sdp.lcid && P.sdp.connected && !P.sdp.closed) {
        sdp_in(p, len);
    } else if (cid == P.rfc.lcid && P.rfc.connected && !P.rfc.closed) {
        rfc_in(p, len);
    } else {
        P.bad_frames++;
    }
}

/* Host -> peer ACL from the fake ROM's air log: reassemble L2CAP (PB 01 = continuation) and dispatch. */
static uint32_t peer_poll(void)
{
    bth_rom_lock();
    uint32_t n = fake_rom.n_acl;
    if (n != 0u) {
        memcpy(s_acl_copy, fake_rom.acl, n * sizeof(fake_acl_rec_t));
        fake_rom.n_acl = 0u;
    }
    bth_rom_unlock();
    for (uint32_t i = 0u; i < n; i++) {
        const fake_acl_rec_t *r = &s_acl_copy[i];
        P.acl_in++;
        if ((r->pb_bc & 3u) != 1u) {
            P.asm_len = 0u;
        }
        if (P.asm_len + r->len > sizeof(P.asm_buf)) {
            P.bad_frames++;
            P.asm_len = 0u;
            continue;
        }
        memcpy(&P.asm_buf[P.asm_len], r->data, r->len);
        P.asm_len += r->len;
        if (P.asm_len >= 4u && P.asm_len >= 4u + rd16(P.asm_buf)) {
            l2_frame_in(P.asm_buf, 4u + rd16(P.asm_buf));
            P.asm_len = 0u;
        }
    }
    return n;
}

/* ---- connection helpers ---- */
/* must = false: an attempt the device may refuse (policy steps); failures are returned, not CHECKed. */
#define RCHECK(cond, ...)                                                                                             \
    do {                                                                                                              \
        if (must) {                                                                                                   \
            PCHECK(cond, __VA_ARGS__);                                                                                \
        }                                                                                                             \
    } while (0)

static bool rfc_connect_ex(uint8_t scn, bool must)
{
    l2_connect(&P.rfc, 0x0003u, LCID_RFC);
    if (!WAIT(l2_open(&P.rfc) || P.rfc.closed, 5000)) {
        RCHECK(0, "L2CAP RFCOMM channel not configured (pending %u)", P.rfc.pending);
        return false;
    }
    RCHECK(!P.rfc.closed, "L2CAP RFCOMM refused (%u)", P.rfc.refused);
    if (P.rfc.closed) {
        return false;
    }
    rfc_frame(0u, 0x3Fu, true, NULL, 0u, -1);                 /* SABM(P) on DLCI 0 */
    if (!WAIT(P.mux_up || P.mux_dm, 3000) || !P.mux_up) {
        RCHECK(0, "RFCOMM multiplexer not up");
        return false;
    }
    P.dlci = (uint8_t)(scn << 1);                              /* initiator: D = 0 */
    uint8_t pn[8] = { P.dlci, 0xF0u, 0u, 0u, (uint8_t)PEER_RFC_N1, (uint8_t)(PEER_RFC_N1 >> 8), 0u, 7u };
    P.rx_credits = 7;
    mux_send(0x20u, true, pn, 8u);
    if (!WAIT(P.pn_rsp, 3000)) {
        RCHECK(0, "no PN response");
        return false;
    }
    RCHECK(P.cfc, "device did not accept credit-based flow control");
    rfc_frame(P.dlci, 0x3Fu, true, NULL, 0u, -1);             /* SABM(P) on the server channel */
    if (!WAIT(P.dlc_up || P.dlc_dm, 5000) || !P.dlc_up) {
        RCHECK(0, "DLC %u not opened (DM %u)", P.dlci, P.dlc_dm);
        return false;
    }
    uint8_t msc[2] = { (uint8_t)((P.dlci << 2) | 0x03u), 0x8Du };   /* RTC, RTR, DV */
    mux_send(0x38u, true, msc, 2u);
    bool msc_ok = WAIT(P.msc_cmd_rx && P.msc_rsp_rx, 3000);
    RCHECK(msc_ok, "MSC exchange incomplete (cmd %u rsp %u)", P.msc_cmd_rx, P.msc_rsp_rx);
    rfc_grant();
    return msc_ok || must;
}

static bool rfc_connect(uint8_t scn) { return rfc_connect_ex(scn, true); }

static bool x_send(const char *s)
{
    if (!WAIT(P.tx_credits > 0 || !P.dlc_up, 3000) || !P.dlc_up) {
        PCHECK(0, "no RFCOMM credit to send '%s'", s);
        return false;
    }
    P.tx_credits--;
    rfc_frame(P.dlci, 0xEFu, true, (const uint8_t *)s, (uint16_t)strlen(s), -1);
    return true;
}

static int stream_find_nl(void)
{
    for (uint32_t i = P.stream_rd; i < P.stream_len; i++) {
        if (s_stream[i] == '\n') {
            return (int)i;
        }
    }
    return -1;
}

/* Next non-empty line (CR/LF stripped), as bt_sync.py _read_line. */
static bool x_line(char *out, size_t cap, uint32_t ms)
{
    for (;;) {
        int nl = -1;
        if (!WAIT((nl = stream_find_nl()) >= 0, ms)) {
            out[0] = '\0';
            return false;
        }
        uint32_t len = (uint32_t)nl - P.stream_rd;
        while (len > 0u && (s_stream[P.stream_rd + len - 1u] == '\r')) {
            len--;
        }
        uint32_t k = len < cap - 1u ? len : (uint32_t)cap - 1u;
        memcpy(out, &s_stream[P.stream_rd], k);
        out[k] = '\0';
        P.stream_rd = (uint32_t)nl + 1u;
        if (k > 0u) {
            return true;
        }
    }
}

/* One GETS frame: "XREC" | u32 offset | u16 len | u16 crc | payload (bt_sync.py _read_xrec_frame). */
static int x_frame(uint32_t *off, uint8_t *payload, uint32_t cap, uint32_t ms)
{
    if (!WAIT(P.stream_len - P.stream_rd >= 12u, ms)) {
        return -1;
    }
    const uint8_t *h = &s_stream[P.stream_rd];
    if (memcmp(h, "XREC", 4u) != 0) {
        return -2;
    }
    *off = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    uint16_t len = rd16(&h[8]), crc = rd16(&h[10]);
    if (len > cap || !WAIT(P.stream_len - P.stream_rd >= 12u + len, ms)) {
        return -3;
    }
    memcpy(payload, &s_stream[P.stream_rd + 12u], len);
    P.stream_rd += 12u + len;
    return openfw_crc16_ccitt_false(payload, len) == crc ? (int)len : -4;
}

static void hex(char *out, const uint8_t *b, uint32_t n)
{
    static const char k[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < n; i++) {
        out[2u * i] = k[b[i] >> 4];
        out[2u * i + 1u] = k[b[i] & 15u];
    }
    out[2u * n] = '\0';
}

static void peer_cfg(fake_peer_cfg_t *c, uint8_t last, uint8_t io_cap, uint8_t auth_req, uint8_t key_type,
                     const char *name)
{
    memset(c, 0, sizeof(*c));
    static const uint8_t base[6] = { 0x00, 0x7E, 0x51, 0x1A, 0x5B, 0x00 };
    memcpy(c->addr, base, 6u);
    c->addr[0] = last;
    c->cod[0] = 0x0Cu;                        /* laptop: computer / laptop */
    c->cod[1] = 0x01u;
    c->cod[2] = 0x1Au;
    c->io_cap = io_cap;
    c->auth_req = auth_req;
    c->numeric = 0x0001E240u + last;          /* 123456 + last */
    c->user_confirms = true;
    c->auto_encrypt = true;
    c->key_type = key_type;
    static const uint8_t f0[8] = { 0xBF, 0xFE, 0xCF, 0xFE, 0xDB, 0xFF, 0x7B, 0x87 };   /* SSP (byte 6 bit 3) */
    memcpy(c->features, f0, 8u);
    c->features_p1[0] = 0x01u;                /* SSP host support */
    c->max_page = 1u;
    c->lmp_ver = 9u;
    c->manuf = 0x0002u;
    c->subver = 0x2222u;
    c->clk_off = 0x1234u;
    snprintf(c->name, sizeof(c->name), "%s", name);
}

static void peer_attach(const fake_peer_cfg_t *c, const uint8_t *key)
{
    bth_rom_lock();
    fake_peer_init(c);
    if (key != NULL) {
        fake_peer_set_key(key);
    }
    bth_rom_unlock();
}

static bool peer_page_and_connect(void)
{
    peer_reset_link_state();
    bthx_snap_t s0 = S();
    bth_rom_lock();
    int r = fake_peer_page(0u);
    bth_rom_unlock();
    bth_ctrl_kick();
    PCHECK(r == 0, "fake_peer_page");
    bool up = WAIT(fake_peer.connected && S().d.acl_links == s0.d.acl_links + 1u, 5000);
    PCHECK(up, "ACL not up (peer %u, session acl_links %u)", fake_peer.connected, S().d.acl_links);
    /* Bluedroid's link set-up reads (041B/041C/041D/041F, name) run now; let them finish before pairing. */
    vTaskDelay(pdMS_TO_TICKS(150));
    peer_poll();
    return up;
}

static void peer_authenticate(void)
{
    bth_rom_lock();
    fake_peer_authenticate();
    bth_rom_unlock();
    bth_ctrl_kick();
}

static void peer_drop(uint8_t reason)
{
    bth_rom_lock();
    fake_peer_disconnect(reason);
    bth_rom_unlock();
    bth_ctrl_kick();
}

/* Stall the air: RFCOMM credits withheld, baseband acks withheld (the ROM keeps ACL "in air", no NOCP). */
static void peer_stall(bool on)
{
    P.withhold = on;
    bth_rom_lock();
    fake_rom.auto_ack_acl = !on;
    if (!on) {
        (void)fake_rom_ack_acl(64u);
    }
    bth_rom_unlock();
    bth_ctrl_kick();
    if (!on) {
        rfc_grant();
    }
}

static uint32_t rom_cmd_last_byte0(uint16_t id, int *found)
{
    *found = 0;
    uint32_t v = 0u;
    bth_rom_lock();
    for (uint32_t i = fake_rom.n_cmds; i > 0u; i--) {
        if (fake_rom.cmds[i - 1u].id == id) {
            v = fake_rom.cmds[i - 1u].data[0];
            *found = 1;
            break;
        }
    }
    bth_rom_unlock();
    return v;
}

static void rom_cmds_clear(void)
{
    bth_rom_lock();
    fake_rom.n_cmds = 0u;
    bth_rom_unlock();
}

/* Download [off, off+len) of a /record file with GETS bursts, comparing every byte. */
static uint8_t s_payload[2048];
static bool gets_verify(const char *name, uint32_t seed, uint32_t off, uint32_t len, uint32_t chunk, int stall_at,
                        uint32_t *frames_out)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "+XREC:GETS,%u,%u,%u,%s\r", (unsigned)off, (unsigned)len, (unsigned)chunk, name);
    if (!x_send(cmd)) {
        return false;
    }
    uint32_t got = 0u, frames = 0u;
    while (got < len) {
        uint32_t fo = 0u;
        int n = x_frame(&fo, s_payload, sizeof(s_payload), 5000u);
        if (n <= 0) {
            PCHECK(0, "GETS %s frame %u: error %d at %u of %u", name, frames, n, got, len);
            return false;
        }
        PCHECK(fo == off + got, "GETS frame offset %u, want %u", fo, off + got);
        for (int i = 0; i < n; i++) {
            if (s_payload[i] != file_byte(seed, fo + (uint32_t)i)) {
                PCHECK(0, "GETS %s byte %u differs", name, fo + (uint32_t)i);
                return false;
            }
        }
        got += (uint32_t)n;
        frames++;
        if (stall_at >= 0 && frames == (uint32_t)stall_at) {
            /* congestion: the peer stops granting credits and acking baseband packets until the device backs up */
            peer_stall(true);
            bool backed_up = WAIT(S().d.cong || S().d.tx_nomem > 0u ||
                                  S().d.inflight >= BT_XREC_INFLIGHT_MAX - BT_XREC_WRITE_MAX, 5000);
            bthx_snap_t s = S();
            PCHECK(backed_up, "device never backed up under the stall (inflight %u tx_used %u cong %u nomem %u)",
                   s.d.inflight, s.d.tx_used, s.d.cong, s.d.tx_nomem);
            vTaskDelay(pdMS_TO_TICKS(300));              /* stays stalled: nothing lost, nothing duplicated */
            s = S();
            bth_log("    congestion: inflight %u tx_used %u cong %u nomem %u cong_evts %u, %u frames received\n",
                    (unsigned)s.d.inflight, (unsigned)s.d.tx_used, s.d.cong, (unsigned)s.d.tx_nomem,
                    (unsigned)s.d.cong_evts, (unsigned)frames);
            peer_stall(false);
        }
    }
    if (frames_out != NULL) {
        *frames_out = frames;
    }
    return true;
}

static uint8_t s_chal_nonce[16];       /* the last CHAL nonce (DRBG output on the device) */
static bool xrec_auth_chal(const uint8_t key[32])
{
    char line[160];
    if (!x_send("+XREC:CHAL\r") || !x_line(line, sizeof(line), 3000u) || strncmp(line, "+XREC:CHAL,", 11) != 0 ||
        strlen(line) != 11u + 32u) {
        PCHECK(0, "CHAL reply '%s'", line);
        return false;
    }
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) {
        unsigned v = 0u;
        if (sscanf(&line[11 + 2 * i], "%2x", &v) != 1) {
            return false;
        }
        nonce[i] = (uint8_t)v;
    }
    memcpy(s_chal_nonce, nonce, 16u);
    uint8_t mac[32];
    openfw_hmac_sha256(key, 32u, nonce, 16u, mac);
    char cmd[96] = "+XREC:AUTH,";
    hex(&cmd[11], mac, 32u);
    strcat(cmd, "\r");
    if (!x_send(cmd) || !x_line(line, sizeof(line), 3000u)) {
        return false;
    }
    PCHECK(strcmp(line, "+XREC:OK,AUTH") == 0, "AUTH reply '%s'", line);
    return strcmp(line, "+XREC:OK,AUTH") == 0;
}

/* bt_sync.py connect_and_handshake() waits for the stock firmware's HFP AT lines; the native-SPP path (OpenFW) sees
 * none and goes straight to PING. */
static void expect_no_at_lines(void)
{
    uint32_t before = P.stream_len;
    vTaskDelay(pdMS_TO_TICKS(200));
    peer_poll();
    PCHECK(P.stream_len == before, "device sent %u bytes unprompted after RFCOMM open (HFP AT?)",
           P.stream_len - before);
}

/* ============================================ scenario ========================================================== */
static int load_card(const char *img_path)
{
    FILE *f = fopen(img_path, "rb");
    if (f == NULL) {
        bth_log("phone: cannot open %s\n", img_path);
        return -1;
    }
    size_t n = fread(s_disk, 1u, sizeof(s_disk), f);
    fclose(f);
    if (n != sizeof(s_disk) || exfat_mount(&s_card, &s_dev) != 0) {
        bth_log("phone: %s is not a %u-sector exFAT image\n", img_path, CARD_SECTORS);
        return -1;
    }
    exfat_set_timestamp(&s_card, 0x5B2A6000u);
    if (exfat_mkdir(&s_card, "record") != 0 || write_record("REC0001.WAV", 40000u, 1u) != 0 ||
        write_record("BIG0002.WAV", 300000u, 2u) != 0) {
        bth_log("phone: cannot populate the card\n");
        return -1;
    }
    return 0;
}

static bool wait_ready(void)
{
    bth_step("bt_xrec_start -> READY", 20000u);
    PCHECK(bthx_call(BTHX_START, 0u, 3000u) == 0, "bt_xrec_start");
    bool ok = WAIT(S().d.state == BT_XREC_ST_READY || S().d.state == BT_XREC_ST_FAULTED, 15000);
    bth_step_done();
    bthx_snap_t s = S();
    PCHECK(ok && s.d.state == BT_XREC_ST_READY, "session not READY (state %u fault %u)", s.d.state, s.d.last_fault);
    return s.d.state == BT_XREC_ST_READY;
}

static void stop_session(const pbase_t *base, const char *what)
{
    bth_step(what, 10000u);
    uint64_t t0 = us_timer_now();
    uint32_t seq = bthx_post(BTHX_STOP, 0u);
    int r = -1000;
    (void)WAIT(bthx_done(seq, &r), 8000);       /* the laptop keeps answering (RFCOMM DISC, L2CAP, detach) */
    uint32_t ms = (uint32_t)((us_timer_now() - t0) / 1000u);
    bth_step_done();
    PCHECK(r == 0, "%s: bt_xrec_stop result %d (hard path reason)", what, r);
    if (r != 0 && getenv("BTH_PHONE_DUMP") != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20000));     /* let the down job finish and log its step times */
    }
    PCHECK(ms < BT_XREC_SHUTDOWN_DEADLINE_MS, "%s: stop took %u ms (deadline %u)", what, ms,
           BT_XREC_SHUTDOWN_DEADLINE_MS);
    bthx_snap_t s = S();
    PCHECK(s.d.state == BT_XREC_ST_OFF, "%s: state %u after stop", what, s.d.state);
    bth_log("    %s: %u ms\n", what, ms);
    controller_clean(what);
    pbase_t now;
    int settled = psettle(base, &now);
    PCHECK(settled, "%s: heap %zu B / %zu blocks (base %zu / %zu), tasks %lu (base %lu), timers %u "
           "(base %u)", what, now.bytes, now.blocks, base->bytes, base->blocks, (unsigned long)now.tasks,
           (unsigned long)base->tasks, now.timers, base->timers);
    if (!settled) {
        bth_heap_dump_since(base->seq);
    }
}

/* > 0: /bt_bonds.bin holds the link key; else -size. The BTC config keeps it as "LinkKey = <hex>" of Bluedroid's
 * LINK_KEY array, which btu_hcif reads from the HCI event with STREAM_TO_ARRAY16 (byte-reversed): the hex is the wire
 * key backwards. The reconnect steps prove the round trip (the key Bluedroid replies with is the peer's). */
static uint8_t s_img[BT_STORE_IMAGE_MAX];
static int bond_file_has_key(const uint8_t key[16])
{
    int n = bthx_read_root_file("bt_bonds.bin", s_img, sizeof(s_img));
    uint8_t rev[16];
    for (int i = 0; i < 16; i++) {
        rev[i] = key[15 - i];
    }
    char kh[40];
    hex(kh, rev, 16u);
    if (n > 0 && mem_has(s_img, (uint32_t)n, kh, 32u)) {
        return n;
    }
    if (getenv("BTH_PHONE_DUMP") != NULL) {
        for (int i = 0; i < n; i++) {
            bth_log("%c", (s_img[i] >= 32 && s_img[i] < 127) ? s_img[i] : '.');
        }
        bth_log("\nwant LinkKey = %s\n", kh);
    }
    return -(n < 0 ? 0 : n);
}

static bool step_pair_window_and_bond(uint8_t key_out[16])
{
    bth_log("[P2] laptop A pages; SSP numeric comparison inside the pairing window; link key stored\n");
    fake_peer_cfg_t a;
    peer_cfg(&a, 0xA1u, 0x01u, 0x05u, 0x05u, "laptop-A");     /* DisplayYesNo, MITM general bonding, auth P-192 */
    a.max_page = 2u;                          /* BlueZ: page 2 with bit 0 clear; the ROM's initiator decision modelled */
    a.rom_auth_decision = true;               /* (docs/BT_ROM_HCI_MAP.md §12): the responder never reaches it */
    peer_attach(&a, NULL);
    if (!peer_page_and_connect()) {
        return false;
    }
    uint32_t bw0 = bthx_bond_writes();
    char want[8];
    snprintf(want, sizeof(want), "%06u", (unsigned)(a.numeric % 1000000u));
    bth_step("SSP numeric comparison", 10000u);
    peer_authenticate();
    /* plan M4: the device shows the value and the peer and waits for the local user (ENTER) */
    bool prompt = WAIT(S().ui.prompt, 3000);
    bthx_snap_t ps = S();
    PCHECK(prompt && strcmp(ps.ui.digits, want) == 0, "prompt %u digits '%s', want %s", ps.ui.prompt, ps.ui.digits,
           want);
    PCHECK(fake_peer.host_confirms == 0u && fake_peer.pairings == 0u, "confirmed before the user answered");
    PCHECK(WAIT(strcmp(S().ui.peer, "laptop-A") == 0, 2000), "prompt peer '%s', want the remote name laptop-A",
           S().ui.peer);
    vTaskDelay(pdMS_TO_TICKS(BT_XREC_SSP_ARM_MS + 50u));
    PCHECK(bthx_call(BTHX_SSP_ANSWER, 1u, 2000u) == 0, "ENTER on the prompt");
    bool paired = WAIT(fake_peer.pairings == 1u && fake_peer.encrypted, 5000);
    bth_step_done();
    PCHECK(paired, "pairing (pairings %u, encrypted %u, confirms %u, rejects %u)", fake_peer.pairings,
           fake_peer.encrypted, fake_peer.host_confirms, fake_peer.host_rejects);
    PCHECK(fake_peer.host_io_cap == 0x01u, "our IO capability %u, want DisplayYesNo", fake_peer.host_io_cap);
    bth_log("    P2: responder IO capability reply auth_req 0x%02X, PIN requests %u\n", fake_peer.host_auth_req,
            fake_peer.pin_requests);
    /* patch 0041 changes the initiator's reply only: the responder's stays 0x00 (as before 0041), and the ROM's
     * initiator decision (legacy PIN with page 2 in the slot) is never reached as responder */
    PCHECK(fake_peer.host_auth_req == 0x00u && fake_peer.pin_requests == 0u && fake_peer.io_cap_requests >= 1u,
           "responder auth_req 0x%02X (want 0x00), PIN requests %u, IO capability requests %u",
           fake_peer.host_auth_req, fake_peer.pin_requests, fake_peer.io_cap_requests);
    PCHECK(fake_peer.host_confirms == 1u && fake_peer.host_rejects == 0u, "user confirmation reply");
    bool seen = WAIT(S().d.ssp_accepted == 1u && S().bonds == 1, 3000);
    bthx_snap_t s = S();
    PCHECK(seen, "session: ssp_accepted %u, bonds %d", s.d.ssp_accepted, s.bonds);
    /* the value the user compared: shown on the prompt (checked above) and logged with the user's accept */
    PCHECK(bthx_ssp_value_logged() == a.numeric % 1000000u, "numeric comparison value %u logged, want %s",
           (unsigned)bthx_ssp_value_logged(), want);
    memcpy(key_out, fake_peer.key, 16u);
    /* bt_store flush request -> /bt_bonds.bin (the BTC config save after the link key notification) */
    PCHECK(WAIT(bthx_bond_writes() > bw0, 3000), "bond store never written to /bt_bonds.bin");
    PCHECK(WAIT(bond_file_has_key(key_out) > 0, 3000), "link key not in /bt_bonds.bin (%d bytes, %u writes)",
           bond_file_has_key(key_out), (unsigned)bthx_bond_writes());
    return p_fail == 0;
}

static bool step_sdp(void)
{
    bth_log("[P3] SDP: SerialPort 0x1101 record\n");
    l2_connect(&P.sdp, 0x0001u, LCID_SDP);
    if (!WAIT(l2_open(&P.sdp) || P.sdp.closed, 3000) || P.sdp.closed) {
        PCHECK(0, "SDP L2CAP channel (refused %u)", P.sdp.refused);
        return false;
    }
    sdp_request();
    PCHECK(WAIT(P.sdp_state != 1, 3000) && P.sdp_state == 2, "SDP response (state %d)", P.sdp_state);
    static const uint8_t spp[3] = { 0x19, 0x11, 0x01 };
    PCHECK(mem_has(P.sdp_attr, P.sdp_attr_len, spp, 3u), "SDP: no SerialPort UUID");
    PCHECK(mem_has(P.sdp_attr, P.sdp_attr_len, "Cassini", 7u), "SDP: no ServiceName 'Cassini'");
    int ch = sdp_rfcomm_channel();
    PCHECK(ch == BT_XREC_SPP_SCN, "SDP: RFCOMM channel %d", ch);
    bth_log("    SDP: %u attribute bytes, RFCOMM channel %d\n", (unsigned)P.sdp_attr_len, ch);
    l2_disconnect(&P.sdp);
    PCHECK(WAIT(P.sdp.closed, 2000), "SDP channel close");
    return p_fail == 0;
}

static bool step_rfcomm_open(void)
{
    if (!rfc_connect(BT_XREC_SPP_SCN)) {
        return false;
    }
    bool open = WAIT(S().d.conn_open && S().d.conn_authorized, 3000);
    bthx_snap_t s = S();
    PCHECK(open, "SPP not open/authorized (open %u auth %u rejected %u)", s.d.conn_open, s.d.conn_authorized,
           s.d.conns_rejected);
    PCHECK(s.d.mtu >= 652u, "negotiated RFCOMM MTU %u (a 640-byte GETS frame needs 652)", s.d.mtu);
    bth_log("    RFCOMM open: dlci %u, N1 %u, device MTU %u, tx credits %d\n", P.dlci, P.n1, s.d.mtu, P.tx_credits);
    expect_no_at_lines();
    return open;
}

/* A peer that must never reach XREC (policy steps): it pages and authenticates; whatever the stack then does with it
 * (re-pairing, the session's bond removal, an SPP refusal), an XREC PING never gets an answer and the session never
 * marks an SPP connection authorized. The link is down afterwards. */
static void expect_never_authorized(const char *who)
{
    uint32_t auth0 = S().d.conns_authorized;
    peer_authenticate();
    (void)WAIT(!fake_peer.connected || fake_peer.encrypted, 3000);
    vTaskDelay(pdMS_TO_TICKS(200));
    peer_poll();
    bool pong = false, spp = false;
    if (fake_peer.connected && rfc_connect_ex(BT_XREC_SPP_SCN, false) && P.dlc_up && P.tx_credits > 0) {
        spp = true;
        P.tx_credits--;
        rfc_frame(P.dlci, 0xEFu, true, (const uint8_t *)"+XREC:PING\r", 11u, -1);
        char line[160];
        pong = x_line(line, sizeof(line), 500u) && strncmp(line, "+XREC:PONG", 10) == 0;
    }
    bthx_snap_t s = S();
    PCHECK(!pong && s.d.conns_authorized == auth0, "%s reached XREC (PONG %d, authorized connections %u -> %u)", who,
           pong, auth0, s.d.conns_authorized);
    bth_log("    %s: encrypted %u, pairings %u, SPP dlc %d; session bonds_removed %u conns_rejected %u\n", who,
            fake_peer.encrypted, fake_peer.pairings, spp, s.d.bonds_removed, s.d.conns_rejected);
    if (fake_peer.connected) {
        peer_drop(0x13u);
    }
    PCHECK(WAIT(!fake_peer.connected && S().d.acl_links == 0u && !S().d.conn_open, 3000), "%s link not down", who);
}

/* Write a bond for bda (Bluedroid's "xx:xx:xx:xx:xx:xx") with the given link key type into /bt_bonds.bin, next to
 * what the file holds (session OFF): the state a power loss, a failed SD write or a lost AUTH_CMPL leaves behind. */
static bool plant_bond(const char *bdstr, int key_type, const uint8_t wire_key[16])
{
    int n = bthx_read_root_file("bt_bonds.bin", s_img, sizeof(s_img));
    if (n <= 0 || bt_store_load(s_img, (size_t)n) != ESP_OK) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("bt_config.conf", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    static char cfg[1600];
    size_t len = sizeof(cfg) - 1u;
    bool ok = nvs_get_blob(h, "bt_cfg_key0", cfg, &len) == ESP_OK;
    if (ok) {
        uint8_t rev[16];
        for (int i = 0; i < 16; i++) {
            rev[i] = wire_key[15 - i];           /* stored byte-reversed (see bond_file_has_key) */
        }
        char kh[40];
        hex(kh, rev, 16u);
        int add = snprintf(cfg + len, sizeof(cfg) - len, "\n[%s]\nLinkKeyType = %d\nPinLength = 0\nSCSupport = 0\n"
                           "LinkKey = %s\n", bdstr, key_type, kh);
        ok = add > 0 && (size_t)add < sizeof(cfg) - len && len + (size_t)add < 1536u &&
             nvs_set_blob(h, "bt_cfg_key0", cfg, len + (size_t)add) == ESP_OK;
    }
    nvs_close(h);
    size_t m = ok ? bt_store_serialize(s_img, sizeof(s_img)) : 0u;
    if (m == 0u) {
        return false;
    }
    exfat_writer_t w;
    (void)exfat_delete_file(&s_card, "/", "bt_bonds.bin");
    return exfat_create_file(&s_card, "/", "bt_bonds.bin", &w) == 0 && exfat_write(&w, s_img, (uint32_t)m) == 0 &&
           exfat_close_file(&w) == 0;
}

/* The supervisor over a quiet stretch: every thread answered, no fault, no WDT hold (plan M4 "Watchdog" tests). */
static void sup_quiet(const char *what, uint32_t ms)
{
    bt_task_sup_stats_t a, b;
    bthx_sup_stats(&a);
    uint32_t f0 = bthx_sup_faults();
    int held = 0;
    for (uint32_t t = 0u; t < ms; t += 10u) {
        for (int k = 0; k < 10; k++) {
            peer_poll();
            vTaskDelay(1);
        }
        held |= bthx_sup_hold();
    }
    bthx_sup_stats(&b);
    PCHECK(b.armed && bthx_sup_faults() == f0 && b.stalls == a.stalls && !held,
           "%s: supervisor armed %u faults %u stalls %u hold %d", what, b.armed, bthx_sup_faults() - f0,
           b.stalls - a.stalls, held);
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        uint32_t n = b.answers[i] - a.answers[i];
        PCHECK(n >= ms / SUP_PERIOD_MS / 2u, "%s: %s answered %u pings in %u ms", what, bt_task_sup_name(i), n, ms);
    }
    bth_log("    %s: %u ms, pings hciT %u BTU %u BTC %u, max latency %u/%u/%u ms\n", what, ms,
            b.answers[0] - a.answers[0], b.answers[1] - a.answers[1], b.answers[2] - a.answers[2], b.max_ms[0],
            b.max_ms[1], b.max_ms[2]);
}

int bth_phone_scenario(const char *img_path)
{
    bth_log("[P] phone mode end to end: XREC backend + Bluedroid + bt_ctrl + fake ROM peer\n");
    if (load_card(img_path) != 0) {
        return 1;
    }
    PCHECK(bthx_init(&s_card) == 0, "bthx_init");
    bthx_sup_limits(SUP_PERIOD_MS, SUP_STALL_MS, SUP_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(20));
    /* Warm-up session (start, READY, stop, no peer). The first esp_spp_init() creates btc_spp.c's resident mutex and
     * TX event group, which upstream keeps for the life of the process on purpose (btc_spp.c "s_spp_resident": a VFS
     * write() may still wait on the group after deinit). Exactly those 2 blocks may stay; the baseline for every
     * later check is taken after this session. */
    pbase_t base0 = psnap();
    if (!wait_ready()) {
        return 1;
    }
    {
        uint32_t seq = bthx_post(BTHX_STOP, 0u);
        int r = -1000;
        PCHECK(WAIT(bthx_done(seq, &r), 5000) && r == 0, "warm-up stop (%d)", r);
        pbase_t w;
        vTaskDelay(pdMS_TO_TICKS(50));
        (void)psettle(&base0, &w);
        PCHECK(w.blocks == base0.blocks + 2u && w.tasks == base0.tasks && w.timers == base0.timers,
               "warm-up session left %zu blocks / %zu B, tasks %lu (base %lu), timers %u (want only the 2 resident SPP "
               "objects)", w.blocks - base0.blocks, w.bytes - base0.bytes, (unsigned long)w.tasks,
               (unsigned long)base0.tasks, w.timers);
        if (w.blocks != base0.blocks + 2u) {
            bth_heap_dump_since(base0.seq);
        }
    }
    pbase_t base = psnap();
    bth_heap_peak_reset();           /* scenario-local peak: everything above the post-warm-up baseline */
    static const uint8_t xkey[32] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 1, 2, 3, 4, 5, 6, 7, 8,
                                      9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
    uint8_t lkey[16];
    char line[200];

    /* ---- 1 ---- */
    bth_log("[P1] bt_xrec_start, pairing window\n");
    rom_cmds_clear();
    if (!wait_ready()) {
        return 1;
    }
    bthx_snap_t s = S();
    PCHECK(strcmp(s.st.dev_name, "Cassini_11") == 0 && s.st.hci_ready, "status name '%s' ready %u", s.st.dev_name,
           s.st.hci_ready);
    PCHECK(s.d.scan_applied == 1u, "connectable-only before the window (%u)", s.d.scan_applied);
    bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
    PCHECK(WAIT(S().d.scan_disc == 1u, 2000), "not discoverable with the window open");
    int found = 0;
    PCHECK(WAIT(rom_cmd_last_byte0(0xE02Cu, &found) == 0x03u, 2000), "0C1A 0x03 (inquiry + page scan) not sent");
    s = S();
    PCHECK(s.st.pair_active && strlen(s.st.pair_pin) == 6u, "XREC PIN '%s'", s.st.pair_pin);

    /* ---- 2 ---- */
    if (!step_pair_window_and_bond(lkey)) {
        goto out;
    }
    /* ---- 3, 4 ---- */
    if (!step_sdp()) {
        goto out;
    }
    bth_log("[P4] RFCOMM channel 1\n");
    if (!step_rfcomm_open()) {
        goto out;
    }
    /* ---- 5 ---- */
    bth_log("[P5] XREC: PING, PAIR, LS, GETS with congestion (session in-flight window)\n");
    PCHECK(x_send("+XREC:PING\r") && x_line(line, sizeof(line), 3000u), "PING");
    PCHECK(strncmp(line, "+XREC:PONG,openfw_v5,v5,bound=0,auth=0,pair=1", 45) == 0, "PONG '%s'", line);
    {
        /* bt_status_t.pair_pin shows the XREC PIN (the user reads it off the screen and types it into the app /
         * bt_sync.py --pair); the numeric comparison value only ever appears on the prompt */
        char k[80], cmd[128];
        hex(k, xkey, 32u);
        PCHECK(!S().d.ssp_showing, "numeric comparison value still shown after the bond");
        s = S();
        snprintf(cmd, sizeof(cmd), "+XREC:PAIR,%s,%s\r", s.st.pair_pin, k);
        PCHECK(strlen(s.st.pair_pin) == 6u, "XREC PIN not shown after the SSP value ('%s')", s.st.pair_pin);
        PCHECK(x_send(cmd) && x_line(line, sizeof(line), 3000u), "PAIR");
        PCHECK(strcmp(line, "+XREC:OK,PAIR") == 0, "PAIR reply '%s'", line);
    }
    PCHECK(WAIT(!S().d.window_open && S().d.scan_applied == 1u, 2000), "window/discoverability not closed by PAIR");
    PCHECK(WAIT(rom_cmd_last_byte0(0xE02Cu, &found) == 0x02u, 2000), "0C1A 0x02 (page scan only) not sent");
    PCHECK(WAIT(bthx_key_writes() == 1u, 2000), "/cassini.key not written");
    PCHECK(x_send("+XREC:LS\r"), "LS");
    {
        int files = 0, rec1 = 0, big = 0;
        while (x_line(line, sizeof(line), 3000u) && strcmp(line, "+XREC:END") != 0) {
            files++;
            rec1 |= strcmp(line, "+XREC:F,40000,REC0001.WAV") == 0;
            big |= strcmp(line, "+XREC:F,300000,BIG0002.WAV") == 0;
        }
        PCHECK(strcmp(line, "+XREC:END") == 0 && files == 2 && rec1 && big, "LS listing (%d files, last '%s')", files,
               line);
    }
    {
        uint32_t frames = 0u;
        bth_step("GETS 40000 with congestion", 30000u);
        PCHECK(gets_verify("REC0001.WAV", 1u, 0u, 40000u, 640u, 12, &frames), "GETS REC0001.WAV");
        bth_step_done();
        s = S();
        PCHECK(frames == 63u, "GETS frames %u (want 63 x <= 640 B)", frames);
        bth_log("    GETS 40000 B in %u frames; writes %u, write events %u, cong events %u, nomem %u, peer UIH %u, "
                "credit grants %u\n", frames, (unsigned)s.d.tx_writes, (unsigned)s.d.write_evts,
                (unsigned)s.d.cong_evts, (unsigned)s.d.tx_nomem, (unsigned)P.uih_rx, (unsigned)P.credit_grants);
    }
    {
        /* Review finding: the step above only fills the session's own in-flight window (= tx_buffer_size), so BTC's
         * ring never refuses a write: ESP_ERR_NO_MEM -> s.cong and the ESP_SPP_CONG_EVT(false) resume never ran
         * against the real btc_spp (upper_cong / SPP_TX_LOW_WM). With a window of twice the ring they must. */
        bth_log("[P5b] GETS with BTC's ring full (session window 2 x tx_buffer_size): ESP_ERR_NO_MEM, CONG_EVT\n");
        bthx_snap_t s0 = S();
        bt_xrec_test_set_inflight_max(2u * BT_XREC_SPP_TX_BUF);
        uint32_t frames = 0u;
        bth_step("GETS 40000 with BTC congested", 30000u);
        PCHECK(gets_verify("REC0001.WAV", 1u, 0u, 40000u, 640u, 12, &frames), "GETS REC0001.WAV, BTC congested");
        bth_step_done();
        bt_xrec_test_set_inflight_max(0u);
        s = S();
        PCHECK(frames == 63u, "GETS frames %u (want 63)", frames);
        PCHECK(s.d.tx_nomem > s0.d.tx_nomem && s.d.cong_evts > s0.d.cong_evts && !s.d.cong,
               "BTC congestion path not exercised (nomem %u -> %u, cong events %u -> %u, cong %u)",
               (unsigned)s0.d.tx_nomem, (unsigned)s.d.tx_nomem, (unsigned)s0.d.cong_evts, (unsigned)s.d.cong_evts,
               s.d.cong);
        bth_log("    GETS 40000 B: nomem %u, cong events %u\n", (unsigned)(s.d.tx_nomem - s0.d.tx_nomem),
                (unsigned)(s.d.cong_evts - s0.d.cong_evts));
    }
    PCHECK(P.fcs_errors == 0u && P.bad_frames == 0u, "peer saw %u FCS errors, %u bad frames", P.fcs_errors,
           P.bad_frames);
    controller_clean("transfer");
    if (p_fail) {
        goto out;
    }

    /* ---- 6 ---- */
    bth_log("[P6] laptop A drops the link (supervision timeout)\n");
    peer_drop(0x08u);
    PCHECK(WAIT(!S().d.conn_open && S().d.acl_links == 0u, 3000), "session still sees the link (open %u acl %u)",
           S().d.conn_open, S().d.acl_links);
    PCHECK(S().st.xrec_auth == 0u, "XREC authorization survived the disconnect");

    /* ---- 7 ---- */
    bth_log("[P7] laptop A reconnects with the stored key (window closed), CHAL/AUTH, GETS\n");
    if (!peer_page_and_connect()) {
        goto out;
    }
    peer_authenticate();
    PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "reconnect authentication (key replies %u neg "
           "%u)", fake_peer.host_key_replies, fake_peer.host_key_neg);
    PCHECK(fake_peer.pairings == 1u && memcmp(fake_peer.host_key, lkey, 16u) == 0, "stored link key not used");
    if (!step_rfcomm_open()) {
        goto out;
    }
    PCHECK(x_send("+XREC:PING\r") && x_line(line, sizeof(line), 3000u), "PING");
    PCHECK(strncmp(line, "+XREC:PONG,openfw_v5,v5,bound=1,auth=0,pair=0", 45) == 0, "PONG '%s'", line);
    PCHECK(xrec_auth_chal(xkey), "CHAL/AUTH");
    PCHECK(gets_verify("REC0001.WAV", 1u, 1000u, 5000u, 640u, -1, NULL), "GETS after reconnect");

    /* ---- 7b ---- */
    bth_log("[P7b] WDT supervisor: idle bonded phone (authorized link, no traffic) for 3 stall windows\n");
    sup_quiet("idle bonded phone", 3u * SUP_STALL_MS);
    PCHECK(S().d.state == BT_XREC_ST_READY && S().d.conn_open, "idle link lost (state %u)", S().d.state);

    /* ---- 8 ---- */
    /* The laptop's app stops reading (RFCOMM credits withheld: data backs up in RFCOMM, BTC and the session) while
     * its stack stays responsive (baseband acks, RFCOMM DISC/UA, L2CAP, detach): the user leaves BT mode mid-sync. */
    bth_log("[P8] bt_xrec_stop with a 300000-byte GETS outstanding (laptop app not reading)\n");
    PCHECK(x_send("+XREC:GETS,0,300000,640,BIG0002.WAV\r"), "GETS BIG");
    {
        uint32_t fo = 0u;
        PCHECK(x_frame(&fo, s_payload, sizeof(s_payload), 3000u) == 640 && fo == 0u, "first BIG frame");
    }
    P.withhold = true;
    PCHECK(WAIT(S().d.inflight >= BT_XREC_INFLIGHT_MAX - BT_XREC_WRITE_MAX, 3000), "nothing outstanding before the "
           "stop (inflight %u)", S().d.inflight);
    s = S();
    bth_log("    outstanding at stop: inflight %u, tx_used %u, cong %u\n", (unsigned)s.d.inflight,
            (unsigned)s.d.tx_used, s.d.cong);
    /* plan M4 "intentionally stalled transfer": the peer, not a task, is stuck; the threads stay healthy */
    sup_quiet("transfer stalled by the peer", 2u * SUP_STALL_MS);
    PCHECK(S().d.state == BT_XREC_ST_READY, "stalled transfer: state %u", S().d.state);
    stop_session(&base, "stop with traffic outstanding");
    PCHECK(!fake_peer.connected && fake_peer.stop_kills == 0u, "link still up when the controller stopped (no detach)");
    PCHECK(P.dlc_disc_rx == 1u || P.mux_disc_rx == 1u || P.rfc.closed, "RFCOMM/L2CAP not closed by the device "
           "(DISC dlc %u mux %u, L2CAP closed %u)", P.dlc_disc_rx, P.mux_disc_rx, P.rfc.closed);
    if (p_fail) {
        goto out;
    }

    /* ---- 9 ---- */
    bth_log("[P9] restart: bonds from /bt_bonds.bin, XREC key from /cassini.key; A reconnects\n");
    rom_cmds_clear();
    if (!wait_ready()) {
        goto out;
    }
    PCHECK(S().bonds == 1, "bond not restored from /bt_bonds.bin (%d)", S().bonds);
    PCHECK(S().st.xrec_bound == 1u, "XREC key not loaded from /cassini.key");
    {
        fake_peer_cfg_t a;
        peer_cfg(&a, 0xA1u, 0x01u, 0x05u, 0x05u, "laptop-A");
        peer_attach(&a, lkey);
    }
    if (!peer_page_and_connect()) {
        goto out;
    }
    peer_authenticate();
    PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "authentication with the restored key");
    PCHECK(fake_peer.pairings == 0u && fake_peer.host_key_replies == 1u, "restored key: pairings %u key replies %u",
           fake_peer.pairings, fake_peer.host_key_replies);
    if (!step_rfcomm_open()) {
        goto out;
    }
    PCHECK(xrec_auth_chal(xkey), "CHAL/AUTH with the key loaded from /cassini.key");
    PCHECK(gets_verify("BIG0002.WAV", 2u, 250000u, 3000u, 640u, -1, NULL), "GETS after restart");
    peer_drop(0x13u);
    PCHECK(WAIT(!S().d.conn_open && S().d.acl_links == 0u, 3000), "link not down");

    /* ---- 10 ---- */
    bth_log("[P10] laptop B pairs with the window closed: rejected\n");
    {
        fake_peer_cfg_t b;
        peer_cfg(&b, 0xB2u, 0x01u, 0x05u, 0x05u, "laptop-B");
        peer_attach(&b, NULL);
        uint32_t rej0 = S().d.ssp_rejected;
        if (!peer_page_and_connect()) {
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(fake_peer.host_rejects == 1u || fake_peer.auth_fail > 0u, 3000), "no negative confirmation reply");
        PCHECK(fake_peer.pairings == 0u && fake_peer.host_confirms == 0u, "B paired (pairings %u confirms %u)",
               fake_peer.pairings, fake_peer.host_confirms);
        PCHECK(WAIT(S().d.ssp_rejected == rej0 + 1u, 2000), "session did not count the rejection");
        vTaskDelay(pdMS_TO_TICKS(100));
        PCHECK(S().bonds == 1, "bonds %d after B (want only A)", S().bonds);
        peer_drop(0x13u);
        PCHECK(WAIT(S().d.acl_links == 0u, 3000), "B link not down");
    }

    /* ---- 11 ---- */
    bth_log("[P11] window open, laptop C (NoInputNoOutput): Just Works bond removed, link dropped\n");
    {
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        PCHECK(WAIT(S().d.scan_disc == 1u, 2000), "window not reopened");
        fake_peer_cfg_t c;
        peer_cfg(&c, 0xC3u, 0x03u, 0x04u, 0x04u, "laptop-C");  /* NoIO, general bonding no MITM, unauth P-192 key */
        peer_attach(&c, NULL);
        uint32_t rm0 = S().d.bonds_removed;
        if (!peer_page_and_connect()) {
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(S().d.bonds_removed == rm0 + 1u, 3000), "Just Works bond not removed (removed %u)",
               S().d.bonds_removed);
        PCHECK(WAIT(!fake_peer.connected, 3000), "Just Works link not dropped");
        PCHECK(WAIT(S().bonds == 1, 2000), "bonds %d after C (want only A)", S().bonds);
        PCHECK(fake_peer.pairings == 1u && fake_peer.host_confirms == 1u, "C: Bluedroid auto-confirmed (%u, %u)",
               fake_peer.pairings, fake_peer.host_confirms);
        PCHECK(S().d.ssp_accepted == 0u, "Just Works reached the numeric-comparison policy");
        /* the session's own view of the link (ACL_DISCONN_CMPL through BTC) can trail the peer's: the next page
         * counts links from here */
        PCHECK(WAIT(S().d.acl_links == 0u && !S().d.conn_open, 3000), "session still counts C's link (acl %u)",
               S().d.acl_links);
    }

    /* ---- 12 ---- */
    /* Review finding: Patch 0026 reports DISABLE ~600 ms after its detach whether the link went down or not. A phone
     * that walked out of range never completes the LMP detach (the controller waits for the supervision timeout), so
     * Bluedroid's deinit ran with the L2CAP link still allocated: btu_free_core -> l2c_free -> l2cu_release_lcb ->
     * btm_acl_removed -> bta_sys_sendmsg -> osi_thread_post(NULL btu_thread): assert / NULL dereference (patch 0027). */
    bth_log("[P12] laptop A reconnects; it walks out of range: its detach never completes; bt_xrec_stop\n");
    {
        fake_peer_cfg_t a;
        peer_cfg(&a, 0xA1u, 0x01u, 0x05u, 0x05u, "laptop-A");
        peer_attach(&a, lkey);
    }
    if (!peer_page_and_connect()) {
        goto out;
    }
    peer_authenticate();
    PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "A authentication");
    if (!step_rfcomm_open()) {
        goto out;
    }
    bth_rom_lock();
    fake_peer.defer_detach = true;
    bth_rom_unlock();
    stop_session(&base, "stop, detach never completes");
    PCHECK(fake_peer.detach_deferred >= 1u && fake_peer.stop_kills == 1u, "detach not exercised (deferred %u, link "
           "killed by the controller stop %u)", fake_peer.detach_deferred, fake_peer.stop_kills);
    if (p_fail) {
        goto out;
    }

    /* ---- 13 ---- */
    /* Review finding: a Just Works key is stored (and a flush requested) before the app sees AUTH_CMPL; BTA DM removes
     * it only once the ACL is down, which never happens if BT stops first. It must still never reach the card. */
    bth_log("[P13] window open, laptop C pairs Just Works; its link is still up when bt_xrec_stop runs\n");
    uint8_t ckey[16];
    rom_cmds_clear();
    if (!wait_ready()) {
        goto out;
    }
    {
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        PCHECK(WAIT(S().d.scan_disc == 1u, 2000), "window not opened");
        fake_peer_cfg_t c;
        peer_cfg(&c, 0xC3u, 0x03u, 0x04u, 0x04u, "laptop-C");
        peer_attach(&c, NULL);
        uint32_t rm0 = S().d.bonds_removed;
        if (!peer_page_and_connect()) {
            goto out;
        }
        bth_rom_lock();
        fake_peer.defer_detach = true;               /* the rejection's disconnect does not complete before the stop */
        bth_rom_unlock();
        peer_authenticate();
        PCHECK(WAIT(S().d.bonds_removed == rm0 + 1u, 3000), "Just Works bond not removed (removed %u)",
               S().d.bonds_removed);
        memcpy(ckey, fake_peer.key, 16u);
        PCHECK(fake_peer.pairings == 1u && fake_peer.connected, "C: pairing %u, link up %u", fake_peer.pairings,
               fake_peer.connected);
        stop_session(&base, "stop right after the Just Works rejection");
        int n = bthx_read_root_file("bt_bonds.bin", s_img, sizeof(s_img));
        PCHECK(n > 0 && !mem_has(s_img, (uint32_t)n, "00:5b:1a:51:7e:c3", 17u),
               "Just Works key of C persisted in /bt_bonds.bin after the stop");
        PCHECK(n > 0 && mem_has(s_img, (uint32_t)n, "00:5b:1a:51:7e:a1", 17u), "A lost from /bt_bonds.bin");
    }
    if (p_fail) {
        goto out;
    }

    /* ---- 14 ---- */
    bth_log("[P14] restart; C reconnects with its Just Works key: never authorized\n");
    rom_cmds_clear();
    if (!wait_ready()) {
        goto out;
    }
    PCHECK(S().bonds == 1, "bonds %d at start (want only A)", S().bonds);
    {
        fake_peer_cfg_t c;
        peer_cfg(&c, 0xC3u, 0x03u, 0x04u, 0x04u, "laptop-C");
        peer_attach(&c, ckey);
        if (!peer_page_and_connect()) {
            goto out;
        }
        expect_never_authorized("C (key from an earlier Just Works pairing)");
    }
    stop_session(&base, "stop after C");
    if (p_fail) {
        goto out;
    }

    /* ---- 15 ---- */
    /* Review finding: link_authorized() trusted any stored bond. A Just Works bond left on the card (power loss after
     * the key was written, a failed rewrite, a lost AUTH_CMPL) must neither load as a bond nor authorize XREC. */
    bth_log("[P15] a stale Just Works bond of C in /bt_bonds.bin; restart; C reconnects with it\n");
    PCHECK(plant_bond("00:5b:1a:51:7e:c3", 4, ckey), "could not write C's stale bond into /bt_bonds.bin");
    {
        /* Review finding (bt_store): serialise cleared the dirty flag before the write, so the removal's failed write
         * (full card, SDIO error) left the stale bond on the card with nothing left to retry. The card refuses writes
         * while the session removes the stale bond, then recovers: the bounded retry must clean the file. */
        uint32_t wf0 = bthx_bond_write_fails();
        bthx_fail_bond_writes(1);
        rom_cmds_clear();
        if (!wait_ready()) {
            bthx_fail_bond_writes(0);
            goto out;
        }
        PCHECK(WAIT(S().bonds == 1, 2000), "stale Just Works bond loaded (bonds %d, want only A)", S().bonds);
        PCHECK(S().d.bonds_removed == 1u, "stale bond not removed at start (removed %u)", S().d.bonds_removed);
        PCHECK(WAIT(bthx_bond_write_fails() > wf0, 2000), "the removal never tried to write /bt_bonds.bin");
        bthx_fail_bond_writes(0);
        int n = -1;
        bool clean = WAIT((n = bthx_read_root_file("bt_bonds.bin", s_img, sizeof(s_img))) > 0 &&
                          !mem_has(s_img, (uint32_t)n, "00:5b:1a:51:7e:c3", 17u), 3000);
        PCHECK(clean, "stale bond still in /bt_bonds.bin after a failed write and the card's recovery (%u failed "
               "writes)", bthx_bond_write_fails() - wf0);
        fake_peer_cfg_t c;
        peer_cfg(&c, 0xC3u, 0x03u, 0x04u, 0x04u, "laptop-C");
        peer_attach(&c, ckey);
        if (!peer_page_and_connect()) {
            goto out;
        }
        expect_never_authorized("C (stale Just Works bond on the card)");
    }

    /* ---- 16 ---- */
    bth_log("[P16] window open, laptop B pairs; the user presses BACK on the prompt: rejected, no bond\n");
    {
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        PCHECK(WAIT(S().d.scan_disc == 1u && S().d.ssp_window, 2000), "window not opened (ssp %u)", S().d.ssp_window);
        fake_peer_cfg_t b;
        peer_cfg(&b, 0xB2u, 0x01u, 0x05u, 0x05u, "laptop-B");
        peer_attach(&b, NULL);
        bthx_snap_t s0 = S();
        if (!peer_page_and_connect()) {
            goto out;
        }
        char want[8];
        snprintf(want, sizeof(want), "%06u", (unsigned)(b.numeric % 1000000u));
        peer_authenticate();
        PCHECK(WAIT(S().ui.prompt && strcmp(S().ui.digits, want) == 0, 3000), "prompt with B's value %s ('%s')", want,
               S().ui.digits);
        PCHECK(WAIT(strcmp(S().ui.peer, "laptop-B") == 0, 2000), "prompt peer '%s', want laptop-B", S().ui.peer);
        vTaskDelay(pdMS_TO_TICKS(BT_XREC_SSP_ARM_MS + 50u));
        PCHECK(bthx_call(BTHX_SSP_ANSWER, 0u, 2000u) == 0, "BACK on the prompt");
        PCHECK(WAIT(fake_peer.host_rejects == 1u || fake_peer.auth_fail > 0u, 3000), "no negative confirmation reply");
        PCHECK(fake_peer.pairings == 0u && fake_peer.host_confirms == 0u, "B paired after BACK (pairings %u confirms "
               "%u)", fake_peer.pairings, fake_peer.host_confirms);
        (void)WAIT(S().d.ssp_user_rejected != s0.d.ssp_user_rejected, 2000);   /* snapshot can trail the reply */
        bthx_snap_t s1 = S();
        PCHECK(s1.d.ssp_user_rejected == s0.d.ssp_user_rejected + 1u && !s1.ui.prompt, "user reject not counted "
               "(%u -> %u, prompt %u)", s0.d.ssp_user_rejected, s1.d.ssp_user_rejected, s1.ui.prompt);
        /* a second key right after the answer is swallowed (-3: not "leave BT mode" / "open a window") */
        PCHECK(bthx_call(BTHX_SSP_ANSWER, 1u, 2000u) == -3, "a late ENTER was not swallowed after BACK");
        vTaskDelay(pdMS_TO_TICKS(BT_XREC_SSP_AFTER_MS + 50u));
        PCHECK(bthx_call(BTHX_SSP_ANSWER, 1u, 2000u) == -1, "ENTER 1 s after BACK found a prompt");
        vTaskDelay(pdMS_TO_TICKS(100));
        PCHECK(S().d.ssp_window && S().bonds == 1, "after BACK: window %u (want still open), bonds %d (want only A)",
               S().d.ssp_window, S().bonds);
        peer_drop(0x13u);
        PCHECK(WAIT(S().d.acl_links == 0u && !S().ui.connected, 3000), "B link not down");
    }

    /* ---- 16b ---- */
    /* Silicon session 6 (docs/BT_HW_RESULTS.md): first pairing with a laptop the device knew no name for. (1) the prompt
     * showed its address: BTM's own name request at link set-up (btm_sec_connected -> btm_sec_start_get_name) was
     * still running when the User Confirmation Request came, so BTA's request (bta_dm_sp_cback) and the session's were
     * refused (ROM: RNR already running -> CS 0x0C) and the late answer reached BTM only. (2) after BACK the screen kept
     * "verbunden": BTM disconnects a link whose pairing failed (0406 after Simple Pairing Complete), and no
     * Disconnection Complete came while the user looked (modelled: the ROM never reports the link's end). */
    bth_log("[P16b] laptop E (name unknown) pairs; BTM's link set-up name request answers only during the prompt (later "
            "requests refused 0x0C): address first, then the name in place; BACK, the device disconnects and the ROM "
            "never reports the end: not \"connected\"\n");
    {
        PCHECK(S().d.ssp_window, "window closed before E");
        fake_peer_cfg_t e;
        peer_cfg(&e, 0xE5u, 0x01u, 0x05u, 0x05u, "laptop-E");
        peer_attach(&e, NULL);
        bth_rom_lock();
        fake_peer.name_hold = true;
        bth_rom_unlock();
        if (!peer_page_and_connect()) {
            goto out;
        }
        PCHECK(fake_peer.name_held, "no name request at link set-up");
        PCHECK(S().ui.connected && S().st.acl_connected, "E's link not shown as connected");
        char want[8];
        snprintf(want, sizeof(want), "%06u", (unsigned)(e.numeric % 1000000u));
        peer_authenticate();
        PCHECK(WAIT(S().ui.prompt && strcmp(S().ui.digits, want) == 0, 3000), "prompt with E's value %s ('%s')", want,
               S().ui.digits);
        vTaskDelay(pdMS_TO_TICKS(300));                /* the session's own name request is refused meanwhile */
        bthx_snap_t s1 = S();
        PCHECK(s1.ui.prompt && !s1.ui.peer_is_name && strchr(s1.ui.peer, ':') != NULL, "prompt peer before the name "
               "'%s', want the address", s1.ui.peer);
        PCHECK(fake_peer.name_refused >= 2u && s1.d.name_failed >= 1u, "later name requests not refused (%u, session "
               "failures %u)", fake_peer.name_refused, s1.d.name_failed);
        bth_rom_lock();
        fake_peer_name_release();                      /* the link set-up request completes, prompt still up */
        bth_rom_unlock();
        bth_ctrl_kick();
        PCHECK(WAIT(S().ui.prompt && strcmp(S().ui.peer, "laptop-E") == 0, 2000), "prompt peer '%s' after the late "
               "name, want laptop-E in place of the address", S().ui.peer);
        PCHECK(S().ui.prompt && strcmp(S().ui.digits, want) == 0, "prompt digits changed under the name update");
        vTaskDelay(pdMS_TO_TICKS(BT_XREC_SSP_ARM_MS + 50u));
        bth_rom_lock();
        fake_peer.defer_detach = true;                 /* the device's 0406 gets CS 0, no Disconnection Complete */
        bth_rom_unlock();
        PCHECK(bthx_call(BTHX_SSP_ANSWER, 0u, 2000u) == 0, "BACK on E's prompt");
        PCHECK(WAIT(fake_peer.host_rejects == 1u, 3000) && fake_peer.pairings == 0u, "E: no negative reply");
        PCHECK(WAIT(fake_peer.detach_deferred >= 1u, 3000), "the device did not disconnect E after the rejection");
        PCHECK(WAIT(!S().ui.connected && !S().st.acl_connected, 2000), "screen still \"connected\" after BACK (ui %u "
               "status %u, acl_links %u)", S().ui.connected, S().st.acl_connected, S().d.acl_links);
        PCHECK(S().d.acl_links == 1u && S().d.acl_closing == 1u, "link counted until reported down (acl %u closing %u)",
               S().d.acl_links, S().d.acl_closing);
        bth_rom_lock();
        fake_peer.defer_detach = false;
        fake_peer_disconnect(0x08u);                   /* the ROM's supervision timeout ends it */
        bth_rom_unlock();
        bth_ctrl_kick();
        PCHECK(WAIT(S().d.acl_links == 0u && S().d.acl_closing == 0u && !S().ui.connected, 3000), "E link not down "
               "(acl %u closing %u)", S().d.acl_links, S().d.acl_closing);
        PCHECK(S().d.ssp_window && S().bonds == 1, "after E: window %u, bonds %d (want only A)", S().d.ssp_window,
               S().bonds);
    }
    if (p_fail) {
        goto out;
    }

    /* ---- 17 ---- */
    bth_log("[P17] BENCH autoconfirm (SD:/BTBENCH.RUN autoconfirm=1): laptop D accepted without the user; D: XREC "
            "CHAL/AUTH twice, GETS\n");
    uint8_t dkey[16];
    {
        PCHECK(bthx_call(BTHX_BENCH_AUTOCONFIRM, 1u, 2000u) == 0, "bench autoconfirm on");
        PCHECK(S().d.ssp_window, "window closed before D");
        fake_peer_cfg_t d;
        peer_cfg(&d, 0xD4u, 0x01u, 0x05u, 0x05u, "laptop-D");
        /* silicon bench 2026-11-24: the peer's name was not known when its User Confirmation Request came, so BTA
         * fetched it first (bta_dm_sp_cback -> bta_dm_pinname_cback) and reported a stale value ("value 0" logged
         * while the laptop showed 230749; patch 0028) */
        peer_attach(&d, NULL);
        bth_rom_lock();
        fake_peer.name_fail_n = 1u;                  /* the name request of the link set-up fails */
        bth_rom_unlock();
        bthx_snap_t s0 = S();
        if (!peer_page_and_connect()) {
            goto out;
        }
        peer_authenticate();
        bool paired = WAIT(fake_peer.pairings == 1u && fake_peer.encrypted, 5000);
        PCHECK(paired && fake_peer.host_confirms == 1u && fake_peer.host_rejects == 0u, "D not paired by the bench "
               "override (pairings %u confirms %u rejects %u)", fake_peer.pairings, fake_peer.host_confirms,
               fake_peer.host_rejects);
        PCHECK(fake_peer.name_fail_n == 0u, "the link set-up name request was not made");
        (void)WAIT(bthx_ssp_value_logged() == d.numeric % 1000000u, 2000);   /* the log line can trail the pairing */
        PCHECK(bthx_ssp_value_logged() == d.numeric % 1000000u, "bench accept logged value %u, want the comparison "
               "value %u (name unknown at the request)", (unsigned)bthx_ssp_value_logged(),
               (unsigned)(d.numeric % 1000000u));
        (void)WAIT(S().d.ssp_bench_autoconfirmed != s0.d.ssp_bench_autoconfirmed, 2000);   /* snapshot can trail */
        bthx_snap_t s1 = S();
        PCHECK(s1.d.ssp_bench_autoconfirmed == s0.d.ssp_bench_autoconfirmed + 1u &&
               s1.d.ssp_user_accepted == s0.d.ssp_user_accepted && !s1.ui.prompt,
               "bench accept counters (bench %u -> %u, user %u -> %u, prompt %u)", s0.d.ssp_bench_autoconfirmed,
               s1.d.ssp_bench_autoconfirmed, s0.d.ssp_user_accepted, s1.d.ssp_user_accepted, s1.ui.prompt);
        PCHECK(WAIT(S().bonds == 2 && !S().d.ssp_window && S().d.scan_applied == 1u, 3000), "D's MITM bond: bonds %d, "
               "SSP part %u, scan %u (one bond per window)", S().bonds, S().d.ssp_window, S().d.scan_applied);
        memcpy(dkey, fake_peer.key, 16u);
        PCHECK(bthx_call(BTHX_BENCH_AUTOCONFIRM, 0u, 2000u) == 0, "bench autoconfirm off");
        if (!step_rfcomm_open()) {
            goto out;
        }
        PCHECK(x_send("+XREC:PING\r") && x_line(line, sizeof(line), 3000u), "PING (D)");
        PCHECK(strncmp(line, "+XREC:PONG,openfw_v5,v5,bound=1,auth=0", 38) == 0, "PONG (D) '%s'", line);
        uint8_t n1[16];
        PCHECK(xrec_auth_chal(xkey), "CHAL/AUTH (D)");
        memcpy(n1, s_chal_nonce, 16u);
        PCHECK(xrec_auth_chal(xkey), "second CHAL/AUTH (D)");
        static const uint8_t zero16[16] = { 0 };
        PCHECK(memcmp(n1, s_chal_nonce, 16u) != 0 && memcmp(n1, zero16, 16u) != 0, "CHAL nonces repeat or are zero");
        PCHECK(gets_verify("REC0001.WAV", 1u, 2000u, 4000u, 640u, -1, NULL), "GETS (D)");
        peer_drop(0x13u);
        PCHECK(WAIT(!S().d.conn_open && S().d.acl_links == 0u, 3000), "D link not down");
    }
    if (p_fail) {
        goto out;
    }

    /* ---- 18 ---- */
    bth_log("[P18] BT-mode entry without credited entropy: nothing opens (bench override on), B rejected without a "
            "prompt, D's CHAL fails closed; a later credit gives the XREC PIN but no SSP (ROM key)\n");
    stop_session(&base, "stop before the no-entropy session");
    bthx_withhold_entropy(1);
    rom_cmds_clear();
    if (!wait_ready()) {
        bthx_withhold_entropy(0);
        goto out;
    }
    {
        PCHECK(bthx_call(BTHX_BENCH_AUTOCONFIRM, 1u, 2000u) == 0, "bench autoconfirm on");
        uint32_t r0 = S().d.ssp_gate_refused;
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        PCHECK(WAIT(S().d.ssp_gate_refused == r0 + 1u, 1000), "gate refusal not counted (%u)", S().d.ssp_gate_refused);
        s = S();
        PCHECK(!s.d.window_open && !s.d.ssp_window && !s.ui.window_open && s.d.scan_applied == 1u &&
               s.st.pair_pin[0] == '\0', "no entropy: window %u ssp %u ui %u scan %u PIN '%s' (want nothing open)",
               s.d.window_open, s.d.ssp_window, s.ui.window_open, s.d.scan_applied, s.st.pair_pin);
        fake_peer_cfg_t b;
        peer_cfg(&b, 0xB2u, 0x01u, 0x05u, 0x05u, "laptop-B");
        peer_attach(&b, NULL);
        uint32_t rej0 = S().d.ssp_rejected;
        if (!peer_page_and_connect()) {
            bthx_withhold_entropy(0);
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(fake_peer.host_rejects == 1u || fake_peer.auth_fail > 0u, 3000), "no negative confirmation reply");
        PCHECK(WAIT(S().d.ssp_rejected == rej0 + 1u, 2000), "rejection not counted");
        s = S();
        PCHECK(fake_peer.pairings == 0u && s.d.ssp_bench_autoconfirmed == 0u && !s.ui.prompt && s.bonds == 2,
               "B with no entropy: pairings %u bench accepts %u prompt %u bonds %d", fake_peer.pairings,
               s.d.ssp_bench_autoconfirmed, s.ui.prompt, s.bonds);
        peer_drop(0x13u);
        PCHECK(WAIT(S().d.acl_links == 0u, 3000), "B link not down");

        fake_peer_cfg_t d;
        peer_cfg(&d, 0xD4u, 0x01u, 0x05u, 0x05u, "laptop-D");
        peer_attach(&d, dkey);
        if (!peer_page_and_connect()) {
            bthx_withhold_entropy(0);
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "D authentication with its stored key");
        if (!step_rfcomm_open()) {
            bthx_withhold_entropy(0);
            goto out;
        }
        PCHECK(x_send("+XREC:CHAL\r"), "CHAL (no entropy)");
        bool reply = x_line(line, sizeof(line), 800u);
        PCHECK(!reply || strncmp(line, "+XREC:CHAL,", 11) != 0, "CHAL answered without entropy: '%s'", line);
        PCHECK(WAIT(!S().d.conn_open && S().st.xrec_auth == 0u, 3000), "link not closed after the refused CHAL (open "
               "%u auth %u)", S().d.conn_open, S().st.xrec_auth);
        peer_drop(0x13u);
        PCHECK(WAIT(S().d.acl_links == 0u && !S().d.conn_open, 3000), "D link not down");

        /* deferred mic collection credits later in the session: the DRBG serves the XREC PIN again, but the ROM key
         * is only replaced at a controller start: the SSP part stays refused until the next BT-mode entry */
        bthx_withhold_entropy(0);
        r0 = S().d.ssp_gate_refused;
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        PCHECK(WAIT(S().d.ssp_gate_refused == r0 + 1u, 1000), "gate refusal (ROM key) not counted");
        s = S();
        PCHECK(s.d.window_open && strlen(s.st.pair_pin) == 6u && !s.d.ssp_window && s.d.scan_applied == 1u,
               "late credit: window %u PIN '%s' ssp %u scan %u (want XREC PIN only)", s.d.window_open, s.st.pair_pin,
               s.d.ssp_window, s.d.scan_applied);
        PCHECK(bthx_call(BTHX_BENCH_AUTOCONFIRM, 0u, 2000u) == 0, "bench autoconfirm off");
    }
    stop_session(&base, "stop after the no-entropy session");
    if (p_fail) {
        goto out;
    }

    /* ---- 19 ---- */
    bth_log("[P19] bench-style start (window opened right after bt_xrec_start, before READY): SSP part opens at READY; "
            "D: XREC PAIR with the PIN (new key), CHAL/AUTH, LS, GETS\n");
    rom_cmds_clear();
    {
        bth_step("bt_xrec_start + open_pairing -> READY", 20000u);
        PCHECK(bthx_call(BTHX_BENCH_XREC_PIN, 246810u, 2000u) == 0, "bench XREC PIN (xrec_pin=246810)");
        bthx_hold_bringup(1);            /* the controller has not started when the window opens (as on target) */
        PCHECK(bthx_call(BTHX_START_PAIR, 60000u, 3000u) == 0, "bt_xrec_start + bt_xrec_open_pairing");
        bool up = WAIT(S().d.state == BT_XREC_ST_UP && S().d.window_open, 1000);
        vTaskDelay(pdMS_TO_TICKS(200));
        bthx_snap_t s0 = S();
        bthx_hold_bringup(0);
        PCHECK(up && s0.d.state == BT_XREC_ST_UP && s0.d.ssp_pending && !s0.d.ssp_window && s0.d.ssp_gate_refused == 0u &&
               strlen(s0.st.pair_pin) == 6u, "before READY: state %u pending %u ssp %u refused %u PIN '%s' (want the gate "
               "deferred, the XREC PIN out)", s0.d.state, s0.d.ssp_pending, s0.d.ssp_window, s0.d.ssp_gate_refused,
               s0.st.pair_pin);
        bool ok = WAIT(S().d.state == BT_XREC_ST_READY || S().d.state == BT_XREC_ST_FAULTED, 15000);
        bth_step_done();
        PCHECK(ok && S().d.state == BT_XREC_ST_READY, "session not READY (state %u)", S().d.state);
        PCHECK(WAIT(S().d.ssp_window && S().d.scan_disc == 1u, 2000), "SSP part not opened at READY (ssp %u pending %u "
               "refused %u)", S().d.ssp_window, S().d.ssp_pending, S().d.ssp_gate_refused);
        s = S();
        PCHECK(s.d.ssp_gate_refused == 0u && !s.d.ssp_pending && s.d.window_open && strlen(s.st.pair_pin) == 6u,
               "deferred gate: refused %u pending %u window %u PIN '%s'", s.d.ssp_gate_refused, s.d.ssp_pending,
               s.d.window_open, s.st.pair_pin);
        PCHECK(WAIT(rom_cmd_last_byte0(0xE02Cu, &found) == 0x03u, 2000), "0C1A 0x03 (inquiry + page scan) not sent");

        fake_peer_cfg_t d;
        peer_cfg(&d, 0xD4u, 0x01u, 0x05u, 0x05u, "laptop-D");
        peer_attach(&d, dkey);
        if (!peer_page_and_connect()) {
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "D authentication with its stored key");
        if (!step_rfcomm_open()) {
            goto out;
        }
        PCHECK(x_send("+XREC:PING\r") && x_line(line, sizeof(line), 3000u), "PING");
        PCHECK(strncmp(line, "+XREC:PONG,openfw_v5,v5,bound=1,auth=0,pair=1", 45) == 0, "PONG '%s'", line);
        static const uint8_t xkey2[32] = { 0xA5, 0x5A, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 31, 30, 29, 28,
                                           27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10 };
        char k[80], cmd[128];
        hex(k, xkey2, 32u);
        uint32_t kw0 = bthx_key_writes();
        s = S();
        PCHECK(strcmp(s.st.pair_pin, "246810") == 0, "window PIN '%s', want the bench PIN 246810", s.st.pair_pin);
        snprintf(cmd, sizeof(cmd), "+XREC:PAIR,%s,%s\r", "246810", k);   /* the operator knows it in advance */
        PCHECK(x_send(cmd) && x_line(line, sizeof(line), 3000u), "PAIR");
        PCHECK(strcmp(line, "+XREC:OK,PAIR") == 0, "PAIR reply '%s'", line);
        PCHECK(WAIT(!S().d.window_open && S().d.scan_applied == 1u, 2000), "window/discoverability not closed by PAIR");
        PCHECK(WAIT(bthx_key_writes() == kw0 + 1u, 2000), "/cassini.key not rewritten");
        PCHECK(xrec_auth_chal(xkey2), "CHAL/AUTH with the new key");
        PCHECK(x_send("+XREC:LS\r"), "LS");
        int files = 0;
        while (x_line(line, sizeof(line), 3000u) && strcmp(line, "+XREC:END") != 0) {
            files++;
        }
        PCHECK(strcmp(line, "+XREC:END") == 0 && files == 2, "LS listing (%d files, last '%s')", files, line);
        PCHECK(gets_verify("REC0001.WAV", 1u, 0u, 3000u, 640u, -1, NULL), "GETS (small file)");
        peer_drop(0x13u);
        PCHECK(WAIT(!S().d.conn_open && S().d.acl_links == 0u, 3000), "D link not down");
        PCHECK(bthx_call(BTHX_BENCH_XREC_PIN, 0xFFFFFFFFu, 2000u) == 0, "bench XREC PIN off (bt_bench_end)");
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        PCHECK(WAIT(S().d.window_open && strlen(S().st.pair_pin) == 6u, 1000) &&
               strcmp(S().st.pair_pin, "246810") != 0, "after the bench run: DRBG PIN again ('%s')", S().st.pair_pin);
    }

    /* ---- 20 ---- */
    /* Silicon bench 2026-11-24 (docs/BLUETOOTH_BLUEDROID_PLAN.md live log): after a long GETS the laptop closed RFCOMM
     * and dropped the ACL, but the ROM never reported the detach (no Disconnection Complete for 20 s); the host's own
     * idle disconnect left its link DISCONNECTING, so the laptop's next pages were rejected 0x0F; then the bench
     * reopened the pairing window and the ROM refused 0C1A 03 (more than one ACL link: ROM 0x3EAC4) -> class-M
     * INVARIANT -> BT stopped. Why the ROM kept the first link is not modelled (unknown); everything after it is. */
    bth_log("[P20] ROM keeps a dead link after a long GETS; re-page rejected 0x0F; the pairing window reopens with two "
            "ROM links (0C1A refused): no BT fault; retried on a timer when the rejected link ends first; a refused window "
            "close fails closed (scan off); re-applied when the dead link goes, D reconnects\n");
    {
        rom_cmds_clear();
        bthx_call(BTHX_OPEN_PAIRING, 1u, 2000u);                 /* close P19's window (deadline passed at once) */
        PCHECK(WAIT(!S().d.window_open && S().d.scan_applied == 1u, 2000), "window not closed (open %u scan %u)",
               S().d.window_open, S().d.scan_applied);
        fake_peer_cfg_t d;
        peer_cfg(&d, 0xD4u, 0x01u, 0x05u, 0x05u, "laptop-D");
        peer_attach(&d, dkey);
        if (!peer_page_and_connect()) {
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "D authentication with its stored key");
        if (!step_rfcomm_open()) {
            goto out;
        }
        static const uint8_t xkey2[32] = { 0xA5, 0x5A, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 31, 30, 29, 28,
                                           27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10 };
        PCHECK(xrec_auth_chal(xkey2), "CHAL/AUTH (D)");
        PCHECK(gets_verify("BIG0002.WAV", 2u, 0u, 300000u, 640u, -1, NULL), "long GETS (300000 B)");
        bthx_snap_t s0 = S();
        uint32_t fl0 = bth_ctrl_faults();
        /* the laptop closes RFCOMM, then drops the link; the ROM never reports that detach (modelled: nothing), and
         * the device's own idle disconnect is accepted (CS 0) but never completes either */
        bth_rom_lock();
        fake_peer.defer_detach = true;
        bth_rom_unlock();
        l2_disconnect(&P.rfc);
        PCHECK(WAIT(!S().d.conn_open, 3000), "SPP still open after the laptop closed its L2CAP channel");
        PCHECK(WAIT(fake_peer.detach_deferred >= 1u, 10000), "the device never sent its idle disconnect (0406)");
        PCHECK(S().d.acl_links == 1u, "session sees the link go (acl_links %u) without a Disconnection Complete",
               S().d.acl_links);
        /* the laptop pages again: rejected 0x0F (Bluedroid: link still DISCONNECTING, l2c_link_hci_conn_req) */
        bth_rom_lock();
        int pr = fake_peer_page_again(1u);
        bth_rom_unlock();
        bth_ctrl_kick();
        PCHECK(pr == 0 && WAIT(fake_peer.ghost_rejects == 1u, 3000), "second page not answered (rejects %u)",
               fake_peer.ghost_rejects);
        PCHECK(fake_peer.ghost_reject_reason == 0x0Fu, "second page rejected with 0x%02X, want 0x0F (silicon)",
               fake_peer.ghost_reject_reason);
        /* the bench reopens the pairing window: 0C1A 03 with two ROM links */
        uint32_t refused0 = fake_rom.scan_refused;
        bthx_call(BTHX_OPEN_PAIRING, 60000u, 2000u);
        /* BTA writes the scan mode twice (BTM_SetDiscoverability, BTM_SetConnectability): both refused */
        PCHECK(WAIT(fake_rom.scan_refused >= refused0 + 1u, 2000), "0C1A 03 not refused by the ROM rule (%u)",
               fake_rom.scan_refused - refused0);
        vTaskDelay(pdMS_TO_TICKS(300));
        uint32_t refused1 = fake_rom.scan_refused;
        bthx_snap_t s1 = S();
        PCHECK(bth_ctrl_faults() == fl0 && s1.d.state == BT_XREC_ST_READY, "BT fault on the refused 0C1A (faults %u, "
               "state %u, last fault %u)", bth_ctrl_faults() - fl0, s1.d.state, s1.d.last_fault);
        PCHECK(s1.d.scan_refused == s0.d.scan_refused + (refused1 - refused0) && s1.d.ctrl_notes > s0.d.ctrl_notes,
               "refusal not seen "
               "by the session (refused %u notes %u)", s1.d.scan_refused - s0.d.scan_refused,
               s1.d.ctrl_notes - s0.d.ctrl_notes);
        if (p_fail) {
            goto out;
        }
        PCHECK(fake_rom.scan_enable == 0x02u, "ROM scan value 0x%02X after the refusal, want 0x02 (kept)",
               fake_rom.scan_enable);
        /* (review) the rejected re-page ends first: a failed Connection Complete, no GAP link-down event for the
         * session. The refused window mode is sent again on the retry timer and is accepted (one ROM link left). */
        bth_rom_lock();
        fake_peer_ghost_end(0x0Fu);
        bth_rom_unlock();
        bth_ctrl_kick();
        PCHECK(WAIT(fake_rom.scan_enable == 0x03u, BT_XREC_SCAN_RETRY_MS + 3000u), "window never discoverable after "
               "the rejected link ended (ROM scan 0x%02X, reapplied %u)", fake_rom.scan_enable,
               S().d.scan_reapplied - s0.d.scan_reapplied);
        PCHECK(WAIT(S().d.acl_links == 1u && S().d.scan_reapplied >= s0.d.scan_reapplied + 1u, 1000), "retry without a link-down "
               "event (acl %u reapplied %u)", S().d.acl_links, S().d.scan_reapplied - s0.d.scan_reapplied);
        if (p_fail) {
            goto out;
        }
        /* (review) the laptop pages again (rejected 0x0F, counted), then the window closes: both 0C1A 02 writes are
         * refused while the ROM is at 0x03. The session fails closed: scan off (0x00), not discoverable. */
        bth_rom_lock();
        pr = fake_peer_page_again(1u);
        bth_rom_unlock();
        bth_ctrl_kick();
        PCHECK(pr == 0 && WAIT(fake_peer.ghost_rejects == 2u, 3000), "third page not answered (rejects %u)",
               fake_peer.ghost_rejects);
        bthx_snap_t s2 = S();
        uint32_t refused2 = fake_rom.scan_refused;
        bthx_call(BTHX_OPEN_PAIRING, 1u, 2000u);         /* window closes (deadline passed at once) */
        PCHECK(WAIT(fake_rom.scan_refused >= refused2 + 1u, 2000), "0C1A 02 not refused (%u)",
               fake_rom.scan_refused - refused2);
        PCHECK(WAIT(fake_rom.scan_enable == 0x00u && S().d.scan_failsafe == s2.d.scan_failsafe + 1u, 2000),
               "device left discoverable after the window closed (ROM scan 0x%02X, failsafe %u)",
               fake_rom.scan_enable, S().d.scan_failsafe - s2.d.scan_failsafe);
        PCHECK(!S().d.window_open && !S().d.scan_disc && bth_ctrl_faults() == fl0, "state after the fail-safe");
        if (p_fail) {
            goto out;
        }
        /* the ROM's supervision timeout ends the dead link (Disconnection Complete 0x08): the session re-applies its
         * scan mode at once, now accepted (one ROM link: the rejected one) */
        rom_cmds_clear();
        uint32_t refused3 = fake_rom.scan_refused;
        uint32_t ra3 = S().d.scan_reapplied;
        bth_rom_lock();
        fake_peer.defer_detach = false;
        fake_peer_disconnect(0x08u);
        bth_rom_unlock();
        bth_ctrl_kick();
        PCHECK(WAIT(S().d.acl_links == 0u && S().d.scan_reapplied >= ra3 + 1u, 3000),
               "scan mode not re-applied after the link went (acl %u reapplied %u)", S().d.acl_links,
               S().d.scan_reapplied - ra3);
        int found2 = 0;
        PCHECK(WAIT(fake_rom.scan_enable == 0x02u && rom_cmd_last_byte0(0xE02Cu, &found2) == 0x02u && found2, 2000),
               "re-applied 0C1A 02 not accepted (ROM scan 0x%02X, refused %u more)", fake_rom.scan_enable,
               fake_rom.scan_refused - refused3);
        bth_rom_lock();
        fake_peer_ghost_end(0x0Fu);                       /* the second rejected link's completion, late */
        bth_rom_unlock();
        bth_ctrl_kick();
        vTaskDelay(pdMS_TO_TICKS(200));
        PCHECK(bth_ctrl_faults() == fl0 && S().d.state == BT_XREC_ST_READY, "fault after the late completions");
        PCHECK(fake_rom.scan_enable == 0x02u, "ROM scan 0x%02X at the end, want 0x02", fake_rom.scan_enable);
        /* D pages again: accepted, reconnects with its key, XREC works */
        peer_attach(&d, dkey);
        if (!peer_page_and_connect()) {
            goto out;
        }
        peer_authenticate();
        PCHECK(WAIT(fake_peer.authenticated && fake_peer.encrypted, 3000), "D re-authentication");
        if (!step_rfcomm_open()) {
            goto out;
        }
        PCHECK(x_send("+XREC:PING\r") && x_line(line, sizeof(line), 3000u), "PING after the recovery");
        peer_drop(0x13u);
        PCHECK(WAIT(!S().d.conn_open && S().d.acl_links == 0u, 3000), "D link not down");
    }
    if (p_fail) {
        goto out;
    }

    /* ---- 22 ---- */
    bth_log("[P22] WDT supervisor: BTC stuck in a work item -> platform fault 4 -> graceful recovery; restart\n");
    {
        uint32_t f0 = bthx_sup_faults();
        uint32_t h0 = bthx_hard_path_reason();
        PCHECK(bthx_block_thread(BT_TASK_SUP_BTC, 1) == 0, "blocker not posted");
        PCHECK(WAIT(bthx_block_running(BT_TASK_SUP_BTC), 1000), "blocker never ran");
        uint64_t t0 = us_timer_now();
        bool seen = WAIT(bthx_sup_faults() != f0, SUP_STALL_MS + SUP_PERIOD_MS + 1000u);
        uint32_t ms = (uint32_t)((us_timer_now() - t0) / 1000u);
        bthx_block_thread(BT_TASK_SUP_BTC, 0);     /* the stuck work ends: the teardown can run through BTC */
        PCHECK(seen, "stuck BTC not reported");
        PCHECK(ms >= SUP_STALL_MS - SUP_PERIOD_MS && ms <= SUP_STALL_MS + SUP_PERIOD_MS + 200u,
               "reported after %u ms (stall %u, period %u)", ms, SUP_STALL_MS, SUP_PERIOD_MS);
        PCHECK(WAIT(S().d.state == BT_XREC_ST_FAULTED, 4000), "not FAULTED after the stall (state %u)", S().d.state);
        bthx_snap_t s2 = S();
        PCHECK(s2.d.last_fault == BT_XREC_FAULT_PLATFORM && bthx_hard_path_reason() == h0,
               "fault %u, hard path %u (want PLATFORM, graceful)", s2.d.last_fault, bthx_hard_path_reason());
        bt_task_sup_stats_t st;
        bthx_sup_stats(&st);
        PCHECK(!st.armed && !bthx_sup_hold(), "supervisor still armed / holding after the teardown");
        bth_log("    BTC stall reported after %u ms; graceful recovery (FAULTED)\n", ms);
        /* not wait_ready(): its READY-or-FAULTED wait would end on the snapshot of this fault */
        PCHECK(bthx_call(BTHX_START, 0u, 3000u) == 0, "bt_xrec_start after the fault");
        if (!WAIT(S().d.state == BT_XREC_ST_READY, 15000)) {
            PCHECK(0, "not READY after the fault (state %u)", S().d.state);
            goto out;
        }
        sup_quiet("after the restart", 2u * SUP_STALL_MS);
    }
    if (p_fail) {
        goto out;
    }

    /* ---- 21 ---- */
    bth_log("[P21] bt_xrec_stop\n");
    stop_session(&base, "final stop");
    {
        static uint8_t img[BT_STORE_IMAGE_MAX];
        int n = bthx_read_root_file("bt_bonds.bin", img, sizeof(img));
        static const char c_addr[] = "00:5b:1a:51:7e:c3";
        static const char b_addr[] = "00:5b:1a:51:7e:b2";
        static const char a_addr[] = "00:5b:1a:51:7e:a1";
        PCHECK(n > 0 && mem_has(img, (uint32_t)n, a_addr, 17u), "A missing from /bt_bonds.bin");
        PCHECK(n > 0 && !mem_has(img, (uint32_t)n, b_addr, 17u) && !mem_has(img, (uint32_t)n, c_addr, 17u),
               "B or C persisted in /bt_bonds.bin");
    }
    {
        /* Review finding: nothing measured phone mode against the RAM budget. Host side: a scenario-local peak (all of
         * Bluedroid, bt_port, the session, task stacks) against a regression ceiling; see PHONE_HEAP_CEILING. */
        bth_heap_stats_t hs;
        bth_heap_stats(&hs);
        size_t peak = hs.peak_bytes - base.bytes;
        bth_log("    heap: scenario peak %zu B above the post-warm-up baseline (host ceiling %u B; x86-64 sizes)\n",
                peak, (unsigned)PHONE_HEAP_CEILING);
        PCHECK(peak <= PHONE_HEAP_CEILING, "scenario heap peak %zu B > host ceiling %u B", peak,
               (unsigned)PHONE_HEAP_CEILING);
    }
out:
    if (p_fail != 0 && S().d.state != BT_XREC_ST_OFF) {
        (void)bthx_call(BTHX_STOP, 0u, 8000u);
    }
    bth_log("phone scenario: %d checks, %d failures\n", p_checks, p_fail);
    return p_fail;
}
