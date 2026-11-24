/* bt_bench: one-shot SD request for unattended phone-mode runs, pure logic (see bt_bench.h).
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "bt_bench.h"

#define NONE 0xFFFFFFFFu

static int is_sep(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';';
}

static uint8_t lower(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

/* Case-insensitive compare of buf[0..n) with a lower-case literal. */
static int word_is(const uint8_t *p, uint32_t n, const char *w)
{
    uint32_t i = 0u;
    for (; i < n && w[i] != '\0'; i++) {
        if (lower(p[i]) != (uint8_t)w[i]) {
            return 0;
        }
    }
    return i == n && w[i] == '\0';
}

/* Decimal 0..65535 without sign, at most 5 digits; -1 if not a number. */
static int32_t number(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0u;
    if (n == 0u || n > 5u) {
        return -1;
    }
    for (uint32_t i = 0u; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return -1;
        }
        v = v * 10u + (uint32_t)(p[i] - '0');
    }
    return v <= 0xFFFFu ? (int32_t)v : -1;
}

void bt_bench_cfg_default(bt_bench_cfg_t *cfg)
{
    cfg->secs = BT_BENCH_SECS_DEF;
    cfg->pair_every = 0u;
    cfg->pair_secs = BT_BENCH_PAIR_SECS_DEF;
    cfg->pair = 0u;
    cfg->autoconfirm = 0u;
    cfg->reboot = 0u;
    for (uint32_t i = 0u; i < sizeof(cfg->xrec_pin); i++) {
        cfg->xrec_pin[i] = '\0';
    }
}

enum { K_SECS = 0, K_PAIR, K_PAIR_EVERY, K_PAIR_SECS, K_AUTOCONFIRM, K_EXIT, K_REBOOT, K_XREC_PIN, K_N };
static const char *const k_keys[K_N] = { "secs", "pair", "pair_every", "pair_secs", "autoconfirm", "exit", "reboot",
                                         "xrec_pin" };

bt_bench_err_t bt_bench_parse(const uint8_t *buf, uint32_t len, bt_bench_cfg_t *cfg, uint32_t *err_at)
{
    uint32_t i = 0u, seen = 0u;
    bt_bench_cfg_t c;
    bt_bench_cfg_default(&c);
    if (err_at != NULL) {
        *err_at = 0u;
    }
    if (buf == NULL) {
        len = 0u;
    }
    if (len >= 3u && buf[0] == 0xEFu && buf[1] == 0xBBu && buf[2] == 0xBFu) {
        i = 3u;                                          /* UTF-8 BOM (Windows editors) */
    }
    while (i < len) {
        uint8_t ch = buf[i];
        if (is_sep(ch)) {
            i++;
            continue;
        }
        if (ch == '#') {                                 /* comment to the end of the line */
            while (i < len && buf[i] != '\n') {
                i++;
            }
            continue;
        }
        uint32_t a = i, eq = NONE;
        while (i < len && !is_sep(buf[i]) && buf[i] != '#') {
            if (buf[i] < 0x20u || buf[i] >= 0x7Fu) {
                if (err_at != NULL) {
                    *err_at = i;
                }
                return BT_BENCH_E_CHAR;
            }
            if (buf[i] == '=' && eq == NONE) {
                eq = i;
            }
            i++;
        }
        if (err_at != NULL) {
            *err_at = a;
        }
        if (eq == NONE) {
            return BT_BENCH_E_KEY;
        }
        uint32_t k = 0u;
        while (k < K_N && !word_is(&buf[a], eq - a, k_keys[k])) {
            k++;
        }
        if (k == K_N) {
            return BT_BENCH_E_KEY;
        }
        if (seen & (1u << k)) {
            return BT_BENCH_E_DUP;
        }
        seen |= 1u << k;
        const uint8_t *v = &buf[eq + 1u];
        uint32_t vn = i - eq - 1u;
        int32_t n = number(v, vn);
        switch (k) {
        case K_SECS:
            if (n < (int32_t)BT_BENCH_SECS_MIN || n > (int32_t)BT_BENCH_SECS_MAX) {
                return BT_BENCH_E_VALUE;
            }
            c.secs = (uint16_t)n;
            break;
        case K_PAIR_EVERY:
            if (n != 0 && (n < (int32_t)BT_BENCH_SECS_MIN || n > (int32_t)BT_BENCH_SECS_MAX)) {
                return BT_BENCH_E_VALUE;
            }
            c.pair_every = (uint16_t)n;
            break;
        case K_PAIR_SECS:
            if (n < (int32_t)BT_BENCH_PAIR_SECS_MIN || n > (int32_t)BT_BENCH_PAIR_SECS_MAX) {
                return BT_BENCH_E_VALUE;
            }
            c.pair_secs = (uint16_t)n;
            break;
        case K_EXIT:
            if (!word_is(v, vn, "auto")) {
                return BT_BENCH_E_VALUE;
            }
            break;
        case K_XREC_PIN:                                 /* exactly 6 digits (leading zeros kept) */
            if (vn != 6u) {
                return BT_BENCH_E_VALUE;
            }
            for (uint32_t j = 0u; j < 6u; j++) {
                if (v[j] < '0' || v[j] > '9') {
                    return BT_BENCH_E_VALUE;
                }
                c.xrec_pin[j] = (char)v[j];
            }
            c.xrec_pin[6] = '\0';
            break;
        default:                                         /* the 0|1 flags */
            if (n != 0 && n != 1) {
                return BT_BENCH_E_VALUE;
            }
            if (k == K_PAIR) {
                c.pair = (uint8_t)n;
            } else if (k == K_AUTOCONFIRM) {
                c.autoconfirm = (uint8_t)n;
            } else {
                c.reboot = (uint8_t)n;
            }
            break;
        }
    }
    if ((c.pair_every != 0u || c.xrec_pin[0] != '\0') && !c.pair) {
        if (err_at != NULL) {
            *err_at = 0u;
        }
        return BT_BENCH_E_CONFLICT;
    }
    if (err_at != NULL) {
        *err_at = 0u;
    }
    if (cfg != NULL) {
        *cfg = c;
    }
    return BT_BENCH_OK;
}

