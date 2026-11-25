/* bt_xrec_bluedroid: phone-mode XREC session over Bluedroid SPP for BT=1 (see bt_xrec_bluedroid.h).
 *
 * Legacy mapping (F: = src/bt_hci.c, S: = src/bt_spp.c, line numbers of this tree; the legacy path drives the same ROM
 * on silicon today and is the reference for names, ids and behaviour):
 *   - Device name "Cassini_" + hex(bd_addr[0]) (F:2005-2012); BD_ADDR from eFuse (F:1990-2003, without the legacy
 *     timer-derived fallback: plan M1 "never timer-derived"; see bt_xrec_plat_identity).
 *   - Class of device 0x200404 (F:1550), SPP server on RFCOMM channel 1 named "Cassini" with an SDP record for
 *     UUID 0x1101 (S:341-353), so tools/bt_sync.py (channel 1) and the Android app (SPP UUID) find it unchanged.
 *   - Connectable always while ready, discoverable only while the pairing window is open (F:1559, F:2092-2095,
 *     F:2113-2118), discoverability closed on a successful XREC PAIR (F:1807-1810).
 *   - XREC PIN for the window (F:2079-2089), now from the DRBG (plan M2: fails closed, no timer fallback).
 *   - Deferred reboot / OTA / DFU after 250 ms once the reply left (F:1823-1835, F:1896-1901, F:2138-2148), now
 *     through bt_shutdown() first (plan M4).
 *   - bt_xrec_stop (F:2151-2176) = bt_shutdown(): graceful Bluedroid/bt_ctrl teardown with the M1 recovery ladder.
 *   - bt_xrec_get_status (F:2178-2197): same fields, equivalent meaning (see bt_xrec_get_status below).
 * Deliberate differences (plan M4 pairing policy): IO capability DisplayYesNo instead of NoInputNoOutput (F:1674),
 * numeric comparison accepted only inside the pairing window and only after the local user confirmed it on the BT
 * screen (ENTER; BACK or 30 s rejects), legacy PIN pairing rejected instead of answering "0000" (F:1636-1647), Just
 * Works / passkey bonds removed, XREC traffic only on MITM-authenticated, encrypted links.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "openfw.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "bt_xrec_bluedroid.h"

#if defined(OPENFW_BT_BENCH_IMAGE) && OPENFW_BT_BENCH_IMAGE
#define BENCH_IMAGE 1   /* make BT=1 BENCH=1: SD:/BTBENCH.RUN overrides compiled in (bt_bench.h) */
#else
#define BENCH_IMAGE 0   /* release BT=1 image: no autoconfirm / fixed-PIN code at all (review finding) */
#endif

_Static_assert((BT_XREC_RX_RING & (BT_XREC_RX_RING - 1u)) == 0u, "RX ring size must be a power of two");
_Static_assert((BT_XREC_TX_RING & (BT_XREC_TX_RING - 1u)) == 0u, "TX ring size must be a power of two");
_Static_assert(BT_XREC_TX_RING >= 12u + XREC_MAX_CHUNK, "one GETS frame must fit the TX ring (send is all-or-nothing)");
_Static_assert(BT_XREC_SPP_TX_BUF >= ESP_SPP_MIN_TX_BUFFER_SIZE && BT_XREC_SPP_TX_BUF <= ESP_SPP_MAX_TX_BUFFER_SIZE,
               "esp_spp_cfg_t.tx_buffer_size range");
_Static_assert(BT_XREC_WRITE_MAX <= BT_XREC_SPP_TX_BUF, "a write must fit BTC's ring");

/* ---- event records (BTC -> app). DATA and WRITE coalesce with the newest record of the same kind and handle. ---- */
enum {
    EV_DATA = 1, EV_WRITE, EV_CONG, EV_SPP_INIT, EV_SPP_START, EV_SRV_OPEN, EV_CLOSE, EV_RX_OVERFLOW,
    EV_CFM_REQ, EV_PIN_REQ, EV_KEY_REQ, EV_KEY_NOTIF, EV_AUTH_CMPL, EV_ACL_CONN, EV_ACL_DISC,
};

typedef struct {
    uint8_t  type;
    uint8_t  status;
    uint8_t  extra;        /* START: scn; AUTH_CMPL: link key type; CONG: cong */
    uint8_t  bda[6];
    uint32_t handle;
    uint32_t val;          /* DATA/WRITE: bytes; CFM_REQ: number; SRV_OPEN: peer MTU */
} ev_t;

typedef struct {
    xrec_ctx_t xrec;
    uint8_t    rx[BT_XREC_RX_RING];
    uint8_t    tx[BT_XREC_TX_RING];
} bufs_t;

typedef struct {
    uint8_t  valid;
    uint8_t  bda[6];
} bda_slot_t;

static struct {
    /* ---- shared with the BTC callbacks: only under bt_xrec_plat_lock() ---- */
    uint8_t  cb_open;          /* callbacks may touch b->rx and the queue */
    uint8_t  cb_rx_bound, cb_rx_closing, cb_rx_overflow;
    uint32_t cb_rx_handle;
    uint8_t  evq_lost;
    ev_t     evq[BT_XREC_EVQ_SLOTS];
    uint32_t ev_head, ev_tail; /* free-running */
    uint32_t rx_head, rx_tail; /* free-running byte counters into b->rx */
    uint32_t rx_frames;        /* ESP_SPP_DATA_IND_EVT count (status rx_acl_pkts) */
    uint32_t rx_overflows, rx_dropped_cb, evq_lost_n;
    bufs_t  *b;
    uint8_t  nm_valid;         /* last remote name Bluedroid reported (READ_REMOTE_NAME / AUTH_CMPL), for the UI */
    uint8_t  nm_bda[6];
    char     nm[BT_XREC_PEER_NAME_MAX];

    /* ---- app task only ---- */
    uint8_t  state;
    uint32_t t_state;
    uint8_t  bd[6];
    char     name[16];
    uint8_t  spp_inited;
    uint8_t  window_open, xrec_pin_issued;
    uint8_t  ssp_window;               /* numeric comparison accepted + discoverable: closes at the first MITM bond */
    uint8_t  ssp_deferred;             /* window opened before READY: the M2.3 gate is evaluated once READY */
    uint32_t window_deadline;
    uint8_t  scan_disc, scan_applied;      /* scan_applied: 0 none, 1 connectable, 2 connectable+discoverable */
    uint8_t  scan_refused;                 /* the controller refused a scan mode write (> 1 ACL link, ROM 0C1A
                                              rule, docs/BT_ROM_HCI_MAP.md §11): sent again at scan_retry_at, at
                                              once when a link goes or when the wanted mode changes */
    uint8_t  scan_failsafe;                /* a refused write dropped discoverability: scan off (0x00) first */
    uint8_t  scan_disc_sent;               /* a discoverable mode was sent since the last fail-safe write */
    uint8_t  scan_want_sent;               /* the mode last sent (1 / 2); scan_applied is cleared by a fail-safe */
    uint32_t scan_retry_at;
    uint8_t  ssp_showing;
    char     ssp_digits[8];
    uint32_t ssp_deadline;
    bda_slot_t cfm_ok, auth_ok, reject;    /* accepted numeric comparison, accepted MITM bond, policy reject */
    uint32_t acl_links;
    /* ACL handles from the GAP link events; closing = the host sent 0406 for it (bt_ctrl DISC_CMD note). Silicon
     * session 6: after a rejected pairing BTM disconnected the link but the screen kept "connected": a link the host is
     * ending is not shown as connected even while its Disconnection Complete is late or missing (BT_ROM_HCI_MAP §11). */
    struct {
        uint16_t h;
        uint8_t  used, closing;
    } links[4];
    /* a 0406 note for a handle whose GAP link-up is still queued (notes drain first): applied by link_up(); cleared by
     * the handle's next Connection Complete note (a new link) or its link-down */
    uint16_t disc_early_h;
    uint8_t  disc_early;
    bda_slot_t acl_peer;                   /* newest ACL peer (UI) */
    bda_slot_t name_asked;                 /* esp_bt_gap_read_remote_name() already sent for this peer */
    /* local numeric-comparison prompt (plan M4) */
    uint8_t  cfm_pending;
    uint8_t  cfm_bda[6];
    char     cfm_digits[8];
    uint32_t cfm_val, cfm_t0;
    uint8_t  cfm_named;                    /* the prompt peer's name became known (logged once per prompt) */
    uint8_t  cfm_ended;                    /* a prompt ended (answered, rejected, withdrawn) at cfm_end_t */
    uint32_t cfm_end_t;
    /* A pairing this session accepted (cfm_ok) that then failed: the peer saw the result of the DHKey check, which the
     * ROM computes with this session's P-192 key and a peer key it never validates (BT_ROM_INTERFACE.md §2.3, invalid-
     * curve class). No further SSP until the next BT-mode entry, which draws a new key: one such query per key. */
    uint8_t  ssp_burned;
    /* connection */
    uint8_t  conn_open, conn_authorized, conn_closing, cong, entropy_fail, xrec_reset_pending;
    uint8_t  close_retry, close_tries;   /* review STEP10 C11: esp_spp_disconnect() refused, retried at close_retry_at */
    uint32_t close_retry_at;
    uint32_t conn_handle;
    uint8_t  conn_bda[6];
    uint16_t mtu;
    uint32_t tx_head, tx_tail, inflight;
    /* deferred reboot */
    uint8_t  reboot_mode;
    uint32_t reboot_at;
    uint32_t last_fault;
    bt_xrec_debug_t d;          /* counters only (the rest is filled by bt_xrec_get_debug) */
} s;

static volatile uint8_t s_spp_uninit_seen;
#if BENCH_IMAGE
static volatile uint8_t s_bench_autoconfirm;   /* BENCH ONLY (bt_xrec_set_bench_autoconfirm); outside s: survives start */
static char s_bench_pin[8];                     /* BENCH ONLY (bt_xrec_set_bench_xrec_pin): "" = DRBG PIN; app task */
#endif
static volatile uint8_t s_spp_init_posted;

#if defined(BT_XREC_TEST_HOOKS) && BT_XREC_TEST_HOOKS
/* Host tests only: an in-flight window above BTC's ring, so esp_spp_write() reaches ESP_ERR_NO_MEM against the real
 * btc_spp ring and the CONG_EVT(false) resume runs end to end (test_bt_host phone, step P5b). */
static volatile uint32_t s_inflight_max = BT_XREC_INFLIGHT_MAX;
#define INFLIGHT_MAX s_inflight_max
void bt_xrec_test_set_inflight_max(uint32_t n);
void bt_xrec_test_set_inflight_max(uint32_t n) { s_inflight_max = n != 0u ? n : BT_XREC_INFLIGHT_MAX; }
#else
#define INFLIGHT_MAX BT_XREC_INFLIGHT_MAX
#endif

/* ============================================ helpers ========================================================== */
static bool bda_eq(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6u) == 0; }

