/* bt_ctrl: see bt_ctrl.h.
 *
 * Data flow
 *   hciT  send()  -> unified TX ring (3 x 1026, any H4 type) -> bt_ctrl tx_drain(): not strict FIFO -- the oldest
 *                    eligible entry wins; ordinary commands need a ROM command credit and a reserved reply slot, ACL
 *                    needs a controller ACL credit (1005 count minus packets the ROM has not completed); order is kept
 *                    within ordinary commands and within ACL; 0C35 (NOACK) is always eligible and handled locally.
 *   ROM   TASK_HCI upcall (inside rom->run()) -> copy into the event ring (copy-before-ack, then CONSUMED), or KEEP:
 *                    ACL_RX always (held until a host credit exists), general events while their ring area is full.
 *         rx_dispatch(): pending NOCP, deferred events, then held ACL -- the ACL branch checks the host credit and ring
 *                    space BEFORE touching any fragment state (plan M1 RX dispatcher order). Deferred events are
 *                    also retried before every kernel run, so a freed slot goes to the oldest deferred event.
 *   bt_dlv dlv_run(): send-available flag, then the event and ACL rings in arrival order: every entry carries one
 *                    sequence number shared by both rings (ROM messages: taken at the upcall; local replies / NOCP:
 *                    taken when queued). The ACL head goes first only if no queued event is older, and held ACL
 *                    does not enter its ring while an older event is still deferred. Events are still never held
 *                    behind credit-gated ACL: that gate is applied by rx_dispatch() before a fragment enters its ring,
 *                    so only ACL the host already has a credit for can precede an event. This keeps ACL the ROM
 *                    delivered before a Disconnection Complete ahead of it (a real controller's wire order).
 * SPDX-License-Identifier: MIT */
#include "bt_ctrl.h"
#include "bt_ctrl_evt.h"
#include <stddef.h>
#include <string.h>

/* ---- byte-capacity asserts (plan M1 "a static assert ties the ring byte capacity to these values") ---- */
_Static_assert(BT_CTRL_TX_SLOT_BYTES >= 1u + 4u + BT_CTRL_H2C_ACL_LEN, "TX slot must hold a full H2C ACL packet");
_Static_assert(BT_CTRL_TX_SLOT_BYTES >= 1u + 3u + 255u, "TX slot must hold the largest HCI command");
_Static_assert(BT_CTRL_EVT_SLOT_BYTES >= BT_CTRL_EVT_MAX_H4, "event slot must hold 1 + 2 + 255 bytes");
_Static_assert(BT_CTRL_ACL_RX_SLOTS * BT_CTRL_ACL_RX_SLOT_BYTES == BT_CTRL_C2H_ACL_CREDITS * (BT_CTRL_C2H_ACL_LEN + 5u),
               "ACL RX ring bytes must equal the 4 x 1021 controller-to-host window plus H4/ACL headers");
_Static_assert(BT_CTRL_ACL_RX_SLOTS >= BT_CTRL_C2H_ACL_CREDITS, "every host credit needs a ring slot");
_Static_assert(BT_CTRL_C2H_ACL_LEN <= BT_ROM_EM_BUF_PAYLOAD, "C2H fragments come from one EM RX buffer");
_Static_assert(BT_CTRL_H2C_ACL_LEN <= BT_ROM_EM_BUF_PAYLOAD, "an H2C ACL packet must fit one EM TX buffer");
_Static_assert(BT_CTRL_H2C_ACL_CREDITS <= BT_ROM_EM_TX_BUFS,
               "1005 must not advertise more ACL buffers than the ROM can take immediately (plan M1)");
_Static_assert(BT_CTRL_HELD_ACL >= BT_ROM_EM_RX_BUFS, "every EM RX buffer may be held while credit-blocked");
_Static_assert(BT_CTRL_EVT_RESERVED >= 2u && BT_CTRL_EVT_RESERVED < BT_CTRL_EVT_SLOTS, "reserved CC/CS + NOCP slots");
_Static_assert(BT_CTRL_ROM_CMD_CREDITS == 1u, "one forwarded command in flight: fwd_* state holds a single entry");
#if defined(L2CAP_HOST_FC_ACL_BUFS)
_Static_assert(L2CAP_HOST_FC_ACL_BUFS == BT_CTRL_C2H_ACL_CREDITS, "0C33 window (bt_port_prefix.h) != bt_ctrl ring");
#endif
#if defined(L2CAP_HOST_FC_ACL_DATA_LEN)
_Static_assert(L2CAP_HOST_FC_ACL_DATA_LEN == BT_CTRL_C2H_ACL_LEN, "0C33 ACL length (bt_port_prefix.h) != bt_ctrl ring");
#endif

enum { ST_OFF = 0, ST_OPEN, ST_STOPPING };
enum { SLOT_FREE = 0, SLOT_FILLING, SLOT_READY };

typedef struct {
    uint8_t  state;
    uint32_t seq;
    uint16_t len;
    uint8_t  data[BT_CTRL_TX_SLOT_BYTES];
} tx_slot_t;

typedef struct {
    uint16_t len;
    uint32_t seq;                 /* delivery order across both RX rings */
    uint8_t  data[BT_CTRL_EVT_SLOT_BYTES];
} evt_slot_t;

typedef struct {
    uint16_t len;
    uint32_t epoch;
    uint32_t seq;
    uint8_t  data[BT_CTRL_ACL_RX_SLOT_BYTES];
} acl_slot_t;

typedef struct {
    uint8_t *param;
    uint8_t *em;
    uint32_t seq;                 /* taken at the ACL_RX upcall; every fragment carries it */
    uint16_t handle, len, off;
    uint8_t  pb, bc;
} held_acl_t;

typedef struct {
    uint8_t *param;
    uint32_t seq;
    uint16_t id, len;
} deferred_t;

typedef struct {
    uint16_t handle;
    uint16_t count;
} hcount_t;

static struct {
    const bt_rom_if_t *rom;
    bt_ctrl_platform_t p;
    const bt_ctrl_host_cb_t *host;
    bt_rom_upcalls_t up;
    uint8_t bd_addr[6];
    uint8_t inited, state, admission_open, dlv_open, rom_running, parked, stop_req;
    int stop_result;
    uint32_t session, rom_gen;
    uint32_t pending_rom_fault;
    uint32_t pending_site, pending_a, pending_b;   /* BT_CTRL_SITE_* + key values of pending_rom_fault */
    uint8_t need_dlv, upcall_activity;

    /* unified TX ring */
    tx_slot_t tx[BT_CTRL_TX_SLOTS];
    uint32_t tx_seq;
    uint8_t send_avail_pending;
    uint8_t acl_blocked;          /* ACL head could not be handed to the ROM in this pass */

    /* commands */
    uint8_t rom_cmd_credits, fwd_pending, fwd_cls, fwd_plen;
    uint8_t fwd_lc;               /* the forwarded command went to the per-link LC task of fwd_handle */
    uint8_t fwd_orphan;           /* ...and that link's Disconnection Complete arrived while it was pending */
    uint16_t fwd_opcode, fwd_handle;
    uint16_t fwd_late_op;         /* opcode answered locally at its deadline; the ROM's late reply is consumed once */
    uint8_t fwd_late_lc;          /* ...it went to the LC task of fwd_late_handle (the ROM drops it with that link) */
    uint8_t fwd_late_counted;     /* the command now held for the current late opcode was counted, hold_t0 taken */
    uint8_t hold_valid;           /* the next forwarded command was held: its deadline runs from hold_t0 */
    uint32_t hold_t0;             /* now_ms() when that command was first held (~ when the host sent it) */
    uint16_t fwd_late_handle;
    uint32_t fwd_late_t0;         /* now_ms() of that deadline: a new command with the same opcode waits until the late
                                     reply was consumed, the link went, or BT_CTRL_FWD_DEADLINE_MS passed again */
    uint32_t fwd_t0;              /* now_ms() when the pending command was forwarded */
    uint8_t fwd_params[255];
    uint8_t dead;                 /* handles 0x80..0x86 whose Disconnection Complete came after their last Connection
                                     Complete: LC commands and ACL for them are answered / dropped locally */
    uint32_t (*now_ms)(void);     /* optional clock for the forwarded-command deadline (bt_ctrl_set_clock) */

    /* event ring (SPSC: bt_ctrl produces, bt_dlv consumes) */
    evt_slot_t evt[BT_CTRL_EVT_SLOTS];
    uint32_t evt_head, evt_tail, evt_resv;
    uint32_t rx_seq;              /* next RX delivery sequence number (bt_ctrl context only) */
    deferred_t def[BT_CTRL_EVT_DEFER];
    uint32_t def_head, def_n;

    /* ACL RX ring (SPSC) + held ROM ACL_RX messages */
    acl_slot_t acl[BT_CTRL_ACL_RX_SLOTS];
    uint32_t acl_head, acl_tail, acl_epoch;
    held_acl_t held[BT_CTRL_HELD_ACL];
    uint32_t held_head, held_n;
    uint8_t acl_min_valid;        /* bit i: acl_min_seq[i] set once bt_dlv hands DISC / CONN_CMP to the host */
    uint32_t acl_min_seq[BT_CTRL_MAX_LINKS];

    /* host -> controller ACL accounting (packets handed to the ROM, not yet completed) */
    hcount_t inflight[BT_CTRL_MAX_LINKS];
    uint32_t inflight_total;
    hcount_t nocp_pend[BT_CTRL_MAX_LINKS];

    /* controller -> host window (0C31 / 0C33 / 0C35) */
    uint8_t c2h_fc;
    uint16_t c2h_len;
    uint32_t c2h_credits;
    hcount_t c2h_out[BT_CTRL_MAX_LINKS];
    uint32_t c2h_out_total;

    uint8_t local_name[BT_CTRL_NAME_LEN];
    uint8_t ssp_host;
    uint32_t dlv_fails;
    uint8_t dlv_faulted, dlv_busy, fault_raised;
    uint8_t scratch[BT_CTRL_EVT_MAX_H4];
    bt_ctrl_note_t notes[BT_CTRL_NOTES];     /* link notes: bt_ctrl context adds, any task pops (both under enter()) */
    uint32_t note_head, note_n;
    uint8_t peer_name_ok, peer_bda[6];       /* newest successful remote name (under enter(), bt_ctrl_peer_name) */
    char peer_name[BT_CTRL_PEER_NAME_MAX];
    bt_ctrl_stats_t st;
} g;

/* Link view and inquiry/page state for the headphones path (bt_ctrl_link_info, bt_ctrl_scan_enable,
 * bt_ctrl_inquiry_active). On target it lives in SRAM_HI (.bss_hi, zeroed at boot, like bt_xrec_fw.c's recovery
 * objects): `g` is in .bt_bss, which comes out of the RTOS_HEAP region, and the BT=1 HCI_PROBE self-test image had
 * only ~28 B left above the 32 KiB heap_5 floor asserted by sl6801_firm.ld. Reset with `g` (bt_ctrl_init) and at
 * every controller reset. */
#if defined(__arm__)
#define BT_CTRL_HI __attribute__((section(".bss_hi"), aligned(4)))
#else
#define BT_CTRL_HI
#endif
static struct {
    bt_ctrl_link_info_t link[BT_CTRL_MAX_LINKS];   /* written by bt_ctrl context and read by any task, under enter() */
    uint8_t page_pending, page_bda[6];       /* 0405 accepted by the ROM (CS 0), Connection Complete still to come */
    uint16_t page_pkt_type;                  /* that 0405's Packet_Type and Allow_Role_Switch (linkinfo, design §10.2) */
    uint8_t page_allow_rs;
    uint8_t early_role_n, early_role, early_bda[6];   /* Role Change(s) before the Connection Complete (wire order) */
    uint8_t accept_pending, accept_bda[6], accept_role;   /* 0409 accepted (CS 0), its Role parameter */
    uint8_t scan_enable;                     /* last 0C1A value the ROM accepted (bt_ctrl_scan_enable) */
    uint8_t inq_active;                      /* 0401 CS 0 seen, no Inquiry Complete / 0402 CC since */
    uint32_t inq_results;                    /* inquiry result events of the running inquiry */
} lv BT_CTRL_HI;

/* ------------------------------------------------------------------------------------------------------------- */
/* helpers                                                                                                        */
/* ------------------------------------------------------------------------------------------------------------- */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static bool seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static void raise_fault(uint32_t code, uint32_t site, uint32_t a, uint32_t b)
{
    bool post = false;
    g.p.enter();
    g.st.faults++;
    if (!g.fault_raised) {
        g.fault_raised = 1u;
        g.st.first_fault = code;
        g.st.fault_site = site;              /* where, with the values that decided it (bt_ctrl.h BT_CTRL_SITE_*) */
        g.st.fault_a = a;
        g.st.fault_b = b;
        post = true;
    }
    g.p.exit();
    if (post) {
        g.p.fault(code);
    }
}

