/* Host tests for bt_port/bt_rom_rng.c (the ROM RNG hook at 0x00803C24, docs/BT_ROM_INTERFACE.md §2.3).
 *   - site table: every traced call site classified, legacy and unknown return addresses get the legacy value
 *   - security sites draw from the pool (byte / word / nonce block fill), consumed bytes are not served twice
 *   - pool short -> legacy value + `starved` (the platform's pairing gate reads it)
 *   - producer/consumer accounting across index wrap
 *   - with the gitignored ROM dump (argv[1], dumps/bootrom_384K.bin): the static facts the table rests on are
 *     re-derived from the ROM bytes: exactly 11 BL/B.W to the thunk 0x3DE98, at exactly the table's sites; the thunk
 *     loads slot 0x00803C1C + 8; the fixed P-192 pair at 0xD7EC/0xD84C is d * G (the key bt_rom_real replaces)
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_p192.h"
#include "bt_rom_rng.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_legacy_calls;
static uint32_t legacy(void)
{
    g_legacy_calls++;
    return 77u;                            /* a legacy-range value (0..79) */
}

static const uint32_t k_legacy_sites[] = { 0x000312FFu, 0x00035781u, 0x00039C31u, 0x00042F79u, 0x00044153u };
static const uint32_t k_byte_sites[] = { 0x00031401u, 0x00031D81u, 0x00031D89u, 0x00031D91u };

static void feed_seq(uint8_t start, size_t n)
{
    uint8_t buf[600];
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)(start + i);
    }
    CHECK(bt_rom_rng_feed(buf, n) == n, "feed refused bytes");
}

static void test_sites(void)
{
    for (size_t i = 0; i < sizeof(k_legacy_sites) / sizeof(k_legacy_sites[0]); i++) {
        CHECK(bt_rom_rng_site(k_legacy_sites[i]) == BT_ROM_RNG_SITE_LEGACY, "legacy site %08x", k_legacy_sites[i]);
    }
    for (size_t i = 0; i < sizeof(k_byte_sites) / sizeof(k_byte_sites[0]); i++) {
        CHECK(bt_rom_rng_site(k_byte_sites[i]) == BT_ROM_RNG_SITE_BYTE, "byte site %08x", k_byte_sites[i]);
    }
    CHECK(bt_rom_rng_site(0x00031D77u) == BT_ROM_RNG_SITE_NONCE_FIRST, "nonce site");
    CHECK(bt_rom_rng_site(0x00031D76u) == BT_ROM_RNG_SITE_NONCE_FIRST, "thumb bit ignored");
    CHECK(bt_rom_rng_site(0x00031EE9u) == BT_ROM_RNG_SITE_WORD, "passkey site");
    CHECK(bt_rom_rng_site(0x00031D7Bu) == BT_ROM_RNG_SITE_UNKNOWN, "unknown site");
}

static void test_hook(void)
{
    bt_rom_rng_stats_t st;
    uint8_t block[BT_ROM_NONCE_BLOCK_BYTES];
    bt_rom_rng_reset();
    g_legacy_calls = 0u;

    /* empty pool: every security site starves, legacy sites never touch the pool */
    memset(block, 0xEE, sizeof(block));
    CHECK(bt_rom_rng_hook(0x00031D77u, block, legacy) == 77u, "starved nonce site not legacy");
    CHECK(block[6] == 0xEE, "starved nonce site wrote the block");
    CHECK(bt_rom_rng_hook(0x00031401u, NULL, legacy) == 77u, "starved byte site not legacy");
    CHECK(bt_rom_rng_hook(0x00039C31u, NULL, legacy) == 77u, "legacy site");
    CHECK(bt_rom_rng_hook(0x00012345u, NULL, legacy) == 77u, "unknown site");
    bt_rom_rng_stats(&st);
    CHECK(st.starved == 2u && st.legacy_calls == 1u && st.unknown_calls == 1u && st.drbg_calls == 0u &&
          st.last_unknown == 0x00012345u, "stats after starvation (%u %u %u %u)", st.starved, st.legacy_calls,
          st.unknown_calls, st.drbg_calls);
    CHECK(g_legacy_calls == 4u, "legacy generator calls %u", g_legacy_calls);

    /* nonce: the first hook call overwrites the whole 28-byte block, returns byte 29; the next three take one each */
    bt_rom_rng_reset();
    feed_seq(1u, 64u);
    CHECK(bt_rom_rng_space() == BT_ROM_RNG_POOL_BYTES - 64u, "space after feed");
    uint32_t v = bt_rom_rng_hook(0x00031D77u, block, legacy);
    int ok = 1;
    for (int i = 0; i < 28; i++) {
        ok &= block[i] == (uint8_t)(1 + i);
    }
    CHECK(ok, "nonce block not filled from the pool");
    CHECK(v == 29u, "nonce first value %u", v);
    CHECK(bt_rom_rng_hook(0x00031D81u, block, legacy) == 30u, "2nd nonce byte");
    CHECK(bt_rom_rng_hook(0x00031D89u, block, legacy) == 31u, "3rd nonce byte");
    CHECK(bt_rom_rng_hook(0x00031D91u, block, legacy) == 32u, "4th nonce byte");
    CHECK(bt_rom_rng_hook(0x00031EE9u, NULL, legacy) == (33u | 34u << 8 | 35u << 16 | 36u << 24), "passkey word");
    bt_rom_rng_stats(&st);
    CHECK(st.nonce_fills == 1u && st.drbg_calls == 5u && st.starved == 0u && st.fed_bytes == 64u, "stats after nonce");
    CHECK(bt_rom_rng_space() == BT_ROM_RNG_POOL_BYTES - 64u + 36u, "space after consumption");

    /* a short pool serves nothing partially: 2 bytes left, the word site starves and the bytes stay for byte sites */
    bt_rom_rng_reset();
    feed_seq(100u, 2u);
    CHECK(bt_rom_rng_hook(0x00031EE9u, NULL, legacy) == 77u, "word served from a 2-byte pool");
    CHECK(bt_rom_rng_hook(0x00031401u, NULL, legacy) == 100u, "byte after a starved word");
    CHECK(bt_rom_rng_hook(0x00031D77u, block, legacy) == 77u, "nonce block from a 1-byte pool");
    bt_rom_rng_stats(&st);
    CHECK(st.starved == 2u, "starved %u", st.starved);

    /* full pool refuses more; wrap-around keeps order over many cycles */
    bt_rom_rng_reset();
    uint8_t big[BT_ROM_RNG_POOL_BYTES + 10u];
    memset(big, 0x5A, sizeof(big));
    CHECK(bt_rom_rng_feed(big, sizeof(big)) == BT_ROM_RNG_POOL_BYTES, "overfeed accepted");
    CHECK(bt_rom_rng_space() == 0u, "space when full");
    bt_rom_rng_reset();
    uint8_t expect = 0u, next = 0u;
    int order_ok = 1;
    for (int round = 0; round < 40; round++) {
        uint8_t chunk[97];
        size_t n = bt_rom_rng_space() < sizeof(chunk) ? bt_rom_rng_space() : sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = next++;
        }
        CHECK(bt_rom_rng_feed(chunk, n) == n, "feed in round %d", round);
        for (int k = 0; k < 60; k++) {
            order_ok &= bt_rom_rng_hook(0x00031401u, NULL, legacy) == expect++;
        }
    }
    CHECK(order_ok, "pool order broken across wrap");
    CHECK(bt_rom_rng_feed(NULL, 4u) == 0u, "NULL feed");
}

