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
#define BT_CTRL_FWD_DEADLINE_MS   7000u  /* class-O forwarded command answered locally (0x22) after this long without a
                                            ROM reply: just under Bluedroid's 8 s hci_layer.c command timeout */

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

/* ---- fault sites: where the session's first fault was raised, with two key values (bt_ctrl_stats_t.fault_site,
 * fault_a, fault_b). Logged with the "BT bt_ctrl fault" line so one silicon run pinpoints the raise. ---- */
enum {
    BT_CTRL_SITE_NONE = 0,
    BT_CTRL_SITE_NOCP_TABLE,       /*  1 INVARIANT: NOCP pending table full            a = handle, b = count */
    BT_CTRL_SITE_ACL_RX_HELD,      /*  2 INVARIANT: ACL_RX with every hold slot used   a = handle, b = length */
    BT_CTRL_SITE_LOCK_UPCALL,      /*  3 INVARIANT: TASK_HCI upcall, ROM lock held     a = ROM msg id, b = depth */
    BT_CTRL_SITE_CLASS_M_STATUS,   /*  4 INVARIANT: class-M command, nonzero ROM reply a = opcode, b = status */
    BT_CTRL_SITE_CLASS_M_LOCAL,    /*  5 INVARIANT: class-M command failed locally     a = opcode, b = status */
    BT_CTRL_SITE_EM_TX_ALLOC,      /*  6 INVARIANT: EM TX alloc failed with a credit   a = handle, b = in flight */
    BT_CTRL_SITE_INFLIGHT_TABLE,   /*  7 INVARIANT: in-flight table full               a = handle, b = in flight */
    BT_CTRL_SITE_C2H_TABLE,        /*  8 INVARIANT: C2H outstanding table full         a = handle, b = outstanding */
    BT_CTRL_SITE_LOCK_RUN,         /*  9 INVARIANT: ROM lock held at the end of a run  a = 0, b = depth */
    BT_CTRL_SITE_INIT_TABLE,       /* 10 INVARIANT: opcode/event table errors          a = error count */
    BT_CTRL_SITE_EVT_OVERFLOW,     /* 11 EVT_OVERFLOW: event ring + defer list full    a = ROM msg id, b = deferred */
    BT_CTRL_SITE_ROM_FAULT,        /* 12 ROM: on_fault upcall                          a = ROM fault code */
    BT_CTRL_SITE_RESET,            /* 13 RESET: controller stop/start failed           a = HCI status */
    BT_CTRL_SITE_DLV,              /* 14 DLV: notify_host_recv kept failing            a = consecutive failures */
    BT_CTRL_SITE_HOST_MALFORMED,   /* 15 HOST_CONTRACT: bad H4 packet from the host    a = length, b = H4 type */
    BT_CTRL_SITE_HOST_FULL,        /* 16 HOST_CONTRACT: send() on a full TX ring       a = length, b = H4 type */
};

/* ---- link notes: one record per link-level controller event, for the session log (bt_ctrl_note_pop) ---- */
#define BT_CTRL_NOTES 16u          /* a pairing adds up to 4 name notes to the link notes (session 6 review) */
enum {
    BT_CTRL_NOTE_CONN = 1,         /* Connection Complete from the ROM: handle, status */
    BT_CTRL_NOTE_DISC,             /* Disconnection Complete from the ROM: handle, status, reason */
    BT_CTRL_NOTE_CONN_REQ,         /* Connection Request from the ROM (a peer paged us) */
    BT_CTRL_NOTE_SCAN_REFUSED,     /* 0C1A Write Scan Enable refused by the ROM: status, reason = scan byte */
    BT_CTRL_NOTE_DISC_CMD,         /* the host's 0406 Disconnect answered: handle, status; local = not by the ROM */
    BT_CTRL_NOTE_NAME_CMD,         /* the host's 0419 Remote Name Request answered (CS): status; local = not by the ROM */
    BT_CTRL_NOTE_NAME,             /* Remote Name Request Complete from the ROM: status, reason = name length (<= 248) */
};

/* Newest remote name a successful Remote Name Request Complete carried (any request: BTM's security name request at
 * link set-up, BTA's, the session's), cut to BT_CTRL_PEER_NAME_MAX - 1 bytes. Silicon session 6: the first one is
 * BTM's own and only BTM sees its result; this lets the session show the name when it arrives late. */
#define BT_CTRL_PEER_NAME_MAX 32u

typedef struct {
    uint8_t  kind, status, reason, local;
    uint16_t handle;
} bt_ctrl_note_t;

