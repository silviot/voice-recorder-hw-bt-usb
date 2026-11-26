/* bt_xrec_bluedroid: the phone-mode XREC session on Bluedroid for BT=1 (plan docs/BLUETOOTH_BLUEDROID_PLAN.md v18 M4
 * "XREC session" + "Pairing security policy"). It implements the API the UI already calls (include/openfw.h:
 * bt_xrec_start / poll / stop / open_pairing / get_status) and replaces the legacy src/bt_hci.c + src/bt_spp.c
 * implementation at link time (open_fw/Makefile renames the legacy symbols in BT=1 images; default image untouched).
 *
 * Ownership (plan D7, M4):
 *   - The app task owns the session: it drains the event queue, feeds xrec_proto, writes with esp_spp_write(),
 *     answers pairing requests and runs bt_shutdown(). Everything here except the two Bluedroid callbacks runs in the
 *     app task.
 *   - The BTC callbacks (GAP, SPP) only copy: events into a bounded record queue, SPP payload into a bounded byte ring,
 *     both under one short critical section, then notify the app. They never call Bluedroid, xrec_proto or the FS.
 *   - Stack bring-up (bt_ctrl + esp_bluedroid_init/enable) and graceful teardown (esp_spp_deinit + disable + bt_ctrl
 *     stop + deinit) run as jobs in the bt_recover worker (firmware: bt_xrec_fw.c); the app only waits on them against
 *     one absolute deadline and takes the hard path (bt_xrec_plat_hard_path) when it passes.
 *
 * The bt_xrec_plat_* functions below are the platform seam: firmware implementation in bt_xrec_fw.c (+
 * bt_xrec_spp_mtu.c), mocks in tests/test_bt_xrec.c. They are bound at link time.
 * SPDX-License-Identifier: MIT */
#ifndef BT_XREC_BLUEDROID_H
#define BT_XREC_BLUEDROID_H

#include <stddef.h>
#include <stdint.h>
#include "openfw_fs.h"
#include "openfw_xrec.h"

/* ---- sizing (plan §4 "SPP mode RFCOMM queues + XREC RX stream buffer", heap, per session) ---- */
#define BT_XREC_RX_RING        2048u   /* SPP payload not yet parsed by the app (XREC PUT is stop-and-wait, <= ~700 B) */
/* TX sizing (docs/BT_SYNC_THROUGHPUT.md, silicon 2026-11-26: 2048/2048 + one write per frame cut at the ring wrap gave
 * ~39 KB/s with ~545 B ACL packets; 8192 + 3960 + coalesced 990 B writes gave 51-57 KB/s with ~860 B packets). */
#define BT_XREC_TX_RING        8192u   /* bytes accepted from xrec_proto, not yet handed to esp_spp_write() */
#define BT_XREC_WRITE_MAX      990u    /* one esp_spp_write() (ESP_SPP_MAX_MTU) = one RFCOMM frame */
#define BT_XREC_SPP_TX_BUF     (4u * BT_XREC_WRITE_MAX)  /* esp_spp_cfg_t.tx_buffer_size: BTC's ring, 4 whole writes */
#define BT_XREC_INFLIGHT_MAX   BT_XREC_SPP_TX_BUF  /* written but not yet reported by ESP_SPP_WRITE_EVT */
/* Coalescing: while a GETS stream keeps producing, a write shorter than BT_XREC_WRITE_MAX waits for more frames, at
 * most this long (app-task ms clock, so 5-6 ms). Text replies (anything but a GETS data frame) are never held. */
#define BT_XREC_COALESCE_MS    5u
#define BT_XREC_BENCH_TX_RING_MAX BT_XREC_TX_RING   /* bench images: largest TX ring bt_xrec_set_bench_tput() takes */
#define BT_XREC_EVQ_SLOTS      32u     /* GAP/SPP event records (DATA/WRITE coalesce, so this only holds control events) */
#define BT_XREC_SPP_SCN        1u      /* RFCOMM channel 1: tools/bt_sync.py connects to it directly (no SDP) */
#define BT_XREC_SPP_NAME       "Cassini"  /* legacy SDP ServiceName (src/bt_spp.c:352) */

