/*
 * Smartlink SL6801 OpenFW Standalone MBR + exFAT Reader & Writer
 *
 * Allocation-free Microsoft exFAT reader/writer for microSD:
 *   - Supports reading contiguous (NoFatChain=1) and FAT-chained files/dirs
 *   - Supports creating single-level root dirs, creating/appending FAT-chained
 *     files, patching sector-0 headers (e.g. final RIFF/WAV sizes), updating
 *     the primary Allocation Bitmap (0x81), NameHash, and 16-bit SetChecksum.
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_fs.h"

#define EXFAT_DOS_TIMESTAMP_DEFAULT 0x5C210000u /* 2026-01-01 00:00:00 */
#define EXFAT_ATTR_ARCHIVE          0x0020u

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
        *d++ = 0u;
        n--;
    }
    uint32_t *d32 = (uint32_t *)(void *)d;
    while (n >= 4u) {
        *d32++ = 0u;
        n -= 4u;
    }
    d = (uint8_t *)d32;
    while (n > 0u) {
        *d++ = 0u;
        n--;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
            *d++ = *s++;
            n--;
        }
        uint32_t *d32 = (uint32_t *)(void *)d;
        const uint32_t *s32 = (const uint32_t *)(const void *)s;
        while (n >= 4u) {
            *d32++ = *s32++;
            n -= 4u;
        }
        d = (uint8_t *)d32;
        s = (const uint8_t *)s32;
    }
    while (n > 0u) {
        *d++ = *s++;
        n--;
    }
}

/* Every device write goes through here: a BLKDEV_ERR_FINAL (the device already retried and gave up) is remembered
 * for exfat_write(), which reports it to its caller instead of a plain -1, also when it happened in a FAT / bitmap
 * write of a cluster allocation. */
static int dev_write(exfat_vol_t *vol, uint32_t lba, uint32_t count, const void *buf)
{
    int rc = vol->dev->write_sectors(vol->dev->ctx, lba, count, buf);
    if (rc == BLKDEV_ERR_FINAL) {
        vol->wr_final = 1u;
    }
    return rc;
}

/* The same for the directory / zeroing helpers that hold a const volume (not on the exfat_write() path). */
static int dev_write_c(const exfat_vol_t *vol, uint32_t lba, uint32_t count, const void *buf)
{
    return vol->dev->write_sectors(vol->dev->ctx, lba, count, buf);
}

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static int str_eq_ci_n(const char *a, const char *b, uint32_t b_len)
{
    for (uint32_t i = 0u; i < b_len; i++) {
        if (a[i] == '\0' || ascii_lower(a[i]) != ascii_lower(b[i])) {
            return 0;
        }
    }
    return a[b_len] == '\0';
}

static uint32_t ascii_len_checked(const char *s, uint32_t max_len)
{
    if (!s) {
        return 0u;
    }
    uint32_t n = 0u;
    while (s[n] != '\0') {
        uint8_t c = (uint8_t)s[n];
        if (c < 0x20u || c >= 0x80u || n >= max_len ||
            c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return 0u;
        }
        n++;
    }
    if ((n == 1u && s[0] == '.') || (n == 2u && s[0] == '.' && s[1] == '.')) {
        return 0u;
    }
    return n;
}

static int is_exfat_vbr(const uint8_t sec[512])
{
    static const char magic[] = "EXFAT   ";
    if (sec[510] != 0x55u || sec[511] != 0xAAu) {
        return 0;
    }
    for (uint32_t i = 0u; i < 8u; i++) {
        if (sec[3u + i] != (uint8_t)magic[i]) {
            return 0;
        }
    }
    return 1;
}

static uint32_t cluster_to_lba(const exfat_vol_t *vol, uint32_t cluster)
{
    return vol->heap_lba + ((cluster - 2u) << vol->spc_shift);
}

static int fat_get_entry(const exfat_vol_t *vol, uint32_t cluster, uint32_t *out_val);

static const uint8_t *fat_read_sector_cached(const exfat_vol_t *cvol, uint32_t lba)
{
    exfat_vol_t *vol = (exfat_vol_t *)(uintptr_t)cvol;
    if (vol->fat_cache_valid && vol->fat_cache_lba == lba) {
        return vol->fat_cache;
    }
    if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, vol->fat_cache) != 0) {
        vol->fat_cache_valid = 0u;
        return 0;
    }
    vol->fat_cache_lba   = lba;
    vol->fat_cache_valid = 1u;
    return vol->fat_cache;
}

static int next_cluster(const exfat_vol_t *vol, uint32_t cur, uint8_t no_fat_chain, uint32_t *out_next)
{
    if (cur < 2u || (cur - 2u) >= vol->cluster_count) {
        return -1;
    }
    if (no_fat_chain) {
        uint32_t nxt = cur + 1u;
        *out_next = ((nxt - 2u) < vol->cluster_count) ? nxt : 0u;
        return 0;
    }

    uint32_t fat_sec_off = cur >> 7;
    uint32_t fat_in_sec  = (cur & 127u) << 2;
    if (fat_sec_off >= vol->fat_sectors) {
        return -1;
    }

    const uint8_t *sec = fat_read_sector_cached(vol, vol->fat_lba + fat_sec_off);
    if (!sec) {
        return -1;
    }
    uint32_t val = le32(sec + fat_in_sec);
    if (val >= 0xFFFFFFF8u || val < 2u || (val - 2u) >= vol->cluster_count) {
        *out_next = 0u; /* EOF or invalid terminator */
        return 0;
    }
    *out_next = val;
    return 0;
}

static int next_bitmap_cluster(const exfat_vol_t *vol, uint32_t cur, uint32_t *out_next)
{
    uint32_t val = 0u;
    if (fat_get_entry(vol, cur, &val) != 0) {
        return -1;
    }
    if (val >= 0xFFFFFFF8u) {
        *out_next = 0u; /* Explicit FAT EOF */
        return 0;
    }
    if (val >= 2u && (val - 2u) < vol->cluster_count) {
        *out_next = val;
        return 0;
    }
    if (val == 0u) {
        /* Contiguous bitmap fallback when FAT entries are unpopulated (0) */
        uint32_t contig = cur + 1u;
        if ((contig - 2u) < vol->cluster_count) {
            *out_next = contig;
            return 0;
        }
    }
    *out_next = 0u;
    return 0;
}

static int scan_root_bitmap(exfat_vol_t *vol, uint8_t active_fat)
{
    uint32_t cls   = vol->root_dir_cluster;
    uint32_t steps = 0u;
    uint8_t buf[4u * 512u];

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        if (++steps > vol->cluster_count) {
            return -1; /* Corrupted FAT cycle */
        }
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s = 0u; s < vol->sectors_per_cluster; ) {
            uint32_t chunk = vol->sectors_per_cluster - s;
            if (chunk > 4u) {
                chunk = 4u;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s, chunk, buf) != 0) {
                return -1;
            }
            for (uint32_t cs = 0u; cs < chunk; cs++) {
                const uint8_t *sec = &buf[cs * 512u];
                for (uint32_t idx = 0u; idx < 16u; idx++) {
                    const uint8_t *d = &sec[idx * 32u];
                    if (d[0] == 0x00u) {
                        return 0;
                    }
                    if (d[0] == 0x81u && (d[1] & 1u) == active_fat) {
                        uint32_t bmp_cls = le32(d + 20u);
                        uint64_t bmp_len = le64(d + 24u);
                        if (bmp_cls >= 2u && (bmp_cls - 2u) < vol->cluster_count) {
                            vol->bitmap_cluster = bmp_cls;
                            vol->bitmap_bytes   = (bmp_len > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)bmp_len;
                            return 0;
                        }
                    }
                }
            }
            s += chunk;
        }
        uint32_t nxt = 0u;
        if (next_cluster(vol, cls, 0u, &nxt) != 0) {
            return -1;
        }
        cls = nxt;
    }
    return 0;
}

int exfat_mount(exfat_vol_t *vol, const blkdev_t *dev)
{
    if (!vol || !dev || !dev->read_sectors) {
        return -1;
    }
    vol->free_valid = 0u;   /* a failed remount must not leave a stale free count behind (mem_zero below on success) */

    uint8_t sec[512];
    if (dev->read_sectors(dev->ctx, 0u, 1u, sec) != 0) {
        return -1;
    }

    uint32_t part_lba = 0u;
    if (!is_exfat_vbr(sec)) {
        if (sec[510] != 0x55u || sec[511] != 0xAAu) {
            return -1;
        }
        uint8_t mbr[512];
        mem_copy(mbr, sec, 512u);
        int found = 0;
        for (uint32_t p = 0u; p < 4u; p++) {
            const uint8_t *pe = &mbr[0x1BEu + p * 16u];
            if (pe[4] == 0x00u) {
                continue;
            }
            uint32_t cand_lba = le32(pe + 8u);
            if (cand_lba == 0u || (dev->total_sectors > 0u && cand_lba >= dev->total_sectors)) {
                continue;
            }
            if (dev->read_sectors(dev->ctx, cand_lba, 1u, sec) == 0 && is_exfat_vbr(sec)) {
                part_lba = cand_lba;
                found = 1;
                break;
            }
        }
        if (!found) {
            return -1;
        }
    }

    uint8_t bps_shift = sec[0x6Cu];
    uint8_t spc_shift = sec[0x6Du];
    if (bps_shift != 9u || spc_shift > 16u) {
        return -1;
    }

    uint8_t num_fats = sec[0x6Eu];
    if (num_fats > 2u) {
        return -1;
    }
    uint8_t  fats_eff   = (num_fats == 2u) ? 2u : 1u;
    uint16_t vol_flags  = le16(sec + 0x6Au);
    uint8_t  active_fat = (num_fats == 2u) ? (uint8_t)(vol_flags & 1u) : 0u;

    uint32_t fat_off  = le32(sec + 0x50u);
    uint32_t fat_len  = le32(sec + 0x54u);
    uint32_t heap_off = le32(sec + 0x58u);
    uint32_t clst_cnt = le32(sec + 0x5Cu);
    uint32_t root_cls = le32(sec + 0x60u);

    if (clst_cnt == 0u || clst_cnt > 0xFFFFFFF0u ||
        root_cls < 2u || (root_cls - 2u) >= clst_cnt ||
        fat_off == 0u || fat_len == 0u || heap_off == 0u) {
        return -1;
    }

    uint64_t fat_end_off = (uint64_t)fat_off + (uint64_t)fat_len * (uint64_t)fats_eff;
    if (fat_end_off > (uint64_t)heap_off) {
        return -1;
    }
    uint64_t fat_lba64   = (uint64_t)part_lba + (uint64_t)fat_off
                         + (active_fat ? (uint64_t)fat_len : 0u);
    uint64_t heap_lba64  = (uint64_t)part_lba + (uint64_t)heap_off;
    uint64_t data_secs64 = (uint64_t)clst_cnt << spc_shift;
    uint64_t total_end64 = heap_lba64 + data_secs64;
    if (fat_lba64 > 0xFFFFFFFFULL || heap_lba64 > 0xFFFFFFFFULL ||
        data_secs64 > 0xFFFFFFFFULL || total_end64 > 0xFFFFFFFFULL) {
        return -1;
    }
    if (dev->total_sectors > 0u && total_end64 > (uint64_t)dev->total_sectors) {
        return -1;
    }

    mem_zero(vol, sizeof(*vol));
    vol->dev                 = dev;
    vol->part_lba            = part_lba;
    vol->fat_lba             = (uint32_t)fat_lba64;
    vol->fat_sectors         = fat_len;
    vol->heap_lba            = (uint32_t)heap_lba64;
    vol->cluster_count       = clst_cnt;
    vol->root_dir_cluster    = root_cls;
    vol->spc_shift           = spc_shift;
    vol->sectors_per_cluster = 1u << spc_shift;
    vol->cluster_size        = 512u << spc_shift;
    vol->default_timestamp   = EXFAT_DOS_TIMESTAMP_DEFAULT;

    (void)scan_root_bitmap(vol, active_fat);
    return 0;
}

