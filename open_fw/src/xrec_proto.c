/*
 * Smartlink SL6801 OpenFW `+XREC v5` Protocol Implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_xrec.h"
#include "openfw_crypto.h"

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

static int str_starts_with(const char *s, const char *pfx)
{
    while (*pfx != '\0') {
        if (*s++ != *pfx++) {
            return 0;
        }
    }
    return 1;
}

static int parse_u32_step(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    uint32_t v = 0u;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
    }
    *pp  = p;
    *out = v;
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int decode_hex(const char *s, uint8_t *out, uint32_t bytes_cnt)
{
    for (uint32_t i = 0u; i < bytes_cnt; i++) {
        int hi = hex_nibble(s[i * 2u]);
        int lo = hex_nibble(s[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (s[bytes_cnt * 2u] == '\0') ? 0 : -1;
}

static void append_str(char *dst, int *pos, int max, const char *s)
{
    while (*s != '\0' && *pos < (max - 1)) {
        dst[(*pos)++] = *s++;
    }
}

static void append_u32(char *dst, int *pos, int max, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0u) {
        if (*pos < (max - 1)) {
            dst[(*pos)++] = '0';
        }
        return;
    }
    while (v > 0u && n < 11) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && *pos < (max - 1)) {
        dst[(*pos)++] = tmp[--n];
    }
}

static void append_2d(char *dst, int *pos, int max, uint32_t v)
{
    if (*pos + 2 < max) {
        dst[(*pos)++] = (char)('0' + ((v / 10u) % 10u));
        dst[(*pos)++] = (char)('0' + (v % 10u));
    }
}

static void append_hex(char *dst, int *pos, int max, const uint8_t *in, uint32_t nbytes)
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < nbytes && (*pos + 2) < max; i++) {
        dst[(*pos)++] = hex[in[i] >> 4];
        dst[(*pos)++] = hex[in[i] & 0x0Fu];
    }
}

static void send_text(xrec_ctx_t *ctx, const char *s, int len)
{
    if (ctx->ops.send && len > 0) {
        (void)ctx->ops.send(ctx->user, (const uint8_t *)s, (uint16_t)len);
    }
}

static void send_err(xrec_ctx_t *ctx, const char *reason)
{
    char buf[64];
    int pos = 0;
    append_str(buf, &pos, (int)sizeof(buf), "+XREC:ERR,");
    append_str(buf, &pos, (int)sizeof(buf), reason);
    append_str(buf, &pos, (int)sizeof(buf), "\r\n");
    send_text(ctx, buf, pos);
}

static int is_safe_filename(const char *s)
{
    if (!s || s[0] == '\0') {
        return 0;
    }
    if (s[0] == '.' && (s[1] == '\0' || (s[1] == '.' && s[2] == '\0'))) {
        return 0;
    }
    uint32_t n = 0u;
    while (s[n] != '\0') {
        if (s[n] == '/' || s[n] == '\\' || (uint8_t)s[n] < 0x20u || n >= 79u) {
            return 0;
        }
        n++;
    }
    return 1;
}

typedef struct {
    xrec_ctx_t *ctx;
    char        buf[512];
    int         pos;
    int         err;
} ls_ctx_t;

static int ls_flush(ls_ctx_t *lc)
{
    if (lc->pos <= 0) {
        return 0;
    }
    if (!lc->ctx->ops.send ||
        lc->ctx->ops.send(lc->ctx->user, (const uint8_t *)lc->buf, (uint16_t)lc->pos) != 0) {
        lc->err = -1;
        return -1;
    }
    lc->pos = 0;
    return 0;
}

static int ls_append(ls_ctx_t *lc, const char *line, int len)
{
    if ((lc->pos + len) > (int)sizeof(lc->buf)) {
        if (ls_flush(lc) != 0) {
            return -1;
        }
    }
    for (int i = 0; i < len && lc->pos < (int)sizeof(lc->buf); i++) {
        lc->buf[lc->pos++] = line[i];
    }
    return 0;
}

static int ls_dir_cb(const exfat_entry_t *entry, void *user)
{
    ls_ctx_t *lc = (ls_ctx_t *)user;
    if (entry->is_dir) {
        return 0;
    }
    char line[128];
    int pos = 0;
    uint32_t sz = (entry->size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)entry->size;
    append_str(line, &pos, (int)sizeof(line), "+XREC:F,");
    append_u32(line, &pos, (int)sizeof(line), sz);
    append_str(line, &pos, (int)sizeof(line), ",");
    append_str(line, &pos, (int)sizeof(line), entry->name);
    append_str(line, &pos, (int)sizeof(line), "\r\n");
    if (ls_append(lc, line, pos) != 0) {
        return 1;
    }
    return 0;
}

static void handle_ping(xrec_ctx_t *ctx)
{
    char buf[96];
    int pos = 0;
    append_str(buf, &pos, (int)sizeof(buf), "+XREC:PONG,openfw_v5,v5,bound=");
    append_u32(buf, &pos, (int)sizeof(buf), ctx->bound ? 1u : 0u);
    append_str(buf, &pos, (int)sizeof(buf), ",auth=");
    append_u32(buf, &pos, (int)sizeof(buf), ctx->auth ? 1u : 0u);
    append_str(buf, &pos, (int)sizeof(buf), ",pair=");
    append_u32(buf, &pos, (int)sizeof(buf), ctx->pair_active ? 1u : 0u);
    append_str(buf, &pos, (int)sizeof(buf), "\r\n");
    send_text(ctx, buf, pos);
}

static void handle_pair(xrec_ctx_t *ctx, const char *args)
{
    if (!ctx->pair_active) {
        send_err(ctx, "PAIRCLOSED");
        return;
    }
    /* Expect <6-digit-pin>,<64-hex-key> */
    int pin_ok = 1;
    for (int i = 0; i < 6; i++) {
        if (args[i] < '0' || args[i] > '9' || args[i] != ctx->pair_pin[i]) {
            pin_ok = 0;
        }
    }
    if (!pin_ok || args[6] != ',') {
        ctx->pair_fails++;
        if (ctx->pair_fails >= 5u) {
            ctx->pair_active = 0u;
        }
        send_err(ctx, "PIN");
        return;
    }
    uint8_t new_key[32];
    if (decode_hex(args + 7, new_key, 32u) != 0) {
        send_err(ctx, "BADKEY");
        return;
    }
    mem_copy(ctx->key, new_key, 32u);
    ctx->bound       = 1u;
    ctx->auth        = 1u;
    ctx->pair_active = 0u;
    ctx->pair_fails  = 0u;
    if (ctx->ops.on_bound_key) {
        ctx->ops.on_bound_key(ctx->user, ctx->key);
    }
    static const char ok[] = "+XREC:OK,PAIR\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
}

