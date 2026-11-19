/*
 * Smartlink SL6801 OpenFW Standalone MBR + exFAT Reader
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_fs.h"

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
    return 0;
}

int exfat_list_dir(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain,
                   exfat_dir_cb_t cb, void *user)
{
    if (!vol || !cb || dir_cluster < 2u) {
        return -1;
    }

    exfat_entry_t cur_ent;
    uint8_t in_set       = 0u;
    uint8_t sec_expected = 0u;
    uint8_t sec_seen     = 0u;
    uint8_t have_stream  = 0u;
    uint8_t name_len     = 0u;
    uint8_t name_pos     = 0u;

    uint32_t cls = dir_cluster;
    uint8_t sec[512];

    while (cls >= 2u && (cls - 2u) < vol->cluster_count) {
        uint32_t base_lba = cluster_to_lba(vol, cls);
        for (uint32_t s = 0u; s < vol->sectors_per_cluster; s++) {
            if (vol->dev->read_sectors(vol->dev->ctx, base_lba + s, 1u, sec) != 0) {
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
                    /* File Directory Entry */
                    in_set = (d[1] >= 2u) ? 1u : 0u;
                    sec_expected = d[1];
                    sec_seen = 0u;
                    have_stream = 0u;
                    name_len = 0u;
                    name_pos = 0u;
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
                    /* Stream Extension Entry */
                    cur_ent.no_fat_chain  = (uint8_t)((d[1] >> 1) & 1u);
                    name_len              = d[3];
                    cur_ent.first_cluster = le32(d + 20u);
                    cur_ent.size          = le64(d + 24u);
                    have_stream           = 1u;
                } else if (etype == 0xC1u) {
                    /* File Name Entry (up to 15 UTF-16LE units) */
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
                        int stop = cb(&cur_ent, user);
                        if (stop != 0) {
                            return 0;
                        }
                    }
                }
            }
        }
        uint32_t nxt = 0u;
        if (next_cluster(vol, cls, no_fat_chain, &nxt) != 0) {
            return -1;
        }
        cls = nxt;
    }

    return 0;
}

typedef struct {
    const char   *target;
    uint32_t      target_len;
    exfat_entry_t result;
    int           found;
} find_ctx_t;

static int find_entry_cb(const exfat_entry_t *entry, void *user)
{
    find_ctx_t *ctx = (find_ctx_t *)user;
    if (str_eq_ci_n(entry->name, ctx->target, ctx->target_len)) {
        ctx->result = *entry;
        ctx->found  = 1;
        return 1; /* stop iteration */
    }
    return 0;
}

int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *out)
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

    const char *p = path;
    while (*p == '/' || *p == '\\') {
        p++;
    }
    if (*p == '\0') {
        *out = cur;
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
        if (exfat_list_dir(vol, cur.first_cluster, cur.no_fat_chain, find_entry_cb, &ctx) != 0 ||
            !ctx.found) {
            return -1;
        }
        cur = ctx.result;

        p += seg_len;
        while (*p == '/' || *p == '\\') {
            p++;
        }
    }

    *out = cur;
    return 0;
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

    /* Walk clusters to reach `offset` (cluster_size is always 512 << spc_shift) */
    uint32_t cls_shift     = 9u + (uint32_t)vol->spc_shift;
    uint64_t skip_clusters = offset >> cls_shift;
    uint32_t in_cls_off    = (uint32_t)(offset & ((uint64_t)vol->cluster_size - 1u));
    if (file->no_fat_chain) {
        cls += (uint32_t)skip_clusters;
    } else {
        for (uint64_t i = 0u; i < skip_clusters; i++) {
            uint32_t nxt = 0u;
            if (next_cluster(vol, cls, 0u, &nxt) != 0 || nxt == 0u) {
                return -1;
            }
            cls = nxt;
        }
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
                for (uint32_t i = 0u; i < take; i++) {
                    out[done + i] = sec[s_off + i];
                }
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
        }
    }

    if (bytes_read) {
        *bytes_read = done;
    }
    return 0;
}
