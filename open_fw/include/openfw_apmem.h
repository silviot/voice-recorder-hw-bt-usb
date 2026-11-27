/*
 * Cassini OpenFW — per-folder autoplay memory (".autoplay.state")
 *
 * Folders containing the .autoplay marker remember the playhead position of
 * each file and the folder's last focused file. Everything lives in ONE hidden
 * fixed-size file per folder, <dir>/.autoplay.state, created once (16 KiB) and
 * afterwards only updated by overwriting single 512-byte data sectors in place:
 * no FAT, allocation bitmap or directory entry is ever touched by a save.
 *
 * Layout: 16 logical sectors, each stored twice (A/B copies at physical file
 * sectors 2L and 2L+1). A save reads both copies, modifies the newest valid
 * one and writes it, with seq+1, over the OTHER copy. A torn write therefore
 * only ever damages the copy being written; the previous state survives.
 *
 *   logical 0      : header — last focused file name of the folder
 *   logical 1..15  : 31 position slots each {name_hash, size_tag, pos_sec, stamp}
 *                    (slot sector = 1 + name_hash % 15, linear probe inside it,
 *                    oldest stamp evicted when full)
 *
 * Every sector: magic u32 "APM1", seq u32, lsec u8, version u8, crc16 u16
 * (CRC-16/CCITT-FALSE over the 512 bytes with the crc field zeroed), 4 bytes
 * reserved, 496 bytes payload. An all-zero sector (fresh file) is "empty".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_APMEM_H
#define OPENFW_APMEM_H

#include <stdint.h>
#include "openfw_fs.h"

#define APMEM_STATE_NAME        ".autoplay.state"
#define APMEM_MAGIC             0x314D5041u /* "APM1" little-endian */
#define APMEM_VERSION           1u
#define APMEM_LSECS             16u
#define APMEM_SLOT_LSECS        (APMEM_LSECS - 1u)
#define APMEM_SLOTS_PER_SEC     31u
#define APMEM_FILE_BYTES        (APMEM_LSECS * 2u * 512u) /* 16 KiB */
#define APMEM_FOCUS_CAP         64u

/* Resume policy: a saved position inside the last APMEM_TAIL_SEC seconds of a
 * file counts as "finished" and the file restarts at 0:00. */
#define APMEM_TAIL_SEC          10u
/* Write policy: during uninterrupted playback save at most every 60 s (every
 * 15 s once the battery drops below APMEM_LOWBAT_MV, as a brown-out may follow). */
#define APMEM_SAVE_PERIOD_MS    60000u
#define APMEM_SAVE_LOWBAT_MS    15000u
#define APMEM_LOWBAT_MV         3500u
/* While paused, position changes (skips) are written at most every 5 s. */
#define APMEM_PAUSED_COALESCE_MS 5000u

/* ---- Record format (pure, no I/O) ---- */
uint32_t apmem_name_hash(const char *name); /* FNV-1a over ASCII-lowercased name, never 0 */
uint32_t apmem_slot_lsec(uint32_t hash);    /* 1..APMEM_SLOT_LSECS */
void     apmem_sec_empty(uint8_t sec[512], uint32_t lsec);
void     apmem_sec_seal(uint8_t sec[512], uint32_t lsec, uint32_t seq);
int      apmem_sec_valid(const uint8_t sec[512], uint32_t lsec, uint32_t *seq);
/* Index (0/1) of the newest valid copy in a 1024-byte A/B pair, or -1 if none. */
int      apmem_pick_copy(const uint8_t pair[1024], uint32_t lsec, uint32_t *seq);
int      apmem_slot_get(const uint8_t sec[512], uint32_t hash, uint32_t size_tag, uint32_t *pos);
/* pos == 0 clears the slot. Returns 1 when the sector content changed. */
int      apmem_slot_set(uint8_t sec[512], uint32_t hash, uint32_t size_tag,
                        uint32_t pos, uint32_t stamp);
void     apmem_hdr_get_focus(const uint8_t sec[512], char *out, uint32_t cap);
int      apmem_hdr_set_focus(uint8_t sec[512], const char *name); /* 1 when changed */

/* ---- Behaviour policy (pure) ---- */
/* Position to start a file at, given its saved position and duration. */
uint32_t apmem_resume_pos(uint32_t saved_sec, uint32_t dur_sec);
/* Position worth storing on exit (0 = finished/start, i.e. clear the slot). */
uint32_t apmem_store_pos(uint32_t pos_sec, uint32_t dur_sec, int ended);

typedef struct {
    uint32_t last_save_ms;
    uint32_t saved_pos;
    uint8_t  prev_state; /* ui play_state: 0 playing, 1 paused, 2 ended */
} apmem_sched_t;

void apmem_sched_start(apmem_sched_t *s, uint32_t now_ms, uint32_t pos_sec);
/* Call on every player UI tick. 1 when the live position should be written now
 * (pause edge, skip while paused, or the periodic interval elapsed during
 * playback) and differs from what was last written. */
int  apmem_sched_tick(apmem_sched_t *s, uint32_t now_ms, uint32_t pos_sec,
                      uint8_t play_state, uint16_t batt_mv);
/* Record that pos_sec was written (or attempted) at now_ms. */
void apmem_sched_saved(apmem_sched_t *s, uint32_t now_ms, uint32_t pos_sec);

/* ---- Storage (exFAT) ---- */
/* Size tag of dir/name (low 32 bits of the file size); -1 if missing. */
int apmem_file_tag(exfat_vol_t *vol, const char *dir, const char *name, uint32_t *tag);
/* 0 and *pos when a saved position exists, -1 otherwise (missing/invalid state). */
int apmem_load_pos(exfat_vol_t *vol, const char *dir, const char *name, uint32_t size_tag,
                   uint32_t *pos);
int apmem_load_focus(exfat_vol_t *vol, const char *dir, char *out, uint32_t cap);
/* Write one sector in place (creating the 16 KiB state file the first time a
 * non-trivial value must be stored). 0 on success or when nothing changed. */
int apmem_save_pos(exfat_vol_t *vol, const char *dir, const char *name, uint32_t size_tag,
                   uint32_t pos);
int apmem_save_focus(exfat_vol_t *vol, const char *dir, const char *name);
void apmem_invalidate_cache(void);

#endif /* OPENFW_APMEM_H */