static bool slot_is(const bda_slot_t *sl, const uint8_t bda[6]) { return sl->valid && bda_eq(sl->bda, bda); }

static void slot_set(bda_slot_t *sl, const uint8_t bda[6])
{
    sl->valid = 1u;
    memcpy(sl->bda, bda, 6u);
}

static void wipe_mem(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0u; i < n; i++) {
        v[i] = 0u;
    }
}

static bool time_reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

static void set_state(uint8_t st)
{
    s.state = st;
    s.t_state = bt_xrec_plat_now_ms();
}

/* "<prefix><reason>" as one log line (plan M2.3 gate messages). */
static void log_gate(const char *prefix, const char *why)
{
    char line[128];
    size_t n = 0u;
    for (const char *p = prefix; *p != '\0' && n + 1u < sizeof(line); p++) {
        line[n++] = *p;
    }
    for (const char *p = why; *p != '\0' && n + 1u < sizeof(line); p++) {
        line[n++] = *p;
    }
    line[n] = '\0';
    bt_xrec_plat_log(line);
}

/* Plan M2.3 gate plus the session's own refusal (ssp_burned). NULL = an SSP pairing may be offered or accepted. */
static const char *ssp_block(void)
{
    if (s.ssp_burned) {
        return "a failed pairing used this session's P-192 key (re-enter BT mode)";
    }
    return bt_xrec_plat_pairing_block();
}

/* An accepted numeric comparison whose pairing then failed or vanished (see s.ssp_burned). */
static void ssp_burn(void)
{
    if (!s.ssp_burned) {
        s.ssp_burned = 1u;
        bt_xrec_plat_log("BT SSP key spent: an accepted pairing failed; no SSP until the next BT-mode entry");
    }
    if (s.ssp_window) {
        s.ssp_window = 0u;                 /* apply_scan_mode() drops discoverability; prompt_poll() rejects a prompt */
        bt_xrec_plat_log("BT SSP pairing window closed: a failed pairing used this session's P-192 key");
    }
}

/* Remote name for the screen: at most BT_XREC_PEER_NAME_MAX - 1 bytes, control bytes replaced, never ending inside a
 * UTF-8 sequence (the cut or a malformed name would leave the font a broken character). */
static void name_copy(char out[BT_XREC_PEER_NAME_MAX], const uint8_t *in, size_t in_max)
{
    size_t n = 0u;
    while (n < in_max && n < BT_XREC_PEER_NAME_MAX - 1u && in[n] != 0u) {
        uint8_t c = in[n];
        out[n] = (c < 0x20u || c == 0x7Fu) ? '?' : (char)c;
        n++;
    }
    size_t k = n;
    while (k > 0u && ((uint8_t)out[k - 1u] & 0xC0u) == 0x80u) {
        k--;                                      /* back over continuation bytes to the lead byte */
    }
    if (k > 0u && ((uint8_t)out[k - 1u] & 0x80u) != 0u) {
        uint8_t lead = (uint8_t)out[k - 1u];
        size_t need = lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : lead >= 0xC0u ? 2u : 1u;
        if (n - (k - 1u) < need || (lead & 0xC0u) != 0xC0u) {
            n = k - 1u;                           /* incomplete (or stray) sequence at the end: drop it */
        }
    }
    out[n] = '\0';
}

static void bda_text(char out[BT_XREC_PEER_NAME_MAX], const uint8_t bda[6])
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0u;
    for (int i = 0; i < 6; i++) {                 /* esp_bd_addr_t: most significant byte first */
        out[o++] = hex[bda[i] >> 4];
        out[o++] = hex[bda[i] & 0x0Fu];
        out[o++] = i < 5 ? ':' : '\0';
    }
}

/* ============================================ BTC side (copy + notify only) ===================================== */
static bool push_locked(const ev_t *e)
{
    if (s.ev_head != s.ev_tail && (e->type == EV_DATA || e->type == EV_WRITE)) {
        ev_t *last = &s.evq[(s.ev_head - 1u) % BT_XREC_EVQ_SLOTS];
        if (last->type == e->type && last->handle == e->handle && last->val <= UINT32_MAX - e->val) {
            last->val += e->val;
            return true;
        }
    }
    if (e->type == EV_RX_OVERFLOW) {
        for (uint32_t i = s.ev_tail; i != s.ev_head; i++) {
            const ev_t *q = &s.evq[i % BT_XREC_EVQ_SLOTS];
            if (q->type == EV_RX_OVERFLOW && q->handle == e->handle) {
                return true;
            }
        }
    }
    if (s.ev_head - s.ev_tail >= BT_XREC_EVQ_SLOTS) {
        s.evq_lost = 1u;
        s.evq_lost_n++;
        return false;
    }
    if (e->type == EV_SRV_OPEN && e->status == ESP_SPP_SUCCESS && !s.cb_rx_bound) {
        s.cb_rx_bound = 1u;
        s.cb_rx_closing = 0u;
        s.cb_rx_overflow = 0u;
        s.cb_rx_handle = e->handle;
    } else if (e->type == EV_CLOSE && s.cb_rx_bound && e->handle == s.cb_rx_handle) {
        s.cb_rx_bound = 0u;
        s.cb_rx_closing = 0u;
        s.cb_rx_overflow = 0u;
        s.cb_rx_handle = 0u;
    }
    s.evq[s.ev_head % BT_XREC_EVQ_SLOTS] = *e;
    s.ev_head++;
    return true;
}

/* BTC context: keep the newest remote name (short copy under the lock; nothing queued, the UI reads it on demand). */
static void name_put(const uint8_t bda[6], const uint8_t *name, size_t max)
{
    if (name == NULL || name[0] == 0u) {
        return;
    }
    bt_xrec_plat_lock();
    if (s.cb_open) {
        memcpy(s.nm_bda, bda, 6u);
        name_copy(s.nm, name, max);
        s.nm_valid = s.nm[0] != '\0';
    }
    bt_xrec_plat_unlock();
}

static void post(const ev_t *e)
{
    bool queued = false;
    bt_xrec_plat_lock();
    if (s.cb_open) {
        (void)push_locked(e);
        queued = true;
    }
    bt_xrec_plat_unlock();
    if (queued) {
        bt_xrec_plat_notify_app();
    }
}

static void on_data_ind(uint32_t handle, const uint8_t *data, uint16_t len)
{
    bool queued = false;
    bt_xrec_plat_lock();
    if (s.cb_open && s.b != NULL && data != NULL && len > 0u) {
        if (!s.cb_rx_bound || s.cb_rx_closing || handle != s.cb_rx_handle) {
            s.rx_dropped_cb += len;
        } else {
            s.rx_frames++;
            uint32_t used = s.rx_head - s.rx_tail;
            ev_t e = { 0 };
            e.handle = handle;
            if (s.cb_rx_overflow || (uint32_t)len > BT_XREC_RX_RING - used) {
                /* No backpressure exists in ESP_SPP_MODE_CB: never drop silently, the app closes this connection. */
                s.cb_rx_overflow = 1u;
                s.rx_overflows++;
                e.type = EV_RX_OVERFLOW;
                (void)push_locked(&e);
            } else {
                uint32_t off = s.rx_head & (BT_XREC_RX_RING - 1u);
                uint32_t first = BT_XREC_RX_RING - off;
                if (first > len) {
                    first = len;
                }
                memcpy(&s.b->rx[off], data, first);
                memcpy(&s.b->rx[0], data + first, (size_t)len - first);
                e.type = EV_DATA;
                e.val = len;
                if (push_locked(&e)) {
                    s.rx_head += len;
                }
            }
            queued = true;
        }
    }
    bt_xrec_plat_unlock();
    if (queued) {
        bt_xrec_plat_notify_app();
    }
}

/* BTC context: patch 0044 reports an RX frame btc_spp could not queue (no memory). The stream has a gap: like an
 * overflow, nothing more is taken from this connection and the app closes it (EV_RX_OVERFLOW, status 1). */
static void on_rx_lost(uint32_t handle)
{
    bool queued = false;
    bt_xrec_plat_lock();
    if (s.cb_open && s.cb_rx_bound && !s.cb_rx_closing && handle == s.cb_rx_handle) {
        ev_t e = { 0 };
        e.type = EV_RX_OVERFLOW;
        e.status = 1u;
        e.handle = handle;
        s.cb_rx_overflow = 1u;
        s.d.rx_lost++;
        (void)push_locked(&e);
        queued = true;
    }
    bt_xrec_plat_unlock();
    if (queued) {
        bt_xrec_plat_notify_app();
    }
}

/* BTC hands callbacks the copy it placed right after the 4-byte btc_msg_t: 4-aligned, which is all Cortex-M4 needs
 * for these pointer-holding unions; x86-64 UBSan (make test-bt-host) wants 8. Host-only finding, not a target issue. */
#define BTC_CB_ARG_ALIGN4 __attribute__((no_sanitize("alignment")))

BTC_CB_ARG_ALIGN4
static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *p)
{
    ev_t e = { 0 };
    if (p == NULL) {
        return;
    }
    switch (event) {
    case ESP_SPP_INIT_EVT:
        e.type = EV_SPP_INIT;
        e.status = (uint8_t)p->init.status;
        break;
    case ESP_SPP_UNINIT_EVT:
        s_spp_uninit_seen = 1u;   /* read by the teardown job; nothing else to do */
        return;
    case ESP_SPP_START_EVT:
        e.type = EV_SPP_START;
        e.status = (uint8_t)p->start.status;
        e.extra = p->start.scn;
        e.handle = p->start.handle;
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        e.type = EV_SRV_OPEN;
        e.status = (uint8_t)p->srv_open.status;
        e.handle = p->srv_open.handle;
        memcpy(e.bda, p->srv_open.rem_bda, 6u);
        e.val = p->srv_open.status == ESP_SPP_SUCCESS ? bt_xrec_plat_spp_peer_mtu(p->srv_open.handle) : 0u;
        break;
    case ESP_SPP_CLOSE_EVT:
        e.type = EV_CLOSE;
        e.handle = p->close.handle;
        break;
    case ESP_SPP_DATA_IND_EVT:
        if (p->data_ind.status != ESP_SPP_SUCCESS) {
            on_rx_lost(p->data_ind.handle);    /* patch 0044: btc_spp dropped a frame (no memory) */
            return;
        }
        on_data_ind(p->data_ind.handle, p->data_ind.data, p->data_ind.len);
        return;
    case ESP_SPP_CONG_EVT:
        e.type = EV_CONG;
        e.handle = p->cong.handle;
        e.extra = p->cong.cong ? 1u : 0u;
        break;
    case ESP_SPP_WRITE_EVT:
        if (p->write.status != ESP_SPP_SUCCESS || p->write.len <= 0) {
            return;
        }
        e.type = EV_WRITE;
        e.handle = p->write.handle;
        e.val = (uint32_t)p->write.len;
        break;
    default:
        return;
    }
    post(&e);
}

