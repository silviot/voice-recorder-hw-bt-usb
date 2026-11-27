/* Wire codec for SD:/bt_phone_cfg.bin ("BPC1"). Pure C so both firmware (bt_xrec_fw.c) and host tests
 * (tests/test_bt_store.c) share one pack/unpack layout without pulling FreeRTOS/exFAT.
 * SPDX-License-Identifier: MIT */
#ifndef BT_PHONE_CFG_CODEC_H
#define BT_PHONE_CFG_CODEC_H

#include <stdint.h>
#include <string.h>
#include "openfw_crypto.h"
#include "bt_xrec_bluedroid.h"

#define BT_PHONE_CFG_BYTES        17u /* BPC1[4] + ver[1] + auto_sync[1] + auto_off[1] + sync_pend[1] + phone_valid[1] + bda[6] + crc[2] */
#define BT_PHONE_CFG_LEGACY_BYTES 16u /* pre-fix 16-byte image where CRC LSB overwrote b[14] (phone_bda[5]) */

static inline void bt_phone_cfg_pack(const bt_phone_cfg_t *cfg, uint8_t b[BT_PHONE_CFG_BYTES])
{
    memset(b, 0, BT_PHONE_CFG_BYTES);
    b[0] = (uint8_t)(BT_PHONE_CFG_MAGIC & 0xFFu);
    b[1] = (uint8_t)((BT_PHONE_CFG_MAGIC >> 8) & 0xFFu);
    b[2] = (uint8_t)((BT_PHONE_CFG_MAGIC >> 16) & 0xFFu);
    b[3] = (uint8_t)((BT_PHONE_CFG_MAGIC >> 24) & 0xFFu);
    b[4] = 1u;
    b[5] = cfg->auto_sync ? 1u : 0u;
    b[6] = cfg->auto_off_idx <= 3u ? cfg->auto_off_idx : 2u;
    b[7] = cfg->sync_pending ? 1u : 0u;
    b[8] = cfg->phone_valid ? 1u : 0u;
    memcpy(&b[9], cfg->phone_bda, 6u);
    uint16_t crc = openfw_crc16_ccitt_false(b, BT_PHONE_CFG_BYTES - 2u);
    b[BT_PHONE_CFG_BYTES - 2u] = (uint8_t)(crc & 0xFFu);
    b[BT_PHONE_CFG_BYTES - 1u] = (uint8_t)((crc >> 8) & 0xFFu);
}

static inline int bt_phone_cfg_unpack(const uint8_t *b, uint32_t len, bt_phone_cfg_t *cfg)
{
    if (b == NULL || cfg == NULL || (len != BT_PHONE_CFG_BYTES && len != BT_PHONE_CFG_LEGACY_BYTES)) {
        return -1;
    }
    uint32_t magic = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    uint16_t want_crc = openfw_crc16_ccitt_false(b, len - 2u);
    uint16_t got_crc = (uint16_t)b[len - 2u] | ((uint16_t)b[len - 1u] << 8);
    if (magic != BT_PHONE_CFG_MAGIC || b[4] != 1u || want_crc != got_crc) {
        return -1;
    }
    cfg->auto_sync = b[5] ? 1u : 0u;
    cfg->auto_off_idx = b[6] <= 3u ? b[6] : 2u;
    cfg->sync_pending = b[7] ? 1u : 0u;
    if (len == BT_PHONE_CFG_BYTES) {
        cfg->phone_valid = b[8] ? 1u : 0u;
        memcpy(cfg->phone_bda, &b[9], 6u);
    } else {
        /* 16-byte files clobbered b[14] (phone_bda[5]) with the CRC low byte; drop the address until re-learned. */
        cfg->phone_valid = 0u;
        memset(cfg->phone_bda, 0, sizeof(cfg->phone_bda));
    }
    return 0;
}

#endif /* BT_PHONE_CFG_CODEC_H */
