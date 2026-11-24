/* bt_ctrl: controller owner between Bluedroid's HCI driver boundary and the RW-BT mask ROM
 * (plan docs/BLUETOOTH_BLUEDROID_PLAN.md v18 M1; transport contract docs/BT_HCI_DRIVER_CONTRACT.md; ROM interface
 * docs/BT_ROM_INTERFACE.md; opcode policy docs/BT_CTRL_OPCODE_TABLE.md).
 *
 * Contexts (plan §3):
 *   hciT (7)     bt_ctrl_check_send_available() / bt_ctrl_send()   -- never blocks, never calls the host callbacks
 *   bt_ctrl (8)  bt_ctrl_run()                                      -- the ONLY caller of bt_rom_if_t entry points
 *   bt_dlv (6)   bt_ctrl_dlv_run()                                  -- the ONLY caller of the host callbacks
 *   IRQ 26       bt_rom_upcalls_t.on_irq -> platform wake_ctrl_from_isr
 *   app/recover  bt_ctrl_open() / bt_ctrl_shutdown_begin()          -- lifecycle; ROM work is done by bt_ctrl_run()
 *
 * Pure logic: all OS access goes through bt_ctrl_platform_t and all ROM access through bt_rom_if_t, so host tests drive
 * every interleaving deterministically against tests/fake_rom.c. Static memory only.
 * SPDX-License-Identifier: MIT */
#ifndef BT_CTRL_H
#define BT_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "bt_ctrl_cmds.h"
#include "bt_rom_if.h"

/* ---- capacities (plan §4 table: ACL RX 4 x (1021+5) + event RX 8 x 260 + unified TX 3 x 1026) ---- */
#define BT_CTRL_TX_SLOTS          3u
#define BT_CTRL_TX_SLOT_BYTES     (1u + 4u + BT_CTRL_H2C_ACL_LEN)       /* 1026: any H4 packet hciT can send */
#define BT_CTRL_EVT_SLOTS         8u
#define BT_CTRL_EVT_SLOT_BYTES    260u
#define BT_CTRL_EVT_RESERVED      2u     /* slots general events may not use: CC/CS (reserved per command) and NOCP */
#define BT_CTRL_EVT_DEFER         4u     /* ROM event messages kept (KEEP) while the general event area is full */
#define BT_CTRL_ACL_RX_SLOTS      BT_CTRL_C2H_ACL_CREDITS
#define BT_CTRL_ACL_RX_SLOT_BYTES (BT_CTRL_C2H_ACL_LEN + 5u)            /* 1026 */
#define BT_CTRL_HELD_ACL          BT_ROM_EM_RX_BUFS                     /* ACL_RX messages kept while credit-blocked */
#define BT_CTRL_ROM_CMD_CREDITS   1u     /* ordinary commands in flight at the ROM (opcode table §2 rule 1) */
#define BT_CTRL_MAX_LINKS         7u     /* ROM handles 0x80..0x86 */
#define BT_CTRL_DLV_FAULT_AFTER   8u     /* consecutive notify_host_recv failures before a BT fault */
#define BT_CTRL_BH_MAX_LOOPS      8u     /* bottom-half iterations per pass before yielding */
#define BT_CTRL_RUN_STEPS         4u     /* ke_event dispatches per rom->run() call */
#define BT_CTRL_RUN_CALLS         16u    /* rom->run() calls per pass */
#define BT_CTRL_PASS_MAX          8u     /* passes per bt_ctrl_run() before it reports "more work" */

/* ---- fault codes (plan M1 recovery ladder: posted once per session to the app task) ---- */
enum {
    BT_CTRL_FAULT_NONE = 0,
    BT_CTRL_FAULT_HOST_CONTRACT,   /* send() on a full ring, bad H4 type/framing, oversize ACL */
    BT_CTRL_FAULT_DLV,             /* BT_CTRL_DLV_FAULT_AFTER consecutive notify_host_recv failures */
    BT_CTRL_FAULT_EVT_OVERFLOW,    /* event ring and defer list both full */
    BT_CTRL_FAULT_ROM,             /* bt_rom_upcalls_t.on_fault */
    BT_CTRL_FAULT_RESET,           /* controller start/stop reported failure */
    BT_CTRL_FAULT_INVARIANT,       /* class-M FWD nonzero status, EM TX alloc failure with credit, lock depth, table */
};

/* ---- lifecycle results ---- */
#define BT_CTRL_STOP_PENDING  (-1)
#define BT_CTRL_STOP_OK       0
#define BT_CTRL_STOP_ERR      1     /* rom->stop() reported that the soft reset did not complete */
#define BT_CTRL_STOP_PARKED   2     /* bt_stop latched: no further ROM call was made */

#define BT_CTRL_DLV_IDLE   0
#define BT_CTRL_DLV_RETRY  1        /* notify_host_recv failed: entry kept; retry after a 1-tick backoff */

