/* Entropy pool + HMAC_DRBG front end. See bt_entropy.h.
 * SPDX-License-Identifier: MIT */
#include "bt_entropy.h"
#include "bt_drbg.h"
#include "openfw_crypto.h"

typedef struct {
    const bt_ent_source_cfg_t *cfg;
    uint32_t processed;      /* samples seen since registration */
    /* repetition count test */
    uint8_t  rct_last;
    uint16_t rct_run;
    /* adaptive proportion test */
    uint8_t  apt_ref;
    uint16_t apt_count;
    uint16_t apt_pos;        /* 0 = start a new window with the next sample */
} source_t;

static struct {
    bt_ent_platform_t plat;
    int have_plat;
    source_t src[BT_ENT_MAX_SOURCES];
    unsigned n_src;
    openfw_sha256_ctx_t pool;
    uint32_t pool_credit_x16;     /* credited 1/16 bits since the pool was last drained */
    bt_drbg_t drbg;
    bt_ent_stats_t stats;
    int failed;
} g;

static void lock(void)
{
    if (g.have_plat && g.plat.lock != NULL) {
        g.plat.lock(g.plat.ctx);
    }
}

static void unlock(void)
{
    if (g.have_plat && g.plat.unlock != NULL) {
        g.plat.unlock(g.plat.ctx);
    }
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) {
        *b++ = 0u;
    }
}

void bt_entropy_init(const bt_ent_platform_t *platform)
{
    wipe(&g, sizeof(g));
    if (platform != NULL) {
        g.plat = *platform;
        g.have_plat = 1;
    }
    openfw_sha256_init(&g.pool);
    g.stats.failed_source = -1;
}

int bt_entropy_register_source(const bt_ent_source_cfg_t *cfg)
{
    if (cfg == NULL || cfg->h_x16 == 0u || cfg->h_x16 > 128u || cfg->rct_cutoff < 2u || cfg->apt_cutoff < 2u ||
        cfg->apt_cutoff > BT_ENT_APT_WINDOW) {
        return BT_ENT_BAD_ARG;
    }
    lock();
    int id = BT_ENT_BAD_ARG;
    if (g.n_src < BT_ENT_MAX_SOURCES) {
        id = (int)g.n_src++;
        g.src[id].cfg = cfg;
    }
    unlock();
    return id;
}

static void fail_latched(int source_id, uint8_t test)
{
    g.failed = 1;
    g.stats.health_failures++;
    if (g.stats.failed_source < 0) {
        g.stats.failed_source = (int8_t)source_id;
        g.stats.failed_test = test;
    }
    bt_drbg_uninstantiate(&g.drbg);
    g.stats.instantiated = false;
    g.pool_credit_x16 = 0u;
}

/* SP 800-90B §4.4.1 / §4.4.2 on one sample. Returns 0 if the sample passes, 1 = RCT failure, 2 = APT failure. */
static int health_step(source_t *s, uint8_t x)
{
    const bt_ent_source_cfg_t *c = s->cfg;
    if (s->processed == 0u || x != s->rct_last) {
        s->rct_last = x;
        s->rct_run = 1u;
    } else if (++s->rct_run >= c->rct_cutoff) {
        return 1;
    }
    if (s->apt_pos == 0u) {
        s->apt_ref = x;
        s->apt_count = 1u;
        s->apt_pos = 1u;
    } else {
        if (x == s->apt_ref && ++s->apt_count >= c->apt_cutoff) {
            return 2;
        }
        if (++s->apt_pos >= BT_ENT_APT_WINDOW) {
            s->apt_pos = 0u;
        }
    }
    s->processed++;
    return 0;
}