static void handle_chal(xrec_ctx_t *ctx)
{
    if (ctx->ops.get_entropy) {
        ctx->ops.get_entropy(ctx->user, ctx->nonce);
    } else {
        for (uint32_t i = 0u; i < 16u; i++) {
            ctx->nonce[i] = (uint8_t)(0xA0u + i);
        }
    }
    ctx->chal_active = 1u;
    char buf[64];
    int pos = 0;
    append_str(buf, &pos, (int)sizeof(buf), "+XREC:CHAL,");
    append_hex(buf, &pos, (int)sizeof(buf), ctx->nonce, 16u);
    append_str(buf, &pos, (int)sizeof(buf), "\r\n");
    send_text(ctx, buf, pos);
}

static void handle_auth(xrec_ctx_t *ctx, const char *hex_mac)
{
    if (!ctx->bound) {
        send_err(ctx, "UNBOUND");
        return;
    }
    if (!ctx->chal_active) {
        send_err(ctx, "NOCHAL");
        return;
    }
    ctx->chal_active = 0u;

    uint8_t got[32], expected[32];
    if (decode_hex(hex_mac, got, 32u) != 0) {
        send_err(ctx, "AUTH");
        return;
    }
    openfw_hmac_sha256(ctx->key, 32u, ctx->nonce, 16u, expected);
    if (!openfw_ct_memeq(got, expected, 32u)) {
        send_err(ctx, "AUTH");
        return;
    }
    ctx->auth = 1u;
    static const char ok[] = "+XREC:OK,AUTH\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
}

