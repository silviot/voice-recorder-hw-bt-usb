/* Step-1 placeholders for the Bluedroid port boundary in the BT=1 firmware image.
 *
 * Step 1 only proves that the real stack links into the firmware with correct memory placement; Bluedroid is
 * NOT started (no code calls esp_bluedroid_init yet). Every placeholder below therefore fails closed: the
 * functions that would matter at runtime (entropy, timers, persistence) report errors or stop the system,
 * so nothing can silently run on a stub. Real implementations replace this file in step 2 (plan M2).
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

/* esp_timer: step 2 maps these onto FreeRTOS timers (plan M2). */
int64_t esp_timer_get_time(void) { BT_STUB_FATAL(1u); }
int64_t esp_timer_get_expiry_time(esp_timer_handle_t t) { (void)t; BT_STUB_FATAL(2u); }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { (void)a; (void)h; return ESP_FAIL; }
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_FAIL; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_FAIL; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; return ESP_FAIL; }
esp_err_t esp_timer_delete(esp_timer_handle_t t) { (void)t; return ESP_FAIL; }
bool esp_timer_is_active(esp_timer_handle_t t) { (void)t; return false; }

/* nvs: step 2 serves these from the app-owned RAM cache (plan M2). */
esp_err_t nvs_flash_init(void) { return ESP_FAIL; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l) { (void)h; (void)k; (void)o; (void)l; return ESP_FAIL; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l) { (void)h; (void)k; (void)v; (void)l; return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_FAIL; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; return ESP_FAIL; }

/* VFS: never used (SPP runs in ESP_SPP_MODE_CB). */
esp_err_t esp_vfs_register_fs_with_id(const esp_vfs_fs_ops_t *v, int f, void *c, esp_vfs_id_t *i) { (void)v; (void)f; (void)c; (void)i; return ESP_FAIL; }
esp_err_t esp_vfs_register_fd(esp_vfs_id_t i, int *fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_fd(esp_vfs_id_t i, int fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_with_id(esp_vfs_id_t i) { (void)i; return ESP_FAIL; }

/* Entropy: there is no entropy source until step 2's DRBG. Never return fake randomness. */
uint32_t esp_random(void) { BT_STUB_FATAL(3u); }
void esp_fill_random(void *b, size_t n) { (void)b; (void)n; BT_STUB_FATAL(4u); }

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
