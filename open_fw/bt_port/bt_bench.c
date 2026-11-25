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
    cfg->ui = 1u;
    cfg->smp = 1u;
    cfg->coalesce = 0u;
    cfg->spp_wr = BT_BENCH_SPP_WR_DEF;
    cfg->spp_buf = BT_BENCH_SPP_BUF_DEF;
    cfg->tx_ring = BT_BENCH_TX_RING_DEF;
    cfg->stat_secs = BT_BENCH_STATUS_MS / 1000u;
    cfg->mode = BT_BENCH_MODE_PHONE;
    cfg->hp_stage = BT_BENCH_HP_SCAN;
    cfg->hp_addr_set = 0u;
    for (uint32_t i = 0u; i < sizeof(cfg->hp_addr); i++) {
        cfg->hp_addr[i] = 0u;
    }
    cfg->hp_pick_first = 0u;
    cfg->hp_pick_last = 0u;
    cfg->hp_pair = 0u;
    cfg->hp_pscan = 0u;
    cfg->hp_pin = 0u;
    cfg->hp_mono = 0u;
    cfg->hp_secs = 10u;
    cfg->hp_rate = 0u;
    cfg->hp_bitpool = 53u;
    cfg->hp_speech_bp = 0u;
    cfg->hp_tick_ms = 20u;
    cfg->hp_pkt_per_tick = 2u;
    cfg->hp_txq = 4u;
    cfg->hp_credit_ticks = 4u;
    cfg->hp_credits = 4u;
    cfg->hp_tone_hz = 1000u;
    for (uint32_t i = 0u; i < sizeof(cfg->hp_file); i++) {
        cfg->hp_file[i] = '\0';
    }
}

static int hexval(uint8_t c)
{
    c = lower(c);
    return (c >= '0' && c <= '9') ? (int)(c - '0') : (c >= 'a' && c <= 'f') ? (int)(c - 'a' + 10) : -1;
}

/* XX:XX:XX:XX:XX:XX, most significant byte first; all-zero and broadcast are refused. */
static int parse_addr(const uint8_t *v, uint32_t n, uint8_t out[6])
{
    uint32_t zero = 0u, ones = 0u;
    if (n != 17u) {
        return -1;
    }
    for (uint32_t i = 0u; i < 6u; i++) {
        int hi = hexval(v[3u * i]), lo = hexval(v[3u * i + 1u]);
        if (hi < 0 || lo < 0 || (i < 5u && v[3u * i + 2u] != ':')) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        zero += out[i] == 0x00u;
        ones += out[i] == 0xFFu;
    }
    return (zero == 6u || ones == 6u) ? -1 : 0;
}

static int name_char(uint8_t c)
{
    c = lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~';
}

/* "/" then components NAME[.EXT] (1..8 + 1..3 characters), separated by '/'; no empty, '.' or '..' component. */
static int parse_path83(const uint8_t *v, uint32_t n, char *out)
{
    if (n < 2u || n > BT_BENCH_HP_FILE_MAX || v[0] != '/' || v[n - 1u] == '/') {
        return -1;
    }
    uint32_t base = 0u, ext = 0u;
    int in_ext = 0;
    for (uint32_t i = 1u; i < n; i++) {
        uint8_t c = v[i];
        if (c == '/') {
            if (base == 0u || (in_ext && ext == 0u)) {
                return -1;
            }
            base = ext = 0u;
            in_ext = 0;
        } else if (c == '.') {
            if (in_ext || base == 0u) {
                return -1;
            }
            in_ext = 1;
        } else if (!name_char(c)) {
            return -1;
        } else if (in_ext) {
            if (++ext > 3u) {
                return -1;
            }
        } else if (++base > 8u) {
            return -1;
        }
    }
    if (in_ext && ext == 0u) {
        return -1;
    }
    for (uint32_t i = 0u; i < n; i++) {
        out[i] = (char)v[i];
    }
    out[n] = '\0';
    return 0;
}

uint8_t bt_bench_hp_inq_len(uint8_t secs)
{
    uint32_t u = ((uint32_t)secs * 100u + 127u) / 128u;
    return (uint8_t)(u < 1u ? 1u : u > 0x30u ? 0x30u : u);
}

