/*
 * Smartlink SL6801 OpenFW Standalone Cryptography & Integrity Primitives
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_crypto.h"

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static void zero_bytes(uint8_t *dst, uint32_t n)
{
    volatile uint8_t *p = dst;
    for (uint32_t i = 0u; i < n; i++) {
        p[i] = 0u;
    }
}

uint16_t openfw_crc16_ccitt_false(const uint8_t *buf, uint32_t len)
{
    uint32_t c = 0xFFFFu;
    for (uint32_t i = 0u; i < len; i++) {
        uint32_t t = ((c >> 8) ^ buf[i]) & 0xFFu;
        t ^= (t >> 4);
        c = ((c << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFFu;
    }
    return (uint16_t)c;
}

static void sha256_compress(openfw_sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[16];
    if (((uintptr_t)block & 3u) == 0u) {
        const uint32_t *b32 = (const uint32_t *)(const void *)block;
        for (uint32_t i = 0u; i < 16u; i++) {
            w[i] = __builtin_bswap32(b32[i]);
        }
    } else {
        for (uint32_t i = 0u; i < 16u; i++) {
            w[i] = ((uint32_t)block[i * 4u] << 24)
                 | ((uint32_t)block[i * 4u + 1u] << 16)
                 | ((uint32_t)block[i * 4u + 2u] << 8)
                 | ((uint32_t)block[i * 4u + 3u]);
        }
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (uint32_t i = 0u; i < 64u; i++) {
        uint32_t wi;
        if (i < 16u) {
            wi = w[i];
        } else {
            uint32_t w15 = w[(i - 15u) & 15u];
            uint32_t w2  = w[(i - 2u) & 15u];
            uint32_t s0  = rotr32(w15, 7u) ^ rotr32(w15, 18u) ^ (w15 >> 3);
            uint32_t s1  = rotr32(w2, 17u) ^ rotr32(w2, 19u) ^ (w2 >> 10);
            wi = w[(i - 16u) & 15u] + s0 + w[(i - 7u) & 15u] + s1;
            w[i & 15u] = wi;
        }
        uint32_t S1  = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch  = (e & f) ^ ((~e) & g);
        uint32_t t1  = h + S1 + ch + SHA256_K[i] + wi;
        uint32_t S0  = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
    zero_bytes((uint8_t *)w, (uint32_t)sizeof(w));
}

void openfw_sha256_init(openfw_sha256_ctx_t *ctx)
{
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->buf_len = 0u;
    ctx->total_len = 0u;
}

void openfw_sha256_update(openfw_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    ctx->total_len += (uint64_t)len;
    if (ctx->buf_len > 0u) {
        uint32_t take = 64u - ctx->buf_len;
        if (take > len) {
            take = len;
        }
        for (uint32_t i = 0u; i < take; i++) {
            ctx->buf[ctx->buf_len + i] = data[i];
        }
        ctx->buf_len += take;
        data += take;
        len  -= take;
        if (ctx->buf_len == 64u) {
            sha256_compress(ctx, ctx->buf);
            ctx->buf_len = 0u;
        }
    }
    while (len >= 64u) {
        sha256_compress(ctx, data);
        data += 64u;
        len  -= 64u;
    }
    for (uint32_t i = 0u; i < len; i++) {
        ctx->buf[i] = data[i];
    }
    ctx->buf_len += len;
}

void openfw_sha256_final(openfw_sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t total_bits = ctx->total_len << 3;
    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > 56u) {
        while (ctx->buf_len < 64u) {
            ctx->buf[ctx->buf_len++] = 0u;
        }
        sha256_compress(ctx, ctx->buf);
        ctx->buf_len = 0u;
    }
    while (ctx->buf_len < 56u) {
        ctx->buf[ctx->buf_len++] = 0u;
    }
    for (uint32_t i = 0u; i < 8u; i++) {
        ctx->buf[56u + i] = (uint8_t)(total_bits >> ((7u - i) * 8u));
    }
    sha256_compress(ctx, ctx->buf);
    for (uint32_t i = 0u; i < 8u; i++) {
        out[i * 4u]      = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)(ctx->h[i]);
    }
}

void openfw_sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    openfw_sha256_ctx_t ctx;
    openfw_sha256_init(&ctx);
    openfw_sha256_update(&ctx, data, len);
    openfw_sha256_final(&ctx, out);
}

void openfw_hmac_sha256(const uint8_t *key, uint32_t key_len,
                        const uint8_t *msg, uint32_t msg_len, uint8_t out[32])
{
    uint8_t key_blk[64];
    uint8_t pad[64];
    uint8_t inner[32];
    openfw_sha256_ctx_t ctx;

    zero_bytes(key_blk, sizeof(key_blk));
    if (key_len > 64u) {
        openfw_sha256(key, key_len, key_blk);
    } else {
        for (uint32_t i = 0u; i < key_len; i++) {
            key_blk[i] = key[i];
        }
    }

    for (uint32_t i = 0u; i < 64u; i++) {
        pad[i] = (uint8_t)(key_blk[i] ^ 0x36u);
    }
    openfw_sha256_init(&ctx);
    openfw_sha256_update(&ctx, pad, 64u);
    openfw_sha256_update(&ctx, msg, msg_len);
    openfw_sha256_final(&ctx, inner);

    for (uint32_t i = 0u; i < 64u; i++) {
        pad[i] = (uint8_t)(key_blk[i] ^ 0x5Cu);
    }
    openfw_sha256_init(&ctx);
    openfw_sha256_update(&ctx, pad, 64u);
    openfw_sha256_update(&ctx, inner, 32u);
    openfw_sha256_final(&ctx, out);

    zero_bytes((uint8_t *)&ctx, (uint32_t)sizeof(ctx));
    zero_bytes(key_blk, sizeof(key_blk));
    zero_bytes(pad, sizeof(pad));
    zero_bytes(inner, sizeof(inner));
}

int openfw_ct_memeq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    volatile uint8_t diff = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    }
    return diff == 0u;
}
