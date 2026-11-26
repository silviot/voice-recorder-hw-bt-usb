/* bt_hp: headphones session (A2DP source + AVRCP) on Bluedroid, see bt_hp.h and docs/BT_A2DP_DESIGN.md §6.
 *
 * Contexts: BTC task (gap_cb, a2d_cb, avrc_ct_cb, avrc_tg_cb, tx_event, openfw_just_works_allowed) copy events into
 * the locked ring; a2d_cb also drives hp_tx_bd_* (design §4.3-4.5). Everything else runs in the app task. hp_down()
 * runs in the bt_recover worker as the down job's profile teardown (bt_xrec_plat_set_mode).
 *
 * Memory: the session block (event ring, discovery list, PCM sink) and the PCM ring come from the heap at start and
 * go back at stop; the few statics live in SRAM_HI (.bss_hi): .bt_bss (this archive's default .bss) is carved from
 * the RTOS heap region, whose 32 KiB floor has < 1 KiB to spare (sl6801_firm.ld).
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "openfw.h"
#include "esp_err.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "bt_ctrl.h"
#include "bt_ctrl_cmds.h"
#include "bt_hp.h"
#include "bt_xrec_bluedroid.h"
#include "hp_sink.h"
#include "hp_tx.h"
#include "hp_tx_bd.h"

#if defined(OPENFW_BT_BENCH_IMAGE) && OPENFW_BT_BENCH_IMAGE
#define BENCH_IMAGE 1
#else
#define BENCH_IMAGE 0
#endif

#if defined(OPENFW_BT_HOST_HARNESS) || defined(BT_HP_HOST)
#define HP_HI
#else
#define HP_HI __attribute__((section(".bss_hi"), aligned(4)))
#endif

#define HP_EVQ_SLOTS     48u
#define HP_KEYQ          8u
#define HP_RING_BYTES    16384u   /* PCM ring (design §3.3): 4096 stereo / 8192 mono frames */
#define HP_CFG_MAGIC     0x31504842u  /* "BHP1" */
#define HP_RC_STEPS      5u
#define HP_BOND_LIST_MAX 8u       /* bonds read back to check the remembered headset still has a key */
static const uint16_t k_rc_backoff_s[HP_RC_STEPS] = { 0u, 3u, 10u, 30u, 60u };   /* design §6.5; then every 60 s */

/* BTC callbacks hand a 4-aligned copy (fine on Cortex-M4; x86-64 UBSan wants 8: see bt_xrec_bluedroid.c) */
#define BTC_CB_ARG_ALIGN4 __attribute__((no_sanitize("alignment")))

enum {
    EV_DISC_STATE = 1, EV_DISC_RES, EV_AUTH, EV_ACL_UP, EV_ACL_DOWN, EV_CFM_REQ, EV_PIN_REQ, EV_KEY_REQ, EV_KEY_NOTIF,
    EV_JW, EV_NAME,
    EV_A2D_CONN, EV_A2D_AUDIO, EV_A2D_CFG, EV_A2D_ACK, EV_A2D_PROF, EV_A2D_SEP, EV_A2D_DELAY, EV_A2D_CAPS,
    EV_CT_CONN, EV_CT_CAPS, EV_CT_NTF, EV_CT_VOL, EV_CT_FEAT, EV_TG_CONN, EV_TG_KEY, EV_TG_VOL, EV_TX_STALL,
};

typedef struct {
    uint8_t  type, a, b, c;
    uint8_t  bda[6];
    uint16_t h;
    uint32_t v;
} ev_t;

/* Media control (design §4.5): one command outstanding, a one-deep "wanted" state. */
enum { MC_NONE = 0, MC_START, MC_SUSPEND };

typedef struct {
    /* ---- shared with BTC: under bt_xrec_plat_lock() ---- */
    uint8_t  cb_open;
    uint8_t  evq_lost;
    ev_t     evq[HP_EVQ_SLOTS];
    uint32_t ev_head, ev_tail;
    bt_hp_found_t found[BT_HP_FOUND_MAX];
    uint8_t  n_found;
    uint8_t  win_open;                 /* pairing window (read by the Just Works hook) */
    uint8_t  win_bda[6];
    uint32_t win_deadline;
    uint8_t  gate_ok;                  /* the M2.3 gate held at the last poll (the hook cannot call the platform; it
                                          reads the gate's RNG latch live, bt_xrec_plat_rng_starved) */
    uint8_t  jw_accepted;              /* a Just Works pairing was accepted for win_bda */
    uint8_t  acl_valid;
    uint8_t  acl_bda[6];
    uint16_t acl_handle;
    uint8_t  a2d_peer_valid;           /* the app's A2DP target (target / target_valid), mirrored for a2d_cb: a late */
    uint8_t  a2d_peer[6];              /* CONNECTED / DISCONNECTED / AUDIO_CFG of another peer leaves TX alone (C13) */
    uint8_t  start_pending;            /* we asked for START: the next STARTED is ours (design §4.5) */
    uint8_t  suspend_pending;          /* we asked for SUSPEND: the next SUSPEND is ours, not the headset's. Kept here,
                                          not in mc_out: the ext-codec source acks a SUSPEND (MEDIA_CTRL_ACK) before
                                          it reports the audio state, so on_a2d_ack has cleared mc_out by then */
    uint8_t  unasked_started;          /* a remote START: the SUSPEND that follows is not a remote suspend */
    uint8_t  cfg_valid;
    uint8_t  cfg_cie[4];               /* AUDIO_CFG arrives before CONNECTED (no MTU yet): applied again there */
    uint32_t cfg_rate;
    uint8_t  cfg_bp_min, cfg_bp_max;
    uint8_t  keyq[HP_KEYQ];
    uint32_t kq_head, kq_tail;
    uint32_t player_ev;
    uint8_t  rn_bda[6];                /* the last READ_REMOTE_NAME_EVT (its EV_NAME carries only bda and status) */
    char     rn_name[BT_HP_NAME_MAX];

    /* ---- app task only ---- */
    uint8_t  state, link;
    uint32_t t_state;
    uint8_t  bd[6];
    char     dev_name[16];
    uint8_t  a2d_inited, avrc_inited, seps_ok, seps_pending, prof_init;
    uint8_t  scanning, scan_cancel;
    uint32_t scan_until;
    uint32_t scan_cancel_until;        /* a cancel whose "discovery stopped" never came is cleared then (STEP10 C16) */
    uint8_t  pick_pending, pick_pair;
    uint8_t  pick_bda[6];
    /* name requests (user report 2026-11-25: a headset whose name the inquiry's name pass missed stayed a MAC for
     * ever in the list, the Forget row and bt_hp_cfg.bin): one esp_bt_gap_read_remote_name at a time */
    uint8_t  rn_busy;
    uint32_t rn_t0;
    uint8_t  rn_want;                  /* the connected headset's name is unknown: ask over its link */
    uint8_t  rn_want_bda[6];
    uint8_t  rn_tried[BT_HP_FOUND_MAX];   /* found[i] was asked once after the search (name pass) */
    uint8_t  pair_again;               /* reconnects stopped until the user picks a headset (BT_HP_MSG_PAIR_AGAIN) */
    uint8_t  forgot_valid;             /* Forget: a late CONNECTED of this device is closed, not remembered again */
    uint8_t  forgot[6];
    uint32_t pick_since;
    uint8_t  target_valid;
    uint8_t  target[6];
    char     target_name[BT_HP_NAME_MAX];
    uint32_t connect_t0;
    uint16_t conn_hdl;
    uint8_t  user_disc;                /* the user dropped the headset: no reconnect */
    uint8_t  rc_active, rc_step;
    uint32_t rc_at;
    uint8_t  mc_out, mc_want;
    uint32_t mc_t;
    uint8_t  start_retries;
    uint32_t start_retry_at;
    uint8_t  start_failed;
    uint32_t start_failed_at;
    uint8_t  start_demand;             /* demand arrived while start_failed: START again after the cooldown */
    uint8_t  susp_retries, susp_retry_due;   /* refused SUSPENDs retried (audit r3 item 5) */
    uint32_t susp_refused_at;
    uint32_t last_real_pcm, last_real_t;
    uint8_t  ct_conn, abs_vol, ct_tl;
    uint16_t ct_hdl;                   /* the AVRCP CT connection's handle (AVCTP may open before A2DP CONNECTED) */
    uint8_t  orphan;                   /* a connect we gave up on (timeout) may still complete in Bluedroid */
    uint8_t  orphan_bda[6];
    uint32_t orphan_until;
    uint8_t  ssp_burned;
    uint8_t  stall_disc;
    uint32_t stall_t;
    uint8_t  msg;
    uint32_t msg_seq;
    uint32_t last_fault;
    uint32_t stat_t;
    /* PCM sink + ring */
    hp_sink_t sink;
    int16_t  *ring;
    uint8_t  ring_is_dac;
    bt_hp_debug_t d;
} hp_t;

/* persisted settings (SD:/bt_hp_cfg.bin) */
typedef struct {
    uint32_t magic;
    uint8_t  version, out_mode, bt_vol, last_valid;
    uint8_t  last[6];
    char     last_name[BT_HP_NAME_MAX];
    uint8_t  pad;
} hp_cfg_t;

static hp_t *g HP_HI;
static hp_cfg_t s_cfg HP_HI;
static uint8_t s_cfg_loaded HP_HI;
static uint8_t s_state HP_HI;          /* session state, readable without g (OFF / FAULTED / DEAD) */
static uint32_t s_last_fault HP_HI;
static uint8_t s_restart_required HP_HI; /* A2DP deinit never confirmed: no new session this boot (STEP10 C14) */
static volatile uint8_t s_a2d_deinit_seen HP_HI;
static volatile uint8_t s_a2d_down HP_HI, s_avrc_down HP_HI;   /* what the down job must deinit */
static volatile uint32_t s_down_ms HP_HI;
#if BENCH_IMAGE
static bt_hp_bench_tun_t s_tun HP_HI;
static uint8_t s_tun_set HP_HI;
#endif
static exfat_vol_t *s_start_vol HP_HI;   /* bt_hp_start() arguments, for the stall escalation's restart */
static int s_start_sd HP_HI;
static uint8_t s_auto_restart HP_HI;     /* restart once the session is FAULTED (stall escalation, design §4.6) */
static uint32_t s_auto_restart_t HP_HI;
static uint8_t s_auto_restart_done HP_HI;
static char s_line[128] HP_HI;
static uint32_t s_ll HP_HI;

static int hp_profile_down(void);

/* ============================================ log lines ======================================================= */
static void lc(char c)
{
    if (s_ll < sizeof(s_line) - 1u) {
        s_line[s_ll++] = c;
    }
}
static void ls(const char *p)
{
    while (*p != '\0') {
        lc(*p++);
    }
}
static void lu(uint32_t v)
{
    char b[10];
    int i = 0;
    do {
        b[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && i < 10);
    while (i > 0) {
        lc(b[--i]);
    }
}
static void lx2(uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    lc(hx[v >> 4]);
    lc(hx[v & 0xFu]);
}
static void lbegin(const char *tag)
{
    s_ll = 0u;
    ls("BT hp ");
    ls(tag);
}
static void lku(const char *k, uint32_t v)
{
    lc(' ');
    ls(k);
    lc('=');
    lu(v);
}
static void lks(const char *k, const char *v)
{
    lc(' ');
    ls(k);
    lc('=');
    ls(v);
}
static void lkx(const char *k, uint32_t v, int digits)
{
    lc(' ');
    ls(k);
    ls("=0x");
    for (int i = digits - 2; i >= 0; i -= 2) {
        lx2((uint8_t)(v >> (4 * i)));
    }
}
/* Peer address cut to its last 3 bytes (design §10.2: logs never carry full peer addresses). */
static void lka(const char *k, const uint8_t bda[6])
{
    lc(' ');
    ls(k);
    ls("=..:");
    lx2(bda[3]);
    lc(':');
    lx2(bda[4]);
    lc(':');
    lx2(bda[5]);
}
static void lend(void)
{
    s_line[s_ll] = '\0';
    bt_xrec_plat_log(s_line);
}
static void log1(const char *tag)
{
    lbegin(tag);
    lend();
}

/* ============================================ helpers ========================================================= */
static bool bda_eq(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6u) == 0; }
static bool time_reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

/* App task: g->target changed (or is about to be paged): mirror it for a2d_cb (STEP10 C13). */
static void a2d_peer_set(const uint8_t bda[6])
{
    bt_xrec_plat_lock();
    g->a2d_peer_valid = 1u;
    memcpy(g->a2d_peer, bda, 6u);
    bt_xrec_plat_unlock();
}

static void target_set(const uint8_t bda[6])
{
    g->target_valid = 1u;
    memcpy(g->target, bda, 6u);
    a2d_peer_set(bda);
}

/* BTC (a2d_cb): the event is about another device than the app's A2DP target, i.e. a late result of a connect the app
 * gave up on (timeout, new pick). The app ignores or closes it (on_a2d_conn); the transmitter belongs to the target's
 * stream and is not touched (STEP10 C13: such a DISCONNECTED stopped the current stream's tick, a CONNECTED replaced
 * its connection handle, an AUDIO_CFG its encoder configuration). No target yet: nothing to protect. */
static bool a2d_stale_peer(const uint8_t bda[6])
{
    bool stale;
    bt_xrec_plat_lock();
    stale = g != NULL && g->a2d_peer_valid && !bda_eq(g->a2d_peer, bda);
    bt_xrec_plat_unlock();
    return stale;
}

static void set_state(uint8_t st)
{
    s_state = st;
    if (g != NULL) {
        g->state = st;
        g->t_state = bt_xrec_plat_now_ms();
    }
}

static void set_msg(uint8_t m)
{
    if (g != NULL) {
        g->msg = m;
        g->msg_seq++;
    }
}

