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
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
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
        if ((uint8_t)s[n] >= 0x80u || n >= max_len) {
            return 0u;
        }
        n++;
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

    uint32_t fat_sec_off = (cur * 4u) >> 9;
    uint32_t fat_in_sec  = (cur * 4u) & 511u;
    if (fat_sec_off >= vol->fat_sectors) {
        return -1;
    }

    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, vol->fat_lba + fat_sec_off, 1u, sec) != 0) {
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
    uint32_t nxt = 0u;
    if (next_cluster(vol, cur, 0u, &nxt) != 0) {
        return -1;
    }
    if (nxt >= 2u && (nxt - 2u) < vol->cluster_count) {
        *out_next = nxt;
        return 0;
    }
    uint32_t contig = cur + 1u;
    if ((contig - 2u) < vol->cluster_count) {
        *out_next = contig;
        return 0;
    }
    *out_next = 0u;
    return 0;
}

static int scan_root_bitmap(exfat_vol_t *vol)
{
    uint32_t cls   = vol->root_dir_cluster;
    uint32_t steps = 0u;
    uint8_t sec[512];

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        if (++steps > vol->cluster_count) {
            return -1; /* Corrupted FAT cycle */
        }
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s = 0u; s < vol->sectors_per_cluster; s++) {
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s, 1u, sec) != 0) {
                return -1;
            }
            for (uint32_t idx = 0u; idx < 16u; idx++) {
                const uint8_t *d = &sec[idx * 32u];
                if (d[0] == 0x00u) {
                    return 0;
                }
                if (d[0] == 0x81u && (d[1] & 1u) == 0u) {
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

    uint8_t sec[512];
    if (dev->read_sectors(dev->ctx, 0u, 1u, sec) != 0) {
        return -1;
    }

    uint32_t part_lba = 0u;
    if (!is_exfat_vbr(sec)) {
        if (sec[510] != 0x55u || sec[511] != 0xAAu) {
            return -1;
        }
        int found = 0;
        for (uint32_t p = 0u; p < 4u; p++) {
            const uint8_t *pe = &sec[0x1BEu + p * 16u];
            if (pe[4] == 0x07u) {
                part_lba = le32(pe + 8u);
                found = 1;
                break;
            }
        }
        if (!found || dev->read_sectors(dev->ctx, part_lba, 1u, sec) != 0 || !is_exfat_vbr(sec)) {
            return -1;
        }
    }

    uint8_t bps_shift = sec[0x6Cu];
    uint8_t spc_shift = sec[0x6Du];
    if (bps_shift != 9u || spc_shift > 16u) {
        return -1;
    }

    uint32_t fat_off  = le32(sec + 0x50u);
    uint32_t fat_len  = le32(sec + 0x54u);
    uint32_t heap_off = le32(sec + 0x58u);
    uint32_t clst_cnt = le32(sec + 0x5Cu);
    uint32_t root_cls = le32(sec + 0x60u);
    if (clst_cnt == 0u || root_cls < 2u || (root_cls - 2u) >= clst_cnt) {
        return -1;
    }

    mem_zero(vol, sizeof(*vol));
    vol->dev                 = dev;
    vol->part_lba            = part_lba;
    vol->fat_lba             = part_lba + fat_off;
    vol->fat_sectors         = fat_len;
    vol->heap_lba            = part_lba + heap_off;
    vol->cluster_count       = clst_cnt;
    vol->root_dir_cluster    = root_cls;
    vol->spc_shift           = spc_shift;
    vol->sectors_per_cluster = 1u << spc_shift;
    vol->cluster_size        = 512u << spc_shift;
    vol->default_timestamp   = EXFAT_DOS_TIMESTAMP_DEFAULT;

    (void)scan_root_bitmap(vol);
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
    uint32_t lba;
    uint8_t  slot;
    uint8_t  sec_cnt;
} exfat_dir_loc_t;

typedef int (*exfat_dir_loc_cb_t)(const exfat_entry_t *entry, const exfat_dir_loc_t *loc, void *user);

static int list_dir_with_loc(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain,
                             uint64_t dir_size, exfat_dir_cb_t cb, exfat_dir_loc_cb_t loc_cb, void *user)
{
    if (!vol || (!cb && !loc_cb) || dir_cluster < 2u) {
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

    uint32_t cls     = dir_cluster;
    uint32_t cls_idx = 0u;
    uint8_t sec[512];

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        if (cls_idx >= vol->cluster_count) {
            return -1; /* Corrupted FAT cycle */
        }
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s = 0u; s < vol->sectors_per_cluster; s++) {
            uint32_t lba = base_lba + s;
            if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            for (uint32_t idx = 0u; idx < 16u; idx++) {
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
                    cur_ent.file_attr = le16(d + 4u);
                    cur_ent.is_dir    = (uint8_t)((cur_ent.file_attr & EXFAT_ATTR_DIR) != 0u);
                    cur_ent.name[0]   = '\0';
                    continue;
                }
                if (!in_set) {
                    continue;
                }

                sec_seen++;
                if (etype == 0xC0u) {
                    cur_ent.no_fat_chain  = (uint8_t)((d[1] >> 1) & 1u);
                    name_len              = d[3];
                    cur_ent.first_cluster = le32(d + 20u);
                    cur_ent.size          = le64(d + 24u);
                    have_stream           = 1u;
                } else if (etype == 0xC1u) {
                    for (uint32_t k = 0u; k < 15u && name_pos < name_len; k++) {
                        uint16_t ch = le16(d + 2u + k * 2u);
                        if (name_pos < (EXFAT_MAX_NAME - 1u)) {
                            cur_ent.name[name_pos] = (ch > 0u && ch < 0x80u) ? (char)ch : '?';
                        }
                        name_pos++;
                    }
                    uint32_t nul_pos = (name_pos < EXFAT_MAX_NAME) ? name_pos : (EXFAT_MAX_NAME - 1u);
                    cur_ent.name[nul_pos] = '\0';
                }

                if (sec_seen >= sec_expected) {
                    in_set = 0u;
                    if (have_stream && cur_ent.name[0] != '\0') {
                        int stop = loc_cb ? loc_cb(&cur_ent, &prim_loc, user)
                                          : cb(&cur_ent, user);
                        if (stop != 0) {
                            return 0;
                        }
                    }
                }
            }
        }
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
                              uint32_t *out_lba, uint8_t *out_slot)
{
    if (!vol || !path || !out) {
        return -1;
    }

    exfat_entry_t cur;
    cur.name[0]       = '/';
    cur.name[1]       = '\0';
    cur.size          = 0u;
    cur.first_cluster = vol->root_dir_cluster;
    cur.file_attr     = EXFAT_ATTR_DIR;
    cur.no_fat_chain  = 0u;
    cur.is_dir        = 1u;

    uint32_t cur_lba  = 0u;
    uint8_t  cur_slot = 0u;

    const char *p = path;
    while (*p == '/' || *p == '\\') {
        p++;
    }
    if (*p == '\0') {
        *out = cur;
        if (out_lba)  *out_lba  = 0u;
        if (out_slot) *out_slot = 0u;
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
        cur      = ctx.result;
        cur_lba  = ctx.loc.lba;
        cur_slot = ctx.loc.slot;

        p += seg_len;
        while (*p == '/' || *p == '\\') {
            p++;
        }
    }

    *out = cur;
    if (out_lba)  *out_lba  = cur_lba;
    if (out_slot) *out_slot = cur_slot;
    return 0;
}

