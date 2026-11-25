/*
 * Smartlink SL6801 OpenFW Block Device & Standalone exFAT Filesystem Interface
 *
 * Clean-room, allocation-free MBR + exFAT reader supporting both contiguous
 * (NoFatChain) and FAT-chained files/directories for microSD / eMMC.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_FS_H
#define OPENFW_FS_H

#include <stdint.h>

typedef struct {
    int (*read_sectors)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int (*write_sectors)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    uint32_t total_sectors;
    void *ctx;
} blkdev_t;

typedef struct {
    uint32_t first_cluster;
    uint32_t cluster_idx;
    uint32_t cluster;
} exfat_rd_cache_t;

typedef struct {
    const blkdev_t *dev;
    uint32_t part_lba;
    uint32_t fat_lba;
    uint32_t fat_sectors;
    uint32_t heap_lba;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint8_t  spc_shift;
    uint32_t bitmap_cluster;
    uint32_t bitmap_bytes;
    uint32_t alloc_hint;
    /* Free-cluster count cache (docs/BT_SYNC_SPEED_AND_RECORDING.md F4): computed by the first exfat_stat_fs() after
     * exfat_mount() (which clears it), then kept by every bitmap change made through this volume (allocate / free).
     * Any bitmap I/O error drops it (recomputed by the next exfat_stat_fs). Writers that bypass this volume (USB MSC)
     * are followed by a remount (AGENTS.md MSC boundary), or by exfat_invalidate_free_cache(). */
    uint32_t free_clusters;
    uint8_t  free_valid;
    uint32_t default_timestamp;
    exfat_rd_cache_t rd_cache[4];
    uint8_t  rd_cache_rr;
    uint8_t  fat_cache_valid;
    uint32_t fat_cache_lba;
    uint8_t  fat_cache[512] __attribute__((aligned(4)));
} exfat_vol_t;

#define EXFAT_MAX_NAME 64u
#define EXFAT_ATTR_DIR 0x0010u

typedef struct {
    char     name[EXFAT_MAX_NAME];
    uint64_t size;
    uint64_t valid_data_len;
    uint32_t first_cluster;
    uint16_t file_attr;
    uint8_t  no_fat_chain;
    uint8_t  is_dir;
    uint32_t created_timestamp;
    uint32_t modified_timestamp;
} exfat_entry_t;

typedef struct {
    exfat_vol_t *vol;
    uint32_t     dir_lba;
    uint8_t      dir_slot;
    uint8_t      total_ents;
    uint8_t      active;
    uint32_t     first_cluster;
    uint32_t     cur_cluster;
    uint32_t     sec_in_cluster;
    uint32_t     sec_buf_used;
    uint64_t     bytes_written;
    uint8_t      sec_buf[512];
} exfat_writer_t;

/* Callback returns 0 to continue iteration, non-zero to stop early. */
typedef int (*exfat_dir_cb_t)(const exfat_entry_t *entry, void *user);

int exfat_mount(exfat_vol_t *vol, const blkdev_t *dev);
int exfat_list_dir(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain,
                   exfat_dir_cb_t cb, void *user);
int exfat_list_dir_entry(const exfat_vol_t *vol, const exfat_entry_t *dir,
                         exfat_dir_cb_t cb, void *user);
int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *out);
int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *file,
                    uint64_t offset, void *dst, uint32_t len, uint32_t *bytes_read);

/* Write & maintenance operations (requires dev->write_sectors != NULL and 0x81 Allocation Bitmap) */
void exfat_set_timestamp(exfat_vol_t *vol, uint32_t dos_timestamp);
/* free_mb from the volume's free-cluster cache (one bitmap scan after a mount, then none); total_mb from the VBR. */
int  exfat_stat_fs(exfat_vol_t *vol, uint32_t *free_mb, uint32_t *total_mb);
/* The cached free-cluster count (scans the bitmap when the cache is not valid). */
int  exfat_free_clusters(exfat_vol_t *vol, uint32_t *out_free);
/* Uncached: counts the clear bits of the allocation bitmap (the reference the cache is tested against). */
int  exfat_count_free_scan(const exfat_vol_t *vol, uint32_t *out_free);
void exfat_invalidate_free_cache(exfat_vol_t *vol);
int  exfat_mkdir(exfat_vol_t *vol, const char *name);
int  exfat_create_file(exfat_vol_t *vol, const char *dir_path, const char *filename, exfat_writer_t *out);
int  exfat_write(exfat_writer_t *w, const void *data, uint32_t len);
int  exfat_patch_header(exfat_writer_t *w, const void *hdr, uint32_t len);
int  exfat_checkpoint_file(exfat_writer_t *w, const void *hdr, uint32_t hdr_len);
int  exfat_close_file(exfat_writer_t *w);
int  exfat_delete_file(exfat_vol_t *vol, const char *dir_path, const char *filename);
int  exfat_replace_file(exfat_vol_t *vol, const char *dir_path, const char *src_name, const char *dst_name);
/*
 * Overwrite one 512-byte sector of an existing file in place. offset must be
 * sector aligned and offset + 512 <= ValidDataLength. Touches nothing but that
 * data sector (no FAT, bitmap, directory entry or timestamp update).
 */
int  exfat_overwrite_sector(exfat_vol_t *vol, const exfat_entry_t *file, uint64_t offset,
                            const void *sec512);

#endif /* OPENFW_FS_H */