static void name_copy(char *out, size_t cap, const uint8_t *in, size_t in_max)
{
    size_t n = 0u;
    while (n < in_max && n + 1u < cap && in[n] != 0u) {
        uint8_t c = in[n];
        out[n] = (c < 0x20u || c == 0x7Fu) ? '?' : (char)c;
        n++;
    }
    size_t k = n;                      /* never end inside a UTF-8 sequence (as bt_xrec_bluedroid.c name_copy) */
    while (k > 0u && ((uint8_t)out[k - 1u] & 0xC0u) == 0x80u) {
        k--;
    }
    if (k > 0u && ((uint8_t)out[k - 1u] & 0x80u) != 0u) {
        uint8_t lead = (uint8_t)out[k - 1u];
        size_t need = lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : lead >= 0xC0u ? 2u : 1u;
        if (n - (k - 1u) < need || (lead & 0xC0u) != 0xC0u) {
            n = k - 1u;
        }
    }
    out[n] = '\0';
}

/* Design §6.2 CoD filter: major Audio/Video, or the Rendering / Audio service bits. */
static uint8_t cod_is_audio(uint32_t cod)
{
    if (cod == 0u) {
        return 0u;
    }
    uint32_t srv = esp_bt_gap_get_cod_srvc(cod);
    return (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV ||
            (srv & (ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO)) != 0u) ? 1u : 0u;
}

/* ============================================ settings file =================================================== */
static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic = HP_CFG_MAGIC;
    s_cfg.version = 1u;
    s_cfg.out_mode = BT_HP_OUT_SPEAKER;
    s_cfg.bt_vol = BT_HP_VOL_DEF;
}

static void cfg_load(void)
{
    if (s_cfg_loaded) {
        return;
    }
    s_cfg_loaded = 1u;
    cfg_defaults();
    hp_cfg_t c;
    uint32_t n = 0u;
    if (bt_hp_plat_cfg_load((uint8_t *)&c, sizeof(c), &n) == 0 && n == sizeof(c) && c.magic == HP_CFG_MAGIC &&
        c.version == 1u && c.out_mode <= BT_HP_OUT_BOTH && c.bt_vol <= 127u) {
        c.last_name[BT_HP_NAME_MAX - 1u] = '\0';
        s_cfg = c;
    }
}

static void cfg_save(void)
{
    if (bt_hp_plat_cfg_save((const uint8_t *)&s_cfg, sizeof(s_cfg)) != 0) {
        log1("settings not saved (SD write failed)");
    }
}

uint8_t bt_hp_out_mode(void)
{
    cfg_load();
    return s_cfg.out_mode;
}

void bt_hp_set_out_mode(uint8_t mode)
{
    cfg_load();
    if (mode > BT_HP_OUT_BOTH || mode == s_cfg.out_mode) {
        return;
    }
    s_cfg.out_mode = mode;
    cfg_save();
}

uint8_t bt_hp_vol31(void)
{
    cfg_load();
    return (uint8_t)((s_cfg.bt_vol * 31u + 63u) / 127u);
}

/* ============================================ BTC side ======================================================== */
static bool push_locked(const ev_t *e)
{
    if (g->ev_head - g->ev_tail >= HP_EVQ_SLOTS) {
        g->evq_lost = 1u;
        g->d.evq_lost++;
        return false;
    }
    g->evq[g->ev_head % HP_EVQ_SLOTS] = *e;
    g->ev_head++;
    return true;
}

static void post(const ev_t *e)
{
    bool queued = false;
    bt_xrec_plat_lock();
    if (g != NULL && g->cb_open) {
        (void)push_locked(e);
        queued = true;
    }
    bt_xrec_plat_unlock();
    if (queued) {
        bt_xrec_plat_notify_app();
    }
}

/* Discovery result: the list is updated here (names arrive in later results of BTA's name pass). */
static void found_update_locked(const esp_bt_gap_cb_param_t *p)
{
    int idx = -1;
    for (int i = 0; i < (int)g->n_found; i++) {
        if (bda_eq(g->found[i].bda, p->disc_res.bda)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (g->n_found >= BT_HP_FOUND_MAX) {
            return;
        }
        idx = g->n_found++;
        memset(&g->found[idx], 0, sizeof(g->found[idx]));
        memcpy(g->found[idx].bda, p->disc_res.bda, 6u);
    }
    bt_hp_found_t *f = &g->found[idx];
    for (int i = 0; i < p->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t *pr = &p->disc_res.prop[i];
        if (pr->val == NULL) {
            continue;
        }
        if (pr->type == ESP_BT_GAP_DEV_PROP_COD && pr->len >= 4) {
            memcpy(&f->cod, pr->val, 4u);
            f->is_audio = cod_is_audio(f->cod);
        } else if (pr->type == ESP_BT_GAP_DEV_PROP_RSSI && pr->len >= 1) {
            f->rssi = *(const int8_t *)pr->val;
            f->has_rssi = 1u;
        } else if (pr->type == ESP_BT_GAP_DEV_PROP_BDNAME && pr->len > 0) {
            name_copy(f->name, sizeof(f->name), (const uint8_t *)pr->val, (size_t)pr->len);
            f->named = f->name[0] != '\0';
        }
    }
}

BTC_CB_ARG_ALIGN4
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p)
{
    ev_t e;
    memset(&e, 0, sizeof(e));
    if (p == NULL) {
        return;
    }
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        bt_xrec_plat_lock();
        if (g != NULL && g->cb_open) {
            found_update_locked(p);
        }
        bt_xrec_plat_unlock();
        e.type = EV_DISC_RES;
        memcpy(e.bda, p->disc_res.bda, 6u);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        e.type = EV_DISC_STATE;
        e.a = p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED ? 1u : 0u;
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        e.type = EV_CFM_REQ;
        memcpy(e.bda, p->cfm_req.bda, 6u);
        e.v = p->cfm_req.num_val;
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
        e.type = EV_AUTH;
        memcpy(e.bda, p->auth_cmpl.bda, 6u);
        e.a = (uint8_t)p->auth_cmpl.stat;
        e.b = p->auth_cmpl.lk_type;
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        e.type = EV_NAME;
        memcpy(e.bda, p->read_rmt_name.bda, 6u);
        e.a = (uint8_t)p->read_rmt_name.stat;
        bt_xrec_plat_lock();
        if (g != NULL && g->cb_open) {
            memcpy(g->rn_bda, p->read_rmt_name.bda, 6u);
            name_copy(g->rn_name, sizeof(g->rn_name), p->read_rmt_name.rmt_name, ESP_BT_GAP_MAX_BDNAME_LEN);
        }
        bt_xrec_plat_unlock();
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        e.type = EV_ACL_UP;
        e.a = (uint8_t)p->acl_conn_cmpl_stat.stat;
        e.h = p->acl_conn_cmpl_stat.handle;
        memcpy(e.bda, p->acl_conn_cmpl_stat.bda, 6u);
        if (p->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
            /* NOCP accounting needs the stream's ACL handle (design §4.4); a2d_cb binds it at CONNECTED */
            bt_xrec_plat_lock();
            if (g != NULL) {
                g->acl_valid = 1u;
                g->acl_handle = e.h;
                memcpy(g->acl_bda, e.bda, 6u);
            }
            bt_xrec_plat_unlock();
        }
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        e.type = EV_ACL_DOWN;
        e.a = (uint8_t)p->acl_disconn_cmpl_stat.reason;
        e.h = p->acl_disconn_cmpl_stat.handle;
        memcpy(e.bda, p->acl_disconn_cmpl_stat.bda, 6u);
        bt_xrec_plat_lock();
        if (g != NULL && g->acl_valid && g->acl_handle == e.h) {
            g->acl_valid = 0u;
        }
        bt_xrec_plat_unlock();
        break;
    default:
        return;
    }
    post(&e);
}

/* Patch 0036 (btc_dm.c, BTC task): Just Works only for the picked device inside its window, with the gate holding. */
int openfw_just_works_allowed(const uint8_t bda[6])
{
    /* 0 unless a headphones session is open: phone mode never keeps a Just Works key (it removes it at AUTH_CMPL, by
     * which time it has replaced the phone's MITM key) and a stopping session pairs nothing (STEP10 C4 / A3) */
    int allow = 0;
    bool hp = false;
    bt_xrec_plat_lock();
    if (g != NULL && g->cb_open) {
        hp = true;
        /* gate_ok is the last poll's gate; the RNG latch is read live: the ROM may have drawn this pairing's nonce
         * from a dry pool since that poll (STEP10 C3) */
        allow = (g->win_open && bda_eq(g->win_bda, bda) && g->gate_ok && !bt_xrec_plat_rng_starved() &&
                 !time_reached(bt_xrec_plat_now_ms(), g->win_deadline)) ? 1 : 0;
        if (allow) {
            g->jw_accepted = 1u;
        }
    }
    bt_xrec_plat_unlock();
    if (hp) {
        ev_t e;
        memset(&e, 0, sizeof(e));
        e.type = EV_JW;
        e.a = (uint8_t)allow;
        memcpy(e.bda, bda, 6u);
        post(&e);
    }
    return allow;
}

static void tx_event(int evt)    /* hp_tx tick (BTC) */
{
    if (evt == HP_TX_EVT_STALL) {
        ev_t e;
        memset(&e, 0, sizeof(e));
        e.type = EV_TX_STALL;
        post(&e);
    }
}

BTC_CB_ARG_ALIGN4
static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p)
{
    ev_t e;
    memset(&e, 0, sizeof(e));
    if (p == NULL) {
        return;
    }
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        e.type = EV_A2D_CONN;
        e.a = (uint8_t)p->conn_stat.state;
        e.b = (uint8_t)p->conn_stat.disc_rsn;
        e.h = p->conn_stat.conn_hdl;
        e.v = p->conn_stat.audio_mtu;
        memcpy(e.bda, p->conn_stat.remote_bda, 6u);
        if ((p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED ||
             p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) && a2d_stale_peer(p->conn_stat.remote_bda)) {
            break;                            /* not the target's link: on_a2d_conn ignores / closes it */
        }
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            hp_tx_bd_set_conn(p->conn_stat.conn_hdl, p->conn_stat.audio_mtu);
            uint16_t h = 0xFFFFu;
            uint8_t cie[4];
            bool have_cfg = false;
            bt_xrec_plat_lock();
            if (g != NULL && g->acl_valid && bda_eq(g->acl_bda, p->conn_stat.remote_bda)) {
                h = g->acl_handle;
            }
            if (g != NULL && g->cfg_valid) {
                memcpy(cie, g->cfg_cie, 4u);
                have_cfg = true;
            }
            bt_xrec_plat_unlock();
            hp_tx_bd_set_acl_handle(h);
            if (have_cfg) {
                e.c = hp_tx_bd_on_audio_cfg(cie) == 0 ? 1u : 0u;   /* now with the stream's audio MTU */
            }
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            hp_tx_bd_stopped(HP_STOP_LINK);   /* link loss or close: rewind snapshot (design §4.4) */
            hp_tx_bd_set_acl_handle(0xFFFFu);
            bt_xrec_plat_lock();
            if (g != NULL) {
                g->start_pending = 0u;
                g->suspend_pending = 0u;
                g->unasked_started = 0u;
                g->cfg_valid = 0u;
            }
            bt_xrec_plat_unlock();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT: {
        e.type = EV_A2D_AUDIO;
        e.a = (uint8_t)p->audio_stat.state;
        bool ours = false, unasked_susp = false, local_susp = false;
        bt_xrec_plat_lock();
        if (g != NULL) {
            if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ours = g->start_pending != 0u;
                g->start_pending = 0u;
                g->unasked_started = ours ? 0u : 1u;
            } else {
                unasked_susp = g->unasked_started != 0u;
                g->unasked_started = 0u;
                local_susp = g->suspend_pending != 0u;
                g->suspend_pending = 0u;
            }
        }
        bt_xrec_plat_unlock();
        if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            e.b = ours ? 1u : 0u;
            if (ours) {
                e.c = hp_tx_bd_started() == 0 ? 1u : 0u;   /* tick armed in BTC (design §4.3 lifecycle) */
            }
        } else {
            e.b = unasked_susp ? 1u : 0u;
            e.c = local_susp ? 1u : 0u;                    /* our SUSPEND (REC switch, idle), not the headset's */
            hp_tx_bd_stopped(HP_STOP_SUSPEND);
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        e.type = EV_A2D_CFG;
        uint8_t cie[4];
        memcpy(cie, &p->audio_cfg.mcc.cie, 4u);
        e.v = (uint32_t)cie[0] | ((uint32_t)cie[1] << 8) | ((uint32_t)cie[2] << 16) | ((uint32_t)cie[3] << 24);
        memcpy(e.bda, p->audio_cfg.remote_bda, 6u);
        if (a2d_stale_peer(p->audio_cfg.remote_bda)) {
            break;                            /* ok=0 in the log: another device's codec, not applied (C13) */
        }
        e.a = (uint8_t)(hp_tx_bd_on_audio_cfg(cie) == 0 ? 1u : 0u);
        hp_sbc_cfg_t c;
        if (hp_sbc_cfg_from_cie(cie, &c) == 0) {
            bt_xrec_plat_lock();
            if (g != NULL) {
                g->cfg_valid = 1u;
                memcpy(g->cfg_cie, cie, 4u);
                g->cfg_rate = c.rate;
                g->cfg_bp_min = c.min_bp;
                g->cfg_bp_max = c.max_bp;
            }
            bt_xrec_plat_unlock();
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        e.type = EV_A2D_ACK;
        e.a = (uint8_t)p->media_ctrl_stat.cmd;
        e.b = (uint8_t)p->media_ctrl_stat.status;
        if (p->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            bt_xrec_plat_lock();
            if (g != NULL) {
                if (p->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START) {
                    g->start_pending = 0u;
                } else if (p->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
                    g->suspend_pending = 0u;   /* refused: no SUSPEND audio state follows */
                }
            }
            bt_xrec_plat_unlock();
        }
        break;
    case ESP_A2D_PROF_STATE_EVT:
        if (p->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS) {
            s_a2d_deinit_seen = 1u;   /* read by the down job (hp_profile_down) */
        }
        e.type = EV_A2D_PROF;
        e.a = (uint8_t)p->a2d_prof_stat.init_state;
        break;
    case ESP_A2D_SEP_REG_STATE_EVT:
        e.type = EV_A2D_SEP;
        e.a = p->a2d_sep_reg_stat.seid;
        e.b = (uint8_t)p->a2d_sep_reg_stat.reg_state;
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        hp_tx_bd_delay_report(p->a2d_report_delay_value_stat.delay_value);
        e.type = EV_A2D_DELAY;
        e.v = p->a2d_report_delay_value_stat.delay_value;
        break;
    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT:
        e.type = EV_A2D_CAPS;
        memcpy(&e.v, &p->a2d_report_snk_codec_caps_stat.mcc.cie, 4u);
        break;
    default:
        return;
    }
    post(&e);
}

