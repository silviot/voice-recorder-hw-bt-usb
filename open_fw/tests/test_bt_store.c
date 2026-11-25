/* Host tests for bt_port/bt_store.c (plan M2 "Store"): nvs semantics as used by osi/config.c, reference-model
 * random operations (compaction), serialise/load round trip, and a structure-aware load fuzzer (checksum recomputed
 * so mutations reach the bounds checks). Run under ASan/UBSan.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_store.h"
#include "openfw_crypto.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int g_flushes, g_locks;
static void p_lock(void *c) { (void)c; g_locks++; }
static void p_unlock(void *c) { (void)c; g_locks--; }
static void p_flush(void *c) { (void)c; g_flushes++; }
static const bt_store_platform_t k_plat = { p_lock, p_unlock, p_flush, NULL };

static void test_api(void)
{
    bt_store_init(&k_plat);
    nvs_handle_t h, ro;
    uint8_t buf[2048];
    size_t len;
    CHECK(nvs_flash_init() == ESP_OK, "init");
    CHECK(nvs_open("bt_config.conf", NVS_READWRITE, &h) == ESP_OK, "open rw");
    CHECK(nvs_open("this_name_is_too_long", NVS_READWRITE, &ro) == ESP_ERR_NVS_INVALID_NAME, "long ns accepted");
    len = 0;
    CHECK(nvs_get_blob(h, "bt_cfg_key0", NULL, &len) == ESP_ERR_NVS_NOT_FOUND, "missing key found");
    memset(buf, 0xA5, 1536);
    CHECK(nvs_set_blob(h, "bt_cfg_key0", buf, 1536) == ESP_OK, "set chunk 0");
    memset(buf, 0x5A, 700);
    CHECK(nvs_set_blob(h, "bt_cfg_key1", buf, 700) == ESP_OK, "set chunk 1");
    len = 0;
    CHECK(nvs_get_blob(h, "bt_cfg_key0", NULL, &len) == ESP_OK && len == 1536, "length query");
    len = 100;
    CHECK(nvs_get_blob(h, "bt_cfg_key0", buf, &len) == ESP_ERR_NVS_INVALID_LENGTH && len == 1536, "short buffer");
    len = sizeof(buf);
    CHECK(nvs_get_blob(h, "bt_cfg_key1", buf, &len) == ESP_OK && len == 700 && buf[0] == 0x5A && buf[699] == 0x5A,
          "read back chunk 1");
    CHECK(g_flushes == 0, "flush before commit");
    CHECK(nvs_commit(h) == ESP_OK && g_flushes == 1, "commit did not request flush");
    CHECK(nvs_open("bt_config.conf", NVS_READONLY, &ro) == ESP_OK && ro == h, "same namespace handle");
    nvs_close(ro);
    CHECK(nvs_set_blob(h, "k", "x", 1) == ESP_OK, "still writable after other handle closed");

    /* out of space keeps the previous value */
    memset(buf, 0x11, sizeof(buf));
    CHECK(nvs_set_blob(h, "big", buf, 2048) == ESP_ERR_NVS_NOT_ENOUGH_SPACE, "over capacity accepted");
    len = sizeof(buf);
    CHECK(nvs_get_blob(h, "bt_cfg_key1", buf, &len) == ESP_OK && len == 700 && buf[0] == 0x5A,
          "existing value damaged by failed set");
    CHECK(nvs_erase_key(h, "bt_cfg_key0") == ESP_OK, "erase key");
    CHECK(nvs_erase_key(h, "bt_cfg_key0") == ESP_ERR_NVS_NOT_FOUND, "erase twice");
    CHECK(nvs_set_blob(h, "big", buf, 2048) == ESP_OK, "space not reclaimed after erase");
    CHECK(nvs_erase_all(h) == ESP_OK, "erase all");
    len = 0;
    CHECK(nvs_get_blob(h, "big", NULL, &len) == ESP_ERR_NVS_NOT_FOUND, "erase_all left data");
    nvs_close(h);
    CHECK(nvs_get_blob(h, "big", NULL, &len) == ESP_ERR_NVS_INVALID_HANDLE, "closed handle usable");

    CHECK(nvs_open("ro_only", NVS_READONLY, &ro) == ESP_OK, "open ro");
    CHECK(nvs_set_blob(ro, "a", "b", 1) == ESP_ERR_NVS_READ_ONLY, "write through read-only handle");
    nvs_close(ro);
    CHECK(g_locks == 0, "unbalanced lock");
}

/* ---- reference model ---- */
#define MK 12
static struct { int present; size_t len; uint8_t v[600]; } model[MK];
static uint32_t rs = 1u;
static uint32_t rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }

static void check_model(nvs_handle_t h)
{
    uint8_t buf[600];
    for (int k = 0; k < MK; k++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", k);
        size_t len = sizeof(buf);
        esp_err_t r = nvs_get_blob(h, key, buf, &len);
        if (model[k].present) {
            CHECK(r == ESP_OK && len == model[k].len && memcmp(buf, model[k].v, len) == 0, "model mismatch %s", key);
        } else {
            CHECK(r == ESP_ERR_NVS_NOT_FOUND, "model says absent: %s", key);
        }
    }
}

static void test_random_ops(void)
{
    bt_store_init(NULL);
    memset(model, 0, sizeof(model));
    nvs_handle_t h;
    CHECK(nvs_open("rnd", NVS_READWRITE, &h) == ESP_OK, "open");
    for (int it = 0; it < 20000; it++) {
        int k = (int)(rnd() % MK);
        char key[16];
        snprintf(key, sizeof(key), "k%d", k);
        if (rnd() % 3u) {
            size_t len = rnd() % 600u;
            uint8_t v[600];
            for (size_t i = 0; i < len; i++) v[i] = (uint8_t)rnd();
            esp_err_t r = nvs_set_blob(h, key, v, len);
            if (r == ESP_OK) {
                model[k].present = 1; model[k].len = len; memcpy(model[k].v, v, len);
            } else {
                CHECK(r == ESP_ERR_NVS_NOT_ENOUGH_SPACE, "unexpected set error %d", r);
            }
        } else {
            esp_err_t r = nvs_erase_key(h, key);
            CHECK(r == (model[k].present ? ESP_OK : ESP_ERR_NVS_NOT_FOUND), "erase result");
            model[k].present = 0;
        }
        if ((it % 97) == 0) {
            check_model(h);
        }
    }
    check_model(h);

    /* round trip */
    static uint8_t img[BT_STORE_IMAGE_MAX];
    size_t n = bt_store_serialize(img, sizeof(img));
    CHECK(n > 0, "serialise");
    /* review finding: serialise cleared the dirty flag before the caller had written anything, so a failed SD write
     * left the card stale with nothing left to retry. Dirty stays set until the image is marked saved. */
    CHECK(bt_store_dirty(), "dirty flag cleared by serialise alone (a failed write would never be retried)");
    bt_store_mark_saved();
    CHECK(!bt_store_dirty(), "still dirty after the image was marked saved");
    /* a change between serialise and mark_saved keeps the store dirty (that change is not in the stored image) */
    CHECK(nvs_set_blob(h, "late", "x", 1u) == ESP_OK, "late change");
    n = bt_store_serialize(img, sizeof(img));
    CHECK(nvs_erase_key(h, "late") == ESP_OK, "change after serialise");
    bt_store_mark_saved();
    CHECK(bt_store_dirty(), "a change made after serialise was marked saved");
    n = bt_store_serialize(img, sizeof(img));
    bt_store_mark_saved();
    CHECK(!bt_store_dirty(), "clean after serialise + mark_saved");
    CHECK(bt_store_serialize(img, n - 1u) == 0u, "serialise into too-small buffer succeeded");
    n = bt_store_serialize(img, sizeof(img));
    CHECK(bt_store_load(img, n) == ESP_OK, "reload own image");
    check_model(h);
    nvs_close(h);
}

/* ---- load fuzzing ---- */
static void reseal(uint8_t *img, size_t n)
{
    uint8_t d[32];
    openfw_sha256(img, (uint32_t)(n - 8u), d);
    memcpy(img + n - 8u, d, 8u);
}