BTC_CB_ARG_ALIGN4
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p)
{
    ev_t e = { 0 };
    if (p == NULL) {
        return;
    }
    switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:   /* Just Works never gets here: btc_dm.c:542 auto-confirms it (see AUTH_CMPL) */
        e.type = EV_CFM_REQ;
        memcpy(e.bda, p->cfm_req.bda, 6u);
        e.val = p->cfm_req.num_val;
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        e.type = EV_PIN_REQ;
        memcpy(e.bda, p->pin_req.bda, 6u);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        e.type = EV_KEY_REQ;
        memcpy(e.bda, p->key_req.bda, 6u);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        e.type = EV_KEY_NOTIF;
        memcpy(e.bda, p->key_notif.bda, 6u);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        e.type = EV_AUTH_CMPL;
        memcpy(e.bda, p->auth_cmpl.bda, 6u);
        e.status = (uint8_t)p->auth_cmpl.stat;
        e.extra = p->auth_cmpl.lk_type;
        name_put(p->auth_cmpl.bda, p->auth_cmpl.device_name, sizeof(p->auth_cmpl.device_name));
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            name_put(p->read_rmt_name.bda, p->read_rmt_name.rmt_name, sizeof(p->read_rmt_name.rmt_name));
        } else {
            bt_xrec_plat_lock();
            s.d.name_failed++;
            bt_xrec_plat_unlock();
        }
        return;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        e.type = EV_ACL_CONN;
        e.status = (uint8_t)p->acl_conn_cmpl_stat.stat;
        e.handle = p->acl_conn_cmpl_stat.handle;
        memcpy(e.bda, p->acl_conn_cmpl_stat.bda, 6u);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        e.type = EV_ACL_DISC;
        e.handle = p->acl_disconn_cmpl_stat.handle;
        memcpy(e.bda, p->acl_disconn_cmpl_stat.bda, 6u);
        break;
    default:
        return;
    }
    post(&e);
}

/* ============================================ app side ========================================================== */
static void fault(uint32_t reason);

static void tx_drop(void)
{
    s.tx_head = s.tx_tail = 0u;
}

/* Disconnect cancels pending work and clears XREC authorization (plan M4), immediately, before Bluedroid's CLOSE.
 * This can run inside xrec_proto (send -> tx_kick -> write error), so only the auth flag is cleared here and the full
 * xrec_on_disconnect() (which aborts a staged PUT) runs at the next xrec_reset_flush(), outside xrec_proto. */
static void conn_close_request(const char *why)
{
    if (!s.conn_open || s.conn_closing) {
        return;
    }
    s.conn_closing = 1u;
    bt_xrec_plat_lock();
    if (s.cb_rx_bound && s.cb_rx_handle == s.conn_handle) {
        s.cb_rx_closing = 1u;
    }
    bt_xrec_plat_unlock();
    tx_drop();
    if (s.b != NULL) {
        s.b->xrec.auth = 0u;
        s.xrec_reset_pending = 1u;
    }
    bt_xrec_plat_log(why);
    esp_err_t r = esp_spp_disconnect(s.conn_handle);
    if (r != ESP_OK) {
        bt_xrec_plat_log_u32("BT XREC esp_spp_disconnect failed", (uint32_t)r);
        /* review STEP10 C11: the request was not queued (no memory for BTC's message). Retried by close_retry_poll();
         * the connection stays closing (no traffic taken or sent) meanwhile. */
        s.close_retry = 1u;
        s.close_tries = 0u;
        s.close_retry_at = bt_xrec_plat_now_ms() + BT_XREC_CLOSE_RETRY_MS;
    }
}

/* App task. Review STEP10 C11: a refused esp_spp_disconnect() is retried until accepted (the CLOSE event then ends
 * the connection); after BT_XREC_CLOSE_RETRIES refusals the session faults and the recovery ladder tears it down. */
static void close_retry_poll(uint32_t now)
{
    if (!s.close_retry || !s.conn_open || !time_reached(now, s.close_retry_at)) {
        return;
    }
    s.d.close_retries++;
    esp_err_t r = esp_spp_disconnect(s.conn_handle);
    if (r == ESP_OK) {
        s.close_retry = 0u;
        bt_xrec_plat_log_u32("BT XREC esp_spp_disconnect accepted, retry", s.close_tries + 1u);
        return;
    }
    if (++s.close_tries >= BT_XREC_CLOSE_RETRIES) {
        s.close_retry = 0u;
        bt_xrec_plat_log_u32("BT XREC esp_spp_disconnect refused, giving up after", s.close_tries);
        fault(BT_XREC_FAULT_CLOSE_FAIL);
        return;
    }
    s.close_retry_at = now + BT_XREC_CLOSE_RETRY_MS;
}

static void xrec_reset_flush(void)
{
    if (s.xrec_reset_pending && s.b != NULL) {
        xrec_on_disconnect(&s.b->xrec);
    }
    s.xrec_reset_pending = 0u;
}

static void conn_reset(void)
{
    uint32_t old_handle = s.conn_handle;
    if (s.conn_open) {
        bt_xrec_plat_lock();
        if (s.cb_rx_bound && s.cb_rx_handle == old_handle) {
            s.cb_rx_bound = 0u;
            s.cb_rx_closing = 0u;
            s.cb_rx_overflow = 0u;
            s.cb_rx_handle = 0u;
        }
        bt_xrec_plat_unlock();
    }
    if (s.b != NULL) {
        xrec_on_disconnect(&s.b->xrec);
    }
    s.xrec_reset_pending = 0u;
    s.conn_open = s.conn_authorized = s.conn_closing = s.cong = s.entropy_fail = 0u;
    s.close_retry = s.close_tries = 0u;
    s.conn_handle = 0u;
    s.mtu = 0u;
    s.inflight = 0u;
    tx_drop();
}

/* Hand queued bytes to esp_spp_write() while BTC accepts them. Every result is checked: ESP_ERR_NO_MEM = BTC ring
 * full (congested until ESP_SPP_CONG_EVT), anything else = the connection is gone or broken. */
static void tx_kick(void)
{
    while (s.conn_open && !s.conn_closing && !s.cong && s.tx_head != s.tx_tail) {
        uint32_t used = s.tx_head - s.tx_tail;
        uint32_t off = s.tx_tail & (BT_XREC_TX_RING - 1u);
        uint32_t n = BT_XREC_TX_RING - off;
        if (n > used) {
            n = used;
        }
        if (n > BT_XREC_WRITE_MAX) {
            n = BT_XREC_WRITE_MAX;
        }
        uint32_t cap = INFLIGHT_MAX;
        if (s.inflight >= cap) {
            return;              /* resumes on ESP_SPP_WRITE_EVT */
        }
        if (n > cap - s.inflight) {
            n = cap - s.inflight;
        }
        if (n == 0u) {
            return;              /* resumes on ESP_SPP_WRITE_EVT */
        }
        esp_err_t r = esp_spp_write(s.conn_handle, (int)n, &s.b->tx[off]);
        if (r == ESP_OK) {       /* BTC copied the bytes into its ring (btc_spp.c esp_spp_write CB mode) */
            s.tx_tail += n;
            s.inflight += n;
            s.d.tx_writes++;
            s.d.tx_bytes += n;
        } else if (r == ESP_ERR_NO_MEM) {
            s.cong = 1u;
            s.d.tx_nomem++;
            return;
        } else {
            s.d.tx_errors++;
            bt_xrec_plat_log_u32("BT XREC esp_spp_write error", (uint32_t)r);
            conn_close_request("BT XREC close: write failed");
            return;
        }
    }
}

/* ---- xrec_proto ops owned by the session ---- */
static int xo_send(void *user, const uint8_t *data, uint16_t len)
{
    (void)user;
    if (!s.conn_open || !s.conn_authorized || s.conn_closing || s.entropy_fail || s.b == NULL) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    if (data == NULL) {
        return -1;
    }
    uint32_t used = s.tx_head - s.tx_tail;
    if ((uint32_t)len > BT_XREC_TX_RING - used) {
        s.d.tx_busy++;
        return -1;               /* all-or-nothing, like S:845 bt_spp_send: xrec_proto retries LS/GETS next poll */
    }
    uint32_t off = s.tx_head & (BT_XREC_TX_RING - 1u);
    uint32_t first = BT_XREC_TX_RING - off;
    if (first > len) {
        first = len;
    }
    memcpy(&s.b->tx[off], data, first);
    memcpy(&s.b->tx[0], data + first, (size_t)len - first);
    s.tx_head += len;
    tx_kick();
    return 0;
}

static uint16_t xo_get_mtu(void *user)
{
    (void)user;
    return s.conn_open ? s.mtu : 0u;
}

static void xo_entropy(void *user, uint8_t out[16])
{
    (void)user;
    if (bt_xrec_plat_entropy(out, 16u) != 0) {
        /* Fail closed (plan M2): no fixed fallback. The reply is never sent and the link is closed by the next poll. */
        wipe_mem(out, 16u);
        if (!s.entropy_fail) {
            bt_xrec_plat_log("BT XREC CHAL refused: entropy not ready");
        }
        s.entropy_fail = 1u;
    }
}

static void window_close(void)
{
    s.window_open = 0u;
    s.ssp_window = 0u;
    s.ssp_deferred = 0u;
    s.xrec_pin_issued = 0u;
    if (s.b != NULL) {
        xrec_close_pairing(&s.b->xrec);
    }
}

static int xo_bound_key(void *user, const uint8_t key[32])
{
    (void)user;
    if (bt_xrec_plat_save_bound_key(key) != 0) {
        return -1;               /* +XREC:ERR,SAVE: old binding kept, window left open for a retry (STEP10 C9) */
    }
    bt_xrec_plat_log("BT XREC PAIRED");
    window_close();              /* legacy F:1805-1810: window and discoverability closed */
    return 0;
}

static void defer_reboot(uint8_t mode)
{
    s.reboot_mode = mode;
    s.reboot_at = bt_xrec_plat_now_ms() + BT_XREC_REBOOT_DELAY_MS;
}

static void xo_reboot(void *user) { (void)user; defer_reboot(1u); }
static void xo_ota(void *user) { (void)user; defer_reboot(2u); }
static void xo_dfu(void *user) { (void)user; defer_reboot(3u); }

/* ---- pairing policy ---- */
static bool is_bonded(const uint8_t bda[6])
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return false;
    }
    esp_bd_addr_t list[8];
    int cnt = (int)(sizeof(list) / sizeof(list[0]));
    if (esp_bt_gap_get_bond_device_list(&cnt, list) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < cnt && i < (int)(sizeof(list) / sizeof(list[0])); i++) {
        if (bda_eq(list[i], bda)) {
            return true;
        }
    }
    return false;
}

/* XREC traffic only on a link that is encrypted and MITM-authenticated (SPP security mask, start_server): either this
 * link just bonded through an accepted numeric comparison, or the peer's STORED key is an authenticated combination
 * key. A bond's existence proves nothing (review finding): Bluedroid stores a Just Works key before the app sees
 * AUTH_CMPL, BTA DM removes it only once the ACL is down, and a stop, a power loss, a failed SD rewrite or a lost
 * AUTH_CMPL (fail-soft allocation) can each leave it behind. */