BTC_CB_ARG_ALIGN4
static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p)
{
    ev_t e;
    memset(&e, 0, sizeof(e));
    if (p == NULL) {
        return;
    }
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        e.type = EV_CT_CONN;
        e.a = p->conn_stat.connected ? 1u : 0u;
        e.h = p->conn_stat.conn_hdl;
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        e.type = EV_CT_CAPS;
        e.v = p->get_rn_caps_rsp.evt_set.bits;
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        e.type = EV_CT_NTF;
        e.a = p->change_ntf.event_id;
        e.b = p->change_ntf.event_parameter.volume;
        break;
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        e.type = EV_CT_VOL;
        e.a = p->set_volume_rsp.volume;
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        e.type = EV_CT_FEAT;
        e.v = p->rmt_feats.feat_mask;
        e.h = p->rmt_feats.tg_feat_flag;
        break;
    default:
        return;
    }
    post(&e);
}

BTC_CB_ARG_ALIGN4
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *p)
{
    ev_t e;
    memset(&e, 0, sizeof(e));
    if (p == NULL) {
        return;
    }
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        e.type = EV_TG_CONN;
        e.a = p->conn_stat.connected ? 1u : 0u;
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        e.type = EV_TG_KEY;
        e.a = p->psth_cmd.key_code;
        e.b = p->psth_cmd.key_state;
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        e.type = EV_TG_VOL;
        e.a = p->set_abs_vol.volume;
        break;
    default:
        return;
    }
    post(&e);
}

/* ============================================ app side: helpers =============================================== */
static void fault(uint32_t reason);

static void keyq_push(uint8_t k)
{
    bt_xrec_plat_lock();
    if (g->kq_head - g->kq_tail < HP_KEYQ) {
        g->keyq[g->kq_head % HP_KEYQ] = k;
        g->kq_head++;
    }
    bt_xrec_plat_unlock();
}

static void player_event(uint32_t bit)
{
    bt_xrec_plat_lock();
    g->player_ev |= bit;
    bt_xrec_plat_unlock();
}

static const char *gate_block(void)
{
    if (g->ssp_burned) {
        return "a failed pairing used this session's P-192 key (restart headphones mode)";
    }
    return bt_xrec_plat_pairing_block();
}

static void window_open(const uint8_t bda[6])
{
    const char *why = gate_block();
    bt_xrec_plat_lock();
    g->win_open = 1u;
    memcpy(g->win_bda, bda, 6u);
    g->win_deadline = bt_xrec_plat_now_ms() + BT_HP_PAIR_WINDOW_MS;
    g->gate_ok = why == NULL ? 1u : 0u;
    g->jw_accepted = 0u;
    bt_xrec_plat_unlock();
    lbegin("pair window open");
    lka("peer", bda);
    lku("secs", BT_HP_PAIR_WINDOW_MS / 1000u);
    lks("gate", why == NULL ? "ok" : why);
    lend();
    if (why != NULL) {
        set_msg(BT_HP_MSG_PAIRING_UNAVAILABLE);
    }
}

static void window_close(const char *why)
{
    bt_xrec_plat_lock();
    uint8_t was = g->win_open;
    g->win_open = 0u;
    g->jw_accepted = 0u;
    bt_xrec_plat_unlock();
    if (was) {
        lbegin("pair window closed");
        lks("why", why);
        lend();
    }
}

static void ssp_burn(void)
{
    if (!g->ssp_burned) {
        g->ssp_burned = 1u;
        g->d.ssp_burned++;
        log1("SSP key spent: an accepted pairing failed; no pairing until headphones mode restarts");
    }
    window_close("key spent");
}

static void set_scan(bool connectable)
{
    esp_err_t r = esp_bt_gap_set_scan_mode(connectable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                                           ESP_BT_NON_DISCOVERABLE);
    if (r != ESP_OK) {
        lbegin("set_scan_mode failed");
        lku("err", (uint32_t)r);
        lend();
    }
}

static void save_last(const uint8_t bda[6], const char *name)
{
    cfg_load();
    if (s_cfg.last_valid && bda_eq(s_cfg.last, bda) && strncmp(s_cfg.last_name, name, BT_HP_NAME_MAX) == 0) {
        return;
    }
    s_cfg.last_valid = 1u;
    memcpy(s_cfg.last, bda, 6u);
    memset(s_cfg.last_name, 0, sizeof(s_cfg.last_name));
    for (size_t i = 0; i + 1u < sizeof(s_cfg.last_name) && name[i] != '\0'; i++) {
        s_cfg.last_name[i] = name[i];
    }
    cfg_save();
}

static void found_name(const uint8_t bda[6], char *out)
{
    out[0] = '\0';
    bt_xrec_plat_lock();
    for (uint32_t i = 0u; i < g->n_found; i++) {
        if (bda_eq(g->found[i].bda, bda) && g->found[i].named) {
            memcpy(out, g->found[i].name, BT_HP_NAME_MAX);
            break;
        }
    }
    bt_xrec_plat_unlock();
}

/* A name learned after the search (name pass, or asked over the headset's link): the list, the target and the
 * remembered headset (saved only on a change). */
static void name_learned(const uint8_t bda[6], const char *name)
{
    if (name[0] == '\0') {
        return;
    }
    bt_xrec_plat_lock();
    for (uint32_t i = 0u; i < g->n_found; i++) {
        if (bda_eq(g->found[i].bda, bda)) {
            memcpy(g->found[i].name, name, BT_HP_NAME_MAX);
            g->found[i].named = 1u;
        }
    }
    bt_xrec_plat_unlock();
    if (g->target_valid && bda_eq(g->target, bda)) {
        memcpy(g->target_name, name, BT_HP_NAME_MAX);
    }
    cfg_load();
    if (s_cfg.last_valid && bda_eq(s_cfg.last, bda) && strncmp(s_cfg.last_name, name, BT_HP_NAME_MAX) != 0) {
        memset(s_cfg.last_name, 0, sizeof(s_cfg.last_name));
        memcpy(s_cfg.last_name, name, BT_HP_NAME_MAX - 1u);
        cfg_save();
        lbegin("name saved");
        lka("peer", bda);
        lks("name", s_cfg.last_name);
        lend();
    }
}

static bool rn_ask(const uint8_t bda[6], uint32_t now)
{
    if (esp_bt_gap_read_remote_name((uint8_t *)bda) != ESP_OK) {
        return false;
    }
    g->rn_busy = 1u;
    g->rn_t0 = now;
    lbegin("name request");
    lka("peer", bda);
    lend();
    return true;
}

/* One name request at a time: first the connected headset's (over its link), then, with no link and no search,
 * the audio devices of the last search that have none (a page each: never while connecting or streaming). */
static void rn_poll(uint32_t now)
{
    if (g->rn_busy) {
        if (time_reached(now, g->rn_t0 + BT_HP_NAME_REQ_TO_MS)) {
            g->rn_busy = 0u;               /* no READ_REMOTE_NAME_EVT: give up on this one */
        }
        return;
    }
    if (g->scanning || g->pick_pending || g->link == BT_HP_LINK_CONNECTING) {
        return;
    }
    if (g->rn_want) {
        if (g->link == BT_HP_LINK_NONE || !(g->target_valid && bda_eq(g->target, g->rn_want_bda))) {
            g->rn_want = 0u;               /* that link is gone */
        } else if (rn_ask(g->rn_want_bda, now)) {
            g->rn_want = 0u;
        }
        return;
    }
    if (g->link != BT_HP_LINK_NONE) {
        return;
    }
    uint8_t bda[6];
    int idx = -1;
    bt_xrec_plat_lock();
    for (uint32_t i = 0u; i < g->n_found && idx < 0; i++) {
        if (g->found[i].is_audio && !g->found[i].named && !g->rn_tried[i]) {
            idx = (int)i;
            memcpy(bda, g->found[i].bda, 6u);
        }
    }
    bt_xrec_plat_unlock();
    if (idx >= 0) {
        g->rn_tried[idx] = 1u;
        (void)rn_ask(bda, now);
    }
}

/* ============================================ media control (design §4.5) ===================================== */
static void start_failure(const char *why, bool final);

static void mc_issue(uint8_t cmd)
{
    esp_a2d_media_ctrl_t c = cmd == MC_START ? ESP_A2D_MEDIA_CTRL_START : ESP_A2D_MEDIA_CTRL_SUSPEND;
    bt_xrec_plat_lock();
    if (cmd == MC_START) {
        g->start_pending = 1u;
    } else {
        g->suspend_pending = 1u;
    }
    bt_xrec_plat_unlock();
    esp_err_t r = esp_a2d_media_ctrl_by_handle(g->conn_hdl, c);
    lbegin(cmd == MC_START ? "media START" : "media SUSPEND");
    lku("err", (uint32_t)r);
    lend();
    if (r != ESP_OK) {
        bt_xrec_plat_lock();
        g->start_pending = 0u;
        g->suspend_pending = 0u;
        bt_xrec_plat_unlock();
        if (cmd == MC_START) {
            start_failure("send", false);    /* counts as an attempt: the retry waits, no START (and log line) per poll */
        }
        return;
    }
    g->mc_out = cmd;
    g->mc_t = bt_xrec_plat_now_ms();
    if (cmd == MC_START) {
        g->d.starts++;
    } else {
        g->d.suspends++;
    }
}

static void mc_pump(void)
{
    if (g->mc_out != MC_NONE || g->mc_want == MC_NONE) {
        return;
    }
    uint8_t w = g->mc_want;
    if ((w == MC_START && g->link == BT_HP_LINK_OPEN) || (w == MC_SUSPEND && g->link == BT_HP_LINK_STARTED)) {
        g->mc_want = MC_NONE;
        mc_issue(w);
    } else if ((w == MC_START && g->link == BT_HP_LINK_STARTED) || (w == MC_SUSPEND && g->link == BT_HP_LINK_OPEN) ||
               g->link == BT_HP_LINK_NONE) {
        g->mc_want = MC_NONE;   /* already there, or nothing to control */
    }
}

/* aout's demand start (design §3.2, rev 3 R-5): cheap and idempotent. */
static void link_request_start(void)
{
    if (g == NULL || g->state != BT_HP_ST_READY) {
        return;
    }
    uint32_t now = bt_xrec_plat_now_ms();
    if (g->start_failed) {
        if (!time_reached(now, g->start_failed_at + BT_HP_START_COOLDOWN_MS)) {
            g->start_demand = 1u;     /* ready_poll starts again after the cooldown if the player still wants it */
            return;
        }
        g->start_failed = 0u;         /* new demand after the cooldown (the user pressed play): two more attempts */
        g->start_retries = 0u;
        g->start_demand = 0u;
        log1("START: new demand after START failed, trying again");
    }
    if (g->link == BT_HP_LINK_OPEN && g->mc_out != MC_START && g->start_retries == 0u) {
        g->mc_want = MC_START;        /* (start_retries 1: ready_poll sends the one retry at start_retry_at) */
        mc_pump();
    }
    g->last_real_t = now;             /* demand restarts the idle clock */
}

static int link_stream_ready(void)
{
    return g != NULL && g->state == BT_HP_ST_READY &&
           (g->link == BT_HP_LINK_OPEN || g->link == BT_HP_LINK_STARTED);
}

/* aout_bt_link_t layout (include/audio_out.h): { int (*stream_ready)(void); void (*request_start)(void); } */
static const struct {
    int  (*stream_ready)(void);
    void (*request_start)(void);
} k_link = { link_stream_ready, link_request_start };

/* ============================================ connect / reconnect ============================================= */
/* bt_ctrl forwards the initiator rows (BT_CTRL_HP_GATE_LIST) in every BT=1 firmware image (bt/Makefile.bluedroid); a
 * bt_ctrl built without them (host tests, scenario G) refuses them, so search and connect cannot work: say so at once. */
static bool hp_rows_on(void)
{
    static const uint16_t k_rows[] = { 0x0401u, 0x0402u, 0x0405u, 0x0408u };
    for (size_t i = 0u; i < sizeof(k_rows) / sizeof(k_rows[0]); i++) {
        const bt_ctrl_cmd_row_t *r = bt_ctrl_cmds_lookup(k_rows[i]);
        if (r == NULL || bt_ctrl_cmds_eff(r) != BTC_FWD) {
            return false;
        }
    }
    return true;
}

static bool hp_rows_refused(const char *what)
{
    if (hp_rows_on()) {
        return false;
    }
    lbegin(what);
    lks("refused", "this build's bt_ctrl refuses inquiry/page (0401/0402/0405/0408): BT_CTRL_HP_GATE_LIST not compiled in");
    lend();
    set_msg(BT_HP_MSG_UNSUPPORTED);
    return true;
}

/* Cancel a connect that has no handle yet (a first connect gets its handle with CONNECTED): the address form, which
 * the BTC task resolves (btc_av_disconnect_evt). Deprecated upstream in favour of the handle form, which cannot do
 * this. */
static esp_err_t a2d_cancel_by_addr(const uint8_t bda[6])
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    esp_err_t r = esp_a2d_source_disconnect((uint8_t *)bda);
#pragma GCC diagnostic pop
    return r;
}

static bool orphan_active(uint32_t now)
{
    if (g->orphan && time_reached(now, g->orphan_until)) {
        g->orphan = 0u;
        log1("timed-out connect: no late result, connecting again is allowed");
    }
    return g->orphan != 0u;
}