/* Upcalls (inside rom->run()) only record a fault; bt_ctrl_run() posts it after the ROM call returned, so no
 * FreeRTOS call is made from ROM context (plan §3 rule (e)). */
static void defer_fault(uint32_t code, uint32_t site, uint32_t a, uint32_t b)
{
    if (g.pending_rom_fault == BT_CTRL_FAULT_NONE) {
        g.pending_rom_fault = code;
        g.pending_site = site;
        g.pending_a = a;
        g.pending_b = b;
    }
}

/* Plan M1: "bt_ctrl checks bt_stop before every ROM call". Once latched, bt_ctrl parks for good: no ROM call, no
 * reply, no ring activity. Diagnostics (lock_depth, stats) are not ROM calls. */
static bool gate(void)
{
    if (g.parked) {
        return false;
    }
    if (g.p.stop_requested()) {
        g.parked = 1u;
        g.st.parked = 1u;
        return false;
    }
    return true;
}

static uint32_t hc_get(const hcount_t *t, uint16_t h)
{
    for (uint32_t i = 0u; i < BT_CTRL_MAX_LINKS; i++) {
        if (t[i].count != 0u && t[i].handle == h) {
            return t[i].count;
        }
    }
    return 0u;
}

static bool hc_add(hcount_t *t, uint16_t h, uint32_t n)
{
    hcount_t *free_e = NULL;
    for (uint32_t i = 0u; i < BT_CTRL_MAX_LINKS; i++) {
        if (t[i].count != 0u && t[i].handle == h) {
            uint32_t v = t[i].count + n;
            t[i].count = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
            return true;
        }
        if (t[i].count == 0u && free_e == NULL) {
            free_e = &t[i];
        }
    }
    if (n == 0u) {
        return true;
    }
    if (free_e == NULL) {
        return false;
    }
    free_e->handle = h;
    free_e->count = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
    return true;
}

/* Subtract up to n; returns the amount actually subtracted. */
static uint32_t hc_sub(hcount_t *t, uint16_t h, uint32_t n)
{
    for (uint32_t i = 0u; i < BT_CTRL_MAX_LINKS; i++) {
        if (t[i].count != 0u && t[i].handle == h) {
            uint32_t d = n < t[i].count ? n : t[i].count;
            t[i].count = (uint16_t)(t[i].count - d);
            return d;
        }
    }
    return 0u;
}

static uint32_t hc_clear(hcount_t *t, uint16_t h)
{
    return hc_sub(t, h, 0xFFFFFFFFu);
}

static uint32_t tx_free_locked(void)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        n += (g.tx[i].state == SLOT_FREE);
    }
    return n;
}

static uint32_t evt_used(void)
{
    g.p.enter();
    uint32_t u = g.evt_head - g.evt_tail;
    g.p.exit();
    return u;
}

static uint32_t acl_used(void)
{
    g.p.enter();
    uint32_t u = g.acl_head - g.acl_tail;
    g.p.exit();
    return u;
}

/* A slot for a command reply (CC/CS) or NOCP: the whole ring minus outstanding reservations. */
static bool evt_reply_space(void)
{
    return evt_used() + g.evt_resv < BT_CTRL_EVT_SLOTS;
}

/* General events never use the BT_CTRL_EVT_RESERVED slots. */
static bool evt_general_space(void)
{
    return evt_used() + g.evt_resv < BT_CTRL_EVT_SLOTS - BT_CTRL_EVT_RESERVED;
}

static void note_add(uint8_t kind, uint16_t handle, uint8_t status, uint8_t reason, uint8_t local);
#if defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE
/* Headphones bench images: every event to the host becomes a link note (NOCP and inquiry results skipped), so the
 * bench log shows the controller's side of paging, pairing and SSP (first master-role silicon runs, 2026-11-25). */
static void trace_evt(const uint8_t *h4, uint16_t len)
{
    if (len < 4u || h4[0] != 0x04u) {
        return;
    }
    uint8_t code = h4[1];
    if (code == 0x13u || code == 0x02u || code == 0x22u || code == 0x2Fu) {
        return;
    }
    uint16_t op = 0u;
    uint8_t st = h4[3];
    if (code == 0x0Eu && len >= 7u) {          /* CC {ncmd, op lo, op hi, status} */
        op = (uint16_t)(h4[4] | (h4[5] << 8));
        st = h4[6];
        if (st == 0u && (op >> 10) != 0x01u) {
            return;                            /* successful CC outside link control (set-up, reads): not traced */
        }
    } else if (code == 0x0Fu && len >= 7u) {   /* CS {status, ncmd, op lo, op hi} */
        op = (uint16_t)(h4[5] | (h4[6] << 8));
    }
    note_add(BT_CTRL_NOTE_TRACE, op, st, code, 0u);
}
#endif

static void evt_push(const uint8_t *h4, uint16_t len, uint32_t seq)
{
#if defined(BT_CTRL_HCI_TRACE) && BT_CTRL_HCI_TRACE
    trace_evt(h4, len);
#endif
    evt_slot_t *s = &g.evt[g.evt_head % BT_CTRL_EVT_SLOTS];
    memcpy(s->data, h4, len);
    s->len = len;
    s->seq = seq;
    g.p.enter();
    g.evt_head++;
    g.p.exit();
    g.st.evt_queued++;
    g.need_dlv = 1u;
}

/* Free a TX slot owned by bt_ctrl; a full -> not-full transition schedules notify_host_send_available() in bt_dlv
 * (contract §4: the only wake-up for ACL blocked by a full ring). */
static void tx_release(uint32_t idx)
{
    g.p.enter();
    bool was_full = tx_free_locked() == 0u;
    g.tx[idx].state = SLOT_FREE;
    if (was_full && g.admission_open) {
        g.send_avail_pending = 1u;
    }
    g.p.exit();
    if (was_full) {
        g.need_dlv = 1u;
    }
}

/* Drop host ACL for handle h still waiting in the TX ring (only bt_ctrl frees READY slots, so the check and the
 * release need not share one critical section). */
static void tx_drop_acl_for(uint16_t h)
{
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        g.p.enter();
        const tx_slot_t *s = &g.tx[i];
        bool hit = s->state == SLOT_READY && s->data[0] == BT_H4_ACL && (rd16(&s->data[1]) & 0x0FFFu) == h;
        g.p.exit();
        if (hit) {
            tx_release(i);
            g.st.acl_tx_dropped++;
        }
    }
}

/* One link note for the session log (bt_ctrl_note_pop); the oldest is kept when nobody drains the ring. */
static void note_add(uint8_t kind, uint16_t handle, uint8_t status, uint8_t reason, uint8_t local)
{
    g.p.enter();
    if (g.note_n < BT_CTRL_NOTES) {
        bt_ctrl_note_t *n = &g.notes[(g.note_head + g.note_n) % BT_CTRL_NOTES];
        n->kind = kind;
        n->handle = handle;
        n->status = status;
        n->reason = reason;
        n->local = local;
        g.note_n++;
    } else {
        g.st.notes_lost++;
    }
    g.p.exit();
}

/* Remote Name Request Complete (ROM layout {s@0, bdaddr@1, name[248]@7}, param_len >= 7 checked by the packer):
 * noted with the name length, and a successful name kept for bt_ctrl_peer_name(). */
