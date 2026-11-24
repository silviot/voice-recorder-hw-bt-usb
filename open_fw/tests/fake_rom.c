/* fake_rom: see fake_rom.h.
 * SPDX-License-Identifier: MIT */
#include "fake_rom.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MSGS   256u
#define MSG_HDR    16u      /* struct ke_msg header (BT_ROM_INTERFACE §3.1) */
#define EM_BLOCK   0x40Cu
#define EM_HDR     12u

enum { M_FREE = 0, M_ALLOC, M_QUEUED, M_KEPT };

typedef struct {
    uint8_t *block;         /* MSG_HDR + len bytes; param = block + MSG_HDR */
    uint16_t id, dst, src, len;
    uint8_t state;
    uint8_t internal;       /* allocated by the fake itself (reply/injection) */
} msg_t;

typedef struct {
    uint16_t req, rsp;
    uint8_t status, ret_len, echo_addr, used;
} reply_t;

typedef struct {
    uint16_t handle;
    uint8_t *em;
} air_t;

fake_rom_t fake_rom;

static struct {
    msg_t msgs[MAX_MSGS];
    uint32_t q[MAX_MSGS];
    uint32_t q_head, q_n;
    uint32_t timer_ev;
    uint32_t pending_irq;
    uint8_t em_tx[BT_ROM_EM_TX_BUFS][EM_BLOCK];
    uint8_t em_rx[BT_ROM_EM_RX_BUFS][EM_BLOCK];
    uint8_t tx_used[BT_ROM_EM_TX_BUFS], rx_used[BT_ROM_EM_RX_BUFS];
    reply_t replies[64];
    uint32_t held[MAX_MSGS];
    uint32_t held_n;
    air_t air[BT_ROM_EM_TX_BUFS];
    uint32_t air_n;
    bool links[7];
    bool running, rf_cal_since_stop;
    bt_rom_upcalls_t up, stale_up;
    bool have_stale;
    uint32_t in_call;
} f;

static void err(const char *fmt, ...)
{
    fake_rom.errors++;
    if (fake_rom.errors == 1u) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(fake_rom.first_error, sizeof(fake_rom.first_error), fmt, ap);
        va_end(ap);
    }
}

/* Every bt_rom_if entry point (except the diagnostics) starts here. */
static uint32_t enter(uint8_t which)
{
    uint32_t idx = ++fake_rom.calls;
    fake_rom.calls_by[which]++;
    if (fake_rom.call_log_n < FAKE_ROM_LOG_MAX) {
        fake_rom.call_log[fake_rom.call_log_n++] = which;
    }
    if (fake_rom.stop_fired_at != 0u) {
        fake_rom.calls_after_stop++;
    } else if (fake_rom.stop_at_call != 0u && idx == fake_rom.stop_at_call && fake_rom.stop_flag != NULL) {
        *fake_rom.stop_flag = true;          /* SysTick deadline expired after bt_ctrl's check, before this call */
        fake_rom.stop_fired_at = idx;
    }
    f.in_call++;
    return idx;
}

static void leave(uint32_t idx)
{
    f.in_call--;
    if (idx == fake_rom.stop_fired_at) {
        fake_rom.stop_call_completed = 1u;
    }
}

/* ---- messages ---- */
static msg_t *find_param(const uint8_t *param)
{
    for (uint32_t i = 0u; i < MAX_MSGS; i++) {
        if (f.msgs[i].state != M_FREE && f.msgs[i].block + MSG_HDR == param) {
            return &f.msgs[i];
        }
    }
    return NULL;
}

static uint8_t *alloc_msg(uint16_t id, uint16_t dst, uint16_t src, uint16_t len, bool internal)
{
    uint32_t cost = MSG_HDR + ((len + 7u) & ~7u);
    if (f.running && fake_rom.heap_used + cost > fake_rom.heap_budget) {
        if (internal) {
            fake_rom.rom_alloc_faults++;
            if (f.up.on_fault != NULL) {
                f.up.on_fault(f.up.ctx, 1u);
            }
        } else {
            fake_rom.alloc_refused++;
        }
        return NULL;
    }
    for (uint32_t i = 0u; i < MAX_MSGS; i++) {
        msg_t *m = &f.msgs[i];
        if (m->state == M_FREE) {
            m->block = calloc(1u, MSG_HDR + len + 1u);
            m->id = id;
            m->dst = dst;
            m->src = src;
            m->len = len;
            m->state = M_ALLOC;
            m->internal = internal;
            fake_rom.heap_used += cost;
            if (fake_rom.heap_used > fake_rom.heap_peak) {
                fake_rom.heap_peak = fake_rom.heap_used;
            }
            return m->block + MSG_HDR;
        }
    }
    err("message table full");
    return NULL;
}

static void free_msg(msg_t *m)
{
    fake_rom.heap_used -= MSG_HDR + ((m->len + 7u) & ~7u);
    free(m->block);
    memset(m, 0, sizeof(*m));
}

static void queue_msg(msg_t *m)
{
    if (f.q_n >= MAX_MSGS) {
        err("queue full");
        return;
    }
    m->state = M_QUEUED;
    f.q[(f.q_head + f.q_n) % MAX_MSGS] = (uint32_t)(m - f.msgs);
    f.q_n++;
}

static void send_internal(uint16_t id, uint16_t dst, uint16_t src, const uint8_t *param, uint16_t len)
{
    uint8_t *p = alloc_msg(id, dst, src, len, true);
    if (p == NULL) {
        return;
    }
    if (len != 0u && param != NULL) {
        memcpy(p, param, len);
    }
    queue_msg(find_param(p));
}

/* ---- EM ---- */
static int em_locate(const uint8_t *p, bool *is_tx, uint32_t *idx, uint32_t *off)
{
    for (uint32_t i = 0u; i < BT_ROM_EM_TX_BUFS; i++) {
        if (p >= f.em_tx[i] + EM_HDR && p < f.em_tx[i] + EM_BLOCK) {
            *is_tx = true;
            *idx = i;
            *off = (uint32_t)(p - (f.em_tx[i] + EM_HDR));
            return 0;
        }
    }
    for (uint32_t i = 0u; i < BT_ROM_EM_RX_BUFS; i++) {
        if (p >= f.em_rx[i] + EM_HDR && p < f.em_rx[i] + EM_BLOCK) {
            *is_tx = false;
            *idx = i;
            *off = (uint32_t)(p - (f.em_rx[i] + EM_HDR));
            return 0;
        }
    }
    return -1;
}