static void handle_stat(xrec_ctx_t *ctx)
{
    xrec_stat_t st;
    mem_zero(&st, sizeof(st));
    if (ctx->ops.get_stat) {
        ctx->ops.get_stat(ctx->user, &st);
    }
    if (st.total_mb == 0u && ctx->vol) {
        (void)exfat_stat_fs(ctx->vol, &st.free_mb, &st.total_mb);
    }
    char buf[128];
    int pos = 0;
    append_str(buf, &pos, (int)sizeof(buf), "+XREC:STAT,");
    append_u32(buf, &pos, (int)sizeof(buf), st.bat_pct);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.voltage_mv);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.charge_status);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.vbus);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.media_status);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.rec_sec);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.free_mb);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), st.total_mb);
    append_str(buf, &pos, (int)sizeof(buf), "\r\n");
    send_text(ctx, buf, pos);
}

static void handle_rtc(xrec_ctx_t *ctx, const char *args)
{
    if (*args == '\0') {
        xrec_rtc_t rt;
        mem_zero(&rt, sizeof(rt));
        if (ctx->ops.get_rtc) {
            ctx->ops.get_rtc(ctx->user, &rt);
        }
        char buf[64];
        int pos = 0;
        append_str(buf, &pos, (int)sizeof(buf), "+XREC:RTC,");
        append_u32(buf, &pos, (int)sizeof(buf), rt.year);
        append_str(buf, &pos, (int)sizeof(buf), "-");
        append_2d(buf, &pos, (int)sizeof(buf), rt.month);
        append_str(buf, &pos, (int)sizeof(buf), "-");
        append_2d(buf, &pos, (int)sizeof(buf), rt.day);
        append_str(buf, &pos, (int)sizeof(buf), " ");
        append_2d(buf, &pos, (int)sizeof(buf), rt.hour);
        append_str(buf, &pos, (int)sizeof(buf), ":");
        append_2d(buf, &pos, (int)sizeof(buf), rt.minute);
        append_str(buf, &pos, (int)sizeof(buf), ":");
        append_2d(buf, &pos, (int)sizeof(buf), rt.second);
        append_str(buf, &pos, (int)sizeof(buf), ",");
        append_u32(buf, &pos, (int)sizeof(buf), rt.weekday);
        append_str(buf, &pos, (int)sizeof(buf), "\r\n");
        send_text(ctx, buf, pos);
        return;
    }

    if (*args != ',' || !ctx->ops.set_rtc) {
        send_err(ctx, "ARGS");
        return;
    }
    const char *p = args + 1;
    uint32_t y, m, d, hh, mm, ss, wd;
    if (parse_u32_step(&p, &y) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &m) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &d) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &hh) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &mm) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &ss) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &wd) != 0 || *p != '\0') {
        send_err(ctx, "ARGS");
        return;
    }
    if (y < 2000u || y > 2099u || m < 1u || m > 12u || d < 1u || d > 31u ||
        hh > 23u || mm > 59u || ss > 59u || wd > 6u) {
        send_err(ctx, "RTCRNG");
        return;
    }
    xrec_rtc_t rt = {
        (uint16_t)y, (uint8_t)m, (uint8_t)d,
        (uint8_t)hh, (uint8_t)mm, (uint8_t)ss, (uint8_t)wd
    };
    if (ctx->ops.set_rtc(ctx->user, &rt) != 0) {
        send_err(ctx, "RTC");
        return;
    }
    static const char ok[] = "+XREC:OK,RTC\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
}