static void rmt_name_seen(const uint8_t *param, uint16_t param_len)
{
    uint16_t n = 0u;
    while (7u + n < param_len && n < BT_CTRL_NAME_LEN && param[7u + n] != 0u) {
        n++;
    }
    if (param[0] == BT_HCI_OK && n != 0u) {
        /* stored before the note is queued: a task that pops the note finds this name (review of session 6) */
        uint16_t c = n < BT_CTRL_PEER_NAME_MAX - 1u ? n : (uint16_t)(BT_CTRL_PEER_NAME_MAX - 1u);
        g.p.enter();
        for (uint32_t i = 0u; i < 6u; i++) {
            g.peer_bda[i] = param[6u - i];   /* wire (LSB first) -> BD_ADDR / esp_bd_addr_t order (MSB first) */
        }
        memcpy(g.peer_name, &param[7], c);
        g.peer_name[c] = '\0';
        g.peer_name_ok = 1u;
        g.p.exit();
    }
    note_add(BT_CTRL_NOTE_NAME, 0u, param[0], (uint8_t)(n > 255u ? 255u : n), 0u);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* ROM upcalls                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------- */
#define BT_HCI_EVT_INQ_CMP  0x01u
#define BT_HCI_EVT_INQ_RES  0x02u
#define BT_HCI_EVT_CONN_CMP 0x03u
#define BT_HCI_EVT_CONN_REQ 0x04u
#define BT_HCI_EVT_RMT_NAME 0x07u
#define BT_HCI_EVT_RMT_FEAT 0x0Bu
#define BT_HCI_EVT_ROLE_CHG 0x12u
#define BT_HCI_EVT_MODE_CHG 0x14u
#define BT_HCI_EVT_MAX_SLOTS 0x1Bu
#define BT_HCI_EVT_PKT_TYPE 0x1Du
#define BT_HCI_EVT_INQ_RSSI 0x22u
#define BT_HCI_EVT_INQ_EXT  0x2Fu
#define BT_CTRL_OP_INQUIRY 0x0401u
#define BT_CTRL_OP_INQUIRY_CANCEL 0x0402u
#define BT_CTRL_OP_CREATE_CONN 0x0405u
#define BT_CTRL_OP_ACCEPT_CONN 0x0409u
#define BT_CTRL_OP_RMT_NAME 0x0419u
#define BT_CTRL_OP_DISCONNECT 0x0406u
#define BT_CTRL_OP_WRITE_SCAN_ENABLE 0x0C1Au

/* The host command the session log follows was answered (by the ROM, or locally when `local`). */
static void cmd_note(uint16_t op, uint16_t handle, uint8_t status, uint8_t local)
{
    switch (op) {
    case BT_CTRL_OP_DISCONNECT:
        note_add(BT_CTRL_NOTE_DISC_CMD, handle, status, 0u, local);
        break;
    case BT_CTRL_OP_RMT_NAME:
        note_add(BT_CTRL_NOTE_NAME_CMD, 0u, status, 0u, local);
        break;
    case BT_CTRL_OP_INQUIRY:
        note_add(BT_CTRL_NOTE_INQ_CMD, 0u, status, 0u, local);
        break;
    case BT_CTRL_OP_CREATE_CONN:
        note_add(BT_CTRL_NOTE_PAGE_CMD, 0u, status, 0u, local);
        break;
    default:
        break;
    }
}

static bt_ctrl_link_info_t *link_of(uint16_t hdl)
{
    return (hdl >= 0x80u && hdl < 0x80u + BT_CTRL_MAX_LINKS) ? &lv.link[hdl - 0x80u] : NULL;
}

/* bda in BD_ADDR order (MSB first) equals the wire-order (LSB first) address w */
static bool bda_is_wire(const uint8_t *bda, const uint8_t *w)
{
    for (uint32_t i = 0u; i < 6u; i++) {
        if (bda[i] != w[5u - i]) {
            return false;
        }
    }
    return true;
}

/* The ROM accepted a forwarded command (status 0 in its initial reply): state the link view needs later. */
static void cmd_accepted(uint16_t op)
{
    if (op == BT_CTRL_OP_CREATE_CONN && g.fwd_plen >= 6u) {
        lv.page_pending = 1u;                 /* the Connection Complete for this address makes an outgoing link */
        memcpy(lv.page_bda, g.fwd_params, 6u);
        lv.page_pkt_type = g.fwd_plen >= 8u ? rd16(&g.fwd_params[6]) : 0u;
        lv.page_allow_rs = g.fwd_plen >= 13u ? g.fwd_params[12] : 0u;
        lv.early_role_n = 0u;
    } else if (op == BT_CTRL_OP_ACCEPT_CONN && g.fwd_plen >= 7u) {
        lv.accept_pending = 1u;
        memcpy(lv.accept_bda, g.fwd_params, 6u);
        lv.accept_role = g.fwd_params[6];
    } else if (op == BT_CTRL_OP_INQUIRY) {
        lv.inq_active = 1u;
        lv.inq_results = 0u;
    } else if (op == BT_CTRL_OP_WRITE_SCAN_ENABLE && g.fwd_plen >= 1u) {
        lv.scan_enable = g.fwd_params[0];
    }
}

/* Link view from the link-level events (ROM param layouts: docs/BT_ROM_HCI_MAP.md §9). */
static void link_event(const bt_ctrl_evt_info_t *info, const uint8_t *param, uint16_t param_len)
{
    uint16_t h = (uint16_t)(info->handle & 0x0FFFu);
    bt_ctrl_link_info_t *l = NULL;
    switch (info->evt_code) {
    case BT_HCI_EVT_CONN_CMP: {                        /* {s, pad, h, pad, bdaddr@5, type, enc} (13) */
        const uint8_t *a = &param[5];
        uint8_t out = lv.page_pending && memcmp(a, lv.page_bda, 6u) == 0;
        uint8_t acc = lv.accept_pending && memcmp(a, lv.accept_bda, 6u) == 0;
        uint8_t role = out ? BT_CTRL_ROLE_MASTER
                       : acc ? (lv.accept_role == 0u ? BT_CTRL_ROLE_MASTER : BT_CTRL_ROLE_SLAVE) : BT_CTRL_ROLE_UNKNOWN;
        /* a Role Change for this address that came before its Connection Complete (the slave asked for the switch
         * during set-up): applied now instead of being lost (it found no link then) */
        uint8_t early = (out || acc) && lv.early_role_n != 0u && memcmp(a, lv.early_bda, 6u) == 0;
        if (early && lv.early_role != BT_CTRL_ROLE_UNKNOWN) {
            role = lv.early_role;
        }
        uint8_t early_n = early ? lv.early_role_n : 0u;
        if (out || acc) {
            lv.early_role_n = 0u;
        }
        lv.page_pending = out ? 0u : lv.page_pending;
        lv.accept_pending = acc ? 0u : lv.accept_pending;
        l = link_of(h);
        if (info->status == BT_HCI_OK && l != NULL) {
            g.p.enter();
            memset(l, 0, sizeof(*l));
            l->up = 1u;
            l->outgoing = out;
            l->role = role;
            l->role_changes = early_n;
            if (out) {
                l->req_pkt_type = lv.page_pkt_type;
                l->req_allow_rs = lv.page_allow_rs;
            }
            for (uint32_t i = 0u; i < 6u; i++) {
                l->bda[i] = a[5u - i];
            }
            g.p.exit();
        }
        break;
    }
    case BT_HCI_EVT_DISC:
        l = link_of(h);
        if (info->status == BT_HCI_OK && l != NULL) {
            g.p.enter();
            l->up = 0u;
            g.p.exit();
        }
        break;
    case BT_HCI_EVT_ROLE_CHG: {                        /* {s, bdaddr, role} (8) verbatim */
        uint16_t hh = 0u;
        bool found = false;
        for (uint32_t i = 0u; i < BT_CTRL_MAX_LINKS; i++) {
            if (lv.link[i].up && bda_is_wire(lv.link[i].bda, &param[1])) {
                hh = (uint16_t)(0x80u + i);
                found = true;
                g.p.enter();
                lv.link[i].role_changes++;
                if (info->status == BT_HCI_OK) {
                    lv.link[i].role = param[7];
                }
                g.p.exit();
                break;
            }
        }
        if (!found && ((lv.page_pending && memcmp(&param[1], lv.page_bda, 6u) == 0) ||
                       (lv.accept_pending && memcmp(&param[1], lv.accept_bda, 6u) == 0))) {
            /* before the Connection Complete of the link being set up: kept for it (TWS earbuds switch here) */
            if (lv.early_role_n == 0u || memcmp(&param[1], lv.early_bda, 6u) != 0) {
                lv.early_role_n = 0u;
                lv.early_role = BT_CTRL_ROLE_UNKNOWN;
                memcpy(lv.early_bda, &param[1], 6u);
            }
            if (lv.early_role_n < 255u) {
                lv.early_role_n++;
            }
            if (info->status == BT_HCI_OK) {
                lv.early_role = param[7];
            }
        }
        note_add(BT_CTRL_NOTE_ROLE, hh, info->status, param[7], 0u);
        break;
    }
    case BT_HCI_EVT_MAX_SLOTS:                         /* {u16 h, slots} (3) */
        l = link_of((uint16_t)(rd16(param) & 0x0FFFu));
        if (l != NULL) {
            g.p.enter();
            l->max_slots = param[2];
            g.p.exit();
        }
        break;
    case BT_HCI_EVT_MODE_CHG:                          /* {s, pad, h, mode@4, pad, interval} */
        l = link_of(h);
        if (l != NULL && info->status == BT_HCI_OK) {
            g.p.enter();
            l->mode = param[4];
            g.p.exit();
        }
        break;
    case BT_HCI_EVT_RMT_FEAT:                          /* {s, pad, h, features[8]@4} (FIX_REM_FEAT) */
        l = link_of((uint16_t)(rd16(&param[2]) & 0x0FFFu));
        if (l != NULL && info->status == BT_HCI_OK && l->up) {
            g.p.enter();
            l->rmt_feat_seen = 1u;
            l->rmt_feat0 = param[4];
            l->rmt_feat3 = param[7];
            g.p.exit();
        }
        break;
    case BT_HCI_EVT_PKT_TYPE:                          /* {s, pad, h, u16 types@4} */
        l = link_of(h);
        if (l != NULL && info->status == BT_HCI_OK) {
            g.p.enter();
            l->pkt_type = rd16(&param[4]);
            g.p.exit();
        }
        break;
    case BT_HCI_EVT_INQ_RES:
    case BT_HCI_EVT_INQ_RSSI:
    case BT_HCI_EVT_INQ_EXT:
        lv.inq_results++;
        break;
    case BT_HCI_EVT_INQ_CMP:
        lv.inq_active = 0u;
        note_add(BT_CTRL_NOTE_INQ_DONE, 0u, info->status, (uint8_t)(lv.inq_results > 255u ? 255u : lv.inq_results), 0u);
        break;
    default:
        break;
    }
    (void)param_len;                                   /* every field read here is inside the packer's `need` */
}

/* One bit per ROM ACL handle 0x80..0x86 (BT_CTRL_MAX_LINKS); 0 for any other handle. */
static uint8_t handle_bit(uint16_t h)
{
    return (h >= 0x80u && h < 0x80u + BT_CTRL_MAX_LINKS) ? (uint8_t)(1u << (h - 0x80u)) : 0u;
}

static void apply_event_side_effects(const bt_ctrl_evt_info_t *info)
{
    if (info->kind != BT_CTRL_EVK_EVT || info->status != BT_HCI_OK) {
        return;
    }
    uint16_t h = (uint16_t)(info->handle & 0x0FFFu);
    if (info->evt_code == BT_HCI_EVT_DISC) {
        /* Disconnection Complete: the host returns this handle's un-completed packets to its own window
         * (l2c on disconnect); bt_ctrl does the same for its ROM-side count and drops the NOCP it still owes.
         * ACL still queued for h is flushed like a real controller does: the host already reclaimed its credits,
         * and the ROM may hand h to the next connection before tx_drain() runs. */
        g.inflight_total -= hc_clear(g.inflight, h);
        (void)hc_clear(g.nocp_pend, h);
        tx_drop_acl_for(h);
        g.dead |= handle_bit(h);
        if (g.fwd_pending && g.fwd_lc && g.fwd_handle == h) {
            g.fwd_orphan = 1u;               /* answered locally once the ROM queue is idle (bt_ctrl_run) */
        }
        if (g.fwd_late_op != 0u && g.fwd_late_lc && g.fwd_late_handle == h) {
            g.fwd_late_op = 0u;              /* the ROM dropped the saved message with the LC instance: no late reply */
            g.st.fwd_late_dropped++;
        }
    } else if (info->evt_code == BT_HCI_EVT_CONN_CMP) {
        /* A new link on h: the host has not seen this Connection Complete yet, so any ACL for h in the TX ring was
         * sent for an earlier link that used the same handle. */
        tx_drop_acl_for(h);
        g.dead &= (uint8_t)~handle_bit(h);
    }
}

static bt_rom_msg_ret_t accept_general(uint16_t id, uint8_t *param, uint16_t param_len, const uint8_t *h4,
                                       uint16_t len, uint32_t seq)
{
    if (g.def_n == 0u && evt_general_space()) {   /* never ahead of older deferred events */
        evt_push(h4, len, seq);
        return BT_ROM_MSG_CONSUMED;
    }
    if (g.def_n < BT_CTRL_EVT_DEFER) {
        deferred_t *d = &g.def[(g.def_head + g.def_n) % BT_CTRL_EVT_DEFER];
        d->param = param;
        d->seq = seq;
        d->id = id;
        d->len = param_len;
        g.def_n++;
        g.st.evt_deferred++;
        return BT_ROM_MSG_KEEP;
    }
    g.st.evt_overflow++;
    defer_fault(BT_CTRL_FAULT_EVT_OVERFLOW, BT_CTRL_SITE_EVT_OVERFLOW, id, g.def_n);
    return BT_ROM_MSG_CONSUMED;
}

static void (*volatile s_nocp_obs)(uint16_t handle, uint32_t pkts);

void bt_ctrl_set_nocp_observer(void (*obs)(uint16_t handle, uint32_t pkts))
{
    s_nocp_obs = obs;
}

static void rom_nocp(const uint8_t *p, uint16_t len)
{
    g.st.nocp_from_rom++;
    if (len < 2u) {
        g.st.rom_short++;
        return;
    }
    uint32_t n = p[0];
    uint32_t max_n = (uint32_t)(len - 2u) / 4u;     /* clamp like bt_hci.c:817-833 */
    if (n > max_n) {
        n = max_n;
        g.st.rom_short++;
    }
    for (uint32_t i = 0u; i < n; i++) {
        uint16_t h = (uint16_t)(rd16(&p[2u + 4u * i]) & 0x0FFFu);
        uint32_t c = rd16(&p[4u + 4u * i]);
        uint32_t done = hc_sub(g.inflight, h, c);
        g.inflight_total -= done;
        g.st.nocp_excess += c - done;
        bt_ctrl_link_info_t *l = link_of(h);
        if (l != NULL && done != 0u) {
            uint32_t t = g.now_ms != NULL ? g.now_ms() : 0u;
            g.p.enter();
            l->acl_completed += done;
            l->nocp_last_ms = t;
            g.p.exit();
        }
        if (done != 0u && !hc_add(g.nocp_pend, h, done)) {
            g.st.invariant++;
            defer_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_NOCP_TABLE, h, done);
        }
        void (*obs)(uint16_t, uint32_t) = s_nocp_obs;
        if (done != 0u && obs != NULL) {
            obs(h, done);
        }
    }
}

static bt_rom_msg_ret_t rom_acl_rx(uint8_t *param, uint16_t len, uint32_t seq)
{
    bt_rom_acl_desc_t d;
    g.st.acl_rx_msgs++;
    if (len < sizeof(d)) {
        g.st.acl_rx_bad++;
        return BT_ROM_MSG_CONSUMED;          /* no trustworthy buf_ptr to free */
    }
    memcpy(&d, param, sizeof(d));
    if (d.buf_ptr == NULL) {
        g.st.acl_rx_bad++;
        return BT_ROM_MSG_CONSUMED;
    }
    uint16_t conhdl = (uint16_t)(d.conhdl & 0x0FFFu);
    uint8_t bit = handle_bit(conhdl);
    if (d.length == 0u || d.length > BT_ROM_EM_BUF_PAYLOAD || bit == 0u || (g.dead & bit) != 0u ||
        g.held_n >= BT_CTRL_HELD_ACL) {
        if (d.length == 0u || d.length > BT_ROM_EM_BUF_PAYLOAD || bit == 0u) {
            g.st.acl_rx_bad++;
        } else if ((g.dead & bit) != 0u) {
            g.st.acl_rx_dropped_disc++;
        } else {
            g.st.acl_rx_overflow++;
            g.st.invariant++;
            defer_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_ACL_RX_HELD, d.conhdl, d.length);
        }
        if (!gate()) {
            return BT_ROM_MSG_KEEP;
        }
        g.rom->em_rx_free(d.buf_ptr);        /* CONSUMED frees only the ke_msg: free EM first (BT_ROM_INTERFACE §6.2) */
        return BT_ROM_MSG_CONSUMED;
    }
    held_acl_t *h = &g.held[(g.held_head + g.held_n) % BT_CTRL_HELD_ACL];
    h->param = param;
    h->em = d.buf_ptr;
    h->seq = seq;
    h->handle = conhdl;
    h->len = d.length;
    h->off = 0u;
    h->pb = (uint8_t)(d.pb_bc_flag & 0x3u);
    h->bc = (uint8_t)((d.pb_bc_flag >> 2) & 0x3u);
    g.held_n++;
    return BT_ROM_MSG_KEEP;
}

