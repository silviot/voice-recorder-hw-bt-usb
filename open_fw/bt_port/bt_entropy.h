/* Entropy pool + DRBG front end for Bluetooth security material (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M2).
 *
 * - Raw noise sources register with a claimed min-entropy per 8-bit sample (h_x16 = bits * 16) and SP 800-90B
 *   §4.4 health-test cutoffs (repetition count, adaptive proportion over a 512-sample window). The cutoffs are
 *   constants computed offline; tests/test_bt_entropy.c recomputes them exactly from h_x16 (alpha = 2^-20).
 * - A source earns credit only after its 1024-sample start-up test passed. Any health failure latches the module
 *   into BT_ENT_HEALTH_FAIL until the next boot (SP 800-90B: failures are not silently cleared).
 * - All samples (credited or not) and uncredited material (e.g. a seed file from SD) are hashed into a SHA-256 pool.
 *   The HMAC_DRBG is instantiated once >= BT_ENT_SEED_BITS are credited, and reseeded whenever >= BT_ENT_RESEED_BITS
 *   of fresh credit exist; if a reseed is due and no credit is available, output fails closed.
 * - Every output call is fallible. There is no fallback randomness anywhere.
 *
 * Thread safety: the platform passes lock/unlock callbacks to bt_entropy_init (a FreeRTOS mutex on target).
 * Never call from interrupt context.
 * SPDX-License-Identifier: MIT */
#ifndef BT_ENTROPY_H
#define BT_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_ENT_MAX_SOURCES   4u
#define BT_ENT_APT_WINDOW    512u
#define BT_ENT_STARTUP_SAMPLES 1024u
#define BT_ENT_SEED_BITS     384u   /* credited bits before first instantiate (256 strength + margin) */
#define BT_ENT_RESEED_BITS   320u   /* credited bits for a reseed (256 + 64, SP 800-90B vetted conditioning) */

typedef enum {
    BT_ENT_OK = 0,
    BT_ENT_NOT_READY = -1,     /* not enough credited entropy yet (or reseed due without fresh credit) */
    BT_ENT_HEALTH_FAIL = -2,   /* a noise source failed a health test; latched until reboot */
    BT_ENT_BAD_ARG = -3,
} bt_ent_status_t;

typedef struct {
    const char *name;
    uint16_t h_x16;        /* claimed min-entropy per sample, in 1/16 bit (1..128) */
    uint16_t rct_cutoff;   /* = 1 + ceil(20 / H) */
    uint16_t apt_cutoff;   /* = 1 + CRITBINOM(512, 2^-H, 1 - 2^-20) */
} bt_ent_source_cfg_t;

typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} bt_ent_platform_t;

typedef struct {
    uint32_t credited_bits_total;
    uint32_t reseeds;
    uint32_t health_failures;
    int8_t   failed_source;   /* -1 if none */
    uint8_t  failed_test;     /* 1 = RCT, 2 = APT */
    bool     instantiated;
} bt_ent_stats_t;

/* platform may be NULL (single-threaded host tests). Resets all state. */
void bt_entropy_init(const bt_ent_platform_t *platform);

/* Returns a source id >= 0, or BT_ENT_BAD_ARG. cfg must outlive the module. */
int bt_entropy_register_source(const bt_ent_source_cfg_t *cfg);

/* Feed raw 8-bit samples from a registered source: health-tested, hashed into the pool, credited when allowed.
 * Returns BT_ENT_OK, BT_ENT_HEALTH_FAIL (latched) or BT_ENT_BAD_ARG. */
int bt_entropy_add_samples(int source_id, const uint8_t *samples, size_t n);

/* Mix material into the pool with zero credit (seed file, device identity, boot counters). */
void bt_entropy_mix_uncredited(const void *data, size_t n);

bool bt_entropy_ready(void);

/* Fill out[0..n) from the DRBG (n <= 4096). Fails closed: BT_ENT_NOT_READY / BT_ENT_HEALTH_FAIL / BT_ENT_BAD_ARG. */
int bt_entropy_get(void *out, size_t n);

void bt_entropy_get_stats(bt_ent_stats_t *out);

#endif /* BT_ENTROPY_H */
