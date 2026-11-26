/* Host tests for bt_port/bt_hp_autoconn.h: session cleanup on REC switch / failure / timeout, PAIR_AGAIN ordering
 * and message preservation, and connect timeout anchored to READY rather than RF/bring-up start.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "bt_hp_autoconn.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            g_fail++;                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static struct {
    bt_hp_ui_t ui;
    int        ready;
    int        connect_rc;
    uint8_t    connect_sets_msg;
    uint32_t   connect_calls;
    uint32_t   stop_calls;
    uint32_t   disc_calls;
    uint8_t    last_connect_bda[6];
    int        last_connect_pair;
} M;

static void mock_reset(void)
{
    memset(&M, 0, sizeof(M));
    M.ui.state = BT_HP_ST_UP;
    M.ui.has_last = 1u;
    for (int i = 0; i < 6; i++) {
        M.ui.last[i] = (uint8_t)(0x10 + i);
    }
}

static int mock_stream_ready(void) { return M.ready; }
static void mock_get_ui(bt_hp_ui_t *out) { *out = M.ui; }

static int mock_connect_addr(const uint8_t bda[6], int pair)
{
    M.connect_calls++;
    memcpy(M.last_connect_bda, bda, 6u);
    M.last_connect_pair = pair;
    if (M.connect_sets_msg != BT_HP_MSG_NONE) {
        M.ui.msg = M.connect_sets_msg;
        M.ui.msg_seq++;
    }
    if (M.connect_rc == 0) {
        M.ui.link = BT_HP_LINK_CONNECTING;
    }
    return M.connect_rc;
}

static int mock_stop(void)
{
    M.stop_calls++;
    M.ui.state = BT_HP_ST_OFF;
    M.ui.link = BT_HP_LINK_NONE;
    return 0;
}

static int mock_disconnect(void)
{
    M.disc_calls++;
    M.ui.link = BT_HP_LINK_NONE;
    return 0;
}

static const bt_hp_autoconn_ops_t k_ops = {
    .stream_ready = mock_stream_ready,
    .get_ui       = mock_get_ui,
    .connect_addr = mock_connect_addr,
    .stop         = mock_stop,
    .disconnect   = mock_disconnect,
};

int main(void)
{
    bt_hp_autoconn_t ac;

    /* 1. Happy path (started_here=1): UP -> READY -> stream_ready returns READY, BT stays running */
    mock_reset();
    bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
    CHECK(bt_hp_autoconn_step(&ac, 1100u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT && ac.phase == 1u, "UP wait");
    M.ui.state = BT_HP_ST_READY;
    M.ui.link  = BT_HP_LINK_CONNECTING; /* rc_poll started the immediate reconnect */
    CHECK(bt_hp_autoconn_step(&ac, 1500u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT && ac.phase == 2u, "READY wait");
    CHECK(M.connect_calls == 0u, "already connecting: connect_addr should not be called (%u)", M.connect_calls);
    M.ready = 1;
    M.ui.link = BT_HP_LINK_OPEN;
    M.ui.msg = BT_HP_MSG_CONNECTED;
    M.ui.msg_seq = 3u;
    CHECK(bt_hp_autoconn_step(&ac, 2200u, 0, 0, 0, &k_ops) == BT_HP_AC_READY, "stream ready");
    CHECK(ac.seen_seq == 3u && M.stop_calls == 0u, "seen_seq %u stop %u", ac.seen_seq, M.stop_calls);

    /* 2. REC slide switch turned ON while started_here=1: abort at once WITHOUT a teardown in front of the recording
     * (the session is left to the REC entry path, design §6.9; triage R2 F4b) */
    mock_reset();
    bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
    CHECK(bt_hp_autoconn_step(&ac, 1050u, 1, 0, 0, &k_ops) == BT_HP_AC_SILENT_ABORT, "REC abort");
    CHECK(M.stop_calls == 0u, "REC switch delayed recording with a BT teardown (stop_calls=%u)", M.stop_calls);

    /* 2b. a slow but healthy bring-up (UP + CONFIG up to bt_hp's own 12 s + 5 s deadlines) is not cut short */
    mock_reset();
    bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
    CHECK(bt_hp_autoconn_step(&ac, 1000u + 16000u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT && M.stop_calls == 0u,
          "bring-up aborted before bt_hp's own UP+CONFIG deadlines");
    CHECK(bt_hp_autoconn_step(&ac, 1000u + BT_HP_AUTOCONN_START_TO_MS, 0, 0, 0, &k_ops) == BT_HP_AC_FAIL_CONNECT &&
              M.stop_calls == 1u,
          "stuck bring-up not stopped at the safety bound (stop=%u)", M.stop_calls);

    /* REC switch when !started_here leaves the user's pre-existing session alone (main.c REC entry calls suspend) */
    mock_reset();
    M.ui.state = BT_HP_ST_READY;
    bt_hp_autoconn_init(&ac, 0, 1000u, 0u);
    CHECK(bt_hp_autoconn_step(&ac, 1050u, 1, 0, 0, &k_ops) == BT_HP_AC_SILENT_ABORT && M.stop_calls == 0u,
          "REC abort with !started_here stopped user session");

    /* 3. Non-ready failures while started_here=1 MUST stop BT (otherwise powered-off headphones leave BT paging every
     * 60 s in the background forever) */
    {
        /* 3a. CONNECT_FAIL */
        mock_reset();
        bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
        M.ui.state = BT_HP_ST_READY;
        M.ui.link  = BT_HP_LINK_CONNECTING;
        CHECK(bt_hp_autoconn_step(&ac, 1500u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT, "wait before fail");
        M.ui.link = BT_HP_LINK_NONE;
        M.ui.msg  = BT_HP_MSG_CONNECT_FAIL;
        M.ui.msg_seq = 1u;
        CHECK(bt_hp_autoconn_step(&ac, 6500u, 0, 0, 0, &k_ops) == BT_HP_AC_FAIL_MSG &&
                  ac.fail_msg == BT_HP_MSG_CONNECT_FAIL,
              "CONNECT_FAIL outcome");
        CHECK(M.stop_calls == 1u, "CONNECT_FAIL with started_here=1 leaked BT session (stop_calls=%u)", M.stop_calls);

        /* 3b. connect_addr refused (e.g. PAIR_AGAIN on unbonded last headset) */
        mock_reset();
        M.ui.state = BT_HP_ST_READY;
        M.connect_rc = -1;
        M.connect_sets_msg = BT_HP_MSG_PAIR_AGAIN;
        bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
        int rc = bt_hp_autoconn_step(&ac, 1200u, 0, 0, 0, &k_ops);
        CHECK(rc == BT_HP_AC_FAIL_MSG && ac.fail_msg == BT_HP_MSG_PAIR_AGAIN,
              "connect_addr PAIR_AGAIN should report FAIL_MSG PAIR_AGAIN (got rc=%d msg=%u)", rc, ac.fail_msg);
        CHECK(M.stop_calls == 1u && ac.seen_seq == M.ui.msg_seq,
              "connect_addr failure leaked BT or missed msg_seq (stop=%u seen=%u/%u)",
              M.stop_calls, ac.seen_seq, M.ui.msg_seq);

        /* 3c. Timeout while started_here=1 */
        mock_reset();
        M.ui.state = BT_HP_ST_READY;
        M.ui.link  = BT_HP_LINK_CONNECTING;
        bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
        CHECK(bt_hp_autoconn_step(&ac, 1200u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT, "wait at READY");
        CHECK(bt_hp_autoconn_step(&ac, 1200u + BT_HP_CONNECT_TO_MS + 1000u, 0, 0, 0, &k_ops) == BT_HP_AC_FAIL_CONNECT,
              "timeout outcome");
        CHECK(M.stop_calls == 1u, "timeout with started_here=1 leaked BT session (stop_calls=%u)", M.stop_calls);
    }

    /* 4. PAIR_AGAIN ordering: if rc_poll in bt_hp_poll() already raised PAIR_AGAIN on reaching READY, autoconn must
     * observe PAIR_AGAIN first and NEVER call bt_hp_connect_addr(last, 0) over it */
    mock_reset();
    bt_hp_autoconn_init(&ac, 1, 1000u, 0u);
    M.ui.state   = BT_HP_ST_READY;
    M.ui.link    = BT_HP_LINK_NONE;
    M.ui.msg     = BT_HP_MSG_PAIR_AGAIN;
    M.ui.msg_seq = 1u;
    CHECK(bt_hp_autoconn_step(&ac, 1400u, 0, 0, 0, &k_ops) == BT_HP_AC_FAIL_MSG &&
              ac.fail_msg == BT_HP_MSG_PAIR_AGAIN,
          "rc_poll PAIR_AGAIN not reported");
    CHECK(M.connect_calls == 0u && M.stop_calls == 1u,
          "rc_poll PAIR_AGAIN called connect_addr=%u or skipped stop=%u", M.connect_calls, M.stop_calls);

    /* 5. Connect deadline anchored to READY + wrap-safe timers + page cancel when !started_here:
     * 2500 ms spent in UP/CONFIG must not steal 2500 ms from BT_HP_CONNECT_TO_MS once READY is reached. */
    mock_reset();
    uint32_t t_wrap = 0xFFFFF800u; /* crosses 32-bit millis() wrap during bring-up/connect */
    bt_hp_autoconn_init(&ac, 1, t_wrap, 0u);
    CHECK(bt_hp_autoconn_step(&ac, t_wrap + 2500u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT, "UP across wrap");
    M.ui.state = BT_HP_ST_READY;
    M.ui.link  = BT_HP_LINK_CONNECTING;
    CHECK(bt_hp_autoconn_step(&ac, t_wrap + 2600u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT, "READY latched");
    /* 29 s after READY (31.6 s after t0): still within BT_HP_CONNECT_TO_MS (30 s) of READY! */
    CHECK(bt_hp_autoconn_step(&ac, t_wrap + 2600u + 29000u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT,
          "premature timeout stole UP/CONFIG bring-up time from connect window");

    /* When !started_here and the READY connect safety timeout fires while CONNECTING, cancel the page */
    mock_reset();
    M.ui.state = BT_HP_ST_READY;
    bt_hp_autoconn_init(&ac, 0, 5000u, 2u);
    M.ui.msg     = BT_HP_MSG_CONNECT_FAIL; /* stale msg from an earlier attempt (msg_seq == 2): must still try now */
    M.ui.msg_seq = 2u;
    CHECK(bt_hp_autoconn_step(&ac, 5000u, 0, 0, 0, &k_ops) == BT_HP_AC_WAIT && M.connect_calls == 1u,
          "stale CONNECT_FAIL blocked fresh connect");
    CHECK(bt_hp_autoconn_step(&ac, 5000u + BT_HP_CONNECT_TO_MS + 1000u, 0, 0, 0, &k_ops) == BT_HP_AC_FAIL_CONNECT,
          "!started_here timeout");
    CHECK(M.stop_calls == 0u && M.disc_calls == 1u,
          "!started_here timeout should cancel page without stopping session (stop=%u disc=%u)",
          M.stop_calls, M.disc_calls);

    printf("test_bt_hp_autoconn: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