int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *out)
{
    return find_path_with_loc(vol, path, out, 0, 0);
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
    uint32_t cls   = file->first_cluster;

    uint32_t cls_shift     = 9u + (uint32_t)vol->spc_shift;
    uint64_t skip_clusters = offset >> cls_shift;
    uint32_t in_cls_off    = (uint32_t)(offset & ((uint64_t)vol->cluster_size - 1u));
    uint32_t cur_idx       = 0u;
    exfat_vol_t *mvol      = (exfat_vol_t *)(uintptr_t)vol;
    int      cache_slot    = -1;

    if (file->no_fat_chain) {
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

    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0u;
    uint8_t sec[512];

    while (done < want) {
        if (cls < 2u || (cls - 2u) >= vol->cluster_count) {
            return -1;
        }
        uint32_t base_lba = cluster_to_lba(vol, cls);
        uint32_t s_idx    = in_cls_off >> 9;
        uint32_t s_off    = in_cls_off & 511u;

        while (s_idx < vol->sectors_per_cluster && done < want) {
            uint32_t take = 512u - s_off;
            if (take > (want - done)) {
                take = want - done;
            }
            if (s_off == 0u && take == 512u) {
                if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_idx, 1u, out + done) != 0) {
                    return -1;
                }
            } else {
                if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s_idx, 1u, sec) != 0) {
                    return -1;
                }
                mem_copy(out + done, sec + s_off, take);
            }
            done += take;
            s_idx++;
            s_off = 0u;
        }
        in_cls_off = 0u;
        if (done < want) {
            uint32_t nxt = 0u;
            if (next_cluster(vol, cls, file->no_fat_chain, &nxt) != 0 || nxt == 0u) {
                return -1;
            }
            cls = nxt;
            cur_idx++;
        }
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
    uint32_t fat_sec_off = (cluster * 4u) >> 9;
    uint32_t fat_in_sec  = (cluster * 4u) & 511u;
    if (fat_sec_off >= vol->fat_sectors) {
        return -1;
    }

    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, vol->fat_lba + fat_sec_off, 1u, sec) != 0) {
        return -1;
    }
    *out_val = le32(sec + fat_in_sec);
    return 0;
}

