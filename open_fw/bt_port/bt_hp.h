/* bt_hp: the BT=1 headphones session (A2DP source + AVRCP) on Bluedroid, docs/BT_A2DP_DESIGN.md §6 (milestones H3-H5,
 * pragmatic subset for the first silicon test; what is left out is listed in docs/BT_A2DP_HP_SESSION.md).
 *
 * Shape (same as phone mode, bt_xrec_bluedroid.c): Bluedroid callbacks (BTC task) only copy events into a bounded,
 * locked ring and notify the app; the app task drains it in bt_hp_poll() and makes every decision. Exceptions, by
 * design (§4.3-4.5): the A2DP callback drives the transmit path (hp_tx_bd_*: codec config, STARTED, SUSPEND, link
 * loss) in BTC context, where the pacer tick also runs, so start/stop/config are serialised with the tick.
 *
 * Phone mode and headphones mode are mutually exclusive (one bt_ctrl owner): bt_hp_start() refuses while phone mode
 * runs, and the caller stops headphones mode before phone mode starts (main.c). Both share the platform seam of
 * bt_xrec_bluedroid.h (bring-up / teardown jobs, deadline, hard path, bond store, entropy gate, controller notes);
 * bt_xrec_plat_set_mode() picks the bond file (bt_bonds.bin vs bt_hp_bonds.bin) and the profile teardown.
 *
 * Pairing policy (§6.3): headsets pair with Just Works. Bluedroid confirms Just Works itself (btc_dm.c); Bluedroid
 * patch 0036 asks openfw_just_works_allowed() first, which this session answers: yes only for the device the user
 * picked, while its pairing window is open, and only while the M2.3 pairing gate holds (entropy credited, ROM P-192
 * key replaced, ROM RNG pool fed). A pairing accepted that way which then fails spends the session key (no further
 * pairing until the next start). Outside headphones mode the hook allows everything (phone mode unchanged: it removes
 * every non-MITM bond itself).
 * SPDX-License-Identifier: MIT */
#ifndef BT_HP_H
#define BT_HP_H

#include <stddef.h>
#include <stdint.h>
#include "openfw_fs.h"

#define BT_HP_FOUND_MAX       8u       /* discovery list entries */
#define BT_HP_NAME_MAX        25u      /* display name bytes incl. NUL (24 + NUL, design §6.2) */
#define BT_HP_PAIR_WINDOW_MS  120000u  /* design §6.3 */
#define BT_HP_SCAN_SECS_DEF   10u      /* discovery length (0401 Inquiry_Length = ceil(secs / 1.28)) */
#define BT_HP_IDLE_SUSPEND_MS 20000u   /* design §6.5: STARTED with no real audio for this long -> SUSPEND */
#define BT_HP_START_RETRY_MS  500u     /* design §4.5: a failed START is retried once after this */
#define BT_HP_CONNECT_TO_MS   30000u   /* an A2DP connect that has not reported CONNECTED/DISCONNECTED by then */
#define BT_HP_STALL_DISC_MS   8000u    /* design §4.6: no Disconnection Complete after the stall's 0406 -> restart */
#define BT_HP_TICK_STOP_MS    100u     /* design §4.3: bound of the BTC-context tick stop at teardown */
#define BT_HP_DEINIT_WAIT_MS  600u     /* design §5.5: ESP_A2D_DEINIT_SUCCESS wait inside the down job */
#define BT_HP_VOL_DEF         64u      /* bt_vol default (0..127, AVRCP scale) */

/* Session states (bt_hp_ui_t.state) */
enum {
    BT_HP_ST_OFF = 0,
    BT_HP_ST_UP,          /* bring-up job running */
    BT_HP_ST_CONFIG,      /* Bluedroid enabled: GAP/AVRC/A2DP init, SEP registration */
    BT_HP_ST_READY,       /* profiles up; link state in bt_hp_ui_t.link */
    BT_HP_ST_FAULTED,     /* a fault ended the session gracefully: off until the next bt_hp_start() */
    BT_HP_ST_DEAD,        /* hard path taken (host tests only: on target it reboots) */
};

/* Link states (bt_hp_ui_t.link) */
enum {
    BT_HP_LINK_NONE = 0,  /* no headset */
    BT_HP_LINK_CONNECTING,/* esp_a2d_source_connect() issued (page, pairing, SDP, AVDTP) */
    BT_HP_LINK_OPEN,      /* AVDTP stream open (configured), not streaming */
    BT_HP_LINK_STARTED,   /* streaming */
    BT_HP_LINK_STALLED,   /* credit stall: the ACL is being closed (design §4.6) */
};

