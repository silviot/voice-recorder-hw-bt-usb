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
#define BT_XREC_SSP_SHOW_MS          30000u  /* numeric-comparison value shown at most this long */

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
    uint8_t  spp_inited, ssp_showing, entropy_fail, reboot_mode;
    uint32_t conn_handle, tx_used, rx_used, inflight, evq_used, acl_links;
    uint16_t mtu;
    uint32_t last_fault;
    /* counters */
    uint32_t rx_frames, rx_bytes, rx_dropped, rx_overflows, tx_writes, tx_bytes, tx_nomem, tx_errors, tx_busy;
    uint32_t write_evts, write_evt_bytes, cong_evts, evq_lost, stale_events;
    uint32_t ssp_accepted, ssp_rejected, pin_rejected, passkey_rejected, bonds_removed, conns_rejected;
} bt_xrec_debug_t;
void bt_xrec_get_debug(bt_xrec_debug_t *out);

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

/* App task. */
void    *bt_xrec_plat_alloc(size_t n);       /* NULL on failure (fail soft) */
void     bt_xrec_plat_free(void *p);
int      bt_xrec_plat_start_allowed(void);   /* 0 after a BT-fault reboot (fault record 0xB1): BT stays off this boot */
void     bt_xrec_plat_identity(uint8_t bd_addr[6]);
int      bt_xrec_plat_entropy(void *out, size_t n);   /* 0 = OK; anything else fails closed (plan M2) */
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

#endif /* BT_XREC_BLUEDROID_H */