static bt_rom_msg_ret_t up_on_hci_msg(void *ctx, uint16_t id, uint8_t *param, uint16_t param_len, uint16_t src_id)
{
    (void)src_id;
    if (g.parked) {
        return BT_ROM_MSG_KEEP;              /* bt_stop latched: no further ROM call, ownership left as is */
    }
    if ((uintptr_t)ctx != (uintptr_t)g.rom_gen || !g.rom_running || g.state != ST_OPEN) {
        g.st.late_upcalls++;                 /* late callback from a previous controller incarnation: rejected */
        return BT_ROM_MSG_CONSUMED;
    }
    if (g.rom->lock_depth != NULL && g.rom->lock_depth() != 0u) {
        g.st.lock_violations++;              /* ROM must call TASK_HCI with its lock released (BT_ROM_INTERFACE §3.4) */
        g.st.invariant++;
        defer_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_LOCK_UPCALL, id, g.rom->lock_depth());
    }
    g.st.rom_msgs++;
    g.upcall_activity = 1u;
    uint32_t seq = g.rx_seq++;               /* ROM delivery order, kept through the rings to bt_dlv */
    if (id == BT_ROM_MSG_ACL_RX) {
        return rom_acl_rx(param, param_len, seq);
    }
    if (id == BT_ROM_MSG_NOCP) {
        rom_nocp(param, param_len);
        return BT_ROM_MSG_CONSUMED;
    }
    bt_ctrl_evt_info_t info;
    uint16_t len = bt_ctrl_evt_pack(id, param, param_len, g.scratch, &info);
    if (info.kind == BT_CTRL_EVK_UNKNOWN) {
        g.st.rom_unknown_ids++;              /* counted, never silently: BT_ROM_INTERFACE §12 last row */
        return BT_ROM_MSG_CONSUMED;
    }
    if (info.kind == BT_CTRL_EVK_SHORT) {
        g.st.rom_short++;
        return BT_ROM_MSG_CONSUMED;
    }
    if ((info.kind == BT_CTRL_EVK_CC || info.kind == BT_CTRL_EVK_CS) && g.fwd_pending && info.opcode == g.fwd_opcode) {
        /* Reply to the forwarded command: its slot was reserved when the command was admitted. */
        cmd_note(info.opcode, g.fwd_handle, info.status, 0u);
        if (info.status == BT_HCI_OK) {
            cmd_accepted(info.opcode);
        }
        if (info.opcode == BT_CTRL_OP_INQUIRY_CANCEL) {
            lv.inq_active = 0u;               /* 0402 CC: the inquiry is over (no Inquiry Complete follows a cancel) */
        }
        g.fwd_pending = 0u;
        g.fwd_orphan = 0u;
        g.rom_cmd_credits++;
        g.evt_resv--;
        evt_push(g.scratch, len, seq);
        g.st.evt_replies++;
        if (info.status == BT_HCI_OK && info.kind == BT_CTRL_EVK_CC) {
            if (info.opcode == 0x0C13u) {                     /* name cache (opcode table §3.7) */
                memset(g.local_name, 0, sizeof(g.local_name));
                memcpy(g.local_name, g.fwd_params, g.fwd_plen < BT_CTRL_NAME_LEN ? g.fwd_plen : BT_CTRL_NAME_LEN);
            } else if (info.opcode == 0x0C56u) {              /* page-1 SSP host bit (§3.6) */
                g.ssp_host = (g.fwd_plen >= 1u && g.fwd_params[0] == 0x01u) ? 1u : 0u;
            }
        }
        if (info.opcode == BT_CTRL_OP_WRITE_SCAN_ENABLE && info.status == BT_HCI_ERR_CMD_DISALLOWED) {
            /* ROM 0x3EAC4 refuses page scan while more than one ACL link is up (BT_ROM_HCI_MAP.md §11): a defined
             * controller answer at run time (second peer paging, or a dead link the ROM has not reported yet), not
             * a broken controller. Passed through and noted (the session re-applies its scan mode when a link
             * goes); no BT fault (silicon bench 2026-11-24 stopped BT here). */
            g.st.scan_refused++;
            note_add(BT_CTRL_NOTE_SCAN_REFUSED, 0u, info.status, g.fwd_plen >= 1u ? g.fwd_params[0] : 0u, 0u);
        } else if (g.fwd_cls == BTC_M && info.status != BT_HCI_OK) {
            g.st.mandatory_fwd_fail++;       /* opcode table §2 rule 7: passed through, BT fault */
            g.st.invariant++;
            defer_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_CLASS_M_STATUS, info.opcode, info.status);
        }
        return BT_ROM_MSG_CONSUMED;
    }
    if ((info.kind == BT_CTRL_EVK_CC || info.kind == BT_CTRL_EVK_CS) && g.fwd_late_op != 0u &&
        info.opcode == g.fwd_late_op) {
        /* The ROM's reply to a command bt_ctrl already answered at its deadline: the host has its reply (one per
         * command, opcode table §2 rule 1), so this one is counted and consumed, never delivered twice. */
        g.fwd_late_op = 0u;
        g.st.fwd_late_reply++;
        return BT_ROM_MSG_CONSUMED;
    }
    if (info.kind == BT_CTRL_EVK_CC || info.kind == BT_CTRL_EVK_CS) {
        g.st.evt_unsolicited_reply++;
        return BT_ROM_MSG_CONSUMED;          /* never forward Num_HCI_Command_Packets=1 without a matching command */
    } else if (info.evt_code == BT_HCI_EVT_DISC) {
        note_add(BT_CTRL_NOTE_DISC, (uint16_t)(info.handle & 0x0FFFu), info.status, len >= 7u ? g.scratch[6] : 0u, 0u);
    } else if (info.evt_code == BT_HCI_EVT_CONN_CMP) {
        note_add(BT_CTRL_NOTE_CONN, (uint16_t)(info.handle & 0x0FFFu), info.status, 0u, 0u);
    } else if (info.evt_code == BT_HCI_EVT_CONN_REQ) {
        note_add(BT_CTRL_NOTE_CONN_REQ, 0u, 0u, 0u, 0u);
    } else if (info.evt_code == BT_HCI_EVT_RMT_NAME) {
        rmt_name_seen(param, param_len);
    }
    if (info.kind == BT_CTRL_EVK_EVT) {
        link_event(&info, param, param_len);
    }
    apply_event_side_effects(&info);
    return accept_general(id, param, param_len, g.scratch, len, seq);
}

static void up_on_irq(void *ctx)
{
    (void)ctx;
    g.st.irq_wakes++;
    g.p.wake_ctrl_from_isr();
}

static void up_on_fault(void *ctx, uint32_t code)
{
    (void)ctx;
    g.st.rom_faults++;                       /* may run with the ROM lock held: flags only */
    g.pending_rom_fault = BT_CTRL_FAULT_ROM;   /* overrides a lesser pending code */
    g.pending_site = BT_CTRL_SITE_ROM_FAULT;
    g.pending_a = code;
    g.pending_b = 0u;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* state reset helpers                                                                                            */
/* ------------------------------------------------------------------------------------------------------------- */
static void controller_state_reset(void)
{
    g.rom_cmd_credits = BT_CTRL_ROM_CMD_CREDITS;
    g.fwd_pending = 0u;
    g.fwd_lc = 0u;
    g.fwd_orphan = 0u;
    g.fwd_late_op = 0u;
    g.fwd_late_lc = 0u;
    g.fwd_late_handle = 0u;
    g.hold_valid = 0u;
    g.dead = 0u;
    g.evt_resv = 0u;
    memset(g.inflight, 0, sizeof(g.inflight));
    g.inflight_total = 0u;
    memset(g.nocp_pend, 0, sizeof(g.nocp_pend));
    g.c2h_fc = 0u;                           /* HCI default: controller-to-host flow control off until 0C31 */
    g.c2h_len = BT_CTRL_C2H_ACL_LEN;
    g.c2h_credits = BT_CTRL_C2H_ACL_CREDITS;
    memset(g.c2h_out, 0, sizeof(g.c2h_out));
    g.c2h_out_total = 0u;
    memset(g.local_name, 0, sizeof(g.local_name));
    g.ssp_host = 0u;
    lv.page_pending = lv.accept_pending = 0u;
    lv.early_role_n = 0u;
    lv.inq_active = 0u;
    lv.inq_results = 0u;
    g.p.enter();
    memset(lv.link, 0, sizeof(lv.link));
    lv.scan_enable = 0u;
    g.p.exit();
    g.held_head = g.held_n = 0u;
    g.acl_min_valid = 0u;
    memset(g.acl_min_seq, 0, sizeof(g.acl_min_seq));
    g.def_head = g.def_n = 0u;
    g.acl_blocked = 0u;
    g.p.enter();
    g.acl_epoch++;                           /* undelivered ACL of the previous controller state is dropped by bt_dlv */
    g.p.exit();
}

/* Release every ROM object bt_ctrl holds (KEEP'd ACL_RX + EM RX buffers, deferred events) while the ROM still runs:
 * a restart invalidates all of them (BT_ROM_INTERFACE §2.1 "Stop"). Nothing is dropped silently: each released
 * deferred event is counted in *evt_dropped and each held ACL_RX in acl_rx_dropped_restart. Returns false if parked. */
static bool release_rom_objects(uint32_t *evt_dropped)
{
    while (g.held_n != 0u) {
        held_acl_t *h = &g.held[g.held_head];
        if (h->em != NULL) {
            if (!gate()) {
                return false;
            }
            uint8_t *em = h->em;
            h->em = NULL;
            g.rom->em_rx_free(em);
        }
        if (!gate()) {
            return false;
        }
        g.rom->msg_free(h->param);
        g.held_head = (g.held_head + 1u) % BT_CTRL_HELD_ACL;
        g.held_n--;
        g.st.acl_rx_dropped_restart++;
    }
    while (g.def_n != 0u) {
        if (!gate()) {
            return false;
        }
        g.rom->msg_free(g.def[g.def_head].param);
        g.def_head = (g.def_head + 1u) % BT_CTRL_EVT_DEFER;
        g.def_n--;
        (*evt_dropped)++;
    }
    return true;
}

/* Empty the rings (teardown). FILLING slots belong to an in-progress send() on hciT, which drops its packet itself
 * because the session changed. */
static void rings_reset(void)
{
    g.p.enter();
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        if (g.tx[i].state == SLOT_READY) {
            g.tx[i].state = SLOT_FREE;
        }
    }
    g.send_avail_pending = 0u;
    g.evt_head = g.evt_tail = 0u;
    g.acl_head = g.acl_tail = 0u;
    g.p.exit();
}

/* ------------------------------------------------------------------------------------------------------------- */
/* host -> controller                                                                                             */
/* ------------------------------------------------------------------------------------------------------------- */
static bool retry_deferred(void);

/* Real controller reset (plan M1 "Reset"): stop, RF calibration (before the EM wipe), start, IRQ on. Returns the
 * HCI status for the synthesized CC. `keep` is the TX slot carrying the Reset itself.
 * Controller -> host: events already in the ring are kept and delivered before the Reset CC; deferred (KEEP'd) ROM
 * events are packed into free general slots first, and those that still do not fit die with the ROM restart and are
 * counted in evt_dropped_reset (never silently). Undelivered ACL is dropped by epoch (dlv_stale), held ACL_RX is
 * counted in acl_rx_dropped_restart. Host -> controller: see the TX ring sweep below. */
static uint8_t do_reset(uint32_t keep)
{
    g.st.resets++;
    (void)retry_deferred();
    if (g.parked || !release_rom_objects(&g.st.evt_dropped_reset)) {
        return BT_HCI_ERR_HW_FAILURE;
    }
    uint8_t status = BT_HCI_OK;
    if (g.rom_running) {
        if (!gate()) {
            return BT_HCI_ERR_HW_FAILURE;
        }
        int s = g.rom->stop();
        g.rom_running = 0u;
        if (s != 0) {
            status = BT_HCI_ERR_HW_FAILURE;
        }
    }
    g.rom_gen++;
    g.up.ctx = (void *)(uintptr_t)g.rom_gen;
    controller_state_reset();
    /* Host state is reset too: drop queued ACL (old handles) and queued 0C35 (old window). Ordinary commands queued
     * behind the Reset are kept: they are answered after it. */
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        const tx_slot_t *s = &g.tx[i];
        if (i != keep && s->state == SLOT_READY &&
            (s->data[0] == BT_H4_ACL || rd16(&s->data[1]) == BT_CTRL_OP_HOST_NUM_COMPLETED)) {
            tx_release(i);
        }
    }
    if (status == BT_HCI_OK) {
        if (!gate()) {
            return BT_HCI_ERR_HW_FAILURE;
        }
        g.st.rf_cal_last = g.rom->rf_calibrate();
        if (!gate()) {
            return BT_HCI_ERR_HW_FAILURE;
        }
        if (g.rom->start(g.bd_addr, &g.up) != 0) {
            status = BT_HCI_ERR_HW_FAILURE;
        } else {
            g.rom_running = 1u;
            if (!gate()) {
                return BT_HCI_ERR_HW_FAILURE;
            }
            g.rom->irq_enable(1);
        }
    }
    if (status != BT_HCI_OK) {
        /* The controller is not running (stop or start failed): mask IRQ 26 as on the teardown path, so a controller
         * in an unknown state cannot keep waking bt_ctrl while the bottom half is skipped. */
        if (!gate()) {
            return BT_HCI_ERR_HW_FAILURE;
        }
        g.rom->irq_enable(0);
        g.st.reset_failures++;
        raise_fault(BT_CTRL_FAULT_RESET, BT_CTRL_SITE_RESET, status, 0u);
    }
    return status;
}