static bool em_range_ok(const uint8_t *p, uint16_t len)
{
    bool tx;
    uint32_t idx, off;
    if (em_locate(p, &tx, &idx, &off) != 0) {
        return false;
    }
    if ((tx && !f.tx_used[idx]) || (!tx && !f.rx_used[idx])) {
        return false;
    }
    return off + len <= BT_ROM_EM_BUF_PAYLOAD;
}

/* ---- scripted remote peer (phone path) ----
 * Plays the ROM's LM/LC side of one remote device with the ROM's own message ids and naturally aligned layouts
 * (docs/BT_ROM_HCI_MAP.md §2 and §9; legacy bt_hci.c:741-850). Replies go to the command's src_id, from LM (0x38) or
 * from the link's LC instance ((link << 8) | 0x39). Each handler checks that bt_ctrl allocated at least the ROM param
 * struct size (a shorter message would make the ROM read past it). */
fake_peer_t fake_peer;
static fake_peer_cfg_t s_peer;

static void peer_emit(uint16_t id, uint16_t src, const uint8_t *p, uint16_t len)
{
    send_internal(id, BT_ROM_TASK_HCI, src, p, len);
    fake_peer.events_sent++;
}

static uint16_t peer_lc_src(void)
{
    return (uint16_t)((fake_peer.link << 8) | BT_ROM_TASK_LC);
}

static void peer_status1(uint16_t id, uint16_t src, uint8_t status)
{
    peer_emit(id, src, &status, 1u);
}

static void peer_s_addr(uint16_t id, uint16_t src, uint8_t status)   /* CC {s, bdaddr} (7) */
{
    uint8_t p[7];
    p[0] = status;
    memcpy(&p[1], s_peer.addr, 6u);
    peer_emit(id, src, p, 7u);
}

static void peer_s_h(uint16_t id, uint8_t status, const uint8_t *tail, uint16_t tail_len)   /* {s, pad, h, ...} */
{
    uint8_t p[16];
    memset(p, 0, sizeof(p));
    p[0] = status;
    p[2] = (uint8_t)fake_peer.handle;
    p[3] = (uint8_t)(fake_peer.handle >> 8);
    if (tail_len != 0u) {
        memcpy(&p[4], tail, tail_len);
    }
    peer_emit(id, peer_lc_src(), p, (uint16_t)(4u + tail_len));
}

static void peer_addr_evt(uint16_t id, uint16_t src)                   /* {bdaddr} (6) */
{
    peer_emit(id, src, s_peer.addr, 6u);
}

static void nocp_send(const uint16_t *handles, const uint16_t *counts, uint32_t n);

/* The ROM's link end (LC 0x2A8B4 -> 0x302B8, docs/BT_ROM_HCI_MAP.md §11): ACL still queued or in flight for the link
 * is freed (EM TX buffers back to the pool) and reported in one E462 NOCP {1, h, count} (ROM 0x29F5E) BEFORE the
 * Disconnection Complete. */
static void peer_flush_air(uint16_t h)
{
    uint16_t cnt = 0u;
    for (uint32_t i = 0u; i < f.air_n;) {
        if (f.air[i].handle != h) {
            i++;
            continue;
        }
        bool tx;
        uint32_t idx, off;
        if (em_locate(f.air[i].em, &tx, &idx, &off) == 0 && tx) {
            f.tx_used[idx] = 0u;
        }
        memmove(&f.air[i], &f.air[i + 1u], (f.air_n - i - 1u) * sizeof(air_t));
        f.air_n--;
        cnt++;
    }
    fake_rom.acl_in_air = f.air_n;
    if (cnt != 0u) {
        fake_peer.tx_flushed += cnt;
        nocp_send(&h, &cnt, 1u);
    }
}

static void peer_link_down(uint8_t reason)
{
    uint8_t r = reason;
    peer_flush_air(fake_peer.handle);
    peer_s_h(0xE458u, 0u, &r, 1u);                                       /* Disconnection Complete {s,pad,h,reason} */
    f.links[fake_peer.link] = false;
    fake_peer.connected = fake_peer.encrypted = fake_peer.authenticated = false;
    fake_peer.origin = FAKE_PEER_ORIGIN_NONE;
    fake_peer.last_disc_reason = reason;
}

static void peer_auth_done(uint8_t status)
{
    if (status == 0u) {
        fake_peer.authenticated = true;
        fake_peer.auth_ok++;
    } else {
        fake_peer.auth_fail++;
    }
    if (fake_peer.origin == FAKE_PEER_ORIGIN_HOST) {
        peer_s_h(0xE459u, status, NULL, 0u);                             /* Authentication Complete {s,pad,h} */
    } else if (status == 0u && s_peer.auto_encrypt) {
        uint8_t on = 1u;
        fake_peer.encrypted = true;
        peer_s_h(0xE45Au, 0u, &on, 1u);                                  /* the phone starts encryption */
    }
    fake_peer.origin = FAKE_PEER_ORIGIN_NONE;
}

static bool peer_need(const msg_t *m, uint16_t n)
{
    if (m->len < n) {
        err("command 0x%04X: %u param bytes, ROM struct needs %u", m->id, m->len, n);
        return false;
    }
    return true;
}

static bool peer_addr_is(const uint8_t *p)
{
    return memcmp(p, s_peer.addr, 6u) == 0;
}

