/* Host tests for bt_port/bt_bench.c: the one-shot SD:/BTBENCH.RUN request for unattended phone-mode runs.
 *   - parsing: defaults, key=value forms, every refusal (a typo must not run a different test);
 *   - one-shot: the request is consumed (deletion verified) before anything runs; a request that cannot be deleted
 *     never runs; bad content is consumed and refused; directories and subdirectories are not requests;
 *   - schedule: pairing windows at the start and every pair_every s, exit after secs (wins over a window), status
 *     lines, a late tick opens one window, millis() wrap;
 *   - log capture from the g_openfw_log ring (wrap, overrun, full buffer, ring reset) and SD:/BTBENCH.LOG.
 * Usage: test_bt_bench <blank exFAT image> (made by mkfs.exfat in make test-bt-port).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openfw_fs.h"
#include "bt_bench.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- in-memory disk ---- */
static uint8_t *g_pristine, *g_disk;
static uint32_t g_sectors;
static int g_drop_writes;   /* 1: writes report success but change nothing */

static int mem_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > g_sectors) {
        return -1;
    }
    memcpy(buf, &g_disk[(size_t)lba * 512u], (size_t)count * 512u);
    return 0;
}

static int mem_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    if (lba + count > g_sectors) {
        return -1;
    }
    if (!g_drop_writes) {
        memcpy(&g_disk[(size_t)lba * 512u], buf, (size_t)count * 512u);
    }
    return 0;
}

static blkdev_t g_rw, g_ro;
static exfat_vol_t g_vol;

static void fresh(void)
{
    memcpy(g_disk, g_pristine, (size_t)g_sectors * 512u);
    g_drop_writes = 0;
    if (exfat_mount(&g_vol, &g_rw) != 0) {
        printf("FAIL: exfat_mount\n");
        exit(1);
    }
}

static void put_data(const char *dir, const char *name, const void *data, uint32_t len)
{
    exfat_writer_t w;
    CHECK(exfat_create_file(&g_vol, dir, name, &w) == 0, "create %s", name);
    CHECK(len == 0u || exfat_write(&w, data, len) == 0, "write %s", name);
    CHECK(exfat_close_file(&w) == 0, "close %s", name);
}

static void put_req(const char *s)
{
    put_data("/", BT_BENCH_TRIGGER_FILE, s, (uint32_t)strlen(s));
}

static int exists(const char *path)
{
    exfat_entry_t e;
    return exfat_find_path(&g_vol, path, &e) == 0;
}

static int read_back(const char *name, char *buf, uint32_t cap)
{
    exfat_entry_t e;
    char path[80];
    uint32_t got = 0u;
    snprintf(path, sizeof(path), "/%s", name);
    if (exfat_find_path(&g_vol, path, &e) != 0 || e.size >= cap ||
        exfat_read_file(&g_vol, &e, 0u, buf, (uint32_t)e.size, &got) != 0 || got != e.size) {
        return -1;
    }
    buf[got] = '\0';
    return (int)got;
}

static bt_bench_err_t parse(const char *s, bt_bench_cfg_t *c, uint32_t *at)
{
    return bt_bench_parse((const uint8_t *)s, (uint32_t)strlen(s), c, at);
}