static void handle_ls(xrec_ctx_t *ctx)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    ls_ctx_t lc;
    lc.ctx = ctx;
    lc.pos = 0;
    lc.err = 0;

    exfat_entry_t rec_dir;
    if (exfat_find_path(ctx->vol, "/record", &rec_dir) == 0 && rec_dir.is_dir) {
        int rc = exfat_list_dir(ctx->vol, rec_dir.first_cluster, rec_dir.no_fat_chain, ls_dir_cb, &lc);
        if (lc.err != 0) {
            return;
        }
        if (rc != 0) {
            send_err(ctx, "SDIO");
            return;
        }
    }
    static const char end[] = "+XREC:END\r\n";
    if (ls_append(&lc, end, (int)(sizeof(end) - 1u)) == 0) {
        (void)ls_flush(&lc);
    }
}

static void handle_get_common(xrec_ctx_t *ctx, const char *args, int is_stream)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    const char *p = args;
    uint32_t off = 0u, tot = 0u, chunk = 0u;
    if (parse_u32_step(&p, &off) != 0 || *p++ != ',' ||
        parse_u32_step(&p, &tot) != 0 || *p++ != ',' ||
        tot == 0u) {
        send_err(ctx, "ARGS");
        return;
    }
    if (is_stream) {
        if (parse_u32_step(&p, &chunk) != 0 || *p++ != ',' ||
            chunk == 0u || chunk > XREC_MAX_CHUNK) {
            send_err(ctx, "ARGS");
            return;
        }
    } else {
        if (tot > XREC_MAX_CHUNK) {
            tot = XREC_MAX_CHUNK;
        }
        chunk = tot;
    }

    if (!is_safe_filename(p)) {
        send_err(ctx, "NAME");
        return;
    }

    char path[96];
    int pos = 0;
    append_str(path, &pos, (int)sizeof(path), "/record/");
    append_str(path, &pos, (int)sizeof(path), p);
    path[pos] = '\0';

    if (exfat_find_path(ctx->vol, path, &ctx->gets_file) != 0 || ctx->gets_file.is_dir) {
        send_err(ctx, "NOENT");
        return;
    }

    ctx->gets_offset    = off;
    ctx->gets_rem       = tot;
    ctx->gets_chunk_max = (uint16_t)chunk;
    ctx->gets_active    = 1u;
    xrec_poll(ctx);
}

static void handle_del(xrec_ctx_t *ctx, const char *filename)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    if (!is_safe_filename(filename)) {
        send_err(ctx, "NAME");
        return;
    }
    if (exfat_delete_file(ctx->vol, "/record", filename) != 0) {
        send_err(ctx, "DEL");
        return;
    }
    static const char ok[] = "+XREC:OK,DEL\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
}

static void handle_recstart(xrec_ctx_t *ctx, const char *opt_name)
{
    if (opt_name && !is_safe_filename(opt_name)) {
        send_err(ctx, "NAME");
        return;
    }
    if (!ctx->ops.rec_start) {
        send_err(ctx, "UNSUP");
        return;
    }
    char out_name[80];
    mem_zero(out_name, sizeof(out_name));
    if (ctx->ops.rec_start(ctx->user, opt_name, out_name, (uint32_t)sizeof(out_name)) != 0 ||
        out_name[0] == '\0') {
        send_err(ctx, "RECSTART");
        return;
    }
    char resp[112];
    int pos = 0;
    append_str(resp, &pos, (int)sizeof(resp), "+XREC:OK,RECSTART,");
    append_str(resp, &pos, (int)sizeof(resp), out_name);
    append_str(resp, &pos, (int)sizeof(resp), "\r\n");
    send_text(ctx, resp, pos);
}

static void handle_recstop(xrec_ctx_t *ctx)
{
    if (!ctx->ops.rec_stop) {
        send_err(ctx, "UNSUP");
        return;
    }
    if (ctx->ops.rec_stop(ctx->user) != 0) {
        send_err(ctx, "NOTREC");
        return;
    }
    static const char ok[] = "+XREC:OK,RECSTOP\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
}

