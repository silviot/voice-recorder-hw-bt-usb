/* bt_bench: one-shot SD trigger for unattended phone-mode tests on silicon (bench images only: make BT=1 BENCH=1;
 * a release BT=1 image links neither this nor the session overrides it switches on).
 *
 * The bench operator drives the laptop side (bluetoothctl, tools/bt_sync.py) but cannot press the device's buttons,
 * and USB is not serviced while BT mode runs. A request file in the SD root, SD:/BTBENCH.RUN, makes the next boot
 * enter BT mode by itself (after the SD mount and the first UI render, before the home loop), open the pairing window
 * on a schedule, optionally auto-confirm SSP numeric comparisons, and leave BT mode after a fixed time through the
 * normal exit path (bt_xrec_stop, as if BACK was pressed). Then the boot continues normally, so USB comes back and the
 * log can be read. Real keys still work during the run (BACK / REC slide leave early).
 *
 * One-shot like the HCI probe (hci_probe_trigger.h): the request is read, deleted and the deletion verified before
 * BT starts, so a hang / WDT cold reset / fault reboot during the run cannot start it again. A request that cannot
 * be deleted does not run; a request with bad content is consumed and refused (logged once).
 *
 * Content: whitespace-, ',' or ';'-separated key=value tokens, '#' starts a comment to the end of the line, keys and
 * words case-insensitive, a UTF-8 BOM and CR/LF are ignored. An empty file means all defaults. Unknown keys, repeated
 * keys, values out of range and anything else are refused (a typo must not silently run a different test).
 *   secs=N         10..3600, default 180: time in BT mode, from bt_xrec_start(), before leaving by itself
 *   pair=0|1       default 0: open the pairing window at once
 *   pair_every=N   0 or 10..3600 s, default 0 (never): re-open the window every N s (needs pair=1)
 *   pair_secs=N    10..600, default 60: length of each pairing window (bt_xrec_open_pairing)
 *   autoconfirm=0|1  default 0: bench override that confirms SSP numeric comparisons without a key press
 *                    (bt_xrec_set_bench_autoconfirm; cleared again when the run ends)
 *   xrec_pin=NNNNNN  exactly 6 digits, default none (DRBG PIN): bench override for the XREC PIN of every window of
 *                    the run (bt_xrec_set_bench_xrec_pin; needs pair=1; cleared when the run ends). The operator
 *                    cannot read the screen and the log is unreachable while BT mode runs, so +XREC:PAIR needs a
 *                    PIN known in advance. Without credited entropy it is not issued either (fails closed as before).
 *   exit=auto      the only exit mode (leave BT mode after secs); accepted for clarity
 *   reboot=0|1     default 0: reboot after BT mode ended and BTBENCH.LOG was written
 * Example: "secs=180 pair=1 pair_every=60 autoconfirm=1 xrec_pin=246810 exit=auto"
 *
 * Headphones mode (docs/BT_A2DP_DESIGN.md §10.2, Exp 4.0-4d; parsed and checked here, run by the headphones bench
 * session; an image without that session refuses mode=hp in bt_bench_boot()):
 *   mode=phone|hp       default phone; every hp_* key needs mode=hp; pair, pair_every, autoconfirm and xrec_pin are
 *                       phone-mode keys and are refused with mode=hp (pair_secs is the hp_pair window length)
 *   hp_stage=scan|connect|tone|player   default scan (Exp 4a / 4b / 4c / 4d)
 *   hp_addr=XX:XX:XX:XX:XX:XX   the headset (as printed by a phone or bluetoothctl, most significant byte first);
 *                       required from hp_stage=connect on unless hp_pick=first|last; not with hp_pick=first|last
 *   hp_pick=addr|first|last  default addr; first = a scan, then the first AV-class device found (headset types
 *                       first, bt_bench_hp_pick); last = the headset stored by an earlier stage (no scan; not with
 *                       hp_addr or hp_pair)
 *   hp_pair=0|1         default 0: open the pairing window (pair_secs) for the picked headset; not with hp_stage=scan
 *   hp_pin=0|1          default 0: answer a legacy PIN request from the picked device inside its window with "0000"
 *                       (bench only; the ROM pairs as initiator with legacy PIN, silicon 2026-11-25)
 *   hp_pscan=0|1        default 0: leave page scan on during 0401/0405 (Exp 4a/4b variant, design §6.2)
 *   hp_secs=N           2..61, default 10: discovery length in s (0401 Inquiry_Length = ceil(N / 1.28 s))
 *   hp_rate=44100|48000|auto   default auto: register only that SEP
 *   hp_bitpool=N        18..53, default 53: max bitpool advertised
 *   hp_speech_bp=N      0 or 18..35, default 0: per-frame bitpool for <= 16 kHz sources, 0 = use the max; given
 *                       explicitly it must not exceed hp_bitpool
 *   hp_mono=0|1         default 0: prefer mono SBC
 *   hp_tick_ms=N        10..30, default 20: pacer tick
 *   hp_pkt_per_tick=N   1..2, default 2: packets per tick; 1 needs hp_tick_ms <= 15 (design §4.3 headroom rule)
 *   hp_txq=N            2..8, default 4: TXQ_PKT_CAP
 *   hp_credit_ticks=N   1..4, default 4: elapsed clamp and the tick term of CREDIT_MAX
 *   hp_tone_hz=N        0 or 100..8000, default 1000: tone stage only; 0 = 440 / 880 Hz alternating each second
 *   hp_file=/DIR/NAME.EXT   player stage only (default: the first file of /record): an absolute path of 8.3
 *                       components, <= 47 chars
 *   hp_credits=N        4..9, default 4: experimental 1005 credit count
 * Example: "mode=hp hp_stage=tone hp_addr=00:1B:66:AA:BB:CC hp_pair=1 secs=600 hp_bitpool=35 hp_credits=4"
 * The headphones run (bt_bench_hp.c): scan logs every inquiry result; connect/tone/player pick hp_addr or the first
 * audio-class device, open its pairing window when hp_pair=1 (Just Works then pairs without the user, logged as
 * "BENCH hp pair window (bench, no user)"), and connect; tone streams for the rest of secs with a stats line every
 * second for the first 15 s, then every 15 s; player plays hp_file (or the first /record file) once.
 *
 * Every log line of the run (the g_openfw_log ring, drained while BT mode runs) is also written to SD:/BTBENCH.LOG
 * after BT mode ended (replaced by each run): the 4 KiB ring wraps in a long session.
 *
 * Pure logic (parse, take, schedule, log capture, log file) is host-tested in tests/test_bt_bench.c
 * (make test-bt-port); firmware glue in bt_bench_fw.c, hooks in src/main.c (run_bt_xrec_mode).
 * SPDX-License-Identifier: MIT */
