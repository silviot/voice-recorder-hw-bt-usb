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
    void (*on_bound_key)(void *user, const uint8_t key[32]);
    void (*on_reboot)(void *user);
    int  (*rec_start)(void *user, const char *opt_name, char *out_name, uint32_t out_max);
    int  (*rec_stop)(void *user);
    void (*on_ota)(void *user);
} xrec_ops_t;

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
    char           rx_line[XREC_RX_LINE_MAX];

    /* Streaming +XREC:GETS state */
    uint8_t        gets_active;
    exfat_entry_t  gets_file;
    uint32_t       gets_offset;
    uint32_t       gets_rem;
    uint16_t       gets_chunk_max;

    /* Streaming +XREC:PUT -> /update.up OTA upload state */
    uint8_t        put_active;
    uint8_t        put_auth_ok;
    uint32_t       put_offset;
    uint16_t       put_len;
    uint16_t       put_got;
    uint16_t       put_crc;
    uint8_t        put_buf[XREC_MAX_CHUNK];

    uint8_t        ota_open;
    uint8_t        ota_has_last;
    exfat_writer_t ota_writer;
    uint32_t       ota_written;
    uint32_t       ota_last_off;
    uint16_t       ota_last_len;
    uint16_t       ota_last_crc;
} xrec_ctx_t;

void xrec_init(xrec_ctx_t *ctx, exfat_vol_t *vol, const xrec_ops_t *ops, void *user);
void xrec_set_bound_key(xrec_ctx_t *ctx, const uint8_t key[32]);
void xrec_open_pairing(xrec_ctx_t *ctx, const char pin_6digits[6]);
void xrec_close_pairing(xrec_ctx_t *ctx);
void xrec_on_disconnect(xrec_ctx_t *ctx);
void xrec_on_rx(xrec_ctx_t *ctx, const uint8_t *data, uint16_t len);
void xrec_poll(xrec_ctx_t *ctx);

#endif /* OPENFW_XREC_H */