static void local_reply(uint16_t len)
{
    if (len != 0u) {
        evt_push(g.scratch, len, g.rx_seq++);
    }
}

/* 0C35 Host Number Of Completed Packets: LOCAL, no reply, no ROM credit (opcode table §3.2, contract §6.3). */
static void handle_host_nocp(const uint8_t *prm, uint8_t plen)
{
    g.st.tx_noack++;
    if (plen < 1u || prm[0] == 0u || plen != 1u + 4u * (uint32_t)prm[0]) {
        g.st.c2h_malformed++;
        return;
    }
    for (uint32_t i = 0u; i < prm[0]; i++) {
        uint16_t h = (uint16_t)(rd16(&prm[1u + 4u * i]) & 0x0FFFu);
        uint32_t c = rd16(&prm[3u + 4u * i]);
        if (!g.c2h_fc || hc_get(g.c2h_out, h) == 0u) {
            g.st.c2h_invalid++;
            continue;
        }
        uint32_t done = hc_sub(g.c2h_out, h, c);
        g.c2h_out_total -= done;
        g.st.c2h_returned += done;
        if (done < c) {
            g.st.c2h_excess++;
        }
    }
    /* "a credit return kicks the dispatcher": rx_dispatch() runs after tx_drain() in the same pass. */
    g.upcall_activity = 1u;
}

static void apply_syn_side_effects(uint16_t op, const uint8_t *prm, uint8_t plen)
{
    if (op == 0x0C31u) {                     /* Set Controller To Host Flow Control: arms the RX credit gate */
        if (plen < 1u) {
            g.st.c2h_config_mismatch++;
            return;
        }
        g.c2h_fc = (prm[0] & 0x01u) ? 1u : 0u;
        g.st.c2h_config_mismatch += (prm[0] != 0x01u);
    } else if (op == 0x0C33u) {              /* Host Buffer Size: enforce min(host, 4 x 1021) */
        if (plen < 7u) {
            g.st.c2h_config_mismatch++;
            return;
        }
        uint16_t acl_len = rd16(&prm[0]);
        uint16_t acl_num = rd16(&prm[3]);
        g.c2h_len = (uint16_t)(acl_len == 0u ? BT_CTRL_C2H_ACL_LEN
                               : acl_len < BT_CTRL_C2H_ACL_LEN ? acl_len : BT_CTRL_C2H_ACL_LEN);
        g.c2h_credits = acl_num == 0u ? BT_CTRL_C2H_ACL_CREDITS
                        : acl_num < BT_CTRL_C2H_ACL_CREDITS ? acl_num : BT_CTRL_C2H_ACL_CREDITS;
        g.st.c2h_config_mismatch += (acl_len != BT_CTRL_C2H_ACL_LEN || acl_num != BT_CTRL_C2H_ACL_CREDITS);
    }
}

/* Forward to the ROM KE handler. Returns 0 if sent, else the HCI status for a local failure reply; 0xFF if parked. */
static uint8_t forward_cmd(const bt_ctrl_cmd_row_t *row, uint16_t op, const uint8_t *prm, uint8_t plen)
{
    if (!g.rom_running) {
        return BT_HCI_ERR_CMD_DISALLOWED;
    }
    uint16_t dest = BT_ROM_MSG_DEST(row->rom_req);
    bool lc = dest == BT_ROM_TASK_LC;
    uint16_t handle = 0u;
    if (lc) {                                /* per-link task from the connection handle (bt_hci.c:940-948) */
        if (plen < 2u) {
            return BT_HCI_ERR_INVALID_PARAMS;
        }
        if (!gate()) {
            return 0xFFu;
        }
        handle = (uint16_t)(rd16(prm) & 0x0FFFu);
        if ((g.dead & handle_bit(handle)) != 0u) {
            /* Disconnection Complete already seen for this handle: the ROM may still map it to a dying LC instance
             * whose handlers consume commands silently (BT_ROM_HCI_MAP.md §1.2 (c)), which would wedge the one-command
             * window. A real controller answers 0x02 here. */
            g.st.cmd_dead_handle++;
            return BT_HCI_ERR_UNKNOWN_CONN;
        }
        dest = g.rom->conhdl_to_task(handle);
        if (dest == BT_ROM_TASK_INVALID) {
            return BT_HCI_ERR_UNKNOWN_CONN;
        }
    }
    uint8_t min_len = bt_ctrl_cmds_fwd_min_len(op);
    uint8_t copy_len = (op == 0x0C13u && plen > BT_CTRL_NAME_LEN) ? BT_CTRL_NAME_LEN : plen;
    uint16_t alloc_len = copy_len > min_len ? copy_len : min_len;
    if (!gate()) {
        return 0xFFu;
    }
    uint8_t *m = g.rom->msg_alloc(row->rom_req, dest, BT_ROM_TASK_HCI, alloc_len);
    if (m == NULL) {
        g.st.cmd_alloc_refused++;            /* real impl: g_bt_heap budget check refused (ROM itself cannot fail) */
        return BT_HCI_ERR_MEM_FULL;
    }
    if (copy_len != 0u) {
        memcpy(m, prm, copy_len);            /* verified layouts only (fwd_min_len table; 0x0C13 keeps m[248]==0) */
    }
    if (!gate()) {
        return 0xFFu;
    }
    g.rom->msg_send(m);
    g.rom_cmd_credits--;
    g.evt_resv++;
    g.fwd_pending = 1u;
    g.fwd_lc = lc ? 1u : 0u;
    g.fwd_orphan = 0u;
    g.fwd_handle = handle;
    g.fwd_opcode = op;
    g.fwd_t0 = g.now_ms != NULL ? g.now_ms() : 0u;
    g.fwd_cls = row->cls;
    g.fwd_plen = plen;
    memcpy(g.fwd_params, prm, plen);
    return 0u;
}

/* The link a pending LC-destined command addressed has gone (successful Disconnection Complete) and the ROM queue
 * ran empty without a reply: the ROM dropped the message with the LC instance. Answer it locally in its reserved
 * slot (status 0x02 Unknown Connection Identifier) so the one-command window does not wedge. A reply the ROM was
 * going to send would already have been dispatched before its queue ran empty, and is then matched normally. */
static void fwd_local_reply(uint8_t status)
{
    cmd_note(g.fwd_opcode, g.fwd_handle, status, 1u);
    const bt_ctrl_cmd_row_t *row = bt_ctrl_cmds_lookup(g.fwd_opcode);
    uint16_t len = bt_ctrl_cmds_reply_fail(row, g.fwd_opcode, g.fwd_params, g.fwd_plen, status, g.scratch);
    g.fwd_pending = 0u;
    g.fwd_orphan = 0u;
    g.rom_cmd_credits++;
    g.evt_resv--;
    if (len != 0u) {
        evt_push(g.scratch, len, g.rx_seq++);
    }
}

static void fwd_orphan_reply(void)
{
    g.st.fwd_orphaned++;
    fwd_local_reply(BT_HCI_ERR_UNKNOWN_CONN);
}

/* Forwarded-command deadline (BT_ROM_HCI_MAP.md §6.1-6.2). The ROM can consume a command without any reply (LC
 * handlers in idle/detach states) or hold it on its saved queue; either way the one-command window would stay shut
 * until Bluedroid's own 8 s command timeout (hci_layer.c COMMAND_PENDING_TIMEOUT) posts a BT fault and restarts the
 * stack. Just before that, bt_ctrl answers a class-O command itself (status 0x22) so only that command fails. Class M
 * commands keep waiting: their failure must reach the recovery ladder. Needs bt_ctrl_set_clock(). */
static void fwd_deadline_check(void)
{
    if (!g.fwd_pending || g.fwd_orphan || g.now_ms == NULL || g.fwd_cls != BTC_O) {
        return;
    }
    if ((uint32_t)(g.now_ms() - g.fwd_t0) < BT_CTRL_FWD_DEADLINE_MS) {
        return;
    }
    g.st.fwd_deadline++;
    g.fwd_late_op = g.fwd_opcode;
    g.fwd_late_lc = g.fwd_lc;
    g.fwd_late_handle = g.fwd_handle;
    g.fwd_late_t0 = g.now_ms();
    g.fwd_late_counted = 0u;
    fwd_local_reply(BT_HCI_ERR_LMP_TIMEOUT);
}

/* Both the late-reply match and the pending-reply match key on the opcode alone, so a new command with the opcode of a
 * deadline-answered one must not reach the ROM while that late reply can still come: the late reply would be taken as
 * the new command's and the new command's own reply swallowed as "late" (a SAVED 0419 behind another LM state is the
 * realistic case, BT_ROM_HCI_MAP.md §6.2). It stays in the TX ring (the host's one-command window keeps it the only
 * one) until the late reply was consumed, its link disconnected, or BT_CTRL_FWD_DEADLINE_MS passed again. */
static bool fwd_late_hold(uint16_t op)
{
    if (g.fwd_late_op == 0u || op != g.fwd_late_op) {
        return false;
    }
    if (g.now_ms == NULL || (uint32_t)(g.now_ms() - g.fwd_late_t0) >= BT_CTRL_FWD_DEADLINE_MS) {
        g.fwd_late_op = 0u;                  /* given up on the late reply: if it still comes it is unsolicited */
        g.st.fwd_late_expired++;
        return false;
    }
    if (!g.fwd_late_counted) {
        g.fwd_late_counted = 1u;             /* one count per held command (the host has one command in flight) */
        g.st.fwd_late_held++;
        g.hold_valid = 1u;
        g.hold_t0 = g.now_ms();
    }
    return true;
}

static void process_cmd(uint32_t idx)
{
    const uint8_t *d = g.tx[idx].data;
    uint16_t op = rd16(&d[1]);
    uint8_t plen = d[3];
    const uint8_t *prm = &d[4];
    if (op == BT_CTRL_OP_HOST_NUM_COMPLETED) {
        handle_host_nocp(prm, plen);
        return;
    }
    g.st.tx_cmds++;
    bool held = g.hold_valid != 0u;          /* this command waited in the ring for a late reply (fwd_late_hold) */
    g.hold_valid = 0u;
    const bt_ctrl_cmd_row_t *row = bt_ctrl_cmds_lookup(op);
    if (row == NULL) {                       /* closed table default: CS 0x01, counted (opcode table §2 rule 5) */
        g.st.cmd_unknown++;
        g.st.unknown_opcode_last = op;
        local_reply(bt_ctrl_cmds_reply_fail(NULL, op, prm, plen, BT_HCI_ERR_UNKNOWN_CMD, g.scratch));
        return;
    }
    switch (bt_ctrl_cmds_eff(row)) {
    case BTC_SYN: {
        bt_ctrl_cmd_ctx_t ctx = { g.bd_addr, g.local_name, g.ssp_host, BT_HCI_OK };
        if (op == BT_CTRL_OP_RESET) {
            ctx.reset_status = do_reset(idx);
            if (g.parked) {
                return;
            }
        }
        apply_syn_side_effects(op, prm, plen);
        g.st.cmd_syn++;
        local_reply(bt_ctrl_cmds_reply_syn(row, prm, plen, &ctx, g.scratch));
        return;
    }
    case BTC_FWD: {
        uint8_t s = forward_cmd(row, op, prm, plen);
        if (s == 0xFFu) {
            return;                          /* parked */
        }
        if (s == 0u) {
            g.st.cmd_fwd++;
            if (held) {
                /* Keep inside Bluedroid's 8 s command timeout while leaving >= 500 ms for the ROM to execute it. */
                uint32_t max_age = BT_CTRL_FWD_DEADLINE_MS - 500u;
                g.fwd_t0 = (g.now_ms != NULL && (uint32_t)(g.fwd_t0 - g.hold_t0) > max_age)
                               ? (uint32_t)(g.fwd_t0 - max_age)
                               : g.hold_t0;
            }
            return;
        }
        g.st.cmd_local_fail++;
        cmd_note(op, plen >= 2u ? (uint16_t)(rd16(prm) & 0x0FFFu) : 0u, s, 1u);
        if (row->cls == BTC_M) {
            g.st.invariant++;                /* class M must not fail */
            raise_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_CLASS_M_LOCAL, op, s);
        }
        local_reply(bt_ctrl_cmds_reply_fail(row, op, prm, plen, s, g.scratch));
        return;
    }
    default:                                 /* BTC_REJ (LOCAL is only 0C35, handled above) */
        g.st.cmd_rej++;
        local_reply(bt_ctrl_cmds_reply_fail(row, op, prm, plen, BT_HCI_ERR_UNKNOWN_CMD, g.scratch));
        return;
    }
}