#ifndef BT_BENCH_H
#define BT_BENCH_H

#include <stdint.h>
#include "openfw_fs.h"

#define BT_BENCH_TRIGGER_FILE "BTBENCH.RUN"   /* SD root; exFAT names are case-insensitive */
#define BT_BENCH_LOG_FILE     "BTBENCH.LOG"   /* SD root; every log line of the last run (replaced each run) */
#define BT_BENCH_TRIGGER_MAX  256u            /* longer request files are refused */

#define BT_BENCH_SECS_MIN      10u
#define BT_BENCH_SECS_MAX      3600u
#define BT_BENCH_SECS_DEF      180u
#define BT_BENCH_PAIR_SECS_MIN 10u
#define BT_BENCH_PAIR_SECS_MAX 600u
#define BT_BENCH_PAIR_SECS_DEF 60u
#define BT_BENCH_STATUS_MS     30000u         /* "BENCH status" line period while BT mode runs */

#define BT_BENCH_MODE_PHONE    0u
#define BT_BENCH_MODE_HP       1u
#define BT_BENCH_HP_SCAN       0u             /* hp_stage values, in Exp order */
#define BT_BENCH_HP_CONNECT    1u
#define BT_BENCH_HP_TONE       2u
#define BT_BENCH_HP_PLAYER     3u
#define BT_BENCH_HP_FILE_MAX   47u            /* hp_file characters */