/* ---- deadlines (ms) ---- */
#define BT_XREC_SHUTDOWN_DEADLINE_MS 2000u   /* plan M1 recovery ladder: one absolute 2 s deadline */
#define BT_XREC_UP_DEADLINE_MS       12000u  /* bt_ctrl open + HCI Reset (RF calibration) + Bluedroid enable */
#define BT_XREC_CONFIG_DEADLINE_MS   5000u   /* SPP INIT + START events after enable */
#define BT_XREC_REBOOT_DELAY_MS      250u    /* legacy F:1827: deferred reboot / OTA / DFU */
#define BT_XREC_REBOOT_DRAIN_MAX_MS  2000u   /* longest wait for the final reply to leave before rebooting anyway */
/* Review STEP10 C11: esp_spp_disconnect() fails only when BTC's message cannot be allocated. The close is retried
 * every BT_XREC_CLOSE_RETRY_MS; after BT_XREC_CLOSE_RETRIES refusals the session faults (graceful recovery). */
#define BT_XREC_CLOSE_RETRY_MS       100u
#define BT_XREC_CLOSE_RETRIES        20u
#define BT_XREC_SSP_SHOW_MS          30000u  /* numeric-comparison value shown at most this long (bench autoconfirm) */
#define BT_XREC_SSP_PROMPT_MS        30000u  /* plan M4: local accept/reject prompt, rejected when it runs out */
#define BT_XREC_SSP_ARM_MS           500u    /* answers in the first 0.5 s are ignored (a key meant for something else) */
#define BT_XREC_SSP_AFTER_MS         1000u   /* ENTER/BACK within 1 s after a prompt ended are swallowed (-3), not
                                              * taken as "open a window" / "leave BT mode" (the prompt can end by
                                              * itself just before the key, and the screen lags up to 150 ms) */
#define BT_XREC_SCAN_RETRY_MS        3000u   /* a refused scan mode (ROM 0C1A rule: > 1 ACL link) is sent again */
#define BT_XREC_PIN_KEEP_MS          10000u  /* re-opening a window with more than this left keeps its XREC PIN */
#define BT_XREC_PEER_NAME_MAX        24u     /* bytes incl. NUL: remote name (UTF-8, cut at a character) or address */

/* ---- BT fault reasons (logged; code of the hard-path fault record 0xB1) ---- */
enum {
    BT_XREC_FAULT_NONE = 0,
    BT_XREC_FAULT_UP_FAIL = 1,        /* bring-up job reported an error */
    BT_XREC_FAULT_UP_TIMEOUT = 2,
    BT_XREC_FAULT_CONFIG = 3,         /* a GAP/SPP configuration call or event failed */
    BT_XREC_FAULT_CONFIG_TIMEOUT = 4,
    BT_XREC_FAULT_EVQ_OVERFLOW = 5,   /* a control event could not be queued: the session state is no longer known */
    BT_XREC_FAULT_PLATFORM = 6,       /* bt_ctrl fault / hang, HCI command timeout, stalled BT task (plat_poll) */
    BT_XREC_FAULT_DOWN_DEADLINE = 7,  /* graceful teardown did not finish within BT_XREC_SHUTDOWN_DEADLINE_MS */
    BT_XREC_FAULT_DOWN_FAIL = 8,      /* graceful teardown reported an error */
    BT_XREC_FAULT_DOWN_LATCHED = 9,   /* teardown finished, but the deadline had already latched bt_stop */
    BT_XREC_FAULT_CLOSE_FAIL = 10,    /* esp_spp_disconnect() refused BT_XREC_CLOSE_RETRIES times (review STEP10 C11) */
    BT_XREC_FAULT_ASSERT = 11,        /* assert() in a BT image (review STEP10 A9): hard path; fault record b = line */
};

/* Session states (bt_xrec_debug_t.state). */
enum {
    BT_XREC_ST_OFF = 0,
    BT_XREC_ST_UP,        /* bring-up job running */
    BT_XREC_ST_CONFIG,    /* Bluedroid enabled; GAP set, waiting for SPP INIT/START */
    BT_XREC_ST_READY,     /* SPP server listening on channel 1 */
    BT_XREC_ST_FAULTED,   /* graceful recovery done after a fault: off until the next bt_xrec_start() */
    BT_XREC_ST_DEAD,      /* hard path taken (host tests only: on target it reboots), or parked (F8, below) */
};

/* Coordinated teardown (plan M4): every exit (BT-mode exit, power-off, deferred reboot/OTA/DFU, faults) goes through
 * here. App task only. Returns 0 when BT is off (graceful), BT_XREC_FAULT_* when the hard path was taken (on target it
 * does not return then). */