typedef struct {
    void (*enter)(void);                /* short critical section guarding ring indices (taskENTER_CRITICAL) */
    void (*exit)(void);
    void (*wake_ctrl)(void);            /* task context: xTaskNotifyGive(bt_ctrl); never blocks */
    void (*wake_ctrl_from_isr)(void);   /* IRQ-26 top half: vTaskNotifyGiveFromISR + portYIELD_FROM_ISR */
    void (*wake_dlv)(void);             /* xTaskNotifyGive(bt_dlv); called only from bt_ctrl_run(), never from an upcall */
    bool (*stop_requested)(void);       /* bt_stop flag latched by the SysTick recovery deadline (plan M1) */
    void (*fault)(uint32_t code);       /* post a BT fault to the app task; never blocks */
} bt_ctrl_platform_t;

/* Same shape as esp_bluedroid_hci_driver_callbacks_t (api/esp_bluedroid_hci.h:22,32). */
typedef struct {
    void (*notify_host_send_available)(void);
    int (*notify_host_recv)(uint8_t *data, uint16_t len);
} bt_ctrl_host_cb_t;

typedef struct {
    /* host -> controller */
    uint32_t tx_cmds, tx_acl, tx_noack, tx_dropped_closed, tx_contract_violation, tx_malformed;
    uint32_t cmd_fwd, cmd_syn, cmd_rej, cmd_unknown, cmd_local_fail, cmd_alloc_refused, mandatory_fwd_fail;
    uint32_t fwd_orphaned;          /* LC-destined command answered locally (0x02) after its link went, no ROM reply */
    uint16_t unknown_opcode_last;
    uint32_t acl_to_rom, acl_tx_dropped, acl_tx_em_fail, acl_tx_credit_blocked;
    /* controller -> host */
    uint32_t rom_msgs, rom_unknown_ids, rom_short, evt_queued, evt_replies, evt_unsolicited_reply, evt_deferred,
             evt_overflow, nocp_from_rom, nocp_excess, nocp_events, acl_rx_msgs, acl_rx_frags, acl_rx_bad,
             acl_rx_credit_blocked, acl_rx_pb_fixed, acl_rx_overflow;
    uint32_t evt_dropped_reset;     /* deferred ROM events that did not fit the ring when HCI Reset restarted the ROM */
    uint32_t evt_dropped_stop;      /* deferred ROM events released by teardown */
    uint32_t acl_rx_dropped_restart;  /* held ACL_RX released by HCI Reset or teardown */
    uint32_t c2h_returned, c2h_invalid, c2h_excess, c2h_malformed, c2h_config_mismatch;
    /* delivery */
    uint32_t dlv_evt, dlv_acl, dlv_fail, dlv_stale, dlv_send_avail;
    /* IRQ 26 bottom half: per-bit services (top-half per-bit occurrences are in bt_rom_stats_t.irq_count) */
    uint32_t irq_wakes, bh_loops, bh_loops_max, bh_budget_exhausted, bh_serviced[32], bh_unhandled[32];
    /* lifecycle / invariants */
    uint32_t resets, reset_failures, rf_cal_last, rom_faults, late_upcalls, lock_violations, invariant, faults;
    uint32_t stop_faults;           /* ROM faults raised on the teardown path (reported as BT_CTRL_STOP_ERR) */
    uint32_t first_fault, parked;
} bt_ctrl_stats_t;

typedef struct {
    uint8_t  state, admission_open, dlv_open, rom_running, parked, c2h_fc, rom_cmd_credits, fwd_pending;
    uint32_t session, tx_free, evt_used, evt_resv, evt_deferred, acl_used, held_acl;
    uint32_t h2c_inflight, c2h_outstanding, c2h_credits, c2h_len, nocp_pending;
} bt_ctrl_debug_t;

/* One-time binding. Returns the number of opcode/event table validation errors (0 required; >0 also faults). */
int bt_ctrl_init(const bt_rom_if_t *rom, const bt_ctrl_platform_t *platform, const uint8_t bd_addr[6]);

/* Lifecycle. open: new session, admission open; the ROM is started by the host's HCI Reset (plan M1 "Reset ... same
 * path on re-enable"). Returns -1 if not OFF or bt_dlv is still inside a host callback.
 * shutdown_begin (plan M1 teardown order): session bumped + deliveries closed (callbacks quiesced) -> admission
 * closed -> bt_ctrl_run() then stops the controller (status kept), masks IRQ 26 and empties the rings. */
int  bt_ctrl_open(void);
void bt_ctrl_shutdown_begin(void);
int  bt_ctrl_shutdown_status(void);     /* BT_CTRL_STOP_* */
bool bt_ctrl_dlv_busy(void);            /* bt_dlv is inside a host callback (deinit must wait) */

/* HCI driver ops (hciT). */
bool bt_ctrl_check_send_available(void);
void bt_ctrl_send(uint8_t *data, uint16_t len);
int  bt_ctrl_register_host_callback(const bt_ctrl_host_cb_t *cb);   /* 0 = ESP_OK */

/* bt_ctrl task body: call after every wake. Returns nonzero if work remains (the task should run again at once). */
uint32_t bt_ctrl_run(void);

/* bt_dlv task body: deliver pending send-available, then events and ACL in arrival order (one sequence across both
 * rings; ACL enters its ring only with a host credit, so events are never held behind credit-gated ACL). */
int bt_ctrl_dlv_run(void);

void bt_ctrl_get_stats(bt_ctrl_stats_t *out);
void bt_ctrl_get_debug(bt_ctrl_debug_t *out);

#endif /* BT_CTRL_H */
