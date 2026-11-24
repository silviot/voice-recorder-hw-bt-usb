/* Phone-mode end-to-end scenario for make test-bt-host (test_bt_host phone <exfat.img>): the real BT=1 XREC backend
 * (bt_port/bt_xrec_bluedroid.c + src/xrec_proto.c + src/exfat.c, platform tests/bt_host/host_xrec.c) on the real
 * Bluedroid (patches 0001-0023) + bt_port + bt_ctrl, against tests/fake_rom.c's scripted remote peer, which this file
 * extends with a laptop's host stack: L2CAP signalling, an SDP client, an RFCOMM client (TS 07.10 basic mode with
 * credit-based flow control) and the +XREC client side of tools/bt_sync.py.
 *
 * Steps (each one CHECKed; any failure stops the scenario):
 *   1. bt_xrec_start -> READY (SPP server on channel 1); pairing window opened -> discoverable (0C1A 0x03).
 *   2. Laptop A pages, authenticates: SSP numeric comparison (A DisplayYesNo) accepted inside the window; the link key
 *      is notified, stored through bt_store and written to /bt_bonds.bin; A encrypts.
 *   3. SDP: ServiceSearchAttribute for 0x1101 finds "Cassini" on RFCOMM channel 1.
 *   4. RFCOMM: mux SABM, PN (CFC), DLC SABM, MSC -> ESP_SPP_SRV_OPEN, link authorized for XREC.
 *   5. XREC: bt_sync.py's native-SPP start (no HFP AT lines from the device), PING, PAIR with the window's PIN
 *      (window and discoverability close), LS, a multi-frame GETS of a 40000-byte file with congestion (the peer
 *      withholds RFCOMM credits and baseband acks mid-transfer), byte-exact.
 *   6. A drops the link (supervision timeout): SPP closed, XREC authorization cleared.
 *   7. A reconnects with the stored key, window closed: no new pairing; RFCOMM straight to channel 1; CHAL/AUTH; GETS.
 *   8. bt_xrec_stop with a 300000-byte GETS outstanding and the peer stalled: graceful teardown inside the 2 s
 *      deadline, no hard path; heap / tasks / esp_timer back to the pre-start baseline.
 *   9. Restart: bonds from /bt_bonds.bin, XREC key from /cassini.key: A reconnects (stored key) and authenticates.
 *  10. Laptop B tries to pair with the window closed: numeric comparison rejected, no bond.
 *  11. Window open, laptop C (NoInputNoOutput): Just Works pairing completes in Bluedroid, the session removes the
 *      unauthenticated bond and drops the link; C never reaches XREC.
 *  12. bt_xrec_stop: baseline restored, no fake-ROM contract error, no unknown opcode, no BT fault, no HCI timeout.
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
static bool rfc_connect(uint8_t scn)
{
    l2_connect(&P.rfc, 0x0003u, LCID_RFC);
    if (!WAIT(l2_open(&P.rfc) || P.rfc.closed, 5000)) {
        PCHECK(0, "L2CAP RFCOMM channel not configured (pending %u)", P.rfc.pending);
        return false;
    }
    PCHECK(!P.rfc.closed, "L2CAP RFCOMM refused (%u)", P.rfc.refused);
    rfc_frame(0u, 0x3Fu, true, NULL, 0u, -1);                 /* SABM(P) on DLCI 0 */
    if (!WAIT(P.mux_up || P.mux_dm, 3000) || !P.mux_up) {
        PCHECK(0, "RFCOMM multiplexer not up");
        return false;
    }
    P.dlci = (uint8_t)(scn << 1);                              /* initiator: D = 0 */
    uint8_t pn[8] = { P.dlci, 0xF0u, 0u, 0u, (uint8_t)PEER_RFC_N1, (uint8_t)(PEER_RFC_N1 >> 8), 0u, 7u };
    P.rx_credits = 7;
    mux_send(0x20u, true, pn, 8u);
    if (!WAIT(P.pn_rsp, 3000)) {
        PCHECK(0, "no PN response");
        return false;
    }
    PCHECK(P.cfc, "device did not accept credit-based flow control");
    rfc_frame(P.dlci, 0x3Fu, true, NULL, 0u, -1);             /* SABM(P) on the server channel */
    if (!WAIT(P.dlc_up || P.dlc_dm, 5000) || !P.dlc_up) {
        PCHECK(0, "DLC %u not opened (DM %u)", P.dlci, P.dlc_dm);
        return false;
    }
    uint8_t msc[2] = { (uint8_t)((P.dlci << 2) | 0x03u), 0x8Du };   /* RTC, RTR, DV */
    mux_send(0x38u, true, msc, 2u);
    PCHECK(WAIT(P.msc_cmd_rx && P.msc_rsp_rx, 3000), "MSC exchange incomplete (cmd %u rsp %u)", P.msc_cmd_rx,
           P.msc_rsp_rx);
    rfc_grant();
    return true;
}

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
    peer_attach(&a, NULL);
    if (!peer_page_and_connect()) {
        return false;
    }
    uint32_t bw0 = bthx_bond_writes();
    char want[8];
    snprintf(want, sizeof(want), "%06u", (unsigned)(a.numeric % 1000000u));
    bth_step("SSP numeric comparison", 10000u);
    peer_authenticate();
    bool paired = WAIT(fake_peer.pairings == 1u && fake_peer.encrypted, 5000);
    bth_step_done();
    PCHECK(paired, "pairing (pairings %u, encrypted %u, confirms %u, rejects %u)", fake_peer.pairings,
           fake_peer.encrypted, fake_peer.host_confirms, fake_peer.host_rejects);
    PCHECK(fake_peer.host_io_cap == 0x01u, "our IO capability %u, want DisplayYesNo", fake_peer.host_io_cap);
    PCHECK(fake_peer.host_confirms == 1u && fake_peer.host_rejects == 0u, "user confirmation reply");
    bool seen = WAIT(S().d.ssp_accepted == 1u && S().bonds == 1, 3000);
    bthx_snap_t s = S();
    PCHECK(seen, "session: ssp_accepted %u, bonds %d", s.d.ssp_accepted, s.bonds);
    /* the value the user compares: logged and shown in bt_status_t.pair_pin from the confirmation request until the
     * bond completes (the fake laptop's user confirms at once, so the screen window is not sampled here) */
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