bt_bench_trig_t bt_bench_take(exfat_vol_t *vol, int mounted, bt_bench_cfg_t *cfg, bt_bench_err_t *perr,
                              uint32_t *err_at)
{
    exfat_entry_t ent;
    uint8_t buf[BT_BENCH_TRIGGER_MAX];
    uint32_t got = 0u, at = 0u;
    bt_bench_cfg_t c;
    bt_bench_err_t e = BT_BENCH_E_VALUE;                 /* unreadable / oversized */
    if (perr != NULL) {
        *perr = BT_BENCH_OK;
    }
    if (err_at != NULL) {
        *err_at = 0u;
    }
    if (vol == NULL || !mounted) {
        return BT_BENCH_TRIG_NO_SD;
    }
    if (exfat_find_path(vol, "/" BT_BENCH_TRIGGER_FILE, &ent) != 0) {
        return BT_BENCH_TRIG_ABSENT;
    }
    if (ent.is_dir) {
        return BT_BENCH_TRIG_NOT_FILE;
    }
    if (ent.size <= sizeof(buf) &&
        exfat_read_file(vol, &ent, 0u, buf, (uint32_t)ent.size, &got) == 0 && got == (uint32_t)ent.size) {
        e = bt_bench_parse(buf, got, &c, &at);
    }
    /* Consumed before BT starts: whatever happens next (hang, WDT cold reset, BT fault reboot, reboot=1), the next
     * boot finds no request. A delete that fails or does not stick means it cannot be consumed: do not run. */
    if (exfat_delete_file(vol, "/", BT_BENCH_TRIGGER_FILE) != 0 ||
        exfat_find_path(vol, "/" BT_BENCH_TRIGGER_FILE, &ent) == 0) {
        return BT_BENCH_TRIG_STUCK;
    }
    if (e != BT_BENCH_OK) {
        if (perr != NULL) {
            *perr = e;
        }
        if (err_at != NULL) {
            *err_at = at;
        }
        return BT_BENCH_TRIG_BAD;
    }
    if (cfg != NULL) {
        *cfg = c;
    }
    return BT_BENCH_TRIG_TAKEN;
}

const char *bt_bench_trig_name(bt_bench_trig_t t)
{
    switch (t) {
    case BT_BENCH_TRIG_TAKEN:
        return "taken";
    case BT_BENCH_TRIG_ABSENT:
        return "absent";
    case BT_BENCH_TRIG_NO_SD:
        return "no_sd";
    case BT_BENCH_TRIG_NOT_FILE:
        return "not_file";
    case BT_BENCH_TRIG_STUCK:
        return "stuck";
    case BT_BENCH_TRIG_BAD:
        return "bad_content";
    }
    return "?";
}