/* ---- static re-derivation from the ROM dump ---- */
static uint16_t hw(const uint8_t *rom, uint32_t a) { return (uint16_t)(rom[a] | rom[a + 1] << 8); }

/* Thumb-2 BL (T1) or B.W (T4) at a: returns 1 and the target, else 0 */
static int decode_bl(const uint8_t *rom, uint32_t a, uint32_t *target, int *is_bl)
{
    uint16_t h1 = hw(rom, a), h2 = hw(rom, a + 2);
    if ((h1 & 0xF800u) != 0xF000u) {
        return 0;
    }
    uint32_t op = h2 & 0xD000u;
    if (op != 0xD000u && op != 0x9000u) {
        return 0;
    }
    uint32_t s = (h1 >> 10) & 1u, j1 = (h2 >> 13) & 1u, j2 = (h2 >> 11) & 1u;
    uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
    uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((uint32_t)(h1 & 0x3FFu) << 12) | ((uint32_t)(h2 & 0x7FFu) << 1);
    if (s) {
        imm |= 0xFE000000u;
    }
    *target = a + 4u + imm;
    *is_bl = op == 0xD000u;
    return 1;
}

static void test_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  ROM dump %s not found: static re-derivation skipped\n", path);
        return;
    }
    static uint8_t rom[384u * 1024u];
    size_t n = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    CHECK(n == sizeof(rom), "short ROM dump (%zu)", n);
    /* thunk: ldr r3,[pc,#4]; ldr r3,[r3,#8]; bx r3; nop; .word 0x00803C1C */
    CHECK(hw(rom, 0x3DE98) == 0x4B01u && hw(rom, 0x3DE9A) == 0x689Bu && hw(rom, 0x3DE9C) == 0x4718u &&
          (rom[0x3DEA0] | rom[0x3DEA1] << 8 | rom[0x3DEA2] << 16 | (uint32_t)rom[0x3DEA3] << 24) == 0x00803C1Cu,
          "RNG thunk at 0x3DE98 is not ldr/ldr [+8]/bx over 0x00803C1C");
    int found = 0, bl_only = 1, in_table = 1;
    for (uint32_t a = 0; a + 4u <= n; a += 2u) {
        uint32_t t;
        int is_bl;
        if (decode_bl(rom, a, &t, &is_bl) && t == 0x3DE98u) {
            found++;
            bl_only &= is_bl;
            int kind = bt_rom_rng_site(a + 4u);
            in_table &= kind != BT_ROM_RNG_SITE_UNKNOWN;
            if (kind == BT_ROM_RNG_SITE_UNKNOWN) {
                printf("  untabled call site 0x%05X\n", a);
            }
        }
    }
    /* Halfword scanning can also decode a BL out of the middle of other instructions or literal data; the table check
     * holds either way (a false hit not in the table would fail). */
    CHECK(found == 11, "expected 11 calls of the RNG thunk, found %d", found);
    CHECK(bl_only && in_table, "a thunk call is a tail branch or is missing from the site table");
    /* the fixed pair the ROM's rom_lm_sec_p192_init copies: d at 0xD72C+0xC0, X||Y at 0xD72C+0x120 */
    uint8_t q[48];
    CHECK(bt_p192_pubkey(&rom[0xD7EC], q) == 0 && memcmp(q, &rom[0xD84C], 48) == 0,
          "ROM 0xD84C is not d * G for d at 0xD7EC");
    printf("  ROM: %d thunk calls, all tabled; fixed P-192 pair at 0xD7EC/0xD84C confirmed\n", found);
}

int main(int argc, char **argv)
{
    test_sites();
    test_hook();
    if (argc > 1) {
        test_rom(argv[1]);
    }
    if (g_fail) {
        printf("test_bt_rom_rng: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_bt_rom_rng: all tests passed\n");
    return 0;
}