int bth_phone_scenario(const char *img_path)
{
    bth_log("[P] phone mode end to end: XREC backend + Bluedroid + bt_ctrl + fake ROM peer\n");
    if (load_card(img_path) != 0) {
        return 1;
    }
    PCHECK(bthx_init(&s_card) == 0, "bthx_init");
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
    bth_log("[P5] XREC: PING, PAIR, LS, GETS with congestion\n");
    PCHECK(x_send("+XREC:PING\r") && x_line(line, sizeof(line), 3000u), "PING");
    PCHECK(strncmp(line, "+XREC:PONG,openfw_v5,v5,bound=0,auth=0,pair=1", 45) == 0, "PONG '%s'", line);
    {
        /* bt_status_t.pair_pin shows the numeric comparison value until the bond completes, then the XREC PIN (the
         * user reads it off the screen and types it into the app / bt_sync.py --pair) */
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
    stop_session(&base, "stop with traffic outstanding");
    PCHECK(!fake_peer.connected, "link still up after bt_xrec_stop");
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
    }

    /* ---- 12 ---- */
    bth_log("[P12] bt_xrec_stop\n");
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
out:
    if (p_fail != 0 && S().d.state != BT_XREC_ST_OFF) {
        (void)bthx_call(BTHX_STOP, 0u, 8000u);
    }
    bth_log("phone scenario: %d checks, %d failures\n", p_checks, p_fail);
    return p_fail;
}