static void connect_now(const uint8_t bda[6])
{
    if (g->scanning) {
        return;          /* the pick waits for DISC_STATE stopped (design §6.2) */
    }
    if (g->link != BT_HP_LINK_NONE) {
        lbegin("a2dp connect skipped");   /* one link at a time: never a second ACL next to an open one */
        lka("peer", bda);
        lku("link", g->link);
        lend();
        return;
    }
    a2d_peer_set(bda);                           /* before the page: its first events may beat the next line */
    esp_err_t r = esp_a2d_source_connect((uint8_t *)bda);
    lbegin("a2dp connect");
    lka("peer", bda);
    lku("err", (uint32_t)r);
    lku("inq", (uint32_t)bt_ctrl_inquiry_active());
    lkx("scan", bt_ctrl_scan_enable(), 2);
    lend();
    if (r != ESP_OK) {
        g->d.connect_fail++;
        set_msg(BT_HP_MSG_CONNECT_FAIL);
        if (g->target_valid) {
            a2d_peer_set(g->target);             /* nothing paged: the mirror follows the target again */
        }
        return;
    }
    g->d.connects++;
    g->conn_hdl = ESP_A2D_INVALID_CONN_HDL;      /* never the previous link's handle (STEP10 C12) */
    g->link = BT_HP_LINK_CONNECTING;
    g->connect_t0 = bt_xrec_plat_now_ms() + BT_HP_CONNECT_TO_MS;   /* deadline */
    target_set(bda);
}

/* Is there a stored link key for bda (bt_hp_bonds.bin, loaded into Bluedroid's config)? */
static bool bonded(const uint8_t bda[6])
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return false;
    }
    esp_bd_addr_t list[HP_BOND_LIST_MAX];
    if (n > (int)HP_BOND_LIST_MAX) {
        n = (int)HP_BOND_LIST_MAX;
    }
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
        return true;                  /* unknown: keep the old behaviour (try) */
    }
    for (int i = 0; i < n; i++) {
        if (bda_eq(list[i], bda)) {
            return true;
        }
    }
    return false;
}

/* The remembered headset needs pairing again (its key is gone here, or it refused ours): reconnects stop until the
 * user picks a headset; the screen says "Pair again" (user report 2026-11-25: endless reconnects, auth st=9). */
static void pair_again(const char *why)
{
    g->pair_again = 1u;
    g->rc_active = 0u;
    set_msg(BT_HP_MSG_PAIR_AGAIN);
    lbegin("reconnect stopped: pair again");
    lks("why", why);
    lend();
}

static void rc_schedule(bool first)
{
    cfg_load();
    if (!s_cfg.last_valid || g->user_disc || g->pair_again || !hp_rows_on()) {
        g->rc_active = 0u;
        return;
    }
    if (first) {
        g->rc_step = 0u;
    } else if (g->rc_step + 1u < HP_RC_STEPS) {
        g->rc_step++;
    }
    g->rc_active = 1u;
    g->rc_at = bt_xrec_plat_now_ms() + (uint32_t)k_rc_backoff_s[g->rc_step] * 1000u;
    lbegin("reconnect scheduled");
    lka("peer", s_cfg.last);
    lku("in_s", k_rc_backoff_s[g->rc_step]);
    lku("step", g->rc_step);
    lend();
}

static void rc_poll(uint32_t now)
{
    if (!g->rc_active || g->link != BT_HP_LINK_NONE || g->scanning || g->pick_pending || orphan_active(now)) {
        return;
    }
    if (!time_reached(now, g->rc_at)) {
        return;
    }
    if (!bonded(s_cfg.last)) {
        pair_again("no stored key");   /* a reconnect cannot pair (no window): it would only be refused, forever */
        return;
    }
    g->d.reconnect_tries++;
    set_msg(BT_HP_MSG_RECONNECTING);
    memcpy(g->target_name, s_cfg.last_name, sizeof(g->target_name));
    connect_now(s_cfg.last);
    if (g->link != BT_HP_LINK_CONNECTING) {
        rc_schedule(false);
    }
}

/* ============================================ events ========================================================== */
static void on_link_down(const char *why, uint32_t pev)
{
    uint8_t was = g->link;
    g->link = BT_HP_LINK_NONE;
    g->mc_out = g->mc_want = MC_NONE;
    g->start_failed = 0u;
    g->start_retries = 0u;
    g->start_demand = 0u;
    g->ct_conn = 0u;
    g->abs_vol = 0u;
    g->ct_hdl = ESP_A2D_INVALID_CONN_HDL;
    g->conn_hdl = ESP_A2D_INVALID_CONN_HDL;   /* a later connect's timeout / cancel goes by address (STEP10 C12) */
    g->stall_disc = 0u;
    if (was == BT_HP_LINK_OPEN || was == BT_HP_LINK_STARTED || was == BT_HP_LINK_STALLED) {
        g->d.disconnects++;
        set_msg(BT_HP_MSG_DISCONNECTED);
        if (pev) {
            player_event(pev);
        }
    }
    lbegin("link down");
    lks("why", why);
    lku("was", was);
    lend();
    rc_schedule(was == BT_HP_LINK_OPEN || was == BT_HP_LINK_STARTED);
}

static void on_a2d_conn(const ev_t *e)
{
    lbegin("a2dp conn");
    lku("state", e->a);
    lka("peer", e->bda);
    lku("hdl", e->h);
    lku("mtu", e->v);
    lku("rsn", e->b);
    if (e->a == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        lku("cfg_ok", e->c);
    }
    lend();
    bool orphan = g->orphan && bda_eq(g->orphan_bda, e->bda);
    if (orphan && (e->a == ESP_A2D_CONNECTION_STATE_CONNECTED || e->a == ESP_A2D_CONNECTION_STATE_DISCONNECTED)) {
        g->orphan = 0u;               /* the timed-out connect reported after all */
    }
    if (e->a == ESP_A2D_CONNECTION_STATE_CONNECTED &&
        (g->link == BT_HP_LINK_OPEN || g->link == BT_HP_LINK_STARTED || g->link == BT_HP_LINK_STALLED ||
         !g->target_valid || !bda_eq(g->target, e->bda) || (g->link == BT_HP_LINK_NONE && g->user_disc))) {
        /* a late result of a connect we gave up on (timeout), for a device that is no longer the target: close it */
        lbegin("late a2dp connect closed");
        lka("peer", e->bda);
        lku("link", g->link);
        lend();
        (void)esp_a2d_source_disconnect_by_handle(e->h);
        return;
    }
    if (e->a == ESP_A2D_CONNECTION_STATE_CONNECTED && g->forgot_valid && bda_eq(g->forgot, e->bda)) {
        /* a connect that was under way when the user chose Forget: close it, do not remember the device again */
        lbegin("forgotten headset connected: closed");
        lka("peer", e->bda);
        lend();
        (void)esp_a2d_source_disconnect_by_handle(e->h);
        g->link = BT_HP_LINK_NONE;
        return;
    }
    if (e->a == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        g->link = BT_HP_LINK_OPEN;
        g->conn_hdl = e->h;
        target_set(e->bda);
        g->rc_active = 0u;
        g->rc_step = 0u;
        g->user_disc = 0u;
        g->start_failed = 0u;
        g->start_retries = 0u;
        g->last_real_t = bt_xrec_plat_now_ms();
        char nm[BT_HP_NAME_MAX];
        found_name(e->bda, nm);
        if (nm[0] == '\0' && s_cfg.last_valid && bda_eq(s_cfg.last, e->bda)) {
            memcpy(nm, s_cfg.last_name, sizeof(nm));
        }
        if (nm[0] != '\0') {
            memcpy(g->target_name, nm, sizeof(nm));
        } else {
            g->rn_want = 1u;          /* no name from the search: ask the headset over this link (rn_poll) */
            memcpy(g->rn_want_bda, e->bda, 6u);
        }
        g->pair_again = 0u;
        set_msg(BT_HP_MSG_CONNECTED);
        window_close("connected");
        /* linkinfo (design §10.2): role, packet types, slots from bt_ctrl's link view */
        bt_xrec_plat_lock();
        uint16_t h = g->acl_valid ? g->acl_handle : 0xFFFFu;
        bt_xrec_plat_unlock();
        bt_ctrl_link_info_t li;
        memset(&li, 0, sizeof(li));
        if (h != 0xFFFFu && bt_ctrl_link_info(h, &li)) {
            lbegin("linkinfo");
            lks("role", li.role == BT_CTRL_ROLE_MASTER ? "M" : li.role == BT_CTRL_ROLE_SLAVE ? "S" : "?");
            lkx("pkt_type", li.pkt_type, 4);          /* 0x0000 while 040F stays REJ (no 0x1D event) */
            lkx("req_pt", li.req_pkt_type, 4);        /* what our 0405 asked for */
            lku("allow_rs", li.req_allow_rs);
            lku("max_slots", li.max_slots);
            lku("role_chg", li.role_changes);
            lku("out", li.outgoing);
            if (li.rmt_feat_seen) {                   /* the peer's LMP page 0: EDR 2M/3M, 3-/5-slot */
                lku("edr", (li.rmt_feat3 >> 1) & 3u);   /* bit 0: 2 Mb/s, bit 1: 3 Mb/s */
                lku("slots35", li.rmt_feat0 & 3u);      /* bit 0: 3-slot, bit 1: 5-slot */
            } else {
                lks("edr", "?");
            }
            lkx("acl", h, 4);
            lend();
        }
        /* the device the user connected to is the one to reconnect to (bonded or not: a headset that pairs again
         * each time still reconnects) */
        save_last(e->bda, g->target_name);
    } else if (e->a == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        if (g->target_valid && !bda_eq(g->target, e->bda)) {
            return;                   /* the timed-out connect to an earlier pick closed: not this link */
        }
        if (g->link == BT_HP_LINK_CONNECTING) {
            g->d.connect_fail++;
            set_msg(BT_HP_MSG_CONNECT_FAIL);
            g->link = BT_HP_LINK_NONE;
            log1("connect failed");
            rc_schedule(false);
        } else if (g->link != BT_HP_LINK_NONE) {
            on_link_down(e->b == ESP_A2D_DISC_RSN_ABNORMAL ? "abnormal" : "normal", BT_HP_PEV_LINK_LOST);
        }
    }
}

static void on_a2d_audio(const ev_t *e)
{
    if (e->a == ESP_A2D_AUDIO_STATE_STARTED) {
        lbegin("audio STARTED");
        lku("ours", e->b);
        lku("tick", e->c);
        lend();
        if (e->b && !e->c) {
            /* our START, but the transmit path did not arm (no valid codec config, fpp 0, alarm): nothing would be
             * sent. Suspend again and pause the player rather than show "playing" with no audio. */
            g->link = BT_HP_LINK_STARTED;
            g->mc_want = MC_SUSPEND;
            start_failure("tick", true);
            return;
        }
        if (e->b) {
            g->link = BT_HP_LINK_STARTED;
            g->start_retries = 0u;
            g->susp_retries = 0u;
            g->susp_retry_due = 0u;
            g->last_real_t = bt_xrec_plat_now_ms();
            hp_tx_t *t = hp_tx_bd();
            lbegin("stream");
            lku("rate", t->conn_rate);
            lku("bp", t->cfg.max_bp);
            lku("bp_min", t->cfg.min_bp);
            lku("speech_bp", t->tun.speech_bp);   /* sources <= 16 kHz go out at this bitpool (0 = always bp) */
            lku("fpp", t->fpp);
            lku("mtu", t->audio_mtu);
            lku("tick_ms", t->tick_ms_eff);
            lend();
        } else {
            g->d.remote_starts++;      /* the stack suspends a sink-initiated START itself (design §4.5) */
        }
    } else {
        lbegin("audio SUSPEND");
        lku("after_remote_start", e->b);
        lku("ours", e->c);
        lend();
        bool was_started = g->link == BT_HP_LINK_STARTED;
        if (g->link == BT_HP_LINK_STARTED) {
            g->link = BT_HP_LINK_OPEN;
        }
        g->susp_retries = 0u;
        g->susp_retry_due = 0u;
        if (was_started && !e->b && !e->c) {
            g->d.remote_suspends++;
            player_event(BT_HP_PEV_REMOTE_SUSPEND);
        }
    }
}

static void on_a2d_ack(const ev_t *e)
{
    lbegin("media ack");
    lku("cmd", e->a);
    lku("st", e->b);
    lend();
    uint8_t out = g->mc_out;
    if ((e->a == ESP_A2D_MEDIA_CTRL_START && out == MC_START) ||
        (e->a == ESP_A2D_MEDIA_CTRL_SUSPEND && out == MC_SUSPEND)) {
        g->mc_out = MC_NONE;
    }
    if (e->b == ESP_A2D_MEDIA_CTRL_ACK_BUSY) {
        g->d.media_busy++;
    }
    if (e->a == ESP_A2D_MEDIA_CTRL_START && e->b != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS && out == MC_START) {
        start_failure("ack", false);
    }
    if (e->a == ESP_A2D_MEDIA_CTRL_SUSPEND && e->b != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS && out == MC_SUSPEND &&
        g->link == BT_HP_LINK_STARTED) {
        /* refused: the stream stays STARTED and sends silence (e.g. a whole recording after the REC switch). Send it
         * again a few times (ready_poll); otherwise only the next idle suspend would. */
        g->susp_refused_at = bt_xrec_plat_now_ms();
        g->susp_retry_due = g->susp_retries < BT_HP_SUSPEND_RETRIES ? 1u : 0u;
        lbegin("SUSPEND refused");
        lku("retry", g->susp_retry_due);
        lku("n", g->susp_retries);
        lend();
    }
    mc_pump();
}

/* A START attempt failed (refused, not sent, or started without a transmit tick). The first failure schedules the one
 * retry at BT_HP_START_RETRY_MS (ready_poll; demand does not bypass it); the second is final: the player pauses
 * (design §4.5). start_failed is not sticky: new demand after BT_HP_START_COOLDOWN_MS (the user presses play) tries
 * again (link_request_start), so a headset that was busy with another source is usable without a power cycle. */
