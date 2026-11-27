/*
 * Cassini OpenFW — per-folder autoplay memory (see include/openfw_apmem.h)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw_apmem.h"

#define HDR_BYTES    16u
#define SLOT_BYTES   16u
#define OFF_MAGIC    0u
#define OFF_SEQ      4u
#define OFF_LSEC     8u
#define OFF_VER      9u
#define OFF_CRC      10u

_Static_assert(HDR_BYTES + APMEM_SLOTS_PER_SEC * SLOT_BYTES == 512u, "slot sector layout");
_Static_assert(HDR_BYTES + APMEM_FOCUS_CAP <= 512u, "header sector layout");

/* One A/B pair; static so the player-loop save path does not need 1 KiB of stack. */
static uint8_t g_pair[1024] __attribute__((aligned(4)));

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void bzero_n(uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        p[i] = 0u;
    }
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* ------------------------------------------------------------------------- */
/* Record format                                                             */
/* ------------------------------------------------------------------------- */

uint32_t apmem_name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    if (name) {
        for (; *name != '\0'; name++) {
            h ^= (uint8_t)lower(*name);
            h *= 16777619u;
        }
    }
    return h ? h : 1u; /* 0 marks an empty slot */
}

uint32_t apmem_slot_lsec(uint32_t hash)
{
    return 1u + (hash % APMEM_SLOT_LSECS);
}

