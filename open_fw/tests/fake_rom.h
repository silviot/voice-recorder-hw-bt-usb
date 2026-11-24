/* fake_rom: host model of the RW-BT mask ROM behind bt_rom_if_t, for bt_ctrl host tests.
 * Implements the minimum contract of docs/BT_ROM_INTERFACE.md §10 "Fake ROM contract":
 *   - a FIFO of kernel messages; run() delivers one message per dispatch to TASK_HCI (bt_ctrl's on_hci_msg) or to
 *     fake LM/LC handlers keyed by id, which answer from a configurable reply table to the command's src_id;
 *   - aligned ROM reply layouts (status first, pad bytes where the ROM has them);
 *   - EM: TX pool 9, RX pool 8 (host memory); ACL TX held "on air" until acked, then E462 NOCP;
 *   - a g_bt_heap byte budget: a bt_ctrl allocation over budget returns NULL (models the real impl's pre-check), a
 *     ROM-internal one calls on_fault;
 *   - contract checks recorded as errors: msg_free on a pointer that is not an allocated/kept param, double EM free,
 *     message to an unregistered task (the ROM HardFaults), CONSUMED ACL_RX whose EM buffer was not freed, RF
 *     calibration missing before start(), EM access outside a buffer;
 *   - a per-call hook to latch bt_stop at the k-th ROM entry (plan M1 "expiry between the bt_stop check and the ROM
 *     call") and count any later entry.
 * SPDX-License-Identifier: MIT */
#ifndef FAKE_ROM_H
#define FAKE_ROM_H

#include <stdbool.h>
#include <stdint.h>

#include "bt_rom_if.h"

extern const bt_rom_if_t fake_rom_if;

enum {
    FR_RF_CAL = 1, FR_START, FR_STOP, FR_IRQ_EN, FR_IRQ_TAKE, FR_IRQ_SERVICE, FR_EVENTS, FR_RUN, FR_MSG_ALLOC,
    FR_MSG_SEND, FR_MSG_FREE, FR_CONHDL_TASK, FR_CONHDL_TYPE, FR_EM_TX_ALLOC, FR_EM_WRITE, FR_EM_READ, FR_EM_RX_FREE,
    FR_N_CALLS
};

#define FAKE_ROM_LOG_MAX 512u

typedef struct {
    uint16_t id, dst, len;
    uint8_t data[256];
} fake_cmd_rec_t;

typedef struct {
    uint16_t handle, len;
    uint8_t pb_bc;
    uint8_t data[1024];
} fake_acl_rec_t;