const char *bt_bench_hp_stage_name(uint8_t stage)
{
    switch (stage) {
    case BT_BENCH_HP_SCAN:
        return "scan";
    case BT_BENCH_HP_CONNECT:
        return "connect";
    case BT_BENCH_HP_TONE:
        return "tone";
    case BT_BENCH_HP_PLAYER:
        return "player";
    default:
        return "?";
    }
}

enum {
    K_SECS = 0, K_PAIR, K_PAIR_EVERY, K_PAIR_SECS, K_AUTOCONFIRM, K_EXIT, K_REBOOT, K_XREC_PIN,
    K_UI, K_SMP, K_COALESCE, K_SPP_WR, K_SPP_BUF, K_TX_RING, K_STAT_SECS,   /* phone-mode throughput keys */
    K_MODE, K_HP_FIRST,                      /* K_HP_FIRST.. are the hp_* keys (need mode=hp) */
    K_HP_STAGE = K_HP_FIRST, K_HP_ADDR, K_HP_PICK, K_HP_PAIR, K_HP_PSCAN, K_HP_SECS, K_HP_RATE, K_HP_BITPOOL,
    K_HP_SPEECH_BP, K_HP_MONO, K_HP_TICK_MS, K_HP_PKT_PER_TICK, K_HP_TXQ, K_HP_CREDIT_TICKS, K_HP_TONE_HZ, K_HP_FILE,
    K_HP_CREDITS, K_HP_PIN, K_N
};
static const char *const k_keys[K_N] = { "secs", "pair", "pair_every", "pair_secs", "autoconfirm", "exit", "reboot",
                                         "xrec_pin", "ui", "smp", "coalesce", "spp_wr", "spp_buf", "tx_ring",
                                         "stat_secs", "mode", "hp_stage", "hp_addr", "hp_pick", "hp_pair", "hp_pscan",
                                         "hp_secs", "hp_rate", "hp_bitpool", "hp_speech_bp", "hp_mono", "hp_tick_ms",
                                         "hp_pkt_per_tick", "hp_txq", "hp_credit_ticks", "hp_tone_hz", "hp_file",
                                         "hp_credits", "hp_pin" };
_Static_assert(K_N <= 64, "seen is a 64-bit key mask");

/* Numeric hp_* keys: range and field. */
static int hp_num(uint32_t k, int32_t n, bt_bench_cfg_t *c)
{
    switch (k) {
    case K_HP_SECS:
        if (n < 2 || n > 61) return -1;
        c->hp_secs = (uint8_t)n;
        return 0;
    case K_HP_BITPOOL:
        if (n < 18 || n > 53) return -1;
        c->hp_bitpool = (uint8_t)n;
        return 0;
    case K_HP_SPEECH_BP:
        if (n != 0 && (n < 18 || n > 35)) return -1;
        c->hp_speech_bp = (uint8_t)n;
        return 0;
    case K_HP_TICK_MS:
        if (n < 10 || n > 30) return -1;
        c->hp_tick_ms = (uint8_t)n;
        return 0;
    case K_HP_PKT_PER_TICK:
        if (n < 1 || n > 2) return -1;
        c->hp_pkt_per_tick = (uint8_t)n;
        return 0;
    case K_HP_TXQ:
        if (n < 2 || n > 8) return -1;
        c->hp_txq = (uint8_t)n;
        return 0;
    case K_HP_CREDIT_TICKS:
        if (n < 1 || n > 4) return -1;
        c->hp_credit_ticks = (uint8_t)n;
        return 0;
    case K_HP_TONE_HZ:
        if (n != 0 && (n < 100 || n > 8000)) return -1;   /* 0: 440 / 880 Hz alternating each second */
        c->hp_tone_hz = (uint16_t)n;
        return 0;
    case K_HP_CREDITS:
        if (n < 4 || n > 9) return -1;
        c->hp_credits = (uint8_t)n;
        return 0;
    default:                                             /* 0|1 flags */
        if (n != 0 && n != 1) return -1;
        if (k == K_HP_PAIR) {
            c->hp_pair = (uint8_t)n;
        } else if (k == K_HP_PSCAN) {
            c->hp_pscan = (uint8_t)n;
        } else if (k == K_HP_PIN) {
            c->hp_pin = (uint8_t)n;
        } else {
            c->hp_mono = (uint8_t)n;
        }
        return 0;
    }
}