int bt_entropy_add_samples(int source_id, const uint8_t *samples, size_t n)
{
    if (samples == NULL && n != 0u) {
        return BT_ENT_BAD_ARG;
    }
    lock();
    if (source_id < 0 || (unsigned)source_id >= g.n_src) {
        unlock();
        return BT_ENT_BAD_ARG;
    }
    if (g.failed) {
        unlock();
        return BT_ENT_HEALTH_FAIL;
    }
    source_t *s = &g.src[source_id];
    uint32_t creditable = 0u;
    for (size_t i = 0; i < n; i++) {
        int r = health_step(s, samples[i]);
        if (r != 0) {
            fail_latched(source_id, (uint8_t)r);
            unlock();
            return BT_ENT_HEALTH_FAIL;
        }
        if (s->processed > BT_ENT_STARTUP_SAMPLES) {
            creditable++;
        }
    }
    /* Hash everything (start-up samples too: they cannot hurt), with a source tag for domain separation. */
    uint8_t tag[2] = { 0xE5u, (uint8_t)source_id };
    openfw_sha256_update(&g.pool, tag, 2u);
    if (n != 0u) {
        openfw_sha256_update(&g.pool, samples, (uint32_t)n);
    }
    uint32_t add = creditable * s->cfg->h_x16;
    g.pool_credit_x16 = (g.pool_credit_x16 > 0xFFFFFFFFu - add) ? 0xFFFFFFFFu : g.pool_credit_x16 + add;
    g.stats.credited_bits_total += add / 16u;
    unlock();
    return BT_ENT_OK;
}

void bt_entropy_mix_uncredited(const void *data, size_t n)
{
    if (data == NULL || n == 0u) {
        return;
    }
    lock();
    uint8_t tag[2] = { 0xE5u, 0xFFu };
    openfw_sha256_update(&g.pool, tag, 2u);
    openfw_sha256_update(&g.pool, (const uint8_t *)data, (uint32_t)n);
    unlock();
}

/* Derive a labelled 32-byte digest from the pool without finalising the running context. */
static void pool_extract(uint8_t label, uint8_t out[32])
{
    openfw_sha256_ctx_t c = g.pool;
    openfw_sha256_update(&c, &label, 1u);
    openfw_sha256_final(&c, out);
    wipe(&c, sizeof(c));
}

/* Restart the pool keyed by its own previous state, so drained material cannot be replayed. */
static void pool_restart(void)
{
    uint8_t carry[32];
    pool_extract('C', carry);
    openfw_sha256_init(&g.pool);
    openfw_sha256_update(&g.pool, carry, 32u);
    wipe(carry, sizeof(carry));
    g.pool_credit_x16 = 0u;
}

/* Called with the lock held. Instantiates or reseeds when credit allows. */
static void maybe_seed(void)
{
    uint8_t e[32], nonce[32];
    if (!g.drbg.instantiated) {
        if (g.pool_credit_x16 < BT_ENT_SEED_BITS * 16u) {
            return;
        }
        pool_extract('E', e);
        pool_extract('N', nonce);   /* unique per instantiation; carries no extra credit */
        (void)bt_drbg_instantiate(&g.drbg, e, 32u, nonce, 16u, (const uint8_t *)"openfw-bt", 9u);
        g.stats.instantiated = true;
        pool_restart();
    } else if (g.pool_credit_x16 >= BT_ENT_RESEED_BITS * 16u) {
        pool_extract('R', e);
        (void)bt_drbg_reseed(&g.drbg, e, 32u, NULL, 0u);
        g.stats.reseeds++;
        pool_restart();
    }
    wipe(e, sizeof(e));
    wipe(nonce, sizeof(nonce));
}

bool bt_entropy_ready(void)
{
    lock();
    if (!g.failed) {
        maybe_seed();
    }
    bool ok = !g.failed && g.drbg.instantiated && !bt_drbg_reseed_due(&g.drbg);
    unlock();
    return ok;
}

int bt_entropy_get(void *out, size_t n)
{
    if ((out == NULL && n != 0u) || n > BT_DRBG_MAX_REQUEST) {
        return BT_ENT_BAD_ARG;
    }
    lock();
    if (g.failed) {
        unlock();
        return BT_ENT_HEALTH_FAIL;
    }
    maybe_seed();
    int r = bt_drbg_generate(&g.drbg, (uint8_t *)out, n, NULL, 0u);
    unlock();
    if (r == BT_DRBG_OK) {
        return BT_ENT_OK;
    }
    if (out != NULL) {
        wipe(out, n);
    }
    return (r == BT_DRBG_ERR_BAD_ARG) ? BT_ENT_BAD_ARG : BT_ENT_NOT_READY;
}

void bt_entropy_get_stats(bt_ent_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    *out = g.stats;
    unlock();
}
