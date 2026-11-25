/*
 * Smartlink SL6801 OpenFW `+XREC v5` Protocol Implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_xrec.h"
#include "openfw_crypto.h"
#include "openfw_bench_meas.h"   /* +XREC:DBG and the GETS byte count: bench images only (OPENFW_BENCH_MEAS) */

typedef uint32_t __attribute__((may_alias)) xrec_u32_t;

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
        *d++ = 0u;
        n--;
    }
    xrec_u32_t *dw = (xrec_u32_t *)d;
    while (n >= 4u) {
        *dw++ = 0u;
        n -= 4u;
    }
    d = (uint8_t *)dw;
    while (n > 0u) {
        *d++ = 0u;
        n--;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
            *d++ = *s++;
            n--;
        }
        xrec_u32_t *dw = (xrec_u32_t *)d;
        const xrec_u32_t *sw = (const xrec_u32_t *)s;
        while (n >= 4u) {
            *dw++ = *sw++;
            n -= 4u;
        }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n > 0u) {
        *d++ = *s++;
        n--;
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

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return (*a == *b) ? 1 : 0;
}

static void copy_cstr(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;
    if (cap == 0u) {
        return;
    }
    while (src && src[i] != '\0' && (i + 1u) < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int parse_u32_step(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    uint32_t v = 0u;
    while (*p >= '0' && *p <= '9') {
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) {
            return -1;
        }
        v = v * 10u + d;
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
        char c_hi = s[i * 2u];
        if (c_hi == '\0') {
            return -1;
        }
        int hi = hex_nibble(c_hi);
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
        uint8_t ch = (uint8_t)s[n];
        if (ch < 0x20u || ch == 0x7Fu ||
            ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"'  || ch == '<' || ch == '>' || ch == '|' ||
            n >= (EXFAT_MAX_NAME - 1u)) {
            return 0;
        }
        n++;
    }
    if (s[n - 1u] == '.' || s[n - 1u] == ' ') {
        return 0;
    }
    return 1;
}

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int str_eq_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower_ascii(*a) != lower_ascii(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* PUT REC,<name>: only a transcript sidecar can be written into /record, never a recording or a hidden file.
 * is_safe_filename, printable ASCII, <= 30 chars (exfat_replace_file limit), not a dot-file, ends in ".cwx". */
static int is_rec_sidecar_name(const char *s)
{
    if (!is_safe_filename(s) || s[0] == '.') {
        return 0;
    }
    uint32_t n = 0u;
    while (s[n] != '\0') {
        uint8_t ch = (uint8_t)s[n];
        if (ch < 0x20u || ch > 0x7Eu || n >= 30u) {
            return 0;
        }
        n++;
    }
    return n > 4u && str_eq_ci(&s[n - 4u], ".cwx");
}

/* Staging location of a streamed PUT target: the temp is a dot-file (hidden from LS and the file list) in the
 * destination folder, committed by an atomic exfat_replace_file (PUTEND / OTA). */
#define XREC_REC_TMP ".xrec_rec.tmp"
static void put_stage_loc(uint8_t mode, const char **dir, const char **tmp)
{
    if (mode == XREC_PUT_CAS) {
        *dir = "/cassini";
        *tmp = ".xrec_cas.tmp";
    } else if (mode == XREC_PUT_REC) {
        *dir = "/record";
        *tmp = XREC_REC_TMP;
    } else {
        *dir = "/";
        *tmp = ".xrec_ota.tmp";
    }
}

static int put_is_named(uint8_t mode)
{
    return mode == XREC_PUT_CAS || mode == XREC_PUT_REC;
}

/* <name> with its 3-letter extension (".wav" / ".cwx", any case) swapped for new_ext; 0 when name has no such
 * extension or does not fit. */
static int swap_ext3(char *out, uint32_t cap, const char *name, const char *old_ext, const char *new_ext)
{
    uint32_t n = 0u;
    while (name[n] != '\0') {
        n++;
    }
    if (n <= 4u || n + 1u > cap || !str_eq_ci(&name[n - 4u], old_ext)) {
        return 0;
    }
    mem_copy(out, name, n - 4u);
    copy_cstr(&out[n - 4u], cap - (n - 4u), new_ext);
    return 1;
}

/* PUT REC only for an existing recording: a sidecar for a missing <stem>.wav would be shown on the next recording
 * that reuses the number. */
static int rec_wav_exists(const xrec_ctx_t *ctx, const char *cwx_name)
{
    char path[48];
    copy_cstr(path, (uint32_t)sizeof(path), "/record/");
    exfat_entry_t e;
    return swap_ext3(&path[8], (uint32_t)sizeof(path) - 8u, cwx_name, ".cwx", ".wav") &&
           exfat_find_path(ctx->vol, path, &e) == 0 && !e.is_dir;
}

typedef struct {
    xrec_ctx_t *ctx;
    char        buf[512];
    int         buf_cap;
    int         pos;
    uint32_t    seen_files;
    uint32_t    batch_files;
    int         busy;
} ls_ctx_t;

static int ls_flush(ls_ctx_t *lc)
{
    if (lc->pos <= 0) {
        return 0;
    }
    if (!lc->ctx->ops.send ||
        lc->ctx->ops.send(lc->ctx->user, (const uint8_t *)lc->buf, (uint16_t)lc->pos) != 0) {
        lc->busy = 1;
        return -1;
    }
    lc->ctx->ls_sent_cnt += lc->batch_files;
    lc->batch_files       = 0u;
    lc->pos               = 0;
    return 0;
}

static int ls_append(ls_ctx_t *lc, const char *line, int len, int is_file_entry)
{
    if (lc->pos > 0 && (lc->pos + len) > lc->buf_cap) {
        if (ls_flush(lc) != 0) {
            return -1;
        }
    }
    for (int i = 0; i < len && lc->pos < (int)sizeof(lc->buf); i++) {
        lc->buf[lc->pos++] = line[i];
    }
    if (is_file_entry) {
        lc->batch_files++;
    }
    return 0;
}

static int ls_dir_cb(const exfat_entry_t *entry, void *user)
{
    ls_ctx_t *lc = (ls_ctx_t *)user;
    /* Hidden files (e.g. a .autoplay folder marker) are not recordings */
    if (entry->is_dir || entry->name[0] == '.') {
        return 0;
    }
    if (lc->seen_files < lc->ctx->ls_sent_cnt) {
        lc->seen_files++;
        return 0;
    }
    lc->seen_files++;
    char line[128];
    int pos = 0;
    uint32_t sz = (entry->size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)entry->size;
    append_str(line, &pos, (int)sizeof(line), "+XREC:F,");
    append_u32(line, &pos, (int)sizeof(line), sz);
    append_str(line, &pos, (int)sizeof(line), ",");
    append_str(line, &pos, (int)sizeof(line), entry->name);
    append_str(line, &pos, (int)sizeof(line), "\r\n");
    if (ls_append(lc, line, pos, 1) != 0) {
        return 1;
    }
    return 0;
}

/* ---- transfer status for the BT screen (xrec_get_xfer) ---- */
static uint32_t xfer_now(const xrec_ctx_t *ctx)
{
    return ctx->ops.now_ms ? ctx->ops.now_ms(ctx->user) : 0u;
}

static int xfer_fresh(const xrec_ctx_t *ctx, uint32_t now)
{
    return ctx->xfer_valid && (!ctx->ops.now_ms || (uint32_t)(now - ctx->xfer_t_ms) < XREC_XFER_HOLD_MS);
}

/* Display copy: at most XREC_XFER_NAME - 1 bytes. A longer name is cut on a UTF-8 character boundary and ends in
 * U+2026 "\xE2\x80\xA6" (in ui_font_sm), so a cut that still fits the label (2-byte scripts) does not pass for the
 * full name; the label's own LONG_DOT handles names that are too wide in pixels. */
static void xfer_copy_name(char *dst, const char *src)
{
    uint32_t n = 0u;
    while (src[n] != '\0' && n < (XREC_XFER_NAME - 1u)) {
        n++;
    }
    if (src[n] == '\0') {
        mem_copy(dst, src, n + 1u);
        return;
    }
    n = XREC_XFER_NAME - 4u;           /* room for the 3-byte ellipsis + NUL */
    while (n > 0u && (((uint8_t)src[n]) & 0xC0u) == 0x80u) {
        n--;                           /* src[n] continues a character: drop its lead byte too */
    }
    mem_copy(dst, src, n);
    dst[n]      = (char)0xE2;
    dst[n + 1u] = (char)0x80;
    dst[n + 2u] = (char)0xA6;
    dst[n + 3u] = '\0';
}

/* A GETS / PUT starts: the same file within the hold time resuming mid-file continues (done never goes back, so a
 * retried window does not flicker); a start at offset 0, another file, or a stale view restarts. done is clamped to
 * total (a GETS at or past EOF moves no byte). */
static void xfer_begin(xrec_ctx_t *ctx, uint8_t upload, const char *name, uint32_t id, uint32_t done, uint32_t total)
{
    uint32_t now = xfer_now(ctx);
    char shown[XREC_XFER_NAME];
    xfer_copy_name(shown, name);
    if (total != 0u && done > total) {
        done = total;
    }
    if (done == 0u || !xfer_fresh(ctx, now) || ctx->xfer_upload != upload || ctx->xfer_id != id ||
        !str_eq(ctx->xfer_name, shown)) {
        ctx->xfer_upload = upload;
        ctx->xfer_id     = id;
        mem_copy(ctx->xfer_name, shown, sizeof(shown));
        ctx->xfer_done   = done;
    } else if (done > ctx->xfer_done) {
        ctx->xfer_done = done;
    }
    ctx->xfer_total = total;
    ctx->xfer_valid = 1u;
    ctx->xfer_t_ms  = now;
}

static void xfer_progress(xrec_ctx_t *ctx, uint8_t upload, uint32_t done)
{
    if (!ctx->xfer_valid || ctx->xfer_upload != upload) {
        return;
    }
    if (done > ctx->xfer_done) {
        ctx->xfer_done = done;
    }
    ctx->xfer_t_ms = xfer_now(ctx);
}

void xrec_get_xfer(const xrec_ctx_t *ctx, uint32_t now_ms, xrec_xfer_t *out)
{
    if (!out) {
        return;
    }
    mem_zero(out, sizeof(*out));
    if (!ctx || !xfer_fresh(ctx, now_ms)) {
        return;
    }
    out->active = 1u;
    out->upload = ctx->xfer_upload;
    mem_copy(out->name, ctx->xfer_name, sizeof(out->name));
    out->done   = ctx->xfer_done;
    out->total  = ctx->xfer_total;
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
    /* capability flags (clients parse key=value and ignore unknown keys): rec=1 = PUT target REC,<stem>.cwx */
    append_str(buf, &pos, (int)sizeof(buf), ",rec=1\r\n");
    send_text(ctx, buf, pos);
}

static void handle_pair(xrec_ctx_t *ctx, const char *args)
{
    if (!ctx->pair_active) {
        send_err(ctx, "PAIRCLOSED");
        return;
    }
    /* Expect <6-digit-pin>,<64-hex-key> without reading past NUL */
    uint32_t alen = 0u;
    while (args[alen] != '\0' && alen < 7u) {
        alen++;
    }
    int pin_ok = (alen == 7u && args[6] == ',') ? 1 : 0;
    if (pin_ok) {
        uint8_t diff = 0u;
        for (int i = 0; i < 6; i++) {
            if (args[i] < '0' || args[i] > '9') {
                pin_ok = 0;
            }
            diff |= (uint8_t)(args[i] ^ ctx->pair_pin[i]);
        }
        if (diff != 0u) {
            pin_ok = 0;
        }
    }
    if (!pin_ok) {
        ctx->pair_fails++;
        if (ctx->pair_fails >= 5u) {
            xrec_close_pairing(ctx);
        }
        send_err(ctx, "PIN");
        return;
    }
    uint8_t new_key[32];
    if (decode_hex(args + 7, new_key, 32u) != 0) {
        send_err(ctx, "BADKEY");
        return;
    }
    /* Saved first: a key that only lived in RAM worked for this session, then the old key came back from the card at the
     * next start while the phone had been told OK (review STEP10 C9). Not saved: ERR, the old binding stays and the
     * window stays open for a retry. */
    if (ctx->ops.on_bound_key && ctx->ops.on_bound_key(ctx->user, new_key) != 0) {
        mem_zero(new_key, sizeof(new_key));
        send_err(ctx, "SAVE");
        return;
    }
    mem_copy(ctx->key, new_key, 32u);
    mem_zero(new_key, sizeof(new_key));
    ctx->bound      = 1u;
    ctx->auth       = 1u;
    ctx->pair_fails = 0u;
    xrec_close_pairing(ctx);
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
    static const uint8_t mdays[12] = { 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };
    uint32_t max_d = mdays[m - 1u];
    if (m == 2u && (y % 4u == 0u) && ((y % 100u != 0u) || (y % 400u == 0u))) {
        max_d = 29u;
    }
    if (d > max_d) {
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

static void xrec_poll_ls(xrec_ctx_t *ctx)
{
    if (!ctx || !ctx->ls_active || !ctx->ops.send) {
        return;
    }
    ls_ctx_t lc;
    lc.ctx         = ctx;
    lc.buf_cap     = (int)sizeof(lc.buf);
    if (ctx->ops.get_mtu) {
        uint16_t mtu = ctx->ops.get_mtu(ctx->user);
        if (mtu >= 64u && (int)mtu < lc.buf_cap) {
            lc.buf_cap = (int)mtu;
        }
    }
    lc.pos         = 0;
    lc.seen_files  = 0u;
    lc.batch_files = 0u;
    lc.busy        = 0;

    if (ctx->ls_has_dir) {
        int rc = exfat_list_dir_entry(ctx->vol, &ctx->ls_dir, ls_dir_cb, &lc);
        if (lc.busy) {
            return; /* Retry on next xrec_poll() once RFCOMM credits/queue free up */
        }
        if (rc != 0) {
            ctx->ls_active = 0u;
            send_err(ctx, "SDIO");
            return;
        }
    }

    static const char end[] = "+XREC:END\r\n";
    if (ls_append(&lc, end, (int)(sizeof(end) - 1u), 0) != 0 || ls_flush(&lc) != 0) {
        return; /* Retry sending +XREC:END on next poll */
    }
    ctx->ls_active = 0u;
}

static void handle_ls(xrec_ctx_t *ctx)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    /* Stop any active GETS stream so binary frames never interleave with LS text */
    ctx->gets_active = 0u;
    ctx->gets_staged = 0u;
    ctx->ls_active   = 1u;
    ctx->ls_sent_cnt = 0u;
    ctx->ls_has_dir  = 0u;

    exfat_entry_t rec_dir;
    if (exfat_find_path(ctx->vol, "/record", &rec_dir) == 0 && rec_dir.is_dir) {
        ctx->ls_dir     = rec_dir;
        ctx->ls_has_dir = 1u;
    }
    xrec_poll_ls(ctx);
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

    if (ctx->ops.get_mtu) {
        uint16_t mtu = ctx->ops.get_mtu(ctx->user);
        if (mtu > 12u) {
            uint32_t max_payload = (uint32_t)(mtu - 12u);
            if (chunk > max_payload) {
                chunk = max_payload;
            }
        } else if (mtu > 0u) {
            chunk = 1u;
        }
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

    /* Stop any active LS stream so text lines never interleave with binary GETS frames */
    ctx->ls_active      = 0u;
    if ((uint64_t)off < ctx->gets_file.size) {   /* at/past EOF (a resume probe) no byte moves: no transfer view */
        xfer_begin(ctx, 0u, ctx->gets_file.name, ctx->gets_file.first_cluster, off,
                   (ctx->gets_file.size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)ctx->gets_file.size);
    }
    ctx->gets_offset    = off;
    ctx->gets_rem       = tot;
    ctx->gets_chunk_max = (uint16_t)chunk;
    ctx->gets_staged    = 0u;
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
    /* the file being recorded: deleting it frees clusters the writer still fills (review STEP10 A1); likewise the
     * staging temp of a PUT REC upload in progress (its writer is open) */
    if ((ctx->ops.file_busy && ctx->ops.file_busy(ctx->user, filename) != 0) ||
        (ctx->put_stream_mode == XREC_PUT_REC && str_eq_ci(filename, XREC_REC_TMP))) {
        send_err(ctx, "BUSY");
        return;
    }
    if (exfat_delete_file(ctx->vol, "/record", filename) != 0) {
        send_err(ctx, "DEL");
        return;
    }
    /* a deleted recording takes its transcript sidecar with it (as the on-device delete does): its number is reused
     * by the next recording, which must not inherit the old transcript */
    char cwx_name[80];
    if (swap_ext3(cwx_name, (uint32_t)sizeof(cwx_name), filename, ".wav", ".cwx")) {
        (void)exfat_delete_file(ctx->vol, "/record", cwx_name);
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

static void abort_staged_put(xrec_ctx_t *ctx)
{
    if (!ctx || ctx->put_stream_mode == XREC_PUT_NONE) {
        return;
    }
    uint8_t mode = ctx->put_stream_mode;
    (void)exfat_close_file(&ctx->put_writer);
    if (ctx->vol && (mode == XREC_PUT_CAS || mode == XREC_PUT_REC || mode == XREC_PUT_OTA)) {
        const char *dir, *tmp;
        put_stage_loc(mode, &dir, &tmp);
        (void)exfat_delete_file(ctx->vol, dir, tmp);
    }
    ctx->put_stream_mode    = XREC_PUT_NONE;
    ctx->put_has_last       = 0u;
    ctx->put_written        = 0u;
    ctx->put_stream_name[0] = '\0';
}

static void handle_put_complete(xrec_ctx_t *ctx)
{
    if (!ctx->put_auth_ok || !ctx->auth) {
        send_err(ctx, "LOCKED");
        return;
    }

    uint16_t crc = openfw_crc16_ccitt_false(ctx->xfer.put_buf, ctx->put_len);
    if (crc != ctx->put_crc) {
        send_err(ctx, "CRC");
        return;
    }

    if (ctx->put_target == XREC_PUT_EXP) {
        if (!ctx->ops.exp_write) {
            send_err(ctx, "UNSUP");
            return;
        }
        if (ctx->ops.exp_write(ctx->user, ctx->put_offset, ctx->xfer.put_buf, ctx->put_len) != 0) {
            send_err(ctx, "ARG");
            return;
        }
        send_put_ok(ctx, ctx->put_offset, ctx->put_len);
        return;
    }

    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }

    if (ctx->put_stream_mode == ctx->put_target && ctx->put_has_last &&
        ctx->put_last_mode == ctx->put_target &&
        ctx->put_offset == ctx->put_last_off &&
        ctx->put_len == ctx->put_last_len &&
        ctx->put_crc == ctx->put_last_crc &&
        (!put_is_named(ctx->put_target) || str_eq(ctx->put_name, ctx->put_stream_name))) {
        xfer_progress(ctx, 1u, ctx->put_written);   /* resent chunk (lost reply): the upload is alive */
        send_put_ok(ctx, ctx->put_offset, ctx->put_len);
        return;
    }

    if (ctx->put_offset == 0u) {
        if (ctx->put_stream_mode != XREC_PUT_NONE) {
            abort_staged_put(ctx);
        }
        if (ctx->put_target == XREC_PUT_REC && !rec_wav_exists(ctx, ctx->put_name)) {
            send_err(ctx, "NOENT");
            return;
        }

        const char *dir;
        const char *tmp_name;
        const char *fname = put_is_named(ctx->put_target) ? ctx->put_name : "update.up";
        put_stage_loc(ctx->put_target, &dir, &tmp_name);

        if (put_is_named(ctx->put_target) && exfat_mkdir(ctx->vol, dir) != 0) {
            send_err(ctx, "SDIO");
            return;
        }

        /* Stage under temporary name so interrupted uploads never clobber existing files */
        (void)exfat_delete_file(ctx->vol, dir, tmp_name);
        if (exfat_create_file(ctx->vol, dir, tmp_name, &ctx->put_writer) != 0) {
            send_err(ctx, "SDIO");
            return;
        }
        ctx->put_stream_mode = ctx->put_target;
        copy_cstr(ctx->put_stream_name, (uint32_t)sizeof(ctx->put_stream_name), fname);
        ctx->put_written  = 0u;
        ctx->put_has_last = 0u;
        xfer_begin(ctx, 1u, fname, 0u, 0u, 0u);
    } else if (ctx->put_stream_mode != ctx->put_target ||
               ctx->put_offset != ctx->put_written ||
               (put_is_named(ctx->put_target) && !str_eq(ctx->put_name, ctx->put_stream_name))) {
        send_err(ctx, "OFF");
        return;
    }

    if (ctx->put_written > (0xFFFFFFFFu - (uint32_t)ctx->put_len)) {
        abort_staged_put(ctx);
        send_err(ctx, "OFF");
        return;
    }

    /* REC / CAS: publish the temp's cluster chain after every chunk, so a temp left by a power loss or reset is a
     * normal file whose clusters the next upload's delete frees (else they stay marked used until fsck). OTA images
     * are large and skip it. */
    if (exfat_write(&ctx->put_writer, ctx->xfer.put_buf, ctx->put_len) != 0 ||
        (put_is_named(ctx->put_target) && exfat_checkpoint_file(&ctx->put_writer, 0, 0u) != 0)) {
        abort_staged_put(ctx);
        send_err(ctx, "SDIO");
        return;
    }

    ctx->put_written   += ctx->put_len;
    ctx->put_last_off   = ctx->put_offset;
    ctx->put_last_len   = ctx->put_len;
    ctx->put_last_crc   = ctx->put_crc;
    ctx->put_last_mode  = ctx->put_target;
    ctx->put_has_last   = 1u;
    xfer_progress(ctx, 1u, ctx->put_written);
    send_put_ok(ctx, ctx->put_offset, ctx->put_len);
}

static void send_putend_ok(xrec_ctx_t *ctx, uint32_t total)
{
    char resp[64];
    int pos = 0;
    append_str(resp, &pos, (int)sizeof(resp), "+XREC:OK,PUTEND,");
    append_u32(resp, &pos, (int)sizeof(resp), total);
    append_str(resp, &pos, (int)sizeof(resp), "\r\n");
    send_text(ctx, resp, pos);
}

static void handle_putend(xrec_ctx_t *ctx)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    if (!put_is_named(ctx->put_stream_mode)) {
        if (ctx->put_done_valid) {
            /* repeated PUTEND after a lost reply: the commit already happened, answer it again */
            send_putend_ok(ctx, ctx->put_done_total);
            return;
        }
        send_err(ctx, "NOPUT");
        return;
    }

    const char *dir, *tmp;
    uint8_t mode = ctx->put_stream_mode;
    put_stage_loc(mode, &dir, &tmp);
    uint32_t total = ctx->put_written;
    char final_name[32];
    copy_cstr(final_name, (uint32_t)sizeof(final_name), ctx->put_stream_name);
    int rc = exfat_close_file(&ctx->put_writer);
    ctx->put_stream_mode    = XREC_PUT_NONE;
    ctx->put_has_last       = 0u;
    ctx->put_written        = 0u;
    ctx->put_stream_name[0] = '\0';
    if (rc == 0 && mode == XREC_PUT_REC && !rec_wav_exists(ctx, final_name)) {
        /* the recording was deleted during the upload: never leave an orphan sidecar */
        (void)exfat_delete_file(ctx->vol, dir, tmp);
        send_err(ctx, "NOENT");
        return;
    }
    if (rc != 0 || exfat_replace_file(ctx->vol, dir, tmp, final_name) != 0) {
        (void)exfat_delete_file(ctx->vol, dir, tmp);
        send_err(ctx, "SDIO");
        return;
    }
    ctx->put_done_valid = 1u;
    ctx->put_done_total = total;
    send_putend_ok(ctx, total);
}

static void handle_ota(xrec_ctx_t *ctx)
{
    if (!ctx->vol) {
        send_err(ctx, "NOSD");
        return;
    }
    if (ctx->put_stream_mode != XREC_PUT_OTA || ctx->put_written < 64u) {
        send_err(ctx, "NOOTA");
        return;
    }

    int rc = exfat_close_file(&ctx->put_writer);
    ctx->put_stream_mode    = XREC_PUT_NONE;
    ctx->put_has_last       = 0u;
    ctx->put_written        = 0u;
    ctx->put_stream_name[0] = '\0';
    if (rc != 0 || exfat_replace_file(ctx->vol, "/", ".xrec_ota.tmp", "update.up") != 0) {
        (void)exfat_delete_file(ctx->vol, "/", ".xrec_ota.tmp");
        send_err(ctx, "SDIO");
        return;
    }

    static const char ok[] = "+XREC:OK,OTA\r\n";
    send_text(ctx, ok, (int)(sizeof(ok) - 1u));
    if (ctx->ops.on_ota) {
        ctx->ops.on_ota(ctx->user);
    }
}

static void handle_run(xrec_ctx_t *ctx, const char *args)
{
    if (!ctx->ops.exp_run) {
        send_err(ctx, "UNSUP");
        return;
    }
    uint32_t entry_off = 0u;
    uint32_t arg       = 0u;
    if (*args == ',') {
        const char *p = args + 1;
        if (parse_u32_step(&p, &entry_off) != 0) {
            send_err(ctx, "ARGS");
            return;
        }
        if (*p == ',') {
            p++;
            if (parse_u32_step(&p, &arg) != 0 || *p != '\0') {
                send_err(ctx, "ARGS");
                return;
            }
        } else if (*p != '\0') {
            send_err(ctx, "ARGS");
            return;
        }
    } else if (*args != '\0') {
        send_err(ctx, "ARGS");
        return;
    }

    uint32_t rc = 0u, elapsed_ms = 0u, out_len = 0u, log_widx = 0u;
    if (ctx->ops.exp_run(ctx->user, entry_off, arg, &rc, &elapsed_ms, &out_len, &log_widx) != 0) {
        send_err(ctx, "RUN");
        return;
    }

    char resp[96];
    int pos = 0;
    append_str(resp, &pos, (int)sizeof(resp), "+XREC:OK,RUN,");
    append_u32(resp, &pos, (int)sizeof(resp), rc);
    append_str(resp, &pos, (int)sizeof(resp), ",");
    append_u32(resp, &pos, (int)sizeof(resp), elapsed_ms);
    append_str(resp, &pos, (int)sizeof(resp), ",");
    append_u32(resp, &pos, (int)sizeof(resp), out_len);
    append_str(resp, &pos, (int)sizeof(resp), ",");
    append_u32(resp, &pos, (int)sizeof(resp), log_widx);
    append_str(resp, &pos, (int)sizeof(resp), "\r\n");
    send_text(ctx, resp, pos);
}

static void handle_audio_cfg_cmd(xrec_ctx_t *ctx, const char *tag,
                                 const char *rest, uint32_t field_idx,
                                 uint32_t min_v, uint32_t max_v)
{
    if (!ctx->ops.audio_cfg) {
        send_err(ctx, "UNSUP");
        return;
    }

    uint8_t cfg[4] = { 0u, 20u, 1u, 0u };
    if (ctx->ops.audio_cfg(ctx->user, 0, &cfg[0], &cfg[1], &cfg[2], &cfg[3]) != 0) {
        send_err(ctx, "FAIL");
        return;
    }

    if (*rest != '\0') {
        if (*rest != '=' && *rest != ',') {
            send_err(ctx, "ARGS");
            return;
        }
        const char *p = rest + 1;
        uint32_t val  = 0u;
        if (parse_u32_step(&p, &val) != 0 || *p != '\0' || val < min_v || val > max_v) {
            send_err(ctx, "ARGS");
            return;
        }
        cfg[field_idx] = (uint8_t)val;
        if (ctx->ops.audio_cfg(ctx->user, 1, &cfg[0], &cfg[1], &cfg[2], &cfg[3]) != 0) {
            send_err(ctx, "ARGS");
            return;
        }
    }

    char resp[48];
    int pos = 0;
    append_str(resp, &pos, (int)sizeof(resp), "+XREC:OK,");
    append_str(resp, &pos, (int)sizeof(resp), tag);
    append_str(resp, &pos, (int)sizeof(resp), ",");
    append_u32(resp, &pos, (int)sizeof(resp), cfg[field_idx]);
    append_str(resp, &pos, (int)sizeof(resp), "\r\n");
    send_text(ctx, resp, pos);
}

static void handle_log_cmd(xrec_ctx_t *ctx, const char *args)
{
    if (str_eq(args, ",CLEAR")) {
        if (!ctx->ops.reset_log) {
            send_err(ctx, "UNSUP");
            return;
        }
        ctx->ops.reset_log(ctx->user);
        static const char ok[] = "+XREC:OK,LOG,CLEAR\r\n";
        send_text(ctx, ok, (int)(sizeof(ok) - 1u));
        return;
    }

    if (!ctx->ops.read_log) {
        send_err(ctx, "UNSUP");
        return;
    }

    uint32_t off = 0u;
    uint32_t req = 512u;
    if (*args == ',') {
        const char *p = args + 1;
        if (parse_u32_step(&p, &off) != 0) {
            send_err(ctx, "ARGS");
            return;
        }
        if (*p == ',') {
            p++;
            if (parse_u32_step(&p, &req) != 0 || *p != '\0') {
                send_err(ctx, "ARGS");
                return;
            }
        } else if (*p != '\0') {
            send_err(ctx, "ARGS");
            return;
        }
    } else if (*args != '\0') {
        send_err(ctx, "ARGS");
        return;
    }

    if (req == 0u) {
        send_err(ctx, "ARGS");
        return;
    }
    if (req > 512u) {
        req = 512u;
    }

    /* Read log payload directly into &frame[64] and prepend header in-place (no tmp[512]) */
    uint8_t frame[64u + 512u];
    uint32_t widx   = 0u;
    uint32_t actual = ctx->ops.read_log(ctx->user, off, &frame[64], req, &widx);
    if (actual > 512u) {
        actual = 512u;
    }

    char hdr[64];
    int hlen = 0;
    append_str(hdr, &hlen, (int)sizeof(hdr), "+XREC:LOG,");
    append_u32(hdr, &hlen, (int)sizeof(hdr), off);
    append_str(hdr, &hlen, (int)sizeof(hdr), ",");
    append_u32(hdr, &hlen, (int)sizeof(hdr), actual);
    append_str(hdr, &hlen, (int)sizeof(hdr), ",");
    append_u32(hdr, &hlen, (int)sizeof(hdr), widx);
    append_str(hdr, &hlen, (int)sizeof(hdr), ":");

    uint8_t *start = &frame[64u - (uint32_t)hlen];
    mem_copy(start, hdr, (uint32_t)hlen);
    if (ctx->ops.send) {
        (void)ctx->ops.send(ctx->user, start, (uint16_t)((uint32_t)hlen + actual));
    }
}

#if OPENFW_BENCH_MEAS
/* +XREC:DBG (bench images only; docs/XREC_PROTOCOL.md): the F0 measurement counters (include/openfw_bench_meas.h).
 *   +XREC:DBG        -> +XREC:DBG,LIVE,<rec fields>,<cpu fields>   the running (else the last) recording
 *   +XREC:DBG,REC,i  -> +XREC:DBG,REC,i,<rec fields>               i-th latest finished recording (0 = latest)
 *                       +XREC:ERR,NOENT when there is none (BENCH_MEAS_HIST are kept) */
static void handle_dbg(xrec_ctx_t *ctx, const char *args)
{
    char line[24u + BENCH_MEAS_LINE_MAX];
    int pos = 0;
    const bench_meas_rec_t *r = &g_bench_meas.cur;
    if (*args == '\0') {
        append_str(line, &pos, (int)sizeof(line), "+XREC:DBG,LIVE,");
    } else if (str_starts_with(args, ",REC,")) {
        const char *p = args + 5;
        uint32_t i = 0u;
        if (parse_u32_step(&p, &i) != 0 || *p != '\0') {
            send_err(ctx, "ARGS");
            return;
        }
        r = bench_meas_hist(&g_bench_meas, i);
        if (r == 0) {
            send_err(ctx, "NOENT");
            return;
        }
        append_str(line, &pos, (int)sizeof(line), "+XREC:DBG,REC,");
        append_u32(line, &pos, (int)sizeof(line), i);
        append_str(line, &pos, (int)sizeof(line), ",");
    } else {
        send_err(ctx, "ARGS");
        return;
    }
    /* room for ",<cpu fields>\r\n" is kept by BENCH_MEAS_LINE_MAX (rec 432 B + cpu 107 B worst case) */
    pos += (int)bench_meas_format_rec(r, ',', &line[pos], (uint32_t)((int)sizeof(line) - pos - 2));
    if (*args == '\0') {
        append_str(line, &pos, (int)sizeof(line) - 2, ",");
        pos += (int)bench_meas_format_cpu(&g_bench_meas, ',', &line[pos], (uint32_t)((int)sizeof(line) - pos - 2));
    }
    line[pos++] = '\r';
    line[pos++] = '\n';
    send_text(ctx, line, pos);
}
#endif

/* A refused PUT header whose length is known: reply the error now and eat exactly that many payload bytes without
 * parsing them, so binary payload never reaches the line parser (where a "\r+XREC:..." piece would run as a
 * command and long pieces would raise unsolicited ERR,LINE replies). */
static void put_refuse(xrec_ctx_t *ctx, uint32_t chunk_len, const char *err)
{
    send_err(ctx, err);
    ctx->put_active     = 1u;
    ctx->put_discard    = 1u;
    ctx->put_done_valid = 0u;
    ctx->put_len     = (uint16_t)chunk_len;
    ctx->put_got     = 0u;
}

static void try_start_put_header(xrec_ctx_t *ctx)
{
    ctx->rx_line[ctx->rx_len] = '\0';
    ctx->rx_len = 0u;

    const char *p = ctx->rx_line + 10; /* after "+XREC:PUT," */
    uint32_t off = 0u, chunk_len = 0u, crc = 0u;
    /* offset and length first: once the payload length is known every later refusal discards the payload */
    if (parse_u32_step(&p, &off) != 0 || *p != ',') {
        send_err(ctx, "ARG");
        return;
    }
    p++;
    if (parse_u32_step(&p, &chunk_len) != 0 || *p != ',' || chunk_len == 0u || chunk_len > XREC_MAX_CHUNK) {
        send_err(ctx, "ARG");
        return;
    }
    p++;
    if (parse_u32_step(&p, &crc) != 0 || *p != ',' || crc > 0xFFFFu) {
        put_refuse(ctx, chunk_len, "ARG");
        return;
    }
    p++;

    uint8_t target = XREC_PUT_NONE;
    char tgt_name[32];
    tgt_name[0] = '\0';
    if (str_eq(p, "OTA")) {
        target = XREC_PUT_OTA;
    } else if (str_eq(p, "EXP")) {
        target = XREC_PUT_EXP;
    } else if (str_starts_with(p, "CAS,")) {
        const char *fname = p + 4;
        uint32_t flen = 0u;
        while (fname[flen] != '\0') {
            flen++;
        }
        if (!is_safe_filename(fname) || flen > 30u) {
            put_refuse(ctx, chunk_len, "NAME");
            return;
        }
        target = XREC_PUT_CAS;
        copy_cstr(tgt_name, (uint32_t)sizeof(tgt_name), fname);
    } else if (str_starts_with(p, "REC,")) {
        /* transcript sidecar next to its recording: only <stem>.cwx, so a recording can never be overwritten */
        if (!is_rec_sidecar_name(p + 4)) {
            put_refuse(ctx, chunk_len, "NAME");
            return;
        }
        target = XREC_PUT_REC;
        copy_cstr(tgt_name, (uint32_t)sizeof(tgt_name), p + 4);
    } else {
        put_refuse(ctx, chunk_len, "ARG");
        return;
    }

    /* PUT binary buffer shares union storage with GETS staged frame */
    ctx->gets_active    = 0u;
    ctx->gets_staged    = 0u;
    ctx->put_active     = 1u;
    ctx->put_discard    = 0u;
    ctx->put_done_valid = 0u;
    ctx->put_auth_ok    = ctx->auth;
    ctx->put_target     = target;
    copy_cstr(ctx->put_name, (uint32_t)sizeof(ctx->put_name), tgt_name);
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
    } else if (str_starts_with(cmd, "DFU") && cmd[3] == '\0') {
        static const char ok[] = "+XREC:OK,DFU\r\n";
        send_text(ctx, ok, (int)(sizeof(ok) - 1u));
        if (ctx->ops.on_dfu) {
            ctx->ops.on_dfu(ctx->user);
        }
    } else if (str_starts_with(cmd, "OTA") && cmd[3] == '\0') {
        handle_ota(ctx);
    } else if (str_starts_with(cmd, "PUTEND") && cmd[6] == '\0') {
        handle_putend(ctx);
    } else if (str_starts_with(cmd, "RUN") && (cmd[3] == '\0' || cmd[3] == ',')) {
        handle_run(ctx, cmd + 3);
    } else if (str_starts_with(cmd, "LOG") && (cmd[3] == '\0' || cmd[3] == ',')) {
        handle_log_cmd(ctx, cmd + 3);
    } else if (str_starts_with(cmd, "DSP") && (cmd[3] == '\0' || cmd[3] == '=' || cmd[3] == ',')) {
        handle_audio_cfg_cmd(ctx, "DSP", cmd + 3, 0u, 0u, 2u);
    } else if (str_starts_with(cmd, "GAIN") && (cmd[4] == '\0' || cmd[4] == '=' || cmd[4] == ',')) {
        handle_audio_cfg_cmd(ctx, "GAIN", cmd + 4, 1u, 0u, 31u);
    } else if (str_starts_with(cmd, "CH") && (cmd[2] == '\0' || cmd[2] == '=' || cmd[2] == ',')) {
        handle_audio_cfg_cmd(ctx, "CH", cmd + 2, 2u, 1u, 2u);
    } else if (str_starts_with(cmd, "VOR") && (cmd[3] == '\0' || cmd[3] == '=' || cmd[3] == ',')) {
        handle_audio_cfg_cmd(ctx, "VOR", cmd + 3, 3u, 0u, 1u);
#if OPENFW_BENCH_MEAS
    } else if (str_starts_with(cmd, "DBG") && (cmd[3] == '\0' || cmd[3] == ',')) {
        handle_dbg(ctx, cmd + 3);
#endif
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
        mem_zero(ctx->pair_pin, sizeof(ctx->pair_pin));
    }
}

void xrec_on_disconnect(xrec_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->put_stream_mode != XREC_PUT_NONE) {
        abort_staged_put(ctx);
    }
    ctx->auth               = 0u;
    ctx->chal_active        = 0u;
    ctx->rx_len             = 0u;
    ctx->rx_overflow        = 0u;
    ctx->gets_active        = 0u;
    ctx->gets_staged        = 0u;
    ctx->ls_active          = 0u;
    ctx->ls_has_dir         = 0u;
    ctx->ls_sent_cnt        = 0u;
    ctx->put_active         = 0u;
    ctx->put_discard        = 0u;
    ctx->put_done_valid     = 0u;
    ctx->put_target         = XREC_PUT_NONE;
    ctx->put_name[0]        = '\0';
    ctx->put_got            = 0u;
    ctx->put_stream_mode    = XREC_PUT_NONE;
    ctx->put_has_last       = 0u;
    ctx->put_last_mode      = XREC_PUT_NONE;
    ctx->put_stream_name[0] = '\0';
    ctx->put_written        = 0u;
    ctx->xfer_valid         = 0u;
}

/* Flow control (review STEP10 A5): room for the worst-case reply before running anything that replies. */
static int tx_room(const xrec_ctx_t *ctx)
{
    return !ctx->ops.tx_free || ctx->ops.tx_free(ctx->user) >= XREC_REPLY_MAX;
}

uint16_t xrec_on_rx(xrec_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    if (!ctx || !data) {
        return len;
    }
    uint16_t i = 0u;
    while (i < len) {
        if (ctx->put_active && ctx->put_discard) {
            uint16_t need  = (uint16_t)(ctx->put_len - ctx->put_got);
            uint16_t avail = (uint16_t)(len - i);
            uint16_t take  = (avail < need) ? avail : need;
            ctx->put_got = (uint16_t)(ctx->put_got + take);
            i            = (uint16_t)(i + take);
            if (ctx->put_got >= ctx->put_len) {
                ctx->put_active  = 0u;
                ctx->put_discard = 0u;
            }
            continue;
        }
        if (ctx->put_active) {
            uint16_t need  = (uint16_t)(ctx->put_len - ctx->put_got);
            uint16_t avail = (uint16_t)(len - i);
            uint16_t take  = (avail < need) ? avail : need;
            if (take == need && !tx_room(ctx)) {
                take = (uint16_t)(need - 1u);   /* hold the byte that completes the chunk (it sends the PUT reply) */
                if (take == 0u) {
                    return i;
                }
            }
            mem_copy(&ctx->xfer.put_buf[ctx->put_got], &data[i], take);
            ctx->put_got = (uint16_t)(ctx->put_got + take);
            i            = (uint16_t)(i + take);
            if (ctx->put_got >= ctx->put_len) {
                ctx->put_active = 0u;
                handle_put_complete(ctx);
            }
            continue;
        }
        char c = (char)data[i];
        if (((c == '\r' && (ctx->rx_len > 0u || ctx->rx_overflow)) ||
             (c == ':' && !ctx->rx_overflow && ctx->rx_len >= 14u && str_starts_with(ctx->rx_line, "+XREC:PUT,"))) &&
            !tx_room(ctx)) {
            return i;                           /* not consumed: offered again once the TX side drained */
        }
        i++;
        if (c == '\n') {
            continue;
        }
        if (c == '\r') {
            if (ctx->rx_overflow) {
                ctx->rx_overflow = 0u;
                ctx->rx_len      = 0u;
                send_err(ctx, "LINE");
                continue;
            }
            ctx->rx_line[ctx->rx_len] = '\0';
            if (ctx->rx_len > 0u) {
                process_line(ctx, ctx->rx_line);
            }
            ctx->rx_len = 0u;
            continue;
        }
        if (ctx->rx_overflow) {
            continue;
        }
        if (c == ':' && ctx->rx_len >= 14u && str_starts_with(ctx->rx_line, "+XREC:PUT,")) {
            try_start_put_header(ctx);
            continue;
        }
        if (ctx->rx_len < (XREC_RX_LINE_MAX - 1u)) {
            ctx->rx_line[ctx->rx_len++] = c;
        } else {
            ctx->rx_overflow = 1u;
        }
    }
    return i;
}

void xrec_poll(xrec_ctx_t *ctx)
{
    if (!ctx || !ctx->ops.send) {
        return;
    }
    if (ctx->ls_active) {
        xrec_poll_ls(ctx);
        return;
    }
    if (!ctx->gets_active || ctx->put_active) {
        return;
    }

    while (ctx->gets_active && ctx->gets_rem > 0u) {
        if (!ctx->gets_staged) {
            uint32_t want = (ctx->gets_rem < ctx->gets_chunk_max) ? ctx->gets_rem : ctx->gets_chunk_max;
            uint32_t max_off_rem = 0xFFFFFFFFu - ctx->gets_offset;
            if (want > max_off_rem) {
                want = max_off_rem;
            }
            uint32_t got = 0u;
            if (want > 0u &&
                exfat_read_file(ctx->vol, &ctx->gets_file, (uint64_t)ctx->gets_offset,
                                &ctx->xfer.gets_frame[12], want, &got) != 0) {
                ctx->gets_active = 0u;
                ctx->gets_staged = 0u;
                send_err(ctx, "SDIO");
                return;
            }

            uint16_t crc = openfw_crc16_ccitt_false(&ctx->xfer.gets_frame[12], got);
            ctx->xfer.gets_frame[0]  = 'X';
            ctx->xfer.gets_frame[1]  = 'R';
            ctx->xfer.gets_frame[2]  = 'E';
            ctx->xfer.gets_frame[3]  = 'C';
            ctx->xfer.gets_frame[4]  = (uint8_t)(ctx->gets_offset);
            ctx->xfer.gets_frame[5]  = (uint8_t)(ctx->gets_offset >> 8);
            ctx->xfer.gets_frame[6]  = (uint8_t)(ctx->gets_offset >> 16);
            ctx->xfer.gets_frame[7]  = (uint8_t)(ctx->gets_offset >> 24);
            ctx->xfer.gets_frame[8]  = (uint8_t)(got);
            ctx->xfer.gets_frame[9]  = (uint8_t)(got >> 8);
            ctx->xfer.gets_frame[10] = (uint8_t)(crc);
            ctx->xfer.gets_frame[11] = (uint8_t)(crc >> 8);

            ctx->gets_staged     = 1u;
            ctx->gets_staged_len = (uint16_t)got;
            ctx->gets_staged_eof = (uint8_t)((got < want || got >= ctx->gets_rem ||
                                              got >= max_off_rem) ? 1u : 0u);
        }

        if (ctx->ops.send(ctx->user, ctx->xfer.gets_frame,
                          (uint16_t)(12u + ctx->gets_staged_len)) != 0) {
            /* Transport busy/no credits: retain staged frame and retry on next poll */
            return;
        }
        uint32_t sent = ctx->gets_staged_len;
        uint8_t  eof  = ctx->gets_staged_eof;
        ctx->gets_staged  = 0u;
        ctx->gets_offset += sent;
        ctx->gets_rem    -= sent;
#if OPENFW_BENCH_MEAS
        bench_meas_gets(&g_bench_meas, sent);
#endif
        if (sent > 0u) {   /* an empty EOF frame (GETS at/past the end) began no view: leave the shown one alone */
            xfer_progress(ctx, 0u, ctx->gets_offset);
        }
        if (eof || ctx->gets_rem == 0u) {
            ctx->gets_active = 0u;
        }
    }
}