/* Cross-key rules of mode=hp (bt_bench.h). Returns 0 when consistent. */
static int hp_rules(uint64_t seen, bt_bench_cfg_t *c)
{
#define SEEN(k) ((seen & (1ull << (k))) != 0u)
    uint64_t hp_keys = ~((1ull << K_HP_FIRST) - 1u);
    if (c->mode != BT_BENCH_MODE_HP) {
        return (seen & hp_keys) != 0u ? -1 : 0;
    }
    if (SEEN(K_PAIR) || SEEN(K_PAIR_EVERY) || SEEN(K_AUTOCONFIRM) || SEEN(K_XREC_PIN) || SEEN(K_UI) || SEEN(K_SMP) ||
        SEEN(K_COALESCE) || SEEN(K_SPP_WR) || SEEN(K_SPP_BUF) || SEEN(K_TX_RING) || SEEN(K_STAT_SECS)) {
        return -1;                                       /* phone-session overrides */
    }
    if (c->hp_stage >= BT_BENCH_HP_CONNECT && !c->hp_addr_set && !c->hp_pick_first && !c->hp_pick_last) {
        return -1;
    }
    if (c->hp_addr_set && (c->hp_pick_first || c->hp_pick_last)) {
        return -1;
    }
    if (c->hp_pick_last && c->hp_pair) {
        return -1;                                       /* the stored headset is bonded: no pairing window */
    }
    if (c->hp_pair && c->hp_stage == BT_BENCH_HP_SCAN) {
        return -1;
    }
    if (c->hp_pkt_per_tick == 1u && c->hp_tick_ms > 15u) {
        return -1;
    }
    if (SEEN(K_HP_TONE_HZ) && c->hp_stage != BT_BENCH_HP_TONE) {
        return -1;
    }
    if (c->hp_file[0] != '\0' && c->hp_stage != BT_BENCH_HP_PLAYER) {
        return -1;                                       /* player stage only; without it: the first /record file */
    }
    if (c->hp_speech_bp > c->hp_bitpool) {
        if (SEEN(K_HP_SPEECH_BP)) {
            return -1;
        }
        c->hp_speech_bp = c->hp_bitpool;                 /* a nonzero default under a smaller explicit max */
    }
    return 0;
#undef SEEN
}