int bt_shutdown(void);

/* Observability for tests and logs. */
typedef struct {
    uint8_t  state, window_open, scan_disc, scan_applied, conn_open, conn_authorized, conn_closing, cong;
    uint8_t  spp_inited, ssp_showing, entropy_fail, reboot_mode, ssp_window;
    uint32_t conn_handle, tx_used, rx_used, inflight, evq_used, acl_links;
    uint16_t mtu;
    uint32_t last_fault;
    /* counters */
    uint32_t rx_frames, rx_bytes, rx_dropped, rx_overflows, tx_writes, tx_bytes, tx_nomem, tx_errors, tx_busy;
    uint32_t rx_held;         /* times xrec_proto held a received command back for TX room (review STEP10 A5) */
    uint32_t write_evts, write_evt_bytes, cong_evts, evq_lost, stale_events;
    uint32_t ssp_accepted, ssp_rejected, pin_rejected, passkey_rejected, bonds_removed, conns_rejected;
    uint32_t conns_authorized;   /* SPP connections marked authorized for XREC */
    uint8_t  ssp_prompt;         /* a numeric comparison waits for the local user */
    uint32_t ssp_user_accepted, ssp_user_rejected, ssp_timeouts, ssp_withdrawn, ssp_bench_autoconfirmed;
    uint32_t ssp_auto_accepted;  /* accepted by the auto-accept policy (bt_xrec_set_ssp_auto_accept) */
    uint32_t name_requests;      /* esp_bt_gap_read_remote_name() calls */
    uint32_t name_failed;        /* ESP_BT_GAP_READ_REMOTE_NAME_EVT with a failure status */
    uint32_t names_from_ctrl;    /* remote names taken from a Remote Name Request Complete (bt_xrec_plat_ctrl_name) */
    uint32_t acl_closing;        /* ACL links the host is disconnecting (0406 answered), not yet reported down */
    uint8_t  ssp_pending;        /* window opened before READY: the M2.3 gate decides its SSP part once READY */
    uint32_t ssp_gate_refused;   /* SSP part of a pairing window refused by the M2.3 gate (open, or deferred to READY) */
    uint8_t  ssp_burned;         /* a pairing this session accepted then failed: SSP refused until the next BT entry */
    uint32_t ssp_gate_cfm_rejects;  /* numeric comparisons rejected because the gate stopped holding (request/answer) */
    uint32_t ctrl_notes;         /* controller link notes logged (bt_xrec_plat_ctrl_note) */
    uint32_t scan_refused;       /* scan mode refused by the controller (more than one ACL link) */
    uint32_t scan_reapplied;     /* scan mode re-applied after a refusal (a link went down, or BT_XREC_SCAN_RETRY_MS) */
    uint32_t scan_failsafe;      /* scan switched off (0x00) because a write dropping discoverability was refused */
    uint32_t rx_lost;            /* SPP reported an RX frame lost for lack of memory (patch 0044): link closed */
    uint32_t close_retries;      /* esp_spp_disconnect() retried after a refusal (review STEP10 C11) */
    uint8_t  parked;             /* F8: a fault during a recording silenced BT (state DEAD); hard path after REC */
    uint32_t lk_refreshed;       /* patch 0048: ESP_BT_LINK_KEY_CHG_COMB kept on an existing MITM bond */
    uint32_t pokes_sent;         /* bt_xrec_poke_bonded(): esp_spp_start_discovery() calls accepted */
    uint32_t pokes_done;         /* ESP_SPP_DISCOVERY_COMP_EVT received */
} bt_xrec_debug_t;
void bt_xrec_get_debug(bt_xrec_debug_t *out);