enum { ACL_DONE = 0, ACL_BLOCKED = 1, ACL_PARKED = 2 };

static int process_acl(uint32_t idx)
{
    const uint8_t *d = g.tx[idx].data;
    uint16_t hdr = rd16(&d[1]);
    uint16_t handle = (uint16_t)(hdr & 0x0FFFu);
    uint16_t dlen = rd16(&d[3]);
    if (!g.rom_running) {
        g.st.acl_tx_dropped++;
        return ACL_DONE;
    }
    if ((g.dead & handle_bit(handle)) != 0u) {
        /* Sent before the host saw this handle's Disconnection Complete. The host returns its credits for the handle
         * on that event, and a packet handed to a dying LC would never be completed by a NOCP: it would hold one of
         * the 1005 controller credits for good. */
        g.st.acl_tx_dropped++;
        g.st.acl_tx_dead_handle++;
        return ACL_DONE;
    }
    if (!gate()) {
        return ACL_PARKED;
    }
    uint8_t type = g.rom->conhdl_type(handle);
    if (!gate()) {
        return ACL_PARKED;
    }
    uint16_t task = g.rom->conhdl_to_task(handle);
    if (type != 1u || task == BT_ROM_TASK_INVALID) {
        g.st.acl_tx_dropped++;               /* stale handle after a disconnect: the host already reclaimed the credit */
        return ACL_DONE;
    }
    if (!gate()) {
        return ACL_PARKED;
    }
    uint8_t *em = g.rom->em_tx_alloc();
    if (em == NULL) {
        g.st.acl_tx_em_fail++;               /* opcode table §3.1: EM TX failure with a credit = invariant violation */
        g.st.invariant++;
        raise_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_EM_TX_ALLOC, handle, g.inflight_total);
        return ACL_BLOCKED;
    }
    if (!gate()) {
        return ACL_PARKED;
    }
    g.rom->em_write(em, &d[5], dlen);
    if (!gate()) {
        return ACL_PARKED;
    }
    uint8_t *m = g.rom->msg_alloc(BT_ROM_MSG_ACL_TX, task, BT_ROM_TASK_HCI, (uint16_t)sizeof(bt_rom_acl_desc_t));
    if (m == NULL) {
        g.st.cmd_alloc_refused++;
        if (gate()) {
            g.rom->em_rx_free(em);           /* unused TX buffer goes back through the routing free */
        }
        return ACL_BLOCKED;
    }
    bt_rom_acl_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.conhdl = handle;
    desc.pb_bc_flag = (uint8_t)(hdr >> 12);
    desc.length = dlen;
    desc.buf_ptr = em;
    memcpy(m, &desc, sizeof(desc));
    if (!gate()) {
        return ACL_PARKED;
    }
    g.rom->msg_send(m);
    if (!hc_add(g.inflight, handle, 1u)) {
        g.st.invariant++;
        raise_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_INFLIGHT_TABLE, handle, g.inflight_total);
    }
    g.inflight_total++;
    g.st.acl_to_rom++;
    bt_ctrl_link_info_t *li = link_of(handle);
    if (li != NULL) {
        g.p.enter();
        li->acl_sent++;
        g.p.exit();
    }
    return ACL_DONE;
}

/* Pick the next TX entry: oldest eligible; order kept within ordinary commands and within ACL. */
static int tx_pick(void)
{
    int oc = -1, oa = -1, on = -1;
    g.p.enter();
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        const tx_slot_t *s = &g.tx[i];
        if (s->state != SLOT_READY) {
            continue;
        }
        if (s->data[0] == BT_H4_ACL) {
            if (oa < 0 || seq_before(s->seq, g.tx[oa].seq)) {
                oa = (int)i;
            }
        } else if (rd16(&s->data[1]) == BT_CTRL_OP_HOST_NUM_COMPLETED) {
            if (on < 0 || seq_before(s->seq, g.tx[on].seq)) {
                on = (int)i;
            }
        } else if (oc < 0 || seq_before(s->seq, g.tx[oc].seq)) {
            oc = (int)i;
        }
    }
    g.p.exit();
    bool cmd_ok = oc >= 0 && g.rom_cmd_credits > 0u && evt_reply_space() && !fwd_late_hold(rd16(&g.tx[oc].data[1]));
    bool acl_ok = oa >= 0 && !g.acl_blocked && (!g.rom_running || g.inflight_total < BT_CTRL_H2C_ACL_CREDITS);
    if (oa >= 0 && !acl_ok && !g.acl_blocked) {
        g.st.acl_tx_credit_blocked++;
    }
    int best = on;
    if (cmd_ok && (best < 0 || seq_before(g.tx[oc].seq, g.tx[best].seq))) {
        best = oc;
    }
    if (acl_ok && (best < 0 || seq_before(g.tx[oa].seq, g.tx[best].seq))) {
        best = oa;
    }
    return best;
}

static bool tx_drain(void)
{
    bool progress = false;
    g.acl_blocked = 0u;
    for (uint32_t guard = 0u; guard < 4u * BT_CTRL_TX_SLOTS; guard++) {
        int idx = tx_pick();
        if (idx < 0) {
            break;
        }
        if (g.tx[idx].data[0] == BT_H4_ACL) {
            int r = process_acl((uint32_t)idx);
            if (r == ACL_PARKED) {
                return progress;
            }
            if (r == ACL_BLOCKED) {
                g.acl_blocked = 1u;
                continue;
            }
        } else {
            process_cmd((uint32_t)idx);
            if (g.parked) {
                return progress;
            }
        }
        tx_release((uint32_t)idx);
        progress = true;
    }
    return progress;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* controller -> host dispatcher                                                                                  */
/* ------------------------------------------------------------------------------------------------------------- */
static bool emit_nocp(void)
{
    uint16_t hs[BT_CTRL_MAX_LINKS], cs[BT_CTRL_MAX_LINKS];
    uint8_t n = 0u;
    for (uint32_t i = 0u; i < BT_CTRL_MAX_LINKS; i++) {
        if (g.nocp_pend[i].count != 0u) {
            hs[n] = g.nocp_pend[i].handle;
            cs[n] = g.nocp_pend[i].count;
            n++;
        }
    }
    if (n == 0u || !evt_reply_space()) {
        return false;
    }
    memset(g.nocp_pend, 0, sizeof(g.nocp_pend));
    evt_push(g.scratch, bt_ctrl_evt_pack_nocp(g.scratch, n, hs, cs), g.rx_seq++);
    g.st.nocp_events++;
    return true;
}

static bool retry_deferred(void)
{
    bool progress = false;
    while (g.def_n != 0u && evt_general_space()) {
        deferred_t *d = &g.def[g.def_head];
        bt_ctrl_evt_info_t info;
        uint16_t len = bt_ctrl_evt_pack(d->id, d->param, d->len, g.scratch, &info);
        if (len != 0u) {
            evt_push(g.scratch, len, d->seq);
        }
        if (!gate()) {
            return progress;
        }
        g.rom->msg_free(d->param);
        g.def_head = (g.def_head + 1u) % BT_CTRL_EVT_DEFER;
        g.def_n--;
        progress = true;
    }
    return progress;
}

static bool held_is_stale(const held_acl_t *h)
{
    uint8_t bit = handle_bit(h->handle);
    if (bit == 0u) {
        return true;
    }
    bool stale = false;
    g.p.enter();
    if ((g.acl_min_valid & bit) != 0u && seq_before(h->seq, g.acl_min_seq[h->handle - 0x80u])) {
        stale = true;
    }
    g.p.exit();
    return stale;
}

/* Drop held ACL_RX whose Disconnection Complete or a newer Connection Complete has already crossed to the host, even
 * while C2H credits are exhausted (so a dead link neither pins EM RX buffers nor injects stale frames onto a reused
 * handle once credits return). Returns false if parked. */
static bool purge_stale_held(bool *progress)
{
    for (uint32_t i = 0u; i < g.held_n; ) {
        uint32_t idx = (g.held_head + i) % BT_CTRL_HELD_ACL;
        held_acl_t *h = &g.held[idx];
        if (!held_is_stale(h)) {
            i++;
            continue;
        }
        if (h->em != NULL) {
            if (!gate()) {
                return false;
            }
            uint8_t *em = h->em;
            h->em = NULL;
            g.rom->em_rx_free(em);
        }
        if (!gate()) {
            return false;
        }
        g.rom->msg_free(h->param);
        for (uint32_t k = i; k + 1u < g.held_n; k++) {
            g.held[(g.held_head + k) % BT_CTRL_HELD_ACL] = g.held[(g.held_head + k + 1u) % BT_CTRL_HELD_ACL];
        }
        g.held_n--;
        g.st.acl_rx_dropped_disc++;
        *progress = true;
    }
    return true;
}

static bool dispatch_acl(void)
{
    bool progress = false;
    if (!purge_stale_held(&progress)) {
        return progress;
    }
    while (g.held_n != 0u) {
        held_acl_t *h = &g.held[g.held_head];
        if (held_is_stale(h)) {
            if (!purge_stale_held(&progress)) {
                return progress;
            }
            continue;
        }
        /* Credit and ring space are checked BEFORE any fragment state changes (plan M1 RX dispatcher (2)). */
        if (g.c2h_fc && g.c2h_out_total >= g.c2h_credits) {
            g.st.acl_rx_credit_blocked++;
            break;
        }
        if (acl_used() >= BT_CTRL_ACL_RX_SLOTS) {
            break;
        }
        if (g.def_n != 0u && seq_before(g.def[g.def_head].seq, h->seq)) {
            break;                           /* an older event is still deferred: ACL never overtakes it */
        }
        uint16_t frag = (uint16_t)(h->len - h->off);
        if (frag > g.c2h_len) {
            frag = g.c2h_len;
        }
        acl_slot_t *s = &g.acl[g.acl_head % BT_CTRL_ACL_RX_SLOTS];
        if (!gate()) {
            return progress;
        }
        g.rom->em_read(&s->data[5], h->em + h->off, frag);
        uint8_t pb;
        if (h->off != 0u) {
            pb = 0x1u;                       /* our own continuation fragment */
        } else if (h->pb == 0x1u || h->pb == 0x2u) {
            pb = h->pb;
        } else {
            pb = 0x2u;                       /* host reassembly accepts only 10 (start) / 01 (contract §5.1) */
            g.st.acl_rx_pb_fixed++;
        }
        s->data[0] = BT_H4_ACL;
        wr16(&s->data[1], (uint16_t)(h->handle | ((uint16_t)pb << 12) | ((uint16_t)h->bc << 14)));
        wr16(&s->data[3], frag);
        s->len = (uint16_t)(5u + frag);
        if (g.c2h_fc) {
            if (!hc_add(g.c2h_out, h->handle, 1u)) {
                g.st.invariant++;
                raise_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_C2H_TABLE, h->handle, g.c2h_out_total);
            }
            g.c2h_out_total++;
        }
        h->off = (uint16_t)(h->off + frag);
        s->seq = h->seq;
        g.p.enter();
        s->epoch = g.acl_epoch;
        g.acl_head++;
        g.p.exit();
        g.st.acl_rx_frags++;
        g.need_dlv = 1u;
        progress = true;
        if (h->off >= h->len) {
            if (!gate()) {
                return progress;
            }
            g.rom->em_rx_free(h->em);
            if (!gate()) {
                return progress;
            }
            g.rom->msg_free(h->param);
            g.held_head = (g.held_head + 1u) % BT_CTRL_HELD_ACL;
            g.held_n--;
        }
    }
    return progress;
}

