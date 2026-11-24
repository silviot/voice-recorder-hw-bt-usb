/* Types only: SPP VFS mode is not used (SPP runs in ESP_SPP_MODE_CB). SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
typedef int esp_vfs_id_t;
typedef struct { ssize_t (*write)(int, const void *, size_t); ssize_t (*read)(int, void *, size_t); int (*close)(int); int (*open)(const char *, int, int); int (*fstat)(int, void *); int (*fcntl)(int, int, int); int (*ioctl)(int, int, va_list); int (*fsync)(int); int (*select)(int, void *, void *, void *, void *); int (*start_select)(int, void *, void *, void *, void *, void *); int (*end_select)(void *); void *unused; } esp_vfs_fs_ops_t;
#define ESP_VFS_FLAG_STATIC 0
#define ESP_VFS_FLAG_CONTEXT_PTR 1
esp_err_t esp_vfs_register_fs(const char *base, const esp_vfs_fs_ops_t *vfs, int flags, void *ctx, esp_vfs_id_t *id);
esp_err_t esp_vfs_register_fs_with_id(const esp_vfs_fs_ops_t *vfs, int flags, void *ctx, esp_vfs_id_t *id);
esp_err_t esp_vfs_register_fd(esp_vfs_id_t id, int *fd);
esp_err_t esp_vfs_unregister_fd(esp_vfs_id_t id, int fd);
esp_err_t esp_vfs_unregister_with_id(esp_vfs_id_t id);
