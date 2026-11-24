/* HMAC_DRBG with SHA-256 (NIST SP 800-90A Rev.1 §10.1.2), no prediction resistance, no derivation function.
 * Pure C, no allocation, not thread-safe (bt_entropy.c serialises access). Verified against NIST CAVP vectors
 * (tests/test_bt_entropy.c). Plan: docs/BLUETOOTH_BLUEDROID_PLAN.md M2.
 * SPDX-License-Identifier: MIT */
#ifndef BT_DRBG_H
#define BT_DRBG_H

#include <stddef.h>
#include <stdint.h>

#define BT_DRBG_SEEDLEN            32u
#define BT_DRBG_MAX_REQUEST        4096u   /* bytes per generate (SP 800-90A allows 2^16; we never need more) */
#define BT_DRBG_RESEED_INTERVAL    10000u  /* generate calls between reseeds (SP 800-90A allows 2^48) */

typedef enum {
    BT_DRBG_OK = 0,
    BT_DRBG_ERR_NOT_INSTANTIATED = -1,
    BT_DRBG_ERR_RESEED_REQUIRED = -2,
    BT_DRBG_ERR_BAD_ARG = -3,
} bt_drbg_status_t;

typedef struct {
    uint8_t  k[32];
    uint8_t  v[32];
    uint32_t reseed_counter;
    uint8_t  instantiated;
} bt_drbg_t;

/* entropy_len must be >= 32 (security strength 256); nonce_len >= 16. */
int  bt_drbg_instantiate(bt_drbg_t *d, const uint8_t *entropy, size_t entropy_len, const uint8_t *nonce,
                         size_t nonce_len, const uint8_t *pers, size_t pers_len);
int  bt_drbg_reseed(bt_drbg_t *d, const uint8_t *entropy, size_t entropy_len, const uint8_t *addl, size_t addl_len);
int  bt_drbg_generate(bt_drbg_t *d, uint8_t *out, size_t out_len, const uint8_t *addl, size_t addl_len);
int  bt_drbg_reseed_due(const bt_drbg_t *d);
void bt_drbg_uninstantiate(bt_drbg_t *d);

#endif /* BT_DRBG_H */