const char *bt_bench_err_name(bt_bench_err_t e)
{
    switch (e) {
    case BT_BENCH_OK:
        return "ok";
    case BT_BENCH_E_KEY:
        return "key";
    case BT_BENCH_E_VALUE:
        return "value";
    case BT_BENCH_E_DUP:
        return "dup";
    case BT_BENCH_E_CONFLICT:
        return "conflict";
    case BT_BENCH_E_CHAR:
        return "char";
    }
    return "?";
}

void bt_bench_run_begin(bt_bench_run_t *r, const bt_bench_cfg_t *cfg, uint32_t now)
{
    r->cfg = *cfg;
    r->t0 = now;
    r->next_pair = cfg->pair ? 0u : NONE;
    r->next_status = BT_BENCH_STATUS_MS;
    r->pairs = 0u;
    r->exited = 0u;
}

uint32_t bt_bench_run_tick(bt_bench_run_t *r, uint32_t now)
{
    uint32_t el = now - r->t0;                           /* wrap-safe while the run is shorter than 49 days */
    uint32_t act = 0u;
    if (r->exited || el >= (uint32_t)r->cfg.secs * 1000u) {
        r->exited = 1u;
        return BT_BENCH_ACT_EXIT;
    }
    if (r->next_pair != NONE && el >= r->next_pair) {
        act |= BT_BENCH_ACT_PAIR;
        r->pairs++;
        if (r->cfg.pair_every != 0u) {
            uint32_t per = (uint32_t)r->cfg.pair_every * 1000u;
            while (r->next_pair <= el) {                 /* skip missed periods: one window per call */
                r->next_pair += per;
            }
        } else {
            r->next_pair = NONE;
        }
    }
    if (el >= r->next_status) {
        act |= BT_BENCH_ACT_STATUS;
        while (r->next_status <= el) {
            r->next_status += BT_BENCH_STATUS_MS;
        }
    }
    return act;
}

void bt_bench_cap_init(bt_bench_cap_t *c, char *buf, uint32_t cap, uint32_t widx)
{
    c->buf = buf;
    c->cap = buf != NULL ? cap : 0u;
    c->len = 0u;
    c->lost = 0u;
    c->resets = 0u;
    c->last_widx = widx;
}

void bt_bench_cap_drain(bt_bench_cap_t *c, const char *ring, uint32_t ring_size, uint32_t widx)
{
    if (ring == NULL || ring_size == 0u || (ring_size & (ring_size - 1u)) != 0u) {
        return;
    }
    if (widx < c->last_widx) {                           /* ring reset (openfw_log_reset): restart at its start */
        c->resets++;
        c->last_widx = 0u;
    }
    uint32_t n = widx - c->last_widx;
    uint32_t from = c->last_widx;
    if (n > ring_size) {                                 /* overwritten before this drain */
        c->lost += n - ring_size;
        from = widx - ring_size;
        n = ring_size;
    }
    for (uint32_t i = 0u; i < n; i++) {
        if (c->len < c->cap) {
            c->buf[c->len++] = ring[(from + i) & (ring_size - 1u)];
        } else {
            c->lost += n - i;
            break;
        }
    }
    c->last_widx = widx;
}

int bt_bench_log_save(exfat_vol_t *vol, int mounted, const char *data, uint32_t len, const char *tail,
                      uint32_t tail_len, exfat_writer_t *w)
{
    exfat_entry_t ent;
    if (vol == NULL || !mounted || w == NULL || (data == NULL && len != 0u) || (tail == NULL && tail_len != 0u)) {
        return -1;
    }
    if (exfat_find_path(vol, "/" BT_BENCH_LOG_FILE, &ent) == 0) {
        if (ent.is_dir || exfat_delete_file(vol, "/", BT_BENCH_LOG_FILE) != 0) {
            return -1;                                   /* never replace a directory; keep an undeletable file */
        }
    }
    if (exfat_create_file(vol, "/", BT_BENCH_LOG_FILE, w) != 0) {
        return -1;
    }
    if ((len != 0u && exfat_write(w, data, len) != 0) || (tail_len != 0u && exfat_write(w, tail, tail_len) != 0)) {
        (void)exfat_close_file(w);
        return -1;
    }
    return exfat_close_file(w);
}