/* Result of the last user-visible operation (bt_hp_ui_t.msg) */
enum {
    BT_HP_MSG_NONE = 0,
    BT_HP_MSG_SCAN_DONE, BT_HP_MSG_SCAN_FAIL, BT_HP_MSG_PAIRED, BT_HP_MSG_PAIR_FAIL, BT_HP_MSG_PAIR_REFUSED,
    BT_HP_MSG_CONNECTED, BT_HP_MSG_CONNECT_FAIL, BT_HP_MSG_DISCONNECTED, BT_HP_MSG_RECONNECTING, BT_HP_MSG_FORGOTTEN,
    BT_HP_MSG_PAIRING_UNAVAILABLE, BT_HP_MSG_STALLED, BT_HP_MSG_START_FAILED,
};

typedef struct {
    uint8_t  bda[6];                 /* esp_bd_addr_t order (most significant byte first) */
    uint32_t cod;                    /* 0 = unknown */
    int8_t   rssi;
    uint8_t  has_rssi;
    uint8_t  is_audio;               /* CoD filter of design §6.2 */
    uint8_t  named;
    char     name[BT_HP_NAME_MAX];   /* the name, else "" */
} bt_hp_found_t;

typedef struct {
    uint8_t  state, link;
    uint8_t  scanning;               /* discovery running (or being cancelled) */
    uint8_t  scan_s_left;
    uint8_t  n_found;
    bt_hp_found_t found[BT_HP_FOUND_MAX];
    uint8_t  pairing;                /* pairing window open for `target` */
    uint8_t  target_valid;
    uint8_t  target[6];              /* the device being connected / last connected */
    char     target_name[BT_HP_NAME_MAX];
    uint8_t  has_last;               /* a bonded headset to reconnect to */
    uint8_t  last[6];
    char     last_name[BT_HP_NAME_MAX];
    uint8_t  reconnecting;           /* a reconnect is scheduled or running */
    uint16_t reconnect_s;            /* seconds to the next attempt (0 = now / running) */
    uint32_t rate;                   /* negotiated SBC rate (0 = none yet) */
    uint8_t  bp_min, bp_max, fpp, abs_vol;
    uint8_t  bt_vol;                 /* 0..127 */
    uint8_t  msg;                    /* BT_HP_MSG_* of the last operation; msg_seq changes with every new one */
    uint32_t msg_seq;
} bt_hp_ui_t;

typedef struct {
    uint32_t evq_lost, disc_results, names, connects, connect_fail, disconnects, reconnect_tries;
    uint32_t jw_allowed, jw_refused, cfm_rejected, pin_rejected, auth_ok, auth_fail, bonds_removed, ssp_burned;
    uint32_t starts, start_fail, suspends, remote_suspends, remote_starts, stalls, avrc_keys, avrc_vol_ntf;
    uint32_t media_busy, idle_suspends, cfg_events, delay_reports;
} bt_hp_debug_t;

/* ---- app task ---- */
/* Start headphones mode (bring-up job; then GAP/AVRCP/A2DP configuration; reconnects to the last headset). 0 = started
 * (or already running), -1 = refused (phone mode on, BT fault this boot, no memory). */
int  bt_hp_start(exfat_vol_t *vol, int sd_mounted);
/* Drive the session: bring-up/config progress, events, reconnect, idle suspend, faults. Cheap when off. */
void bt_hp_poll(void);
/* Graceful teardown with the shared 2 s deadline (design §5.5). 0 = off; else the hard-path reason (on target the
 * hard path reboots). */
int  bt_hp_stop(void);
int  bt_hp_active(void);              /* UP, CONFIG or READY */
/* Discovery (design §6.2): secs 2..61. 0 = started. Results in bt_hp_get_ui(). */
int  bt_hp_scan(uint8_t secs);
void bt_hp_scan_cancel(void);
/* Connect to a found device (index into bt_hp_ui_t.found) or an address; opens its pairing window (pair != 0).
 * Cancels discovery first and connects once it stopped. 0 = scheduled. */
int  bt_hp_pick(uint8_t idx, int pair);
int  bt_hp_connect_addr(const uint8_t bda[6], int pair);
int  bt_hp_disconnect(void);          /* drop the headset (AVDTP close; no reconnect until the next start / pick) */
int  bt_hp_forget(void);              /* remove the last headset's bond and forget it */
void bt_hp_get_ui(bt_hp_ui_t *out);
void bt_hp_get_debug(bt_hp_debug_t *out);
int  bt_hp_stream_ready(void);        /* OPEN, STARTED or suspended (aout routes to the headset) */
int  bt_hp_streaming(void);

