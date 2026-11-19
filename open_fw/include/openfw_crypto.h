/*
 * Smartlink SL6801 OpenFW Standalone Cryptography & Integrity Primitives
 *
 * Clean-room FIPS 180-4 SHA-256, RFC 2104 HMAC-SHA256, CRC-16/CCITT-FALSE,
 * and constant-time equality comparison (zero dynamic allocation).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_CRYPTO_H
#define OPENFW_CRYPTO_H

#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    uint32_t buf_len;
    uint64_t total_len;
} openfw_sha256_ctx_t;

uint16_t openfw_crc16_ccitt_false(const uint8_t *buf, uint32_t len);

void openfw_sha256_init(openfw_sha256_ctx_t *ctx);
void openfw_sha256_update(openfw_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void openfw_sha256_final(openfw_sha256_ctx_t *ctx, uint8_t out[32]);
void openfw_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);

void openfw_hmac_sha256(const uint8_t *key, uint32_t key_len,
                        const uint8_t *msg, uint32_t msg_len, uint8_t out[32]);

int openfw_ct_memeq(const uint8_t *a, const uint8_t *b, uint32_t n);

#endif /* OPENFW_CRYPTO_H */