void exfat_set_timestamp(exfat_vol_t *vol, uint32_t dos_timestamp)
{
    if (vol && dos_timestamp != 0u) {
        vol->default_timestamp = dos_timestamp;
    }
}

typedef struct {
    uint32_t cls;
    uint32_t s;
    uint8_t  slot;
    uint8_t  no_fat_chain;
} exfat_parent_loc_t;

typedef struct {
    uint32_t cls;
    uint32_t s;
    uint32_t lba;
    uint8_t  slot;
    uint8_t  sec_cnt;
} exfat_dir_loc_t;

typedef int (*exfat_dir_loc_cb_t)(const exfat_entry_t *entry, const exfat_dir_loc_t *loc, void *user);

/* The directory walker behind every listing and lookup. Starts at *pos when pos names a cluster (else at the start),
 * hands each complete entry set to cb / loc_cb / pos_cb (pos_cb also gets the position right after the set), and asks
 * yield before each card read except the first one of the call and never inside an entry set (so every call makes
 * progress and no set is cut). Returns 0 at the end of the directory, 1 when a callback stopped the walk, 2 when yield
 * did (*pos = where to resume: every entry before it went to the callback), -1 on a read error. */
static int dir_walk(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain, uint64_t dir_size,
                    exfat_dir_cb_t cb, exfat_dir_loc_cb_t loc_cb, exfat_dir_pos_cb_t pos_cb,
                    exfat_dir_yield_t yield, void *user, exfat_dir_pos_t *pos)
{
    if (!vol || (!cb && !loc_cb && !pos_cb) || dir_cluster < 2u) {
        return -1;
    }

    exfat_entry_t cur_ent;
    uint8_t in_set       = 0u;
    uint8_t sec_expected = 0u;
    uint8_t sec_seen     = 0u;
    uint8_t have_stream  = 0u;
    uint8_t name_len     = 0u;
    uint8_t name_pos     = 0u;
    exfat_dir_loc_t prim_loc;
    mem_zero(&prim_loc, sizeof(prim_loc));
    mem_zero(&cur_ent, sizeof(cur_ent));

    uint32_t cls_shift = 9u + (uint32_t)vol->spc_shift;
    uint32_t max_cls   = 0u;
    if (no_fat_chain) {
        if (dir_size > 0u) {
            uint64_t mc = (dir_size + ((uint64_t)vol->cluster_size - 1u)) >> cls_shift;
            max_cls = (mc > vol->cluster_count) ? vol->cluster_count : (uint32_t)mc;
        } else {
            max_cls = 1u;
        }
    }

    uint32_t cls       = dir_cluster;
    uint32_t cls_idx   = 0u;
    uint32_t s_start   = 0u;
    uint32_t slot0     = 0u;
    if (pos && pos->cls >= 2u) {
        cls     = pos->cls;
        cls_idx = pos->cls_idx;
        s_start = pos->sec;
        slot0   = pos->slot;
        if (slot0 >= 16u) {                      /* "after the last slot of a sector" */
            s_start++;
            slot0 = 0u;
        }
        if (s_start >= vol->sectors_per_cluster) {   /* "after the last sector of a cluster" */
            s_start = 0u;
            cls_idx++;
            if (no_fat_chain && cls_idx >= max_cls) {
                return 0;
            }
            uint32_t nxt = 0u;
            if (next_cluster(vol, cls, no_fat_chain, &nxt) != 0) {
                return -1;
            }
            cls = nxt;
        }
    }
    uint8_t did_read = 0u;
    uint8_t buf[4u * 512u];

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        if (cls_idx >= vol->cluster_count) {
            return -1; /* Corrupted FAT cycle */
        }
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s_base = s_start; s_base < vol->sectors_per_cluster; ) {
            uint32_t chunk = vol->sectors_per_cluster - s_base;
            if (chunk > 4u) {
                chunk = 4u;
            }
            if (yield && did_read && !in_set && yield(user) != 0) {
                if (pos) {
                    pos->cls     = cls;
                    pos->cls_idx = cls_idx;
                    pos->sec     = s_base;
                    pos->slot    = 0u;
                }
                return 2;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_base, chunk, buf) != 0) {
                return -1;
            }
            did_read = 1u;
            for (uint32_t cs = 0u; cs < chunk; cs++) {
                uint32_t s   = s_base + cs;
                uint32_t lba = base_lba + s;
                const uint8_t *sec = &buf[cs * 512u];
                for (uint32_t idx = slot0; idx < 16u; idx++) {
                const uint8_t *d = &sec[idx * 32u];
                uint8_t etype = d[0];
                if (etype == 0x00u) {
                    return 0; /* End of directory table */
                }
                if ((etype & 0x80u) == 0u) {
                    in_set = 0u; /* Deleted / free entry */
                    continue;
                }
                if (etype == 0x85u) {
                    in_set = (d[1] >= 2u) ? 1u : 0u;
                    sec_expected = d[1];
                    sec_seen = 0u;
                    have_stream = 0u;
                    name_len = 0u;
                    name_pos = 0u;
                    prim_loc.cls     = cls;
                    prim_loc.s       = s;
                    prim_loc.lba     = lba;
                    prim_loc.slot    = (uint8_t)idx;
                    prim_loc.sec_cnt = d[1];
                    cur_ent.created_timestamp  = le32(d + 8u);
                    cur_ent.modified_timestamp = le32(d + 12u);
                    cur_ent.file_attr      = le16(d + 4u);
                    cur_ent.is_dir         = (uint8_t)((cur_ent.file_attr & EXFAT_ATTR_DIR) != 0u);
                    cur_ent.size           = 0u;
                    cur_ent.valid_data_len = 0u;
                    cur_ent.name[0]        = '\0';
                    continue;
                }
                if (!in_set) {
                    continue;
                }

                sec_seen++;
                if (etype == 0xC0u) {
                    uint64_t vdl = le64(d + 8u);
                    uint64_t dsz = le64(d + 24u);
                    cur_ent.no_fat_chain   = (uint8_t)((d[1] >> 1) & 1u);
                    name_len               = d[3];
                    cur_ent.first_cluster  = le32(d + 20u);
                    cur_ent.size           = dsz;
                    cur_ent.valid_data_len = (vdl < dsz) ? vdl : dsz;
                    have_stream            = 1u;
                } else if (etype == 0xC1u) {
                    for (uint32_t k = 0u; k < 15u && name_pos < name_len; k++) {
                        uint16_t ch = le16(d + 2u + k * 2u);
                        if (name_pos < (EXFAT_MAX_NAME - 1u)) {
                            cur_ent.name[name_pos] = (ch > 0u && ch < 0x80u) ? (char)ch : '_';
                        }
                        name_pos++;
                    }
                    uint32_t nul_pos = (name_pos < EXFAT_MAX_NAME) ? name_pos : (EXFAT_MAX_NAME - 1u);
                    cur_ent.name[nul_pos] = '\0';
                }

                if (sec_seen >= sec_expected) {
                    in_set = 0u;
                    if (have_stream && cur_ent.name[0] != '\0') {
                        int stop;
                        if (pos_cb) {
                            exfat_dir_pos_t next;
                            next.cls     = cls;
                            next.cls_idx = cls_idx;
                            next.sec     = s;
                            next.slot    = idx + 1u;
                            stop = pos_cb(&cur_ent, &next, user);
                        } else {
                            stop = loc_cb ? loc_cb(&cur_ent, &prim_loc, user) : cb(&cur_ent, user);
                        }
                        if (stop != 0) {
                            return 1;
                        }
                    }
                }
                }
                slot0 = 0u;
            }
            s_base += chunk;
        }
        s_start = 0u;
        cls_idx++;
        if (no_fat_chain && cls_idx >= max_cls) {
            break;
        }
        uint32_t nxt = 0u;
        if (next_cluster(vol, cls, no_fat_chain, &nxt) != 0) {
            return -1;
        }
        cls = nxt;
    }

    return 0;
}

static int list_dir_with_loc(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain,
                             uint64_t dir_size, exfat_dir_cb_t cb, exfat_dir_loc_cb_t loc_cb, void *user)
{
    int rc = dir_walk(vol, dir_cluster, no_fat_chain, dir_size, cb, loc_cb, 0, 0, user, 0);
    return (rc < 0) ? -1 : 0;
}

int exfat_list_dir(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain,
                   exfat_dir_cb_t cb, void *user)
{
    return list_dir_with_loc(vol, dir_cluster, no_fat_chain, 0u, cb, 0, user);
}

int exfat_list_dir_entry(const exfat_vol_t *vol, const exfat_entry_t *dir,
                         exfat_dir_cb_t cb, void *user)
{
    if (!dir || !dir->is_dir) {
        return -1;
    }
    return list_dir_with_loc(vol, dir->first_cluster, dir->no_fat_chain, dir->size, cb, 0, user);
}

int exfat_list_dir_resume(const exfat_vol_t *vol, const exfat_entry_t *dir, exfat_dir_pos_t *pos,
                          exfat_dir_pos_cb_t cb, exfat_dir_yield_t yield, void *user)
{
    if (!dir || !dir->is_dir || !pos || !cb) {
        return -1;
    }
    return dir_walk(vol, dir->first_cluster, dir->no_fat_chain, dir->size, 0, 0, cb, yield, user, pos);
}

typedef struct {
    const char       *name;
    uint32_t          name_len;
    exfat_entry_t    *out;
    exfat_dir_yield_t yield;
    void             *yield_user;
} find_resume_ctx_t;

static int find_resume_cb(const exfat_entry_t *entry, const exfat_dir_pos_t *next, void *user)
{
    (void)next;
    find_resume_ctx_t *fc = (find_resume_ctx_t *)user;
    if (str_eq_ci_n(entry->name, fc->name, fc->name_len)) {
        *fc->out = *entry;
        return 1;
    }
    return 0;
}

static int find_resume_yield(void *user)
{
    find_resume_ctx_t *fc = (find_resume_ctx_t *)user;
    return fc->yield(fc->yield_user);
}

int exfat_find_in_dir_resume(const exfat_vol_t *vol, const exfat_entry_t *dir, const char *name,
                             exfat_dir_pos_t *pos, exfat_dir_yield_t yield, void *user, exfat_entry_t *out)
{
    if (!dir || !dir->is_dir || !name || !pos || !out) {
        return -1;
    }
    find_resume_ctx_t fc;
    fc.name     = name;
    fc.name_len = 0u;
    while (name[fc.name_len] != '\0') {
        fc.name_len++;
    }
    fc.out        = out;
    fc.yield      = yield;
    fc.yield_user = user;
    int rc = dir_walk(vol, dir->first_cluster, dir->no_fat_chain, dir->size, 0, 0, find_resume_cb,
                      yield ? find_resume_yield : 0, &fc, pos);
    if (rc == 1) {
        return 0;                                /* found */
    }
    if (rc == 0) {
        return 1;                                /* not in the directory */
    }
    return rc;                                   /* 2: yielded (*pos), -1: read error */
}

