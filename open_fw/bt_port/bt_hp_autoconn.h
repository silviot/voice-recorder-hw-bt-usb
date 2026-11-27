/* bt_hp_autoconn: step state machine for main.c's hp_auto_connect_for_play() when output is AOUT_MODE_HEADPHONES.
 * Extracted as a header-only state machine (like bt_lowbatt.h) so host tests (test_bt_hp_autoconn) exercise
 * every exit, cleanup, PAIR_AGAIN, REC-switch, and deadline transition directly.
 * SPDX-License-Identifier: MIT */
#ifndef BT_HP_AUTOCONN_H
#define BT_HP_AUTOCONN_H

#include <stdint.h>
#include "bt_hp.h"
#include "bt_xrec_bluedroid.h"   /* BT_XREC_UP_DEADLINE_MS, BT_XREC_CONFIG_DEADLINE_MS */

/* Pre-READY safety bound: 1 s past bt_hp's own bring-up deadlines (UP 12 s + CONFIG 5 s, which end in FAULTED), so a
 * slow but healthy bring-up is never cut short here. Once READY is reached, allow BT_HP_CONNECT_TO_MS + 1 s from READY
 * so bt_hp's own BT_HP_CONNECT_TO_MS fires CONNECT_FAIL first. */
#define BT_HP_AUTOCONN_START_TO_MS (BT_XREC_UP_DEADLINE_MS + BT_XREC_CONFIG_DEADLINE_MS + 1000u)
#define BT_HP_AUTOCONN_READY_TO_MS (BT_HP_CONNECT_TO_MS + 1000u)

enum {
    BT_HP_AC_WAIT               = 0,
    BT_HP_AC_READY              = 1,
    BT_HP_AC_SILENT_ABORT       = -1,
    BT_HP_AC_FAIL_NOT_CONNECTED = -2,
    BT_HP_AC_FAIL_CONNECT       = -3,
    BT_HP_AC_FAIL_MSG           = -4,
};

typedef struct {
    int  (*stream_ready)(void);
    void (*get_ui)(bt_hp_ui_t *out);
    int  (*connect_addr)(const uint8_t bda[6], int pair);
    int  (*stop)(void);
    int  (*disconnect)(void);
} bt_hp_autoconn_ops_t;

typedef struct {
    uint32_t start_deadline;
    uint32_t ready_deadline;
    uint32_t seen_seq;
    uint8_t  started_here;
    uint8_t  ready_seen;
    uint8_t  tried_connect;
    uint8_t  inherited_conn;
    uint8_t  phase;      /* 1 = Starting, 2 = Connecting */
    uint8_t  fail_msg;   /* BT_HP_MSG_* when BT_HP_AC_FAIL_MSG is returned */
} bt_hp_autoconn_t;

static inline void bt_hp_autoconn_init(bt_hp_autoconn_t *ac, int started_here, uint32_t now_ms, uint32_t msg_seq)
{
    ac->start_deadline = now_ms + BT_HP_AUTOCONN_START_TO_MS;
    ac->ready_deadline = 0u;
    ac->seen_seq       = msg_seq;
    ac->started_here   = started_here ? 1u : 0u;
    ac->ready_seen     = 0u;
    ac->tried_connect  = 0u;
    ac->inherited_conn = 0u;
    ac->phase          = 1u;
    ac->fail_msg       = BT_HP_MSG_NONE;
}

static inline void bt_hp_autoconn_stop_started(const bt_hp_autoconn_t *ac, const bt_hp_autoconn_ops_t *ops)
{
    if (ac->started_here && !ops->stream_ready()) {
        (void)ops->stop();
    }
}

static inline int bt_hp_autoconn_is_fail_msg(uint8_t msg)
{
    return msg == BT_HP_MSG_CONNECT_FAIL || msg == BT_HP_MSG_PAIR_AGAIN || msg == BT_HP_MSG_UNSUPPORTED;
}