static int fat_set_entry(const exfat_vol_t *vol, uint32_t cluster, uint32_t value)
{
    if (cluster < 2u || (cluster - 2u) >= vol->cluster_count) {
        return -1;
    }
    uint32_t fat_sec_off = (cluster * 4u) >> 9;
    uint32_t fat_in_sec  = (cluster * 4u) & 511u;
    if (fat_sec_off >= vol->fat_sectors) {
        return -1;
    }

    uint8_t sec[512];
    uint32_t lba = vol->fat_lba + fat_sec_off;
    if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
        return -1;
    }
    put_le32(sec + fat_in_sec, value);
    return vol->dev->write_sectors(vol->dev->ctx, lba, 1u, sec);
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
    uint8_t  sec[512];

    while (bmp_cls >= 2u && cur_byte < end_byte) {
        uint32_t base_lba = cluster_to_lba(vol, bmp_cls);
        for (uint32_t s = s_start; s < vol->sectors_per_cluster && cur_byte < end_byte; s++) {
            uint32_t sec_base = cur_byte + (s << 9);
            if (sec_base >= end_byte) {
                return 1;
            }
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s, 1u, sec) != 0) {
                return -1;
            }
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
                    if (vol->dev->write_sectors(vol->dev->ctx, base_lba + s, 1u, sec) != 0) {
                        return -1;
                    }
                    if (fat_set_entry(vol, new_c, 0xFFFFFFFFu) != 0) {
                        return -1;
                    }
                    if (prev_cluster >= 2u && fat_set_entry(vol, prev_cluster, new_c) != 0) {
                        return -1;
                    }
                    vol->alloc_hint = idx + 1u;
                    *out_cluster    = new_c;
                    return 0;
                }
            }
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
    sec[in_sec] &= (uint8_t)~(1u << bit);
    if (vol->dev->write_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
        return -1;
    }

    if (idx < vol->alloc_hint) {
        vol->alloc_hint = idx;
    }
    return 0;
}