static void send_put_ok(xrec_ctx_t *ctx, uint32_t offset, uint16_t chunk_len)
{
    char buf[64];
    int pos = 0;
    append_str(buf, &pos, (int)sizeof(buf), "+XREC:OK,PUT,");
    append_u32(buf, &pos, (int)sizeof(buf), offset);
    append_str(buf, &pos, (int)sizeof(buf), ",");
    append_u32(buf, &pos, (int)sizeof(buf), (uint32_t)chunk_len);
    append_str(buf, &pos, (int)sizeof(buf), "\r\n");
    send_text(ctx, buf, pos);
}

static void handle_put_complete(xrec_ctx_t *ctx)
{
    if (!ctx->put_auth_ok || !ctx->auth) {
        send_err(ctx, "LOCKED");
        return;
    }
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }

    uint16_t crc = openfw_crc16_ccitt_false(ctx->put_buf, ctx->put_len);
    if (crc != ctx->put_crc) {
        send_err(ctx, "CRC");
        return;
    }

    if (ctx->ota_open && ctx->ota_has_last &&
        ctx->put_offset == ctx->ota_last_off &&
        ctx->put_len == ctx->ota_last_len &&
        ctx->put_crc == ctx->ota_last_crc) {
        send_put_ok(ctx, ctx->put_offset, ctx->put_len);
        return;
    }

    if (ctx->put_offset == 0u) {
        if (ctx->ota_open) {
            (void)exfat_close_file(&ctx->ota_writer);
            ctx->ota_open = 0u;
        }
        exfat_entry_t existing;
        if (exfat_find_path(ctx->vol, "/update.up", &existing) == 0 && !existing.is_dir) {
            (void)exfat_delete_file(ctx->vol, "/", "update.up");
        }
        if (exfat_create_file(ctx->vol, "/", "update.up", &ctx->ota_writer) != 0) {
            send_err(ctx, "SDIO");
            return;
        }
        ctx->ota_open     = 1u;
        ctx->ota_written  = 0u;
        ctx->ota_has_last = 0u;
    } else if (!ctx->ota_open || ctx->put_offset != ctx->ota_written) {
        send_err(ctx, "OFF");
        return;
    }

    if (exfat_write(&ctx->ota_writer, ctx->put_buf, ctx->put_len) != 0) {
        send_err(ctx, "SDIO");
        return;
    }

    ctx->ota_written  += ctx->put_len;
    ctx->ota_last_off  = ctx->put_offset;
    ctx->ota_last_len  = ctx->put_len;
    ctx->ota_last_crc  = ctx->put_crc;
    ctx->ota_has_last  = 1u;
    send_put_ok(ctx, ctx->put_offset, ctx->put_len);
}

static void handle_ota(xrec_ctx_t *ctx)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    if (!ctx->ota_open || ctx->ota_written < 64u) {
        send_err(ctx, "NOOTA");
        return;
    }

    int rc = exfat_close_file(&ctx->ota_writer);
    ctx->ota_open     = 0u;
    ctx->ota_has_last = 0u;
    if (rc != 0) {
        send_err(ctx, "SDIO");
        return;
    }

    static const char ok[] = "+XREC:OK,OTA\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
    if (ctx->ops.on_ota) {
        ctx->ops.on_ota(ctx->user);
    }
}