static bool peer_handle(msg_t *m)
{
    if (!fake_peer.enabled) {
        return false;
    }
    const uint8_t *p = m->block + MSG_HDR;
    bool lc = (m->dst & 0xFFu) == BT_ROM_TASK_LC;
    if (lc && (!fake_peer.connected || (uint32_t)(m->dst >> 8) != fake_peer.link)) {
        return false;                                                    /* another link: default reply table */
    }
    switch (m->id) {
    case 0xE006u:                                                        /* 0409 Accept Connection Request */
        if (!peer_need(m, 7u) || !peer_addr_is(p)) return false;
        if (fake_peer.ghost_pending) {
            err("ghost page accepted by the host (the fake models one link per peer)");
            return false;
        }
        fake_peer.host_accept_role = p[6];
        peer_status1(0xE0C9u, BT_ROM_TASK_LM, 0u);
        f.links[fake_peer.link] = true;
        fake_peer.connected = true;
        fake_peer.encrypted = fake_peer.authenticated = false;
        fake_peer.handle = (uint16_t)(0x80u + fake_peer.link);
        fake_peer.conn_count++;
        {
            uint8_t c[13];
            memset(c, 0, sizeof(c));
            c[2] = (uint8_t)fake_peer.handle;                            /* {s, pad, h, pad, bdaddr, type, enc} */
            memcpy(&c[5], s_peer.addr, 6u);
            c[11] = 1u;
            peer_emit(0xE456u, peer_lc_src(), c, 13u);
        }
        return true;
    case 0xE007u:                                                        /* 040A Reject Connection Request */
        if (!peer_need(m, 7u) || !peer_addr_is(p)) return false;
        peer_status1(0xE0CAu, BT_ROM_TASK_LM, 0u);
        if (fake_peer.ghost_pending) {
            fake_peer.ghost_pending = false;
            fake_peer.ghost_up = true;
            fake_peer.ghost_rejects++;
            fake_peer.ghost_reject_reason = p[6];
            f.links[fake_peer.ghost_link] = true;                        /* still an LD link: its detach stalls */
            return true;
        }
        {
            uint8_t c[13];
            memset(c, 0, sizeof(c));
            c[0] = p[6];
            memcpy(&c[5], s_peer.addr, 6u);
            c[11] = 1u;
            peer_emit(0xE0D2u, BT_ROM_TASK_LM, c, 13u);
        }
        return true;
    case 0xE405u:                                                        /* 041B Read Remote Supported Features */
        if (!peer_need(m, 2u)) return false;
        peer_status1(0xE441u, peer_lc_src(), 0u);
        peer_s_h(0xE45Du, 0u, s_peer.features, 8u);
        return true;
    case 0xE406u: {                                                      /* 041C Read Remote Extended Features */
        if (!peer_need(m, 4u)) return false;
        uint8_t t[10];
        memset(t, 0, sizeof(t));
        t[0] = p[2];
        t[1] = s_peer.max_page;
        if (p[2] == 0u) memcpy(&t[2], s_peer.features, 8u);
        if (p[2] == 1u) memcpy(&t[2], s_peer.features_p1, 8u);
        peer_status1(0xE442u, peer_lc_src(), 0u);
        peer_s_h(0xE46Du, 0u, t, 10u);
        return true;
    }
    case 0xE407u: {                                                      /* 041D Read Remote Version Information */
        if (!peer_need(m, 2u)) return false;
        uint8_t t[6] = { s_peer.lmp_ver, 0u, (uint8_t)s_peer.manuf, (uint8_t)(s_peer.manuf >> 8),
                         (uint8_t)s_peer.subver, (uint8_t)(s_peer.subver >> 8) };
        peer_status1(0xE443u, peer_lc_src(), 0u);
        peer_s_h(0xE45Eu, 0u, t, 6u);
        return true;
    }
    case 0xE408u: {                                                      /* 041F Read Clock Offset (slave: CS first) */
        if (!peer_need(m, 2u)) return false;
        uint8_t t[2] = { (uint8_t)s_peer.clk_off, (uint8_t)(s_peer.clk_off >> 8) };
        peer_status1(0xE444u, peer_lc_src(), 0u);
        peer_s_h(0xE469u, 0u, t, 2u);
        return true;
    }
    case 0xE00Cu: {                                                      /* 0419 Remote Name Request */
        if (!peer_need(m, 10u)) return false;
        uint8_t r[255];
        memset(r, 0, sizeof(r));
        memcpy(&r[1], p, 6u);
        peer_status1(0xE0CBu, BT_ROM_TASK_LM, 0u);
        if (!peer_addr_is(p)) {
            r[0] = 0x04u;                                                /* Page Timeout: nobody answers */
        } else if (fake_peer.name_fail_n != 0u) {
            fake_peer.name_fail_n--;
            r[0] = 0x22u;                                                /* LMP Response Timeout: no name */
        } else {
            if (s_peer.host_feat_notify) {
                uint8_t h[14];
                memcpy(h, s_peer.addr, 6u);
                memcpy(&h[6], s_peer.features_p1, 8u);
                peer_emit(0xE47Cu, BT_ROM_TASK_LM, h, 14u);
            }
            memcpy(&r[7], s_peer.name, strnlen(s_peer.name, sizeof(s_peer.name)));
        }
        peer_emit(0xE0D3u, BT_ROM_TASK_LM, r, 255u);
        fake_peer.name_requests++;
        return true;
    }
    case 0xE00Du:                                                        /* 041A RNR Cancel: none running */
        if (!peer_need(m, 6u)) return false;
        peer_s_addr(0xE06Eu, BT_ROM_TASK_LM, 0x12u);
        return true;
    case 0xE402u:                                                        /* 0411 Authentication Requested */
        if (!peer_need(m, 2u)) return false;
        peer_status1(0xE43Eu, peer_lc_src(), 0u);
        fake_peer.origin = FAKE_PEER_ORIGIN_HOST;
        peer_addr_evt(0xE465u, peer_lc_src());                           /* Link Key Request */
        return true;
    case 0xE008u:                                                        /* 040B Link Key Request Reply */
        if (!peer_need(m, 22u) || !peer_addr_is(p)) return false;
        peer_s_addr(0xE06Au, BT_ROM_TASK_LM, 0u);
        fake_peer.host_key_replies++;
        memcpy(fake_peer.host_key, &p[6], 16u);
        if (fake_peer.has_key && memcmp(fake_peer.key, &p[6], 16u) == 0) {
            peer_auth_done(0u);
        } else if (fake_peer.origin == FAKE_PEER_ORIGIN_HOST) {
            peer_auth_done(0x06u);                                       /* PIN or Key Missing */
        } else {
            fake_peer.auth_fail++;
            peer_link_down(0x05u);                                       /* the phone drops a link that fails auth */
        }
        return true;
    case 0xE009u:                                                        /* 040C Link Key Request Negative Reply */
        if (!peer_need(m, 6u) || !peer_addr_is(p)) return false;
        peer_s_addr(0xE06Bu, BT_ROM_TASK_LM, 0u);
        fake_peer.host_key_neg++;
        if (fake_peer.origin != FAKE_PEER_ORIGIN_HOST) {
            /* responder: the initiator's LMP io_capability_req arrives first -> IO Capability Response, then
             * IO Capability Request (Core Vol 2 Part F, SSP IO capability exchange) */
            uint8_t io[9];
            memcpy(io, s_peer.addr, 6u);
            io[6] = s_peer.io_cap;
            io[7] = s_peer.oob;
            io[8] = s_peer.auth_req;
            peer_emit(0xE473u, peer_lc_src(), io, 9u);
        }
        peer_addr_evt(0xE472u, peer_lc_src());                           /* IO Capability Request */
        return true;
    case 0xE011u:                                                        /* 042B IO Capability Request Reply */
        if (!peer_need(m, 9u) || !peer_addr_is(p)) return false;
        peer_s_addr(0xE06Fu, BT_ROM_TASK_LM, 0u);
        fake_peer.host_io_replies++;
        fake_peer.host_io_cap = p[6];
        fake_peer.host_oob = p[7];
        fake_peer.host_auth_req = p[8];
        if (fake_peer.origin == FAKE_PEER_ORIGIN_HOST) {
            uint8_t io[9];
            memcpy(io, s_peer.addr, 6u);
            io[6] = s_peer.io_cap;
            io[7] = s_peer.oob;
            io[8] = s_peer.auth_req;
            peer_emit(0xE473u, peer_lc_src(), io, 9u);                   /* the peer's LMP io_capability_res */
        }
        {
            uint8_t uc[12];                                              /* {bdaddr, pad[2], u32 numeric} */
            memset(uc, 0, sizeof(uc));
            memcpy(uc, s_peer.addr, 6u);
            memcpy(&uc[8], &s_peer.numeric, 4u);
            peer_emit(0xE474u, peer_lc_src(), uc, 12u);
        }
        return true;
    case 0xE018u: {                                                      /* 0434 IO Capability Request Neg Reply */
        if (!peer_need(m, 7u) || !peer_addr_is(p)) return false;
        peer_s_addr(0xE076u, BT_ROM_TASK_LM, 0u);
        fake_peer.host_io_neg++;
        uint8_t sp[7];
        sp[0] = p[6];
        memcpy(&sp[1], s_peer.addr, 6u);
        peer_emit(0xE477u, peer_lc_src(), sp, 7u);                       /* Simple Pairing Complete (failed) */
        peer_auth_done(p[6]);
        return true;
    }
    case 0xE012u:                                                        /* 042C User Confirmation Request Reply */
    case 0xE013u: {                                                      /* 042D ... Negative Reply */
        if (!peer_need(m, 6u) || !peer_addr_is(p)) return false;
        bool yes = m->id == 0xE012u;
        peer_s_addr(yes ? 0xE070u : 0xE071u, BT_ROM_TASK_LM, 0u);
        if (yes) {
            fake_peer.host_confirms++;
        } else {
            fake_peer.host_rejects++;
        }
        uint8_t sp[7];
        sp[0] = (yes && s_peer.user_confirms) ? 0u : 0x05u;              /* Authentication Failure */
        memcpy(&sp[1], s_peer.addr, 6u);
        peer_emit(0xE477u, peer_lc_src(), sp, 7u);
        if (sp[0] == 0u) {
            uint8_t ln[23];                                              /* {bdaddr, key[16], type} */
            for (uint32_t i = 0u; i < 16u; i++) {
                fake_peer.key[i] = (uint8_t)((s_peer.numeric >> (8u * (i & 3u))) ^ s_peer.addr[i % 6u] ^ (i * 29u));
            }
            fake_peer.has_key = true;
            fake_peer.pairings++;
            memcpy(ln, s_peer.addr, 6u);
            memcpy(&ln[6], fake_peer.key, 16u);
            ln[22] = s_peer.key_type;
            peer_emit(0xE466u, peer_lc_src(), ln, 23u);
        }
        peer_auth_done(sp[0]);
        return true;
    }
    case 0xE403u: {                                                      /* 0413 Set Connection Encryption */
        if (!peer_need(m, 4u)) return false;
        uint8_t on = p[2];
        peer_status1(0xE43Fu, peer_lc_src(), 0u);
        uint8_t st = fake_peer.authenticated ? 0u : 0x06u;
        if (st == 0u) {
            fake_peer.encrypted = on != 0u;
        }
        peer_s_h(0xE45Au, st, &on, 1u);
        return true;
    }
    case 0xE400u:                                                        /* 0406 Disconnect */
        if (!peer_need(m, 4u)) return false;
        peer_status1(0xE43Au, peer_lc_src(), 0u);
        if (fake_peer.defer_detach) {
            fake_peer.detach_deferred++;                                 /* LMP detach pending until supervision TO */
            return true;
        }
        peer_link_down(0x16u);                                           /* Connection Terminated By Local Host */
        return true;
    default:
        return false;
    }
}

