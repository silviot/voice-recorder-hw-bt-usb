/*
 * Smartlink SL6801 OpenFW Real-Time WAV Recorder
 *
 * Streams 16 kHz / 16-bit mono PCM audio frames from DMAC CH1 ring buffers
 * directly into /record/REC_NNNN.wav on exFAT, then back-patches the exact
 * RIFF/data byte sizes cleanly on stop.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw.h"
#include "openfw_wav.h"

static int            g_rec_active = 0;
static int            g_rec_saving = 0;
static exfat_writer_t g_writer;
static uint32_t       g_pcm_bytes  = 0u;
static char           g_filename[16];

static uint32_t parse_rec_index(const char *name)
{
    /* Match "REC_NNNN.wav" case-insensitively */
    const char *pfx = "REC_";
    for (int i = 0; i < 4; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        }
        if (c != pfx[i]) {
            return 0u;
        }
    }
    uint32_t num = 0u;
    for (int i = 4; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0u;
        }
        num = num * 10u + (uint32_t)(name[i] - '0');
    }
    if (name[8] != '.') {
        return 0u;
    }
    const char *ext = "wav";
    for (int i = 0; i < 3; i++) {
        char c = name[9 + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        if (c != ext[i]) {
            return 0u;
        }
    }
    return (name[12] == '\0') ? num : 0u;
}

static int max_rec_cb(const exfat_entry_t *entry, void *user)
{
    if (!entry->is_dir) {
        uint32_t *max_idx = (uint32_t *)user;
        uint32_t idx = parse_rec_index(entry->name);
        if (idx > *max_idx && idx <= 9999u) {
            *max_idx = idx;
        }
    }
    return 0;
}

static void format_rec_filename(uint32_t idx, char out[16])
{
    out[0]  = 'R';
    out[1]  = 'E';
    out[2]  = 'C';
    out[3]  = '_';
    out[4]  = (char)('0' + ((idx / 1000u) % 10u));
    out[5]  = (char)('0' + ((idx / 100u) % 10u));
    out[6]  = (char)('0' + ((idx / 10u) % 10u));
    out[7]  = (char)('0' + (idx % 10u));
    out[8]  = '.';
    out[9]  = 'w';
    out[10] = 'a';
    out[11] = 'v';
    out[12] = '\0';
}

int recorder_start(exfat_vol_t *vol, int sd_mounted)
{
    if (g_rec_active) {
        return 0;
    }
    if (audio_capture_start(24u) != 0) {
        return -1;
    }

    g_rec_active = 1;
    g_rec_saving = 0;
    g_pcm_bytes  = 0u;
    g_filename[0] = '\0';

    if (sd_mounted && vol && exfat_mkdir(vol, "record") == 0) {
        rtc_datetime_t now;
        rtc_get(&now);
        exfat_set_timestamp(vol, rtc_to_dos_timestamp(&now));

        exfat_entry_t rec_dir;
        if (exfat_find_path(vol, "/record", &rec_dir) == 0 && rec_dir.is_dir) {
            uint32_t max_idx = 0u;
            (void)exfat_list_dir(vol, rec_dir.first_cluster, rec_dir.no_fat_chain,
                                 max_rec_cb, &max_idx);
            uint32_t next_idx = (max_idx < 9999u) ? (max_idx + 1u) : 1u;

            for (uint32_t tries = 0u; tries < 9999u; tries++) {
                uint32_t cand_idx = ((next_idx - 1u + tries) % 9999u) + 1u;
                char name[16];
                char full[24];
                format_rec_filename(cand_idx, name);
                const char *pfx = "/record/";
                int p = 0;
                for (int i = 0; pfx[i] != '\0'; i++) {
                    full[p++] = pfx[i];
                }
                for (int i = 0; name[i] != '\0'; i++) {
                    full[p++] = name[i];
                }
                full[p] = '\0';

                exfat_entry_t existing;
                if (exfat_find_path(vol, full, &existing) != 0) {
                    if (exfat_create_file(vol, "/record", name, &g_writer) == 0) {
                        uint8_t hdr[44];
                        (void)wav_build_header(hdr, 16000u, 1u, 16u, 0u);
                        if (exfat_write(&g_writer, hdr, 44u) == 0) {
                            for (int i = 0; i < 16; i++) {
                                g_filename[i] = name[i];
                            }
                            g_rec_saving = 1;
                        } else {
                            (void)exfat_close_file(&g_writer);
                        }
                    }
                    break;
                }
            }
        }
    }

    return 0;
}

void recorder_poll(void)
{
    if (!g_rec_active) {
        return;
    }

    uint32_t nbytes = 0u;
    const int16_t *frame;
    while ((frame = audio_pop_frame(&nbytes)) != 0) {
        if (g_rec_saving) {
            if (exfat_write(&g_writer, frame, nbytes) != 0) {
                (void)exfat_close_file(&g_writer);
                g_rec_saving = 0;
            } else {
                g_pcm_bytes += nbytes;
            }
        }
    }
}

int recorder_stop(void)
{
    if (!g_rec_active) {
        return 0;
    }

    audio_capture_stop();
    recorder_poll();

    if (g_rec_saving) {
        if (g_writer.vol) {
            rtc_datetime_t now;
            rtc_get(&now);
            exfat_set_timestamp(g_writer.vol, rtc_to_dos_timestamp(&now));
        }
        uint8_t hdr[44];
        (void)wav_build_header(hdr, 16000u, 1u, 16u, g_pcm_bytes);
        (void)exfat_patch_header(&g_writer, hdr, 44u);
        (void)exfat_close_file(&g_writer);
        g_rec_saving = 0;
    }

    g_rec_active = 0;
    return 0;
}

int recorder_is_saving(void)
{
    return g_rec_saving;
}

uint32_t recorder_get_seconds(void)
{
    if (g_pcm_bytes > 0u) {
        return g_pcm_bytes / 32000u; /* 16 kHz * 2 B */
    }
    return (audio_get_frames() * 128u) / 1000u;
}

const char *recorder_get_filename(void)
{
    return g_filename;
}