/* CRC-16/CCITT-FALSE over the sector with the crc field (bytes 10..11) skipped */
static uint16_t sec_crc(const uint8_t sec[512])
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0u; i < 512u; i++) {
        if (i == OFF_CRC || i == OFF_CRC + 1u) {
            continue;
        }
        crc ^= (uint16_t)((uint16_t)sec[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void apmem_sec_empty(uint8_t sec[512], uint32_t lsec)
{
    bzero_n(sec, 512u);
    sec[OFF_LSEC] = (uint8_t)lsec;
}

void apmem_sec_seal(uint8_t sec[512], uint32_t lsec, uint32_t seq)
{
    wr32(sec + OFF_MAGIC, APMEM_MAGIC);
    wr32(sec + OFF_SEQ, seq);
    sec[OFF_LSEC] = (uint8_t)lsec;
    sec[OFF_VER]  = (uint8_t)APMEM_VERSION;
    uint16_t crc = sec_crc(sec);
    sec[OFF_CRC]      = (uint8_t)crc;
    sec[OFF_CRC + 1u] = (uint8_t)(crc >> 8);
}

int apmem_sec_valid(const uint8_t sec[512], uint32_t lsec, uint32_t *seq)
{
    if (rd32(sec + OFF_MAGIC) != APMEM_MAGIC || sec[OFF_LSEC] != (uint8_t)lsec ||
        sec[OFF_VER] != (uint8_t)APMEM_VERSION) {
        return 0;
    }
    uint16_t crc = (uint16_t)(sec[OFF_CRC] | ((uint16_t)sec[OFF_CRC + 1u] << 8));
    if (crc != sec_crc(sec)) {
        return 0;
    }
    if (seq) {
        *seq = rd32(sec + OFF_SEQ);
    }
    return 1;
}

int apmem_pick_copy(const uint8_t pair[1024], uint32_t lsec, uint32_t *seq)
{
    uint32_t sa = 0u, sb = 0u;
    int va = apmem_sec_valid(pair, lsec, &sa);
    int vb = apmem_sec_valid(pair + 512, lsec, &sb);
    int pick = -1;
    if (va && vb) {
        pick = ((int32_t)(sb - sa) > 0) ? 1 : 0; /* wrap-safe newest */
    } else if (va) {
        pick = 0;
    } else if (vb) {
        pick = 1;
    }
    if (seq) {
        *seq = (pick == 0) ? sa : (pick == 1) ? sb : 0u;
    }
    return pick;
}

static uint8_t *slot_at(uint8_t *sec, uint32_t i)
{
    return sec + HDR_BYTES + i * SLOT_BYTES;
}

int apmem_slot_get(const uint8_t sec[512], uint32_t hash, uint32_t size_tag, uint32_t *pos)
{
    for (uint32_t i = 0u; i < APMEM_SLOTS_PER_SEC; i++) {
        const uint8_t *s = sec + HDR_BYTES + i * SLOT_BYTES;
        if (rd32(s) == hash && rd32(s + 4u) == size_tag) {
            if (pos) {
                *pos = rd32(s + 8u);
            }
            return 1;
        }
    }
    return 0;
}

int apmem_slot_set(uint8_t sec[512], uint32_t hash, uint32_t size_tag, uint32_t pos, uint32_t stamp)
{
    int empty = -1, oldest = -1;
    uint32_t oldest_stamp = 0u;
    for (uint32_t i = 0u; i < APMEM_SLOTS_PER_SEC; i++) {
        uint8_t *s = slot_at(sec, i);
        uint32_t h = rd32(s);
        if (h == hash && rd32(s + 4u) == size_tag) {
            if (pos == 0u) {
                bzero_n(s, SLOT_BYTES);
                return 1;
            }
            if (rd32(s + 8u) == pos) {
                return 0;
            }
            wr32(s + 8u, pos);
            wr32(s + 12u, stamp);
            return 1;
        }
        if (h == 0u) {
            if (empty < 0) {
                empty = (int)i;
            }
        } else if (oldest < 0 || (int32_t)(rd32(s + 12u) - oldest_stamp) < 0) {
            oldest       = (int)i;
            oldest_stamp = rd32(s + 12u);
        }
    }
    if (pos == 0u) {
        return 0; /* nothing stored, nothing to clear */
    }
    uint8_t *s = slot_at(sec, (uint32_t)((empty >= 0) ? empty : oldest));
    wr32(s, hash);
    wr32(s + 4u, size_tag);
    wr32(s + 8u, pos);
    wr32(s + 12u, stamp);
    return 1;
}

void apmem_hdr_get_focus(const uint8_t sec[512], char *out, uint32_t cap)
{
    if (!out || cap == 0u) {
        return;
    }
    uint32_t i = 0u;
    while (i + 1u < cap && i < APMEM_FOCUS_CAP - 1u && sec[HDR_BYTES + i] != 0u) {
        out[i] = (char)sec[HDR_BYTES + i];
        i++;
    }
    out[i] = '\0';
}

int apmem_hdr_set_focus(uint8_t sec[512], const char *name)
{
    uint8_t buf[APMEM_FOCUS_CAP];
    bzero_n(buf, APMEM_FOCUS_CAP);
    for (uint32_t i = 0u; name && name[i] != '\0' && i < APMEM_FOCUS_CAP - 1u; i++) {
        buf[i] = (uint8_t)name[i];
    }
    int changed = 0;
    for (uint32_t i = 0u; i < APMEM_FOCUS_CAP; i++) {
        if (sec[HDR_BYTES + i] != buf[i]) {
            sec[HDR_BYTES + i] = buf[i];
            changed = 1;
        }
    }
    return changed;
}

/* ------------------------------------------------------------------------- */
/* Behaviour policy                                                          */
/* ------------------------------------------------------------------------- */

uint32_t apmem_resume_pos(uint32_t saved_sec, uint32_t dur_sec)
{
    if (saved_sec == 0u || dur_sec == 0u) {
        return 0u; /* unknown length: never seek blind */
    }
    if (saved_sec >= dur_sec || dur_sec - saved_sec <= APMEM_TAIL_SEC) {
        return 0u; /* past the end or in the closing seconds: treat as finished */
    }
    return saved_sec;
}

uint32_t apmem_store_pos(uint32_t pos_sec, uint32_t dur_sec, int ended)
{
    if (ended) {
        return 0u;
    }
    return apmem_resume_pos(pos_sec, dur_sec);
}

void apmem_sched_start(apmem_sched_t *s, uint32_t now_ms, uint32_t pos_sec)
{
    s->last_save_ms = now_ms;
    s->saved_pos    = pos_sec;
    s->prev_state   = 0u;
}

int apmem_sched_tick(apmem_sched_t *s, uint32_t now_ms, uint32_t pos_sec,
                     uint8_t play_state, uint16_t batt_mv)
{
    uint8_t prev = s->prev_state;
    s->prev_state = play_state;
    if (pos_sec == s->saved_pos) {
        return 0;
    }
    uint32_t since = (uint32_t)(now_ms - s->last_save_ms);
    if (play_state == 1u) {
        /* Pause edge: the listener may walk away now. Skips while paused are
         * coalesced to one write per APMEM_PAUSED_COALESCE_MS. */
        return (prev != 1u || since >= APMEM_PAUSED_COALESCE_MS) ? 1 : 0;
    }
    if (play_state != 0u) {
        return 0; /* ended: the caller stores 0 when the track is left */
    }
    uint32_t period = (batt_mv != 0u && batt_mv < APMEM_LOWBAT_MV) ? APMEM_SAVE_LOWBAT_MS
                                                                    : APMEM_SAVE_PERIOD_MS;
    return (since >= period) ? 1 : 0;
}

void apmem_sched_saved(apmem_sched_t *s, uint32_t now_ms, uint32_t pos_sec)
{
    s->last_save_ms = now_ms;
    s->saved_pos    = pos_sec;
}

/* ------------------------------------------------------------------------- */
/* Storage                                                                   */
/* ------------------------------------------------------------------------- */

static int join_path(char *out, uint32_t cap, const char *dir, const char *name)
{
    uint32_t p = 0u;
    if (!dir || !name || dir[0] == '\0') {
        return -1;
    }
    if (dir[0] != '/') {
        if (p + 1u >= cap) return -1;
        out[p++] = '/';
    }
    for (uint32_t i = 0u; dir[i] != '\0'; i++) {
        if (p + 1u >= cap) return -1;
        out[p++] = dir[i];
    }
    if (p > 0u && out[p - 1u] != '/') {
        if (p + 1u >= cap) return -1;
        out[p++] = '/';
    }
    for (uint32_t i = 0u; name[i] != '\0'; i++) {
        if (p + 1u >= cap) return -1;
        out[p++] = name[i];
    }
    out[p] = '\0';
    return 0;
}

int apmem_file_tag(exfat_vol_t *vol, const char *dir, const char *name, uint32_t *tag)
{
    char path[EXFAT_MAX_NAME + 96];
    exfat_entry_t ent;
    if (!vol || join_path(path, (uint32_t)sizeof(path), dir, name) != 0 ||
        exfat_find_path(vol, path, &ent) != 0 || ent.is_dir) {
        return -1;
    }
    if (tag) {
        *tag = (uint32_t)ent.size;
    }
    return 0;
}

/* Cached .autoplay.state directory entry so periodic 60 s saves avoid re-walking directories. */
static struct {
    const exfat_vol_t *vol;
    const blkdev_t    *dev;
    uint32_t           part_lba;
    uint32_t           heap_lba;
    uint32_t           cluster_count;
    char               dir[96];
    exfat_entry_t      ent;
    uint8_t            valid;
} g_state_cache;

void apmem_invalidate_cache(void)
{
    g_state_cache.valid = 0u;
}

static int state_usable(const exfat_entry_t *ent)
{
    return !ent->is_dir && ent->size == APMEM_FILE_BYTES &&
           ent->valid_data_len >= APMEM_FILE_BYTES && ent->first_cluster >= 2u;
}

static int dir_eq_ci(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (a[i] != '\0' && b[i] != '\0') {
        if (lower(a[i]) != lower(b[i])) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int cache_matches(const exfat_vol_t *vol, const char *dir)
{
    return g_state_cache.valid &&
           g_state_cache.vol == vol &&
           g_state_cache.dev == vol->dev &&
           g_state_cache.part_lba == vol->part_lba &&
           g_state_cache.heap_lba == vol->heap_lba &&
           g_state_cache.cluster_count == vol->cluster_count &&
           dir_eq_ci(g_state_cache.dir, dir);
}

static void cache_store(const exfat_vol_t *vol, const char *dir, const exfat_entry_t *ent)
{
    if (!vol || !dir || !ent || !state_usable(ent)) {
        return;
    }
    uint32_t i = 0u;
    while (dir[i] != '\0') {
        if (i + 1u >= (uint32_t)sizeof(g_state_cache.dir)) {
            g_state_cache.valid = 0u;
            return;
        }
        g_state_cache.dir[i] = dir[i];
        i++;
    }
    g_state_cache.dir[i]        = '\0';
    g_state_cache.vol           = vol;
    g_state_cache.dev           = vol->dev;
    g_state_cache.part_lba      = vol->part_lba;
    g_state_cache.heap_lba      = vol->heap_lba;
    g_state_cache.cluster_count = vol->cluster_count;
    g_state_cache.ent           = *ent;
    g_state_cache.valid         = 1u;
}

/* Find (and with create != 0, create once) the folder's state file. */
static int state_open(exfat_vol_t *vol, const char *dir, int create, exfat_entry_t *ent)
{
    char path[EXFAT_MAX_NAME + 96];
    if (!vol || join_path(path, (uint32_t)sizeof(path), dir, APMEM_STATE_NAME) != 0) {
        return -1;
    }
    if (!create && cache_matches(vol, dir) && state_usable(&g_state_cache.ent)) {
        *ent = g_state_cache.ent;
        return 0;
    }
    if (exfat_find_path(vol, path, ent) == 0) {
        if (state_usable(ent)) {
            cache_store(vol, dir, ent);
            return 0;
        }
        if (!create || ent->is_dir) {
            return -1;
        }
        apmem_invalidate_cache();
        /* Truncated by a power cut during the one-time creation: start over */
        if (exfat_delete_file(vol, dir, APMEM_STATE_NAME) != 0) {
            return -1;
        }
    } else if (!create) {
        return -1;
    }

    apmem_invalidate_cache();
    exfat_writer_t w;
    if (exfat_create_file(vol, dir, APMEM_STATE_NAME, &w) != 0) {
        return -1;
    }
    bzero_n(g_pair, 512u);
    int rc = 0;
    for (uint32_t i = 0u; i < APMEM_LSECS * 2u && rc == 0; i++) {
        rc = exfat_write(&w, g_pair, 512u);
    }
    if (exfat_close_file(&w) != 0 || rc != 0) {
        (void)exfat_delete_file(vol, dir, APMEM_STATE_NAME);
        return -1;
    }
    if (exfat_find_path(vol, path, ent) != 0 || !state_usable(ent)) {
        return -1;
    }
    cache_store(vol, dir, ent);
    return 0;
}

/* Load logical sector lsec into g_pair; returns newest copy index or -1. */
static int load_pair(exfat_vol_t *vol, const exfat_entry_t *ent, uint32_t lsec, uint32_t *seq)
{
    uint32_t got = 0u;
    if (exfat_read_file(vol, ent, (uint64_t)lsec * 1024u, g_pair, 1024u, &got) != 0 || got != 1024u) {
        apmem_invalidate_cache();
        bzero_n(g_pair, 1024u);
        return -2;
    }
    return apmem_pick_copy(g_pair, lsec, seq);
}

typedef int (*apmem_edit_fn)(uint8_t *sec, uint32_t stamp, const void *arg);

/* Read-modify-write of one logical sector: exactly one data-sector write. */
static int edit_lsec(exfat_vol_t *vol, const char *dir, uint32_t lsec, int may_create,
                     apmem_edit_fn edit, const void *arg)
{
    exfat_entry_t ent;
    if (state_open(vol, dir, 0, &ent) != 0) {
        if (!may_create || state_open(vol, dir, 1, &ent) != 0) {
            return may_create ? -1 : 0;
        }
    }
    uint32_t seq = 0u;
    int cur = load_pair(vol, &ent, lsec, &seq);
    if (cur == -2) {
        return -1;
    }
    uint8_t *work = (cur == 1) ? g_pair + 512 : g_pair;
    if (cur < 0) {
        apmem_sec_empty(work, lsec);
    }
    uint32_t new_seq = seq + 1u;
    if (!edit(work, new_seq, arg)) {
        return 0; /* unchanged: no write at all */
    }
    apmem_sec_seal(work, lsec, new_seq);
    uint32_t target = (cur == 0) ? 1u : 0u; /* overwrite the older / invalid copy */
    if (exfat_overwrite_sector(vol, &ent, (uint64_t)(lsec * 2u + target) * 512u, work) != 0) {
        apmem_invalidate_cache();
        return -1;
    }
    return 0;
}

int apmem_load_pos(exfat_vol_t *vol, const char *dir, const char *name, uint32_t size_tag,
                   uint32_t *pos)
{
    exfat_entry_t ent;
    if (!name || state_open(vol, dir, 0, &ent) != 0) {
        return -1;
    }
    uint32_t hash = apmem_name_hash(name);
    uint32_t lsec = apmem_slot_lsec(hash);
    int cur = load_pair(vol, &ent, lsec, 0);
    if (cur < 0) {
        return -1;
    }
    return apmem_slot_get(g_pair + (cur ? 512 : 0), hash, size_tag, pos) ? 0 : -1;
}

int apmem_load_focus(exfat_vol_t *vol, const char *dir, char *out, uint32_t cap)
{
    exfat_entry_t ent;
    if (!out || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (state_open(vol, dir, 0, &ent) != 0) {
        return -1;
    }
    int cur = load_pair(vol, &ent, 0u, 0);
    if (cur < 0) {
        return -1;
    }
    apmem_hdr_get_focus(g_pair + (cur ? 512 : 0), out, cap);
    return (out[0] != '\0') ? 0 : -1;
}

typedef struct {
    uint32_t hash;
    uint32_t tag;
    uint32_t pos;
} pos_arg_t;

static int edit_pos(uint8_t *sec, uint32_t stamp, const void *arg)
{
    const pos_arg_t *a = (const pos_arg_t *)arg;
    return apmem_slot_set(sec, a->hash, a->tag, a->pos, stamp);
}

static int edit_focus(uint8_t *sec, uint32_t stamp, const void *arg)
{
    (void)stamp;
    return apmem_hdr_set_focus(sec, (const char *)arg);
}

int apmem_save_pos(exfat_vol_t *vol, const char *dir, const char *name, uint32_t size_tag,
                   uint32_t pos)
{
    if (!vol || !name || name[0] == '\0') {
        return -1;
    }
    pos_arg_t a = { apmem_name_hash(name), size_tag, pos };
    /* Clearing a position never creates the state file */
    return edit_lsec(vol, dir, apmem_slot_lsec(a.hash), pos != 0u, edit_pos, &a);
}

int apmem_save_focus(exfat_vol_t *vol, const char *dir, const char *name)
{
    if (!vol || !name || name[0] == '\0') {
        return -1;
    }
    return edit_lsec(vol, dir, 0u, 1, edit_focus, name);
}
