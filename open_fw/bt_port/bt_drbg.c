/* HMAC_DRBG (SHA-256), NIST SP 800-90A Rev.1 §10.1.2. See bt_drbg.h.
 * SPDX-License-Identifier: MIT */
#include "bt_drbg.h"
#include "openfw_crypto.h"

/* Incremental HMAC-SHA256 with a 32-byte key (the DRBG key K is always 32 bytes). */
typedef struct {
    openfw_sha256_ctx_t inner;
    uint8_t opad_key[64];
} hmac_ctx_t;

static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) {
        *b++ = 0u;
    }
}

static void hmac_begin(hmac_ctx_t *h, const uint8_t key[32])
{
    uint8_t ipad_key[64];
    for (unsigned i = 0; i < 64u; i++) {
        uint8_t k = (i < 32u) ? key[i] : 0u;
        ipad_key[i] = (uint8_t)(k ^ 0x36u);
        h->opad_key[i] = (uint8_t)(k ^ 0x5Cu);
    }
    openfw_sha256_init(&h->inner);
    openfw_sha256_update(&h->inner, ipad_key, 64u);
    secure_zero(ipad_key, sizeof(ipad_key));
}

static void hmac_add(hmac_ctx_t *h, const uint8_t *data, size_t len)
{
    if (len != 0u) {
        openfw_sha256_update(&h->inner, data, (uint32_t)len);
    }
}

static void hmac_end(hmac_ctx_t *h, uint8_t out[32])
{
    uint8_t inner_digest[32];
    openfw_sha256_ctx_t outer;
    openfw_sha256_final(&h->inner, inner_digest);
    openfw_sha256_init(&outer);
    openfw_sha256_update(&outer, h->opad_key, 64u);
    openfw_sha256_update(&outer, inner_digest, 32u);
    openfw_sha256_final(&outer, out);
    secure_zero(inner_digest, sizeof(inner_digest));
    secure_zero(h, sizeof(*h));
    secure_zero(&outer, sizeof(outer));
}

/* HMAC_DRBG_Update with provided_data = a || b || c (any part may be empty). */
static void drbg_update(bt_drbg_t *d, const uint8_t *a, size_t al, const uint8_t *b, size_t bl,
                        const uint8_t *c, size_t cl)
{
    static const uint8_t sep[2] = { 0x00u, 0x01u };
    const int provided = (al + bl + cl) != 0u;
    for (unsigned round = 0; round < (provided ? 2u : 1u); round++) {
        hmac_ctx_t h;
        hmac_begin(&h, d->k);
        hmac_add(&h, d->v, 32u);
        hmac_add(&h, &sep[round], 1u);
        hmac_add(&h, a, al);
        hmac_add(&h, b, bl);
        hmac_add(&h, c, cl);
        hmac_end(&h, d->k);
        hmac_begin(&h, d->k);
        hmac_add(&h, d->v, 32u);
        hmac_end(&h, d->v);
    }
}

int bt_drbg_instantiate(bt_drbg_t *d, const uint8_t *entropy, size_t entropy_len, const uint8_t *nonce,
                        size_t nonce_len, const uint8_t *pers, size_t pers_len)
{
    if (d == NULL || entropy == NULL || entropy_len < 32u || nonce == NULL || nonce_len < 16u ||
        (pers == NULL && pers_len != 0u)) {
        return BT_DRBG_ERR_BAD_ARG;
    }
    for (unsigned i = 0; i < 32u; i++) {
        d->k[i] = 0x00u;
        d->v[i] = 0x01u;
    }
    drbg_update(d, entropy, entropy_len, nonce, nonce_len, pers, pers_len);
    d->reseed_counter = 1u;
    d->instantiated = 1u;
    return BT_DRBG_OK;
}

int bt_drbg_reseed(bt_drbg_t *d, const uint8_t *entropy, size_t entropy_len, const uint8_t *addl, size_t addl_len)
{
    if (d == NULL || entropy == NULL || entropy_len < 32u || (addl == NULL && addl_len != 0u)) {
        return BT_DRBG_ERR_BAD_ARG;
    }
    if (!d->instantiated) {
        return BT_DRBG_ERR_NOT_INSTANTIATED;
    }
    drbg_update(d, entropy, entropy_len, addl, addl_len, NULL, 0u);
    d->reseed_counter = 1u;
    return BT_DRBG_OK;
}

int bt_drbg_reseed_due(const bt_drbg_t *d)
{
    return d->instantiated && d->reseed_counter > BT_DRBG_RESEED_INTERVAL;
}

int bt_drbg_generate(bt_drbg_t *d, uint8_t *out, size_t out_len, const uint8_t *addl, size_t addl_len)
{
    if (d == NULL || (out == NULL && out_len != 0u) || out_len > BT_DRBG_MAX_REQUEST ||
        (addl == NULL && addl_len != 0u)) {
        return BT_DRBG_ERR_BAD_ARG;
    }
    if (!d->instantiated) {
        return BT_DRBG_ERR_NOT_INSTANTIATED;
    }
    if (d->reseed_counter > BT_DRBG_RESEED_INTERVAL) {
        return BT_DRBG_ERR_RESEED_REQUIRED;
    }
    if (addl_len != 0u) {
        drbg_update(d, addl, addl_len, NULL, 0u, NULL, 0u);
    }
    size_t done = 0u;
    while (done < out_len) {
        hmac_ctx_t h;
        hmac_begin(&h, d->k);
        hmac_add(&h, d->v, 32u);
        hmac_end(&h, d->v);
        size_t n = out_len - done;
        if (n > 32u) {
            n = 32u;
        }
        for (size_t i = 0; i < n; i++) {
            out[done + i] = d->v[i];
        }
        done += n;
    }
    drbg_update(d, addl, addl_len, NULL, 0u, NULL, 0u);
    d->reseed_counter++;
    return BT_DRBG_OK;
}

void bt_drbg_uninstantiate(bt_drbg_t *d)
{
    if (d != NULL) {
        secure_zero(d, sizeof(*d));
    }
}