void fake_peer_init(const fake_peer_cfg_t *cfg)
{
    memset(&fake_peer, 0, sizeof(fake_peer));
    memset(&s_peer, 0, sizeof(s_peer));
    if (cfg != NULL) {
        s_peer = *cfg;
        fake_peer.enabled = true;
    }
}

void fake_peer_set_key(const uint8_t key[16])
{
    memcpy(fake_peer.key, key, 16u);
    fake_peer.has_key = true;
}

int fake_peer_page(uint32_t link)
{
    if (!fake_peer.enabled || link >= 7u || fake_peer.connected) {
        return -1;
    }
    fake_peer.link = link;
    uint8_t c[10];                                                       /* {bdaddr, cod[3], link_type} = wire */
    memcpy(c, s_peer.addr, 6u);
    memcpy(&c[6], s_peer.cod, 3u);
    c[9] = 1u;
    peer_emit(0xE457u, BT_ROM_TASK_LM, c, 10u);
    return 0;
}

void fake_peer_authenticate(void)
{
    fake_peer.origin = FAKE_PEER_ORIGIN_PEER;
    peer_addr_evt(0xE465u, peer_lc_src());
}

void fake_peer_encrypt(bool on)
{
    uint8_t v = on ? 1u : 0u;
    uint8_t st = fake_peer.authenticated ? 0u : 0x06u;
    if (st == 0u) {
        fake_peer.encrypted = on;
    }
    peer_s_h(0xE45Au, st, &v, 1u);
}

void fake_peer_disconnect(uint8_t reason)
{
    if (fake_peer.connected) {
        peer_link_down(reason);
    }
}

int fake_peer_page_again(uint32_t link)
{
    if (!fake_peer.enabled || !fake_peer.connected || link >= 7u || link == fake_peer.link || f.links[link] ||
        fake_peer.ghost_pending || fake_peer.ghost_up) {
        return -1;
    }
    fake_peer.ghost_link = link;
    fake_peer.ghost_pending = true;
    uint8_t c[10];
    memcpy(c, s_peer.addr, 6u);
    memcpy(&c[6], s_peer.cod, 3u);
    c[9] = 1u;
    peer_emit(0xE457u, BT_ROM_TASK_LM, c, 10u);
    return 0;
}