static void start_failure(const char *why, bool final)
{
    g->d.start_fail++;
    uint32_t now = bt_xrec_plat_now_ms();
    if (g->start_retries == 0u && !final) {
        g->start_retries = 1u;
        g->start_retry_at = now + BT_HP_START_RETRY_MS;
        lbegin("START failed, retry");
        lks("why", why);
        lku("in_ms", BT_HP_START_RETRY_MS);
        lend();
        return;
    }
    if (g->start_failed) {
        return;
    }
    g->start_failed = 1u;
    g->start_failed_at = now;
    g->start_retries = 0u;
    g->start_demand = 0u;
    set_msg(BT_HP_MSG_START_FAILED);
    player_event(BT_HP_PEV_START_FAILED);
    lbegin("START failed twice: headphones not ready");
    lks("why", why);
    lend();
}

static void on_auth(const ev_t *e)
{
    bool picked = g->target_valid && bda_eq(g->target, e->bda);
    bt_xrec_plat_lock();
    bool accepted = g->jw_accepted && bda_eq(g->win_bda, e->bda);
    bt_xrec_plat_unlock();
    lbegin("auth");
    lka("peer", e->bda);
    lku("st", e->a);
    lku("key", e->b);
    lku("picked", picked);
    lend();
    if (e->a == ESP_BT_STATUS_SUCCESS) {
        g->d.auth_ok++;
        if (accepted) {
            set_msg(BT_HP_MSG_PAIRED);
            window_close("paired");
        }
        /* a key from a device that was not picked (only reachable through a stored key or a path that bypassed the
         * Just Works hook) is removed: second line of design §6.3 */
        if (!picked && !(s_cfg.last_valid && bda_eq(s_cfg.last, e->bda))) {
            g->d.bonds_removed++;
            log1("bond removed: not the picked headset");
            (void)esp_bt_gap_remove_bond_device((uint8_t *)e->bda);
        }
    } else {
        g->d.auth_fail++;
        set_msg(BT_HP_MSG_PAIR_FAIL);
        if (accepted) {
            ssp_burn();
        }
        cfg_load();
        bool win = false;
        bt_xrec_plat_lock();
        win = g->win_open && bda_eq(g->win_bda, e->bda);
        bt_xrec_plat_unlock();
        if (!win && s_cfg.last_valid && bda_eq(s_cfg.last, e->bda) && !bonded(e->bda)) {
            pair_again("authentication failed, no key");   /* a reconnect (no window) without a key cannot pair */
        }
    }
}

/* Passthrough presses -> player keys (design §6.6). We register no PLAYBACK_STATUS notification and a player pause
 * keeps the stream STARTED (silence) until the idle suspend, so a one-button headset guesses PLAY or PAUSE from its
 * own view: both are toggles (BT_HP_KEY_TOGGLE). STOP always pauses. VOL+/- passthrough (headsets without absolute
 * volume) is one player key; the player changes bt_vol through bt_hp_set_vol31(), once. */
static void on_tg_key(const ev_t *e)
{
    uint8_t k = BT_HP_KEY_NONE;
    if (e->b == 0u) {                 /* press edges only */
        switch (e->a) {
        case ESP_AVRC_PT_CMD_PLAY:
        case ESP_AVRC_PT_CMD_PAUSE:    k = BT_HP_KEY_TOGGLE; break;
        case ESP_AVRC_PT_CMD_STOP:     k = BT_HP_KEY_PAUSE; break;
        case ESP_AVRC_PT_CMD_FORWARD:  k = BT_HP_KEY_NEXT; break;
        case ESP_AVRC_PT_CMD_BACKWARD: k = BT_HP_KEY_PREV; break;
        case ESP_AVRC_PT_CMD_VOL_UP:   k = BT_HP_KEY_VOL_UP; break;
        case ESP_AVRC_PT_CMD_VOL_DOWN: k = BT_HP_KEY_VOL_DOWN; break;
        default: break;
        }
    }
    lbegin("avrcp key");
    lkx("code", e->a, 2);
    lku("rel", e->b);
    lku("key", k);
    lend();
    if (k != BT_HP_KEY_NONE) {
        g->d.avrc_keys++;
        keyq_push(k);
    }
}

/* AVRCP CT commands go to the AVRCP connection's handle (valid before A2DP CONNECTED on headsets that open AVCTP
 * first; on a first connect g->conn_hdl is still invalid then). */
static uint16_t ct_handle(void)
{
    return g->ct_hdl != ESP_A2D_INVALID_CONN_HDL ? g->ct_hdl : g->conn_hdl;
}

static void abs_vol_send(void)
{
    if (!g->ct_conn || !g->abs_vol) {
        return;
    }
    g->ct_tl = (uint8_t)((g->ct_tl + 1u) & 0x0Fu);
    esp_err_t r = esp_avrc_ct_send_set_absolute_volume_cmd_by_handle(ct_handle(), g->ct_tl, s_cfg.bt_vol);
    if (r != ESP_OK) {
        lbegin("set_absolute_volume failed");
        lku("err", (uint32_t)r);
        lend();
    }
}

static void vol_register(void)
{
    g->ct_tl = (uint8_t)((g->ct_tl + 1u) & 0x0Fu);
    esp_err_t r = esp_avrc_ct_send_register_notification_cmd_by_handle(ct_handle(), g->ct_tl,
                                                                       ESP_AVRC_RN_VOLUME_CHANGE, 0u);
    if (r != ESP_OK) {
        lbegin("register volume notification failed");
        lku("err", (uint32_t)r);
        lend();
    }
}

static void on_event(const ev_t *e)
{
    switch (e->type) {
    case EV_DISC_STATE:
        lbegin(e->a ? "discovery started" : "discovery stopped");
        lku("found", g->n_found);
        lku("inq", (uint32_t)bt_ctrl_inquiry_active());
        lend();
        if (!e->a) {
            if (g->scanning) {
                set_msg(BT_HP_MSG_SCAN_DONE);
            }
            g->scanning = 0u;
            g->scan_cancel = 0u;
        }
        break;
    case EV_DISC_RES: {
        g->d.disc_results++;
        bt_hp_found_t f;
        bool have = false;
        bt_xrec_plat_lock();
        for (uint32_t i = 0u; i < g->n_found; i++) {
            if (bda_eq(g->found[i].bda, e->bda)) {
                f = g->found[i];
                have = true;
            }
        }
        bt_xrec_plat_unlock();
        if (have) {
            lbegin("found");
            lka("peer", f.bda);
            lkx("cod", f.cod, 6);
            lku("audio", f.is_audio);
            if (f.has_rssi) {
                lc(' ');
                ls("rssi=-");
                lu((uint32_t)(f.rssi < 0 ? -(int32_t)f.rssi : (int32_t)f.rssi));
            }
            lks("name", f.named ? f.name : "?");
            lend();
        }
        break;
    }
    case EV_NAME: {
        char nm[BT_HP_NAME_MAX];
        bt_xrec_plat_lock();
        bool same = bda_eq(g->rn_bda, e->bda);
        memcpy(nm, g->rn_name, sizeof(nm));
        bt_xrec_plat_unlock();
        g->rn_busy = 0u;
        lbegin("name");
        lka("peer", e->bda);
        lku("st", e->a);
        lks("name", same && e->a == ESP_BT_STATUS_SUCCESS ? nm : "");
        lend();
        if (same && e->a == ESP_BT_STATUS_SUCCESS) {
            name_learned(e->bda, nm);
        }
        break;
    }
    case EV_JW:
        if (e->a) {
            g->d.jw_allowed++;
            log1("Just Works pairing accepted (picked headset, window open)");
        } else {
            g->d.jw_refused++;
            set_msg(BT_HP_MSG_PAIR_REFUSED);
            lbegin("Just Works pairing refused");
            lka("peer", e->bda);
            lend();
        }
        break;
    case EV_CFM_REQ: {
        /* a headset with a display (numeric comparison): accepted for the picked device inside the window only */
        bt_xrec_plat_lock();
        bool ok = g->win_open && bda_eq(g->win_bda, e->bda) && g->gate_ok && !bt_xrec_plat_rng_starved() &&
                  !time_reached(bt_xrec_plat_now_ms(), g->win_deadline);   /* as the Just Works hook (STEP10 A8, C3) */
        if (ok) {
            g->jw_accepted = 1u;
        }
        bt_xrec_plat_unlock();
        if (!ok) {
            g->d.cfm_rejected++;
        }
        (void)esp_bt_gap_ssp_confirm_reply((uint8_t *)e->bda, ok);
        lbegin(ok ? "numeric comparison accepted (picked headset)" : "numeric comparison rejected");
        lku("value", e->v % 1000000u);
        lend();
        break;
    }
    case EV_PIN_REQ: {
        esp_bt_pin_code_t pin;
        memset(pin, 0, sizeof(pin));
#if BENCH_IMAGE
        if (s_tun_set && s_tun.pin0000) {   /* only during the bench run that asked for it (STEP10 C17) */
            /* bench only (hp_pin=1): the ROM pairs as initiator with legacy PIN (silicon 2026-11-25); answer "0000"
             * for the picked device inside its window, with the same gate as the Just Works hook */
            bt_xrec_plat_lock();
            bool ok = g->win_open && bda_eq(g->win_bda, e->bda) && g->gate_ok && !bt_xrec_plat_rng_starved();
            if (ok) {
                g->jw_accepted = 1u;
            }
            bt_xrec_plat_unlock();
            if (ok) {
                memset(pin, '0', 4u);
                (void)esp_bt_gap_pin_reply((uint8_t *)e->bda, true, 4u, pin);
                log1("legacy PIN pairing answered 0000 (bench hp_pin=1, picked headset)");
                break;
            }
        }
#endif
        g->d.pin_rejected++;
        (void)esp_bt_gap_pin_reply((uint8_t *)e->bda, false, 0u, pin);
        log1("legacy PIN pairing rejected");   /* design §6.3; Exp 4 records any headset that needs a PIN */
        break;
    }
    case EV_KEY_REQ:
        (void)esp_bt_gap_ssp_passkey_reply((uint8_t *)e->bda, false, 0u);
        log1("passkey entry rejected");
        break;
    case EV_KEY_NOTIF:
        log1("passkey display ignored");
        break;
    case EV_AUTH:
        on_auth(e);
        break;
    case EV_ACL_UP:
        lbegin("acl up");
        lka("peer", e->bda);
        lku("st", e->a);
        lkx("h", e->h, 4);
        lend();
        break;
    case EV_ACL_DOWN: {
        lbegin("acl down");
        lka("peer", e->bda);
        lkx("rsn", e->a, 2);
        lkx("h", e->h, 4);
        lend();
        bt_xrec_plat_lock();
        bool acc = g->jw_accepted && bda_eq(g->win_bda, e->bda);
        bt_xrec_plat_unlock();
        if (acc && g->link == BT_HP_LINK_CONNECTING) {
            ssp_burn();                /* accepted, then the link went before AUTH_CMPL */
        }
        if (g->link == BT_HP_LINK_STALLED && g->target_valid && bda_eq(g->target, e->bda)) {
            on_link_down("stall closed", 0u);
        }
        break;
    }
    case EV_A2D_CONN:
        on_a2d_conn(e);
        break;
    case EV_A2D_AUDIO:
        on_a2d_audio(e);
        break;
    case EV_A2D_CFG: {
        g->d.cfg_events++;
        hp_sbc_cfg_t c;
        uint8_t cie[4] = { (uint8_t)e->v, (uint8_t)(e->v >> 8), (uint8_t)(e->v >> 16), (uint8_t)(e->v >> 24) };
        lbegin("codec cfg");
        lkx("cie", e->v, 8);
        lku("ok", e->a);
        if (hp_sbc_cfg_from_cie(cie, &c) == 0) {
            lku("rate", c.rate);
            lku("mode", c.ch_mode);
            lku("blocks", c.blocks);
            lku("sb", c.subbands);
            lku("bp", c.min_bp);
            lc('-');
            lu(c.max_bp);
        }
        lend();
        break;
    }
    case EV_A2D_CAPS:
        lbegin("sink caps");
        lkx("cie", e->v, 8);
        lend();
        break;
    case EV_A2D_ACK:
        on_a2d_ack(e);
        break;
    case EV_A2D_PROF:
        lbegin("a2dp prof");
        lku("state", e->a);
        lend();
        if (e->a == ESP_A2D_INIT_SUCCESS) {
            g->prof_init = 1u;
        }
        break;
    case EV_A2D_SEP:
        lbegin("sep registered");
        lku("seid", e->a);
        lku("st", e->b);
        lend();
        if (g->seps_pending) {
            g->seps_pending--;
        }
        if (e->b != ESP_A2D_SEP_REG_SUCCESS) {
            fault(BT_XREC_FAULT_CONFIG);   /* design §4.1: a SEP that did not register is an init failure */
        }
        break;
    case EV_A2D_DELAY:
        g->d.delay_reports++;
        lbegin("delay report");
        lku("0p1ms", e->v);
        lend();
        break;
    case EV_CT_CONN:
        g->ct_conn = e->a;
        g->abs_vol = 0u;
        g->ct_hdl = e->a ? e->h : ESP_A2D_INVALID_CONN_HDL;
        lbegin("avrcp ct");
        lku("conn", e->a);
        lku("hdl", e->h);
        if (e->a) {
            g->ct_tl = (uint8_t)((g->ct_tl + 1u) & 0x0Fu);
            lku("caps_err", (uint32_t)esp_avrc_ct_send_get_rn_capabilities_cmd_by_handle(ct_handle(), g->ct_tl));
        }
        lend();
        break;
    case EV_CT_FEAT:
        lbegin("avrcp remote features");
        lkx("mask", e->v, 8);
        lkx("tg", e->h, 4);
        lend();
        break;
    case EV_CT_CAPS: {
        bool vol = (e->v & (1u << ESP_AVRC_RN_VOLUME_CHANGE)) != 0u;
        lbegin("avrcp rn caps");
        lkx("bits", e->v, 4);
        lku("abs_vol", vol);
        lend();
        if (vol) {
            g->abs_vol = 1u;
            vol_register();
            abs_vol_send();
        }
        break;
    }
    case EV_CT_NTF:
        if (e->a == ESP_AVRC_RN_VOLUME_CHANGE) {
            g->d.avrc_vol_ntf++;
            s_cfg.bt_vol = (uint8_t)(e->b & 0x7Fu);
            lbegin("headset volume");
            lku("v", s_cfg.bt_vol);
            lend();
            vol_register();            /* notifications are one-shot (design §6.6) */
        }
        break;
    case EV_CT_VOL:
        s_cfg.bt_vol = (uint8_t)(e->a & 0x7Fu);
        break;
    case EV_TG_CONN:
        lbegin("avrcp tg");
        lku("conn", e->a);
        lend();
        break;
    case EV_TG_KEY:
        on_tg_key(e);
        break;
    case EV_TG_VOL:
        s_cfg.bt_vol = (uint8_t)(e->a & 0x7Fu);
        break;
    case EV_TX_STALL:
        g->d.stalls++;
        set_msg(BT_HP_MSG_STALLED);
        player_event(BT_HP_PEV_STALLED);
        log1("credit stall: no NOCP for the stall time with packets downstream; closing the ACL (0406)");
        {
            bt_xrec_plat_lock();
            uint16_t h = g->acl_valid ? g->acl_handle : 0xFFFFu;
            bt_xrec_plat_unlock();
            if (h != 0xFFFFu) {
                hp_tx_bd_close_acl(h);
            }
        }
        g->link = BT_HP_LINK_STALLED;
        g->stall_disc = 1u;
        g->stall_t = bt_xrec_plat_now_ms();
        break;
    default:
        break;
    }
}

