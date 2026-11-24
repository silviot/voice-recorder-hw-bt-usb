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
#define BT_XREC_TX_RING        2048u   /* bytes accepted from xrec_proto, not yet handed to esp_spp_write() */
#define BT_XREC_SPP_TX_BUF     2048u   /* esp_spp_cfg_t.tx_buffer_size: BTC's per-connection ring */
#define BT_XREC_INFLIGHT_MAX   BT_XREC_SPP_TX_BUF  /* written but not yet reported by ESP_SPP_WRITE_EVT */
#define BT_XREC_WRITE_MAX      990u    /* one esp_spp_write() (ESP_SPP_MAX_MTU) */
#define BT_XREC_EVQ_SLOTS      32u     /* GAP/SPP event records (DATA/WRITE coalesce, so this only holds control events) */
#define BT_XREC_SPP_SCN        1u      /* RFCOMM channel 1: tools/bt_sync.py connects to it directly (no SDP) */
#define BT_XREC_SPP_NAME       "Cassini"  /* legacy SDP ServiceName (src/bt_spp.c:352) */

/* ---- deadlines (ms) ---- */
#define BT_XREC_SHUTDOWN_DEADLINE_MS 2000u   /* plan M1 recovery ladder: one absolute 2 s deadline */
#define BT_XREC_UP_DEADLINE_MS       12000u  /* bt_ctrl open + HCI Reset (RF calibration) + Bluedroid enable */
#define BT_XREC_CONFIG_DEADLINE_MS   5000u   /* SPP INIT + START events after enable */
#define BT_XREC_REBOOT_DELAY_MS      250u    /* legacy F:1827: deferred reboot / OTA / DFU */
#define BT_XREC_REBOOT_DRAIN_MAX_MS  2000u   /* longest wait for the final reply to leave before rebooting anyway */
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
    BT_XREC_FAULT_PLATFORM = 6,       /* bt_ctrl fault / hang, HCI command timeout (bt_xrec_plat_poll) */
    BT_XREC_FAULT_DOWN_DEADLINE = 7,  /* graceful teardown did not finish within BT_XREC_SHUTDOWN_DEADLINE_MS */
    BT_XREC_FAULT_DOWN_FAIL = 8,      /* graceful teardown reported an error */
    BT_XREC_FAULT_DOWN_LATCHED = 9,   /* teardown finished, but the deadline had already latched bt_stop */
};

/* Session states (bt_xrec_debug_t.state). */
enum {
    BT_XREC_ST_OFF = 0,
    BT_XREC_ST_UP,        /* bring-up job running */
    BT_XREC_ST_CONFIG,    /* Bluedroid enabled; GAP set, waiting for SPP INIT/START */
    BT_XREC_ST_READY,     /* SPP server listening on channel 1 */
    BT_XREC_ST_FAULTED,   /* graceful recovery done after a fault: off until the next bt_xrec_start() */
    BT_XREC_ST_DEAD,      /* hard path taken (host tests only: on target it reboots) */
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
    uint32_t write_evts, write_evt_bytes, cong_evts, evq_lost, stale_events;
    uint32_t ssp_accepted, ssp_rejected, pin_rejected, passkey_rejected, bonds_removed, conns_rejected;
    uint32_t conns_authorized;   /* SPP connections marked authorized for XREC */
    uint8_t  ssp_prompt;         /* a numeric comparison waits for the local user */
    uint32_t ssp_user_accepted, ssp_user_rejected, ssp_timeouts, ssp_withdrawn, ssp_bench_autoconfirmed;
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
} bt_xrec_debug_t;
void bt_xrec_get_debug(bt_xrec_debug_t *out);

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
/* The local user's answer: accept != 0 accepts. 0 = applied, -1 no prompt pending, -2 not armed yet (ignored),
 * -3 no prompt pending but one ended less than BT_XREC_SSP_AFTER_MS ago (swallowed: the key was meant for it).
 * The caller treats every value but -1 as consumed. Accepting re-checks the M2.3 gate first (rejects if it fails). */
int  bt_xrec_ssp_answer(int accept);
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
const xrec_ops_t *bt_xrec_plat_device_ops(void);       /* stat, rtc, recorder, log, audio cfg, exp (legacy F:1733-1966) */
void     bt_xrec_plat_load_bound_key(xrec_ctx_t *ctx);  /* SD:/cassini.key (legacy F:1968, same CSK5 format) */
void     bt_xrec_plat_save_bound_key(const uint8_t key[32]);
/* Bring-up, app part (bt_port_init, bond store loaded from SD before esp_bluedroid_init), then posts the "up" job to
 * the worker (bt_ctrl open + HCI driver attach + esp_bluedroid_init + enable). 0 = job posted. */
int      bt_xrec_plat_stack_up_begin(exfat_vol_t *vol, int sd_mounted, const uint8_t bd_addr[6]);
/* Posts the graceful "down" job (esp_spp_deinit, disable, bt_ctrl stop, deinit) and arms the hard-stop deadline. */
void     bt_xrec_plat_stack_down_begin(void);
/* 1 when the last posted job finished (*result: 0 = OK). */
int      bt_xrec_plat_job_done(int *result);
/* Disarm the hard-stop deadline; returns 1 if it had already latched (bt_stop + IRQ 26 masked). */
int      bt_xrec_plat_deadline_disarm(void);
/* Heap-free hard path: mask IRQ 26 / bt_stop, silence RW-BT, finalise the recording, fault record 0xB1 + reason,
 * reboot. Does not return on target. */
void     bt_xrec_plat_hard_path(uint32_t reason);
void     bt_xrec_plat_service(void);         /* keep the recorder serviced while waiting (recorder_poll) */
void     bt_xrec_plat_wait(void);            /* sleep about one tick */
uint32_t bt_xrec_plat_poll(void);            /* bond-store flush when requested; returns a fault reason (0 = none) */
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
