/*
 * Smartlink SL6801 OpenFW `+XREC v5` Protocol State Machine
 *
 * Implements compatible wire framing with docs/XREC_PROTOCOL.md over any
 * byte-stream transport (Classic BT RFCOMM Channel 1 or host test harness).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_XREC_H
#define OPENFW_XREC_H

#include <stdint.h>
#include "openfw_fs.h"

#define XREC_RX_LINE_MAX  192u
#define XREC_MAX_CHUNK    640u
/* Largest single reply one command line (or one completed PUT chunk) can produce: +XREC:DBG,LIVE (~585 B worst case)
 * and the +XREC:LOG frame (header <= 64 B + 512 B payload). LS / GETS stream from xrec_poll() and retry on their own.
 * Flow control (review STEP10 A5): with ops.tx_free set, xrec_on_rx() stops before any byte that would produce a reply
 * while less than this is free. */
#define XREC_REPLY_MAX    (88u + 512u)

/* ---- Sync while recording (docs/BT_SYNC_SPEED_AND_RECORDING.md F1, F1b, F6, F7; docs/XREC_PROTOCOL.md) ----
 * While ops.rec_channels reports a recording:
 *   - XREC work waits for the recorder: no command runs and no GETS / LS chunk is read from the card while
 *     ops.rec_pending reports audio blocks the recorder has not taken yet (the caller polls the recorder first);
 *   - one xrec_poll() pass stops reading the card once it used XREC_REC_PASS_BUDGET_US: GETS chunks, LS directory
 *     chunks and the GET / GETS lookup in /record (both resume at the next pass);
 *   - GETS payload is capped by a token bucket at XREC_REC_GETS_RATE_BPS (bucket XREC_REC_GETS_BURST bytes);
 *   - writers answer +XREC:ERR,LATER,<XREC_LATER_MS> (DEL, PUT to the card, PUTEND, OTA, PAIR), RTC set is not
 *     applied (+XREC:OK,RTC,LATER), and in a stereo recording GETS / GET answer LATER too (mono only, decision 5);
 *   - GETS / GET of the file being recorded answers +XREC:ERR,BUSY (ops.file_busy);
 *   - a disconnect keeps a staged upload's writer and temp until the recording stopped (xrec_deferred_poll).
 * The cap and the budget are compile-time defaults (-D...) and can be changed per context (xrec_set_rec_pace; the
 * bench image's +XREC:DBG,PACE,<bytes/s>). */
#ifndef XREC_REC_GETS_RATE_BPS
#define XREC_REC_GETS_RATE_BPS  48000u   /* bytes/s of GETS payload while recording (M3 sets the final value) */
#endif
#ifndef XREC_REC_PASS_BUDGET_US
#define XREC_REC_PASS_BUDGET_US 5000u    /* card reads per xrec_poll() pass while recording */
#endif
#ifndef XREC_REC_GETS_BURST
#define XREC_REC_GETS_BURST     2048u    /* token-bucket depth, bytes (>= XREC_MAX_CHUNK) */
#endif
#define XREC_LATER_MS           30000u   /* the <ms> of +XREC:ERR,LATER: ask again after this long */
#define XREC_PACE_WINDOW        16384u   /* app GETS window while recording (A1): STAT pace_ms is its time at the cap */
#if XREC_REC_GETS_BURST < XREC_MAX_CHUNK
#error "XREC_REC_GETS_BURST must hold one full chunk"
#endif

typedef struct {
    uint16_t voltage_mv;
    uint8_t  bat_pct;
    uint8_t  charge_status;
    uint8_t  vbus;
    uint8_t  media_status;
    uint32_t rec_sec;
    uint32_t free_mb;
    uint32_t total_mb;
    uint32_t rec_idx;      /* NNNN of the REC_NNNN.wav being recorded, 0 = none (STAT field 9) */
    uint32_t pace_ms;      /* filled by xrec_proto (STAT field 10): 0 = no pacing (not recording) */
} xrec_stat_t;

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;
} xrec_rtc_t;