static inline int bt_hp_autoconn_step(bt_hp_autoconn_t *ac, uint32_t now, int rec_switch, int lowbatt_stopped,
                                      int back_pressed, const bt_hp_autoconn_ops_t *ops)
{
    if (rec_switch) {
        /* Recording starts at once: no teardown (up to the 2 s shutdown deadline) in front of it. The session stays
         * up like a user-started one (design §6.9: bt_hp_suspend on REC); the BT-during-recording policy is S3/S5. */
        return BT_HP_AC_SILENT_ABORT;
    }
    if (lowbatt_stopped) {
        return BT_HP_AC_SILENT_ABORT;
    }
    if (back_pressed) {
        bt_hp_autoconn_stop_started(ac, ops);
        return BT_HP_AC_SILENT_ABORT;
    }

    bt_hp_ui_t u;
    if (ops->stream_ready()) {
        ops->get_ui(&u);
        ac->seen_seq = u.msg_seq;
        return BT_HP_AC_READY;
    }

    ops->get_ui(&u);
    if (u.state == BT_HP_ST_OFF || u.state == BT_HP_ST_FAULTED || u.state == BT_HP_ST_DEAD) {
        return BT_HP_AC_FAIL_NOT_CONNECTED;
    }

    if (u.state == BT_HP_ST_READY && !ac->ready_seen) {
        ac->ready_seen     = 1u;
        ac->ready_deadline = now + BT_HP_AUTOCONN_READY_TO_MS;
        if (!ac->started_here && u.link == BT_HP_LINK_CONNECTING) {
            ac->inherited_conn = 1u;
        }
    }

    /* Observe new session messages (e.g. rc_poll PAIR_AGAIN or CONNECT_FAIL) BEFORE issuing connect_addr.
     * If an already-in-flight background rc_poll attempt fails before we issued our own connect_addr, retry once
     * instead of immediately failing user playback. */
    if (u.msg_seq != ac->seen_seq) {
        ac->seen_seq = u.msg_seq;
        if (u.msg == BT_HP_MSG_CONNECT_FAIL && ac->inherited_conn && !ac->tried_connect &&
            u.state == BT_HP_ST_READY && u.has_last) {
            ac->inherited_conn = 0u;
            ac->ready_deadline = now + BT_HP_AUTOCONN_READY_TO_MS;
        } else if (bt_hp_autoconn_is_fail_msg(u.msg)) {
            ac->fail_msg = u.msg;
            bt_hp_autoconn_stop_started(ac, ops);
            return BT_HP_AC_FAIL_MSG;
        }
    }

    if (u.state == BT_HP_ST_READY) {
        if (!u.has_last && u.link == BT_HP_LINK_NONE) {
            bt_hp_autoconn_stop_started(ac, ops);
            return BT_HP_AC_FAIL_NOT_CONNECTED;
        }
        if (u.has_last && u.link == BT_HP_LINK_NONE && !ac->tried_connect) {
            ac->tried_connect = 1u;
            if (ops->connect_addr(u.last, 0) != 0) {
                ops->get_ui(&u);
                if (u.msg_seq != ac->seen_seq) {
                    ac->seen_seq = u.msg_seq;
                    if (bt_hp_autoconn_is_fail_msg(u.msg)) {
                        ac->fail_msg = u.msg;
                        bt_hp_autoconn_stop_started(ac, ops);
                        return BT_HP_AC_FAIL_MSG;
                    }
                }
                bt_hp_autoconn_stop_started(ac, ops);
                return BT_HP_AC_FAIL_CONNECT;
            }
        }
        if ((int32_t)(now - ac->ready_deadline) >= 0) {
            if (ac->started_here) {
                bt_hp_autoconn_stop_started(ac, ops);
            } else if (u.link == BT_HP_LINK_CONNECTING) {
                (void)ops->disconnect();
            }
            return BT_HP_AC_FAIL_CONNECT;
        }
    } else if ((int32_t)(now - ac->start_deadline) >= 0) {
        bt_hp_autoconn_stop_started(ac, ops);
        return BT_HP_AC_FAIL_CONNECT;
    }

    ac->phase = (u.state == BT_HP_ST_READY) ? 2u : 1u;
    return BT_HP_AC_WAIT;
}

#endif /* BT_HP_AUTOCONN_H */