static int zero_cluster(const exfat_vol_t *vol, uint32_t cluster)
{
    uint8_t zero[512];
    mem_zero(zero, sizeof(zero));
    uint32_t base_lba = cluster_to_lba(vol, cluster);
    for (uint32_t s = 0u; s < vol->sectors_per_cluster; s++) {
        if (vol->dev->write_sectors(vol->dev->ctx, base_lba + s, 1u, zero) != 0) {
            return -1;
        }
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

static uint16_t exfat_entry_set_checksum(const uint8_t *entries, uint32_t total_ents)
{
    uint32_t bytes = total_ents * 32u;
    uint16_t sum = 0u;
    for (uint32_t i = 0u; i < bytes; i++) {
        if (i == 2u || i == 3u) {
            continue;
        }
        sum = (uint16_t)(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + entries[i]);
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

static int grow_dir_size_in_parent(const exfat_vol_t *vol, uint32_t parent_lba, uint8_t parent_slot)
{
    if (parent_lba == 0u || parent_slot >= 15u) {
        return 0; /* Root dir has no parent entry */
    }
    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, parent_lba, 1u, sec) != 0) {
        return -1;
    }
    uint8_t *f = &sec[(uint32_t)parent_slot * 32u];
    uint8_t *s = &sec[((uint32_t)parent_slot + 1u) * 32u];
    if (f[0] != 0x85u || s[0] != 0xC0u) {
        return -1;
    }
    uint32_t total_ents = (uint32_t)f[1] + 1u;
    if ((uint32_t)parent_slot + total_ents > 16u) {
        return -1;
    }
    uint64_t new_len = le64(s + 24u) + vol->cluster_size;
    put_le64(s + 8u, new_len);
    put_le64(s + 24u, new_len);
    put_le16(f + 2u, exfat_entry_set_checksum(f, total_ents));
    return vol->dev->write_sectors(vol->dev->ctx, parent_lba, 1u, sec);
}

static int dir_alloc_slots(exfat_vol_t *vol, uint32_t dir_cluster, uint8_t dir_no_fat_chain,
                           uint64_t dir_size, uint32_t parent_lba, uint8_t parent_slot,
                           uint8_t total_ents, uint32_t *out_lba, uint8_t *out_slot)
{
    uint32_t cls_shift = 9u + (uint32_t)vol->spc_shift;
    uint32_t max_cls   = 0u;
    if (dir_no_fat_chain) {
        if (dir_size > 0u) {
            uint64_t mc = (dir_size + ((uint64_t)vol->cluster_size - 1u)) >> cls_shift;
            max_cls = (mc > vol->cluster_count) ? vol->cluster_count : (uint32_t)mc;
        } else {
            max_cls = 1u;
        }
    }

    uint32_t cls     = dir_cluster;
    uint32_t cls_idx = 0u;
    uint8_t  sec[512];

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s = 0u; s < vol->sectors_per_cluster; s++) {
            uint32_t lba = base_lba + s;
            if (vol->dev->read_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            uint32_t free_run   = 0u;
            uint32_t run_start  = 0u;
            for (uint32_t idx = 0u; idx < 16u; idx++) {
                uint8_t etype = sec[idx * 32u];
                if (etype == 0x00u) {
                    if ((16u - idx) >= total_ents) {
                        *out_lba  = lba;
                        *out_slot = (uint8_t)idx;
                        return 0;
                    }
                    /* Pad tail of sector with deleted File markers so scanner continues cleanly */
                    for (uint32_t k = idx; k < 16u; k++) {
                        sec[k * 32u] = 0x05u;
                    }
                    if (vol->dev->write_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
                        return -1;
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

        cls_idx++;
        uint32_t nxt = 0u;
        if (dir_no_fat_chain && cls_idx >= max_cls) {
            nxt = 0u; /* Reached allocated end of contiguous NoFatChain directory */
        } else if (next_cluster(vol, cls, dir_no_fat_chain, &nxt) != 0) {
            return -1;
        }
        if (nxt == 0u) {
            if (dir_no_fat_chain) {
                return -1; /* Avoid overflowing or mutating foreign NoFatChain directory */
            }
            /* Zero the newly allocated cluster BEFORE linking it into the directory chain */
            if (alloc_cluster(vol, 0u, &nxt) != 0 || zero_cluster(vol, nxt) != 0 ||
                fat_set_entry(vol, cls, nxt) != 0) {
                return -1;
            }
            if (grow_dir_size_in_parent(vol, parent_lba, parent_slot) != 0) {
                return -1;
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
    for (uint32_t i = 0u; i < name_len; i++) {
        if (name[i] == '/' || name[i] == '\\') {
            return -1;
        }
    }

    exfat_entry_t existing;
    if (find_path_with_loc(vol, name, &existing, 0, 0) == 0) {
        return existing.is_dir ? 0 : -1;
    }

    uint32_t dir_cls = 0u;
    if (alloc_cluster(vol, 0u, &dir_cls) != 0 || zero_cluster(vol, dir_cls) != 0) {
        return -1;
    }

    uint8_t total_ents = (name_len <= 15u) ? 3u : 4u;
    uint32_t slot_lba  = 0u;
    uint8_t  slot_idx  = 0u;
    if (dir_alloc_slots(vol, vol->root_dir_cluster, 0u, 0u, 0u, 0u, total_ents,
                        &slot_lba, &slot_idx) != 0) {
        return -1;
    }

    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
        return -1;
    }
    build_entry_set(&sec[(uint32_t)slot_idx * 32u], total_ents, name, name_len,
                    EXFAT_ATTR_DIR, dir_cls, vol->cluster_size, vol->default_timestamp);
    return vol->dev->write_sectors(vol->dev->ctx, slot_lba, 1u, sec);
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
    for (uint32_t i = 0u; i < name_len; i++) {
        if (filename[i] == '/' || filename[i] == '\\') {
            return -1;
        }
    }

    exfat_entry_t dir_ent;
    uint32_t parent_lba  = 0u;
    uint8_t  parent_slot = 0u;
    if (find_path_with_loc(vol, dir_path, &dir_ent, &parent_lba, &parent_slot) != 0 || !dir_ent.is_dir) {
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
    if (dir_alloc_slots(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                        parent_lba, parent_slot, total_ents, &slot_lba, &slot_idx) != 0) {
        return -1;
    }

    uint8_t sec[512];
    if (vol->dev->read_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
        return -1;
    }
    build_entry_set(&sec[(uint32_t)slot_idx * 32u], total_ents, filename, name_len,
                    EXFAT_ATTR_ARCHIVE, 0u, 0u, vol->default_timestamp);
    if (vol->dev->write_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
        return -1;
    }

    out->vol        = vol;
    out->dir_lba    = slot_lba;
    out->dir_slot   = slot_idx;
    out->total_ents = total_ents;
    out->active     = 1u;
    return 0;
}

static int writer_write_sector(exfat_writer_t *w, const uint8_t *sec_data)
{
    if (w->cur_cluster == 0u || w->sec_in_cluster >= w->vol->sectors_per_cluster) {
        uint32_t new_c = 0u;
        if (alloc_cluster(w->vol, w->cur_cluster, &new_c) != 0) {
            return -1;
        }
        if (w->first_cluster == 0u) {
            w->first_cluster = new_c;
        }
        w->cur_cluster    = new_c;
        w->sec_in_cluster = 0u;
    }

    uint32_t lba = cluster_to_lba(w->vol, w->cur_cluster) + w->sec_in_cluster;
    if (w->vol->dev->write_sectors(w->vol->dev->ctx, lba, 1u, sec_data) != 0) {
        return -1;
    }
    w->sec_in_cluster++;
    return 0;
}

int exfat_write(exfat_writer_t *w, const void *data, uint32_t len)
{
    if (!w || !w->active || (!data && len > 0u)) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint32_t rem = len;

    while (rem > 0u) {
        if (w->sec_buf_used == 0u && rem >= 512u) {
            if (writer_write_sector(w, src) != 0) {
                return -1;
            }
            src += 512u;
            rem -= 512u;
            w->bytes_written += 512u;
            continue;
        }

        uint32_t space = 512u - w->sec_buf_used;
        uint32_t take  = (rem < space) ? rem : space;
        mem_copy(w->sec_buf + w->sec_buf_used, src, take);
        w->sec_buf_used  += take;
        w->bytes_written += take;
        src              += take;
        rem              -= take;

        if (w->sec_buf_used == 512u) {
            if (writer_write_sector(w, w->sec_buf) != 0) {
                return -1;
            }
            w->sec_buf_used = 0u;
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
    return w->vol->dev->write_sectors(w->vol->dev->ctx, lba, 1u, sec);
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
    if (w->vol->dev->write_sectors(w->vol->dev->ctx, w->dir_lba, 1u, sec) != 0) {
        return -1;
    }

    if (hdr && hdr_len > 0u) {
        if (exfat_patch_header(w, hdr, hdr_len) != 0) {
            return -1;
        }
    }
    return 0;
}

int exfat_close_file(exfat_writer_t *w)
{
    if (!w || !w->active) {
        return -1;
    }

    int tail_err = 0;
    if (w->sec_buf_used > 0u) {
        mem_zero(w->sec_buf + w->sec_buf_used, 512u - w->sec_buf_used);
        if (writer_write_sector(w, w->sec_buf) != 0) {
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
    put_le32(s + 20u, w->first_cluster);
    put_le64(s + 24u, w->bytes_written);
    put_le16(f + 2u, exfat_entry_set_checksum(f, w->total_ents));

    if (w->vol->dev->write_sectors(w->vol->dev->ctx, w->dir_lba, 1u, sec) != 0) {
        return -1;
    }

    w->active = 0u;
    return tail_err;
}

int exfat_stat_fs(const exfat_vol_t *vol, uint32_t *free_mb, uint32_t *total_mb)
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
    if (vol->bitmap_cluster < 2u || vol->bitmap_bytes == 0u) {
        *free_mb = 0u;
        return -1;
    }

    uint32_t need_bytes = (vol->cluster_count + 7u) >> 3;
    uint32_t limit      = (vol->bitmap_bytes < need_bytes) ? vol->bitmap_bytes : need_bytes;
    uint32_t bmp_cls    = vol->bitmap_cluster;
    uint32_t cur_byte   = 0u;
    uint32_t free_cls   = 0u;
    uint8_t  sec[512];

    while (bmp_cls >= 2u && cur_byte < limit) {
        uint32_t base_lba = cluster_to_lba(vol, bmp_cls);
        for (uint32_t s = 0u; s < spc && cur_byte < limit; s++) {
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s, 1u, sec) != 0) {
                return -1;
            }
            uint32_t b_end = ((limit - cur_byte) < 512u) ? (limit - cur_byte) : 512u;
            for (uint32_t b = 0u; b < b_end; b++) {
                uint32_t base_idx = (cur_byte + b) << 3;
                uint32_t rem_bits = vol->cluster_count - base_idx;
                uint32_t nbits    = (rem_bits < 8u) ? rem_bits : 8u;
                uint8_t  v        = sec[b];
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
        }
        if (cur_byte < limit) {
            uint32_t nxt = 0u;
            if (next_bitmap_cluster(vol, bmp_cls, &nxt) != 0) {
                return -1;
            }
            bmp_cls = nxt;
        }
    }

    *free_mb = ((free_cls >> 11) * spc) + (((free_cls & 0x7FFu) * spc) >> 11);
    return 0;
}

static int free_file_clusters(exfat_vol_t *vol, uint32_t first_cluster, uint64_t size, uint8_t no_fat_chain);

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
    for (uint32_t i = 0u; i < name_len; i++) {
        if (filename[i] == '/' || filename[i] == '\\') {
            return -1;
        }
    }

    exfat_entry_t dir_ent;
    if (find_path_with_loc(vol, dir_path, &dir_ent, 0, 0) != 0 || !dir_ent.is_dir) {
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
        if (vol->dev->write_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
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
        (void)list_dir_with_loc(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                                cluster_ref_cb, 0, &ref);
        if (!ref.found) {
            if (free_file_clusters(vol, fc.result.first_cluster, fc.result.size, fc.result.no_fat_chain) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int free_file_clusters(exfat_vol_t *vol, uint32_t first_cluster, uint64_t size, uint8_t no_fat_chain)
{
    if (first_cluster < 2u || size == 0u) {
        return 0;
    }
    if (no_fat_chain) {
        uint32_t cls_shift = 9u + (uint32_t)vol->spc_shift;
        uint64_t n_cls64   = (size + ((uint64_t)vol->cluster_size - 1u)) >> cls_shift;
        uint32_t n_cls     = (n_cls64 > vol->cluster_count) ? vol->cluster_count : (uint32_t)n_cls64;
        for (uint32_t i = 0u; i < n_cls; i++) {
            if (free_cluster_bit(vol, first_cluster + i) != 0) {
                return -1;
            }
        }
        return 0;
    }
    uint32_t cur   = first_cluster;
    uint32_t steps = 0u;
    while (cur >= 2u && (cur - 2u) < vol->cluster_count && steps < vol->cluster_count) {
        uint32_t nxt = 0u;
        if (fat_get_entry(vol, cur, &nxt) != 0) {
            return -1;
        }
        if (free_cluster_bit(vol, cur) != 0) {
            return -1;
        }
        if (fat_set_entry(vol, cur, 0u) != 0) {
            return -1;
        }
        steps++;
        if (nxt >= 0xFFFFFFF8u || nxt < 2u || (nxt - 2u) >= vol->cluster_count) {
            break;
        }
        cur = nxt;
    }
    return 0;
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
    for (uint32_t i = 0u; i < dst_len; i++) {
        if (dst_name[i] == '/' || dst_name[i] == '\\') {
            return -1;
        }
    }

    exfat_entry_t dir_ent;
    uint32_t parent_lba  = 0u;
    uint8_t  parent_slot = 0u;
    if (find_path_with_loc(vol, dir_path, &dir_ent, &parent_lba, &parent_slot) != 0 || !dir_ent.is_dir) {
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
        put_le64(s + 8u, sc.result.size);
        put_le32(s + 20u, sc.result.first_cluster);
        put_le64(s + 24u, sc.result.size);
        put_le16(f + 2u, exfat_entry_set_checksum(f, 1u + (uint32_t)dc.loc.sec_cnt));
        if (vol->dev->write_sectors(vol->dev->ctx, dc.loc.lba, 1u, sec) != 0) {
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
        if (dir_alloc_slots(vol, dir_ent.first_cluster, dir_ent.no_fat_chain, dir_ent.size,
                            parent_lba, parent_slot, total_ents, &slot_lba, &slot_idx) != 0) {
            return -1;
        }
        if (vol->dev->read_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
            return -1;
        }
        uint8_t *ent = &sec[(uint32_t)slot_idx * 32u];
        build_entry_set(ent, total_ents, dst_name, dst_len,
                        EXFAT_ATTR_ARCHIVE, sc.result.first_cluster, sc.result.size,
                        vol->default_timestamp);
        if (sc.result.no_fat_chain) {
            ent[32u + 1u] = 0x03u;
            put_le16(ent + 2u, exfat_entry_set_checksum(ent, total_ents));
        }
        if (vol->dev->write_sectors(vol->dev->ctx, slot_lba, 1u, sec) != 0) {
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
                if (vol->dev->write_sectors(vol->dev->ctx, dlba, 1u, sec) != 0) {
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
        if (vol->dev->write_sectors(vol->dev->ctx, lba, 1u, sec) != 0) {
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