static void try_start_put_header(xrec_ctx_t *ctx)
{
    ctx->rx_line[ctx->rx_len] = '\0';
    ctx->rx_len = 0u;

    const char *p = ctx->rx_line + 10; /* after "+XREC:PUT," */
    uint32_t off = 0u, chunk_len = 0u, crc = 0u;
    if (parse_u32_step(&p, &off) != 0 || *p != ',') {
        send_err(ctx, "ARG");
        return;
    }
    p++;
    if (parse_u32_step(&p, &chunk_len) != 0 || *p != ',') {
        send_err(ctx, "ARG");
        return;
    }
    p++;
    if (parse_u32_step(&p, &crc) != 0) {
        send_err(ctx, "ARG");
        return;
    }
    if (p[0] != ',' || p[1] != 'O' || p[2] != 'T' || p[3] != 'A' || p[4] != '\0') {
        send_err(ctx, "ARG");
        return;
    }
    if (chunk_len == 0u || chunk_len > XREC_MAX_CHUNK || crc > 0xFFFFu) {
        send_err(ctx, "ARG");
        return;
    }

    ctx->put_active  = 1u;
    ctx->put_auth_ok = ctx->auth;
    ctx->put_offset  = off;
    ctx->put_len     = (uint16_t)chunk_len;
    ctx->put_crc     = (uint16_t)crc;
    ctx->put_got     = 0u;
}

static void process_line(xrec_ctx_t *ctx, const char *line)
{
    if (!str_starts_with(line, "+XREC:")) {
        return;
    }
    const char *cmd = line + 6;

    if (str_starts_with(cmd, "PING") && cmd[4] == '\0') {
        handle_ping(ctx);
        return;
    }
    if (str_starts_with(cmd, "PAIR,")) {
        handle_pair(ctx, cmd + 5);
        return;
    }
    if (str_starts_with(cmd, "CHAL") && cmd[4] == '\0') {
        handle_chal(ctx);
        return;
    }
    if (str_starts_with(cmd, "AUTH,")) {
        handle_auth(ctx, cmd + 5);
        return;
    }

    if (!ctx->auth) {
        send_err(ctx, "LOCKED");
        return;
    }

    if (str_starts_with(cmd, "STAT") && cmd[4] == '\0') {
        handle_stat(ctx);
    } else if (str_starts_with(cmd, "RTC") && (cmd[3] == '\0' || cmd[3] == ',')) {
        handle_rtc(ctx, cmd + 3);
    } else if (str_starts_with(cmd, "LS") && cmd[2] == '\0') {
        handle_ls(ctx);
    } else if (str_starts_with(cmd, "GETS,")) {
        handle_get_common(ctx, cmd + 5, 1);
    } else if (str_starts_with(cmd, "GET,")) {
        handle_get_common(ctx, cmd + 4, 0);
    } else if (str_starts_with(cmd, "DEL,")) {
        handle_del(ctx, cmd + 4);
    } else if (str_starts_with(cmd, "RECSTART") && (cmd[8] == '\0' || cmd[8] == ',')) {
        handle_recstart(ctx, (cmd[8] == ',') ? (cmd + 9) : 0);
    } else if (str_starts_with(cmd, "RECSTOP") && cmd[7] == '\0') {
        handle_recstop(ctx);
    } else if (str_starts_with(cmd, "REBOOT") && cmd[6] == '\0') {
        static const char ok[] = "+XREC:OK,REBOOT\r\n";
        send_text(ctx, ok, (int)(sizeof(ok) - 1u));
        if (ctx->ops.on_reboot) {
            ctx->ops.on_reboot(ctx->user);
        }
    } else if (str_starts_with(cmd, "OTA") && cmd[3] == '\0') {
        handle_ota(ctx);
    } else {
        send_err(ctx, "CMD");
    }
}

void xrec_init(xrec_ctx_t *ctx, exfat_vol_t *vol, const xrec_ops_t *ops, void *user)
{
    if (!ctx) {
        return;
    }
    mem_zero(ctx, sizeof(*ctx));
    ctx->vol  = vol;
    ctx->user = user;
    if (ops) {
        ctx->ops = *ops;
    }
}

void xrec_set_bound_key(xrec_ctx_t *ctx, const uint8_t key[32])
{
    if (!ctx || !key) {
        return;
    }
    mem_copy(ctx->key, key, 32u);
    ctx->bound = 1u;
}