typedef struct {
    /* configuration (tests write) */
    uint32_t heap_budget;           /* bytes of g_bt_heap (default 8192) */
    int start_result, stop_result;
    bool auto_ack_acl;              /* complete ACL TX immediately with NOCP */
    bool hold_replies;              /* LM/LC handlers swallow commands (reply withheld) */
    uint32_t lock_depth;
    bool *stop_flag;                /* bt_stop flag the hook latches */
    uint32_t stop_at_call;          /* 1-based ROM entry index at which bt_stop latches (0 = off) */
    uint32_t irq_rearm;             /* irq_service() re-raises the serviced bits this many more times */
    bool lc_swallow;                /* LC handlers drop commands without any reply (LC instance freed under them) */
    bool em_tx_fail;                /* em_tx_alloc() returns NULL */
    int conhdl_type_override;       /* >= 0: conhdl_type() returns this for every handle (default -1: 1 for ACL) */
    bool fault_on_stop;             /* stop() raises on_fault (a fault on the teardown path) */

    /* observations */
    uint32_t errors;
    char first_error[160];
    uint32_t calls, calls_by[FR_N_CALLS];
    uint32_t stop_fired_at, calls_after_stop, stop_call_completed;
    uint8_t call_log[FAKE_ROM_LOG_MAX];
    uint32_t call_log_n;
    uint32_t starts, stops, irq_enabled, irq_count[32];
    uint32_t kept_at_stop, em_rx_held_at_stop;   /* bt_ctrl-owned objects still alive when stop() ran */
    uint32_t heap_used, heap_peak, alloc_refused, rom_alloc_faults;
    fake_cmd_rec_t cmds[64];
    uint32_t n_cmds;
    fake_acl_rec_t *acl;            /* ACL packets received on "air" (allocated, FAKE_ROM_LOG_MAX entries) */
    uint32_t n_acl;
    uint32_t acl_in_air;
    uint32_t scan_refused;          /* 0C1A refused by the ROM's more-than-one-link rule */
    uint8_t  scan_enable;           /* the scan value of the last accepted 0C1A (0 after reset): 1 inquiry, 2 page */
    uint32_t scan_writes;           /* 0C1A writes received (accepted or refused) */

    /* ---- headphones path (docs/BT_A2DP_DESIGN.md §8.2 "Fake ROM additions"; ROM facts: docs/BT_ROM_HCI_MAP.md §2,
     * §3). The fake TASK_LM state: 0 idle, 1/2 the undocumented states in which the ROM SAVES 0401/0405/0419 (the
     * message waits on the saved queue and runs again at the next LM state change), 3 paging, 4 inquiry, 7 inquiry
     * cancel pending. Tests may force 1 or 2 with fake_rom_set_lm_state(). ---- */
    uint8_t  lm_state;
    bool     lm_state2_err;         /* LM state 2 with env +0x4b set: 0401 answered 0x3A instead of SAVED */
    bool     inq_auto_complete;     /* Inquiry Complete right after the results (else at fake_rom_inquiry_end()) */
    bool     inq_cancel_hold;       /* 0402's deferred CC waits for the LD stop (fake_rom_inquiry_end()) */
    bool     page_hold;             /* 0405 keeps paging until fake_rom_page_answer() / _page_timeout() / 0408 */
    uint8_t  inq_mode;              /* accepted 0C45 value: 0 -> E0D1, 1 -> E0D6, 2 -> E0D7 (§3) */
    /* ACL air model with pacing (for the A2DP media path): with auto_ack_acl false and air_rate_Bps != 0,
     * fake_rom_air_tick(ms) completes in-air packets in order as the byte budget allows, one NOCP per tick; with
     * air_stall the baseband stops acknowledging (NOCP stops, no detach: the "ghost link" of BT_HW_RESULTS session 4-6). */
    uint32_t air_rate_Bps;
    bool     air_stall;
    uint32_t air_budget;            /* bytes the next tick may complete (carried over, capped at one second's worth) */
    /* observations */
    uint32_t inquiries, inq_results_sent, inq_completes, inq_cancels, inq_refused;
    uint32_t pages, pages_ok, page_timeouts, page_cancels, page_refused, create_cancels;
    uint32_t saved, saved_reruns;   /* messages SAVED by the fake LM / re-dispatched after a state change */
    uint32_t air_ticks, air_acked, air_acked_bytes, air_nocp_events;
} fake_rom_t;

extern fake_rom_t fake_rom;

void fake_rom_reset(void);          /* full reset incl. configuration defaults; frees everything */
void fake_rom_shutdown(void);       /* free host allocations (end of test binary) */

/* Reply table: request id -> reply id with payload; `echo_addr` copies 6 bytes of the request after status. */
void fake_rom_set_reply(uint16_t req_id, uint16_t rsp_id, uint8_t status, uint8_t ret_len, bool echo_addr);
void fake_rom_set_reply_status(uint16_t req_id, uint8_t status);
/* Release withheld replies (hold_replies=false first). */
void fake_rom_release_held(void);

void fake_rom_link(uint32_t link, bool up);           /* handle 0x80 + link */

/* Controller -> host traffic generated "by the air" (no bt_ctrl ROM call is counted). */
int  fake_rom_inject(uint16_t id, const uint8_t *param, uint16_t len);        /* message to TASK_HCI */
int  fake_rom_inject_acl(uint16_t handle, uint8_t pb_bc, const uint8_t *data, uint16_t len); /* E47D; -1 = no EM */
void fake_rom_raise_irq(uint32_t bits);
uint32_t fake_rom_ack_acl(uint32_t n);                /* complete n in-air ACL packets -> one E462 */
/* Deliver a message through upcalls saved from an earlier start() (late callback after a restart). */
int  fake_rom_deliver_stale(uint16_t id, const uint8_t *param, uint16_t len);