typedef struct {
    const char     *target;
    uint32_t        target_len;
    exfat_entry_t   result;
    exfat_dir_loc_t loc;
    int             found;
} find_ctx_t;

static int find_entry_loc_cb(const exfat_entry_t *entry, const exfat_dir_loc_t *loc, void *user)
{
    find_ctx_t *ctx = (find_ctx_t *)user;
    if (str_eq_ci_n(entry->name, ctx->target, ctx->target_len)) {
        ctx->result = *entry;
        ctx->loc    = *loc;
        ctx->found  = 1;
        return 1;
    }
    return 0;
}

static int find_path_with_loc(const exfat_vol_t *vol, const char *path, exfat_entry_t *out,
                              exfat_parent_loc_t *out_parent)
{
    if (!vol || !path || !out) {
        return -1;
    }

    exfat_entry_t cur = {0};
    cur.name[0]        = '/';
    cur.name[1]        = '\0';
    cur.size           = 0u;
    cur.valid_data_len = 0u;
    cur.first_cluster  = vol->root_dir_cluster;
    cur.file_attr      = EXFAT_ATTR_DIR;
    cur.no_fat_chain   = 0u;
    cur.is_dir         = 1u;

    exfat_parent_loc_t ploc;
    mem_zero(&ploc, sizeof(ploc));

    const char *p = path;
    while (*p == '/' || *p == '\\') {
        p++;
    }
    if (*p == '\0') {
        *out = cur;
        if (out_parent) *out_parent = ploc;
        return 0;
    }

    while (*p != '\0') {
        if (!cur.is_dir) {
            return -1;
        }
        const char *seg = p;
        uint32_t seg_len = 0u;
        while (p[seg_len] != '\0' && p[seg_len] != '/' && p[seg_len] != '\\') {
            seg_len++;
        }

        find_ctx_t ctx;
        ctx.target     = seg;
        ctx.target_len = seg_len;
        ctx.found      = 0;
        if (list_dir_with_loc(vol, cur.first_cluster, cur.no_fat_chain, cur.size,
                              0, find_entry_loc_cb, &ctx) != 0 || !ctx.found) {
            return -1;
        }
        ploc.cls          = ctx.loc.cls;
        ploc.s            = ctx.loc.s;
        ploc.slot         = ctx.loc.slot;
        ploc.no_fat_chain = cur.no_fat_chain;
        cur               = ctx.result;

        p += seg_len;
        while (*p == '/' || *p == '\\') {
            p++;
        }
    }

    *out = cur;
    if (out_parent) *out_parent = ploc;
    return 0;
}

int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *out)
{
    return find_path_with_loc(vol, path, out, 0);
}

int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *file,
                    uint64_t offset, void *dst, uint32_t len, uint32_t *bytes_read)
{
    if (!vol || !file || (!dst && len > 0u)) {
        return -1;
    }
    if (bytes_read) {
        *bytes_read = 0u;
    }
    if (file->is_dir || offset >= file->size || len == 0u) {
        return 0;
    }

    uint64_t avail = file->size - offset;
    uint32_t want  = (avail < (uint64_t)len) ? (uint32_t)avail : len;
    uint64_t vdl   = (file->valid_data_len < file->size) ? file->valid_data_len : file->size;
    uint8_t *out   = (uint8_t *)dst;

    if (offset >= vdl) {
        /* Entire requested range is in [valid_data_len, size) -> zero-fill */
        mem_zero(out, want);
        if (bytes_read) {
            *bytes_read = want;
        }
        return 0;
    }

    uint64_t disk_avail = vdl - offset;
    uint32_t disk_want  = (disk_avail < (uint64_t)want) ? (uint32_t)disk_avail : want;
    uint32_t zero_tail  = want - disk_want;
    uint32_t cls        = file->first_cluster;

    uint32_t cls_shift     = 9u + (uint32_t)vol->spc_shift;
    uint64_t skip_clusters = offset >> cls_shift;
    uint32_t in_cls_off    = (uint32_t)(offset & ((uint64_t)vol->cluster_size - 1u));
    uint32_t cur_idx       = 0u;
    exfat_vol_t *mvol      = (exfat_vol_t *)(uintptr_t)vol;
    int      cache_slot    = -1;

    if (skip_clusters >= (uint64_t)vol->cluster_count) {
        return -1;
    }

    if (file->no_fat_chain) {
        if (cls < 2u || (cls - 2u) >= vol->cluster_count ||
            skip_clusters >= (uint64_t)(vol->cluster_count - (cls - 2u))) {
            return -1;
        }
        cls    += (uint32_t)skip_clusters;
        cur_idx = (uint32_t)skip_clusters;
    } else {
        uint64_t start_idx = 0u;
        for (int ci = 0; ci < 4; ci++) {
            if (mvol->rd_cache[ci].first_cluster == file->first_cluster &&
                mvol->rd_cache[ci].cluster >= 2u) {
                cache_slot = ci;
                if ((uint64_t)mvol->rd_cache[ci].cluster_idx <= skip_clusters) {
                    cls       = mvol->rd_cache[ci].cluster;
                    start_idx = (uint64_t)mvol->rd_cache[ci].cluster_idx;
                }
                break;
            }
        }
        for (uint64_t i = start_idx; i < skip_clusters; i++) {
            uint32_t nxt = 0u;
            if (next_cluster(vol, cls, 0u, &nxt) != 0 || nxt == 0u) {
                return -1;
            }
            cls = nxt;
        }
        cur_idx = (uint32_t)skip_clusters;
    }

    uint32_t done = 0u;
    uint8_t sec[512];

    while (done < disk_want) {
        if (cls < 2u || (cls - 2u) >= vol->cluster_count) {
            return -1;
        }
        uint32_t base_lba = cluster_to_lba(vol, cls);
        uint32_t s_idx    = in_cls_off >> 9;
        uint32_t s_off    = in_cls_off & 511u;

        while (s_idx < vol->sectors_per_cluster && done < disk_want) {
            uint32_t rem = disk_want - done;
            if (s_off == 0u && rem >= 512u) {
                /* Multi-sector batched read (CMD18) within this cluster */
                uint32_t nsec     = rem >> 9;
                uint32_t cls_rem  = vol->sectors_per_cluster - s_idx;
                if (nsec > cls_rem) {
                    nsec = cls_rem;
                }
                if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_idx, nsec, out + done) != 0) {
                    return -1;
                }
                done  += nsec << 9;
                s_idx += nsec;
                continue;
            }
            uint32_t take = 512u - s_off;
            if (take > rem) {
                take = rem;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_idx, 1u, sec) != 0) {
                return -1;
            }
            mem_copy(out + done, sec + s_off, take);
            done  += take;
            s_idx++;
            s_off  = 0u;
        }
        in_cls_off = 0u;
        if (done < disk_want) {
            uint32_t nxt = 0u;
            if (next_cluster(vol, cls, file->no_fat_chain, &nxt) != 0 || nxt == 0u) {
                return -1;
            }
            cls = nxt;
            cur_idx++;
        }
    }

    if (zero_tail > 0u) {
        mem_zero(out + done, zero_tail);
        done += zero_tail;
    }

    if (!file->no_fat_chain && cls >= 2u) {
        if (cache_slot < 0) {
            cache_slot = (int)(mvol->rd_cache_rr & 3u);
            mvol->rd_cache_rr++;
        }
        mvol->rd_cache[cache_slot].first_cluster = file->first_cluster;
        mvol->rd_cache[cache_slot].cluster_idx   = cur_idx;
        mvol->rd_cache[cache_slot].cluster       = cls;
    }

    if (bytes_read) {
        *bytes_read = done;
    }
    return 0;
}

/* ============================================================================
 * Write Support: Allocation Bitmap, FAT Chain, Checksums, Directory & File I/O
 * ============================================================================ */

static int fat_get_entry(const exfat_vol_t *vol, uint32_t cluster, uint32_t *out_val)
{
    if (cluster < 2u || (cluster - 2u) >= vol->cluster_count) {
        return -1;
    }
    uint32_t fat_sec_off = cluster >> 7;
    uint32_t fat_in_sec  = (cluster & 127u) << 2;
    if (fat_sec_off >= vol->fat_sectors) {
        return -1;
    }

    const uint8_t *sec = fat_read_sector_cached(vol, vol->fat_lba + fat_sec_off);
    if (!sec) {
        return -1;
    }
    *out_val = le32(sec + fat_in_sec);
    return 0;
}

static int fat_set_entry(const exfat_vol_t *cvol, uint32_t cluster, uint32_t value)
{
    if (cluster < 2u || (cluster - 2u) >= cvol->cluster_count) {
        return -1;
    }
    uint32_t fat_sec_off = cluster >> 7;
    uint32_t fat_in_sec  = (cluster & 127u) << 2;
    if (fat_sec_off >= cvol->fat_sectors) {
        return -1;
    }

    uint32_t lba = cvol->fat_lba + fat_sec_off;
    if (!fat_read_sector_cached(cvol, lba)) {
        return -1;
    }
    exfat_vol_t *vol = (exfat_vol_t *)(uintptr_t)cvol;
    put_le32(vol->fat_cache + fat_in_sec, value);
    if (dev_write(vol, lba, 1u, vol->fat_cache) != 0) {
        vol->fat_cache_valid = 0u;
        return -1;
    }
    return 0;
}

/* Free-cluster cache bookkeeping (F4): only bits that really changed state move the count; an I/O error drops it. */
static void free_cache_add(exfat_vol_t *vol, int32_t delta)
{
    if (!vol->free_valid) {
        return;
    }
    int64_t n = (int64_t)vol->free_clusters + delta;
    if (n < 0 || n > (int64_t)vol->cluster_count) {
        vol->free_valid = 0u;   /* inconsistent: recount at the next stat */
        return;
    }
    vol->free_clusters = (uint32_t)n;
}

void exfat_invalidate_free_cache(exfat_vol_t *vol)
{
    if (vol) {
        vol->free_valid = 0u;
    }
}