static bool link_authorized(const uint8_t bda[6])
{
    if (slot_is(&s.reject, bda)) {
        return false;
    }
    return slot_is(&s.auth_ok, bda) || (is_bonded(bda) && bt_xrec_plat_bond_mitm(bda));
}

static void reject_bond(const uint8_t bda[6], const char *why)
{
    slot_set(&s.reject, bda);
    s.d.bonds_removed++;
    bt_xrec_plat_log(why);
    /* The key leaves the BTC config (and so every later bond-file write) now; BTA DM's own removal below waits for the
     * ACL to go down, which a BT stop or a power loss can pre-empt (review finding). */
    bt_xrec_plat_bond_purge(bda);
    esp_err_t r = esp_bt_gap_remove_bond_device((uint8_t *)bda);   /* also takes the ACL down (bta_dm_remove_device) */
    if (r != ESP_OK) {
        bt_xrec_plat_log_u32("BT SSP remove_bond_device failed", (uint32_t)r);
    }
    if (s.conn_open && bda_eq(s.conn_bda, bda)) {
        conn_close_request("BT XREC close: link not MITM-authenticated");
    }
}

static void digits6(char out[8], uint32_t v)
{
    v %= 1000000u;
    for (int i = 5; i >= 0; i--) {
        out[i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    out[6] = '\0';
    out[7] = '\0';
}

static bool name_known(const uint8_t bda[6])
{
    bt_xrec_plat_lock();
    bool k = s.nm_valid && bda_eq(s.nm_bda, bda);
    bt_xrec_plat_unlock();
    return k;
}

/* Ask Bluedroid for the peer's name once per peer (an HCI Remote Name Request on the existing ACL, the same request
 * BTA DM / BTM already send before a confirmation request or a secured connection). No answer = the address shows. */
static void request_name(const uint8_t bda[6])
{
    if (name_known(bda) || slot_is(&s.name_asked, bda)) {
        return;
    }
    slot_set(&s.name_asked, bda);
    s.d.name_requests++;
    esp_err_t r = esp_bt_gap_read_remote_name((uint8_t *)bda);
    if (r != ESP_OK) {
        bt_xrec_plat_log_u32("BT read_remote_name failed", (uint32_t)r);
    }
}

/* Silicon session 6: the prompt showed the address, the name never came. One line per prompt says whether and when
 * the name became known (with the HCI "name request" / "name complete" notes this shows which request failed). */
static void prompt_name_poll(uint32_t now)
{
    if (s.cfm_pending && !s.cfm_named && name_known(s.cfm_bda)) {
        s.cfm_named = 1u;
        bt_xrec_plat_log_u32("BT SSP prompt shows the peer name, ms after the prompt", now - s.cfm_t0);
    }
}

static void prompt_end_log(void)
{
    if (!s.cfm_named && !name_known(s.cfm_bda)) {   /* a name drained at the end of the poll is already shown */
        bt_xrec_plat_log("BT SSP prompt ended without the peer name (address shown)");
    }
}

static int link_find(uint16_t h)
{
    for (int i = 0; i < (int)(sizeof(s.links) / sizeof(s.links[0])); i++) {
        if (s.links[i].used && s.links[i].h == h) {
            return i;
        }
    }
    return -1;
}

static void link_up(uint16_t h)
{
    int i = link_find(h);
    for (int j = 0; i < 0 && j < (int)(sizeof(s.links) / sizeof(s.links[0])); j++) {
        if (!s.links[j].used) {
            i = j;
        }
    }
    if (i >= 0) {                          /* more links than slots: the extra ones always count as connected */
        s.links[i].used = 1u;
        s.links[i].h = h;
        s.links[i].closing = 0u;
        if (s.disc_early && s.disc_early_h == h) {
            s.links[i].closing = 1u;       /* the host's 0406 for this link was noted before its link-up event */
            s.disc_early = 0u;
        }
    }
}

static void link_down(uint16_t h)
{
    int i = link_find(h);
    if (i >= 0) {
        s.links[i].used = 0u;
    }
    if (s.disc_early && s.disc_early_h == h) {
        s.disc_early = 0u;
    }
}

static uint32_t links_closing(void)
{
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < sizeof(s.links) / sizeof(s.links[0]); i++) {
        n += (s.links[i].used && s.links[i].closing) ? 1u : 0u;
    }
    return n;
}

/* What the screen calls "connected": SPP open, or an ACL link the host is not ending. */
static bool ui_connected(void)
{
    return s.acl_links > links_closing() || (s.conn_open && !s.conn_closing);
}

static void cfm_reply(const uint8_t bda[6], bool accept)
{
    esp_err_t r = esp_bt_gap_ssp_confirm_reply((uint8_t *)bda, accept);
    if (r != ESP_OK) {
        bt_xrec_plat_log_u32("BT SSP confirm_reply failed", (uint32_t)r);
    }
}

static void cfm_accepted(const uint8_t bda[6])
{
    s.d.ssp_accepted++;
    slot_set(&s.cfm_ok, bda);
}

/* The pending prompt ends without a reply (its pairing is gone, or BT stops). */
static void prompt_withdraw(const char *why)
{
    if (!s.cfm_pending) {
        return;
    }
    s.cfm_pending = 0u;
    memset(s.cfm_digits, 0, sizeof(s.cfm_digits));
    s.cfm_ended = 1u;
    s.cfm_end_t = bt_xrec_plat_now_ms();
    s.d.ssp_withdrawn++;
    bt_xrec_plat_log(why);
    prompt_end_log();
}

/* The pending prompt ends with a negative reply. */
static void prompt_reject(const char *why)
{
    if (!s.cfm_pending) {
        return;
    }
    cfm_reply(s.cfm_bda, false);
    s.d.ssp_rejected++;
    s.cfm_pending = 0u;
    memset(s.cfm_digits, 0, sizeof(s.cfm_digits));
    s.cfm_ended = 1u;
    s.cfm_end_t = bt_xrec_plat_now_ms();
    bt_xrec_plat_log(why);
    prompt_end_log();
}

static void on_cfm_req(const ev_t *e)
{
    if (!s.ssp_window || s.state != BT_XREC_ST_READY) {
        cfm_reply(e->bda, false);
        s.d.ssp_rejected++;
        bt_xrec_plat_log("BT SSP numeric comparison rejected: pairing window closed");
        return;
    }
    /* The gate is re-checked here, not only by the poll after drain_events(): the request may come from a pairing
     * whose nonce the ROM drew while the RNG pool ran dry since the last poll (review finding). */
    const char *why = ssp_block();
    if (why != NULL) {
        cfm_reply(e->bda, false);
        s.d.ssp_rejected++;
        s.d.ssp_gate_cfm_rejects++;
        s.ssp_window = 0u;
        log_gate("BT SSP numeric comparison rejected, pairing window closed: ", why);
        return;
    }
#if BENCH_IMAGE
    if (s_bench_autoconfirm) {
        /* BENCH ONLY (bt_xrec_set_bench_autoconfirm): the old interim behaviour, the device does not verify the value;
         * shown through bt_status_t.pair_pin until AUTH_CMPL. Logged loudly on every use. */
        cfm_reply(e->bda, true);
        cfm_accepted(e->bda);
        s.d.ssp_bench_autoconfirmed++;
        digits6(s.ssp_digits, e->val);
        s.ssp_showing = 1u;
        s.ssp_deadline = bt_xrec_plat_now_ms() + BT_XREC_SSP_SHOW_MS;
        bt_xrec_plat_log("BT SSP BENCH autoconfirm: numeric comparison accepted WITHOUT the user (bench only)");
        bt_xrec_plat_log_u32("BT SSP numeric comparison accepted (BENCH autoconfirm), value", e->val % 1000000u);
        return;
    }
#endif
    if (s.cfm_pending) {
        /* One prompt at a time: the digits on screen never change under the user's finger. The same peer asking again
         * means its first pairing is gone, so that prompt goes too (BTM pairs one peer at a time: one reply). */
        bool same = bda_eq(s.cfm_bda, e->bda);
        cfm_reply(e->bda, false);
        s.d.ssp_rejected++;
        bt_xrec_plat_log("BT SSP numeric comparison rejected: another one waits for the user");
        if (same) {
            prompt_withdraw("BT SSP prompt withdrawn: the peer asked again");
        }
        return;
    }
    s.cfm_pending = 1u;
    s.cfm_named = 0u;
    memcpy(s.cfm_bda, e->bda, 6u);
    s.cfm_val = e->val % 1000000u;
    digits6(s.cfm_digits, e->val);
    s.cfm_t0 = bt_xrec_plat_now_ms();
    bt_xrec_plat_log_u32("BT SSP numeric comparison waiting for the user (ENTER/BACK, 30 s), value", s.cfm_val);
    request_name(e->bda);
    prompt_name_poll(s.cfm_t0);
}

/* Window closed or prompt timed out (bt_xrec_poll). */
static void prompt_poll(uint32_t now)
{
    if (!s.cfm_pending) {
        return;
    }
    prompt_name_poll(now);
    if (!s.ssp_window || s.state != BT_XREC_ST_READY) {
        prompt_reject("BT SSP numeric comparison rejected: pairing window closed while waiting for the user");
    } else if (now - s.cfm_t0 >= BT_XREC_SSP_PROMPT_MS) {
        s.d.ssp_timeouts++;
        prompt_reject("BT SSP numeric comparison rejected: no answer within 30 s");
    }
}

static void on_gap_event(const ev_t *e)
{
    esp_err_t r;
    switch (e->type) {
    case EV_CFM_REQ:
        on_cfm_req(e);
        break;
    case EV_PIN_REQ: {
        esp_bt_pin_code_t pin;
        memset(pin, 0, sizeof(pin));
        s.d.pin_rejected++;
        r = esp_bt_gap_pin_reply((uint8_t *)e->bda, false, 0u, pin);
        bt_xrec_plat_log("BT legacy PIN pairing rejected");
        if (r != ESP_OK) {
            bt_xrec_plat_log_u32("BT pin_reply failed", (uint32_t)r);
        }
        break;
    }
    case EV_KEY_REQ:
        s.d.passkey_rejected++;
        r = esp_bt_gap_ssp_passkey_reply((uint8_t *)e->bda, false, 0u);
        bt_xrec_plat_log("BT SSP passkey entry rejected");
        if (r != ESP_OK) {
            bt_xrec_plat_log_u32("BT passkey_reply failed", (uint32_t)r);
        }
        break;
    case EV_KEY_NOTIF:
        /* Passkey display has no reply: mark the peer so the resulting bond is removed at AUTH_CMPL. */
        s.d.passkey_rejected++;
        slot_set(&s.reject, e->bda);
        bt_xrec_plat_log("BT SSP passkey display rejected");
        break;
    case EV_AUTH_CMPL:
        if (s.ssp_showing && slot_is(&s.cfm_ok, e->bda)) {
            s.ssp_showing = 0u;
        }
        if (s.cfm_pending && bda_eq(s.cfm_bda, e->bda)) {
            /* the peer's pairing ended before the user answered (its user said no, or it timed out): nothing was
             * accepted, so a key that came out of it anyway is removed below */
            prompt_withdraw("BT SSP prompt withdrawn: pairing ended by the peer");
        }
        if (e->status == ESP_BT_STATUS_SUCCESS) {
            bool mitm = e->extra == ESP_BT_LINK_KEY_AUTHED_COMB_P192 || e->extra == ESP_BT_LINK_KEY_AUTHED_COMB_P256;
            if (mitm && slot_is(&s.cfm_ok, e->bda) && !slot_is(&s.reject, e->bda)) {
                slot_set(&s.auth_ok, e->bda);
                bt_xrec_plat_log("BT SSP bonded (numeric comparison, MITM)");
                if (s.ssp_window) {
                    s.ssp_window = 0u;     /* one bond per window; apply_scan_mode() drops discoverability */
                    bt_xrec_plat_log("BT SSP window closed after the first bond");
                }
            } else {
                reject_bond(e->bda, mitm ? "BT SSP bond removed: not confirmed in the pairing window"
                                         : "BT SSP bond removed: Just Works / unauthenticated key");
            }
        } else {
            bt_xrec_plat_log_u32("BT SSP pairing failed, status", e->status);
            if (slot_is(&s.cfm_ok, e->bda)) {
                ssp_burn();                /* accepted, then failed: the peer saw the DHKey check (s.ssp_burned) */
            }
        }
        if (slot_is(&s.cfm_ok, e->bda)) {
            s.cfm_ok.valid = 0u;
        }
        break;
    case EV_ACL_CONN:
        if (e->status == ESP_BT_STATUS_SUCCESS) {
            s.acl_links++;
            link_up((uint16_t)e->handle);
            slot_set(&s.acl_peer, e->bda);
        }
        break;
    case EV_ACL_DISC:
        if (s.acl_links > 0u) {
            s.acl_links--;
        }
        link_down((uint16_t)e->handle);
        if (s.scan_refused) {
            s.scan_retry_at = bt_xrec_plat_now_ms();   /* apply_scan_mode() sends the wanted mode again this poll */
        }
        if (slot_is(&s.acl_peer, e->bda)) {
            s.acl_peer.valid = 0u;
        }
        if (s.cfm_pending && bda_eq(s.cfm_bda, e->bda)) {
            prompt_withdraw("BT SSP prompt withdrawn: peer disconnected");
        }
        if (slot_is(&s.reject, e->bda)) {
            s.reject.valid = 0u;
        }
        if (slot_is(&s.auth_ok, e->bda)) {
            s.auth_ok.valid = 0u;
        }
        if (slot_is(&s.cfm_ok, e->bda)) {
            ssp_burn();                    /* accepted, then the link went down before AUTH_CMPL */
            s.cfm_ok.valid = 0u;
            s.ssp_showing = 0u;
        }
        break;
    default:
        break;
    }
}

/* Bonds loaded from the card whose key is not MITM-authenticated (left there by a power loss or a failed rewrite while a
 * rejected bond was still stored) are removed before the device becomes connectable. */
static void purge_unauthenticated_bonds(void)
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return;
    }
    esp_bd_addr_t list[8];
    int cnt = (int)(sizeof(list) / sizeof(list[0]));
    if (esp_bt_gap_get_bond_device_list(&cnt, list) != ESP_OK) {
        return;
    }
    for (int i = 0; i < cnt && i < (int)(sizeof(list) / sizeof(list[0])); i++) {
        if (!bt_xrec_plat_bond_mitm(list[i])) {
            s.d.bonds_removed++;
            bt_xrec_plat_log("BT stored bond removed: not MITM-authenticated");
            bt_xrec_plat_bond_purge(list[i]);
            esp_err_t r = esp_bt_gap_remove_bond_device(list[i]);   /* no ACL yet: BTM record dropped at once */
            if (r != ESP_OK) {
                bt_xrec_plat_log_u32("BT SSP remove_bond_device failed", (uint32_t)r);
            }
        }
    }
}