/* ---- scripted remote peer (phone/laptop) for the phone path ----
 * When enabled, the fake LM/LC answer the phone-path commands for this one peer with the ROM's own message ids and
 * aligned layouts (docs/BT_ROM_HCI_MAP.md §2 and §9) instead of the status-only reply table: initial CS/CC first,
 * then the terminal event, from LM (0x38) or the link's LC instance like the ROM. Covered: 0409/040A (+ E456/E0D2),
 * 041B/041C/041D/041F (+ E45D/E46D/E45E/E469), 0419/041A (+ E47C/E0D3), 0411 (+ E465), 040B/040C, 042B/0434,
 * 042C/042D (+ E473/E472/E474/E477/E466/E459), 0413 (+ E45A), 0406 (+ E458). ACL is the existing air model
 * (host -> peer: fake_rom.acl log + NOCP; peer -> host: fake_peer_send_acl). */
#define FAKE_PEER_ORIGIN_NONE 0u
#define FAKE_PEER_ORIGIN_PEER 1u    /* the remote authenticates us (we are the SSP responder) */
#define FAKE_PEER_ORIGIN_HOST 2u    /* the host sent 0411 (we are the SSP initiator) */

typedef struct {
    uint8_t addr[6];                /* wire order (LSB first), as in every ROM message */
    uint8_t cod[3];
    uint8_t io_cap, oob, auth_req;  /* the peer's IO Capability Response (1 = DisplayYesNo) */
    uint32_t numeric;               /* numeric comparison value in User Confirmation Request (E474) */
    bool user_confirms;             /* the peer's user accepts the comparison */
    bool auto_encrypt;              /* after a peer-initiated authentication succeeds, the peer starts encryption */
    bool host_feat_notify;          /* E47C (0x3D) precedes E0D3 on a remote name request */
    uint8_t key_type;               /* Link Key Notification key type (0x05: authenticated combination P-192) */
    uint8_t features[8], features_p1[8], max_page;
    uint8_t lmp_ver;
    uint16_t manuf, subver, clk_off;
    char name[32];
    /* headphones path: the peer as a discoverable headset that we page (docs/BT_A2DP_DESIGN.md §8.2) */
    bool discoverable;              /* answers inquiries (CoD `cod`, page scan repetition mode `psrm`, `rssi`) */
    bool ignores_pages;             /* out of range / not connectable: a 0405 to it ends in page timeout 0x04 */
    uint8_t psrm;
    int8_t rssi;
    bool role_switch_on_connect;    /* right after our page the headset takes the master role: E461 (we: slave) */
    uint8_t max_slots_on_connect;   /* E468 Max Slots Change after the Connection Complete (0 = none) */
} fake_peer_cfg_t;

typedef struct {
    bool enabled, connected, encrypted, authenticated;
    uint32_t link;
    uint16_t handle;
    uint8_t origin;                 /* FAKE_PEER_ORIGIN_* of the authentication in progress */
    bool has_key;                   /* the peer holds a link key for us (pairing done, or fake_peer_set_key) */
    uint8_t key[16];
    /* what bt_ctrl delivered to the ROM on behalf of the host */
    uint8_t host_accept_role, host_io_cap, host_oob, host_auth_req;
    uint8_t host_key[16];
    uint32_t host_io_replies, host_io_neg, host_confirms, host_rejects, host_key_replies, host_key_neg;
    /* outcome counters */
    uint32_t conn_count, auth_ok, auth_fail, pairings, name_requests, events_sent;
    uint8_t last_disc_reason;
    /* Runtime knob (set under bth_rom_lock): 0406 gets its CS 0 but the LMP detach never completes (no E458), like a
     * phone that walked out of range or froze (a real controller waits for the supervision timeout, up to 20 s). */
    bool defer_detach;
    /* Runtime knob (under bth_rom_lock): the next name_fail_n remote name requests fail (E0D3 status 0x22). */
    uint32_t name_fail_n;
    /* Runtime knob (under bth_rom_lock): the next remote name request is accepted (CS 0) but its Remote Name Request
     * Complete is held until fake_peer_name_release(); while one is held the ROM refuses another with CS 0x0C (RNR
     * already running, env +0x217: BT_ROM_HCI_MAP.md §2, 0419). Silicon session 6: BTM's own name request at link
     * set-up was still running when the confirmation request came, so BTA's and the session's requests failed. */
    bool name_hold;
    bool name_held;
    uint32_t name_refused;          /* requests refused 0x0C while one was held */
    /* Ghost page (fake_peer_page_again): the peer pages again while the ROM still holds its first link (a detach the
     * ROM never reported). The host's 040A gets its CS; the rejected link stays counted as an ACL link (LD) and no
     * Connection Complete comes until fake_peer_ghost_end() (its own detach stalls like the first: silicon bench
     * 2026-11-24, where the laptop's second page was rejected 0x0F as well). */
    bool ghost_pending, ghost_up;
    uint32_t ghost_link, ghost_rejects;
    uint8_t ghost_reject_reason;
    uint32_t detach_deferred;       /* 0406 accepted without a Disconnection Complete */
    uint32_t stop_kills;            /* the link was still up when the controller stopped (no event: the ROM is gone) */
    uint32_t tx_flushed;            /* ACL in flight when the link ended: freed, reported by NOCP before E458 */
    /* headphones path: what the host's 0405 asked for, and the link it made */
    bool we_paged;                  /* the current link came from the host's 0405 */
    uint8_t host_allow_rs, host_psrm;
    uint16_t host_pkt_type, host_clk_off;
    uint32_t host_creates;          /* 0405 that reached the fake LM (incl. refused) */
    uint32_t role_changes;
} fake_peer_t;