static int scan_bitmap_range(exfat_vol_t *vol, uint32_t start_byte, uint32_t end_byte,
                             uint32_t prev_cluster, uint32_t *out_cluster)
{
    uint32_t cls_shift   = 9u + (uint32_t)vol->spc_shift;
    uint32_t cls_mask    = vol->cluster_size - 1u;
    uint32_t target_cidx = start_byte >> cls_shift;

    uint32_t bmp_cls = vol->bitmap_cluster;
    for (uint32_t c = 0u; c < target_cidx; c++) {
        uint32_t nxt = 0u;
        if (next_bitmap_cluster(vol, bmp_cls, &nxt) != 0 || nxt == 0u) {
            return 1; /* Not found / end */
        }
        bmp_cls = nxt;
    }

    uint32_t cur_byte = target_cidx << cls_shift;
    uint32_t s_start  = (start_byte & cls_mask) >> 9;
    uint8_t *buf      = vol->io_buf;

    while (bmp_cls >= 2u && cur_byte < end_byte) {
        uint32_t base_lba = cluster_to_lba(vol, bmp_cls);
        for (uint32_t s_base = s_start; s_base < vol->sectors_per_cluster && cur_byte + (s_base << 9) < end_byte; ) {
            uint32_t rem_bytes = end_byte - (cur_byte + (s_base << 9));
            uint32_t need_secs = (rem_bytes + 511u) >> 9;
            uint32_t chunk     = vol->sectors_per_cluster - s_base;
            if (chunk > need_secs) {
                chunk = need_secs;
            }
            if (chunk > 4u) {
                chunk = 4u;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_base, chunk, buf) != 0) {
                return -1;
            }
            for (uint32_t cs = 0u; cs < chunk; cs++) {
                uint32_t s        = s_base + cs;
                uint32_t sec_base = cur_byte + (s << 9);
                if (sec_base >= end_byte) {
                    return 1;
                }
                uint8_t *sec     = &buf[cs * 512u];
                uint32_t b_start = (start_byte > sec_base) ? (start_byte - sec_base) : 0u;
                uint32_t b_end   = (end_byte - sec_base < 512u) ? (end_byte - sec_base) : 512u;
                for (uint32_t b = b_start; b < b_end; b++) {
                    if (sec[b] == 0xFFu) {
                        continue;
                    }
                    for (uint32_t bit = 0u; bit < 8u; bit++) {
                        if (sec[b] & (1u << bit)) {
                            continue;
                        }
                        uint32_t idx = ((sec_base + b) << 3) + bit;
                        if (idx >= vol->cluster_count) {
                            return 1;
                        }
                        uint32_t new_c = idx + 2u;
                        sec[b] |= (uint8_t)(1u << bit);
                        if (dev_write(vol, base_lba + s, 1u, sec) != 0) {
                            vol->free_valid = 0u;   /* the bit may or may not be on the card */
                            return -1;
                        }
                        int fat_rc = 0;
                        if (prev_cluster >= 2u && (prev_cluster >> 7) == (new_c >> 7)) {
                            /* Coalesce both FAT updates into a single cached sector write */
                            uint32_t fat_sec_off = new_c >> 7;
                            if (fat_sec_off >= vol->fat_sectors) {
                                fat_rc = -1;
                            } else {
                                uint32_t flba = vol->fat_lba + fat_sec_off;
                                if (!fat_read_sector_cached(vol, flba)) {
                                    fat_rc = -1;
                                } else {
                                    put_le32(vol->fat_cache + ((new_c & 127u) << 2), 0xFFFFFFFFu);
                                    put_le32(vol->fat_cache + ((prev_cluster & 127u) << 2), new_c);
                                    if (dev_write(vol, flba, 1u, vol->fat_cache) != 0) {
                                        vol->fat_cache_valid = 0u;
                                        fat_rc = -1;
                                    }
                                }
                            }
                        } else {
                            if (fat_set_entry(vol, new_c, 0xFFFFFFFFu) != 0) {
                                fat_rc = -1;
                            } else if (prev_cluster >= 2u && fat_set_entry(vol, prev_cluster, new_c) != 0) {
                                (void)fat_set_entry(vol, new_c, 0u);
                                fat_rc = -1;
                            }
                        }
                        if (fat_rc != 0) {
                            sec[b] &= (uint8_t)~(1u << bit);
                            if (dev_write(vol, base_lba + s, 1u, sec) != 0) {
                                vol->free_valid = 0u;   /* the rollback did not reach the card */
                            }
                            return -1;
                        }
                        free_cache_add(vol, -1);
                        vol->alloc_hint = idx + 1u;
                        *out_cluster    = new_c;
                        return 0;
                    }
                }
            }
            s_base += chunk;
        }
        s_start  = 0u;
        cur_byte += vol->cluster_size;
        uint32_t nxt = 0u;
        if (next_bitmap_cluster(vol, bmp_cls, &nxt) != 0) {
            return -1;
        }
        bmp_cls = nxt;
    }

    return 1;
}

static int alloc_cluster(exfat_vol_t *vol, uint32_t prev_cluster, uint32_t *out_cluster)
{
    if (!vol || !vol->dev->write_sectors || vol->bitmap_cluster < 2u || vol->bitmap_bytes == 0u) {
        return -1;
    }

    uint32_t limit = (vol->cluster_count + 7u) >> 3;
    if (vol->bitmap_bytes < limit) {
        limit = vol->bitmap_bytes;
    }

    uint32_t hint_byte = vol->alloc_hint >> 3;
    if (hint_byte >= limit) {
        hint_byte = 0u;
    }

    int rc = scan_bitmap_range(vol, hint_byte, limit, prev_cluster, out_cluster);
    if (rc == 0) {
        return 0;
    }
    if (rc < 0 || hint_byte == 0u) {
        return -1;
    }
    return (scan_bitmap_range(vol, 0u, hint_byte + 1u, prev_cluster, out_cluster) == 0) ? 0 : -1;
}

static int free_cluster_bit(exfat_vol_t *vol, uint32_t cluster)
{
    if (!vol || !vol->dev->write_sectors || vol->bitmap_cluster < 2u ||
        cluster < 2u || (cluster - 2u) >= vol->cluster_count) {
        return -1;
    }

    uint32_t idx      = cluster - 2u;
    uint32_t byte_off = idx >> 3;
    uint32_t bit      = idx & 7u;
    if (byte_off >= vol->bitmap_bytes) {
        return -1;
    }

    uint32_t cls_shift   = 9u + (uint32_t)vol->spc_shift;
    uint32_t target_cidx = byte_off >> cls_shift;
    uint32_t sec_in_cls  = (byte_off & (vol->cluster_size - 1u)) >> 9;
    uint32_t in_sec      = byte_off & 511u;

    uint32_t bmp_cls = vol->bitmap_cluster;
    for (uint32_t c = 0u; c < target_cidx; c++) {
        uint32_t nxt = 0u;
        if (next_bitmap_cluster(vol, bmp_cls, &nxt) != 0 || nxt == 0u) {
            return -1;
        }
        bmp_cls = nxt;
    }

    uint8_t sec[512];
    uint32_t lba = cluster_to_lba(vol, bmp_cls) + sec_in_cls;
    if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
        return -1;
    }
    int was_set = (sec[in_sec] & (1u << bit)) != 0u;
    sec[in_sec] &= (uint8_t)~(1u << bit);
    if (dev_write(vol, lba, 1u, sec) != 0) {
        vol->free_valid = 0u;
        return -1;
    }
    if (was_set) {
        free_cache_add(vol, 1);
    }

    if (idx < vol->alloc_hint) {
        vol->alloc_hint = idx;
    }
    return 0;
}

static int zero_cluster(exfat_vol_t *vol, uint32_t cluster)
{
    uint8_t *zero = vol->io_buf;
    mem_zero(zero, sizeof(vol->io_buf));
    uint32_t base_lba = cluster_to_lba(vol, cluster);
    uint32_t s = 0u;
    while (s < vol->sectors_per_cluster) {
        uint32_t chunk = vol->sectors_per_cluster - s;
        if (chunk > 4u) {
            chunk = 4u;
        }
        if (dev_write(vol, base_lba + s, chunk, zero) != 0) {
            return -1;
        }
        s += chunk;
    }
    return 0;
}

static uint16_t exfat_name_hash_ascii(const char *name, uint32_t len)
{
    uint16_t hash = 0u;
    for (uint32_t i = 0u; i < len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 'a' && c <= 'z') {
            c = (uint8_t)(c - ('a' - 'A'));
        }
        hash = (uint16_t)(((hash & 1u) ? 0x8000u : 0u) + (hash >> 1) + c);
        hash = (uint16_t)(((hash & 1u) ? 0x8000u : 0u) + (hash >> 1));
    }
    return hash;
}

static uint16_t checksum_add_entry(uint16_t sum, const uint8_t *ent, int is_primary)
{
    for (uint32_t i = 0u; i < 32u; i++) {
        if (is_primary && (i == 2u || i == 3u)) {
            continue;
        }
        sum = (uint16_t)(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + ent[i]);
    }
    return sum;
}

static uint16_t exfat_entry_set_checksum(const uint8_t *entries, uint32_t total_ents)
{
    uint16_t sum = 0u;
    for (uint32_t e = 0u; e < total_ents; e++) {
        sum = checksum_add_entry(sum, entries + e * 32u, e == 0u);
    }
    return sum;
}

static void build_entry_set(uint8_t *dst, uint32_t total_ents, const char *name, uint32_t name_len,
                            uint16_t file_attr, uint32_t first_cluster, uint64_t data_len,
                            uint32_t dos_timestamp)
{
    mem_zero(dst, total_ents * 32u);
    if (dos_timestamp == 0u) {
        dos_timestamp = EXFAT_DOS_TIMESTAMP_DEFAULT;
    }

    /* 0x85 Primary File Directory Entry */
    uint8_t *f = dst;
    f[0] = 0x85u;
    f[1] = (uint8_t)(total_ents - 1u);
    put_le16(f + 4u, file_attr);
    put_le32(f + 8u, dos_timestamp);
    put_le32(f + 12u, dos_timestamp);
    put_le32(f + 16u, dos_timestamp);
    f[22] = 0x00u; /* Local time (UTC offset not set per exFAT spec) */
    f[23] = 0x00u;
    f[24] = 0x00u;

    /* 0xC0 Stream Extension */
    uint8_t *s = dst + 32u;
    s[0] = 0xC0u;
    s[1] = 0x01u; /* AllocationPossible=1, NoFatChain=0 (FAT-chained) */
    s[3] = (uint8_t)name_len;
    put_le16(s + 4u, exfat_name_hash_ascii(name, name_len));
    put_le64(s + 8u, data_len);
    put_le32(s + 20u, first_cluster);
    put_le64(s + 24u, data_len);

    /* 0xC1 File Name entries (15 UTF-16LE chars per slot) */
    uint32_t pos = 0u;
    for (uint32_t e = 2u; e < total_ents; e++) {
        uint8_t *fn = dst + e * 32u;
        fn[0] = 0xC1u;
        for (uint32_t k = 0u; k < 15u && pos < name_len; k++, pos++) {
            put_le16(fn + 2u + k * 2u, (uint8_t)name[pos]);
        }
    }

    put_le16(f + 2u, exfat_entry_set_checksum(dst, total_ents));
}

static int dir_advance_sector(const exfat_vol_t *vol, uint8_t no_fat_chain,
                              uint32_t *cls, uint32_t *s)
{
    (*s)++;
    if (*s >= vol->sectors_per_cluster) {
        *s = 0u;
        uint32_t nxt = 0u;
        if (next_cluster(vol, *cls, no_fat_chain, &nxt) != 0 || nxt == 0u) {
            return -1;
        }
        *cls = nxt;
    }
    return 0;
}