/* Live sync / sidecar-upload telemetry + outbound doorbell poke for background sync (src/main.c, app task only). */
typedef struct {
    uint8_t  sync_seen;          /* 1 once the current session received +XREC:SYNC,<pending> */
    uint16_t sync_pending;       /* last reported pending items (downloads + transcriptions + uploads) */
    uint16_t sync_eta_sec;       /* optional ETA hint from +XREC:SYNC */
    uint32_t sync_seq;           /* increments on every +XREC:SYNC command */
    uint32_t cwx_seq;            /* increments on every committed PUT REC,<stem>.cwx */
    uint8_t  last_cwx_state;     /* 1 = words > 0, 2 = 0-word ("No speech") */
    char     last_cwx_name[32];  /* committed "<stem>.cwx" */
    uint8_t  poke_busy;          /* 1 while esp_spp_start_discovery() is in flight */
    uint8_t  last_peer_valid;    /* 1 once an authorized MITM peer connected or bonded this session */
    uint8_t  last_peer_bda[6];
} bt_xrec_sync_info_t;
void bt_xrec_get_sync_info(bt_xrec_sync_info_t *out);
void bt_xrec_clear_sync_done(void);
/* Outbound "doorbell poke": while READY with no open link and no open pairing window, page a MITM-bonded phone and
 * query its SDP server (esp_spp_start_discovery) so Android wakes CompanionDeviceService / ACTION_ACL_CONNECTED and
 * opens RFCOMM back to our server. Prefers hint_bda (when non-NULL and MITM-bonded), else the first MITM bond in
 * Bluedroid's store. Returns 0 when discovery was started, -1 otherwise. App task only. */
int  bt_xrec_poke_bonded(const uint8_t hint_bda[6]);
/* 1 when Bluedroid's loaded bond store holds at least one MITM-authenticated bond (writes its BDA to out_bda if
 * non-NULL). App task, stack READY. */
int  bt_xrec_has_mitm_bond(uint8_t out_bda[6]);

/* Persistent phone-sync & smart auto-power-off settings (SD:/bt_phone_cfg.bin, BT=1 only; settings.bin untouched). */
#define BT_PHONE_CFG_MAGIC 0x31435042u /* "BPC1" LE */
typedef struct {
    uint8_t  auto_sync;          /* 1 = automatic background sync on (default), 0 = manual only */
    uint8_t  auto_off_idx;       /* 0 = Off, 1 = 2m, 2 = 5m (default), 3 = 15m */
    uint8_t  sync_pending;       /* 1 = untranscribed recording / unfinished sync awaits phone */
    uint8_t  phone_valid;        /* 1 = phone_bda holds the most recent authorized phone */
    uint8_t  phone_bda[6];
} bt_phone_cfg_t;
void bt_phone_cfg_defaults(bt_phone_cfg_t *cfg);
int  bt_phone_cfg_load(bt_phone_cfg_t *cfg);
int  bt_phone_cfg_save(const bt_phone_cfg_t *cfg);
/* Cheap file-existence check on the boot volume before spinning up Bluedroid in the background: 1 iff /cassini.key
 * (36 B) and a non-empty /bt_bonds.bin (> 48 B; an empty bt_store image is 43 B) both exist on the mounted card. */
int  bt_phone_has_bond_files(void);

/* ---- BENCH images only (make BT=1 BENCH=1; SD:/BTBENCH.RUN phone keys, docs/BT_SYNC_THROUGHPUT.md) ----
 * TX path knobs for the sync-throughput A/B runs. Defaults = the release behaviour (990, 3960, 8192, coalesce 1). Set
 * before the SPP connection (bt_bench_begin, right after bt_xrec_start): refused while TX data is queued. wr_max
 * 64..990, spp_buf wr_max..9900 (BTC ring and in-flight cap), tx_ring 2048/4096/8192, coalesce 0|1 (0 = the pre-
 * 2026-11-26 path: one write per frame, cut at the ring wrap, never held). NULL restores the defaults. App task. */
typedef struct {
    uint16_t wr_max;
    uint16_t spp_buf;
    uint16_t tx_ring;
    uint8_t  coalesce;
} bt_xrec_bench_tput_t;
void bt_xrec_set_bench_tput(const bt_xrec_bench_tput_t *t);
void bt_xrec_set_bench_clock_us(uint32_t (*now_us)(void));   /* xrec_poll timing + coalesce wait bound; NULL = off */
typedef struct {
    uint32_t tx_used, tx_size, inflight, inflight_cap, handle;
    uint8_t  cong, gets, conn;
    uint32_t xrec_us, xrec_n;   /* cumulative: time in xrec_poll (GETS: SD read + CRC + framing), calls */
    uint32_t co_waits, co_forced, wr_wrap;   /* coalesce holds, 5 ms forced short writes, writes cut by the ring wrap */
} bt_xrec_bench_tput_snap_t;
void bt_xrec_bench_tput_snap(bt_xrec_bench_tput_snap_t *o);   /* any task: racy reads, diagnostics only */