#define BT_CTRL_NOTE_LINE 64u      /* bt_ctrl_note_format() buffer size that always fits */

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
    uint32_t fwd_deadline;          /* class-O command answered locally (0x22) at BT_CTRL_FWD_DEADLINE_MS */
    uint32_t fwd_late_reply;        /* ROM reply that arrived after its command was answered at the deadline (consumed) */
    uint32_t fwd_late_held;         /* a new command with the late opcode kept in the TX ring until the late reply came */
    uint32_t fwd_late_expired;      /* ...or until BT_CTRL_FWD_DEADLINE_MS passed again (late reply given up) */
    uint32_t fwd_late_dropped;      /* late reply given up because its LC link disconnected (the ROM dropped it) */
    uint32_t cmd_dead_handle;       /* LC-destined command for a handle already reported disconnected (answered 0x02) */
    uint32_t acl_tx_dead_handle;    /* host ACL for a handle already reported disconnected (dropped; also acl_tx_dropped) */
    uint16_t unknown_opcode_last;
    uint32_t acl_to_rom, acl_tx_dropped, acl_tx_em_fail, acl_tx_credit_blocked;
    /* controller -> host */
    uint32_t rom_msgs, rom_unknown_ids, rom_short, evt_queued, evt_replies, evt_unsolicited_reply, evt_deferred,
             evt_overflow, nocp_from_rom, nocp_excess, nocp_events, acl_rx_msgs, acl_rx_frags, acl_rx_bad,
             acl_rx_credit_blocked, acl_rx_pb_fixed, acl_rx_overflow;
    uint32_t evt_dropped_reset;     /* deferred ROM events that did not fit the ring when HCI Reset restarted the ROM */
    uint32_t evt_dropped_stop;      /* deferred ROM events released by teardown */
    uint32_t acl_rx_dropped_restart;  /* held ACL_RX released by HCI Reset or teardown */
    uint32_t acl_rx_dropped_disc;     /* inbound ACL_RX dropped for a disconnected or reused handle */
    uint32_t c2h_returned, c2h_invalid, c2h_excess, c2h_malformed, c2h_config_mismatch;
    /* delivery */
    uint32_t dlv_evt, dlv_acl, dlv_fail, dlv_stale, dlv_send_avail;
    /* IRQ 26 bottom half: per-bit services (top-half per-bit occurrences are in bt_rom_stats_t.irq_count) */
    uint32_t irq_wakes, bh_loops, bh_loops_max, bh_budget_exhausted, bh_serviced[32], bh_unhandled[32];
    /* lifecycle / invariants */
    uint32_t resets, reset_failures, rf_cal_last, rom_faults, late_upcalls, lock_violations, invariant, faults;
    uint32_t stop_faults;           /* ROM faults raised on the teardown path (reported as BT_CTRL_STOP_ERR) */
    uint32_t first_fault, parked;
    uint32_t fault_site, fault_a, fault_b;   /* BT_CTRL_SITE_* and key values of first_fault (this session) */
    uint32_t scan_refused;          /* 0C1A answered 0x0C by the ROM (> 1 ACL link): passed through, no fault */
    uint32_t notes_lost;            /* link notes dropped because nobody drained the ring */
} bt_ctrl_stats_t;

typedef struct {
    uint8_t  state, admission_open, dlv_open, rom_running, parked, c2h_fc, rom_cmd_credits, fwd_pending;
    uint32_t session, tx_free, evt_used, evt_resv, evt_deferred, acl_used, held_acl;
    uint32_t h2c_inflight, c2h_outstanding, c2h_credits, c2h_len, nocp_pending;
} bt_ctrl_debug_t;

/* One-time binding. Returns the number of opcode/event table validation errors (0 required; >0 also faults). */
int bt_ctrl_init(const bt_rom_if_t *rom, const bt_ctrl_platform_t *platform, const uint8_t bd_addr[6]);

/* Optional monotonic millisecond clock (wraps at 2^32) for the forwarded-command deadline; call after bt_ctrl_init().
 * Called only from bt_ctrl_run() (bt_ctrl task context). NULL (the default) disables the deadline. */
void bt_ctrl_set_clock(uint32_t (*now_ms)(void));

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

/* Oldest link note (any task; recorded by bt_ctrl context, BT_CTRL_NOTES deep, overflow counted in notes_lost).
 * Returns 1 if one was popped. bt_ctrl_note_format() writes one log line (no newline) and returns its length. */
int      bt_ctrl_note_pop(bt_ctrl_note_t *out);
/* The newest remote name (see BT_CTRL_PEER_NAME_MAX): 1 with bda (Bluedroid BD_ADDR / esp_bd_addr_t order: most
 * significant byte first, i.e. the wire bytes reversed) and a NUL-terminated name, 0 when none this session.
 * Any task. */
int      bt_ctrl_peer_name(uint8_t bda[6], char *name, uint16_t cap);
uint16_t bt_ctrl_note_format(const bt_ctrl_note_t *n, char *buf, uint16_t cap);

/* Session log lines (no newline; return the length; truncated within cap). Fault: "BT bt_ctrl fault, first code N
 * site S a=0x........ b=0x........", this session's first fault with its site and values read together (all 0 when
 * the session has not faulted). Stats: the counters that tell a ROM event bt_ctrl dropped (unknown id, short) or
 * answered late from one the ROM never sent: "BT ctrl stats unk=.. short=.. late=.. unsol=.. fwd_to=.. late_reply=..
 * late_drop=.. scan_refused=.. notes_lost=..". */
#define BT_CTRL_FAULT_LINE 80u
#define BT_CTRL_STATS_LINE 192u
uint16_t bt_ctrl_fault_format(char *buf, uint16_t cap);
uint16_t bt_ctrl_stats_format(char *buf, uint16_t cap);

#endif /* BT_CTRL_H */