static void drain_events(void)
{
    for (uint32_t budget = 0u; budget < HP_EVQ_SLOTS; budget++) {
        ev_t e;
        bt_xrec_plat_lock();
        if (g->ev_head == g->ev_tail) {
            bt_xrec_plat_unlock();
            return;
        }
        e = g->evq[g->ev_tail % HP_EVQ_SLOTS];
        g->ev_tail++;
        bt_xrec_plat_unlock();
        on_event(&e);
        if (g == NULL || (g->state != BT_HP_ST_CONFIG && g->state != BT_HP_ST_READY)) {
            return;
        }
    }
}

static void ctrl_notes_drain(void)
{
    char line[BT_XREC_NOTE_LINE];
    for (int i = 0; i < 8; i++) {
        bt_xrec_note_t n;
        memset(&n, 0, sizeof(n));
        if (bt_xrec_plat_ctrl_note(line, sizeof(line), &n) == 0) {
            return;
        }
        bt_xrec_plat_log(line);   /* every HCI step of inquiry / page / connect with its status (bt_ctrl notes) */
    }
}

/* ============================================ configuration =================================================== */
static void sep_mcc(esp_a2d_mcc_t *m, uint8_t freq, uint8_t max_bp)
{
    memset(m, 0, sizeof(*m));
    m->type = ESP_A2D_MCT_SBC;
    m->cie.sbc_info.samp_freq = freq & 0xFu;
    m->cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO | ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
                              ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    m->cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_4 | ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
                                ESP_A2D_SBC_CIE_BLOCK_LEN_12 | ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    m->cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 | ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    m->cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR | ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    m->cie.sbc_info.min_bitpool = 2u;
    m->cie.sbc_info.max_bitpool = max_bp;
}

static void config_begin(void)
{
    esp_err_t r;
    uint8_t iocap = ESP_BT_IO_CAP_IO;          /* DisplayYesNo as in phone mode; headsets (NoInputNoOutput) -> Just Works */
    esp_bt_cod_t cod;
    memset(&cod, 0, sizeof(cod));
    cod.minor = 0x07u;                          /* 0x28041C (design §6.4): major AV, portable audio, Audio+Capturing */
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_CAPTURING;
    set_state(BT_HP_ST_CONFIG);
    esp_avrc_psth_bit_mask_t psth;
    memset(&psth, 0, sizeof(psth));
    static const uint8_t keys[] = { ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STOP,
                                    ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_FAST_FORWARD,
                                    ESP_AVRC_PT_CMD_REWIND, ESP_AVRC_PT_CMD_VOL_UP, ESP_AVRC_PT_CMD_VOL_DOWN };
    for (size_t i = 0; i < sizeof(keys); i++) {
        (void)esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth, (esp_avrc_pt_cmd_t)keys[i]);
    }
    /* design §5.4 order: GAP, AVRC CT + TG (before A2DP), A2DP callback + source init; SEPs once PROF_STATE init */
    if ((r = esp_bt_gap_register_callback(gap_cb)) != ESP_OK ||
        (r = esp_bt_gap_set_device_name(g->dev_name)) != ESP_OK ||
        (r = esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD)) != ESP_OK ||
        (r = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap))) != ESP_OK ||
        (r = esp_avrc_ct_register_callback(avrc_ct_cb)) != ESP_OK || (r = esp_avrc_ct_init()) != ESP_OK ||
        (r = esp_avrc_tg_register_callback(avrc_tg_cb)) != ESP_OK || (r = esp_avrc_tg_init()) != ESP_OK) {
        lbegin("GAP/AVRCP configuration failed");
        lku("err", (uint32_t)r);
        lend();
        fault(BT_XREC_FAULT_CONFIG);
        return;
    }
    g->avrc_inited = 1u;
    s_avrc_down = 1u;
    if ((r = esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &psth)) != ESP_OK) {
        lbegin("AVRCP passthrough filter failed");
        lku("err", (uint32_t)r);
        lend();
    }
    if ((r = esp_a2d_register_callback(a2d_cb)) != ESP_OK || (r = esp_a2d_source_init()) != ESP_OK) {
        lbegin("A2DP source init failed");
        lku("err", (uint32_t)r);
        lend();
        fault(BT_XREC_FAULT_CONFIG);
        return;
    }
    g->a2d_inited = 1u;
    s_a2d_down = 1u;
    set_scan(false);                           /* we always initiate (v1, live log): no page or inquiry scan */
}

static void config_seps(void)
{
    uint8_t bp = HP_MAX_BITPOOL, f0 = ESP_A2D_SBC_CIE_SF_44K | ESP_A2D_SBC_CIE_SF_48K, f1 = ESP_A2D_SBC_CIE_SF_44K;
#if BENCH_IMAGE
    if (s_tun_set) {
        if (s_tun.bitpool) {
            bp = s_tun.bitpool;
        }
        if (s_tun.rate == 44100u) {
            f0 = f1 = ESP_A2D_SBC_CIE_SF_44K;   /* a fixed rate overwrites both SEIDs (design §10.2, R-15d) */
        } else if (s_tun.rate == 48000u) {
            f0 = f1 = ESP_A2D_SBC_CIE_SF_48K;
        }
    }
#endif
    esp_a2d_mcc_t m;
    esp_err_t r0, r1;
    sep_mcc(&m, f0, bp);
    r0 = esp_a2d_source_register_stream_endpoint(0u, &m);   /* SEID 0: 44.1|48 (the initiator's SEP, §4.1) */
    sep_mcc(&m, f1, bp);
    r1 = esp_a2d_source_register_stream_endpoint(1u, &m);   /* SEID 1: the 44.1 fallback */
    lbegin("seps");
    lkx("sep0", f0, 2);
    lkx("sep1", f1, 2);
    lku("max_bp", bp);
    lku("err0", (uint32_t)r0);
    lku("err1", (uint32_t)r1);
    lend();
    if (r0 != ESP_OK || r1 != ESP_OK) {
        fault(BT_XREC_FAULT_CONFIG);
        return;
    }
    g->seps_pending = 2u;
    g->seps_ok = 1u;
}

static void config_finish(void)
{
    hp_tx_tun_t tun;
    memset(&tun, 0, sizeof(tun));
    tun.speech_bp = 0xFFu;
    tun.flush_drops_txq = 0xFFu;
#if BENCH_IMAGE
    if (s_tun_set) {
        tun.tick_ms = s_tun.tick_ms;
        tun.max_pkt_per_tick = s_tun.pkt_per_tick;
        tun.txq_cap = s_tun.txq;
        tun.credit_ticks = s_tun.credit_ticks;
        tun.speech_bp = s_tun.speech_bp;
    }
#endif
    /* PCM ring (design §3.3): heap first, so output mode BOTH can run the DAC too; the DAC's slot buffer otherwise */
    g->ring = (int16_t *)bt_xrec_plat_alloc(HP_RING_BYTES);
    uint32_t bytes = HP_RING_BYTES;
    g->ring_is_dac = 0u;
    if (g->ring == NULL) {
        g->ring = bt_hp_plat_borrow_dac_buf(&bytes);
        g->ring_is_dac = 1u;
        log1("PCM ring: no heap, borrowing the DAC buffer (output mode Both plays the headset only)");
    }
    if (g->ring == NULL) {
        log1("PCM ring unavailable");
        fault(BT_XREC_FAULT_CONFIG);
        return;
    }
    hp_sink_init(&g->sink, g->ring, bytes);
    if (hp_tx_bd_init(&g->sink, &tun, tx_event) != 0) {
        log1("hp_tx init failed (alarm)");
        fault(BT_XREC_FAULT_CONFIG);
        return;
    }
    bt_hp_plat_attach_sink(hp_tx_bd(), &g->sink, &k_link, g->ring_is_dac);
    set_state(BT_HP_ST_READY);
    /* the gate takes the entropy mutex: evaluated outside bt_xrec_plat_lock() (a critical section on target) */
    const char *why = gate_block();
    bt_xrec_plat_lock();
    g->gate_ok = why == NULL ? 1u : 0u;
    bt_xrec_plat_unlock();
    lbegin("ready");
    lks("name", g->dev_name);
    lku("last", s_cfg.last_valid);
    lks("gate", why == NULL ? "ok" : why);
    lku("hp_rows", hp_rows_on() ? 1u : 0u);   /* 0: release image, search/connect refused (BT=1 BENCH=1 HP_GATES=1) */
    lend();
    rc_schedule(true);
}

/* ============================================ teardown ======================================================== */
/* bt_recover worker (down job, before esp_bluedroid_disable): design §5.5 steps 2-3. */
static int hp_profile_down(void)
{
    int rc = 0;
    uint32_t t0 = bt_xrec_plat_now_ms();
    hp_tx_bd_deinit();                           /* the tick was stopped by the app (step 1) */
    if (s_avrc_down) {
        s_avrc_down = 0u;
        if (esp_avrc_ct_deinit() != ESP_OK || esp_avrc_tg_deinit() != ESP_OK) {
            rc = 1;
        }
    }
    if (s_a2d_down) {
        s_a2d_down = 0u;
        s_a2d_deinit_seen = 0u;
        if (esp_a2d_source_deinit() == ESP_OK) {
            while (!s_a2d_deinit_seen && bt_xrec_plat_now_ms() - t0 < BT_HP_DEINIT_WAIT_MS) {
                bt_xrec_plat_wait();
            }
            if (!s_a2d_deinit_seen) {
                rc = 2;   /* hp_av_deinit_timeout (design §5.5): a fault for the app (patches 0039/0043 close a
                           * stream still closing locally, without an allocation; the btc_av state reset backstop is
                           * deferred: BT_A2DP_HP_SESSION.md) */
            }
        } else {
            rc = 3;
        }
    }
    s_down_ms = bt_xrec_plat_now_ms() - t0;
    return rc;
}

static void free_session(void)
{
    hp_t *b;
    bt_xrec_plat_lock();
    b = g;
    if (b != NULL) {
        b->cb_open = 0u;
    }
    g = NULL;
    bt_xrec_plat_unlock();
    if (b != NULL) {
        if (b->ring != NULL && !b->ring_is_dac) {
            bt_xrec_plat_free(b->ring);
        }
        memset(b, 0, sizeof(*b));
        bt_xrec_plat_free(b);
    }
}

static void sleep_1ms(uint32_t ms)
{
    (void)ms;
    bt_xrec_plat_wait();
}

static uint8_t s_hp_parked HP_HI;   /* F8: parked by a fault during a recording */
static int park_poll(void);

static int shutdown_now(void)
{
    if (s_hp_parked) {
        (void)park_poll();            /* F8: the deferred hard path once REC stopped */
        return (int)s_last_fault;
    }
    if (g == NULL) {
        return s_state == BT_HP_ST_DEAD ? (int)s_last_fault : 0;
    }
    uint32_t t0 = bt_xrec_plat_now_ms();
    /* 1. stop the tick in BTC context, bounded (design §4.3, §5.5 step 1); detach the audio output */
    bool tick_stuck = hp_tx_bd_stop_wait(BT_HP_TICK_STOP_MS, sleep_1ms) != 0;
    if (tick_stuck) {
        log1("tick did not stop in BTC context within the bound (teardown continues)");
    }
    bt_hp_plat_attach_sink(NULL, NULL, NULL, 0);
    window_close("stopping");
    bt_xrec_plat_lock();
    g->cb_open = 0u;                             /* late events are dropped; the down job still sees DEINIT */
    bt_xrec_plat_unlock();
    /* 2. graceful teardown in the worker (profile down -> disable -> bt_ctrl stop -> deinit) under one 2 s deadline */
    bt_xrec_plat_stack_down_begin();
    uint32_t td = bt_xrec_plat_now_ms();
    uint32_t reason = BT_XREC_FAULT_NONE;
    int r = 0;
    for (;;) {
        if (bt_xrec_plat_job_done(&r)) {
            if (bt_xrec_plat_deadline_disarm()) {
                reason = BT_XREC_FAULT_DOWN_LATCHED;
            } else if (r != 0 && r != 5) {
                reason = BT_XREC_FAULT_DOWN_FAIL;
            }
            break;
        }
        if (bt_xrec_plat_now_ms() - td >= BT_XREC_SHUTDOWN_DEADLINE_MS) {
            (void)bt_xrec_plat_deadline_disarm();
            reason = BT_XREC_FAULT_DOWN_DEADLINE;
            break;
        }
        bt_xrec_plat_service();
        bt_xrec_plat_wait();
    }
    lbegin("teardown");
    lku("ms", bt_xrec_plat_now_ms() - t0);
    lku("profile_ms", s_down_ms);
    lku("job_rc", (uint32_t)r);
    lku("reason", reason);
    lend();
    if (reason == BT_XREC_FAULT_NONE && tick_stuck && r != 0) {
        /* the tick never confirmed its stop and BTC did not finish the profile teardown either: the tick may still
         * run on BTC over the sink and ring free_session() returns (STEP10 A4) */
        reason = BT_XREC_FAULT_DOWN_FAIL;
    }
    bool profile_fault = false;
    if (r == 5 && reason == BT_XREC_FAULT_NONE) {
        /* the stack itself went down (no hard path), but btc_av / btc_avrc kept their init state: escalated to a
         * fault the screen shows ("BT fault", design §5.5 "offers a reboot instead of a silent failure"), and no
         * new session this boot (STEP10 A6; C14 only logged it and reported a normal OFF) */
        log1("A2DP deinit did not confirm in time (hp_av_deinit_timeout): headphones mode off until the next boot");
        s_restart_required = 1u;
        profile_fault = true;
    }
    if (reason != BT_XREC_FAULT_NONE) {
        /* the hard path first (it reboots on target): BTC may still use the sink / ring, so they are not freed under
         * it (STEP10 A4); the host harness's hard path returns, then the session is freed */
        bt_xrec_plat_set_mode(BT_XREC_MODE_PHONE, NULL);
        s_last_fault = reason;
        set_state(BT_HP_ST_DEAD);
        if (bt_xrec_plat_rec_active()) {
            /* F8: a recording runs: parked (the session stays allocated, see park()), hard path after REC */
            bt_xrec_plat_park(reason);
            s_hp_parked = 1u;
            return (int)reason;
        }
        bt_xrec_plat_hard_path(reason);
        free_session();
        return (int)reason;
    }
    free_session();
    bt_xrec_plat_session_end();        /* final bond flush to bt_hp_bonds.bin: still in headphones mode */
    bt_xrec_plat_set_mode(BT_XREC_MODE_PHONE, NULL);
    if (profile_fault) {
        s_last_fault = BT_XREC_FAULT_DOWN_FAIL;
        set_state(BT_HP_ST_FAULTED);
    } else {
        set_state(BT_HP_ST_OFF);
    }
    cfg_save();
    log1("stopped");
    return 0;
}