/* ---- local pairing prompt (plan M4 "Pairing security policy"; replaces interim deviation 1) ----
 * An SSP numeric comparison that arrives inside the user-opened pairing window is held for the local user: the BT
 * screen shows the 6-digit value and the peer, ENTER accepts, BACK rejects, BT_XREC_SSP_PROMPT_MS without an answer
 * rejects. It is also rejected when the window closes first (deadline, XREC PAIR, first MITM bond), withdrawn without a
 * reply when the peer's pairing ends first (AUTH_CMPL, ACL down) or BT stops, and a second request while one is
 * pending is rejected (same peer: the pending one is withdrawn too). Outside the window: rejected without a prompt.
 * The reply goes through esp_bt_gap_ssp_confirm_reply() from the app task (BTC -> BTA DM -> BTM_ConfirmReqReply, which
 * ignores a reply whose pairing is gone), the same path the interim auto-confirm used. App task only. */
typedef struct {
    uint8_t  prompt;                       /* 1: waiting for ENTER / BACK */
    uint8_t  prompt_armed;                 /* answers are taken (BT_XREC_SSP_ARM_MS after it appeared) */
    uint8_t  prompt_s_left;                /* seconds until the prompt rejects by itself */
    char     digits[8];                    /* "123456" while prompt */
    char     peer[BT_XREC_PEER_NAME_MAX];  /* prompt peer, else the connected/bonding peer: name, else address */
    uint8_t  peer_bda[6];
    uint8_t  peer_is_name;                 /* peer holds the remote name (else the address text) */
    uint8_t  window_open;                  /* the user-opened pairing window (XREC PIN and/or SSP) */
    uint16_t window_s_left;
    uint8_t  connected;                    /* SPP open, or an ACL link up that the host is not disconnecting */
    uint8_t  bonded;                       /* that peer holds a MITM bond (just bonded, or authorized SPP link) */
} bt_xrec_ui_t;
void bt_xrec_get_ui(bt_xrec_ui_t *out);
/* Changes whenever a prompt appears or ends, the pairing window opens or closes, a bench-accepted value is shown or
 * the M2.3 gate refuses a window: the BT screen redraws at once instead of at its paced period (main.c,
 * openfw_bt_ui_pace.h). Cheap (no lock, no copy). App task only. */
uint32_t bt_xrec_ui_sig(void);
/* The local user's answer: accept != 0 accepts. 0 = applied, -1 no prompt pending, -2 not armed yet (ignored),
 * -3 no prompt pending but one ended less than BT_XREC_SSP_AFTER_MS ago (swallowed: the key was meant for it).
 * The caller treats every value but -1 as consumed. Accepting re-checks the M2.3 gate first (rejects if it fails). */
int  bt_xrec_ssp_answer(int accept);
/* SSP numeric-comparison policy (C2 / A8, user decision 2026-11-25: convenience over local confirmation). On
 * (BT_XREC_SSP_AUTO_ACCEPT_DEFAULT, 1): a comparison inside an open pairing window that passes the M2.3 gate is
 * accepted at once, no ENTER / BACK prompt; the value is shown through bt_status_t.pair_pin for BT_XREC_SSP_SHOW_MS
 * and logged. The pairing window (opened by the user on the device, time-limited, closed by the first MITM bond, key
 * spent after a failed accepted pairing) is the protection. Off: the plan M4 prompt. Survives bt_xrec_start. Any task. */
#ifndef BT_XREC_SSP_AUTO_ACCEPT_DEFAULT
#define BT_XREC_SSP_AUTO_ACCEPT_DEFAULT 1
#endif
void bt_xrec_set_ssp_auto_accept(int on);
#if defined(OPENFW_BT_BENCH_IMAGE) && OPENFW_BT_BENCH_IMAGE
/* Bench images only (make BT=1 BENCH=1; a release BT=1 image has neither function nor the code behind them).
 * BENCH ONLY (default off; survives bt_xrec_start): numeric comparisons inside the pairing window are accepted without
 * the user, like interim deviation 1, and every use logs "BENCH autoconfirm". Never enabled by the UI. Any task. */
void bt_xrec_set_bench_autoconfirm(int on);
/* BENCH ONLY (default off; survives bt_xrec_start; SD:/BTBENCH.RUN xrec_pin=): the XREC PIN of every later pairing
 * window is pin6 ("NNNNNN") instead of a DRBG draw; still only issued when the DRBG draw succeeds (entropy credited).
 * NULL or anything but 6 digits switches it off. Logged on every use. App task. */