static int grow_dir_size_in_parent(const exfat_vol_t *vol, const exfat_parent_loc_t *ploc,
                                   int clear_no_fat_chain)
{
    if (!ploc || ploc->cls < 2u) {
        return 0; /* Root dir has no parent entry */
    }
    uint8_t  sec0[512];
    uint32_t lba0 = cluster_to_lba(vol, ploc->cls) + ploc->s;
    if (vol->dev->read_sectors(vol->dev->ctx, lba0, 1u, sec0) != 0) {
        return -1;
    }
    uint8_t *f = &sec0[(uint32_t)ploc->slot * 32u];
    if (f[0] != 0x85u) {
        return -1;
    }
    uint32_t total_ents = 1u + (uint32_t)f[1];
    if (total_ents < 3u || total_ents > 19u) {
        return -1;
    }

    /* Fast path: entire entry set fits within sec0 */
    if ((uint32_t)ploc->slot + total_ents <= 16u) {
        uint8_t *s = &sec0[((uint32_t)ploc->slot + 1u) * 32u];
        if (s[0] != 0xC0u) {
            return -1;
        }
        if (clear_no_fat_chain) {
            s[1] &= (uint8_t)~0x02u; /* Clear NoFatChain */
        }
        uint64_t new_len = le64(s + 24u) + vol->cluster_size;
        put_le64(s + 8u, new_len);
        put_le64(s + 24u, new_len);
        put_le16(f + 2u, exfat_entry_set_checksum(f, total_ents));
        return dev_write_c(vol, lba0, 1u, sec0);
    }

    /* Sector-spanning path (handles slot 15 and entries crossing sector/cluster boundaries) */
    uint8_t  sec1[512];
    uint32_t lba1  = 0u;
    int      s_in_sec1 = (ploc->slot == 15u);

    uint32_t cur_cls = ploc->cls;
    uint32_t cur_s   = ploc->s;
    if (s_in_sec1) {
        if (dir_advance_sector(vol, ploc->no_fat_chain, &cur_cls, &cur_s) != 0) {
            return -1;
        }
        lba1 = cluster_to_lba(vol, cur_cls) + cur_s;
        if (vol->dev->read_sectors(vol->dev->ctx, lba1, 1u, sec1) != 0) {
            return -1;
        }
        if (sec1[0] != 0xC0u) {
            return -1;
        }
        if (clear_no_fat_chain) {
            sec1[1] &= (uint8_t)~0x02u;
        }
        uint64_t new_len = le64(sec1 + 24u) + vol->cluster_size;
        put_le64(sec1 + 8u, new_len);
        put_le64(sec1 + 24u, new_len);
    } else {
        uint8_t *s = &sec0[((uint32_t)ploc->slot + 1u) * 32u];
        if (s[0] != 0xC0u) {
            return -1;
        }
        if (clear_no_fat_chain) {
            s[1] &= (uint8_t)~0x02u;
        }
        uint64_t new_len = le64(s + 24u) + vol->cluster_size;
        put_le64(s + 8u, new_len);
        put_le64(s + 24u, new_len);
    }

    /* Recompute 16-bit SetChecksum across all spanning sectors */
    uint16_t sum = 0u;
    uint32_t done_ents = 0u;
    for (uint32_t sl = ploc->slot; sl < 16u && done_ents < total_ents; sl++, done_ents++) {
        sum = checksum_add_entry(sum, &sec0[sl * 32u], done_ents == 0u);
    }

    uint32_t w_cls = ploc->cls;
    uint32_t w_s   = ploc->s;
    uint8_t  tmp[512];
    int      sec1_consumed = 0;
    while (done_ents < total_ents) {
        if (dir_advance_sector(vol, ploc->no_fat_chain, &w_cls, &w_s) != 0) {
            return -1;
        }
        const uint8_t *src_sec;
        if (s_in_sec1 && !sec1_consumed) {
            src_sec = sec1;
            sec1_consumed = 1;
        } else {
            uint32_t wlba = cluster_to_lba(vol, w_cls) + w_s;
            if (vol->dev->read_sectors(vol->dev->ctx, wlba, 1u, tmp) != 0) {
                return -1;
            }
            src_sec = tmp;
        }
        for (uint32_t sl = 0u; sl < 16u && done_ents < total_ents; sl++, done_ents++) {
            sum = checksum_add_entry(sum, &src_sec[sl * 32u], 0);
        }
    }

    put_le16(f + 2u, sum);
    if (s_in_sec1) {
        if (dev_write_c(vol, lba1, 1u, sec1) != 0) {
            return -1;
        }
    }
    return dev_write_c(vol, lba0, 1u, sec0);
}

static int dir_alloc_slots(exfat_vol_t *vol, uint32_t dir_cluster, uint8_t *dir_no_fat_chain,
                           uint64_t dir_size, const exfat_parent_loc_t *parent_loc,
                           uint8_t total_ents, uint32_t *out_lba, uint8_t *out_slot)
{
    uint8_t  nfc       = dir_no_fat_chain ? *dir_no_fat_chain : 0u;
    uint32_t cls_shift = 9u + (uint32_t)vol->spc_shift;
    uint32_t max_cls   = 0u;
    if (nfc) {
        if (dir_size > 0u) {
            uint64_t mc = (dir_size + ((uint64_t)vol->cluster_size - 1u)) >> cls_shift;
            max_cls = (mc > vol->cluster_count) ? vol->cluster_count : (uint32_t)mc;
        } else {
            max_cls = 1u;
        }
    }

    uint32_t cls      = dir_cluster;
    uint32_t cls_idx  = 0u;
    uint32_t pad_lba  = 0u;
    uint8_t  pad_slot = 0u;
    uint8_t *buf      = vol->io_buf;

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s_base = 0u; s_base < vol->sectors_per_cluster; ) {
            uint32_t chunk = vol->sectors_per_cluster - s_base;
            if (chunk > 4u) {
                chunk = 4u;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_base, chunk, buf) != 0) {
                return -1;
            }
            for (uint32_t cs = 0u; cs < chunk; cs++) {
                uint32_t lba       = base_lba + s_base + cs;
                uint8_t *sec       = &buf[cs * 512u];
                uint32_t free_run  = 0u;
                uint32_t run_start = 0u;
                for (uint32_t idx = 0u; idx < 16u; idx++) {
                    uint8_t etype = sec[idx * 32u];
                    if (etype == 0x00u) {
                        if ((16u - idx) >= total_ents) {
                            /* Allocation succeeded: flush deferred 0x05 tail padding if any */
                            if (pad_lba != 0u) {
                                uint8_t *psec = vol->io_buf;
                                if (vol->dev->read_sectors(vol->dev->ctx, pad_lba, 1u, psec) != 0) {
                                    return -1;
                                }
                                for (uint32_t k = pad_slot; k < 16u; k++) {
                                    psec[k * 32u] = 0x05u;
                                }
                                if (dev_write(vol, pad_lba, 1u, psec) != 0) {
                                    return -1;
                                }
                            }
                            *out_lba  = lba;
                            *out_slot = (uint8_t)idx;
                            return 0;
                        }
                        /* Defer 0x05 tail padding until a subsequent slot/cluster succeeds */
                        if (pad_lba == 0u) {
                            pad_lba  = lba;
                            pad_slot = (uint8_t)idx;
                        }
                        break;
                    }
                    if ((etype & 0x80u) == 0u) {
                        if (free_run == 0u) {
                            run_start = idx;
                        }
                        free_run++;
                        if (free_run >= total_ents) {
                            *out_lba  = lba;
                            *out_slot = (uint8_t)run_start;
                            return 0;
                        }
                    } else {
                        free_run = 0u;
                    }
                }
            }
            s_base += chunk;
        }

        cls_idx++;
        uint32_t nxt = 0u;
        if (nfc && cls_idx >= max_cls) {
            nxt = 0u; /* Reached allocated end of contiguous NoFatChain directory */
        } else if (next_cluster(vol, cls, nfc, &nxt) != 0) {
            return -1;
        }
        if (nxt == 0u) {
            if (nfc && (!parent_loc || parent_loc->cls < 2u)) {
                return -1; /* Cannot clear NoFatChain without a parent entry */
            }
            /* Allocate & zero new cluster BEFORE linking it into the directory chain */
            if (alloc_cluster(vol, 0u, &nxt) != 0) {
                return -1;
            }
            if (zero_cluster(vol, nxt) != 0) {
                (void)fat_set_entry(vol, nxt, 0u);
                (void)free_cluster_bit(vol, nxt);
                return -1;
            }
            int fat_ok = 1;
            if (nfc) {
                /* Materialize FAT chain for existing contiguous NoFatChain directory clusters */
                for (uint32_t i = 0u; i + 1u < max_cls; i++) {
                    if (fat_set_entry(vol, dir_cluster + i, dir_cluster + i + 1u) != 0) {
                        fat_ok = 0;
                        break;
                    }
                }
            }
            if (fat_ok && fat_set_entry(vol, cls, nxt) != 0) {
                fat_ok = 0;
            }
            if (!fat_ok || grow_dir_size_in_parent(vol, parent_loc, nfc ? 1 : 0) != 0) {
                /* Roll back newly allocated cluster and FAT modifications */
                if (nfc) {
                    for (uint32_t i = 0u; i < max_cls; i++) {
                        (void)fat_set_entry(vol, dir_cluster + i, 0u);
                    }
                } else {
                    (void)fat_set_entry(vol, cls, 0xFFFFFFFFu);
                }
                (void)fat_set_entry(vol, nxt, 0u);
                (void)free_cluster_bit(vol, nxt);
                return -1;
            }
            if (nfc) {
                nfc = 0u;
                if (dir_no_fat_chain) {
                    *dir_no_fat_chain = 0u;
                }
            }
        }
        if (cls_idx >= vol->cluster_count) {
            return -1; /* Corrupted FAT cycle */
        }
        cls = nxt;
    }
    return -1;
}

int exfat_mkdir(exfat_vol_t *vol, const char *name)
{
    if (!vol || !vol->dev->write_sectors || !name) {
        return -1;
    }
    while (*name == '/' || *name == '\\') {
        name++;
    }
    uint32_t name_len = ascii_len_checked(name, 30u);
    if (name_len == 0u) {
        return -1;
    }

    exfat_entry_t existing;
    if (find_path_with_loc(vol, name, &existing, 0) == 0) {
        return existing.is_dir ? 0 : -1;
    }

    uint32_t dir_cls = 0u;
    if (alloc_cluster(vol, 0u, &dir_cls) != 0) {
        return -1;
    }
    if (zero_cluster(vol, dir_cls) != 0) {
        (void)fat_set_entry(vol, dir_cls, 0u);
        (void)free_cluster_bit(vol, dir_cls);
        return -1;
    }

    uint8_t  total_ents = (name_len <= 15u) ? 3u : 4u;
    uint32_t slot_lba   = 0u;
    uint8_t  slot_idx   = 0u;
    uint8_t  root_nfc   = 0u;
    if (dir_alloc_slots(vol, vol->root_dir_cluster, &root_nfc, 0u, 0, total_ents,
                        &slot_lba, &slot_idx) != 0) {
        (void)fat_set_entry(vol, dir_cls, 0u);
        (void)free_cluster_bit(vol, dir_cls);
        return -1;
    }

    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
        (void)fat_set_entry(vol, dir_cls, 0u);
        (void)free_cluster_bit(vol, dir_cls);
        return -1;
    }
    build_entry_set(&sec[(uint32_t)slot_idx * 32u], total_ents, name, name_len,
                    EXFAT_ATTR_DIR, dir_cls, vol->cluster_size, vol->default_timestamp);
    if (dev_write(vol, slot_lba, 1u, sec) != 0) {
        (void)fat_set_entry(vol, dir_cls, 0u);
        (void)free_cluster_bit(vol, dir_cls);
        return -1;
    }
    return 0;
}