/* ---------------------------------------------------------------------------------------------------------------- */
static void test_parse(void)
{
    bt_bench_cfg_t c;
    uint32_t at = 99u;

    /* empty (touch) = defaults */
    CHECK(parse("", &c, &at) == BT_BENCH_OK && at == 0u, "empty");
    CHECK(c.secs == 180u && c.pair == 0u && c.pair_every == 0u && c.pair_secs == 60u && c.autoconfirm == 0u &&
          c.reboot == 0u, "defaults");
    CHECK(bt_bench_parse(NULL, 0u, &c, NULL) == BT_BENCH_OK && c.secs == 180u, "NULL buffer");
    CHECK(parse(" \r\n\t,;", &c, NULL) == BT_BENCH_OK && c.secs == 180u, "separators only");

    /* the documented example */
    CHECK(parse("secs=180 pair=1 autoconfirm=1 exit=auto", &c, NULL) == BT_BENCH_OK, "example");
    CHECK(c.secs == 180u && c.pair == 1u && c.autoconfirm == 1u && c.reboot == 0u && c.pair_every == 0u, "example cfg");

    /* every key, mixed separators, case, comments, BOM + CRLF */
    const char *full = "\xEF\xBB\xBF# bench run\r\nSECS=600, Pair=1;pair_every=120\r\n"
                       "pair_secs=90 # longer windows\r\nAutoConfirm=0\texit=AUTO reboot=1\r\n";
    CHECK(parse(full, &c, &at) == BT_BENCH_OK, "full (at=%u)", at);
    CHECK(c.secs == 600u && c.pair == 1u && c.pair_every == 120u && c.pair_secs == 90u && c.autoconfirm == 0u &&
          c.reboot == 1u, "full cfg");
    CHECK(parse("secs=60#no space before the comment", &c, NULL) == BT_BENCH_OK && c.secs == 60u, "inline comment");

    /* ranges */
    CHECK(parse("secs=10", &c, NULL) == BT_BENCH_OK && c.secs == 10u, "secs min");
    CHECK(parse("secs=3600", &c, NULL) == BT_BENCH_OK && c.secs == 3600u, "secs max");
    CHECK(parse("secs=0010", &c, NULL) == BT_BENCH_OK && c.secs == 10u, "leading zeros");
    CHECK(parse("pair=1 pair_every=0", &c, NULL) == BT_BENCH_OK && c.pair_every == 0u, "pair_every 0");
    CHECK(parse("pair=1 pair_every=10", &c, NULL) == BT_BENCH_OK && c.pair_every == 10u, "pair_every min");
    CHECK(c.xrec_pin[0] == '\0', "xrec_pin default: none (DRBG PIN)");
    CHECK(parse("pair=1 XREC_PIN=012345", &c, NULL) == BT_BENCH_OK && strcmp(c.xrec_pin, "012345") == 0,
          "xrec_pin with a leading zero ('%s')", c.xrec_pin);
    CHECK(parse("secs=180 pair=1 pair_every=60 autoconfirm=1 xrec_pin=246810 exit=auto", &c, NULL) == BT_BENCH_OK &&
          strcmp(c.xrec_pin, "246810") == 0 && c.autoconfirm == 1u, "documented example with xrec_pin");

    static const struct { const char *s; bt_bench_err_t e; uint32_t at; } bad[] = {
        { "secs=9", BT_BENCH_E_VALUE, 0u },
        { "secs=3601", BT_BENCH_E_VALUE, 0u },
        { "secs=", BT_BENCH_E_VALUE, 0u },
        { "secs=-5", BT_BENCH_E_VALUE, 0u },
        { "secs=+5", BT_BENCH_E_VALUE, 0u },
        { "secs=1e3", BT_BENCH_E_VALUE, 0u },
        { "secs=999999", BT_BENCH_E_VALUE, 0u },
        { "secs=4294967476", BT_BENCH_E_VALUE, 0u },  /* would wrap to 180 in 32 bits */
        { "secs=180=1", BT_BENCH_E_VALUE, 0u },
        { "secs = 180", BT_BENCH_E_KEY, 0u },          /* "secs" alone is not key=value */
        { "sec=180", BT_BENCH_E_KEY, 0u },
        { "pair=1 sec=180", BT_BENCH_E_KEY, 7u },
        { "=1", BT_BENCH_E_KEY, 0u },
        { "pair", BT_BENCH_E_KEY, 0u },
        { "pair=2", BT_BENCH_E_VALUE, 0u },
        { "pair=yes", BT_BENCH_E_VALUE, 0u },
        { "autoconfirm=true", BT_BENCH_E_VALUE, 0u },
        { "exit=back", BT_BENCH_E_VALUE, 0u },
        { "exit=", BT_BENCH_E_VALUE, 0u },
        { "pair_secs=9", BT_BENCH_E_VALUE, 0u },
        { "pair_secs=601", BT_BENCH_E_VALUE, 0u },
        { "pair=1 pair_every=5", BT_BENCH_E_VALUE, 7u },
        { "pair_every=60", BT_BENCH_E_CONFLICT, 0u },
        { "pair=0 pair_every=60", BT_BENCH_E_CONFLICT, 0u },
        { "secs=60 secs=60", BT_BENCH_E_DUP, 8u },
        { "pair=1 PAIR=1", BT_BENCH_E_DUP, 7u },
        { "pair=1 xrec_pin=12345", BT_BENCH_E_VALUE, 7u },
        { "pair=1 xrec_pin=1234567", BT_BENCH_E_VALUE, 7u },
        { "pair=1 xrec_pin=12a456", BT_BENCH_E_VALUE, 7u },
        { "pair=1 xrec_pin=", BT_BENCH_E_VALUE, 7u },
        { "xrec_pin=123456", BT_BENCH_E_CONFLICT, 0u },
        { "pair=1 xrec_pin=123456 xrec_pin=123456", BT_BENCH_E_DUP, 23u },
        { "secs=6\x01" "0", BT_BENCH_E_CHAR, 6u },
        { "secs=\xC3\xA9", BT_BENCH_E_CHAR, 5u },
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        at = 99u;
        bt_bench_err_t e = parse(bad[i].s, &c, &at);
        CHECK(e == bad[i].e && at == bad[i].at, "bad #%zu \"%s\": err %s at %u (want %s at %u)", i, bad[i].s,
              bt_bench_err_name(e), at, bt_bench_err_name(bad[i].e), bad[i].at);
    }
    CHECK(parse("reboot=01", &c, NULL) == BT_BENCH_OK && c.reboot == 1u, "reboot=01 (leading zero)");
    /* an embedded NUL is refused (file content is not a C string) */
    CHECK(bt_bench_parse((const uint8_t *)"secs=60\0", 8u, &c, &at) == BT_BENCH_E_CHAR, "NUL byte");
    /* a refused parse leaves *cfg alone */
    bt_bench_cfg_default(&c);
    c.secs = 1234u;
    CHECK(parse("secs=60 bogus=1", &c, NULL) == BT_BENCH_E_KEY && c.secs == 1234u, "cfg untouched on error");

    CHECK(strcmp(bt_bench_err_name(BT_BENCH_OK), "ok") == 0 && strcmp(bt_bench_err_name(BT_BENCH_E_KEY), "key") == 0 &&
          strcmp(bt_bench_err_name(BT_BENCH_E_CONFLICT), "conflict") == 0, "err names");
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* headphones keys (docs/BT_A2DP_DESIGN.md §10.2): defaults, every key, ranges, cross-key rules */
static void test_parse_hp(void)
{
    bt_bench_cfg_t c;
    uint32_t at = 99u;
    CHECK(parse("", &c, NULL) == BT_BENCH_OK && c.mode == BT_BENCH_MODE_PHONE && c.hp_stage == BT_BENCH_HP_SCAN &&
          !c.hp_addr_set && !c.hp_pick_first && !c.hp_pair && !c.hp_pscan && !c.hp_mono && c.hp_secs == 10u &&
          c.hp_rate == 0u && c.hp_bitpool == 35u && c.hp_speech_bp == 24u && c.hp_tick_ms == 20u &&
          c.hp_pkt_per_tick == 2u && c.hp_txq == 4u && c.hp_credit_ticks == 2u && c.hp_tone_hz == 1000u &&
          c.hp_credits == 4u && c.hp_file[0] == '\0', "hp defaults");
    CHECK(parse("mode=phone secs=60", &c, NULL) == BT_BENCH_OK && c.mode == BT_BENCH_MODE_PHONE, "mode=phone");
    CHECK(parse("mode=hp", &c, NULL) == BT_BENCH_OK && c.mode == BT_BENCH_MODE_HP && c.hp_stage == BT_BENCH_HP_SCAN,
          "mode=hp alone: scan stage (Exp 4.0 / 4a)");
    /* the documented example */
    CHECK(parse("mode=hp hp_stage=tone hp_addr=00:1B:66:AA:BB:CC hp_pair=1 secs=600 hp_bitpool=35 hp_credits=4", &c,
                &at) == BT_BENCH_OK, "example (at %u)", at);
    static const uint8_t a[6] = { 0x00, 0x1B, 0x66, 0xAA, 0xBB, 0xCC };
    CHECK(c.hp_stage == BT_BENCH_HP_TONE && c.hp_addr_set && memcmp(c.hp_addr, a, 6) == 0 && c.hp_pair == 1u &&
          c.secs == 600u && c.hp_bitpool == 35u && c.hp_credits == 4u, "example cfg");
    /* every key at once, mixed case, lower-case hex */
    const char *all = "MODE=HP Hp_Stage=Player hp_pick=FIRST hp_pair=1 hp_pscan=1 hp_secs=61 hp_rate=44100 "
                      "hp_bitpool=53 hp_speech_bp=0 hp_mono=1 hp_tick_ms=10 hp_pkt_per_tick=1 hp_txq=8 "
                      "hp_credit_ticks=4 hp_file=/MUSIC/A_1.WAV hp_credits=9 pair_secs=120";
    CHECK(parse(all, &c, &at) == BT_BENCH_OK, "all keys (at %u)", at);
    CHECK(c.hp_stage == BT_BENCH_HP_PLAYER && c.hp_pick_first && c.hp_pair && c.hp_pscan && c.hp_secs == 61u &&
          c.hp_rate == 44100u && c.hp_bitpool == 53u && c.hp_speech_bp == 0u && c.hp_mono && c.hp_tick_ms == 10u &&
          c.hp_pkt_per_tick == 1u && c.hp_txq == 8u && c.hp_credit_ticks == 4u && strcmp(c.hp_file, "/MUSIC/A_1.WAV") == 0
          && c.hp_credits == 9u && c.pair_secs == 120u, "all keys cfg");
    CHECK(parse("mode=hp hp_stage=connect hp_addr=a0:b1:c2:d3:e4:f5 hp_rate=48000 hp_rate_x=1", &c, NULL) ==
          BT_BENCH_E_KEY, "unknown hp key");
    CHECK(parse("mode=hp hp_stage=connect hp_addr=a0:b1:c2:d3:e4:f5 hp_rate=48000", &c, NULL) == BT_BENCH_OK &&
          c.hp_addr[0] == 0xA0u && c.hp_addr[5] == 0xF5u && c.hp_rate == 48000u, "lower-case address, 48 kHz");
    CHECK(parse("mode=hp hp_stage=connect hp_pick=first hp_rate=auto", &c, NULL) == BT_BENCH_OK && c.hp_pick_first &&
          c.hp_rate == 0u, "pick first");
    CHECK(parse("mode=hp hp_bitpool=20", &c, NULL) == BT_BENCH_OK && c.hp_speech_bp == 20u,
          "default speech bitpool lowered to an explicit smaller max");
    CHECK(parse("mode=hp hp_bitpool=20 hp_speech_bp=18", &c, NULL) == BT_BENCH_OK && c.hp_speech_bp == 18u,
          "explicit speech bitpool under the max");
    CHECK(parse("mode=hp hp_stage=tone hp_pick=first hp_tone_hz=440 hp_tick_ms=15 hp_pkt_per_tick=1", &c, NULL) ==
          BT_BENCH_OK && c.hp_tone_hz == 440u, "tone stage keys");
    CHECK(parse("mode=hp hp_stage=scan hp_addr=00:1B:66:AA:BB:CC", &c, NULL) == BT_BENCH_OK,
          "hp_addr allowed (scan reports it)");
    CHECK(parse("mode=hp hp_stage=player hp_pick=first", &c, NULL) == BT_BENCH_OK && c.hp_file[0] == '\0',
          "player stage without hp_file: the first /record file");
    CHECK(parse("mode=hp hp_stage=tone hp_pick=first hp_tone_hz=0", &c, NULL) == BT_BENCH_OK && c.hp_tone_hz == 0u,
          "hp_tone_hz=0: alternating 440 / 880 Hz");
    CHECK(parse("mode=hp hp_stage=player hp_pick=first hp_file=/A.B", &c, NULL) == BT_BENCH_OK &&
          strcmp(c.hp_file, "/A.B") == 0, "shortest 8.3 path");
    CHECK(parse("mode=hp hp_stage=player hp_pick=first hp_file=/ABCDEFGH/IJKLMNOP.WAV", &c, NULL) == BT_BENCH_OK,
          "8-character components");
    CHECK(parse("mode=hp hp_stage=player hp_pick=first hp_file=/README", &c, NULL) == BT_BENCH_OK,
          "a name without extension");

    static const struct { const char *s; bt_bench_err_t e; } bad[] = {
        { "mode=headphones", BT_BENCH_E_VALUE },
        { "mode=", BT_BENCH_E_VALUE },
        { "mode=hp mode=hp", BT_BENCH_E_DUP },
        { "hp_stage=tone", BT_BENCH_E_CONFLICT },                       /* hp_* without mode=hp */
        { "mode=phone hp_bitpool=35", BT_BENCH_E_CONFLICT },
        { "mode=hp hp_stage=stream", BT_BENCH_E_VALUE },
        { "mode=hp hp_stage=connect", BT_BENCH_E_CONFLICT },            /* no address, no pick=first */
        { "mode=hp hp_stage=tone hp_addr=00:1B:66:AA:BB:CC hp_pick=first", BT_BENCH_E_CONFLICT },
        { "mode=hp hp_pair=1", BT_BENCH_E_CONFLICT },                   /* pairing needs a connect stage */
        { "mode=hp pair=1", BT_BENCH_E_CONFLICT },                      /* phone-session keys */
        { "mode=hp autoconfirm=1", BT_BENCH_E_CONFLICT },
        { "mode=hp pair=1 xrec_pin=123456", BT_BENCH_E_CONFLICT },
        { "mode=hp hp_stage=connect hp_pick=first hp_pkt_per_tick=1", BT_BENCH_E_CONFLICT },  /* tick 20 > 15 */
        { "mode=hp hp_stage=connect hp_pick=first hp_tick_ms=16 hp_pkt_per_tick=1", BT_BENCH_E_CONFLICT },
        { "mode=hp hp_tone_hz=440", BT_BENCH_E_CONFLICT },              /* tone key, scan stage */
        { "mode=hp hp_stage=tone hp_pick=first hp_file=/A.WAV", BT_BENCH_E_CONFLICT },
        { "mode=hp hp_bitpool=20 hp_speech_bp=24", BT_BENCH_E_CONFLICT },
        { "mode=hp hp_addr=00:1B:66:AA:BB", BT_BENCH_E_VALUE },
        { "mode=hp hp_addr=00:1B:66:AA:BB:CC:DD", BT_BENCH_E_VALUE },
        { "mode=hp hp_addr=00-1B-66-AA-BB-CC", BT_BENCH_E_VALUE },
        { "mode=hp hp_addr=00:1B:66:AA:BB:CG", BT_BENCH_E_VALUE },
        { "mode=hp hp_addr=00:00:00:00:00:00", BT_BENCH_E_VALUE },
        { "mode=hp hp_addr=FF:FF:FF:FF:FF:FF", BT_BENCH_E_VALUE },
        { "mode=hp hp_pick=last", BT_BENCH_E_VALUE },
        { "mode=hp hp_pair=2", BT_BENCH_E_VALUE },
        { "mode=hp hp_pscan=yes", BT_BENCH_E_VALUE },
        { "mode=hp hp_secs=1", BT_BENCH_E_VALUE },
        { "mode=hp hp_secs=62", BT_BENCH_E_VALUE },
        { "mode=hp hp_rate=32000", BT_BENCH_E_VALUE },
        { "mode=hp hp_rate=", BT_BENCH_E_VALUE },
        { "mode=hp hp_bitpool=17", BT_BENCH_E_VALUE },
        { "mode=hp hp_bitpool=54", BT_BENCH_E_VALUE },
        { "mode=hp hp_speech_bp=17", BT_BENCH_E_VALUE },
        { "mode=hp hp_speech_bp=36", BT_BENCH_E_VALUE },
        { "mode=hp hp_mono=2", BT_BENCH_E_VALUE },
        { "mode=hp hp_tick_ms=9", BT_BENCH_E_VALUE },
        { "mode=hp hp_tick_ms=31", BT_BENCH_E_VALUE },
        { "mode=hp hp_pkt_per_tick=0", BT_BENCH_E_VALUE },
        { "mode=hp hp_pkt_per_tick=3", BT_BENCH_E_VALUE },
        { "mode=hp hp_txq=1", BT_BENCH_E_VALUE },
        { "mode=hp hp_txq=9", BT_BENCH_E_VALUE },
        { "mode=hp hp_credit_ticks=0", BT_BENCH_E_VALUE },
        { "mode=hp hp_credit_ticks=5", BT_BENCH_E_VALUE },
        { "mode=hp hp_stage=tone hp_pick=first hp_tone_hz=99", BT_BENCH_E_VALUE },
        { "mode=hp hp_stage=tone hp_pick=first hp_tone_hz=8001", BT_BENCH_E_VALUE },
        { "mode=hp hp_credits=3", BT_BENCH_E_VALUE },
        { "mode=hp hp_credits=10", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=A.WAV", BT_BENCH_E_VALUE },                  /* not absolute */
        { "mode=hp hp_file=/", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/MUSIC/", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/LONGNAME9.WAV", BT_BENCH_E_VALUE },         /* 9-character name */
        { "mode=hp hp_file=/A.WAVE", BT_BENCH_E_VALUE },                /* 4-character extension */
        { "mode=hp hp_file=/A..WAV", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/.WAV", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/A./B", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=//A.WAV", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/A.W/../B", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/A$.WAV", BT_BENCH_E_VALUE },
        { "mode=hp hp_file=/AAAAAAAA/BBBBBBBB/CCCCCCCC/DDDDDDDD/EEEEEEEE.WAV", BT_BENCH_E_VALUE },   /* > 47 */
        { "mode=hp hp_bitpool=35 hp_bitpool=35", BT_BENCH_E_DUP },
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bt_bench_cfg_default(&c);
        c.secs = 4321u;
        bt_bench_err_t e = parse(bad[i].s, &c, &at);
        CHECK(e == bad[i].e, "hp bad #%zu \"%s\": err %s (want %s)", i, bad[i].s, bt_bench_err_name(e),
              bt_bench_err_name(bad[i].e));
        CHECK(c.secs == 4321u, "hp bad #%zu: cfg untouched", i);
    }
    /* the longest well-formed request still fits the 256-byte file limit */
    CHECK(strlen(all) < BT_BENCH_TRIGGER_MAX, "all-keys request fits BT_BENCH_TRIGGER_MAX (%zu)", strlen(all));
    /* inquiry length: ceil(s / 1.28), 1..0x30 */
    CHECK(bt_bench_hp_inq_len(10) == 8u && bt_bench_hp_inq_len(2) == 2u && bt_bench_hp_inq_len(61) == 0x30u &&
          bt_bench_hp_inq_len(1) == 1u && bt_bench_hp_inq_len(0) == 1u && bt_bench_hp_inq_len(255) == 0x30u &&
          bt_bench_hp_inq_len(13) == 11u, "inquiry length");
    CHECK(strcmp(bt_bench_hp_stage_name(BT_BENCH_HP_TONE), "tone") == 0 && strcmp(bt_bench_hp_stage_name(9), "?") == 0,
          "stage names");
}

/* ---------------------------------------------------------------------------------------------------------------- */
static bt_bench_trig_t take(exfat_vol_t *vol, int mounted, bt_bench_cfg_t *c)
{
    bt_bench_err_t e = (bt_bench_err_t)77;
    uint32_t at = 77u;
    bt_bench_trig_t t = bt_bench_take(vol, mounted, c, &e, &at);
    CHECK((t == BT_BENCH_TRIG_BAD) == (e != BT_BENCH_OK), "err set exactly when bad (t=%d e=%d)", (int)t, (int)e);
    return t;
}

static void test_take(void)
{
    bt_bench_cfg_t c;

    fresh();
    CHECK(take(&g_vol, 0, &c) == BT_BENCH_TRIG_NO_SD, "unmounted");
    CHECK(take(NULL, 1, &c) == BT_BENCH_TRIG_NO_SD, "NULL volume");
    CHECK(bt_bench_take(NULL, 1, NULL, NULL, NULL) == BT_BENCH_TRIG_NO_SD, "NULL outputs");

    /* blank card: nothing written */
    fresh();
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_ABSENT, "blank card");
    CHECK(memcmp(g_disk, g_pristine, (size_t)g_sectors * 512u) == 0, "absent request must not write the card");

    /* taken once, deleted, config returned; the next boot (remount) has no request; other files untouched */
    fresh();
    put_data("/", "KEEP.TXT", "x", 1u);
    put_req("secs=240 pair=1 pair_every=60 autoconfirm=1 exit=auto reboot=1\n");
    memset(&c, 0, sizeof(c));
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_TAKEN, "taken");
    CHECK(c.secs == 240u && c.pair == 1u && c.pair_every == 60u && c.autoconfirm == 1u && c.reboot == 1u, "cfg");
    CHECK(!exists("/" BT_BENCH_TRIGGER_FILE), "deleted");
    CHECK(exists("/KEEP.TXT"), "other file kept");
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_ABSENT, "second call: none");
    exfat_vol_t again;
    CHECK(exfat_mount(&again, &g_rw) == 0, "remount");
    CHECK(take(&again, 1, &c) == BT_BENCH_TRIG_ABSENT, "next boot: none");

    /* empty request (touch) runs with defaults */
    fresh();
    put_req("");
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_TAKEN && c.secs == 180u && c.pair == 0u, "empty = defaults");

    /* lower-case name from a PC */
    fresh();
    put_data("/", "btbench.run", "pair=1", 6u);
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_TAKEN && c.pair == 1u, "lower-case name");
    CHECK(!exists("/btbench.run"), "lower-case deleted");

    /* bad content: consumed (reported once), not run */
    fresh();
    put_req("secs=180 pari=1");
    bt_bench_err_t e = BT_BENCH_OK;
    uint32_t at = 0u;
    CHECK(bt_bench_take(&g_vol, 1, &c, &e, &at) == BT_BENCH_TRIG_BAD && e == BT_BENCH_E_KEY && at == 9u,
          "bad content (e=%d at=%u)", (int)e, at);
    CHECK(!exists("/" BT_BENCH_TRIGGER_FILE), "bad content consumed");
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_ABSENT, "bad content: next boot none");

    /* oversized: refused, consumed */
    fresh();
    char big[BT_BENCH_TRIGGER_MAX + 16u];
    memset(big, ' ', sizeof(big));
    memcpy(big, "secs=60", 7u);
    put_data("/", BT_BENCH_TRIGGER_FILE, big, (uint32_t)sizeof(big));
    CHECK(bt_bench_take(&g_vol, 1, &c, &e, &at) == BT_BENCH_TRIG_BAD && e == BT_BENCH_E_VALUE, "oversized");
    CHECK(!exists("/" BT_BENCH_TRIGGER_FILE), "oversized consumed");
    /* exactly the limit is fine */
    fresh();
    put_data("/", BT_BENCH_TRIGGER_FILE, big, BT_BENCH_TRIGGER_MAX);
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_TAKEN && c.secs == 60u, "limit-sized request");

    /* a directory of that name is not a request and is left alone */
    fresh();
    CHECK(exfat_mkdir(&g_vol, BT_BENCH_TRIGGER_FILE) == 0, "mkdir");
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_NOT_FILE, "directory");
    CHECK(exists("/" BT_BENCH_TRIGGER_FILE), "directory kept");

    /* subdirectory only: not a request */
    fresh();
    CHECK(exfat_mkdir(&g_vol, "SUB") == 0, "mkdir SUB");
    put_data("/SUB", BT_BENCH_TRIGGER_FILE, "pair=1", 6u);
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_ABSENT, "subdirectory ignored");

    /* cannot be consumed: never runs (it would run on every boot) */
    fresh();
    put_req("pair=1");
    exfat_vol_t ro;
    CHECK(exfat_mount(&ro, &g_ro) == 0, "read-only mount");
    CHECK(take(&ro, 1, &c) == BT_BENCH_TRIG_STUCK, "read-only card");
    CHECK(exists("/" BT_BENCH_TRIGGER_FILE), "read-only: still there");
    fresh();
    put_req("pair=1");
    g_drop_writes = 1;
    CHECK(take(&g_vol, 1, &c) == BT_BENCH_TRIG_STUCK, "delete that does not stick");
    g_drop_writes = 0;
    /* stuck with bad content: still stuck (not run, not reported as consumed) */
    fresh();
    put_req("bogus");
    CHECK(exfat_mount(&ro, &g_ro) == 0, "read-only remount");
    CHECK(take(&ro, 1, &c) == BT_BENCH_TRIG_STUCK, "read-only bad content");

    CHECK(strcmp(bt_bench_trig_name(BT_BENCH_TRIG_TAKEN), "taken") == 0 &&
          strcmp(bt_bench_trig_name(BT_BENCH_TRIG_ABSENT), "absent") == 0 &&
          strcmp(bt_bench_trig_name(BT_BENCH_TRIG_STUCK), "stuck") == 0 &&
          strcmp(bt_bench_trig_name(BT_BENCH_TRIG_BAD), "bad_content") == 0 &&
          strcmp(bt_bench_trig_name(BT_BENCH_TRIG_NOT_FILE), "not_file") == 0 &&
          strcmp(bt_bench_trig_name(BT_BENCH_TRIG_NO_SD), "no_sd") == 0, "trig names");
}