void bt_xrec_set_bench_xrec_pin(const char *pin6);
#endif

/* 1 once ESP_SPP_UNINIT_EVT arrived after the last bt_shutdown() began (the teardown job waits for it). Any task. */
int bt_xrec_spp_uninit_seen(void);
/* 1 once esp_spp_enhanced_init() posted BTC_SPP_ACT_INIT (even before BTC_TASK sets spp_inited). Any task. */
int bt_xrec_spp_deinit_needed(void);

/* ======================================== platform seam ======================================================= */
/* Any context. */
uint32_t bt_xrec_plat_now_ms(void);
void     bt_xrec_plat_lock(void);            /* short critical section shared with the BTC callbacks */
void     bt_xrec_plat_unlock(void);
void     bt_xrec_plat_notify_app(void);      /* never blocks (BTC context) */
void     bt_xrec_plat_log(const char *msg);  /* one line, no secrets */
void     bt_xrec_plat_log_u32(const char *msg, uint32_t v);
/* BTC context: negotiated RFCOMM MTU of an open SPP connection (0 = unknown). */
uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle);
/* App task, stack enabled (bt_xrec_bond.c, under btc_config's mutex): 1 if the stored bond of bda holds an
 * authenticated combination key (P-192 0x05 / P-256 0x08); purge drops bda's bond from the BTC config at once (the
 * next bond-file write cannot carry it), independent of BTA DM's deferred unpair. */
int      bt_xrec_plat_bond_mitm(const uint8_t bda[6]);
void     bt_xrec_plat_bond_purge(const uint8_t bda[6]);

/* App task. */
void    *bt_xrec_plat_alloc(size_t n);       /* NULL on failure (fail soft) */
void     bt_xrec_plat_free(void *p);
int      bt_xrec_plat_start_allowed(void);   /* 0 after a BT-fault reboot (fault record 0xB1): BT stays off this boot */
void     bt_xrec_plat_identity(uint8_t bd_addr[6]);
int      bt_xrec_plat_entropy(void *out, size_t n);   /* 0 = OK; anything else fails closed (plan M2) */
/* Plan M2.3 pairing gate: NULL when a new SSP pairing may be offered (entropy credited, ROM P-192 key replaced, ROM
 * RNG pool never ran dry this session), else a short reason for the log. Checked when the window opens and on every
 * poll while its SSP part is open. */
const char *bt_xrec_plat_pairing_block(void);
/* The gate's RNG part alone, lock-free and callable from any task or inside bt_xrec_plat_lock(): nonzero once the ROM
 * RNG pool ran dry at a security call site this session (latched until the next session). Pairing decisions taken
 * between two polls (the patch 0036 Just Works hook in BTC, the confirmation replies) read it next to the polled gate
 * (STEP10 C3). */
int      bt_xrec_plat_rng_starved(void);
const xrec_ops_t *bt_xrec_plat_device_ops(void);       /* stat, rtc, recorder, log, audio cfg, exp (legacy F:1733-1966) */
void     bt_xrec_plat_load_bound_key(xrec_ctx_t *ctx);  /* SD:/cassini.key (legacy F:1968, same CSK5 format) */
int      bt_xrec_plat_save_bound_key(const uint8_t key[32]);  /* 0 saved (or no card: RAM-only binding), -1 write failed */
/* Bring-up, app part (bt_port_init, bond store loaded from SD before esp_bluedroid_init), then posts the "up" job to
 * the worker (bt_ctrl open + HCI driver attach + esp_bluedroid_init + enable). 0 = job posted. */
int      bt_xrec_plat_stack_up_begin(exfat_vol_t *vol, int sd_mounted, const uint8_t bd_addr[6]);
/* Posts the graceful "down" job (esp_spp_deinit, disable, bt_ctrl stop, deinit) and arms the hard-stop deadline. */
void     bt_xrec_plat_stack_down_begin(void);
/* App task, before bt_xrec_plat_stack_up_begin(): the session mode (one bt_ctrl owner: phone and headphones mode never
 * run at once). Picks the bond file (phone: bt_bonds.bin, headphones: bt_hp_bonds.bin) and resets the RAM bond store
 * when the mode differs from the one whose file it last loaded (docs/BT_A2DP_DESIGN.md §6.4: a missing file must not
 * leave the other mode's bonds in the store, to be written into this mode's file). profile_down (NULL for none) runs
 * in the down job before esp_bluedroid_disable() (headphones: AVRCP + A2DP deinit); nonzero = teardown error. */