int exfat_create_file(exfat_vol_t *vol, const char *dir_path, const char *filename, exfat_writer_t *out)
{
    if (!vol || !vol->dev->write_sectors || !dir_path || !filename || !out) {
        return -1;
    }
    mem_zero(out, sizeof(*out));

    uint32_t name_len = ascii_len_checked(filename, 30u);
    if (name_len == 0u) {
        return -1;
    }

    exfat_entry_t      dir_ent;
    exfat_parent_loc_t parent_loc;
    if (find_path_with_loc(vol, dir_path, &dir_ent, &parent_loc) != 0 || !dir_ent.is_dir) {
        return -1;
    }

    /* Fail cleanly if file already exists in target directory */
    find_ctx_t dup;
    dup.target     = filename;
    dup.target_len = name_len;
    dup.found      = 0;
    if (list_dir_with_loc(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                          0, find_entry_loc_cb, &dup) != 0 || dup.found) {
        return -1;
    }

    uint8_t total_ents = (name_len <= 15u) ? 3u : 4u;
    uint32_t slot_lba  = 0u;
    uint8_t  slot_idx  = 0u;
    if (dir_alloc_slots(vol, dir_ent.first_cluster, &dir_ent.no_fat_chain, dir_ent.size,
                        &parent_loc, total_ents, &slot_lba, &slot_idx) != 0) {
        return -1;
    }

    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
        return -1;
    }
    build_entry_set(&sec[(uint32_t)slot_idx * 32u], total_ents, filename, name_len,
                    EXFAT_ATTR_ARCHIVE, 0u, 0u, vol->default_timestamp);
    if (dev_write(vol, slot_lba, 1u, sec) != 0) {
        return -1;
    }

    out->vol        = vol;
    out->dir_lba    = slot_lba;
    out->dir_slot   = slot_idx;
    out->total_ents = total_ents;
    out->active     = 1u;
    return 0;
}

static int writer_write_sectors(exfat_writer_t *w, const uint8_t *sec_data, uint32_t count)
{
    uint32_t prev_cluster = w->cur_cluster;
    uint32_t prev_sec     = w->sec_in_cluster;
    int      allocated    = 0;
    uint32_t new_c        = 0u;

    if (w->cur_cluster == 0u || w->sec_in_cluster >= w->vol->sectors_per_cluster) {
        if (alloc_cluster(w->vol, w->cur_cluster, &new_c) != 0) {
            return -1;
        }
        allocated = 1;
        if (w->first_cluster == 0u) {
            w->first_cluster = new_c;
        }
        w->cur_cluster    = new_c;
        w->sec_in_cluster = 0u;
    }

    uint32_t lba = cluster_to_lba(w->vol, w->cur_cluster) + w->sec_in_cluster;
    if (dev_write(w->vol, lba, count, sec_data) != 0) {
        if (allocated) {
            /* Unlink and free freshly allocated unused cluster so FAT chain stays fsck-clean */
            if (prev_cluster >= 2u) {
                (void)fat_set_entry(w->vol, prev_cluster, 0xFFFFFFFFu);
            } else {
                w->first_cluster = 0u;
            }
            (void)fat_set_entry(w->vol, new_c, 0u);
            (void)free_cluster_bit(w->vol, new_c);
            w->cur_cluster    = prev_cluster;
            w->sec_in_cluster = prev_sec;
        }
        return -1;
    }
    w->sec_in_cluster += count;
    return 0;
}

int exfat_write(exfat_writer_t *w, const void *data, uint32_t len)
{
    if (!w || !w->active || (!data && len > 0u)) {
        return -1;
    }
    w->vol->wr_final = 0u;
    if (len == 0u) {
        return 0;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint32_t rem = len;

    while (rem > 0u) {
        if (w->sec_buf_used == 0u && rem >= 512u) {
            uint32_t nsec      = rem >> 9;
            uint32_t cls_avail = (w->cur_cluster == 0u || w->sec_in_cluster >= w->vol->sectors_per_cluster)
                               ? w->vol->sectors_per_cluster
                               : (w->vol->sectors_per_cluster - w->sec_in_cluster);
            if (nsec > cls_avail) {
                nsec = cls_avail;
            }
            if (writer_write_sectors(w, src, nsec) != 0) {
                return w->vol->wr_final ? BLKDEV_ERR_FINAL : -1;
            }
            uint32_t bytes = nsec << 9;
            src              += bytes;
            rem              -= bytes;
            w->bytes_written += bytes;
            continue;
        }

        uint32_t prev_used = w->sec_buf_used;
        uint32_t space     = 512u - prev_used;
        uint32_t take      = (rem < space) ? rem : space;
        mem_copy(w->sec_buf + prev_used, src, take);

        if (prev_used + take == 512u) {
            if (writer_write_sectors(w, w->sec_buf, 1u) != 0) {
                /* Leave w->sec_buf_used at prev_used (< 512) and do not advance bytes_written */
                return w->vol->wr_final ? BLKDEV_ERR_FINAL : -1;
            }
            w->sec_buf_used   = 0u;
            w->bytes_written += take;
            src              += take;
            rem              -= take;
        } else {
            w->sec_buf_used   = prev_used + take;
            w->bytes_written += take;
            src              += take;
            rem              -= take;
        }
    }
    return 0;
}

int exfat_patch_header(exfat_writer_t *w, const void *hdr, uint32_t len)
{
    if (!w || !w->vol || !hdr || len > 512u || (uint64_t)len > w->bytes_written) {
        return -1;
    }
    if (!w->active && w->first_cluster < 2u) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }

    if (w->first_cluster == 0u) {
        mem_copy(w->sec_buf, hdr, len);
        return 0;
    }

    uint8_t sec[512];
    uint32_t lba = cluster_to_lba(w->vol, w->first_cluster);
    if (w->vol->dev->read_sectors(w->vol->dev->ctx, lba, 1u, sec) != 0) {
        return -1;
    }
    mem_copy(sec, hdr, len);
    return dev_write(w->vol, lba, 1u, sec);
}

int exfat_checkpoint_file(exfat_writer_t *w, const void *hdr, uint32_t hdr_len)
{
    if (!w || !w->active) {
        return -1;
    }
    uint64_t flushed = (w->bytes_written >= (uint64_t)w->sec_buf_used)
                     ? (w->bytes_written - (uint64_t)w->sec_buf_used)
                     : 0u;
    if (w->first_cluster < 2u || flushed == 0u) {
        return 0; /* Nothing flushed to cluster heap yet */
    }
    if (hdr && hdr_len > 0u) {
        if (hdr_len > 512u || (uint64_t)hdr_len > flushed) {
            return -1;
        }
    }

    /*
     * Write ordering guarantee:
     * 1. Publish directory entry (first_cluster + flushed length) FIRST.
     *    All clusters covering `flushed` are already on disk and FAT-linked.
     * 2. Patch sector 0 header (e.g. WAV RIFF/data length) SECOND.
     * If power fails between steps 1 and 2, the file on disk has all `flushed`
     * bytes valid while the WAV header still reflects the previous (smaller)
     * checkpoint, so the header NEVER over-claims past EOF.
     */
    uint8_t sec[512];
    if (w->vol->dev->read_sectors(w->vol->dev->ctx, w->dir_lba, 1u, sec) != 0) {
        return -1;
    }
    uint8_t *f = &sec[(uint32_t)w->dir_slot * 32u];
    uint8_t *s = &sec[((uint32_t)w->dir_slot + 1u) * 32u];
    if (f[0] != 0x85u || s[0] != 0xC0u) {
        return -1;
    }
    if (w->vol->default_timestamp != 0u) {
        put_le32(f + 12u, w->vol->default_timestamp);
        put_le32(f + 16u, w->vol->default_timestamp);
    }
    s[1] = 0x01u; /* AllocationPossible=1, NoFatChain=0 */
    put_le64(s + 8u, flushed);
    put_le32(s + 20u, w->first_cluster);
    put_le64(s + 24u, flushed);
    put_le16(f + 2u, exfat_entry_set_checksum(f, w->total_ents));
    if (dev_write(w->vol, w->dir_lba, 1u, sec) != 0) {
        return -1;
    }

    if (hdr && hdr_len > 0u) {
        if (exfat_patch_header(w, hdr, hdr_len) != 0) {
            return -1;
        }
    }
    return 0;
}

int exfat_overwrite_sector(exfat_vol_t *vol, const exfat_entry_t *file, uint64_t offset,
                           const void *sec512)
{
    if (!vol || !vol->dev || !vol->dev->write_sectors || !file || !sec512 || file->is_dir ||
        (offset & 511u) != 0u) {
        return -1;
    }
    uint64_t vdl = (file->valid_data_len < file->size) ? file->valid_data_len : file->size;
    if (offset + 512u > vdl) {
        return -1;
    }
    uint32_t cls_shift = 9u + (uint32_t)vol->spc_shift;
    uint64_t skip      = offset >> cls_shift;
    uint32_t cls       = file->first_cluster;
    if (cls < 2u || (cls - 2u) >= vol->cluster_count || skip >= (uint64_t)vol->cluster_count) {
        return -1;
    }
    for (uint64_t i = 0u; i < skip; i++) {
        uint32_t nxt = 0u;
        if (next_cluster(vol, cls, file->no_fat_chain, &nxt) != 0 || nxt < 2u ||
            (nxt - 2u) >= vol->cluster_count) {
            return -1;
        }
        cls = nxt;
    }
    uint32_t sec_in_cls = (uint32_t)((offset & ((uint64_t)vol->cluster_size - 1u)) >> 9);
    return dev_write(vol, cluster_to_lba(vol, cls) + sec_in_cls, 1u, sec512);
}

static int free_file_clusters(exfat_vol_t *vol, uint32_t first_cluster, uint64_t size, uint8_t no_fat_chain);

int exfat_close_file(exfat_writer_t *w)
{
    if (!w || !w->active) {
        return -1;
    }

    int tail_err = 0;
    if (w->sec_buf_used > 0u) {
        mem_zero(w->sec_buf + w->sec_buf_used, 512u - w->sec_buf_used);
        if (writer_write_sectors(w, w->sec_buf, 1u) != 0) {
            /* Card full or write error: drop unflushed tail but still commit flushed sectors */
            if (w->bytes_written >= (uint64_t)w->sec_buf_used) {
                w->bytes_written -= (uint64_t)w->sec_buf_used;
            } else {
                w->bytes_written = 0u;
            }
            tail_err = -1;
        }
        w->sec_buf_used = 0u;
    }

    /* Never commit FirstCluster != 0 with DataLength == 0 (violates exFAT spec / fsck.exfat) */
    if (w->bytes_written == 0u && w->first_cluster >= 2u) {
        (void)free_file_clusters(w->vol, w->first_cluster, (uint64_t)w->vol->cluster_size, 0u);
        w->first_cluster  = 0u;
        w->cur_cluster    = 0u;
        w->sec_in_cluster = 0u;
    }
    uint32_t commit_cls = (w->bytes_written > 0u) ? w->first_cluster : 0u;

    uint8_t sec[512];
    if (w->vol->dev->read_sectors(w->vol->dev->ctx, w->dir_lba, 1u, sec) != 0) {
        return -1;
    }

    uint8_t *f = &sec[(uint32_t)w->dir_slot * 32u];
    uint8_t *s = &sec[((uint32_t)w->dir_slot + 1u) * 32u];
    if (f[0] != 0x85u || s[0] != 0xC0u) {
        return -1;
    }

    if (w->vol->default_timestamp != 0u) {
        put_le32(f + 12u, w->vol->default_timestamp);
        put_le32(f + 16u, w->vol->default_timestamp);
    }
    s[1] = 0x01u; /* AllocationPossible=1, NoFatChain=0 */
    put_le64(s + 8u, w->bytes_written);
    put_le32(s + 20u, commit_cls);
    put_le64(s + 24u, w->bytes_written);
    put_le16(f + 2u, exfat_entry_set_checksum(f, w->total_ents));

    if (dev_write(w->vol, w->dir_lba, 1u, sec) != 0) {
        return -1;
    }

    w->active = 0u;
    return tail_err;
}