/* Order: NOCP and deferred events first (events are never gated by ACL credits), then ACL. */
static bool rx_dispatch(void)
{
    bool progress = false;
    progress |= emit_nocp();
    progress |= retry_deferred();
    if (g.parked) {
        return progress;
    }
    progress |= dispatch_acl();
    return progress;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* IRQ 26 bottom half + ROM kernel                                                                                */
/* ------------------------------------------------------------------------------------------------------------- */
/* The top half has already acked INTSTAT (BT_ROM_INTERFACE §7), so "loop until the status register reads clear"
 * (plan M1) is a loop until the latched software pending word reads clear. Bounded; leftovers make run() report
 * more work. */
static bool bottom_half(bool *more)
{
    uint32_t loops = 0u;
    for (;;) {
        if (loops >= BT_CTRL_BH_MAX_LOOPS) {
            g.st.bh_budget_exhausted++;
            *more = true;
            break;
        }
        if (!gate()) {
            return false;
        }
        uint32_t bits = g.rom->irq_take();
        if (bits == 0u) {
            break;
        }
        for (uint32_t b = 0u; b < 32u; b++) {
            if (bits & (1u << b)) {
                g.st.bh_serviced[b]++;
            }
        }
        if (!gate()) {
            return false;
        }
        uint32_t un = g.rom->irq_service(bits);
        for (uint32_t b = 0u; b < 32u; b++) {
            if (un & (1u << b)) {
                g.st.bh_unhandled[b]++;
            }
        }
        loops++;
    }
    g.st.bh_loops += loops;
    if (loops > g.st.bh_loops_max) {
        g.st.bh_loops_max = loops;
    }
    return true;
}

/* Returns true if kernel events remain after the budget. */
static bool kernel_run(void)
{
    for (uint32_t k = 0u; k < BT_CTRL_RUN_CALLS; k++) {
        if (!gate()) {
            return false;
        }
        if (g.rom->run(BT_CTRL_RUN_STEPS) == 0u) {
            return false;
        }
    }
    return true;
}

static void do_stop(void)
{
    int result = BT_CTRL_STOP_OK;
    if (!release_rom_objects(&g.st.evt_dropped_stop)) {
        result = BT_CTRL_STOP_PARKED;
    } else if (g.rom_running) {
        if (!gate()) {
            result = BT_CTRL_STOP_PARKED;
        } else {
            int s = g.rom->stop();           /* reset completion checked (plan M1 teardown) */
            g.rom_running = 0u;
            if (s != 0) {
                result = BT_CTRL_STOP_ERR;
                g.st.reset_failures++;
            }
        }
    }
    if (result != BT_CTRL_STOP_PARKED) {
        if (gate()) {
            g.rom->irq_enable(0);            /* IRQ 26 masked */
        } else {
            result = BT_CTRL_STOP_PARKED;
        }
    }
    /* A ROM fault raised on the teardown path (on_fault during a release or inside stop()) belongs to this session:
     * it fails the stop result instead of being posted at the first run of the next session. */
    if (g.pending_rom_fault != BT_CTRL_FAULT_NONE) {
        g.pending_rom_fault = BT_CTRL_FAULT_NONE;
        g.st.stop_faults++;
        if (result == BT_CTRL_STOP_OK) {
            result = BT_CTRL_STOP_ERR;
        }
    }
    g.rom_running = 0u;
    g.rom_gen++;
    g.up.ctx = (void *)(uintptr_t)g.rom_gen;
    controller_state_reset();
    rings_reset();                           /* rings freed */
    g.need_dlv = 0u;                         /* nothing left to deliver; bt_dlv is closed */
    g.p.enter();
    g.state = ST_OFF;
    g.stop_req = 0u;
    g.stop_result = result;
    g.p.exit();
}

uint32_t bt_ctrl_run(void)
{
    if (!g.inited) {
        return 0u;
    }
    /* need_dlv is NOT cleared here: it is cleared only when wake_dlv() is actually called (end of this function), so
     * a wake withheld because the ROM lock was held is not lost. */
    if (g.state == ST_STOPPING) {
        do_stop();                           /* gate() inside do_stop() skips all ROM calls if g.parked -> STOP_PARKED */
        return 0u;
    }
    if (g.parked || g.state != ST_OPEN) {
        return 0u;
    }
    bool more = false;
    uint32_t pass;
    for (pass = 0u; pass < BT_CTRL_PASS_MAX; pass++) {
        bool progress = false;
        bool events_left = false;
        g.upcall_activity = 0u;
        if (g.rom_running) {
            bool bh_more = false;
            if (!bottom_half(&bh_more)) {
                return 0u;
            }
            /* Slots bt_dlv freed since the last pass go to the oldest deferred events BEFORE the kernel can upcall
             * new ones: otherwise a new event finds the defer list still full and overflows (fault) although a
             * slot is free. */
            progress |= retry_deferred();
            if (g.parked) {
                return 0u;
            }
            events_left = kernel_run();
            if (g.parked) {
                return 0u;
            }
            progress |= bh_more;
            if (g.fwd_orphan && g.fwd_pending && !events_left) {
                fwd_orphan_reply();
                progress = true;
            }
            if (g.fwd_pending && !events_left) {
                fwd_deadline_check();
                progress |= !g.fwd_pending;
            }
        }
        progress |= rx_dispatch();
        if (g.parked) {
            return 0u;
        }
        progress |= tx_drain();
        if (g.parked) {
            return 0u;
        }
        progress |= (g.upcall_activity != 0u) || events_left;
        if (!progress) {
            break;
        }
    }
    if (pass == BT_CTRL_PASS_MAX) {
        more = true;
    }
    /* Plan §3 rule (e): no FreeRTOS call while the ROM lock (PRIMASK) is held. Checked BEFORE anything is posted: a
     * lock leaked by a ROM path is itself an invariant violation, recorded (not posted) together with any pending
     * fault and the pending bt_dlv wake; both are delivered by the first run that finds the lock free. */
    if (g.rom->lock_depth != NULL && g.rom->lock_depth() != 0u) {
        g.st.lock_violations++;
        g.st.invariant++;
        defer_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_LOCK_RUN, 0u, g.rom->lock_depth());
        return more ? 1u : 0u;
    }
    if (g.pending_rom_fault != BT_CTRL_FAULT_NONE) {
        /* code, site and values taken together: up_on_fault (ROM heap paths) may overwrite them at any time */
        g.p.enter();
        uint32_t code = g.pending_rom_fault, site = g.pending_site, a = g.pending_a, b = g.pending_b;
        g.pending_rom_fault = BT_CTRL_FAULT_NONE;
        g.p.exit();
        raise_fault(code, site, a, b);
    }
    if (g.need_dlv) {
        g.need_dlv = 0u;
        g.p.wake_dlv();
    }
    return more ? 1u : 0u;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* HCI driver ops (hciT)                                                                                          */
/* ------------------------------------------------------------------------------------------------------------- */
bool bt_ctrl_check_send_available(void)
{
    if (!g.inited) {
        return false;
    }
    g.p.enter();
    bool ok = g.admission_open && tx_free_locked() != 0u;
    g.p.exit();
    return ok;
}

static bool h4_tx_valid(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len < 1u || len > BT_CTRL_TX_SLOT_BYTES) {
        return false;
    }
    if (data[0] == BT_H4_CMD) {
        return len >= 4u && (uint32_t)data[3] == (uint32_t)len - 4u;
    }
    if (data[0] == BT_H4_ACL) {
        if (len < 5u) {
            return false;
        }
        uint16_t dlen = rd16(&data[3]);
        return dlen == len - 5u && dlen <= BT_CTRL_H2C_ACL_LEN;
    }
    return false;                            /* 0x03 SCO is not compiled in; anything else is a host bug */
}

void bt_ctrl_send(uint8_t *data, uint16_t len)
{
    if (!g.inited) {
        return;
    }
    if (!h4_tx_valid(data, len)) {
        g.p.enter();
        g.st.tx_malformed++;
        g.p.exit();
        raise_fault(BT_CTRL_FAULT_HOST_CONTRACT, BT_CTRL_SITE_HOST_MALFORMED, len,
                    (data != NULL && len != 0u) ? data[0] : 0u);
        return;
    }
    g.p.enter();
    if (!g.admission_open) {
        g.st.tx_dropped_closed++;            /* admission closed between check and send: dropped, not a violation */
        g.p.exit();
        return;
    }
    int idx = -1;
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        if (g.tx[i].state == SLOT_FREE) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        g.st.tx_contract_violation++;        /* send() without a true check: the driver contract forbids it */
        g.p.exit();
        raise_fault(BT_CTRL_FAULT_HOST_CONTRACT, BT_CTRL_SITE_HOST_FULL, len, data[0]);
        return;
    }
    tx_slot_t *s = &g.tx[idx];
    s->state = SLOT_FILLING;
    uint32_t sess = g.session;
    g.p.exit();
    memcpy(s->data, data, len);              /* the buffer is borrowed: copy before returning (contract §3.3) */
    s->len = len;
    g.p.enter();
    if (g.admission_open && g.session == sess) {
        s->seq = g.tx_seq++;
        s->state = SLOT_READY;
        if (data[0] == BT_H4_ACL) {
            g.st.tx_acl++;
        }
    } else {
        s->state = SLOT_FREE;
        g.st.tx_dropped_closed++;
    }
    g.p.exit();
    g.p.wake_ctrl();
}

