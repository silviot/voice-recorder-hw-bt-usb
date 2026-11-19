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

/* Callback returns 0 to continue iteration, non-zero to stop early. */
typedef int (*exfat_dir_cb_t)(const exfat_entry_t *entry, void *user);

int exfat_mount(exfat_vol_t *vol, const blkdev_t *dev);
int exfat_list_dir(const exfat_vol_t *vol, uint32_t dir_cluster, uint8_t no_fat_chain,
                   exfat_dir_cb_t cb, void *user);
int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *out);
int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *file,
                    uint64_t offset, void *dst, uint32_t len, uint32_t *bytes_read);

#endif /* OPENFW_FS_H */
