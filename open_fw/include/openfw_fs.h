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
} exfat_vol_t;

#define EXFAT_MAX_NAME 64u
#define EXFAT_ATTR_DIR 0x0010u

typedef struct {
    char     name[EXFAT_MAX_NAME];
    uint64_t size;
    uint32_t first_cluster;
    uint16_t file_attr;
    uint8_t  no_fat_chain;
    uint8_t  is_dir;
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
int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *out);
int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *file,
                    uint64_t offset, void *dst, uint32_t len, uint32_t *bytes_read);

/* Write operations (requires dev->write_sectors != NULL and 0x81 Allocation Bitmap) */
int exfat_mkdir(exfat_vol_t *vol, const char *name);
int exfat_create_file(exfat_vol_t *vol, const char *dir_path, const char *filename, exfat_writer_t *out);
int exfat_write(exfat_writer_t *w, const void *data, uint32_t len);
int exfat_patch_header(exfat_writer_t *w, const void *hdr, uint32_t len);
int exfat_close_file(exfat_writer_t *w);

#endif /* OPENFW_FS_H */