int bt_ctrl_register_host_callback(const bt_ctrl_host_cb_t *cb)
{
    g.p.enter();
    g.host = cb;
    g.p.exit();
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* bt_dlv                                                                                                         */
/* ------------------------------------------------------------------------------------------------------------- */
int bt_ctrl_dlv_run(void)
{
    if (!g.inited) {
        return BT_CTRL_DLV_IDLE;
    }
    for (;;) {
        g.p.enter();
        const bt_ctrl_host_cb_t *host = g.host;
        if (!g.dlv_open || host == NULL) {
            g.p.exit();
            return BT_CTRL_DLV_IDLE;
        }
        uint32_t sess = g.session;
        if (g.send_avail_pending) {
            g.send_avail_pending = 0u;
            g.dlv_busy = 1u;
            g.st.dlv_send_avail++;
            g.p.exit();
            host->notify_host_send_available();   /* may block on hciT's work queue: bt_dlv only (contract §4) */
            g.p.enter();
            g.dlv_busy = 0u;
            g.p.exit();
            continue;
        }
        uint8_t *data = NULL;
        uint16_t len = 0u;
        int which = 0;
        evt_slot_t *es = g.evt_head != g.evt_tail ? &g.evt[g.evt_tail % BT_CTRL_EVT_SLOTS] : NULL;
        acl_slot_t *as = g.acl_head != g.acl_tail ? &g.acl[g.acl_tail % BT_CTRL_ACL_RX_SLOTS] : NULL;
        if (as != NULL && as->epoch != g.acl_epoch) {
            g.acl_tail++;                        /* queued before an HCI Reset: dropped */
            g.st.dlv_stale++;
            g.p.exit();
            continue;
        }
        /* Arrival order across both rings (see the file header): the ACL head goes first only if no queued event is
         * older (a reserved reply may sit ahead of an older, previously deferred event in the event ring, so the
         * whole ring is checked, not just its head). Every ACL entry here already holds a host credit, so an event
         * never waits on the credit gate. */
        bool acl_first = as != NULL;
        for (uint32_t i = g.evt_tail; acl_first && i != g.evt_head; i++) {
            acl_first = !seq_before(g.evt[i % BT_CTRL_EVT_SLOTS].seq, as->seq);
        }
        if (acl_first) {
            data = as->data;
            len = as->len;
            which = 2;
        } else if (es != NULL) {
            data = es->data;
            len = es->len;
            which = 1;
        }
        if (which == 0) {
            g.p.exit();
            return BT_CTRL_DLV_IDLE;
        }
        if (which == 1 && len >= 6u && (data[1] == BT_HCI_EVT_DISC || data[1] == BT_HCI_EVT_CONN_CMP) &&
            data[3] == BT_HCI_OK) {
            uint16_t eh = (uint16_t)(rd16(&data[4]) & 0x0FFFu);
            uint8_t ebit = handle_bit(eh);
            if (ebit != 0u) {
                g.acl_min_seq[eh - 0x80u] = es->seq;
                g.acl_min_valid |= ebit;
            }
        }
        g.dlv_busy = 1u;
        g.p.exit();
        int r = host->notify_host_recv(data, len);     /* no lock held across the callback */
        bool fault = false;
        g.p.enter();
        g.dlv_busy = 0u;
        if (g.session != sess || !g.dlv_open) {
            g.p.exit();
            return BT_CTRL_DLV_IDLE;             /* torn down meanwhile: indices belong to the next session */
        }
        if (r != 0) {
            g.st.dlv_fail++;
            g.dlv_fails++;
            if (g.dlv_fails >= BT_CTRL_DLV_FAULT_AFTER && !g.dlv_faulted) {
                g.dlv_faulted = 1u;
                fault = true;
            }
            g.p.exit();
            if (fault) {
                /* posted to the app; bt_dlv keeps delivering (plan M1) */
                raise_fault(BT_CTRL_FAULT_DLV, BT_CTRL_SITE_DLV, BT_CTRL_DLV_FAULT_AFTER, 0u);
            }
            return BT_CTRL_DLV_RETRY;            /* entry kept */
        }
        g.dlv_fails = 0u;
        if (which == 1) {
            g.evt_tail++;
            g.st.dlv_evt++;
        } else {
            g.acl_tail++;
            g.st.dlv_acl++;
        }
        g.p.exit();
        g.p.wake_ctrl();                         /* a slot freed: reservations / deferred events / held ACL */
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* lifecycle                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------- */
int bt_ctrl_init(const bt_rom_if_t *rom, const bt_ctrl_platform_t *platform, const uint8_t bd_addr[6])
{
    memset(&g, 0, sizeof(g));
    memset(&lv, 0, sizeof(lv));
    g.rom = rom;
    g.p = *platform;
    memcpy(g.bd_addr, bd_addr, 6u);
    g.up.on_hci_msg = up_on_hci_msg;
    g.up.on_irq = up_on_irq;
    g.up.on_fault = up_on_fault;
    g.session = 1u;
    g.rom_gen = 1u;
    g.up.ctx = (void *)(uintptr_t)g.rom_gen;
    g.state = ST_OFF;
    g.stop_result = BT_CTRL_STOP_OK;
    controller_state_reset();
    int err = bt_ctrl_cmds_init();
    /* Every ROM reply id in the table (effective FWD rows and gated rows alike) must be packed back to the same
     * opcode, so a gate that passes cannot leave a reply unpacked. */
    uint32_t n = 0u;
    const bt_ctrl_cmd_row_t *rows = bt_ctrl_cmds_table(&n);
    for (uint32_t i = 0u; i < n; i++) {
        if ((bt_ctrl_cmds_eff(&rows[i]) == BTC_FWD || rows[i].rom_rsp != 0u) &&
            bt_ctrl_evt_reply_opcode(rows[i].rom_rsp) != rows[i].op) {
            err++;
        }
    }
    g.inited = 1u;
    if (err != 0) {
        g.st.invariant++;
        raise_fault(BT_CTRL_FAULT_INVARIANT, BT_CTRL_SITE_INIT_TABLE, (uint32_t)err, 0u);
    }
    return err;
}

void bt_ctrl_set_clock(uint32_t (*now_ms)(void))
{
    g.now_ms = now_ms;
}

int bt_ctrl_open(void)
{
    if (!g.inited) {
        return -1;
    }
    g.p.enter();
    if (g.state != ST_OFF || g.dlv_busy) {
        g.p.exit();
        return -1;
    }
    g.session++;
    for (uint32_t i = 0u; i < BT_CTRL_TX_SLOTS; i++) {
        if (g.tx[i].state == SLOT_READY) {
            g.tx[i].state = SLOT_FREE;
        }
    }
    g.evt_head = g.evt_tail = 0u;
    g.acl_head = g.acl_tail = 0u;
    g.send_avail_pending = 0u;
    g.dlv_fails = 0u;
    g.dlv_faulted = 0u;
    g.fault_raised = 0u;
    g.pending_rom_fault = BT_CTRL_FAULT_NONE;   /* a new session never inherits a fault of the previous one */
    g.note_head = g.note_n = 0u;
    g.peer_name_ok = 0u;
    g.need_dlv = 0u;
    g.admission_open = 1u;
    g.dlv_open = 1u;
    g.state = ST_OPEN;
    g.stop_req = 0u;
    g.stop_result = BT_CTRL_STOP_OK;
    g.p.exit();
    return 0;
}

void bt_ctrl_shutdown_begin(void)
{
    if (!g.inited) {
        return;
    }
    g.p.enter();
    if (g.state != ST_OPEN) {
        g.p.exit();
        return;
    }
    g.session++;                 /* callbacks quiesced: late deliveries rejected */
    g.dlv_open = 0u;
    g.send_avail_pending = 0u;
    g.admission_open = 0u;       /* admission closed */
    g.stop_req = 1u;
    g.state = ST_STOPPING;
    g.stop_result = BT_CTRL_STOP_PENDING;
    g.p.exit();
    g.p.wake_ctrl();             /* controller stop / IRQ mask / ring reset happen in bt_ctrl context */
}

int bt_ctrl_shutdown_status(void)
{
    g.p.enter();
    int r = g.stop_result;
    g.p.exit();
    return r;
}

bool bt_ctrl_dlv_busy(void)
{
    g.p.enter();
    bool b = g.dlv_busy != 0u;
    g.p.exit();
    return b;
}

void bt_ctrl_get_stats(bt_ctrl_stats_t *out)
{
    g.p.enter();
    *out = g.st;
    g.p.exit();
}

void bt_ctrl_get_debug(bt_ctrl_debug_t *out)
{
    memset(out, 0, sizeof(*out));
    g.p.enter();
    out->state = g.state;
    out->admission_open = g.admission_open;
    out->dlv_open = g.dlv_open;
    out->rom_running = g.rom_running;
    out->parked = g.parked;
    out->c2h_fc = g.c2h_fc;
    out->rom_cmd_credits = g.rom_cmd_credits;
    out->fwd_pending = g.fwd_pending;
    out->session = g.session;
    out->tx_free = tx_free_locked();
    out->evt_used = g.evt_head - g.evt_tail;
    out->evt_resv = g.evt_resv;
    out->evt_deferred = g.def_n;
    out->acl_used = g.acl_head - g.acl_tail;
    out->held_acl = g.held_n;
    out->h2c_inflight = g.inflight_total;
    out->c2h_outstanding = g.c2h_out_total;
    out->c2h_credits = g.c2h_credits;
    out->c2h_len = g.c2h_len;
    uint32_t np = 0u;
    for (uint32_t i = 0u; i < BT_CTRL_MAX_LINKS; i++) {
        np += g.nocp_pend[i].count;
    }
    out->nocp_pending = np;
    g.p.exit();
}

int bt_ctrl_note_pop(bt_ctrl_note_t *out)
{
    int got = 0;
    g.p.enter();
    if (g.note_n != 0u) {
        *out = g.notes[g.note_head];
        g.note_head = (g.note_head + 1u) % BT_CTRL_NOTES;
        g.note_n--;
        got = 1;
    }
    g.p.exit();
    return got;
}

int bt_ctrl_peer_name(uint8_t bda[6], char *name, uint16_t cap)
{
    int got = 0;
    if (bda == NULL || name == NULL || cap == 0u) {
        return 0;
    }
    g.p.enter();
    if (g.peer_name_ok) {
        memcpy(bda, g.peer_bda, 6u);
        uint16_t i = 0u;
        for (; i + 1u < cap && g.peer_name[i] != '\0'; i++) {
            name[i] = g.peer_name[i];
        }
        name[i] = '\0';
        got = 1;
    }
    g.p.exit();
    return got;
}

int bt_ctrl_link_info(uint16_t h, bt_ctrl_link_info_t *out)
{
    const bt_ctrl_link_info_t *l = link_of((uint16_t)(h & 0x0FFFu));
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (l == NULL) {
        return 0;
    }
    g.p.enter();
    *out = *l;
    g.p.exit();
    return 1;
}

uint8_t bt_ctrl_scan_enable(void)
{
    g.p.enter();
    uint8_t v = lv.scan_enable;
    g.p.exit();
    return v;
}

int bt_ctrl_inquiry_active(void)
{
    g.p.enter();
    int v = lv.inq_active ? 1 : 0;
    g.p.exit();
    return v;
}

static uint16_t put_str(char *buf, uint16_t cap, uint16_t at, const char *s)
{
    while (*s != '\0' && at + 1u < cap) {
        buf[at++] = *s++;
    }
    buf[at] = '\0';
    return at;
}

static uint16_t put_hex(char *buf, uint16_t cap, uint16_t at, const char *label, uint32_t v, uint32_t digits)
{
    static const char k[] = "0123456789ABCDEF";
    at = put_str(buf, cap, at, label);
    at = put_str(buf, cap, at, "0x");
    for (uint32_t i = digits; i-- > 0u && at + 1u < cap;) {
        buf[at++] = k[(v >> (4u * i)) & 0xFu];
    }
    buf[at] = '\0';
    return at;
}

static uint16_t put_dec(char *buf, uint16_t cap, uint16_t at, const char *label, uint32_t v)
{
    char d[10];
    uint32_t i = 0u;
    do {
        d[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    at = put_str(buf, cap, at, label);
    while (i > 0u && at + 1u < cap) {
        buf[at++] = d[--i];
    }
    buf[at] = '\0';
    return at;
}

uint16_t bt_ctrl_fault_format(char *buf, uint16_t cap)
{
    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    uint32_t code = 0u, site = 0u, a = 0u, b = 0u;
    g.p.enter();
    if (g.fault_raised) {                    /* this session's: first_fault outlives the session that set it */
        code = g.st.first_fault;
        site = g.st.fault_site;
        a = g.st.fault_a;
        b = g.st.fault_b;
    }
    g.p.exit();
    uint16_t at = put_dec(buf, cap, 0u, "BT bt_ctrl fault, first code ", code);
    at = put_dec(buf, cap, at, " site ", site);
    at = put_hex(buf, cap, at, " a=", a, 8u);
    return put_hex(buf, cap, at, " b=", b, 8u);
}

uint16_t bt_ctrl_stats_format(char *buf, uint16_t cap)
{
    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    uint16_t at = put_dec(buf, cap, 0u, "BT ctrl stats unk=", s.rom_unknown_ids);
    at = put_dec(buf, cap, at, " short=", s.rom_short);
    at = put_dec(buf, cap, at, " late=", s.late_upcalls);
    at = put_dec(buf, cap, at, " unsol=", s.evt_unsolicited_reply);
    at = put_dec(buf, cap, at, " fwd_to=", s.fwd_deadline);
    at = put_dec(buf, cap, at, " late_reply=", s.fwd_late_reply);
    at = put_dec(buf, cap, at, " late_drop=", s.fwd_late_dropped);
    at = put_dec(buf, cap, at, " scan_refused=", s.scan_refused);
    return put_dec(buf, cap, at, " notes_lost=", s.notes_lost);
}

uint16_t bt_ctrl_note_format(const bt_ctrl_note_t *n, char *buf, uint16_t cap)
{
    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    uint16_t at = put_str(buf, cap, 0u, "BT HCI ");
    switch (n->kind) {
    case BT_CTRL_NOTE_CONN:
        at = put_hex(buf, cap, put_str(buf, cap, at, "connection complete"), " h=", n->handle, 4u);
        return put_hex(buf, cap, at, " st=", n->status, 2u);
    case BT_CTRL_NOTE_DISC:
        at = put_hex(buf, cap, put_str(buf, cap, at, "disconnection complete"), " h=", n->handle, 4u);
        at = put_hex(buf, cap, at, " st=", n->status, 2u);
        return put_hex(buf, cap, at, " reason=", n->reason, 2u);
    case BT_CTRL_NOTE_CONN_REQ:
        return put_str(buf, cap, at, "connection request");
    case BT_CTRL_NOTE_SCAN_REFUSED:
        at = put_hex(buf, cap, put_str(buf, cap, at, "scan enable"), " ", n->reason, 2u);
        return put_hex(buf, cap, at, " refused st=", n->status, 2u);
    case BT_CTRL_NOTE_DISC_CMD:
        at = put_hex(buf, cap, put_str(buf, cap, at, "disconnect cmd"), " h=", n->handle, 4u);
        at = put_hex(buf, cap, at, " st=", n->status, 2u);
        return put_str(buf, cap, at, n->local ? " local" : " rom");
    case BT_CTRL_NOTE_NAME_CMD:
        at = put_hex(buf, cap, put_str(buf, cap, at, "name request"), " st=", n->status, 2u);
        return put_str(buf, cap, at, n->local ? " local" : " rom");
    case BT_CTRL_NOTE_NAME:
        at = put_hex(buf, cap, put_str(buf, cap, at, "name complete"), " st=", n->status, 2u);
        return put_dec(buf, cap, at, " len=", n->reason);
    case BT_CTRL_NOTE_INQ_CMD:
        at = put_hex(buf, cap, put_str(buf, cap, at, "inquiry"), " st=", n->status, 2u);
        return put_str(buf, cap, at, n->local ? " local" : " rom");
    case BT_CTRL_NOTE_INQ_DONE:
        at = put_hex(buf, cap, put_str(buf, cap, at, "inquiry complete"), " st=", n->status, 2u);
        return put_dec(buf, cap, at, " results=", n->reason);
    case BT_CTRL_NOTE_PAGE_CMD:
        at = put_hex(buf, cap, put_str(buf, cap, at, "create connection"), " st=", n->status, 2u);
        return put_str(buf, cap, at, n->local ? " local" : " rom");
    case BT_CTRL_NOTE_ROLE:
        at = put_hex(buf, cap, put_str(buf, cap, at, "role change"), " h=", n->handle, 4u);
        at = put_hex(buf, cap, at, " st=", n->status, 2u);
        return put_str(buf, cap, at, n->reason == BT_CTRL_ROLE_MASTER ? " master" : " slave");
    case BT_CTRL_NOTE_TRACE:
        at = put_hex(buf, cap, put_str(buf, cap, at, "trace"), " ev=", n->reason, 2u);
        at = put_hex(buf, cap, at, " st=", n->status, 2u);
        return n->handle != 0u ? put_hex(buf, cap, at, " op=", n->handle, 4u) : at;
    default:
        return put_hex(buf, cap, put_str(buf, cap, at, "note"), " ", n->kind, 2u);
    }
}