static void start_server(void)
{
    esp_spp_start_srv_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.local_scn = BT_XREC_SPP_SCN;
    cfg.create_spp_record = true;
    /* MITM: BTM asks for a link-key upgrade when an unauthenticated key could become an authenticated one (the upgrade
     * pairing then meets the numeric-comparison gate). It does NOT refuse an unauthenticated key it cannot upgrade
     * (btm_sec_is_upgrade_possible: a NoInputNoOutput peer, or unknown IO caps on a reconnect with a stored key): the
     * host harness opened SPP for a stale Just Works bond with this flag alone. link_authorized()'s key-type check is
     * the enforcement. esp_spp_api.h advises against ESP_SPP_SEC_MITM "due to certain limitations"; the harness runs
     * the bonded, re-paired and Just Works paths with it (silicon: BT_HW_RESULTS recipe). */
    cfg.sec_mask = ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_MITM;
    cfg.role = ESP_SPP_ROLE_SLAVE;
    cfg.name = BT_XREC_SPP_NAME;
    esp_err_t r = esp_spp_start_srv_with_cfg(&cfg);
    if (r != ESP_OK) {
        bt_xrec_plat_log_u32("BT XREC esp_spp_start_srv failed", (uint32_t)r);
        fault(BT_XREC_FAULT_CONFIG);
    }
}

static void on_spp_event(const ev_t *e)
{
    switch (e->type) {
    case EV_SPP_INIT:
        if (s.state != BT_XREC_ST_CONFIG) {
            s.d.stale_events++;
        } else if (e->status != ESP_SPP_SUCCESS) {
            s_spp_init_posted = 0u;
            bt_xrec_plat_log_u32("BT XREC SPP init failed, status", e->status);
            fault(BT_XREC_FAULT_CONFIG);
        } else {
            s.spp_inited = 1u;
            purge_unauthenticated_bonds();
            start_server();
        }
        break;
    case EV_SPP_START:
        if (s.state != BT_XREC_ST_CONFIG) {
            s.d.stale_events++;
        } else if (e->status != ESP_SPP_SUCCESS || e->extra != BT_XREC_SPP_SCN) {
            bt_xrec_plat_log_u32("BT XREC SPP server start failed, scn", e->extra);
            fault(BT_XREC_FAULT_CONFIG);
        } else {
            set_state(BT_XREC_ST_READY);
            bt_xrec_plat_log("BT XREC READY (SPP channel 1)");
        }
        break;
    case EV_SRV_OPEN:
        if (e->status != ESP_SPP_SUCCESS) {
            break;
        }
        if (s.conn_open) {
            /* One session at a time (legacy F:1585-1590 rejects a second ACL). */
            s.d.conns_rejected++;
            bt_xrec_plat_log("BT XREC second SPP connection refused");
            esp_err_t r = esp_spp_disconnect(e->handle);
            if (r != ESP_OK) {
                bt_xrec_plat_log_u32("BT XREC esp_spp_disconnect failed", (uint32_t)r);
            }
            break;
        }
        conn_reset();
        s.conn_open = 1u;
        s.conn_handle = e->handle;
        memcpy(s.conn_bda, e->bda, 6u);
        s.mtu = (uint16_t)(e->val > 0xFFFFu ? 0xFFFFu : e->val);
        if (link_authorized(e->bda)) {
            s.conn_authorized = 1u;
            s.d.conns_authorized++;
            bt_xrec_plat_log_u32("BT XREC SPP open, mtu", s.mtu);
            request_name(e->bda);
        } else {
            s.d.conns_rejected++;
            conn_close_request("BT XREC close: peer not bonded with MITM");
        }
        break;
    case EV_CLOSE:
        if (s.conn_open && e->handle == s.conn_handle) {
            conn_reset();
            bt_xrec_plat_log("BT XREC SPP closed");
        }
        break;
    case EV_RX_OVERFLOW:
        if (s.conn_open && e->handle == s.conn_handle) {
            conn_close_request(e->status != 0u ? "BT XREC close: RX frame lost (no memory)"
                                               : "BT XREC close: RX buffer overflow");
        }
        break;
    case EV_WRITE:
        s.d.write_evts++;
        s.d.write_evt_bytes += e->val;
        if (s.conn_open && e->handle == s.conn_handle) {
            s.inflight -= e->val < s.inflight ? e->val : s.inflight;
        } else {
            s.d.stale_events++;
        }
        break;
    case EV_CONG:
        s.d.cong_evts++;
        if (s.conn_open && e->handle == s.conn_handle) {
            s.cong = e->extra;
        }
        break;
    default:
        break;
    }
}

/* Hand received bytes to xrec_proto one line at a time ('\r' ends a command; xrec_proto is a stream parser, so the
 * split changes nothing else). A CHAL that found no entropy (xo_entropy) leaves xrec_proto with chal_active set over
 * an all-zero nonce: nothing after that line may reach it, or an AUTH carrying HMAC(key, 0^16) in the same SPP write
 * would authorize the privileged commands that follow it (review finding). The challenge is disarmed and the link
 * closed before the next line. */
static void rx_feed(const uint8_t *p, uint32_t n)
{
    uint32_t i = 0u;
    while (i < n) {
        uint32_t k = i;
        while (k < n && p[k] != '\r') {
            k++;
        }
        uint32_t seg = (k < n ? k + 1u : n) - i;
        s.d.rx_bytes += seg;
        xrec_on_rx(&s.b->xrec, &p[i], (uint16_t)seg);
        i += seg;
        if (s.entropy_fail) {
            s.b->xrec.chal_active = 0u;
            wipe_mem(s.b->xrec.nonce, sizeof(s.b->xrec.nonce));
            conn_close_request("BT XREC close: entropy not ready");
            s.d.rx_dropped += n - i;
            return;
        }
        if (!s.conn_open || s.conn_closing) {
            s.d.rx_dropped += n - i;          /* closed by a send error inside xrec_proto: nothing more is served */
            return;
        }
    }
}