/* ---------------------------------------------------------------------------------------------------------------- */
static void run_sched(uint32_t t0)
{
    bt_bench_cfg_t c;
    bt_bench_run_t r;
    CHECK(parse("secs=200 pair=1 pair_every=60", &c, NULL) == BT_BENCH_OK, "sched cfg");
    bt_bench_run_begin(&r, &c, t0);
    CHECK(bt_bench_run_tick(&r, t0) == BT_BENCH_ACT_PAIR && r.pairs == 1u, "window at the start");
    CHECK(bt_bench_run_tick(&r, t0 + 1u) == 0u, "nothing at 1 ms");
    CHECK(bt_bench_run_tick(&r, t0 + 29999u) == 0u, "nothing before the first status");
    CHECK(bt_bench_run_tick(&r, t0 + 30000u) == BT_BENCH_ACT_STATUS, "status at 30 s");
    CHECK(bt_bench_run_tick(&r, t0 + 59999u) == 0u, "no window before 60 s");
    CHECK(bt_bench_run_tick(&r, t0 + 60000u) == (BT_BENCH_ACT_PAIR | BT_BENCH_ACT_STATUS) && r.pairs == 2u,
          "window + status at 60 s");
    CHECK(bt_bench_run_tick(&r, t0 + 60001u) == 0u, "once");
    /* late tick (app busy 70 s): one window, one status, not one per missed period */
    CHECK(bt_bench_run_tick(&r, t0 + 130000u) == (BT_BENCH_ACT_PAIR | BT_BENCH_ACT_STATUS) && r.pairs == 3u,
          "late tick: one window");
    CHECK(bt_bench_run_tick(&r, t0 + 150000u) == BT_BENCH_ACT_STATUS, "status at 150 s");
    CHECK(bt_bench_run_tick(&r, t0 + 179999u) == 0u, "next window at 180 s, not 190 s");
    CHECK(bt_bench_run_tick(&r, t0 + 180000u) == (BT_BENCH_ACT_PAIR | BT_BENCH_ACT_STATUS), "window at 180 s");
    CHECK(bt_bench_run_tick(&r, t0 + 199999u) == 0u, "before the end");
    CHECK(bt_bench_run_tick(&r, t0 + 200000u) == BT_BENCH_ACT_EXIT, "exit at secs");
    CHECK(bt_bench_run_tick(&r, t0 + 200001u) == BT_BENCH_ACT_EXIT, "exit stays");
    CHECK(bt_bench_run_tick(&r, t0 + 5u) == BT_BENCH_ACT_EXIT, "exit latched");
    CHECK(r.pairs == 4u, "4 windows");
}

