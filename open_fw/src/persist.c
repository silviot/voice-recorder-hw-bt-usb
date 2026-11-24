/*
 * Cassini OpenFW — Persistent Settings & Last-Played Resume State
 *
 * Stores a compact 64-byte CRC-16/CCITT-FALSE protected binary record at
 * /cassini/settings.bin on the exFAT microSD volume (zero NOR flash wear,
 * zero risk to HLKJ/PSMP, survives firmware reflashes).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw_ui.h"
#include "openfw_crypto.h"

_Static_assert(sizeof(openfw_persist_t) == 64u, "openfw_persist_t must be 64 bytes");

static void persist_zero(void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0u; i < len; i++) {
        p[i] = 0u;
    }
}

static void persist_copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;
    if (!dst || cap == 0u) {
        return;
    }
    if (src) {
        while (src[i] != '\0' && (i + 1u) < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void persist_clamp_settings(ui_settings_t *dst, const ui_settings_t *src)
{
    dst->lang           = (src->lang < (uint8_t)UI_LANG_COUNT) ? src->lang : (uint8_t)UI_LANG_EN;
    dst->mic_gain       = (src->mic_gain <= 31u) ? src->mic_gain : 31u;
    dst->sound_mode     = (src->sound_mode <= 2u) ? src->sound_mode : AUDIO_DSP_DEFAULT_PROFILE;
    dst->channels       = (src->channels == 1u || src->channels == 2u) ? src->channels : 2u;
    dst->vor_enabled    = src->vor_enabled ? 1u : 0u;
    dst->brightness_idx = (src->brightness_idx <= 4u) ? src->brightness_idx : 3u;
    dst->screen_off_idx = (src->screen_off_idx <= 4u) ? src->screen_off_idx : UI_SCR_OFF_30S;
    dst->skip_mode      = (src->skip_mode <= 2u) ? src->skip_mode : UI_SKIP_5_10;
    dst->hp_vol         = (src->hp_vol <= 31u) ? src->hp_vol : 31u;
    dst->spk_vol        = (src->spk_vol <= 31u) ? src->spk_vol : 31u;
}

int ui_persist_load(exfat_vol_t *vol, ui_settings_t *cfg,
                    char *last_file, uint32_t *last_resume, int *has_last)
{
    if (!vol || !cfg) {
        return -1;
    }

    exfat_entry_t ent;
    if (exfat_find_path(vol, "/cassini/settings.bin", &ent) != 0 ||
        ent.is_dir || ent.size != sizeof(openfw_persist_t)) {
        return -1;
    }

    openfw_persist_t rec;
    uint32_t nread = 0u;
    if (exfat_read_file(vol, &ent, 0u, &rec, (uint32_t)sizeof(rec), &nread) != 0 ||
        nread != (uint32_t)sizeof(rec)) {
        return -1;
    }

    if (rec.magic != OPENFW_PERSIST_MAGIC || rec.version != OPENFW_PERSIST_VERSION) {
        return -1;
    }

    uint16_t want_crc = openfw_crc16_ccitt_false((const uint8_t *)&rec,
                                                 (uint32_t)(sizeof(rec) - sizeof(rec.crc16)));
    if (rec.crc16 != want_crc) {
        return -1;
    }

    persist_clamp_settings(cfg, &rec.cfg);

    rec.last_filename[sizeof(rec.last_filename) - 1u] = '\0';
    int valid_last = (rec.has_last != 0u && rec.last_filename[0] != '\0') ? 1 : 0;

    if (last_file) {
        persist_copy_str(last_file, (uint32_t)sizeof(rec.last_filename),
                         valid_last ? rec.last_filename : "");
    }
    if (last_resume) {
        *last_resume = valid_last ? rec.last_resume_sec : 0u;
    }
    if (has_last) {
        *has_last = valid_last;
    }

    return 0;
}

int ui_persist_save(exfat_vol_t *vol, const ui_settings_t *cfg,
                    const char *last_file, uint32_t last_resume, int has_last)
{
    if (!vol || !cfg) {
        return -1;
    }

    openfw_persist_t rec;
    persist_zero(&rec, (uint32_t)sizeof(rec));

    rec.magic   = OPENFW_PERSIST_MAGIC;
    rec.version = OPENFW_PERSIST_VERSION;
    persist_clamp_settings(&rec.cfg, cfg);

    if (has_last && last_file && last_file[0] != '\0') {
        rec.has_last        = 1u;
        rec.last_resume_sec = last_resume;
        persist_copy_str(rec.last_filename, (uint32_t)sizeof(rec.last_filename), last_file);
    } else {
        rec.has_last        = 0u;
        rec.last_resume_sec = 0u;
        rec.last_filename[0] = '\0';
    }

    rec.crc16 = openfw_crc16_ccitt_false((const uint8_t *)&rec,
                                         (uint32_t)(sizeof(rec) - sizeof(rec.crc16)));

    if (exfat_mkdir(vol, "cassini") != 0) {
        return -1;
    }

    (void)exfat_delete_file(vol, "/cassini", "settings.tmp");

    exfat_writer_t w;
    if (exfat_create_file(vol, "/cassini", "settings.tmp", &w) != 0) {
        return -1;
    }
    if (exfat_write(&w, &rec, (uint32_t)sizeof(rec)) != 0) {
        (void)exfat_close_file(&w);
        (void)exfat_delete_file(vol, "/cassini", "settings.tmp");
        return -1;
    }
    if (exfat_close_file(&w) != 0) {
        (void)exfat_delete_file(vol, "/cassini", "settings.tmp");
        return -1;
    }
    if (exfat_replace_file(vol, "/cassini", "settings.tmp", "settings.bin") != 0) {
        (void)exfat_delete_file(vol, "/cassini", "settings.tmp");
        return -1;
    }
    return 0;
}
