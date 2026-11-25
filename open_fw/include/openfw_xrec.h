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

typedef struct {
    uint16_t voltage_mv;
    uint8_t  bat_pct;
    uint8_t  charge_status;
    uint8_t  vbus;
    uint8_t  media_status;
    uint32_t rec_sec;
    uint32_t free_mb;
    uint32_t total_mb;
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
} xrec_ops_t;

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

    /* Resumable +XREC:LS state */
    uint8_t        ls_active;
    uint8_t        ls_has_dir;
    exfat_entry_t  ls_dir;
    uint32_t       ls_sent_cnt;

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
    uint32_t       put_done_total;

    /* Transfer status (xrec_get_xfer): updated where GETS / PUT chunks move, read by the UI from the same task */
    uint8_t        xfer_valid;
    uint8_t        xfer_upload;
    char           xfer_name[XREC_XFER_NAME];
    uint32_t       xfer_id;            /* GETS: the file's first cluster (same file = same id) */
    uint32_t       xfer_done;
    uint32_t       xfer_total;
    uint32_t       xfer_t_ms;          /* ops.now_ms at the last chunk */
} xrec_ctx_t;

void xrec_init(xrec_ctx_t *ctx, exfat_vol_t *vol, const xrec_ops_t *ops, void *user);
void xrec_set_bound_key(xrec_ctx_t *ctx, const uint8_t key[32]);
void xrec_open_pairing(xrec_ctx_t *ctx, const char pin_6digits[6]);
void xrec_close_pairing(xrec_ctx_t *ctx);
void xrec_on_disconnect(xrec_ctx_t *ctx);
/* Returns the bytes consumed. Fewer than len only when ops.tx_free reports less than XREC_REPLY_MAX free before a byte
 * that completes a command (line end, PUT header ':' or the last PUT payload byte): the caller keeps the rest and
 * hands it in again after the transport drained (xrec_poll() first). */
uint16_t xrec_on_rx(xrec_ctx_t *ctx, const uint8_t *data, uint16_t len);
void xrec_poll(xrec_ctx_t *ctx);
/* The transfer shown on the BT screen; out->active = 0 once no chunk moved for XREC_XFER_HOLD_MS (now_ms: the clock
 * of ops.now_ms). No SD access. */
void xrec_get_xfer(const xrec_ctx_t *ctx, uint32_t now_ms, xrec_xfer_t *out);

#endif /* OPENFW_XREC_H */
