/* Step-1 placeholders for the Bluedroid port boundary in the BT=1 firmware image.
 *
 * Remaining placeholders (VFS: never used in ESP_SPP_MODE_CB; log: compiled out; libc hooks). Entropy, timers and
 * persistence have real implementations (bt_entropy/bt_drbg, bt_alarm/bt_esp_timer, bt_store, bt_port_glue).
 *
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "nvs_flash.h"
#include "esp_log.h"

/* Initialised-data canary: the RTOS self-test (T_BTDATA) checks it lies in .bt_data and was copied from flash. */
volatile uint32_t g_bt_data_canary = 0xB7DA7A11u;

void openfw_rtos_fatal(uint32_t code, uint32_t a, uint32_t b) __attribute__((noreturn));
#define BT_STUB_FATAL(n) openfw_rtos_fatal(0xB0u, (n), 0u)

/* esp_timer -> bt_esp_timer.c, nvs_* -> bt_store.c, esp_random -> bt_port_glue.c (step 2). */

/* VFS: never used (SPP runs in ESP_SPP_MODE_CB). */
esp_err_t esp_vfs_register_fs_with_id(const esp_vfs_fs_ops_t *v, int f, void *c, esp_vfs_id_t *i) { (void)v; (void)f; (void)c; (void)i; return ESP_FAIL; }
esp_err_t esp_vfs_register_fd(esp_vfs_id_t i, int *fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_fd(esp_vfs_id_t i, int fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_with_id(esp_vfs_id_t i) { (void)i; return ESP_FAIL; }

#if !(defined(BT_LOGS) && BT_LOGS)
void esp_log_write(esp_log_level_t l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; }
uint32_t esp_log_timestamp(void) { return 0; }
#endif
int ets_printf(const char *f, ...) { (void)f; return 0; }

/* libc environment hooks expected by picolibc. */
int gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; return -1; }
void _exit(int c) { BT_STUB_FATAL(0x100u | ((uint32_t)c & 0xFFu)); }
static int stdio_put(char c, FILE *f) { (void)f; return (unsigned char)c; } /* step 2: route to openfw_log */
static FILE stdio_file = FDEV_SETUP_STREAM(stdio_put, NULL, NULL, _FDEV_SETUP_WRITE);
FILE *const stdout = &stdio_file;
FILE *const stderr = &stdio_file;