void xrec_open_pairing(xrec_ctx_t *ctx, const char pin_6digits[6])
{
    if (!ctx || !pin_6digits) {
        return;
    }
    mem_copy(ctx->pair_pin, pin_6digits, 6u);
    ctx->pair_pin[6] = '\0';
    ctx->pair_active = 1u;
    ctx->pair_fails  = 0u;
}

void xrec_close_pairing(xrec_ctx_t *ctx)
{
    if (ctx) {
        ctx->pair_active = 0u;
    }
}

void xrec_on_disconnect(xrec_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->ota_open) {
        (void)exfat_close_file(&ctx->ota_writer);
    }
    ctx->auth         = 0u;
    ctx->chal_active  = 0u;
    ctx->rx_len       = 0u;
    ctx->gets_active  = 0u;
    ctx->put_active   = 0u;
    ctx->put_got      = 0u;
    ctx->ota_open     = 0u;
    ctx->ota_has_last = 0u;
    ctx->ota_written  = 0u;
}

void xrec_on_rx(xrec_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    if (!ctx || !data) {
        return;
    }
    for (uint16_t i = 0u; i < len; i++) {
        if (ctx->put_active) {
            ctx->put_buf[ctx->put_got++] = data[i];
            if (ctx->put_got >= ctx->put_len) {
                ctx->put_active = 0u;
                handle_put_complete(ctx);
            }
            continue;
        }
        char c = (char)data[i];
        if (c == '\n') {
            continue;
        }
        if (c == '\r') {
            ctx->rx_line[ctx->rx_len] = '\0';
            if (ctx->rx_len > 0u) {
                process_line(ctx, ctx->rx_line);
            }
            ctx->rx_len = 0u;
            continue;
        }
        if (c == ':' && ctx->rx_len >= 14u && str_starts_with(ctx->rx_line, "+XREC:PUT,")) {
            try_start_put_header(ctx);
            continue;
        }
        if (ctx->rx_len < (XREC_RX_LINE_MAX - 1u)) {
            ctx->rx_line[ctx->rx_len++] = c;
        }
    }
}

void xrec_poll(xrec_ctx_t *ctx)
{
    if (!ctx || !ctx->gets_active || !ctx->ops.send) {
        return;
    }

    uint8_t frame[12u + XREC_MAX_CHUNK];
    while (ctx->gets_active && ctx->gets_rem > 0u) {
        uint32_t want = (ctx->gets_rem < ctx->gets_chunk_max) ? ctx->gets_rem : ctx->gets_chunk_max;
        uint32_t got  = 0u;
        if (exfat_read_file(ctx->vol, &ctx->gets_file, (uint64_t)ctx->gets_offset,
                            &frame[12], want, &got) != 0) {
            ctx->gets_active = 0u;
            send_err(ctx, "SDIO");
            return;
        }

        uint16_t crc = openfw_crc16_ccitt_false(&frame[12], got);
        frame[0]  = 'X';
        frame[1]  = 'R';
        frame[2]  = 'E';
        frame[3]  = 'C';
        frame[4]  = (uint8_t)(ctx->gets_offset);
        frame[5]  = (uint8_t)(ctx->gets_offset >> 8);
        frame[6]  = (uint8_t)(ctx->gets_offset >> 16);
        frame[7]  = (uint8_t)(ctx->gets_offset >> 24);
        frame[8]  = (uint8_t)(got);
        frame[9]  = (uint8_t)(got >> 8);
        frame[10] = (uint8_t)(crc);
        frame[11] = (uint8_t)(crc >> 8);

        if (ctx->ops.send(ctx->user, frame, (uint16_t)(12u + got)) != 0) {
            /* Transport busy/no credits: retry on next poll without advancing offset */
            return;
        }
        ctx->gets_offset += got;
        ctx->gets_rem    -= got;
        if (got < want || ctx->gets_rem == 0u) {
            ctx->gets_active = 0u;
        }
    }
}