void fake_peer_ghost_end(uint8_t status)
{
    if (!fake_peer.ghost_up) {
        return;
    }
    uint8_t c[13];
    memset(c, 0, sizeof(c));
    c[0] = status;
    memcpy(&c[5], s_peer.addr, 6u);
    c[11] = 1u;
    peer_emit(0xE0D2u, BT_ROM_TASK_LM, c, 13u);
    f.links[fake_peer.ghost_link] = false;
    fake_peer.ghost_up = false;
}

int fake_peer_send_acl(const uint8_t *data, uint16_t len)
{
    return fake_peer.connected ? fake_rom_inject_acl(fake_peer.handle, 0x2u, data, len) : -1;
}

/* ---- fake LM / LC ---- */
static reply_t *find_reply(uint16_t req)
{
    for (uint32_t i = 0u; i < 64u; i++) {
        if (f.replies[i].used && f.replies[i].req == req) {
            return &f.replies[i];
        }
    }
    return NULL;
}

static void nocp_send(const uint16_t *handles, const uint16_t *counts, uint32_t n)
{
    uint8_t p[2u + 4u * 7u];
    memset(p, 0, sizeof(p));
    p[0] = (uint8_t)n;
    for (uint32_t i = 0u; i < n; i++) {
        memcpy(&p[2u + 4u * i], &handles[i], 2u);
        memcpy(&p[4u + 4u * i], &counts[i], 2u);
    }
    send_internal(BT_ROM_MSG_NOCP, BT_ROM_TASK_HCI, BT_ROM_TASK_LC, p, (uint16_t)(2u + 4u * n));
}

static void lmlc_handle(msg_t *m)
{
    if (m->src != BT_ROM_TASK_HCI) {
        err("command 0x%04X with src 0x%X", m->id, m->src);
    }
    if (m->id == BT_ROM_MSG_ACL_TX) {
        bt_rom_acl_desc_t d;
        if (m->len < sizeof(d)) {
            err("short ACL_TX descriptor");
            return;
        }
        memcpy(&d, m->block + MSG_HDR, sizeof(d));
        if (!em_range_ok(d.buf_ptr, d.length)) {
            err("ACL_TX buf_ptr not an allocated EM TX buffer");
            return;
        }
        if (fake_rom.acl != NULL && fake_rom.n_acl < FAKE_ROM_LOG_MAX) {
            fake_acl_rec_t *r = &fake_rom.acl[fake_rom.n_acl++];
            r->handle = d.conhdl;
            r->len = d.length;
            r->pb_bc = d.pb_bc_flag;
            memcpy(r->data, d.buf_ptr, d.length);
        }
        if (f.air_n >= BT_ROM_EM_TX_BUFS) {
            err("more ACL in air than EM TX buffers");
            return;
        }
        f.air[f.air_n].handle = d.conhdl;
        f.air[f.air_n].em = d.buf_ptr;
        f.air_n++;
        fake_rom.acl_in_air = f.air_n;
        if (fake_rom.auto_ack_acl) {
            (void)fake_rom_ack_acl(1u);
        }
        return;
    }
    if (fake_rom.lc_swallow && (m->dst & 0xFFu) == BT_ROM_TASK_LC) {
        return;                              /* message discarded by a freed LC instance: no reply, ever */
    }
    if (fake_rom.n_cmds < 64u) {
        fake_cmd_rec_t *r = &fake_rom.cmds[fake_rom.n_cmds++];
        r->id = m->id;
        r->dst = m->dst;
        r->len = m->len;
        memcpy(r->data, m->block + MSG_HDR, m->len < 256u ? m->len : 256u);
    }
    if (fake_rom.hold_replies) {
        if (f.held_n < MAX_MSGS) {
            f.held[f.held_n++] = ((uint32_t)m->id << 16) | m->len;   /* address echo comes from the cmds log */
        }
        return;
    }
    if (peer_handle(m)) {
        return;                              /* scripted remote peer answered with the ROM's own messages */
    }
    reply_t *r = find_reply(m->id);
    if (r == NULL) {
        return;                              /* the ROM handles it without a reply */
    }
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    p[0] = r->status;
    if (m->id == 0xE02Cu && r->status == 0u && m->len >= 1u) {
        /* 0C1A Write Scan Enable, ROM handler 0x3EAC4 (docs/BT_ROM_HCI_MAP.md §11): a value with page scan (2, 3) is
         * refused with 0x0C while more than one ACL link is up (LD link count, ROM 0x368FC); > 3 is 0x12. */
        uint8_t scan = m->block[MSG_HDR];
        uint32_t links = 0u;
        for (uint32_t i = 0u; i < 7u; i++) {
            links += f.links[i] ? 1u : 0u;
        }
        p[0] = scan > 3u ? 0x12u : ((scan & 0x02u) != 0u && links > 1u) ? 0x0Cu : 0x00u;
        fake_rom.scan_refused += p[0] == 0x0Cu;
        fake_rom.scan_writes++;
        if (p[0] == 0x00u) {
            fake_rom.scan_enable = scan;
        }
    }
    if (r->echo_addr) {
        memcpy(&p[1], m->block + MSG_HDR, m->len >= 6u ? 6u : m->len);
    }
    send_internal(r->rsp, m->src, (uint16_t)(m->dst & 0xFFu), p, r->ret_len);
}

void fake_rom_release_held(void)
{
    uint32_t n = f.held_n;
    f.held_n = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        uint16_t id = (uint16_t)(f.held[i] >> 16);
        reply_t *r = find_reply(id);
        if (r == NULL) {
            continue;
        }
        uint8_t p[64];
        memset(p, 0, sizeof(p));
        p[0] = r->status;
        if (r->echo_addr) {
            for (uint32_t k = fake_rom.n_cmds; k-- > 0u;) {
                if (fake_rom.cmds[k].id == id) {
                    memcpy(&p[1], fake_rom.cmds[k].data, 6u);
                    break;
                }
            }
        }
        send_internal(r->rsp, BT_ROM_TASK_HCI, BT_ROM_TASK_LM, p, r->ret_len);
    }
}