/* F8 (docs/BT_SYNC_SPEED_AND_RECORDING.md 2.5; bt_xrec_bluedroid.h bt_xrec_plat_park): a fault while a recording
 * runs. No graceful teardown, no automatic restart, no reboot: the output leaves the headset, late BTC callbacks are
 * dropped, the platform silences RW-BT. No Bluedroid API is called and the session memory stays allocated (the BTC
 * tick may still touch it). The deferred hard path runs once REC stops (bt_hp_poll / bt_hp_stop). */

static void park(uint32_t reason)
{
    bt_hp_plat_attach_sink(NULL, NULL, NULL, 0);
    bt_xrec_plat_lock();
    g->cb_open = 0u;
    g->link = BT_HP_LINK_NONE;
    bt_xrec_plat_unlock();
    s_auto_restart = 0u;
    bt_xrec_plat_park(reason);
    s_hp_parked = 1u;
    s_last_fault = reason;
    set_state(BT_HP_ST_DEAD);
    lbegin("fault while recording: BT silenced, recording continues, recovery after REC");
    lku("reason", reason);
    lend();
}

/* Parked: 1 while the recording runs; then the deferred hard path (target: reboot; host: returns 0). */
static int park_poll(void)
{
    if (!s_hp_parked) {
        return 0;
    }
    if (bt_xrec_plat_park_poll() != 0) {
        return 1;
    }
    s_hp_parked = 0u;
    return 0;
}

static void fault(uint32_t reason)
{
    if (g == NULL || s_state == BT_HP_ST_OFF || s_state == BT_HP_ST_FAULTED || s_state == BT_HP_ST_DEAD) {
        return;
    }
    s_last_fault = reason;
    lbegin("fault");
    lku("reason", reason);
    lend();
    if (bt_xrec_plat_rec_active()) {
        park(reason);                 /* F8: the recording first */
        return;
    }
    if (shutdown_now() == 0) {
        set_state(BT_HP_ST_FAULTED);
    }
}

/* ============================================ public API ====================================================== */
#if BENCH_IMAGE
void bt_hp_set_bench_tun(const bt_hp_bench_tun_t *t)
{
    s_tun_set = t != NULL ? 1u : 0u;
    if (t != NULL) {
        s_tun = *t;
    } else {
        memset(&s_tun, 0, sizeof(s_tun));   /* the run ended: no bench tuning (PIN 0000) for manual use */
    }
}
#endif

int bt_hp_start(exfat_vol_t *vol, int sd_mounted)
{
    if (g != NULL && (s_state == BT_HP_ST_UP || s_state == BT_HP_ST_CONFIG || s_state == BT_HP_ST_READY)) {
        return 0;
    }
    if (s_state == BT_HP_ST_DEAD || !bt_xrec_plat_start_allowed()) {
        log1("start refused: BT fault recorded this boot");
        return -1;
    }
    if (s_restart_required) {
        log1("start refused: the last A2DP teardown did not confirm (restart the recorder)");
        return -1;
    }
    bt_status_t ps;
    bt_xrec_get_status(&ps);
    if (ps.active) {
        log1("start refused: phone mode is running (one controller owner)");
        return -1;
    }
    cfg_load();
    hp_t *b = (hp_t *)bt_xrec_plat_alloc(sizeof(hp_t));
    if (b == NULL) {
        log1("start failed: no memory");
        return -1;
    }
    memset(b, 0, sizeof(*b));
    bt_xrec_plat_identity(b->bd);
    static const char hex[] = "0123456789ABCDEF";
    memcpy(b->dev_name, "Cassini_", 8u);
    b->dev_name[8] = hex[b->bd[0] >> 4];
    b->dev_name[9] = hex[b->bd[0] & 0x0Fu];
    b->dev_name[10] = '\0';
    b->cb_open = 1u;
    b->conn_hdl = ESP_A2D_INVALID_CONN_HDL;
    b->ct_hdl = ESP_A2D_INVALID_CONN_HDL;
    s_start_vol = vol;
    s_start_sd = sd_mounted;
    bt_xrec_plat_lock();
    g = b;
    bt_xrec_plat_unlock();
    s_a2d_down = s_avrc_down = 0u;
    set_state(BT_HP_ST_UP);
    lbegin("start (Bluedroid, headphones)");
    lks("name", b->dev_name);
    lku("last", s_cfg.last_valid);
    lku("out", s_cfg.out_mode);
#if BENCH_IMAGE
    lku("bench_tun", s_tun_set);
#endif
    lend();
    bt_xrec_plat_set_mode(BT_XREC_MODE_HP, hp_profile_down);
    if (bt_xrec_plat_stack_up_begin(vol, sd_mounted, b->bd) != 0) {
        log1("start failed: platform");
        bt_xrec_plat_set_mode(BT_XREC_MODE_PHONE, NULL);
        free_session();
        set_state(BT_HP_ST_OFF);
        return -1;
    }
    return 0;
}

static void ready_poll(uint32_t now)
{
    /* pairing window deadline */
    bt_xrec_plat_lock();
    bool win = g->win_open && time_reached(now, g->win_deadline);
    bt_xrec_plat_unlock();
    if (win) {
        window_close("deadline");
    }
    /* the gate the Just Works hook reads (design §6.3: checked on every poll while a window is open) */
    bool gate = gate_block() == NULL;
    bt_xrec_plat_lock();
    g->gate_ok = gate ? 1u : 0u;
    bt_xrec_plat_unlock();
    /* discovery length (the stack stops itself; this is the bound if it does not) */
    if (g->scanning && !g->scan_cancel && time_reached(now, g->scan_until)) {
        g->scan_cancel = 1u;
        g->scan_cancel_until = now + BT_HP_SCAN_CANCEL_TO_MS;
        (void)esp_bt_gap_cancel_discovery();
    } else if (g->scanning && g->scan_cancel && time_reached(now, g->scan_cancel_until)) {
        /* "discovery stopped" never came (event lost, cancel not posted): leave "Searching" so a pick and a new
         * search can run; bt_ctrl's inquiry flag still holds a pick back (below) */
        lbegin("discovery stop not reported: search cleared");
        lku("inq", (uint32_t)bt_ctrl_inquiry_active());
        lend();
        g->scanning = 0u;
        g->scan_cancel = 0u;
        set_msg(BT_HP_MSG_SCAN_DONE);
    }
    /* a pick waits for the discovery to stop (design §6.2), then for bt_ctrl's inquiry flag, at most 1 s more */
    if (g->pick_pending && !g->scanning && !orphan_active(now)) {
        if (!bt_ctrl_inquiry_active() || time_reached(now, g->pick_since + 1000u)) {
            g->pick_pending = 0u;
            if (g->pick_pair) {
                window_open(g->pick_bda);
            }
            connect_now(g->pick_bda);
        }
    }
    /* connect timeout (a page or pairing that never reported) */
    if (g->link == BT_HP_LINK_CONNECTING && time_reached(now, g->connect_t0)) {
        /* cancel by address: on a first connect there is no handle yet (it comes with CONNECTED), and the handle
         * call would return INVALID_ARG while Bluedroid keeps paging / pairing. Until that connect reports (or
         * another BT_HP_CONNECT_TO_MS passed) no new connect starts, and a late CONNECTED for another device is
         * closed (on_a2d_conn). */
        esp_err_t r = g->conn_hdl != ESP_A2D_INVALID_CONN_HDL ? esp_a2d_source_disconnect_by_handle(g->conn_hdl)
                                                              : a2d_cancel_by_addr(g->target);
        lbegin("connect timed out");
        lka("peer", g->target);
        lku("cancel_err", (uint32_t)r);
        lend();
        g->orphan = 1u;
        memcpy(g->orphan_bda, g->target, 6u);
        g->orphan_until = now + BT_HP_CONNECT_TO_MS;
        g->link = BT_HP_LINK_NONE;
        g->d.connect_fail++;
        set_msg(BT_HP_MSG_CONNECT_FAIL);
        rc_schedule(false);
    }
    rc_poll(now);
    rn_poll(now);
    /* START retry (design §4.5): once, BT_HP_START_RETRY_MS after the first failure (start_retries 2 = the retry is
     * out: its failure is final) */
    if (g->start_retries == 1u && g->mc_out == MC_NONE && g->link == BT_HP_LINK_OPEN && !g->start_failed &&
        time_reached(now, g->start_retry_at)) {
        g->start_retries = 2u;
        g->mc_want = MC_NONE;
        mc_issue(MC_START);
    }
    /* START failed for good: after the cooldown, start again if the player still wants audio (unmuted, ring filled
     * while it waited); a paused player (muted) waits for its next play press (link_request_start) */
    if (g->start_failed && time_reached(now, g->start_failed_at + BT_HP_START_COOLDOWN_MS)) {
        g->start_failed = 0u;
        g->start_retries = 0u;
        if (g->start_demand && !g->sink.mute && hp_sink_queued(&g->sink) > 0u) {
            log1("START: the player still waits after the cooldown, trying again");
            link_request_start();
        }
        g->start_demand = 0u;
    }
    /* idle suspend (design §6.5): STARTED, no real audio taken from the ring for BT_HP_IDLE_SUSPEND_MS */
    if (g->link == BT_HP_LINK_STARTED) {
        uint32_t real = hp_tx_bd()->st.real_pcm;
        if (real != g->last_real_pcm) {
            g->last_real_pcm = real;
            g->last_real_t = now;
        } else if (time_reached(now, g->last_real_t + BT_HP_IDLE_SUSPEND_MS) && g->mc_out == MC_NONE) {
            g->d.idle_suspends++;
            log1("idle: suspending the stream");
            g->mc_want = MC_SUSPEND;
            g->last_real_t = now;
        }
    }
    /* refused SUSPEND (audit r3 item 5): again after BT_HP_SUSPEND_RETRY_MS, unless the stream is no longer STARTED
     * or real audio / demand came since the refusal (playing again: the player wants the stream) */
    if (g->susp_retry_due && time_reached(now, g->susp_refused_at + BT_HP_SUSPEND_RETRY_MS) && g->mc_out == MC_NONE) {
        g->susp_retry_due = 0u;
        if (g->link == BT_HP_LINK_STARTED && (int32_t)(g->last_real_t - g->susp_refused_at) <= 0) {
            g->susp_retries++;
            log1("SUSPEND: trying again");
            g->mc_want = MC_SUSPEND;
        }
    }
    /* a media command that was never acknowledged */
    if (g->mc_out != MC_NONE && time_reached(now, g->mc_t + 5000u)) {
        log1("media command not acknowledged in 5 s");
        uint8_t out = g->mc_out;
        g->mc_out = MC_NONE;
        bt_xrec_plat_lock();
        g->start_pending = 0u;
        g->suspend_pending = 0u;
        bt_xrec_plat_unlock();
        if (out == MC_START) {
            start_failure("no ack", false);
        }
    }
    mc_pump();
    /* stall escalation (design §4.6, Q7): no Disconnection Complete within BT_HP_STALL_DISC_MS (the ROM keeps a dead
     * link for its supervision timeout, ~20-40 s on silicon) -> stop the controller and start headphones mode again,
     * which reconnects to the last headset (backoff from step 0). Bounded: one automatic restart per
     * BT_HP_AUTO_RESTART_MIN_MS; a second escalation inside that leaves the session FAULTED. */
    if (g->stall_disc && time_reached(now, g->stall_t + BT_HP_STALL_DISC_MS)) {
        g->stall_disc = 0u;
        bool may = !s_auto_restart_done || time_reached(now, s_auto_restart_t + BT_HP_AUTO_RESTART_MIN_MS);
        lbegin("stall: no Disconnection Complete after the 0406");
        lks("then", may ? "restarting headphones mode" : "headphones mode off (restarted recently)");
        lend();
        s_auto_restart = may ? 1u : 0u;
        fault(BT_XREC_FAULT_PLATFORM);
    }
}