typedef struct {
    uint16_t secs;          /* BT mode length */
    uint16_t pair_every;    /* 0 = open the window once (if pair) */
    uint16_t pair_secs;     /* pairing window length */
    uint8_t  pair;          /* open the window at the start */
    uint8_t  autoconfirm;   /* bench SSP auto-confirm override */
    uint8_t  reboot;        /* reboot after the run */
    char     xrec_pin[8];   /* "" = DRBG PIN; else the bench XREC PIN (6 digits, NUL-terminated) */
    /* headphones mode (mode=hp); defaults as in the table above */
    uint8_t  mode;          /* BT_BENCH_MODE_* */
    uint8_t  hp_stage;      /* BT_BENCH_HP_* */
    uint8_t  hp_addr_set;   /* hp_addr given */
    uint8_t  hp_addr[6];    /* esp_bd_addr_t order: most significant byte first, as written */
    uint8_t  hp_pick_first; /* hp_pick=first */
    uint8_t  hp_pick_last;  /* hp_pick=last: the stored last headset (SD:/bt_hp_cfg.bin), no scan, no pairing */
    uint8_t  hp_pair, hp_pscan, hp_mono, hp_pin;
    uint8_t  hp_secs;       /* discovery length (s) */
    uint16_t hp_rate;       /* 0 = auto, 44100, 48000 */
    uint8_t  hp_bitpool, hp_speech_bp, hp_tick_ms, hp_pkt_per_tick, hp_txq, hp_credit_ticks, hp_credits;
    uint16_t hp_tone_hz;
    char     hp_file[BT_BENCH_HP_FILE_MAX + 1u];   /* "" = none */
} bt_bench_cfg_t;

typedef enum {
    BT_BENCH_OK = 0,
    BT_BENCH_E_KEY,         /* unknown key (or a token without '=') */
    BT_BENCH_E_VALUE,       /* value not a number / word, or out of range */
    BT_BENCH_E_DUP,         /* a key given twice */
    BT_BENCH_E_CONFLICT,    /* pair_every or xrec_pin without pair=1; a mode=hp rule above broken */
    BT_BENCH_E_CHAR,        /* NUL or another control byte */
} bt_bench_err_t;

typedef enum {
    BT_BENCH_TRIG_TAKEN = 0,   /* the file was there and is gone now; content valid: run once */
    BT_BENCH_TRIG_ABSENT,      /* no request */
    BT_BENCH_TRIG_NO_SD,       /* no mounted volume */
    BT_BENCH_TRIG_NOT_FILE,    /* the name exists but is a directory (left alone) */
    BT_BENCH_TRIG_STUCK,       /* the file could not be deleted: do not run (it would run again on every boot) */
    BT_BENCH_TRIG_BAD,         /* consumed, but unreadable, too long or bad content: do not run */
} bt_bench_trig_t;

/* Defaults (what an empty request means). */
void bt_bench_cfg_default(bt_bench_cfg_t *cfg);

/* Parse a request file's content into *cfg (defaults first). Returns BT_BENCH_OK or the first error; *err_at (if
 * not NULL) gets the byte offset of the offending token. *cfg is only meaningful on BT_BENCH_OK. */
bt_bench_err_t bt_bench_parse(const uint8_t *buf, uint32_t len, bt_bench_cfg_t *cfg, uint32_t *err_at);

/* Check for the request file, read and parse it, consume it (delete + verify). App task (owns the filesystem).
 * *cfg is valid for BT_BENCH_TRIG_TAKEN only; *perr (if not NULL) gets the parse error for BT_BENCH_TRIG_BAD
 * (BT_BENCH_E_VALUE also stands for an unreadable or oversized file). */
bt_bench_trig_t bt_bench_take(exfat_vol_t *vol, int mounted, bt_bench_cfg_t *cfg, bt_bench_err_t *perr,
                              uint32_t *err_at);

/* 0401 Inquiry_Length (units of 1.28 s, 1..0x30) for hp_secs: ceil(secs / 1.28). */
uint8_t bt_bench_hp_inq_len(uint8_t secs);
const char *bt_bench_hp_stage_name(uint8_t stage);   /* scan, connect, tone, player */
/* hp_pick=first over a scan's devices (their CoD, discovery order): the first AV-major device with a headset-type minor
 * (wearable headset, hands-free, loudspeaker, headphones, portable audio), else the first AV-major device; -1 if none.
 * Devices that only set the Audio/Rendering service bits (phones, laptops) are never picked. */
