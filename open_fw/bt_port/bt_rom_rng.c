/* ROM RNG hook core (see bt_rom_rng.h). Pure C: host-tested in tests/test_bt_rom_rng.c, linked into every BT=1 image
 * through bt_rom_real.c.
 * SPDX-License-Identifier: MIT */
#include "bt_rom_rng.h"

#if (BT_ROM_RNG_POOL_BYTES & (BT_ROM_RNG_POOL_BYTES - 1u)) != 0u
#error "BT_ROM_RNG_POOL_BYTES must be a power of two"
#endif

static uint8_t s_pool[BT_ROM_RNG_POOL_BYTES];
static volatile uint32_t s_head;   /* written by the producer only */
static volatile uint32_t s_tail;   /* written by the consumer only */
static bt_rom_rng_stats_t s_st;

typedef struct {
    uint32_t ret;   /* thumb bit clear */
    uint8_t kind;
} site_t;

static const site_t k_sites[] = {
    { 0x000312FEu, BT_ROM_RNG_SITE_LEGACY },       /* 0x312F4 range rand (timer) */
    { 0x00031400u, BT_ROM_RNG_SITE_BYTE },         /* 0x313F4 16-byte LMP random */
    { 0x00031D76u, BT_ROM_RNG_SITE_NONCE_FIRST },  /* 0x31D6C SSP nonce, 1st hook byte */
    { 0x00031D80u, BT_ROM_RNG_SITE_BYTE },         /* 0x31D6C 2nd */
    { 0x00031D88u, BT_ROM_RNG_SITE_BYTE },         /* 0x31D6C 3rd */
    { 0x00031D90u, BT_ROM_RNG_SITE_BYTE },         /* 0x31D6C 4th */
    { 0x00031EE8u, BT_ROM_RNG_SITE_WORD },         /* 0x31EE2 passkey % 1000000 */
    { 0x00035780u, BT_ROM_RNG_SITE_LEGACY },       /* 0x3577C slot/backoff */
    { 0x00039C30u, BT_ROM_RNG_SITE_LEGACY },       /* 0x39C2C AFH channel pick */
    { 0x00042F78u, BT_ROM_RNG_SITE_LEGACY },       /* 0x42F74 PDU fields */
    { 0x00044152u, BT_ROM_RNG_SITE_LEGACY },       /* 0x4414E slot offset */
};

int bt_rom_rng_site(uint32_t ret_addr)
{
    uint32_t a = ret_addr & ~1u;
    for (unsigned i = 0; i < sizeof(k_sites) / sizeof(k_sites[0]); i++) {
        if (k_sites[i].ret == a) {
            return k_sites[i].kind;
        }
    }
    return BT_ROM_RNG_SITE_UNKNOWN;
}

static void wipe(volatile uint8_t *p, size_t n)
{
    while (n--) {
        *p++ = 0u;
    }
}

void bt_rom_rng_reset(void)
{
    wipe(s_pool, sizeof(s_pool));
    s_head = 0u;
    s_tail = 0u;
    bt_rom_rng_stats_t z = { 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    s_st = z;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

size_t bt_rom_rng_space(void)
{
    uint32_t used = s_head - s_tail;
    return (size_t)(BT_ROM_RNG_POOL_BYTES - used);
}

size_t bt_rom_rng_feed(const uint8_t *p, size_t n)
{
    if (p == NULL) {
        return 0u;
    }
    uint32_t head = s_head;
    uint32_t tail = s_tail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);       /* the consumer's wipe of [old tail, tail) is done */
    size_t room = (size_t)(BT_ROM_RNG_POOL_BYTES - (head - tail));
    if (n > room) {
        n = room;
    }
    for (size_t i = 0; i < n; i++) {
        s_pool[(head + (uint32_t)i) & (BT_ROM_RNG_POOL_BYTES - 1u)] = p[i];
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);       /* bytes visible before the index that publishes them */
    s_head = head + (uint32_t)n;
    s_st.fed_bytes += (uint32_t)n;
    return n;
}

/* Consumer: copy n bytes out and wipe them, or take nothing. */
static int take(uint8_t *out, uint32_t n)
{
    uint32_t tail = s_tail;
    uint32_t head = s_head;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (head - tail < n) {
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        volatile uint8_t *b = &s_pool[(tail + i) & (BT_ROM_RNG_POOL_BYTES - 1u)];
        out[i] = *b;
        *b = 0u;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_tail = tail + n;
    return 0;
}

uint32_t bt_rom_rng_hook(uint32_t ret_addr, volatile uint8_t *nonce_block, uint32_t (*legacy)(void))
{
    uint8_t b[BT_ROM_NONCE_BLOCK_BYTES + 1u];
    uint32_t v = 0u;
    int kind = bt_rom_rng_site(ret_addr);
    switch (kind) {
    case BT_ROM_RNG_SITE_BYTE:
        if (take(b, 1u) == 0) {
            v = b[0];
            s_st.drbg_calls++;
            goto out;
        }
        break;
    case BT_ROM_RNG_SITE_WORD:
        if (take(b, 4u) == 0) {
            v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            s_st.drbg_calls++;
            goto out;
        }
        break;
    case BT_ROM_RNG_SITE_NONCE_FIRST:
        if (nonce_block != NULL && take(b, BT_ROM_NONCE_BLOCK_BYTES + 1u) == 0) {
            for (uint32_t i = 0; i < BT_ROM_NONCE_BLOCK_BYTES; i++) {
                nonce_block[i] = b[i];
            }
            v = b[BT_ROM_NONCE_BLOCK_BYTES];
            s_st.drbg_calls++;
            s_st.nonce_fills++;
            goto out;
        }
        break;
    case BT_ROM_RNG_SITE_LEGACY:
        s_st.legacy_calls++;
        return legacy();
    default:
        s_st.unknown_calls++;
        s_st.last_unknown = ret_addr;
        return legacy();
    }
    s_st.starved++;                                 /* security site, pool short: cannot fail here */
    return legacy();
out:
    wipe(b, sizeof(b));
    return v;
}

void bt_rom_rng_stats(bt_rom_rng_stats_t *out)
{
    if (out != NULL) {
        *out = s_st;
    }
}