/* Drain the event queue in order. DATA is consumed in chunks so the lock is never held while xrec_proto runs. */
static void drain_events(void)
{
    uint8_t chunk[128];
    uint32_t budget = BT_XREC_RX_RING * 2u;    /* bounded work per poll: the UI loop keeps its cadence */
    for (;;) {
        ev_t e;
        uint32_t n = 0u;
        bt_xrec_plat_lock();
        if (s.ev_head == s.ev_tail || s.b == NULL) {
            bt_xrec_plat_unlock();
            break;
        }
        ev_t *head = &s.evq[s.ev_tail % BT_XREC_EVQ_SLOTS];
        e = *head;
        if (e.type == EV_DATA) {
            n = e.val < sizeof(chunk) ? e.val : (uint32_t)sizeof(chunk);
            uint32_t off = s.rx_tail & (BT_XREC_RX_RING - 1u);
            uint32_t first = BT_XREC_RX_RING - off;
            if (first > n) {
                first = n;
            }
            memcpy(chunk, &s.b->rx[off], first);
            memcpy(chunk + first, &s.b->rx[0], n - first);
            s.rx_tail += n;
            head->val -= n;
            if (head->val == 0u) {
                s.ev_tail++;
            }
        } else {
            s.ev_tail++;
        }
        bt_xrec_plat_unlock();

        if (e.type == EV_DATA) {
            if (s.conn_open && e.handle == s.conn_handle && s.conn_authorized && !s.conn_closing && !s.entropy_fail) {
                rx_feed(chunk, n);
            } else {
                s.d.rx_dropped += n;
            }
            xrec_reset_flush();
            if (budget <= n) {
                break;
            }
            budget -= n;
        } else if (e.type >= EV_CFM_REQ) {
            on_gap_event(&e);
        } else {
            on_spp_event(&e);
        }
        xrec_reset_flush();
        if (s.state == BT_XREC_ST_OFF || s.state == BT_XREC_ST_FAULTED || s.state == BT_XREC_ST_DEAD) {
            break;
        }
    }
    wipe_mem(chunk, sizeof(chunk));
}

static void config_begin(void)
{
    esp_err_t r;
    uint8_t iocap = ESP_BT_IO_CAP_IO;          /* DisplayYesNo: numeric comparison with phones (plan M4) */
    esp_bt_cod_t cod;
    esp_spp_cfg_t cfg;
    memset(&cod, 0, sizeof(cod));
    cod.minor = 0x01u;                          /* 0x200404 = legacy F:1550 {0x04, 0x04, 0x20} */
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.service = ESP_BT_COD_SRVC_AUDIO;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = ESP_SPP_MODE_CB;
    cfg.enable_l2cap_ertm = false;              /* basic mode, as the legacy RFCOMM (S:) and the desktop tools */
    cfg.tx_buffer_size = BT_XREC_SPP_TX_BUF;

    set_state(BT_XREC_ST_CONFIG);
    if ((r = esp_bt_gap_register_callback(gap_cb)) != ESP_OK ||
        (r = esp_spp_register_callback(spp_cb)) != ESP_OK ||
        (r = esp_bt_gap_set_device_name(s.name)) != ESP_OK ||
        (r = esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD)) != ESP_OK ||
        (r = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap))) != ESP_OK ||
        (r = esp_spp_enhanced_init(&cfg)) != ESP_OK) {
        bt_xrec_plat_log_u32("BT XREC GAP/SPP configuration failed", (uint32_t)r);
        fault(BT_XREC_FAULT_CONFIG);
    } else {
        s_spp_init_posted = 1u;
    }
}

/* Controller link notes into the session log, one line each (bounded per poll; bt_ctrl counts what overflows).
 * Drained before the GAP events of the poll: a refusal is always noted (bt_ctrl context) before the host can see a
 * later link drop, so the drop finds scan_refused set. The retry timer covers a link that ends without any GAP event
 * (a rejected re-page the ROM still counted) and a note that reached the ring only after the drain. */
static void ctrl_notes_drain(void)
{
    char line[BT_XREC_NOTE_LINE];
    for (int i = 0; i < 8; i++) {
        bt_xrec_note_t n = { 0 };
        int kind = bt_xrec_plat_ctrl_note(line, sizeof(line), &n);
        if (kind == 0) {
            return;
        }
        uint8_t arg = n.arg;
        s.d.ctrl_notes++;
        bt_xrec_plat_log(line);
        if (kind == (int)BT_XREC_NOTE_DISC_CMD) {
            /* the host is ending this link (a rejected or failed pairing, idle, policy): whatever the controller
             * answered, the screen stops calling it connected; the GAP link-down event removes it. Any status on
             * purpose (BT_ROM_HCI_MAP §11 H1/H2: 0x02 = the link is already gone). A refused 0406 (0x0C) or a local
             * deadline answer (0x22) may leave a live link shown as not connected until it is reported down or SPP
             * opens: preferred over a "connected" that may never clear. */
            int li = link_find(n.handle);
            if (li >= 0) {
                s.links[li].closing = 1u;
            } else {                       /* its GAP link-up is still queued behind this note (same poll) */
                s.disc_early_h = n.handle;
                s.disc_early = 1u;
            }
        } else if (kind == (int)BT_XREC_NOTE_CONN) {
            if (s.disc_early && s.disc_early_h == n.handle) {
                s.disc_early = 0u;         /* a new link on that handle: an older 0406 mark is stale */
            }
        } else if (kind == (int)BT_XREC_NOTE_NAME && n.status == 0u && arg != 0u) {
            /* any successful name request (BTM's own at link set-up answers late on silicon, and only BTM sees it):
             * the prompt shows the name in place of the address from now on. An empty name is not kept by bt_ctrl,
             * whose newest stored name then belongs to an older peer: skipped (arg = name length). Two queued name
             * notes both apply the newest name; the session keeps one name slot, so the end state is the same. */
            uint8_t bda[6];
            char nm[BT_XREC_PEER_NAME_MAX];
            if (bt_xrec_plat_ctrl_name(bda, nm, sizeof(nm))) {
                name_put(bda, (const uint8_t *)nm, sizeof(nm));
                s.d.names_from_ctrl++;
            }
        }
        if (kind == (int)BT_XREC_NOTE_SCAN_REFUSED) {
            s.d.scan_refused++;
            s.scan_refused = 1u;
            s.scan_retry_at = bt_xrec_plat_now_ms() + BT_XREC_SCAN_RETRY_MS;
            if ((arg & 0x01u) == 0u && s.scan_disc_sent) {
                /* a write without inquiry scan refused after a discoverable one was sent: the ROM may still be at
                 * 0x03 (discoverable after the window closed). Fail closed: scan off, which it always accepts. */
                s.scan_failsafe = 1u;
            }
        }
    }
}

static void apply_scan_mode(void)
{
    if (s.state != BT_XREC_ST_READY) {
        return;
    }
    if (s.scan_failsafe) {
        /* BTA writes 0C1A twice (discoverability, then connectability): the last one is 0x00 */
        esp_err_t r = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (r != ESP_OK) {
            bt_xrec_plat_log_u32("BT XREC set_scan_mode off failed (retrying)", (uint32_t)r);
            return;
        }
        s.scan_failsafe = 0u;
        s.scan_disc_sent = 0u;
        s.scan_applied = 0u;
        s.d.scan_failsafe++;
        bt_xrec_plat_log("BT XREC scan mode refused while discoverable: scan off until it can be applied");
    }
    uint8_t want = s.ssp_window ? 2u : 1u;
    if (s.scan_refused) {
        if (want == s.scan_want_sent && !time_reached(bt_xrec_plat_now_ms(), s.scan_retry_at)) {
            return;                        /* bounded: one retry per BT_XREC_SCAN_RETRY_MS while refused */
        }
        s.scan_refused = 0u;
        s.scan_applied = 0u;
        s.d.scan_reapplied++;
    }
    if (want == s.scan_applied) {
        return;
    }
    esp_err_t r = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                           want == 2u ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    if (r == ESP_OK) {
        s.scan_applied = want;
        s.scan_want_sent = want;
        if (want == 2u) {
            s.scan_disc_sent = 1u;
        }
    } else {
        bt_xrec_plat_log_u32("BT XREC set_scan_mode failed (retrying)", (uint32_t)r);
    }
}

/* The whole session block goes back to the heap zeroed: besides xrec.key, the RX ring and xrec.rx_line still hold an
 * XREC PAIR line ("+XREC:PAIR,<pin>,<64-hex key>") in plaintext, and the next allocation of that block would get it
 * (review finding). volatile: the stores are not dropped as dead before the free. */
static void wipe_free(bufs_t *b)
{
    wipe_mem(b, sizeof(*b));
    bt_xrec_plat_free(b);
}

static void free_session(void)
{
    bufs_t *b;
    bt_xrec_plat_lock();
    s.cb_open = 0u;
    s.cb_rx_bound = s.cb_rx_closing = s.cb_rx_overflow = 0u;
    s.cb_rx_handle = 0u;
    b = s.b;
    s.b = NULL;
    s.ev_head = s.ev_tail = 0u;
    s.rx_head = s.rx_tail = 0u;
    s.evq_lost = 0u;
    s.nm_valid = 0u;
    memset(s.nm, 0, sizeof(s.nm));
    bt_xrec_plat_unlock();
    if (b != NULL) {
        wipe_free(b);
    }
}

/* Headphones mode (bt_hp_bluedroid.c) overrides these; the phone-only images and tests keep the defaults. */
__attribute__((weak)) int bt_hp_shutdown_hook(void) { return 0; }
__attribute__((weak)) int bt_hp_active(void) { return 0; }

int bt_shutdown(void)
{
    if (s.state == BT_XREC_ST_OFF || s.state == BT_XREC_ST_FAULTED) {
        return bt_hp_shutdown_hook();          /* one exit path for both modes (docs/BT_A2DP_DESIGN.md §6.7) */
    }
    if (s.state == BT_XREC_ST_DEAD) {
        return (int)s.last_fault;
    }
    /* 1. quiesce: late BTC callbacks no longer touch the session; pending XREC work is cancelled. */
    bt_xrec_plat_lock();
    s.cb_open = 0u;
    bt_xrec_plat_unlock();
    if (s.b != NULL) {
        xrec_close_pairing(&s.b->xrec);
        xrec_on_disconnect(&s.b->xrec);
    }
    conn_reset();
    s.window_open = s.xrec_pin_issued = s.ssp_showing = s.ssp_window = s.ssp_deferred = 0u;
    prompt_withdraw("BT SSP prompt withdrawn: BT stopping");
    s.reboot_mode = 0u;
    /* 2. graceful teardown in the worker; the app keeps the recorder serviced and enforces one absolute deadline. */
    s_spp_uninit_seen = 0u;
    bt_xrec_plat_stack_down_begin();
    uint32_t t0 = bt_xrec_plat_now_ms();
    uint32_t reason = BT_XREC_FAULT_NONE;
    for (;;) {
        int r = 0;
        if (bt_xrec_plat_job_done(&r)) {
            if (bt_xrec_plat_deadline_disarm()) {
                reason = BT_XREC_FAULT_DOWN_LATCHED;   /* disarm first, then check the latch (plan M1) */
            } else if (r != 0) {
                reason = BT_XREC_FAULT_DOWN_FAIL;
            }
            break;
        }
        if (bt_xrec_plat_now_ms() - t0 >= BT_XREC_SHUTDOWN_DEADLINE_MS) {
            (void)bt_xrec_plat_deadline_disarm();
            reason = BT_XREC_FAULT_DOWN_DEADLINE;
            break;
        }
        bt_xrec_plat_service();
        bt_xrec_plat_wait();
    }
    s.spp_inited = 0u;
    s_spp_init_posted = 0u;
    s.scan_applied = 0u;
    s.scan_refused = s.scan_failsafe = s.scan_disc_sent = s.scan_want_sent = 0u;
    s.acl_links = 0u;
    memset(s.links, 0, sizeof(s.links));
    s.cfm_ok.valid = s.auth_ok.valid = s.reject.valid = s.acl_peer.valid = s.name_asked.valid = 0u;
    free_session();
    if (reason != BT_XREC_FAULT_NONE) {
        s.last_fault = reason;
        set_state(BT_XREC_ST_DEAD);
        bt_xrec_plat_log_u32("BT shutdown: hard path, reason", reason);
        bt_xrec_plat_hard_path(reason);        /* target: finalise recording, fault record 0xB1, reboot */
        return (int)reason;
    }
    bt_xrec_plat_session_end();
    set_state(BT_XREC_ST_OFF);
    bt_xrec_plat_log("BT stopped");
    return 0;
}