int exfat_stat_fs(exfat_vol_t *vol, uint32_t *free_mb, uint32_t *total_mb)
{
    if (!vol || !vol->dev || !vol->dev->read_sectors) {
        return -1;
    }
    uint32_t spc = vol->sectors_per_cluster;
    if (total_mb) {
        uint32_t tc = vol->cluster_count;
        *total_mb = ((tc >> 11) * spc) + (((tc & 0x7FFu) * spc) >> 11);
    }
    if (!free_mb) {
        return 0;
    }
    uint32_t free_cls = 0u;
    if (exfat_free_clusters(vol, &free_cls) != 0) {
        *free_mb = 0u;
        return -1;
    }
    *free_mb = ((free_cls >> 11) * spc) + (((free_cls & 0x7FFu) * spc) >> 11);
    return 0;
}

int exfat_free_clusters(exfat_vol_t *vol, uint32_t *out_free)
{
    if (!vol || !out_free) {
        return -1;
    }
    if (!vol->free_valid) {
        uint32_t n = 0u;
        if (exfat_count_free_scan(vol, &n) != 0) {
            return -1;
        }
        vol->free_clusters = n;
        vol->free_valid    = 1u;
    }
    *out_free = vol->free_clusters;
    return 0;
}

int exfat_count_free_scan(const exfat_vol_t *vol, uint32_t *out_free)
{
    if (!vol || !out_free || !vol->dev || !vol->dev->read_sectors) {
        return -1;
    }
    uint32_t spc = vol->sectors_per_cluster;
    if (vol->bitmap_cluster < 2u || vol->bitmap_bytes == 0u) {
        return -1;
    }

    uint32_t need_bytes = (vol->cluster_count + 7u) >> 3;
    uint32_t limit      = (vol->bitmap_bytes < need_bytes) ? vol->bitmap_bytes : need_bytes;
    uint32_t bmp_cls    = vol->bitmap_cluster;
    uint32_t cur_byte   = 0u;
    uint32_t free_cls   = 0u;
    uint8_t  buf[4u * 512u];

    while (bmp_cls >= 2u && cur_byte < limit) {
        uint32_t base_lba = cluster_to_lba(vol, bmp_cls);
        uint32_t s = 0u;
        while (s < spc && cur_byte < limit) {
            uint32_t rem_bytes = limit - cur_byte;
            uint32_t need_secs = (rem_bytes + 511u) >> 9;
            uint32_t chunk     = spc - s;
            if (chunk > need_secs) {
                chunk = need_secs;
            }
            if (chunk > 4u) {
                chunk = 4u;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s, chunk, buf) != 0) {
                return -1;
            }
            uint32_t chunk_bytes = chunk << 9;
            uint32_t b_end = (rem_bytes < chunk_bytes) ? rem_bytes : chunk_bytes;
            for (uint32_t b = 0u; b < b_end; b++) {
                uint32_t base_idx = (cur_byte + b) << 3;
                uint32_t rem_bits = vol->cluster_count - base_idx;
                uint32_t nbits    = (rem_bits < 8u) ? rem_bits : 8u;
                uint8_t  v        = buf[b];
                if (nbits == 8u && v == 0x00u) {
                    free_cls += 8u;
                } else if (nbits == 8u && v == 0xFFu) {
                    continue;
                } else {
                    for (uint32_t bit = 0u; bit < nbits; bit++) {
                        if ((v & (1u << bit)) == 0u) {
                            free_cls++;
                        }
                    }
                }
            }
            cur_byte += b_end;
            s        += chunk;
        }
        if (cur_byte < limit) {
            uint32_t nxt = 0u;
            if (next_bitmap_cluster(vol, bmp_cls, &nxt) != 0) {
                return -1;
            }
            bmp_cls = nxt;
        }
    }

    *out_free = free_cls;
    return 0;
}

typedef struct {
    uint32_t target_cluster;
    int      found;
} cluster_ref_ctx_t;

static int cluster_ref_cb(const exfat_entry_t *ent, void *user)
{
    cluster_ref_ctx_t *ctx = (cluster_ref_ctx_t *)user;
    if (!ent->is_dir && ent->first_cluster == ctx->target_cluster && ent->size > 0u) {
        ctx->found = 1;
        return 1; /* Stop walking */
    }
    return 0;
}