typedef struct {
    int  (*send)(void *user, const uint8_t *data, uint16_t len);
    void (*get_stat)(void *user, xrec_stat_t *out);
    void (*get_rtc)(void *user, xrec_rtc_t *out);
    int  (*set_rtc)(void *user, const xrec_rtc_t *dt);
    void (*get_entropy)(void *user, uint8_t out[16]);
    /* PAIR: persist the new key BEFORE it is installed and acknowledged. Non-zero = not saved: the session replies
     * +XREC:ERR,SAVE and keeps the previous binding (review STEP10 C9). */
    int  (*on_bound_key)(void *user, const uint8_t key[32]);
    void (*on_reboot)(void *user);
    int  (*rec_start)(void *user, const char *opt_name, char *out_name, uint32_t out_max);
    int  (*rec_stop)(void *user);
    void (*on_ota)(void *user);
    int      (*exp_write)(void *user, uint32_t off, const uint8_t *data, uint16_t len);
    int      (*exp_run)(void *user, uint32_t entry_off, uint32_t arg,
                        uint32_t *rc, uint32_t *elapsed_ms,
                        uint32_t *out_len, uint32_t *log_widx);
    uint32_t (*read_log)(void *user, uint32_t off, uint8_t *dst,
                         uint32_t max_len, uint32_t *out_widx);
    void     (*reset_log)(void *user);
    void     (*on_dfu)(void *user);
    int      (*audio_cfg)(void *user, int set, uint8_t *dsp_profile,
                          uint8_t *gain, uint8_t *channels, uint8_t *vor);
    uint16_t (*get_mtu)(void *user);
    /* DEL: non-zero when /record/<name> is the file being recorded (+XREC:ERR,BUSY; review STEP10 A1). Optional. */
    int      (*file_busy)(void *user, const char *name);
    /* Bytes the transport accepts right now (send is all-or-nothing). Optional: NULL = no RX backpressure (a reply
     * that does not fit is dropped). Review STEP10 A5. */
    uint32_t (*tx_free)(void *user);
    /* Millisecond clock for the transfer status (xrec_get_xfer). Optional: NULL = a transfer stays shown until the
     * link drops. */
    uint32_t (*now_ms)(void *user);
    /* Sync while recording (see above). All optional: NULL = never recording / nothing pending. rec_channels: 0 when
     * not recording, else the running recording's channel count (1 mono, 2 stereo). rec_pending: audio blocks the
     * recorder has not taken yet (audio_ring_pending). now_us: microsecond clock, wraps at 2^32 (NULL: now_ms x 1000). */
    uint8_t  (*rec_channels)(void *user);
    uint32_t (*rec_pending)(void *user);
    uint32_t (*now_us)(void *user);
    /* Cooperative background sync (+XREC:SYNC,<pending>[,<eta_s>]) and live CWX commit notification. Optional. */
    void     (*on_sync_state)(void *user, uint32_t pending, uint32_t eta_sec);
    void     (*on_rec_cwx)(void *user, const char *cwx_name, uint8_t tr_state);
} xrec_ops_t;

/* Pacing counters (bench +XREC:DBG,PACE; tests). */
typedef struct {
    uint32_t rate_bps;         /* GETS cap while recording */
    uint32_t budget_us;        /* card-read budget per pass while recording */
    uint32_t ring_waits;       /* a command / chunk waited because the recorder had blocks pending */
    uint32_t budget_stops;     /* a pass stopped at its budget */
    uint32_t cap_stops;        /* a pass stopped for lack of tokens */
    uint32_t later;            /* +XREC:ERR,LATER replies (and RTC sets not applied) */
    uint32_t busy;             /* GETS / GET of the file being recorded (ERR,BUSY) */
    uint32_t rec_bytes;        /* GETS payload read while recording */
} xrec_pace_stats_t;

typedef enum {
    XREC_PUT_NONE = 0,
    XREC_PUT_OTA  = 1,
    XREC_PUT_EXP  = 2,
    XREC_PUT_CAS  = 3,   /* CAS,<name>: /cassini/<name> */
    XREC_PUT_REC  = 4,   /* REC,<stem>.cwx: transcript sidecar /record/<stem>.cwx (only .cwx names) */
} xrec_put_target_t;

/* A file transfer kept shown this long after its last chunk: an app sync pauses between GETS bursts (and between
 * LS and the next file), the view must not flicker. */
#define XREC_XFER_HOLD_MS 2000u
#define XREC_XFER_NAME    24u      /* display copy of the file name; a longer one is cut on a UTF-8 boundary + U+2026 */

/* The current (or last, within XREC_XFER_HOLD_MS) file transfer, for the BT screen. */
typedef struct {
    uint8_t  active;               /* a chunk moved less than XREC_XFER_HOLD_MS ago */
    uint8_t  upload;               /* 1: phone -> device (PUT REC / CAS / OTA); 0: device -> phone (GETS / GET) */
    char     name[XREC_XFER_NAME]; /* the file's name as on the card (OTA: update.up) */
    uint32_t done;                 /* GETS: highest offset + length sent; PUT: bytes staged */
    uint32_t total;                /* GETS: the file's size on the card; PUT: 0 = unknown */
} xrec_xfer_t;