bt_bench_err_t bt_bench_parse(const uint8_t *buf, uint32_t len, bt_bench_cfg_t *cfg, uint32_t *err_at)
{
    uint32_t i = 0u;
    uint64_t seen = 0u;
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
        if (seen & (1ull << k)) {
            return BT_BENCH_E_DUP;
        }
        seen |= 1ull << k;
        const uint8_t *v = &buf[eq + 1u];
        uint32_t vn = i - eq - 1u;
        int32_t n = number(v, vn);
        if (k >= K_HP_FIRST && k != K_HP_STAGE && k != K_HP_ADDR && k != K_HP_PICK && k != K_HP_RATE &&
            k != K_HP_FILE) {
            if (hp_num(k, n, &c) != 0) {
                return BT_BENCH_E_VALUE;
            }
            continue;
        }
        switch (k) {
        case K_MODE:
            if (word_is(v, vn, "phone")) {
                c.mode = BT_BENCH_MODE_PHONE;
            } else if (word_is(v, vn, "hp")) {
                c.mode = BT_BENCH_MODE_HP;
            } else {
                return BT_BENCH_E_VALUE;
            }
            break;
        case K_HP_STAGE: {
            uint8_t st = 0u;
            while (st <= BT_BENCH_HP_PLAYER && !word_is(v, vn, bt_bench_hp_stage_name(st))) {
                st++;
            }
            if (st > BT_BENCH_HP_PLAYER) {
                return BT_BENCH_E_VALUE;
            }
            c.hp_stage = st;
            break;
        }
        case K_HP_ADDR:
            if (parse_addr(v, vn, c.hp_addr) != 0) {
                return BT_BENCH_E_VALUE;
            }
            c.hp_addr_set = 1u;
            break;
        case K_HP_PICK:
            if (word_is(v, vn, "addr")) {
                c.hp_pick_first = c.hp_pick_last = 0u;
            } else if (word_is(v, vn, "first")) {
                c.hp_pick_first = 1u;
                c.hp_pick_last = 0u;
            } else if (word_is(v, vn, "last")) {
                c.hp_pick_last = 1u;
                c.hp_pick_first = 0u;
            } else {
                return BT_BENCH_E_VALUE;
            }
            break;
        case K_HP_RATE:
            if (word_is(v, vn, "auto")) {
                c.hp_rate = 0u;
            } else if (n == 44100 || n == 48000) {
                c.hp_rate = (uint16_t)n;
            } else {
                return BT_BENCH_E_VALUE;
            }
            break;
        case K_HP_FILE:
            if (parse_path83(v, vn, c.hp_file) != 0) {
                return BT_BENCH_E_VALUE;
            }
            break;
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
        case K_UI:
        case K_SMP:
        case K_COALESCE:
            if (n != 0 && n != 1) {
                return BT_BENCH_E_VALUE;
            }
            if (k == K_UI) {
                c.ui = (uint8_t)n;
            } else if (k == K_SMP) {
                c.smp = (uint8_t)n;
            } else {
                c.coalesce = (uint8_t)n;
            }
            break;
        case K_SPP_WR:
            if (n < 64 || n > (int32_t)BT_BENCH_SPP_WR_DEF) {
                return BT_BENCH_E_VALUE;
            }
            c.spp_wr = (uint16_t)n;
            break;
        case K_SPP_BUF:
            if (n < (int32_t)BT_BENCH_SPP_BUF_MIN || n > (int32_t)BT_BENCH_SPP_BUF_MAX) {
                return BT_BENCH_E_VALUE;
            }
            c.spp_buf = (uint16_t)n;
            break;
        case K_TX_RING:
            if (n != 2048 && n != 4096 && n != 8192) {
                return BT_BENCH_E_VALUE;
            }
            c.tx_ring = (uint16_t)n;
            break;
        case K_STAT_SECS:
            if (n < 10 || n > 300) {
                return BT_BENCH_E_VALUE;
            }
            c.stat_secs = (uint16_t)n;
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
    if (((c.pair_every != 0u || c.xrec_pin[0] != '\0') && !c.pair) || c.spp_buf < c.spp_wr || hp_rules(seen, &c) != 0) {
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

static uint32_t status_ms(const bt_bench_cfg_t *c)
{
    return c->stat_secs != 0u ? (uint32_t)c->stat_secs * 1000u : BT_BENCH_STATUS_MS;
}

void bt_bench_run_begin(bt_bench_run_t *r, const bt_bench_cfg_t *cfg, uint32_t now)
{
    r->cfg = *cfg;
    r->t0 = now;
    r->next_pair = cfg->pair ? 0u : NONE;
    r->next_status = status_ms(&r->cfg);
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
            r->next_status += status_ms(&r->cfg);
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

int bt_bench_hp_pick(const uint32_t *cod, uint32_t n)
{
    int any_av = -1;
    for (uint32_t i = 0u; i < n; i++) {
        uint32_t major = (cod[i] >> 8) & 0x1Fu, minor = (cod[i] >> 2) & 0x3Fu;
        if (major != 0x04u) {
            continue;                                    /* not Audio/Video: service bits alone do not count */
        }
        /* wearable headset 1, hands-free 2, loudspeaker 5, headphones 6, portable audio 7 */
        if (minor == 1u || minor == 2u || minor == 5u || minor == 6u || minor == 7u) {
            return (int)i;
        }
        if (any_av < 0) {
            any_av = (int)i;
        }
    }
    return any_av;
}