#define BT_XREC_MODE_PHONE 0u
#define BT_XREC_MODE_HP    1u
void     bt_xrec_plat_set_mode(uint8_t mode, int (*profile_down)(void));
/* 1 when the last posted job finished (*result: 0 = OK). */
int      bt_xrec_plat_job_done(int *result);
/* Disarm the hard-stop deadline; returns 1 if it had already latched (bt_stop + IRQ 26 masked). */
int      bt_xrec_plat_deadline_disarm(void);
/* Heap-free hard path: mask IRQ 26 / bt_stop, silence RW-BT, finalise the recording, fault record 0xB1 + reason,
 * reboot. Does not return on target. Never taken while a recording runs (F8): the sessions park instead. */
void     bt_xrec_plat_hard_path(uint32_t reason);
/* F8 (docs/BT_SYNC_SPEED_AND_RECORDING.md 2.5): a BT fault must not end a recording. rec_active: nonzero while a
 * recording is being written. park: the fault path for that case: no graceful teardown, no recovery ladder, no reboot:
 * bt_stop + IRQ 26 masked, RW-BT silenced without the ROM, the task supervisor and the bt_ctrl hang guard disarmed;
 * the recording continues and BT stays silent; no Bluedroid API may be called afterwards. Idempotent (the first
 * reason is kept). park_poll: 0 when not parked; while parked, 1 as long as the recording runs, then the deferred
 * hard path (fault record 0xB1 + reboot; on the host it returns 0 and counts as a hard path). */
int      bt_xrec_plat_rec_active(void);
void     bt_xrec_plat_park(uint32_t reason);
int      bt_xrec_plat_park_poll(void);
void     bt_xrec_plat_service(void);         /* keep the recorder serviced while waiting (recorder_poll) */
void     bt_xrec_plat_wait(void);            /* sleep about one tick */
/* Bond-store flush when requested (deferred while recording, F6); returns a platform fault (0 = none): 1 bt_ctrl hung /
 * over its CPU share, 2 bt_ctrl fault, 3 HCI command timeout, 4 a Bluedroid task stalled (bt_task_sup.h: ping
 * unanswered for 8 s), 5 an assert() in a BT task while recording (BT already parked: the session parks too). */
uint32_t bt_xrec_plat_poll(void);
void     bt_xrec_plat_session_end(void);     /* final bond-store flush; release the volume */
void     bt_xrec_plat_reboot(uint8_t mode);  /* 1 reboot, 2 SD OTA, 3 USB DFU (legacy F:2138-2147) */
uint32_t bt_xrec_plat_irq_count(void);
/* App task: the next controller link note (bt_ctrl_note_t: connection request / complete, disconnection complete,
 * the 0406 answer, a refused scan enable, the 0419 answer, a remote name complete) formatted as one log line into
 * line[cap]; *info gets its status, reason byte (for a refused scan enable: the refused scan value) and handle.
 * Returns its kind (BT_XREC_NOTE_*), 0 when there is none. */
#define BT_XREC_NOTE_CONN         1u        /* == BT_CTRL_NOTE_CONN */
#define BT_XREC_NOTE_DISC         2u        /* == BT_CTRL_NOTE_DISC */
#define BT_XREC_NOTE_CONN_REQ     3u        /* == BT_CTRL_NOTE_CONN_REQ */
#define BT_XREC_NOTE_SCAN_REFUSED 4u        /* == BT_CTRL_NOTE_SCAN_REFUSED */
#define BT_XREC_NOTE_DISC_CMD     5u        /* == BT_CTRL_NOTE_DISC_CMD */
#define BT_XREC_NOTE_NAME         7u        /* == BT_CTRL_NOTE_NAME */
#define BT_XREC_NOTE_LINE         64u       /* == BT_CTRL_NOTE_LINE */
typedef struct {
    uint8_t  status, arg;
    uint16_t handle;
} bt_xrec_note_t;
int      bt_xrec_plat_ctrl_note(char *line, uint16_t cap, bt_xrec_note_t *info);
/* App task: the newest remote name a successful Remote Name Request Complete carried (bt_ctrl_peer_name), whoever
 * asked for it. 1 with bda and a NUL-terminated name, 0 when none. */
int      bt_xrec_plat_ctrl_name(uint8_t bda[6], char *name, uint16_t cap);

#endif /* BT_XREC_BLUEDROID_H */