typedef struct {
    exfat_vol_t   *vol;
    xrec_ops_t     ops;
    void          *user;

    uint8_t        bound;
    uint8_t        key[32];
    uint8_t        pair_active;
    uint8_t        pair_fails;
    char           pair_pin[8];

    uint8_t        auth;
    uint8_t        chal_active;
    uint8_t        nonce[16];

    uint16_t       rx_len;
    uint8_t        rx_overflow;
    char           rx_line[XREC_RX_LINE_MAX];

    /* Streaming +XREC:GETS state */
    uint8_t        gets_active;
    uint8_t        gets_staged;
    uint8_t        gets_staged_eof;
    uint16_t       gets_staged_len;
    exfat_entry_t  gets_file;
    uint32_t       gets_offset;
    uint32_t       gets_rem;
    uint16_t       gets_chunk_max;
    /* While recording, the GET / GETS file lookup in /record runs over several polls within the pass budget */
    uint8_t        gets_lookup;
    exfat_entry_t  gets_dir;
    exfat_dir_pos_t gets_find_pos;
    char           gets_name[EXFAT_MAX_NAME];
    uint32_t       walk_t0;            /* pace_now_us() at the start of the lookup pass */

    /* Resumable +XREC:LS state */
    uint8_t        ls_active;
    uint8_t        ls_has_dir;
    exfat_entry_t  ls_dir;
    exfat_dir_pos_t ls_pos;            /* the first directory slot not yet listed (entries before it were sent) */

    /* Streaming +XREC:PUT -> OTA / EXP / CAS / REC upload state */
    uint8_t        put_active;
    uint8_t        put_discard;        /* refused header: eat put_len payload bytes unparsed, no reply */
    uint8_t        put_auth_ok;
    uint8_t        put_target;
    char           put_name[32];       /* CAS / REC file name of the chunk being received */
    uint32_t       put_offset;
    uint16_t       put_len;
    uint16_t       put_got;
    uint16_t       put_crc;
    /* PUT binary ingestion and GETS staged frame are mutually exclusive */
    union {
        uint8_t    put_buf[XREC_MAX_CHUNK];
        uint8_t    gets_frame[12u + XREC_MAX_CHUNK];
    } xfer;

    uint8_t        put_stream_mode;
    uint8_t        put_orphan;         /* a staged upload whose link went during a recording: put_writer still open,
                                        * closed and its temp deleted once the recording stopped (F6) */
    uint8_t        put_has_last;
    uint8_t        put_last_mode;
    char           put_stream_name[32];
    exfat_writer_t put_writer;
    uint32_t       put_written;
    uint32_t       put_last_off;
    uint16_t       put_last_len;
    uint16_t       put_last_crc;
    /* last PUTEND commit, so a repeated PUTEND (lost reply) answers the same OK until the next PUT header */
    uint8_t        put_done_valid;
    uint8_t        put_rec_deleted;
    uint8_t        put_cwx_state;      /* 1 = words > 0, 2 = 0 words ("No speech") */
    uint32_t       put_done_total;

    /* Transfer status (xrec_get_xfer): updated where GETS / PUT chunks move, read by the UI from the same task */
    uint8_t        xfer_valid;
    uint8_t        xfer_upload;
    char           xfer_name[XREC_XFER_NAME];
    uint32_t       xfer_id;            /* GETS: the file's first cluster (same file = same id) */
    uint32_t       xfer_done;
    uint32_t       xfer_total;
    uint32_t       xfer_t_ms;          /* ops.now_ms at the last chunk */

    /* Sync while recording: GETS token bucket (bytes), its last refill (ops.now_us), counters */
    uint8_t        pace_live;          /* the bucket runs (a recording was seen by the last pass) */
    uint32_t       pace_tokens;
    uint32_t       pace_t_us;
    xrec_pace_stats_t pace;
} xrec_ctx_t;

void xrec_init(xrec_ctx_t *ctx, exfat_vol_t *vol, const xrec_ops_t *ops, void *user);
void xrec_set_bound_key(xrec_ctx_t *ctx, const uint8_t key[32]);
void xrec_open_pairing(xrec_ctx_t *ctx, const char pin_6digits[6]);
void xrec_close_pairing(xrec_ctx_t *ctx);
void xrec_on_disconnect(xrec_ctx_t *ctx);
/* Card work a disconnect deferred while recording (a staged upload's temp): runs it once no recording is running.
 * Call it every session poll, connected or not. No card access while recording. */
void xrec_deferred_poll(xrec_ctx_t *ctx);
/* The same now, recording or not (session end: the context goes away). */
void xrec_deferred_flush(xrec_ctx_t *ctx);
/* Returns the bytes consumed. Fewer than len only when ops.tx_free reports less than XREC_REPLY_MAX free before a byte
 * that completes a command (line end, PUT header ':' or the last PUT payload byte): the caller keeps the rest and
 * hands it in again after the transport drained (xrec_poll() first). */
uint16_t xrec_on_rx(xrec_ctx_t *ctx, const uint8_t *data, uint16_t len);
void xrec_poll(xrec_ctx_t *ctx);
/* The transfer shown on the BT screen; out->active = 0 once no chunk moved for XREC_XFER_HOLD_MS (now_ms: the clock
 * of ops.now_ms). No SD access. */
void xrec_get_xfer(const xrec_ctx_t *ctx, uint32_t now_ms, xrec_xfer_t *out);
/* GETS cap (bytes/s, 0 keeps the current one) and card-read budget per pass (us, 0 keeps) while recording. */
void xrec_set_rec_pace(xrec_ctx_t *ctx, uint32_t rate_bps, uint32_t budget_us);
void xrec_get_pace_stats(const xrec_ctx_t *ctx, xrec_pace_stats_t *out);

#endif /* OPENFW_XREC_H */