static void test_schedule(void)
{
    run_sched(1000u);
    run_sched(0xFFFFFFFFu - 70000u);                     /* millis() wraps during the run */

    bt_bench_cfg_t c;
    bt_bench_run_t r;
    /* no pairing */
    bt_bench_cfg_default(&c);
    c.secs = 10u;
    bt_bench_run_begin(&r, &c, 0u);
    CHECK(bt_bench_run_tick(&r, 0u) == 0u && r.pairs == 0u, "pair=0: no window");
    CHECK(bt_bench_run_tick(&r, 9999u) == 0u, "pair=0: nothing");
    CHECK(bt_bench_run_tick(&r, 10000u) == BT_BENCH_ACT_EXIT, "pair=0: exit");
    /* one window only (pair_every=0) */
    CHECK(parse("secs=300 pair=1", &c, NULL) == BT_BENCH_OK, "cfg");
    bt_bench_run_begin(&r, &c, 50u);
    CHECK(bt_bench_run_tick(&r, 60u) == BT_BENCH_ACT_PAIR, "first window");
    for (uint32_t t = 1000u; t < 300000u; t += 1000u) {
        CHECK((bt_bench_run_tick(&r, 50u + t) & BT_BENCH_ACT_PAIR) == 0u, "no second window at %u", t);
    }
    CHECK(r.pairs == 1u, "one window");
    /* exit wins over a due window: never open one at or after the end */
    CHECK(parse("secs=60 pair=1 pair_every=60", &c, NULL) == BT_BENCH_OK, "cfg");
    bt_bench_run_begin(&r, &c, 0u);
    CHECK(bt_bench_run_tick(&r, 0u) == BT_BENCH_ACT_PAIR, "start window");
    CHECK(bt_bench_run_tick(&r, 60000u) == BT_BENCH_ACT_EXIT && r.pairs == 1u, "exit, no window at the end");
    /* first tick only after the end: exit, no window */
    CHECK(parse("secs=10 pair=1", &c, NULL) == BT_BENCH_OK, "cfg");
    bt_bench_run_begin(&r, &c, 0u);
    CHECK(bt_bench_run_tick(&r, 10000u) == BT_BENCH_ACT_EXIT && r.pairs == 0u, "late first tick");
    /* longest run */
    CHECK(parse("secs=3600", &c, NULL) == BT_BENCH_OK, "cfg");
    bt_bench_run_begin(&r, &c, 7u);
    CHECK((bt_bench_run_tick(&r, 7u + 3599999u) & BT_BENCH_ACT_EXIT) == 0u, "3600 s: not yet");
    CHECK(bt_bench_run_tick(&r, 7u + 3600000u) == BT_BENCH_ACT_EXIT, "3600 s: exit");
}