static void dispatch_one(void)
{
    uint32_t i = f.q[f.q_head];
    f.q_head = (f.q_head + 1u) % MAX_MSGS;
    f.q_n--;
    msg_t *m = &f.msgs[i];
    uint8_t task = (uint8_t)(m->dst & 0xFFu);
    if (task == BT_ROM_TASK_HCI) {
        uint8_t *param = m->block + MSG_HDR;
        bt_rom_acl_desc_t d;
        bool is_acl = m->id == BT_ROM_MSG_ACL_RX && m->len >= sizeof(d);
        if (is_acl) {
            memcpy(&d, param, sizeof(d));
        }
        bt_rom_msg_ret_t r = f.up.on_hci_msg(f.up.ctx, m->id, param, m->len, m->src);
        if (r == BT_ROM_MSG_CONSUMED) {
            if (is_acl && d.buf_ptr != NULL && em_range_ok(d.buf_ptr, 0u)) {
                err("ACL_RX consumed without em_rx_free (EM leak)");
            }
            free_msg(m);
        } else {
            m->state = M_KEPT;
        }
        return;
    }
    if (task == BT_ROM_TASK_LM || (task == BT_ROM_TASK_LC && (m->dst >> 8) < 7u)) {
        lmlc_handle(m);
        free_msg(m);
        return;
    }
    err("message 0x%04X to unregistered task 0x%04X (ROM udf)", m->id, m->dst);
    free_msg(m);
}

/* ---- bt_rom_if_t ---- */
static uint32_t i_rf_calibrate(void)
{
    uint32_t c = enter(FR_RF_CAL);
    f.rf_cal_since_stop = true;
    leave(c);
    return 0x53u;
}

static void drop_all(void)
{
    for (uint32_t i = 0u; i < MAX_MSGS; i++) {
        if (f.msgs[i].state != M_FREE) {
            free_msg(&f.msgs[i]);
        }
    }
    f.q_head = f.q_n = 0u;
    f.timer_ev = 0u;
    f.pending_irq = 0u;
    memset(f.tx_used, 0, sizeof(f.tx_used));
    memset(f.rx_used, 0, sizeof(f.rx_used));
    f.air_n = 0u;
    fake_rom.acl_in_air = 0u;
    f.held_n = 0u;
    fake_rom.heap_used = 0u;
}

static int i_start(const uint8_t bd_addr[6], const bt_rom_upcalls_t *up)
{
    uint32_t c = enter(FR_START);
    (void)bd_addr;
    if (!f.rf_cal_since_stop) {
        err("start() without RF calibration first (EM wipe would destroy it)");
    }
    if (f.running) {
        err("start() while running");
    }
    drop_all();                              /* EM wipe + bt_heap_init */
    memset(f.em_tx, 0, sizeof(f.em_tx));
    memset(f.em_rx, 0, sizeof(f.em_rx));
    f.up = *up;
    fake_rom.starts++;
    int r = fake_rom.start_result;
    f.running = r == 0;
    leave(c);
    return r;
}

static int i_stop(void)
{
    uint32_t c = enter(FR_STOP);
    fake_rom.kept_at_stop = fake_rom_msgs_owned_by_ctrl();
    uint32_t rx = 0u;
    for (uint32_t i = 0u; i < BT_ROM_EM_RX_BUFS; i++) {
        rx += f.rx_used[i];
    }
    /* EM RX buffers still referenced by queued (undelivered) ACL_RX belong to the ROM, not bt_ctrl. */
    for (uint32_t k = 0u; k < f.q_n; k++) {
        msg_t *m = &f.msgs[f.q[(f.q_head + k) % MAX_MSGS]];
        if (m->id == BT_ROM_MSG_ACL_RX) {
            rx--;
        }
    }
    fake_rom.em_rx_held_at_stop = rx;
    fake_rom.stops++;
    if (fake_rom.fault_on_stop && f.up.on_fault != NULL) {
        f.up.on_fault(f.up.ctx, 2u);
    }
    f.stale_up = f.up;                       /* upcalls of the incarnation being stopped */
    f.have_stale = true;
    if (fake_peer.enabled && fake_peer.connected) {
        /* The controller is gone: the scripted peer's link dies with it (the peer sees a supervision timeout). */
        f.links[fake_peer.link] = false;
        fake_peer.connected = fake_peer.encrypted = fake_peer.authenticated = false;
        fake_peer.origin = FAKE_PEER_ORIGIN_NONE;
        fake_peer.last_disc_reason = 0x08u;
        fake_peer.stop_kills++;
    }
    f.running = false;
    f.rf_cal_since_stop = false;
    drop_all();
    leave(c);
    return fake_rom.stop_result;
}

static void i_irq_enable(int on)
{
    uint32_t c = enter(FR_IRQ_EN);
    fake_rom.irq_enabled = on ? 1u : 0u;
    leave(c);
}

static uint32_t i_irq_take(void)
{
    uint32_t c = enter(FR_IRQ_TAKE);
    uint32_t v = f.pending_irq;
    f.pending_irq = 0u;
    leave(c);
    return v;
}

static uint32_t i_irq_service(uint32_t bits)
{
    uint32_t c = enter(FR_IRQ_SERVICE);
    if (bits & BT_ROM_IRQ_KE_TIMER) {
        f.timer_ev = 1u;
    }
    if (fake_rom.irq_rearm != 0u) {
        fake_rom.irq_rearm--;
        fake_rom_raise_irq(bits & BT_ROM_IRQ_SERVICED);   /* interrupt taken while the bottom half ran */
    }
    leave(c);
    return bits & ~BT_ROM_IRQ_SERVICED;
}

static uint32_t events_now(void)
{
    return (f.q_n != 0u ? 1u : 0u) | (f.timer_ev ? 2u : 0u);
}

static uint32_t i_events(void)
{
    uint32_t c = enter(FR_EVENTS);
    uint32_t e = events_now();
    leave(c);
    return e;
}

static uint32_t i_run(uint32_t max_steps)
{
    uint32_t c = enter(FR_RUN);
    for (uint32_t s = 0u; s < max_steps; s++) {
        if (f.timer_ev) {
            f.timer_ev = 0u;
        } else if (f.q_n != 0u) {
            dispatch_one();
        } else {
            break;
        }
    }
    uint32_t e = events_now();
    leave(c);
    return e;
}

static uint8_t *i_msg_alloc(uint16_t id, uint16_t dst, uint16_t src, uint16_t len)
{
    uint32_t c = enter(FR_MSG_ALLOC);
    if (!f.running) {
        err("msg_alloc while stopped");
    }
    uint8_t *p = alloc_msg(id, dst, src, len, false);
    leave(c);
    return p;
}