static void fault(uint32_t reason)
{
    if (s.state == BT_XREC_ST_OFF || s.state == BT_XREC_ST_FAULTED || s.state == BT_XREC_ST_DEAD) {
        return;
    }
    s.last_fault = reason;
    bt_xrec_plat_log_u32("BT fault, reason", reason);
    if (bt_shutdown() == 0) {
        set_state(BT_XREC_ST_FAULTED);         /* graceful recovery done: BT off until the next bt_xrec_start() */
    }
}

/* ============================================ public API ======================================================== */
int bt_xrec_start(exfat_vol_t *vol, int sd_mounted)
{
    if (s.state == BT_XREC_ST_UP || s.state == BT_XREC_ST_CONFIG || s.state == BT_XREC_ST_READY) {
        return 0;
    }
    if (s.state == BT_XREC_ST_DEAD || !bt_xrec_plat_start_allowed()) {
        bt_xrec_plat_log("BT start refused: BT fault recorded this boot");
        return -1;
    }
    if (bt_hp_active()) {
        bt_xrec_plat_log("BT start refused: headphones mode is running (one controller owner)");
        return -1;
    }
    bufs_t *b = (bufs_t *)bt_xrec_plat_alloc(sizeof(bufs_t));
    if (b == NULL) {
        bt_xrec_plat_log("BT start failed: no memory");
        return -1;
    }
    memset(b, 0, sizeof(*b));

    uint32_t last_fault = s.last_fault;
    memset(&s, 0, sizeof(s));
    s.last_fault = last_fault;
    s_spp_init_posted = 0u;
    bt_xrec_plat_identity(s.bd);
    static const char hex[] = "0123456789ABCDEF";
    memcpy(s.name, "Cassini_", 8u);            /* legacy naming scheme F:2005-2012 */
    s.name[8] = hex[s.bd[0] >> 4];
    s.name[9] = hex[s.bd[0] & 0x0Fu];
    s.name[10] = '\0';

    xrec_ops_t ops = *bt_xrec_plat_device_ops();
    ops.send = xo_send;
    ops.get_entropy = xo_entropy;
    ops.on_bound_key = xo_bound_key;
    ops.on_reboot = xo_reboot;
    ops.on_ota = xo_ota;
    ops.on_dfu = xo_dfu;
    ops.get_mtu = xo_get_mtu;
    xrec_init(&b->xrec, sd_mounted ? vol : 0, &ops, 0);
    bt_xrec_plat_load_bound_key(&b->xrec);

    bt_xrec_plat_lock();
    s.b = b;
    s.cb_open = 1u;
    bt_xrec_plat_unlock();

    bt_xrec_plat_log("BT start (Bluedroid):");
    bt_xrec_plat_log(s.name);
    bt_xrec_plat_set_mode(BT_XREC_MODE_PHONE, NULL);   /* bt_bonds.bin, SPP teardown only */
    if (bt_xrec_plat_stack_up_begin(vol, sd_mounted, s.bd) != 0) {
        bt_xrec_plat_log("BT start failed: platform");
        free_session();
        return -1;
    }
    set_state(BT_XREC_ST_UP);
    return 0;
}

void bt_xrec_open_pairing(uint32_t duration_ms)
{
    if (s.state != BT_XREC_ST_UP && s.state != BT_XREC_ST_CONFIG && s.state != BT_XREC_ST_READY) {
        return;
    }
    /* Plan M2.3 gate: no SSP part (discoverable, numeric comparison) while the platform cannot back a new pairing with
     * credited entropy (DRBG seeded, ROM P-192 key replaced, ROM RNG pool fed); without any entropy nothing opens.
     * Before READY the controller has not started yet, so the ROM key cannot have been replaced: the gate is evaluated
     * once READY (bt_xrec_poll), e.g. for the SD bench run that opens the window right after bt_xrec_start. */
    const char *why = NULL;
    uint8_t defer = (uint8_t)(s.state != BT_XREC_ST_READY);
    if (!defer) {
        why = ssp_block();
        if (why != NULL) {
            s.d.ssp_gate_refused++;
            log_gate("BT SSP pairing window refused: ", why);
        }
    }
    uint32_t now = bt_xrec_plat_now_ms();
    /* ENTER again while a window with its PIN is still well open (a bounce, an impatient user): the deadline moves,
     * the PIN stays, so a phone typing it is not cut off by a new one (review finding). */
    bool keep_pin = s.window_open && s.xrec_pin_issued && s.b != NULL && s.b->xrec.pair_active &&
                    !time_reached(now, s.window_deadline) && s.window_deadline - now > BT_XREC_PIN_KEEP_MS;
    s.window_open = 1u;
    s.ssp_window = (!defer && why == NULL) ? 1u : 0u;
    s.ssp_deferred = defer;
    s.window_deadline = now + (duration_ms ? duration_ms : 60000u);
    if (keep_pin) {
        bt_xrec_plat_log("BT XREC pairing window extended, PIN kept");
        apply_scan_mode();
        return;
    }
    /* XREC PIN from the DRBG (rejection sampling: no modulo bias). Fails closed: the SSP window still opens, but
     * XREC PAIR answers PAIRCLOSED until entropy is credited. */
    uint32_t v = 0u;
    bool ok = false;
    for (int i = 0; i < 4 && !ok; i++) {
        uint8_t r4[4];
        if (bt_xrec_plat_entropy(r4, sizeof(r4)) != 0) {
            break;
        }
        v = (uint32_t)r4[0] | ((uint32_t)r4[1] << 8) | ((uint32_t)r4[2] << 16) | ((uint32_t)r4[3] << 24);
        ok = v < 4294000000u;                   /* largest multiple of 10^6 below 2^32 */
    }
    if (ok && s.b != NULL) {
        char pin[8];
        digits6(pin, v);
#if BENCH_IMAGE
        if (s_bench_pin[0] != '\0') {           /* BENCH ONLY: the operator's fixed PIN (still only with entropy) */
            memcpy(pin, s_bench_pin, sizeof(pin));
            bt_xrec_plat_log("BT XREC BENCH PIN: fixed PIN from SD:/BTBENCH.RUN for this window (bench only)");
        }
#endif
        xrec_open_pairing(&s.b->xrec, pin);
        s.xrec_pin_issued = 1u;
        wipe_mem(pin, sizeof(pin));
    } else if (why == NULL) {
        bt_xrec_plat_log(defer ? "BT XREC PIN unavailable: entropy not ready (SSP pairing window once READY)"
                               : "BT XREC PIN unavailable: entropy not ready (SSP pairing window open)");
    } else {
        bt_xrec_plat_log("BT XREC PIN unavailable: entropy not ready (no pairing window)");
        s.window_open = 0u;                     /* nothing to offer */
    }
    apply_scan_mode();
}

void bt_xrec_poll(void)
{
    if (s.state == BT_XREC_ST_OFF || s.state == BT_XREC_ST_FAULTED || s.state == BT_XREC_ST_DEAD) {
        return;
    }
    uint32_t now = bt_xrec_plat_now_ms();
    if (s.state == BT_XREC_ST_UP) {
        int r = 0;
        if (bt_xrec_plat_job_done(&r)) {
            if (r != 0) {
                bt_xrec_plat_log_u32("BT bring-up failed, code", (uint32_t)r);
                fault(BT_XREC_FAULT_UP_FAIL);
                return;
            }
            config_begin();
        } else if (now - s.t_state >= BT_XREC_UP_DEADLINE_MS) {
            fault(BT_XREC_FAULT_UP_TIMEOUT);
            return;
        }
        if (s.state != BT_XREC_ST_CONFIG) {
            return;
        }
    }

    ctrl_notes_drain();                         /* before the GAP events: see ctrl_notes_drain() */
    drain_events();
    if (s.state != BT_XREC_ST_CONFIG && s.state != BT_XREC_ST_READY) {
        return;
    }
    bt_xrec_plat_lock();
    uint8_t lost = s.evq_lost;
    bt_xrec_plat_unlock();
    if (lost) {
        fault(BT_XREC_FAULT_EVQ_OVERFLOW);
        return;
    }
    now = bt_xrec_plat_now_ms();
    if (s.state == BT_XREC_ST_CONFIG && now - s.t_state >= BT_XREC_CONFIG_DEADLINE_MS) {
        fault(BT_XREC_FAULT_CONFIG_TIMEOUT);
        return;
    }

    /* pairing window: deadline, or closed by xrec_proto (PAIR success, 5 wrong PINs) like legacy F:2107-2118 */
    if (s.window_open && (time_reached(now, s.window_deadline) ||
                          (s.xrec_pin_issued && s.b != NULL && !s.b->xrec.pair_active))) {
        window_close();
    }
    if (s.ssp_showing && time_reached(now, s.ssp_deadline)) {
        s.ssp_showing = 0u;
    }
    if (s.ssp_deferred && s.state == BT_XREC_ST_READY) {
        s.ssp_deferred = 0u;                    /* window opened before READY: its SSP part is decided now */
        if (s.window_open) {
            const char *why = ssp_block();
            if (why == NULL) {
                s.ssp_window = 1u;
                bt_xrec_plat_log("BT SSP pairing window open (opened before READY)");
            } else {
                s.d.ssp_gate_refused++;
                log_gate("BT SSP pairing window refused: ", why);
                if (!s.xrec_pin_issued) {
                    bt_xrec_plat_log("BT XREC PIN unavailable: entropy not ready (no pairing window)");
                    window_close();             /* nothing to offer */
                }
            }
        }
    }
    if (s.ssp_window) {                        /* plan M2.3: the SSP part closes when the gate stops holding (e.g. */
        const char *why = ssp_block();          /* the ROM RNG pool ran dry during a pairing); a pending */
        if (why != NULL) {                      /* prompt is rejected with it (prompt_poll) */
            s.ssp_window = 0u;
            log_gate("BT SSP pairing window closed: ", why);
        }
    }
    prompt_poll(now);
    apply_scan_mode();

    if (s.entropy_fail && s.conn_open && !s.conn_closing) {
        conn_close_request("BT XREC close: entropy not ready");
    }
    close_retry_poll(now);
    if (s.state != BT_XREC_ST_CONFIG && s.state != BT_XREC_ST_READY) {
        return;                                  /* close_retry_poll() faulted the session */
    }
    if (s.conn_open && s.conn_authorized && !s.conn_closing) {
        xrec_poll(&s.b->xrec);
    }
    tx_kick();
    xrec_reset_flush();

    if (s.reboot_mode != 0u && time_reached(now, s.reboot_at) &&
        ((s.tx_head == s.tx_tail && s.inflight == 0u) || now - s.reboot_at >= BT_XREC_REBOOT_DRAIN_MAX_MS)) {
        uint8_t mode = s.reboot_mode;
        s.reboot_mode = 0u;
        bt_xrec_plat_log_u32("BT XREC deferred reboot, mode", mode);
        (void)bt_shutdown();                    /* coordinated teardown before any reboot (plan M4) */
        bt_xrec_plat_reboot(mode);
        return;
    }

    ctrl_notes_drain();
    uint32_t why = bt_xrec_plat_poll();
    if (why != 0u) {
        bt_xrec_plat_log_u32("BT platform fault", why);
        fault(BT_XREC_FAULT_PLATFORM);
    }
}