/* ---------------------------------------------------------------------------------------------------------------- */
#define RING 64u
static char g_ring[RING];
static uint32_t g_widx;

static void ring_puts(const char *s)
{
    while (*s != '\0') {
        g_ring[g_widx++ & (RING - 1u)] = *s++;
    }
}

static void test_capture(void)
{
    char buf[100];
    bt_bench_cap_t c;

    /* starts at the current write index: earlier ring content is not captured */
    memset(g_ring, 0, sizeof(g_ring));
    g_widx = 0u;
    ring_puts("before start\n");
    bt_bench_cap_init(&c, buf, sizeof(buf), g_widx);
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.len == 0u && c.lost == 0u, "nothing new");
    ring_puts("BENCH start\n");
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.len == 12u && memcmp(buf, "BENCH start\n", 12u) == 0, "first line");

    /* across the ring's wrap */
    ring_puts("0123456789012345678901234567890123456789\n");   /* 41 B: wraps (13 + 12 + 41 > 64) */
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.len == 53u && memcmp(buf + 12, "0123456789012345678901234567890123456789\n", 41u) == 0, "wrap");
    CHECK(c.lost == 0u, "no loss");

    /* more than one ring between two drains: the overwritten head is lost, the last RING bytes are kept */
    for (int i = 0; i < 10; i++) {
        ring_puts("abcdefghij");                          /* 100 B > 64 */
    }
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.lost == 36u + (53u + 64u - 100u), "overrun: 36 B overwritten + the rest beyond the buffer (lost=%u)",
          c.lost);
    CHECK(c.len == 100u, "buffer full (len=%u)", c.len);
    CHECK(memcmp(buf + 53, "ghijabcdef", 10u) == 0, "kept bytes start where the ring still held them");

    /* full buffer: everything after is counted, nothing written past it */
    uint32_t lost0 = c.lost;
    ring_puts("more\n");
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.len == 100u && c.lost == lost0 + 5u, "full: counted");

    /* ring reset (XREC log reset): capture restarts at the ring's start */
    bt_bench_cap_init(&c, buf, sizeof(buf), g_widx);
    memset(g_ring, 0, sizeof(g_ring));
    g_widx = 0u;
    ring_puts("after reset\n");
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.resets == 1u && c.len == 12u && memcmp(buf, "after reset\n", 12u) == 0, "reset");

    /* write index wrap (uint32) */
    bt_bench_cap_init(&c, buf, sizeof(buf), 0xFFFFFFF8u);
    g_widx = 0xFFFFFFF8u;
    ring_puts("wrapping index\n");
    bt_bench_cap_drain(&c, g_ring, RING, g_widx);
    CHECK(c.resets == 1u, "index wrap is seen as a reset (once in 4 GB of log): resets=%u", c.resets);

    /* bad ring arguments: ignored */
    bt_bench_cap_init(&c, buf, sizeof(buf), 0u);
    bt_bench_cap_drain(&c, NULL, RING, 10u);
    bt_bench_cap_drain(&c, g_ring, 48u, 10u);
    CHECK(c.len == 0u && c.last_widx == 0u, "bad ring ignored");
    bt_bench_cap_init(&c, NULL, 100u, 0u);
    bt_bench_cap_drain(&c, g_ring, RING, 10u);
    CHECK(c.len == 0u && c.lost == 10u, "no buffer: all lost");
}