/* The stall escalation's restart (ready_poll): once the fault's teardown left the session FAULTED. */
static void auto_restart_poll(void)
{
    if (!s_auto_restart || g != NULL || s_state != BT_HP_ST_FAULTED) {
        return;
    }
    s_auto_restart = 0u;
    s_auto_restart_done = 1u;
    s_auto_restart_t = bt_xrec_plat_now_ms();
    log1("restart after the stall escalation");
    if (bt_hp_start(s_start_vol, s_start_sd) != 0) {
        log1("restart refused: headphones mode stays off");
    }
}

void bt_hp_poll(void)
{
    if (s_hp_parked) {
        (void)park_poll();
        return;
    }
    if (g == NULL) {
        auto_restart_poll();
        return;
    }
    uint32_t now = bt_xrec_plat_now_ms();
    if (s_state == BT_HP_ST_UP) {
        int r = 0;
        if (bt_xrec_plat_job_done(&r)) {
            if (r != 0) {
                lbegin("bring-up failed");
                lku("code", (uint32_t)r);
                lend();
                fault(BT_XREC_FAULT_UP_FAIL);
                return;
            }
            config_begin();
        } else if (now - g->t_state >= BT_XREC_UP_DEADLINE_MS) {
            fault(BT_XREC_FAULT_UP_TIMEOUT);
            return;
        }
        if (g == NULL || s_state != BT_HP_ST_CONFIG) {
            return;
        }
    }
    ctrl_notes_drain();
    drain_events();
    if (g == NULL || (s_state != BT_HP_ST_CONFIG && s_state != BT_HP_ST_READY)) {
        return;
    }
    bt_xrec_plat_lock();
    uint8_t lost = g->evq_lost;
    bt_xrec_plat_unlock();
    if (lost) {
        fault(BT_XREC_FAULT_EVQ_OVERFLOW);
        return;
    }
    now = bt_xrec_plat_now_ms();
    if (s_state == BT_HP_ST_CONFIG) {
        if (g->prof_init && !g->seps_ok) {
            config_seps();
        }
        if (g != NULL && g->seps_ok && g->seps_pending == 0u && s_state == BT_HP_ST_CONFIG) {
            config_finish();
        }
        if (g != NULL && s_state == BT_HP_ST_CONFIG && now - g->t_state >= BT_XREC_CONFIG_DEADLINE_MS) {
            fault(BT_XREC_FAULT_CONFIG_TIMEOUT);
        }
        return;
    }
    ready_poll(now);
    if (g == NULL || s_state != BT_HP_ST_READY) {
        auto_restart_poll();          /* in the same poll: the UI and the bench never see the FAULTED gap */
        return;
    }
    ctrl_notes_drain();
    uint32_t why = bt_xrec_plat_poll();
    if (why != 0u) {
        lbegin("platform fault");
        lku("why", why);
        lend();
        fault(why == 5u ? BT_XREC_FAULT_ASSERT : BT_XREC_FAULT_PLATFORM);
    }
}

int bt_hp_stop(void)
{
    s_auto_restart = 0u;              /* an explicit stop (BACK + off, phone mode, USB disk, power) wins */
    if (s_hp_parked) {
        (void)park_poll();
        return (int)s_last_fault;
    }
    if (g == NULL) {
        return s_state == BT_HP_ST_DEAD ? (int)s_last_fault : 0;
    }
    return shutdown_now();
}

int bt_hp_active(void)
{
    return g != NULL && (s_state == BT_HP_ST_UP || s_state == BT_HP_ST_CONFIG || s_state == BT_HP_ST_READY);
}

int bt_hp_scan(uint8_t secs)
{
    if (g == NULL || s_state != BT_HP_ST_READY || g->scanning) {
        return -1;
    }
    if (g->link == BT_HP_LINK_STARTED || g->link == BT_HP_LINK_CONNECTING) {
        return -1;   /* design §11: no discovery while streaming; one page at a time */
    }
    if (hp_rows_refused("discovery")) {
        return -1;
    }
    if (secs < 2u || secs > 61u) {
        secs = BT_HP_SCAN_SECS_DEF;
    }
    uint32_t len = ((uint32_t)secs * 100u + 127u) / 128u;
    if (len < 1u) {
        len = 1u;
    }
    bt_xrec_plat_lock();
    g->n_found = 0u;
    memset(g->found, 0, sizeof(g->found));
    bt_xrec_plat_unlock();
    memset(g->rn_tried, 0, sizeof(g->rn_tried));
    g->rc_active = 0u;   /* a scheduled reconnect would page during the inquiry */
    esp_err_t r = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, (uint8_t)len, 0u);
    lbegin("discovery");
    lku("secs", secs);
    lku("inq_len", len);
    lku("err", (uint32_t)r);
    lend();
    if (r != ESP_OK) {
        set_msg(BT_HP_MSG_SCAN_FAIL);
        return -1;
    }
    g->scanning = 1u;
    g->scan_cancel = 0u;
    g->scan_until = bt_xrec_plat_now_ms() + (uint32_t)secs * 1000u + 15000u;   /* + BTA's name pass */
    return 0;
}

void bt_hp_scan_cancel(void)
{
    if (g != NULL && g->scanning && !g->scan_cancel) {
        g->scan_cancel = 1u;
        g->scan_cancel_until = bt_xrec_plat_now_ms() + BT_HP_SCAN_CANCEL_TO_MS;
        esp_err_t r = esp_bt_gap_cancel_discovery();
        lbegin("discovery cancel");
        lku("err", (uint32_t)r);
        lend();
    }
}

int bt_hp_connect_addr(const uint8_t bda[6], int pair)
{
    if (g == NULL || s_state != BT_HP_ST_READY) {
        return -1;
    }
    if (g->link != BT_HP_LINK_NONE && !(g->target_valid && bda_eq(g->target, bda))) {
        return -1;   /* one headset at a time (design §1) */
    }
    if (g->link != BT_HP_LINK_NONE) {
        return 0;
    }
    if (hp_rows_refused("connect")) {
        return -1;
    }
    if (!pair && !bonded(bda)) {
        pair_again("no stored key");   /* a connect without a pairing window cannot pair: refuse at once */
        return -1;
    }
    if (pair) {
        const char *why = gate_block();
        if (why != NULL) {
            lbegin("pairing unavailable");
            lks("why", why);
            lend();
            set_msg(BT_HP_MSG_PAIRING_UNAVAILABLE);   /* still connects: a bonded headset needs no pairing */
        }
    }
    memcpy(g->pick_bda, bda, 6u);
    g->pick_pair = pair ? 1u : 0u;
    g->pick_pending = 1u;
    g->pick_since = bt_xrec_plat_now_ms();
    g->user_disc = 0u;
    g->rc_active = 0u;
    g->pair_again = 0u;               /* the user picked a headset: reconnects may run again after it connects */
    if (g->forgot_valid && bda_eq(g->forgot, bda)) {
        g->forgot_valid = 0u;
    }
    found_name(bda, g->target_name);
    target_set(bda);
    bt_hp_scan_cancel();
    lbegin("pick");
    lka("peer", bda);
    lku("pair", g->pick_pair);
    lks("name", g->target_name);
    lend();
    return 0;
}

int bt_hp_pick(uint8_t idx, int pair)
{
    if (g == NULL) {
        return -1;
    }
    uint8_t bda[6];
    bt_xrec_plat_lock();
    bool ok = idx < g->n_found;
    if (ok) {
        memcpy(bda, g->found[idx].bda, 6u);
    }
    bt_xrec_plat_unlock();
    return ok ? bt_hp_connect_addr(bda, pair) : -1;
}

int bt_hp_disconnect(void)
{
    if (g == NULL || s_state != BT_HP_ST_READY) {
        return -1;
    }
    g->user_disc = 1u;
    g->rc_active = 0u;
    if (g->link == BT_HP_LINK_NONE) {
        return 0;
    }
    /* while CONNECTING on a first connect there is no handle yet: cancel by address */
    esp_err_t r = g->conn_hdl != ESP_A2D_INVALID_CONN_HDL || !g->target_valid
                      ? esp_a2d_source_disconnect_by_handle(g->conn_hdl)
                      : a2d_cancel_by_addr(g->target);
    if (g->link == BT_HP_LINK_CONNECTING && g->target_valid) {
        g->orphan = 1u;               /* a late CONNECTED is closed (user_disc), no new connect until it reports */
        memcpy(g->orphan_bda, g->target, 6u);
        g->orphan_until = bt_xrec_plat_now_ms() + BT_HP_CONNECT_TO_MS;
        g->link = BT_HP_LINK_NONE;
    }
    lbegin("disconnect");
    lku("err", (uint32_t)r);
    lend();
    return r == ESP_OK ? 0 : -1;
}

int bt_hp_forget(void)
{
    cfg_load();
    if (!s_cfg.last_valid) {
        return -1;
    }
    uint8_t bda[6];
    memcpy(bda, s_cfg.last, 6u);
    /* only with the stack up and the removal posted: otherwise the link key stayed in bt_hp_bonds.bin while the menu
     * entry to remove it went away (STEP10 C8); the UI offers Forget only in READY */
    if (g == NULL || s_state != BT_HP_ST_READY) {
        log1("forget refused: headphones mode not ready");
        return -1;
    }
    g->user_disc = 1u;
    g->rc_active = 0u;
    esp_err_t r = esp_bt_gap_remove_bond_device(bda);   /* also takes the link down (bta_dm_remove_device) */
    lbegin("forget");
    lka("peer", bda);
    lku("err", (uint32_t)r);
    lend();
    if (r != ESP_OK) {
        return -1;                       /* entry kept: Forget can be chosen again */
    }
    set_msg(BT_HP_MSG_FORGOTTEN);
    g->forgot_valid = 1u;             /* a connect still under way for it is closed when it reports */
    memcpy(g->forgot, bda, 6u);
    g->rn_want = 0u;
    g->pair_again = 0u;
    s_cfg.last_valid = 0u;
    memset(s_cfg.last, 0, sizeof(s_cfg.last));
    memset(s_cfg.last_name, 0, sizeof(s_cfg.last_name));
    cfg_save();
    return 0;
}

void bt_hp_get_ui(bt_hp_ui_t *out)
{
    memset(out, 0, sizeof(*out));
    cfg_load();
    out->state = s_state;
    out->bt_vol = s_cfg.bt_vol;
    out->has_last = s_cfg.last_valid;
    memcpy(out->last, s_cfg.last, 6u);
    memcpy(out->last_name, s_cfg.last_name, sizeof(out->last_name));
    if (g == NULL) {
        return;
    }
    uint32_t now = bt_xrec_plat_now_ms();
    out->link = g->link;
    out->scanning = g->scanning;
    if (g->scanning && !time_reached(now, g->scan_until - 15000u)) {
        out->scan_s_left = (uint8_t)((g->scan_until - 15000u - now + 999u) / 1000u);
    }
    bt_xrec_plat_lock();
    out->n_found = g->n_found;
    memcpy(out->found, g->found, sizeof(out->found));
    out->pairing = g->win_open;
    out->rate = g->cfg_valid ? g->cfg_rate : 0u;
    out->bp_min = g->cfg_bp_min;
    out->bp_max = g->cfg_bp_max;
    bt_xrec_plat_unlock();
    out->target_valid = g->target_valid;
    memcpy(out->target, g->target, 6u);
    memcpy(out->target_name, g->target_name, sizeof(out->target_name));
    out->reconnecting = g->rc_active || (g->link == BT_HP_LINK_CONNECTING && !g->pick_pair);
    if (g->rc_active && !time_reached(now, g->rc_at)) {
        out->reconnect_s = (uint16_t)((g->rc_at - now + 999u) / 1000u);
    }
    out->fpp = (uint8_t)hp_tx_bd()->fpp;
    out->abs_vol = g->abs_vol;
    out->msg = g->msg;
    out->msg_seq = g->msg_seq;
}

void bt_hp_get_debug(bt_hp_debug_t *out)
{
    if (g == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = g->d;
}

int bt_hp_stream_ready(void)
{
    return link_stream_ready();
}

void bt_hp_suspend(void)
{
    if (g != NULL && s_state == BT_HP_ST_READY && g->link == BT_HP_LINK_STARTED) {
        g->mc_want = MC_SUSPEND;
        mc_pump();
    }
}

uint16_t bt_hp_acl_handle(void)
{
    uint16_t h = 0xFFFFu;
    bt_xrec_plat_lock();
    if (g != NULL && g->acl_valid) {
        h = g->acl_handle;
    }
    bt_xrec_plat_unlock();
    return h;
}

int bt_hp_abs_vol(void)
{
    return g != NULL && s_state == BT_HP_ST_READY && g->ct_conn && g->abs_vol;
}

int bt_hp_streaming(void)
{
    return g != NULL && g->link == BT_HP_LINK_STARTED;
}

int bt_hp_take_key(void)
{
    int k = BT_HP_KEY_NONE;
    bt_xrec_plat_lock();
    if (g != NULL && g->kq_head != g->kq_tail) {
        k = g->keyq[g->kq_tail % HP_KEYQ];
        g->kq_tail++;
    }
    bt_xrec_plat_unlock();
    return k;
}

uint32_t bt_hp_take_player_events(void)
{
    uint32_t v = 0u;
    bt_xrec_plat_lock();
    if (g != NULL) {
        v = g->player_ev;
        g->player_ev = 0u;
    }
    bt_xrec_plat_unlock();
    return v;
}

void bt_hp_player_begin(void)
{
    bt_xrec_plat_lock();
    if (g != NULL) {
        g->player_ev = 0u;
        g->kq_tail = g->kq_head;
    }
    bt_xrec_plat_unlock();
}

void bt_hp_set_vol31(uint8_t v31)
{
    cfg_load();
    if (v31 > 31u) {
        v31 = 31u;
    }
    uint8_t v = (uint8_t)((v31 * 127u + 15u) / 31u);
    if (v == s_cfg.bt_vol) {
        return;
    }
    s_cfg.bt_vol = v;
    if (g != NULL && s_state == BT_HP_ST_READY) {
        abs_vol_send();
    }
}

/* Phone mode's bt_shutdown() runs this first (weak default in bt_xrec_bluedroid.c): one exit path for both modes. */
int bt_hp_shutdown_hook(void)
{
    return bt_hp_stop();
}