static void i_msg_send(uint8_t *param)
{
    uint32_t c = enter(FR_MSG_SEND);
    msg_t *m = find_param(param);
    if (m == NULL || m->state != M_ALLOC) {
        err("msg_send on a pointer that is not an unsent param");
    } else {
        queue_msg(m);
    }
    leave(c);
}

static void i_msg_free(uint8_t *param)
{
    uint32_t c = enter(FR_MSG_FREE);
    msg_t *m = find_param(param);
    if (m == NULL || (m->state != M_ALLOC && m->state != M_KEPT)) {
        err("msg_free on a pointer that is not an allocated/kept param");
    } else {
        free_msg(m);
    }
    leave(c);
}

static uint16_t i_conhdl_to_task(uint16_t h)
{
    uint32_t c = enter(FR_CONHDL_TASK);
    uint16_t t = BT_ROM_TASK_INVALID;
    if (h >= 0x80u && h <= 0x86u && f.links[h - 0x80u]) {
        t = (uint16_t)(((h - 0x80u) << 8) | BT_ROM_TASK_LC);
    }
    leave(c);
    return t;
}

static uint8_t i_conhdl_type(uint16_t h)
{
    uint32_t c = enter(FR_CONHDL_TYPE);
    uint8_t t = (h >= 0x80u && h <= 0x86u) ? 1u : 0u;
    if (fake_rom.conhdl_type_override >= 0) {
        t = (uint8_t)fake_rom.conhdl_type_override;
    }
    leave(c);
    return t;
}

static uint8_t *i_em_tx_alloc(void)
{
    uint32_t c = enter(FR_EM_TX_ALLOC);
    uint8_t *p = NULL;
    for (uint32_t i = 0u; !fake_rom.em_tx_fail && i < BT_ROM_EM_TX_BUFS; i++) {
        if (!f.tx_used[i]) {
            f.tx_used[i] = 1u;
            p = f.em_tx[i] + EM_HDR;
            break;
        }
    }
    leave(c);
    return p;
}

static void i_em_write(uint8_t *em, const uint8_t *src, uint16_t len)
{
    uint32_t c = enter(FR_EM_WRITE);
    if (!em_range_ok(em, len)) {
        err("em_write outside an allocated EM buffer");
    } else {
        memcpy(em, src, len);
    }
    leave(c);
}

static void i_em_read(uint8_t *dst, const uint8_t *em, uint16_t len)
{
    uint32_t c = enter(FR_EM_READ);
    if (!em_range_ok(em, len)) {
        err("em_read outside an allocated EM buffer");
        memset(dst, 0, len);
    } else {
        memcpy(dst, em, len);
    }
    leave(c);
}

static void i_em_rx_free(uint8_t *em)
{
    uint32_t c = enter(FR_EM_RX_FREE);
    bool tx;
    uint32_t idx, off;
    if (em_locate(em, &tx, &idx, &off) != 0 || off != 0u) {
        err("em_rx_free on a non-EM pointer");
    } else if (tx ? !f.tx_used[idx] : !f.rx_used[idx]) {
        err("EM double free");
    } else if (tx) {
        f.tx_used[idx] = 0u;
    } else {
        f.rx_used[idx] = 0u;
    }
    leave(c);
}

static uint32_t i_lock_depth(void)
{
    return fake_rom.lock_depth;
}

static void i_stats(bt_rom_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->irq_count, fake_rom.irq_count, sizeof(out->irq_count));
    out->heap_free = fake_rom.heap_budget - fake_rom.heap_used;
    out->alloc_fail = fake_rom.alloc_refused;
}

const bt_rom_if_t fake_rom_if = {
    i_rf_calibrate, i_start, i_stop, i_irq_enable, i_irq_take, i_irq_service, i_events, i_run,
    i_msg_alloc, i_msg_send, i_msg_free, i_conhdl_to_task, i_conhdl_type,
    i_em_tx_alloc, i_em_write, i_em_read, i_em_rx_free, i_lock_depth, i_stats,
};

/* ---- test API ---- */
void fake_rom_set_reply(uint16_t req_id, uint16_t rsp_id, uint8_t status, uint8_t ret_len, bool echo_addr)
{
    reply_t *r = find_reply(req_id);
    for (uint32_t i = 0u; r == NULL && i < 64u; i++) {
        if (!f.replies[i].used) {
            r = &f.replies[i];
        }
    }
    if (r == NULL) {
        err("reply table full");
        return;
    }
    r->req = req_id;
    r->rsp = rsp_id;
    r->status = status;
    r->ret_len = ret_len;
    r->echo_addr = echo_addr ? 1u : 0u;
    r->used = 1u;
}

void fake_rom_set_reply_status(uint16_t req_id, uint8_t status)
{
    reply_t *r = find_reply(req_id);
    if (r != NULL) {
        r->status = status;
    }
}

