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
    bt_ctrl_stats_t st;
} g;

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

static void raise_fault(uint32_t code)
{
    bool post = false;
    g.p.enter();
    g.st.faults++;
    if (!g.fault_raised) {
        g.fault_raised = 1u;
        g.st.first_fault = code;
        post = true;
    }
    g.p.exit();
    if (post) {
        g.p.fault(code);
    }
}

/* Upcalls (inside rom->run()) only record a fault; bt_ctrl_run() posts it after the ROM call returned, so no
 * FreeRTOS call is made from ROM context (plan §3 rule (e)). */
static void defer_fault(uint32_t code)
{
    if (g.pending_rom_fault == BT_CTRL_FAULT_NONE) {
        g.pending_rom_fault = code;
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

static void evt_push(const uint8_t *h4, uint16_t len, uint32_t seq)
{
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

/* ------------------------------------------------------------------------------------------------------------- */
/* ROM upcalls                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------- */
#define BT_HCI_EVT_CONN_CMP 0x03u

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
    defer_fault(BT_CTRL_FAULT_EVT_OVERFLOW);
    return BT_ROM_MSG_CONSUMED;
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
        if (done != 0u && !hc_add(g.nocp_pend, h, done)) {
            g.st.invariant++;
            defer_fault(BT_CTRL_FAULT_INVARIANT);
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
    if (d.length == 0u || d.length > BT_ROM_EM_BUF_PAYLOAD || g.held_n >= BT_CTRL_HELD_ACL) {
        if (g.held_n >= BT_CTRL_HELD_ACL) {
            g.st.acl_rx_overflow++;
            g.st.invariant++;
            defer_fault(BT_CTRL_FAULT_INVARIANT);
        } else {
            g.st.acl_rx_bad++;
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
    h->handle = (uint16_t)(d.conhdl & 0x0FFFu);
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
        defer_fault(BT_CTRL_FAULT_INVARIANT);
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
        if (g.fwd_cls == BTC_M && info.status != BT_HCI_OK) {
            g.st.mandatory_fwd_fail++;       /* opcode table §2 rule 7: passed through, BT fault */
            g.st.invariant++;
            defer_fault(BT_CTRL_FAULT_INVARIANT);
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
    (void)code;
    g.st.rom_faults++;                       /* may run with the ROM lock held: flags only */
    g.pending_rom_fault = BT_CTRL_FAULT_ROM;   /* overrides a lesser pending code */
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
    g.held_head = g.held_n = 0u;
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
        if (!gate()) {
            return false;
        }
        g.rom->em_rx_free(h->em);
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
        raise_fault(BT_CTRL_FAULT_RESET);
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
    uint16_t alloc_len = plen > min_len ? plen : min_len;
    if (!gate()) {
        return 0xFFu;
    }
    uint8_t *m = g.rom->msg_alloc(row->rom_req, dest, BT_ROM_TASK_HCI, alloc_len);
    if (m == NULL) {
        g.st.cmd_alloc_refused++;            /* real impl: g_bt_heap budget check refused (ROM itself cannot fail) */
        return BT_HCI_ERR_MEM_FULL;
    }
    if (plen != 0u) {
        memcpy(m, prm, plen);                /* verified layouts only (fwd_min_len table) */
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
    fwd_local_reply(BT_HCI_ERR_LMP_TIMEOUT);
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
            return;
        }
        g.st.cmd_local_fail++;
        if (row->cls == BTC_M) {
            g.st.invariant++;                /* class M must not fail */
            raise_fault(BT_CTRL_FAULT_INVARIANT);
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
        raise_fault(BT_CTRL_FAULT_INVARIANT);
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
        raise_fault(BT_CTRL_FAULT_INVARIANT);
    }
    g.inflight_total++;
    g.st.acl_to_rom++;
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
    bool cmd_ok = oc >= 0 && g.rom_cmd_credits > 0u && evt_reply_space();
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

static bool dispatch_acl(void)
{
    bool progress = false;
    while (g.held_n != 0u) {
        held_acl_t *h = &g.held[g.held_head];
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
                raise_fault(BT_CTRL_FAULT_INVARIANT);
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
    if (!g.inited || g.parked) {
        return 0u;
    }
    /* need_dlv is NOT cleared here: it is cleared only when wake_dlv() is actually called (end of this function), so
     * a wake withheld because the ROM lock was held is not lost. */
    if (g.state == ST_STOPPING) {
        do_stop();
        return 0u;
    }
    if (g.state != ST_OPEN) {
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
        defer_fault(BT_CTRL_FAULT_INVARIANT);
        return more ? 1u : 0u;
    }
    if (g.pending_rom_fault != BT_CTRL_FAULT_NONE) {
        uint32_t code = g.pending_rom_fault;
        g.pending_rom_fault = BT_CTRL_FAULT_NONE;
        raise_fault(code);
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
        raise_fault(BT_CTRL_FAULT_HOST_CONTRACT);
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
        raise_fault(BT_CTRL_FAULT_HOST_CONTRACT);
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
                raise_fault(BT_CTRL_FAULT_DLV);  /* posted to the app; bt_dlv keeps delivering (plan M1) */
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
        raise_fault(BT_CTRL_FAULT_INVARIANT);
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