static void test_load_fuzz(void)
{
    bt_store_init(NULL);
    nvs_handle_t h;
    CHECK(nvs_open("bt_config.conf", NVS_READWRITE, &h) == ESP_OK, "open");
    uint8_t v[300];
    memset(v, 7, sizeof(v));
    for (int k = 0; k < 6; k++) {
        char key[24];
        snprintf(key, sizeof(key), "bt_cfg_key%d", k);
        CHECK(nvs_set_blob(h, key, v, sizeof(v) - (size_t)k * 40u) == ESP_OK, "fill");
    }
    static uint8_t good[BT_STORE_IMAGE_MAX], img[BT_STORE_IMAGE_MAX + 16];
    size_t n = bt_store_serialize(good, sizeof(good));

    /* unsealed bit flips: checksum must reject every one */
    int accepted = 0;
    for (int it = 0; it < 2000; it++) {
        memcpy(img, good, n);
        img[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8u));
        if (bt_store_load(img, n) == ESP_OK) accepted++;
    }
    CHECK(accepted == 0, "checksum accepted %d corrupted images", accepted);

    /* resealed structural mutations and truncations: must never crash; any accepted image must be self-consistent */
    int ok = 0;
    for (int it = 0; it < 20000; it++) {
        size_t m = n;
        memcpy(img, good, n);
        int kind = (int)(rnd() % 4u);
        if (kind == 0) {
            img[4u + rnd() % (n - 12u)] = (uint8_t)rnd();
        } else if (kind == 1) {
            m = 14u + rnd() % (n - 14u);
        } else if (kind == 2) {
            for (int j = 0; j < 4; j++) img[4u + rnd() % (n - 12u)] = (uint8_t)rnd();
        } else {
            img[4] = (uint8_t)rnd(); img[5] = (uint8_t)rnd();
        }
        reseal(img, m);
        if (bt_store_load(img, m) == ESP_OK) {
            ok++;
            static uint8_t again[BT_STORE_IMAGE_MAX];
            size_t n2 = bt_store_serialize(again, sizeof(again));
            CHECK(n2 > 0 && bt_store_load(again, n2) == ESP_OK, "accepted image does not round-trip");
        }
    }
    printf("  fuzz: %d of 20000 resealed mutations parsed as valid stores (rest rejected)\n", ok);
    CHECK(bt_store_load(good, n) == ESP_OK, "good image rejected after fuzzing");
    CHECK(bt_store_load(good, 3) != ESP_OK, "tiny image accepted");
    memcpy(img, good, n); img[0] = 'X'; reseal(img, n);
    CHECK(bt_store_load(img, n) == ESP_ERR_INVALID_VERSION, "bad magic accepted");
    nvs_close(h);
}

/* Write hold (silicon 2026-11-25 headphones bond loss): a non-empty card image the store could not take over is held,
 * so the empty store btc_config_init() saves at every start is never written over it. */
static void test_hold(void)
{
    bt_store_init(NULL);
    CHECK(!bt_store_held(), "fresh store held");
    nvs_handle_t h;
    CHECK(nvs_open("bt_config.conf", NVS_READWRITE, &h) == ESP_OK, "open");
    static const char cfg[] = "[00:11:22:33:44:55]\nLinkKeyType = 4\nLinkKey = 00112233445566778899aabbccddeeff\n";
    CHECK(nvs_set_blob(h, "bt_cfg_key0", cfg, sizeof(cfg) - 1u) == ESP_OK, "set");
    static uint8_t good[BT_STORE_IMAGE_MAX], img[BT_STORE_IMAGE_MAX];
    size_t n = bt_store_serialize(good, sizeof(good));
    CHECK(n > 0u, "serialise");

    memcpy(img, good, n);
    img[n - 1u] ^= 0x01u;                                    /* torn / corrupted digest */
    CHECK(bt_store_load(img, n) == ESP_ERR_INVALID_CRC, "corrupt image accepted");
    CHECK(bt_store_held(), "rejected non-empty image not held (the empty store would replace the card's bonds)");
    CHECK(bt_store_load(good, n) == ESP_OK && !bt_store_held(), "a good load did not clear the hold");
    memcpy(img, good, n); img[0] = 'B'; img[3] = '2'; reseal(img, n);   /* another store version */
    CHECK(bt_store_load(img, n) == ESP_ERR_INVALID_VERSION && bt_store_held(), "other version not held");
    bt_store_reset();
    CHECK(!bt_store_held(), "reset (mode switch) did not clear the hold");
    CHECK(bt_store_load(img, 0u) != ESP_OK && !bt_store_held(), "an empty file is held");

    /* patch 0045: Bluedroid could not parse the config: held only when the store holds something */
    bt_store_reset();
    bt_store_hold_nonempty();
    CHECK(!bt_store_held(), "empty store held by hold_nonempty");
    CHECK(bt_store_load(good, n) == ESP_OK, "reload");
    bt_store_hold_nonempty();
    CHECK(bt_store_held(), "non-empty store not held by hold_nonempty");
    bt_store_hold(0);
    CHECK(!bt_store_held(), "hold(0)");
    bt_store_hold(1);
    CHECK(bt_store_held(), "hold(1)");
    nvs_close(h);
    bt_store_init(NULL);
    CHECK(!bt_store_held(), "init did not clear the hold");
}

int main(void)
{
    test_api();
    test_random_ops();
    test_load_fuzz();
    test_hold();
    if (g_fail) {
        printf("test_bt_store: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_bt_store: all tests passed\n");
    return 0;
}