void bt_xrec_stop(void)
{
    (void)bt_shutdown();
}

/* Same fields and meaning as legacy F:2178-2197. rx/tx_acl_pkts count SPP data events / esp_spp_write calls
 * (Bluedroid hides ACL packets); main.c only compares them between iterations (activity) and uses tx for progress. */
void bt_xrec_get_status(bt_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->active = (uint8_t)(s.state == BT_XREC_ST_UP || s.state == BT_XREC_ST_CONFIG || s.state == BT_XREC_ST_READY);
    out->hci_ready = (uint8_t)(s.state == BT_XREC_ST_READY);
    out->acl_connected = (uint8_t)ui_connected();   /* the BT screen's "connected" (main.c bt_state) */
    out->spp_open = (uint8_t)(s.conn_open && !s.conn_closing);
    if (s.b != NULL) {
        out->xrec_bound = s.b->xrec.bound;
        out->xrec_auth = s.b->xrec.auth;
    }
    out->pair_active = s.window_open;
    if (s.ssp_showing) {
        memcpy(out->pair_pin, s.ssp_digits, sizeof(out->pair_pin));   /* BENCH autoconfirm: value accepted */
    } else if (s.window_open && s.b != NULL && s.b->xrec.pair_active) {
        memcpy(out->pair_pin, s.b->xrec.pair_pin, sizeof(out->pair_pin));
    }
    bt_xrec_plat_lock();
    out->rx_acl_pkts = s.rx_frames;
    bt_xrec_plat_unlock();
    out->tx_acl_pkts = s.d.tx_writes;
    out->irq_count = out->active ? bt_xrec_plat_irq_count() : 0u;
    memcpy(out->bd_addr, s.bd, 6u);
    memcpy(out->dev_name, s.name, sizeof(out->dev_name));
}

void bt_xrec_get_debug(bt_xrec_debug_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s.d;
    out->state = s.state;
    out->window_open = s.window_open;
    out->scan_disc = (uint8_t)(s.scan_applied == 2u);
    out->scan_applied = s.scan_applied;
    out->conn_open = s.conn_open;
    out->conn_authorized = s.conn_authorized;
    out->conn_closing = s.conn_closing;
    out->cong = s.cong;
    out->spp_inited = s.spp_inited;
    out->ssp_showing = s.ssp_showing;
    out->entropy_fail = s.entropy_fail;
    out->reboot_mode = s.reboot_mode;
    out->ssp_window = s.ssp_window;
    out->ssp_pending = s.ssp_deferred;
    out->ssp_prompt = s.cfm_pending;
    out->ssp_burned = s.ssp_burned;
    out->conn_handle = s.conn_handle;
    out->tx_used = s.tx_head - s.tx_tail;
    out->inflight = s.inflight;
    out->acl_links = s.acl_links;
    out->acl_closing = links_closing();
    out->mtu = s.mtu;
    out->last_fault = s.last_fault;
    bt_xrec_plat_lock();
    out->rx_used = s.rx_head - s.rx_tail;
    out->evq_used = s.ev_head - s.ev_tail;
    out->rx_frames = s.rx_frames;
    out->rx_overflows = s.rx_overflows;
    out->rx_dropped = s.d.rx_dropped + s.rx_dropped_cb;
    out->evq_lost = s.evq_lost_n;
    bt_xrec_plat_unlock();
}

static uint32_t secs_left(uint32_t now, uint32_t at)
{
    if (time_reached(now, at)) {
        return 0u;
    }
    return (at - now + 999u) / 1000u;
}

void bt_xrec_get_ui(bt_xrec_ui_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s.state != BT_XREC_ST_UP && s.state != BT_XREC_ST_CONFIG && s.state != BT_XREC_ST_READY) {
        return;
    }
    uint32_t now = bt_xrec_plat_now_ms();
    const uint8_t *peer = NULL;
    if (s.cfm_pending) {
        uint32_t el = now - s.cfm_t0;
        out->prompt = 1u;
        out->prompt_armed = (uint8_t)(el >= BT_XREC_SSP_ARM_MS);
        out->prompt_s_left = (uint8_t)secs_left(now, s.cfm_t0 + BT_XREC_SSP_PROMPT_MS);
        memcpy(out->digits, s.cfm_digits, sizeof(out->digits));
        peer = s.cfm_bda;
    } else if (s.conn_open && !s.conn_closing) {
        peer = s.conn_bda;
    } else if (s.auth_ok.valid) {
        peer = s.auth_ok.bda;                     /* just bonded, SPP not open yet */
    } else if (s.acl_peer.valid) {
        peer = s.acl_peer.bda;
    }
    out->window_open = s.window_open;
    out->window_s_left = s.window_open ? (uint16_t)secs_left(now, s.window_deadline) : 0u;
    out->connected = (uint8_t)ui_connected();
    if (peer != NULL) {
        memcpy(out->peer_bda, peer, 6u);
        out->bonded = (uint8_t)(!s.cfm_pending && (slot_is(&s.auth_ok, peer) ||
                                (s.conn_open && s.conn_authorized && !s.conn_closing && bda_eq(s.conn_bda, peer))));
        bt_xrec_plat_lock();
        bool named = s.nm_valid && bda_eq(s.nm_bda, peer);
        if (named) {
            memcpy(out->peer, s.nm, sizeof(out->peer));
            out->peer_is_name = 1u;
        }
        bt_xrec_plat_unlock();
        if (!named) {
            bda_text(out->peer, peer);
        }
    }
}

int bt_xrec_ssp_answer(int accept)
{
    uint32_t now = bt_xrec_plat_now_ms();
    if (!s.cfm_pending) {
        /* the prompt ended by itself (timeout, peer gave up, window/gate closed) or was just answered: a key within
         * BT_XREC_SSP_AFTER_MS was meant for it, not for "leave BT mode" / "open a new window" (review finding) */
        return (s.cfm_ended && now - s.cfm_end_t < BT_XREC_SSP_AFTER_MS) ? -3 : -1;
    }
    if (now - s.cfm_t0 < BT_XREC_SSP_ARM_MS) {
        return -2;                                /* a key pressed as the prompt appeared: not an answer */
    }
    const char *why = (accept && s.ssp_window && s.state == BT_XREC_ST_READY) ? ssp_block() : NULL;
    if (why != NULL) {
        /* the gate stopped holding since the last poll (e.g. the ROM RNG pool ran dry at this pairing's nonce) */
        s.ssp_window = 0u;
        s.d.ssp_gate_cfm_rejects++;
        log_gate("BT SSP pairing window closed: ", why);
        prompt_reject("BT SSP numeric comparison rejected: pairing gate closed");
    } else if (accept && s.ssp_window && s.state == BT_XREC_ST_READY) {
        cfm_reply(s.cfm_bda, true);
        cfm_accepted(s.cfm_bda);
        s.d.ssp_user_accepted++;
        s.cfm_pending = 0u;
        memset(s.cfm_digits, 0, sizeof(s.cfm_digits));
        s.cfm_ended = 1u;
        s.cfm_end_t = now;
        bt_xrec_plat_log_u32("BT SSP numeric comparison accepted by the user, value", s.cfm_val);
        prompt_end_log();
    } else {
        s.d.ssp_user_rejected += accept ? 0u : 1u;
        prompt_reject(accept ? "BT SSP numeric comparison rejected: pairing window closed"
                             : "BT SSP numeric comparison rejected by the user");
    }
    return 0;
}

#if BENCH_IMAGE
void bt_xrec_set_bench_autoconfirm(int on)
{
    s_bench_autoconfirm = on ? 1u : 0u;
    bt_xrec_plat_log(on ? "BT SSP BENCH autoconfirm ON: numeric comparisons in the window accepted without the user"
                        : "BT SSP BENCH autoconfirm off");
}

void bt_xrec_set_bench_xrec_pin(const char *pin6)
{
    bool ok = pin6 != NULL;
    for (int i = 0; ok && i < 6; i++) {
        ok = pin6[i] >= '0' && pin6[i] <= '9';
    }
    ok = ok && pin6[6] == '\0';
    memset(s_bench_pin, 0, sizeof(s_bench_pin));
    if (ok) {
        memcpy(s_bench_pin, pin6, 6u);
    }
    bt_xrec_plat_log(ok ? "BT XREC BENCH PIN ON: pairing windows use the bench PIN" : "BT XREC BENCH PIN off");
}
#endif

/* The teardown job (bt_xrec_fw.c) waits for ESP_SPP_UNINIT_EVT before esp_bluedroid_disable(). */
int bt_xrec_spp_uninit_seen(void)
{
    return s_spp_uninit_seen != 0u;
}

int bt_xrec_spp_deinit_needed(void)
{
    return s_spp_init_posted != 0u;
}

#if defined(BT_XREC_TEST_HOOKS) && BT_XREC_TEST_HOOKS
/* Host tests only (app task, via bthx_call_fn): the device closes the open connection, as an RX overflow or a write
 * error would (test_bt_host phone, step P7c: review STEP10 C11). */
int bt_xrec_test_close_request(uint32_t arg);
int bt_xrec_test_close_request(uint32_t arg)
{
    (void)arg;
    if (!s.conn_open || s.conn_closing) {
        return -1;
    }
    conn_close_request("BT XREC close: test request");
    return 0;
}

/* Host tests only: forget a DEAD session (on target the hard path reboots). */
void bt_xrec_test_reset(void);
void bt_xrec_test_reset(void)
{
    if (s.b != NULL) {
        wipe_free(s.b);
    }
    memset(&s, 0, sizeof(s));
    s_spp_uninit_seen = 0u;
#if BENCH_IMAGE
    s_bench_autoconfirm = 0u;
    memset(s_bench_pin, 0, sizeof(s_bench_pin));
#endif
    s_spp_init_posted = 0u;
}
#endif