void fake_rom_reset(void)
{
    drop_all();
    fake_acl_rec_t *acl = fake_rom.acl;
    memset(&f, 0, sizeof(f));
    memset(&fake_rom, 0, sizeof(fake_rom));
    fake_rom.acl = acl != NULL ? acl : calloc(FAKE_ROM_LOG_MAX, sizeof(fake_acl_rec_t));
    fake_rom.heap_budget = 8192u;
    fake_rom.auto_ack_acl = true;
    fake_rom.conhdl_type_override = -1;
    /* Verified legacy reply paths (bt_hci.c:742-761, BT_CTRL_OPCODE_TABLE V-x rows), aligned layouts. */
    fake_rom_set_reply(0xE01Cu, 0xE079u, 0u, 1u, false);   /* 0C01 */
    fake_rom_set_reply(0xE025u, 0xE083u, 0u, 1u, false);   /* 0C13 */
    fake_rom_set_reply(0xE02Cu, 0xE08Au, 0u, 1u, false);   /* 0C1A */
    fake_rom_set_reply(0xE034u, 0xE094u, 0u, 1u, false);   /* 0C24 */
    fake_rom_set_reply(0xE04Fu, 0xE0B1u, 0u, 1u, false);   /* 0C56 */
    fake_rom_set_reply(0xE006u, 0xE0C9u, 0u, 1u, false);   /* 0409 CS */
    fake_rom_set_reply(0xE007u, 0xE0CAu, 0u, 1u, false);   /* 040A CS */
    fake_rom_set_reply(0xE400u, 0xE43Au, 0u, 1u, false);   /* 0406 CS (LC) */
    fake_rom_set_reply(0xE008u, 0xE06Au, 0u, 7u, true);    /* 040B */
    fake_rom_set_reply(0xE009u, 0xE06Bu, 0u, 7u, true);
    fake_rom_set_reply(0xE00Au, 0xE06Cu, 0u, 7u, true);
    fake_rom_set_reply(0xE00Bu, 0xE06Du, 0u, 7u, true);
    fake_rom_set_reply(0xE011u, 0xE06Fu, 0u, 7u, true);    /* 042B */
    fake_rom_set_reply(0xE012u, 0xE070u, 0u, 7u, true);
    fake_rom_set_reply(0xE013u, 0xE071u, 0u, 7u, true);
    /* Phone-path rows promoted to FWD from docs/BT_ROM_HCI_MAP.md §2 (initial replies only; the scripted peer below
     * adds the terminal events). */
    fake_rom_set_reply(0xE00Cu, 0xE0CBu, 0u, 1u, false);   /* 0419 CS (LM) */
    fake_rom_set_reply(0xE00Du, 0xE06Eu, 0u, 7u, true);    /* 041A CC {s, bdaddr} */
    fake_rom_set_reply(0xE018u, 0xE076u, 0u, 7u, true);    /* 0434 CC {s, bdaddr} */
    fake_rom_set_reply(0xE402u, 0xE43Eu, 0u, 1u, false);   /* 0411 CS (LC) */
    fake_rom_set_reply(0xE403u, 0xE43Fu, 0u, 1u, false);   /* 0413 CS (LC) */
    fake_rom_set_reply(0xE405u, 0xE441u, 0u, 1u, false);   /* 041B CS (LC) */
    fake_rom_set_reply(0xE406u, 0xE442u, 0u, 1u, false);   /* 041C CS (LC) */
    fake_rom_set_reply(0xE407u, 0xE443u, 0u, 1u, false);   /* 041D CS (LC) */
    fake_rom_set_reply(0xE408u, 0xE444u, 0u, 1u, false);   /* 041F CS (LC) */
    memset(&fake_peer, 0, sizeof(fake_peer));
    memset(&s_peer, 0, sizeof(s_peer));
}

void fake_rom_shutdown(void)
{
    drop_all();
    free(fake_rom.acl);
    fake_rom.acl = NULL;
}

void fake_rom_link(uint32_t link, bool up)
{
    if (link < 7u) {
        f.links[link] = up;
    }
}

int fake_rom_inject(uint16_t id, const uint8_t *param, uint16_t len)
{
    uint8_t *p = alloc_msg(id, BT_ROM_TASK_HCI, BT_ROM_TASK_LC, len, true);
    if (p == NULL) {
        return -1;
    }
    if (len != 0u && param != NULL) {
        memcpy(p, param, len);
    }
    queue_msg(find_param(p));
    return 0;
}

int fake_rom_inject_acl(uint16_t handle, uint8_t pb_bc, const uint8_t *data, uint16_t len)
{
    if (len > BT_ROM_EM_BUF_PAYLOAD) {
        return -1;
    }
    for (uint32_t i = 0u; i < BT_ROM_EM_RX_BUFS; i++) {
        if (!f.rx_used[i]) {
            f.rx_used[i] = 1u;
            uint8_t *em = f.em_rx[i] + EM_HDR;
            if (len != 0u) {
                memcpy(em, data, len);
            }
            bt_rom_acl_desc_t d;
            memset(&d, 0, sizeof(d));
            d.conhdl = handle;
            d.pb_bc_flag = pb_bc;
            d.length = len;
            d.buf_ptr = em;
            if (fake_rom_inject(BT_ROM_MSG_ACL_RX, (const uint8_t *)&d, (uint16_t)sizeof(d)) != 0) {
                f.rx_used[i] = 0u;
                return -1;
            }
            return 0;
        }
    }
    return -1;                               /* all EM RX buffers held: the baseband would flow-stop or drop */
}

void fake_rom_raise_irq(uint32_t bits)
{
    for (uint32_t b = 0u; b < 32u; b++) {
        if (bits & (1u << b)) {
            fake_rom.irq_count[b]++;
        }
    }
    f.pending_irq |= bits;
    if (fake_rom.irq_enabled && f.up.on_irq != NULL) {
        f.up.on_irq(f.up.ctx);
    }
}

uint32_t fake_rom_ack_acl(uint32_t n)
{
    uint16_t hs[7], cs[7];
    uint32_t nh = 0u, done = 0u;
    while (done < n && f.air_n != 0u) {
        air_t a = f.air[0];
        memmove(&f.air[0], &f.air[1], (f.air_n - 1u) * sizeof(air_t));
        f.air_n--;
        bool tx;
        uint32_t idx, off;
        if (em_locate(a.em, &tx, &idx, &off) == 0 && tx) {
            f.tx_used[idx] = 0u;             /* baseband acknowledged: the ROM frees the EM TX buffer */
        }
        uint32_t k;
        for (k = 0u; k < nh && hs[k] != a.handle; k++) {
        }
        if (k == nh) {
            hs[nh] = a.handle;
            cs[nh] = 0u;
            nh++;
        }
        cs[k]++;
        done++;
    }
    fake_rom.acl_in_air = f.air_n;
    if (nh != 0u) {
        nocp_send(hs, cs, nh);
    }
    return done;
}

int fake_rom_deliver_stale(uint16_t id, const uint8_t *param, uint16_t len)
{
    if (!f.have_stale) {
        return -1;
    }
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, param, len < sizeof(buf) ? len : sizeof(buf));
    return (int)f.stale_up.on_hci_msg(f.stale_up.ctx, id, buf, len, BT_ROM_TASK_LM);
}

uint32_t fake_rom_em_rx_in_use(void)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < BT_ROM_EM_RX_BUFS; i++) {
        n += f.rx_used[i];
    }
    return n;
}

uint32_t fake_rom_em_tx_in_use(void)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < BT_ROM_EM_TX_BUFS; i++) {
        n += f.tx_used[i];
    }
    return n;
}

uint32_t fake_rom_msgs_owned_by_ctrl(void)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < MAX_MSGS; i++) {
        n += (f.msgs[i].state == M_ALLOC && !f.msgs[i].internal) || f.msgs[i].state == M_KEPT;
    }
    return n;
}

uint32_t fake_rom_queue_len(void)
{
    return f.q_n;
}