int exfat_delete_file(exfat_vol_t *vol, const char *dir_path, const char *filename)
{
    if (!vol || !vol->dev || !vol->dev->read_sectors || !vol->dev->write_sectors ||
        !dir_path || !filename) {
        return -1;
    }
    uint32_t name_len = ascii_len_checked(filename, EXFAT_MAX_NAME - 1u);
    if (name_len == 0u) {
        return -1;
    }

    exfat_entry_t dir_ent;
    if (find_path_with_loc(vol, dir_path, &dir_ent, 0) != 0 || !dir_ent.is_dir) {
        return -1;
    }

    find_ctx_t fc;
    fc.target     = filename;
    fc.target_len = name_len;
    fc.found      = 0;
    if (list_dir_with_loc(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                          0, find_entry_loc_cb, &fc) != 0 ||
        !fc.found || fc.result.is_dir) {
        return -1;
    }

    /* Invalidate read cluster cache for this volume */
    mem_zero(vol->rd_cache, sizeof(vol->rd_cache));

    /* Clear bit 7 across all directory entries in the set across sector/cluster boundaries */
    uint8_t  sec[512];
    uint32_t rem = 1u + (uint32_t)fc.loc.sec_cnt;
    uint32_t c   = fc.loc.cls;
    uint32_t s   = fc.loc.s;
    uint32_t sl  = fc.loc.slot;
    while (rem > 0u) {
        if (c < 2u || (c - 2u) >= vol->cluster_count) {
            return -1;
        }
        uint32_t lba = cluster_to_lba(vol, c) + s;
        if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
            return -1;
        }
        while (sl < 16u && rem > 0u) {
            sec[sl * 32u] &= 0x7Fu;
            sl++;
            rem--;
        }
        if (dev_write(vol, lba, 1u, sec) != 0) {
            return -1;
        }
        sl = 0u;
        s++;
        if (s >= vol->sectors_per_cluster && rem > 0u) {
            s = 0u;
            uint32_t nxt = 0u;
            if (next_cluster(vol, c, dir_ent.no_fat_chain, &nxt) != 0 || nxt == 0u) {
                return -1;
            }
            c = nxt;
        }
    }

    /* Return data clusters ONLY if no other entry in this directory still references them
     * (protects against power loss during exfat_replace_file leaving a shared cluster). */
    if (fc.result.first_cluster >= 2u && fc.result.size > 0u) {
        cluster_ref_ctx_t ref = { fc.result.first_cluster, 0 };
        if (list_dir_with_loc(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                              cluster_ref_cb, 0, &ref) != 0 && !ref.found) {
            /* The scan did not finish (I/O error): another entry may still own these clusters. Leak them (the entry
             * is already gone; a card check reclaims them) rather than free them under a live file (STEP10 C1). */
            return -1;
        }
        if (!ref.found) {
            if (free_file_clusters(vol, fc.result.first_cluster, fc.result.size, fc.result.no_fat_chain) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

typedef struct {
    uint8_t  buf[512];
    uint32_t lba;
    uint8_t  valid;
    uint8_t  dirty;
} sec_wb_cache_t;

static int sec_wb_flush(const exfat_vol_t *vol, sec_wb_cache_t *c)
{
    if (c->dirty) {
        if (dev_write_c(vol, c->lba, 1u, c->buf) != 0) {
            return -1;
        }
        c->dirty = 0u;
    }
    return 0;
}

static int sec_wb_load(const exfat_vol_t *vol, sec_wb_cache_t *c, uint32_t lba)
{
    if (c->valid && c->lba == lba) {
        return 0;
    }
    if (sec_wb_flush(vol, c) != 0) {
        return -1;
    }
    if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, c->buf) != 0) {
        c->valid = 0u;
        return -1;
    }
    c->lba   = lba;
    c->valid = 1u;
    return 0;
}

/* After a free_file_clusters() pass: the cleared bits are free now, unless a bitmap write failed (then some of them
 * may still be set on the card: recount). */
static void free_file_clusters_account(exfat_vol_t *vol, int rc, uint32_t freed)
{
    if (rc != 0) {
        vol->free_valid = 0u;
    } else {
        free_cache_add(vol, (int32_t)freed);
    }
}

static int free_file_clusters(exfat_vol_t *vol, uint32_t first_cluster, uint64_t size, uint8_t no_fat_chain)
{
    if (first_cluster < 2u || size == 0u) {
        return 0;
    }
    if (!vol || !vol->dev->write_sectors || vol->bitmap_cluster < 2u) {
        return -1;
    }

    /* 1-sector writeback cache for FAT & Allocation Bitmap (avoids 5 I/Os per cluster) */
    sec_wb_cache_t fat_c;
    sec_wb_cache_t bmp_c;
    mem_zero(&fat_c, sizeof(fat_c));
    mem_zero(&bmp_c, sizeof(bmp_c));

    uint32_t cls_shift       = 9u + (uint32_t)vol->spc_shift;
    uint32_t cls_mask        = vol->cluster_size - 1u;
    uint32_t cached_bmp_cidx = 0u;
    uint32_t cached_bmp_cls  = vol->bitmap_cluster;
    int      rc              = 0;
    uint32_t freed           = 0u;   /* bitmap bits this call cleared (F4 free-cluster cache) */

    if (no_fat_chain) {
        uint64_t n_cls64 = (size + ((uint64_t)vol->cluster_size - 1u)) >> cls_shift;
        uint32_t n_cls   = (n_cls64 > vol->cluster_count) ? vol->cluster_count : (uint32_t)n_cls64;
        for (uint32_t i = 0u; i < n_cls; i++) {
            uint32_t cluster = first_cluster + i;
            if (cluster < 2u || (cluster - 2u) >= vol->cluster_count) {
                rc = -1;
                break;
            }
            uint32_t idx      = cluster - 2u;
            uint32_t byte_off = idx >> 3;
            uint32_t bit      = idx & 7u;
            if (byte_off >= vol->bitmap_bytes) {
                rc = -1;
                break;
            }
            uint32_t target_cidx = byte_off >> cls_shift;
            if (target_cidx < cached_bmp_cidx) {
                cached_bmp_cidx = 0u;
                cached_bmp_cls  = vol->bitmap_cluster;
            }
            while (cached_bmp_cidx < target_cidx) {
                uint32_t nb = 0u;
                if (next_bitmap_cluster(vol, cached_bmp_cls, &nb) != 0 || nb == 0u) {
                    rc = -1;
                    break;
                }
                cached_bmp_cls = nb;
                cached_bmp_cidx++;
            }
            if (rc != 0) {
                break;
            }
            uint32_t blba = cluster_to_lba(vol, cached_bmp_cls) + ((byte_off & cls_mask) >> 9);
            if (sec_wb_load(vol, &bmp_c, blba) != 0) {
                rc = -1;
                break;
            }
            if (bmp_c.buf[byte_off & 511u] & (1u << bit)) {
                freed++;
            }
            bmp_c.buf[byte_off & 511u] &= (uint8_t)~(1u << bit);
            bmp_c.dirty = 1u;
            if (idx < vol->alloc_hint) {
                vol->alloc_hint = idx;
            }
        }
        if (sec_wb_flush(vol, &bmp_c) != 0) {
            rc = -1;
        }
        free_file_clusters_account(vol, rc, freed);
        return rc;
    }

    uint32_t cur   = first_cluster;
    uint32_t steps = 0u;
    while (cur >= 2u && (cur - 2u) < vol->cluster_count && steps < vol->cluster_count) {
        uint32_t fat_sec_off = cur >> 7;
        uint32_t fat_in_sec  = (cur & 127u) << 2;
        if (fat_sec_off >= vol->fat_sectors) {
            rc = -1;
            break;
        }
        if (sec_wb_load(vol, &fat_c, vol->fat_lba + fat_sec_off) != 0) {
            rc = -1;
            break;
        }
        uint32_t nxt = le32(fat_c.buf + fat_in_sec);

        /* Clear Allocation Bitmap bit (cached) */
        uint32_t idx      = cur - 2u;
        uint32_t byte_off = idx >> 3;
        uint32_t bit      = idx & 7u;
        if (byte_off >= vol->bitmap_bytes) {
            rc = -1;
            break;
        }
        uint32_t target_cidx = byte_off >> cls_shift;
        if (target_cidx < cached_bmp_cidx) {
            cached_bmp_cidx = 0u;
            cached_bmp_cls  = vol->bitmap_cluster;
        }
        while (cached_bmp_cidx < target_cidx) {
            uint32_t nb = 0u;
            if (next_bitmap_cluster(vol, cached_bmp_cls, &nb) != 0 || nb == 0u) {
                rc = -1;
                break;
            }
            cached_bmp_cls = nb;
            cached_bmp_cidx++;
        }
        if (rc != 0) {
            break;
        }
        uint32_t blba = cluster_to_lba(vol, cached_bmp_cls) + ((byte_off & cls_mask) >> 9);
        if (sec_wb_load(vol, &bmp_c, blba) != 0) {
            rc = -1;
            break;
        }
        if (bmp_c.buf[byte_off & 511u] & (1u << bit)) {
            freed++;
        }
        bmp_c.buf[byte_off & 511u] &= (uint8_t)~(1u << bit);
        bmp_c.dirty = 1u;
        if (idx < vol->alloc_hint) {
            vol->alloc_hint = idx;
        }

        /* Clear FAT entry (cached) */
        put_le32(fat_c.buf + fat_in_sec, 0u);
        fat_c.dirty = 1u;

        steps++;
        if (nxt >= 0xFFFFFFF8u || nxt < 2u || (nxt - 2u) >= vol->cluster_count) {
            break;
        }
        cur = nxt;
    }

    if (sec_wb_flush(vol, &bmp_c) != 0) {
        rc = -1;
    }
    free_file_clusters_account(vol, rc, freed);
    if (sec_wb_flush(vol, &fat_c) != 0) {
        rc = -1;
    }
    vol->fat_cache_valid = 0u;
    return rc;
}

int exfat_replace_file(exfat_vol_t *vol, const char *dir_path, const char *src_name, const char *dst_name)
{
    if (!vol || !vol->dev || !vol->dev->read_sectors || !vol->dev->write_sectors ||
        !dir_path || !src_name || !dst_name) {
        return -1;
    }
    uint32_t src_len = ascii_len_checked(src_name, 30u);
    uint32_t dst_len = ascii_len_checked(dst_name, 30u);
    if (src_len == 0u || dst_len == 0u) {
        return -1;
    }

    exfat_entry_t      dir_ent;
    exfat_parent_loc_t parent_loc;
    if (find_path_with_loc(vol, dir_path, &dir_ent, &parent_loc) != 0 || !dir_ent.is_dir) {
        return -1;
    }

    find_ctx_t sc;
    sc.target     = src_name;
    sc.target_len = src_len;
    sc.found      = 0;
    if (list_dir_with_loc(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                          0, find_entry_loc_cb, &sc) != 0 ||
        !sc.found || sc.result.is_dir) {
        return -1;
    }

    if (str_eq_ci_n(src_name, dst_name, dst_len)) {
        return 0;
    }

    /* Check if dst_name already exists in the directory */
    find_ctx_t dc;
    dc.target     = dst_name;
    dc.target_len = dst_len;
    dc.found      = 0;
    if (list_dir_with_loc(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                          0, find_entry_loc_cb, &dc) != 0) {
        return -1;
    }
    if (dc.found && dc.result.is_dir) {
        return -1;
    }

    /* Invalidate read cluster cache before mutating directory / clusters */
    mem_zero(vol->rd_cache, sizeof(vol->rd_cache));

    uint8_t  sec[512];
    int      have_old_dst = 0;
    uint32_t old_dst_cls  = 0u;
    uint64_t old_dst_size = 0u;
    uint8_t  old_dst_nfc  = 0u;

    /*
     * Publish dst_name BEFORE touching src_name's directory slots or freeing
     * old destination clusters, so dst_name never disappears on interruption.
     * When dst_name already exists within a single sector, swap its Stream
     * Extension in-place in one atomic sector write.
     */
    if (dc.found && ((uint32_t)dc.loc.slot + 1u + (uint32_t)dc.loc.sec_cnt) <= 16u) {
        have_old_dst = 1;
        old_dst_cls  = dc.result.first_cluster;
        old_dst_size = dc.result.size;
        old_dst_nfc  = dc.result.no_fat_chain;

        if (vol->dev->read_sectors(vol->dev->ctx, dc.loc.lba, 1u, sec) != 0) {
            return -1;
        }
        uint8_t *f = &sec[(uint32_t)dc.loc.slot * 32u];
        uint8_t *s = &sec[((uint32_t)dc.loc.slot + 1u) * 32u];
        if (f[0] != 0x85u || s[0] != 0xC0u) {
            return -1;
        }
        if (vol->default_timestamp != 0u) {
            put_le32(f + 12u, vol->default_timestamp);
            put_le32(f + 16u, vol->default_timestamp);
        }
        s[1] = sc.result.no_fat_chain ? 0x03u : 0x01u;
        put_le64(s + 8u, sc.result.valid_data_len);
        put_le32(s + 20u, sc.result.first_cluster);
        put_le64(s + 24u, sc.result.size);
        put_le16(f + 2u, exfat_entry_set_checksum(f, 1u + (uint32_t)dc.loc.sec_cnt));
        if (dev_write(vol, dc.loc.lba, 1u, sec) != 0) {
            return -1;
        }
    } else {
        if (dc.found) {
            have_old_dst = 1;
            old_dst_cls  = dc.result.first_cluster;
            old_dst_size = dc.result.size;
            old_dst_nfc  = dc.result.no_fat_chain;
        }
        uint8_t total_ents = (dst_len <= 15u) ? 3u : 4u;
        uint32_t slot_lba  = 0u;
        uint8_t  slot_idx  = 0u;
        if (dir_alloc_slots(vol, dir_ent.first_cluster, &dir_ent.no_fat_chain, dir_ent.size,
                            &parent_loc, total_ents, &slot_lba, &slot_idx) != 0) {
            return -1;
        }
        if (vol->dev->read_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
            return -1;
        }
        uint8_t *ent = &sec[(uint32_t)slot_idx * 32u];
        build_entry_set(ent, total_ents, dst_name, dst_len,
                        EXFAT_ATTR_ARCHIVE, sc.result.first_cluster, sc.result.size,
                        vol->default_timestamp);
        if (sc.result.no_fat_chain || sc.result.valid_data_len < sc.result.size) {
            if (sc.result.no_fat_chain) {
                ent[32u + 1u] = 0x03u;
            }
            put_le64(ent + 32u + 8u, sc.result.valid_data_len);
            put_le16(ent + 2u, exfat_entry_set_checksum(ent, total_ents));
        }
        if (dev_write(vol, slot_lba, 1u, sec) != 0) {
            return -1;
        }
        /* If dst existed spanning sectors, unlink its old dir slots now */
        if (dc.found) {
            uint32_t drem = 1u + (uint32_t)dc.loc.sec_cnt;
            uint32_t dcl  = dc.loc.cls;
            uint32_t ds   = dc.loc.s;
            uint32_t dsl  = dc.loc.slot;
            while (drem > 0u) {
                if (dcl < 2u || (dcl - 2u) >= vol->cluster_count) {
                    return -1;
                }
                uint32_t dlba = cluster_to_lba(vol, dcl) + ds;
                if (vol->dev->read_sectors(vol->dev->ctx, dlba, 1u, sec) != 0) {
                    return -1;
                }
                while (dsl < 16u && drem > 0u) {
                    sec[dsl * 32u] &= 0x7Fu;
                    dsl++;
                    drem--;
                }
                if (dev_write(vol, dlba, 1u, sec) != 0) {
                    return -1;
                }
                dsl = 0u;
                ds++;
                if (ds >= vol->sectors_per_cluster && drem > 0u) {
                    ds = 0u;
                    uint32_t nxt = 0u;
                    if (next_cluster(vol, dcl, dir_ent.no_fat_chain, &nxt) != 0 || nxt == 0u) {
                        return -1;
                    }
                    dcl = nxt;
                }
            }
        }
    }

    /* Mark old src_name directory slots deleted WITHOUT freeing its data clusters */
    uint32_t rem = 1u + (uint32_t)sc.loc.sec_cnt;
    uint32_t c   = sc.loc.cls;
    uint32_t s   = sc.loc.s;
    uint32_t sl  = sc.loc.slot;
    while (rem > 0u) {
        if (c < 2u || (c - 2u) >= vol->cluster_count) {
            return -1;
        }
        uint32_t lba = cluster_to_lba(vol, c) + s;
        if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
            return -1;
        }
        while (sl < 16u && rem > 0u) {
            sec[sl * 32u] &= 0x7Fu;
            sl++;
            rem--;
        }
        if (dev_write(vol, lba, 1u, sec) != 0) {
            return -1;
        }
        sl = 0u;
        s++;
        if (s >= vol->sectors_per_cluster && rem > 0u) {
            s = 0u;
            uint32_t nxt = 0u;
            if (next_cluster(vol, c, dir_ent.no_fat_chain, &nxt) != 0 || nxt == 0u) {
                return -1;
            }
            c = nxt;
        }
    }

    /* Free previous destination's old clusters LAST */
    if (have_old_dst && old_dst_cls != sc.result.first_cluster) {
        if (free_file_clusters(vol, old_dst_cls, old_dst_size, old_dst_nfc) != 0) {
            return -1;
        }
    }

    mem_zero(vol->rd_cache, sizeof(vol->rd_cache));
    return 0;
}