/* ---- player integration (src/player.c, BT=1) ---- */
#define BT_HP_KEY_NONE      0
#define BT_HP_KEY_PLAY      1         /* AVRCP PLAY press: resume if paused */
#define BT_HP_KEY_PAUSE     2         /* AVRCP PAUSE / STOP press: pause if playing */
#define BT_HP_KEY_NEXT      3         /* AVRCP FORWARD press */
#define BT_HP_KEY_PREV      4         /* AVRCP BACKWARD press */
#define BT_HP_KEY_VOL_UP    5         /* passthrough volume without absolute volume */
#define BT_HP_KEY_VOL_DOWN  6
#define BT_HP_KEY_TOGGLE    7         /* AVRCP PLAY while playing on headsets with one button: treated as toggle */
int  bt_hp_take_key(void);            /* next AVRCP key (BT_HP_KEY_*), BT_HP_KEY_NONE when none */
/* Events that pause the player (design §6.7): link lost, credit stall, remote suspend, START failed for good.
 * Returns the bits seen since the last call (BT_HP_PEV_*). */
#define BT_HP_PEV_LINK_LOST      0x1u
#define BT_HP_PEV_STALLED        0x2u
#define BT_HP_PEV_REMOTE_SUSPEND 0x4u
#define BT_HP_PEV_START_FAILED   0x8u
uint32_t bt_hp_take_player_events(void);
/* Player volume 0..31 <-> bt_vol 0..127 (design §6.6). */
uint8_t bt_hp_vol31(void);
void    bt_hp_set_vol31(uint8_t v31);     /* sends SetAbsoluteVolume when the headset supports it */
int     bt_hp_abs_vol(void);              /* 1: the headset scales (absolute volume): keep the PCM gain at unity */
/* Phone mode's bt_shutdown() calls this first (weak no-op without headphones mode): one exit path for both. */
int     bt_hp_shutdown_hook(void);

/* main.c (BT=1), once after the SD mount: the boot volume for SD:/bt_hp_cfg.bin while BT is off. */
void bt_hp_set_boot_volume(exfat_vol_t *vol, const int *mounted);

/* ---- persisted headphones settings (SD:/bt_hp_cfg.bin, design §6.4/§6.6: not in ui_settings_t) ---- */
#define BT_HP_OUT_SPEAKER    0u
#define BT_HP_OUT_HEADPHONES 1u
#define BT_HP_OUT_BOTH       2u
uint8_t bt_hp_out_mode(void);         /* loads the file on first use */
void    bt_hp_set_out_mode(uint8_t mode);   /* applied to audio_out at the next file; saved */

#if defined(OPENFW_BT_BENCH_IMAGE) && OPENFW_BT_BENCH_IMAGE
/* BENCH ONLY (SD:/BTBENCH.RUN mode=hp): tunables applied at the next bt_hp_start() (hp_tx tick, queue cap, credits
 * clamp; SEP rate and max bitpool); logged. NULL = defaults. */
typedef struct {
    uint16_t rate;                    /* 0 auto, 44100, 48000 */
    uint8_t  bitpool, speech_bp, tick_ms, pkt_per_tick, txq, credit_ticks, pscan;
} bt_hp_bench_tun_t;
void bt_hp_set_bench_tun(const bt_hp_bench_tun_t *t);
#endif

/* ---- Bluedroid patch 0036 hook (btc_dm.c, BTC task) ---- */
int openfw_just_works_allowed(const uint8_t bda[6]);

/* ======================================== platform seam ====================================================== */
/* Firmware: bt_xrec_fw.c (SD files) + src/audio_out.c binding; host: tests/bt_host/host_xrec.c. */
/* Persisted settings blob (<= 64 bytes): 0 = loaded (*len set), -1 = none. Save: 0 = written. App task. */
int  bt_hp_plat_cfg_load(uint8_t *buf, uint32_t cap, uint32_t *len);
int  bt_hp_plat_cfg_save(const uint8_t *buf, uint32_t len);
/* Bind the transmit path and the PCM ring to the audio output (src/audio_out.c aout_bt_attach); NULL tx detaches.
 * link is a const aout_bt_link_t * (opaque here). App task. */
void bt_hp_plat_attach_sink(void *tx, void *sink, const void *link, int ring_is_dac_buf);
/* The DAC slot buffer while the DAC is stopped (audio_dac_borrow_buf), else NULL. */
int16_t *bt_hp_plat_borrow_dac_buf(uint32_t *bytes);

#endif /* BT_HP_H */