static void test_log_save(void)
{
    exfat_writer_t w;
    char rb[20000];
    fresh();
    CHECK(bt_bench_log_save(&g_vol, 1, "line 1\nline 2\n", 14u, "TAIL\n", 5u, &w) == 0, "save");
    CHECK(read_back(BT_BENCH_LOG_FILE, rb, sizeof(rb)) == 19 && strcmp(rb, "line 1\nline 2\nTAIL\n") == 0, "content");
    /* replaced by the next run */
    CHECK(bt_bench_log_save(&g_vol, 1, "x\n", 2u, NULL, 0u, &w) == 0, "replace");
    CHECK(read_back(BT_BENCH_LOG_FILE, rb, sizeof(rb)) == 2 && strcmp(rb, "x\n") == 0, "replaced");
    /* large (several clusters / sectors) */
    static char big[12000];
    for (size_t i = 0u; i < sizeof(big); i++) {
        big[i] = (char)('a' + i % 26u);
    }
    CHECK(bt_bench_log_save(&g_vol, 1, big, sizeof(big), "END\n", 4u, &w) == 0, "big");
    CHECK(read_back(BT_BENCH_LOG_FILE, rb, sizeof(rb)) == (int)sizeof(big) + 4 && memcmp(rb, big, sizeof(big)) == 0 &&
          memcmp(rb + sizeof(big), "END\n", 4u) == 0, "big content");
    /* empty */
    CHECK(bt_bench_log_save(&g_vol, 1, NULL, 0u, NULL, 0u, &w) == 0, "empty");
    CHECK(read_back(BT_BENCH_LOG_FILE, rb, sizeof(rb)) == 0, "empty content");
    /* refusals */
    CHECK(bt_bench_log_save(&g_vol, 0, "x", 1u, NULL, 0u, &w) == -1, "unmounted");
    CHECK(bt_bench_log_save(NULL, 1, "x", 1u, NULL, 0u, &w) == -1, "no volume");
    CHECK(bt_bench_log_save(&g_vol, 1, "x", 1u, NULL, 0u, NULL) == -1, "no writer");
    CHECK(bt_bench_log_save(&g_vol, 1, NULL, 1u, NULL, 0u, &w) == -1, "NULL data");
    CHECK(bt_bench_log_save(&g_vol, 1, "x", 1u, NULL, 1u, &w) == -1, "NULL tail");
    fresh();
    CHECK(exfat_mkdir(&g_vol, BT_BENCH_LOG_FILE) == 0, "mkdir log name");
    CHECK(bt_bench_log_save(&g_vol, 1, "x", 1u, NULL, 0u, &w) == -1, "directory never replaced");
    CHECK(exists("/" BT_BENCH_LOG_FILE), "directory kept");
    fresh();
    exfat_vol_t ro;
    CHECK(exfat_mount(&ro, &g_ro) == 0, "ro mount");
    CHECK(bt_bench_log_save(&ro, 1, "x", 1u, NULL, 0u, &w) == -1, "read-only card");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <blank exFAT image>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_sectors = (uint32_t)(sz / 512);
    g_pristine = malloc((size_t)sz);
    g_disk = malloc((size_t)sz);
    if (g_pristine == NULL || g_disk == NULL || fread(g_pristine, 1, (size_t)sz, f) != (size_t)sz) {
        printf("FAIL: read image\n");
        return 1;
    }
    fclose(f);
    g_rw = (blkdev_t){ mem_read, mem_write, g_sectors, NULL };
    g_ro = (blkdev_t){ mem_read, NULL, g_sectors, NULL };

    test_parse();
    test_parse_hp();
    test_take();
    test_schedule();
    test_capture();
    test_log_save();

    free(g_pristine);
    free(g_disk);
    if (g_fail != 0) {
        printf("test_bt_bench: %d FAILED\n", g_fail);
        return 1;
    }
    printf("test_bt_bench: all passed\n");
    return 0;
}
