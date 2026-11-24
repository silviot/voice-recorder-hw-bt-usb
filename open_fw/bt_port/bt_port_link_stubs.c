/* Link-test-only stubs for the port boundary. The real implementations are Phase 2/3 work:
 *   esp_timer_*  -> FreeRTOS timers + us_timer_now()
 *   nvs_*        -> persist.c (bond keys, bt_config)
 *   esp_vfs_*    -> never used (SPP runs in ESP_SPP_MODE_CB); must not be reached
 *   heap_caps_*, malloc family -> bt_port_heap.c (real implementation)
 *   hooks / libc -> src/rtos/*, startup.c
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "nvs_flash.h"
#include "esp_log.h"

int64_t esp_timer_get_time(void) { return 0; }
int64_t esp_timer_get_expiry_time(esp_timer_handle_t t) { (void)t; return 0; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { (void)a; (void)h; return ESP_FAIL; }
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_FAIL; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_FAIL; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; return ESP_FAIL; }
esp_err_t esp_timer_delete(esp_timer_handle_t t) { (void)t; return ESP_FAIL; }
bool esp_timer_is_active(esp_timer_handle_t t) { (void)t; return false; }

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l) { (void)h; (void)k; (void)o; (void)l; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l) { (void)h; (void)k; (void)v; (void)l; return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t esp_vfs_register_fs_with_id(const esp_vfs_fs_ops_t *v, int f, void *c, esp_vfs_id_t *i) { (void)v; (void)f; (void)c; (void)i; return ESP_FAIL; }
esp_err_t esp_vfs_register_fd(esp_vfs_id_t i, int *fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_fd(esp_vfs_id_t i, int fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_with_id(esp_vfs_id_t i) { (void)i; return ESP_FAIL; }

uint32_t esp_random(void) { return 4; }                 /* link test only: NOT random */
void esp_fill_random(void *b, size_t n) { (void)b; (void)n; }

#if !(defined(BT_LOGS) && BT_LOGS)
void esp_log_write(esp_log_level_t l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; }
uint32_t esp_log_timestamp(void) { return 0; }
#endif
/* libc/OS/kernel hooks normally provided by the firmware */
int gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; return -1; }
void _exit(int c) { (void)c; for (;;) {} }
/* picolibc idiom: the application owns stdout/stderr (route to the UART log in the real port) */
static int stdio_put(char c, FILE *f) { (void)f; return (unsigned char)c; }
static FILE stdio_file = FDEV_SETUP_STREAM(stdio_put, NULL, NULL, _FDEV_SETUP_WRITE);
FILE *const stdout = &stdio_file;
FILE *const stderr = &stdio_file;
uint32_t g_openfw_cpu_hz = 96000000u;
uint64_t us_timer_now(void) { return 0; }
void openfw_rtos_assert_failed(const char *f, int l) { (void)f; (void)l; for (;;) {} }
void vApplicationMallocFailedHook(void) {}
void vApplicationStackOverflowHook(void *t, char *n) { (void)t; (void)n; }
int ets_printf(const char *f, ...) { (void)f; return 0; }
