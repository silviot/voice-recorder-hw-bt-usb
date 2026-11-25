/* bt_store: the nvs_* surface Bluedroid uses (osi/config.c, bond keys), served from a RAM key-value cache that the
 * app task owns (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M2 "Store").
 *
 * - The app loads the cache from SD (bt_store_load) BEFORE esp_bluedroid_init(); nothing in here touches the
 *   filesystem. nvs_commit() only marks the cache dirty and calls the platform's request_flush() (non-blocking post
 *   to the app), which serialises it (bt_store_serialize) and writes it with the exFAT staged replace.
 * - Serialised form: "BTS1" | u16 entry_count | entries { u8 ns_len, ns, u8 key_len, key, u16 len, data } |
 *   8-byte SHA-256 prefix over everything before it. bt_store_load() validates every bound and the checksum and
 *   rejects the whole image on any error (the store then starts empty: bonds are lost, never misparsed).
 * - Capacity is fixed (no heap). Link keys are never logged.
 * SPDX-License-Identifier: MIT */
#ifndef BT_STORE_H
#define BT_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "nvs.h"

#define BT_STORE_DATA_MAX     4096u   /* total value bytes */
#define BT_STORE_ENTRIES_MAX  24u
#define BT_STORE_NS_MAX       4u      /* distinct namespaces open at once */
#define BT_STORE_NAME_MAX     15u     /* NVS_KEY_NAME_MAX_SIZE - 1 */
#define BT_STORE_IMAGE_MAX    (8u + BT_STORE_ENTRIES_MAX * (2u + 2u * BT_STORE_NAME_MAX + 2u) + BT_STORE_DATA_MAX + 8u)

typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*request_flush)(void *ctx);   /* non-blocking; the app serialises + writes later */
    void *ctx;
} bt_store_platform_t;

/* Reset to an empty store. platform may be NULL (single-threaded tests, no flush requests). */
void bt_store_init(const bt_store_platform_t *platform);

/* Replace the cache with a serialised image. Returns ESP_OK, or ESP_ERR_INVALID_CRC / ESP_ERR_INVALID_SIZE /
 * ESP_ERR_INVALID_VERSION / ESP_ERR_INVALID_ARG; on any error the store is left empty and, for a non-empty image, held
 * (bt_store_held). */
esp_err_t bt_store_load(const uint8_t *img, size_t len);

/* Serialise into buf. Returns the image length, or 0 if cap is too small. The dirty flag is NOT cleared: a write of the
 * image can still fail (full or removed card), and the store must then stay dirty so the next flush retries. */
size_t bt_store_serialize(uint8_t *buf, size_t cap);
/* The image of the last bt_store_serialize() was stored: clears the dirty flag unless the store changed since. */
void bt_store_mark_saved(void);

int bt_store_dirty(void);

/* Empty the store (entries and data cleared, not dirty) without touching the platform binding: a mode change between
 * phone and headphones mode (docs/BT_A2DP_DESIGN.md §6.4). App task, stack off. */
void bt_store_reset(void);

/* Write hold (silicon 2026-11-25: a headphones bond was replaced by an empty store). A card image whose bonds this
 * session cannot take over must not be replaced by the store Bluedroid saves at every start (btc_config_init()), which
 * is then empty. The hold is set by a bt_store_load() that rejects a non-empty image, by bt_store_hold_nonempty()
 * (Bluedroid patch 0045: the config the store holds could not be parsed) and by the app (bond file present but not
 * readable); a load that succeeds and bt_store_reset() clear it. The app writes nothing while it is set; the card file
 * is retried at the next start. */
int  bt_store_held(void);
void bt_store_hold(int on);
void bt_store_hold_nonempty(void);

#endif /* BT_STORE_H */