int bt_bench_hp_pick(const uint32_t *cod, uint32_t n);

/* Short words for the log. */
const char *bt_bench_trig_name(bt_bench_trig_t t);   /* taken, absent, no_sd, not_file, stuck, bad_content */
const char *bt_bench_err_name(bt_bench_err_t e);     /* ok, key, value, dup, conflict, char */

/* ---- schedule: what to do at time now (ms, wrapping millis()) ---- */
#define BT_BENCH_ACT_PAIR   0x1u    /* open the pairing window (pair_secs) now */
#define BT_BENCH_ACT_EXIT   0x2u    /* time is up: leave BT mode */
#define BT_BENCH_ACT_STATUS 0x4u    /* log a status line */

typedef struct {
    bt_bench_cfg_t cfg;
    uint32_t t0;            /* run start (ms) */
    uint32_t next_pair;     /* ms after t0 of the next window; UINT32_MAX = none */
    uint32_t next_status;   /* ms after t0 of the next status line */
    uint32_t pairs;         /* windows opened so far */
    uint8_t  exited;        /* EXIT already returned */
} bt_bench_run_t;

void     bt_bench_run_begin(bt_bench_run_t *r, const bt_bench_cfg_t *cfg, uint32_t now);
/* Actions due at now (BT_BENCH_ACT_* bits). EXIT wins over PAIR (no window opens at or after the end) and is
 * returned on every call once due. A late call opens one window, not one per missed period. */
uint32_t bt_bench_run_tick(bt_bench_run_t *r, uint32_t now);

/* ---- log capture: copies what the g_openfw_log ring received since the last drain into buf ---- */
typedef struct {
    char    *buf;
    uint32_t cap;           /* bytes buf holds */
    uint32_t len;           /* bytes captured */
    uint32_t lost;          /* bytes not captured: buf full, or more than one ring's worth between two drains */
    uint32_t resets;        /* ring resets seen (write index went back: XREC log reset) */
    uint32_t last_widx;     /* ring write index at the last drain */
} bt_bench_cap_t;

void bt_bench_cap_init(bt_bench_cap_t *c, char *buf, uint32_t cap, uint32_t widx);
/* ring_size: power of two (OPENFW_LOG_SIZE); widx: the ring's free-running write index (read once by the caller). */
void bt_bench_cap_drain(bt_bench_cap_t *c, const char *ring, uint32_t ring_size, uint32_t widx);

/* Replace BT_BENCH_LOG_FILE in the SD root with data[0..len) followed by tail[0..tail_len) (either may be empty).
 * App task only; w is caller scratch. Returns 0 on success, -1 otherwise. */
int bt_bench_log_save(exfat_vol_t *vol, int mounted, const char *data, uint32_t len, const char *tail,
                      uint32_t tail_len, exfat_writer_t *w);

/* ---- firmware hooks (bt_bench_fw.c; called from src/main.c, app task) ---- */
int  bt_bench_boot(exfat_vol_t *vol, int sd_mounted);   /* 1: phone run, 2: headphones run (logs BENCH start/OFF) */
void bt_bench_begin(void);                              /* in run_bt_xrec_mode(), after bt_xrec_start() */
int  bt_bench_tick(uint32_t now_ms);                    /* in the BT-mode loop: 1 = leave BT mode now */
int  bt_bench_end(exfat_vol_t *vol, int sd_mounted);    /* after BT mode: BTBENCH.LOG; 1 = reboot requested */
/* mode=hp (bt_bench_boot() returned 2): main.c runs the headphones bench loop. */
#define BT_BENCH_HP_ACT_EXIT 0x1
#define BT_BENCH_HP_ACT_PLAY 0x2
void bt_bench_hp_prepare(void);                         /* before bt_hp_start(): the request's tunables */
void bt_bench_hp_begin(void);                           /* after bt_hp_start() */
int  bt_bench_hp_tick(uint32_t now_ms, char *path, uint32_t cap);   /* BT_BENCH_HP_ACT_*; PLAY: path to play */
void bt_bench_hp_finish(uint8_t out_mode);              /* tone off, the user's output mode back */

#endif /* BT_BENCH_H */