extern fake_peer_t fake_peer;

void fake_peer_init(const fake_peer_cfg_t *cfg);     /* NULL disables; fake_rom_reset() also disables */
void fake_peer_set_key(const uint8_t key[16]);       /* the peer already holds this key (re-connect) */
int  fake_peer_page(uint32_t link);                  /* the peer pages us: E457 Connection Request; -1 if connected */
void fake_peer_authenticate(void);                   /* the peer authenticates: E465 Link Key Request */
void fake_peer_encrypt(bool on);                     /* the peer switches encryption: E45A */
void fake_peer_disconnect(uint8_t reason);           /* the peer drops the link: E458 */
int  fake_peer_send_acl(const uint8_t *data, uint16_t len);   /* peer -> host ACL start fragment (PB 10) */
int  fake_peer_page_again(uint32_t link);            /* ghost page while connected (see ghost_pending); -1 if not */
void fake_peer_ghost_end(uint8_t status);            /* the rejected link ends: E0D2 Connection Complete(status) */
void fake_peer_name_release(void);                   /* the held name request completes (E0D3 with the name) */
void fake_peer_role_switch(uint8_t new_role);        /* the peer switches roles: E461 {0, addr, our new role} */
void fake_peer_max_slots(uint8_t slots);             /* E468 Max Slots Change on the peer's link */

/* ---- headphones path: discoverable devices other than the scripted peer (inquiry results and names only; a page
 * to one of them times out like an out-of-range device) ---- */
#define FAKE_ROM_DEVS 8u
typedef struct {
    uint8_t addr[6];                /* wire order */
    uint8_t cod[3];
    uint8_t psrm;
    uint16_t clk_off;
    int8_t rssi;
    bool answers_name;              /* a 0419 temporary page succeeds (else E0D3 0x04 page timeout) */
    char name[32];
} fake_dev_t;

int  fake_rom_add_device(const fake_dev_t *d);       /* -1 when FAKE_ROM_DEVS are configured already */
void fake_rom_set_lm_state(uint8_t st);              /* force the fake LM state (1/2: SAVED states); re-runs saved */
void fake_rom_inquiry_end(void);                     /* the LD inquiry ends: E0D0 (state 4) or the deferred 0402 CC
                                                        (state 7); LM state 0, saved messages run again */
void fake_rom_page_answer(void);                     /* a held page (page_hold) to the scripted peer succeeds */
void fake_rom_page_timeout(void);                    /* a held page fails: E0D2 0x04 */
uint32_t fake_rom_air_tick(uint32_t ms);             /* pacing model step; returns packets completed */

uint32_t fake_rom_em_rx_in_use(void);
uint32_t fake_rom_em_tx_in_use(void);
uint32_t fake_rom_msgs_owned_by_ctrl(void);           /* ALLOC (unsent) + KEPT messages */
uint32_t fake_rom_queue_len(void);

#endif /* FAKE_ROM_H */
