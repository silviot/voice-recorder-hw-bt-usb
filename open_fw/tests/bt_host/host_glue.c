/* Host versions of bt_port/bt_port_glue.c and bt_port/bt_port_fw_stubs.c for make test-bt-host.
 *
 * Same bindings as the firmware glue: bt_alarm on a FreeRTOS task (priority OPENFW_PRIO_BT_ALARM) with the esp_timer
 * shim on top, bt_entropy behind a mutex, bt_store behind a mutex, the patch 0005-0007 hooks. Differences:
 *   - us_timer_now() is CLOCK_MONOTONIC;
 *   - the entropy pool is fed by a DETERMINISTIC xorshift source. TEST ONLY: it exists so esp_random()/
 *     esp_fill_random() behave as on target (fail closed until seeded, DRBG output afterwards) with reproducible
 *     runs. It must never be linked into firmware (this file is host-only; bt/Makefile.bluedroid never sees it);
 *   - store flush requests are counted, not written;
 *   - log / VFS / libc stubs are host-flavoured.
 * SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "openfw_rtos.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bt_alarm.h"
#include "bt_esp_timer.h"
#include "bt_entropy.h"
#include "bt_store.h"
#include "bt_port_hooks.h"
#include "host_env.h"

extern bool btc_post_alarm_drain(void); /* Bluedroid patch 0006 */

static TaskHandle_t s_alarm_task;
static SemaphoreHandle_t s_ent_mutex, s_store_mutex;
static volatile uint32_t s_hci_cmd_timeouts, s_flush_requests;
static volatile uint32_t s_timers_in_use;

int bth_in_isr(void) { return 0; }

uint64_t us_timer_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* ---- bt_alarm platform (as bt_port_glue.c) ---- */
static uint64_t plat_now_us(void) { return us_timer_now(); }
static void plat_enter(void) { taskENTER_CRITICAL(); }
static void plat_exit(void) { taskEXIT_CRITICAL(); }
static void plat_wake(void)
{
    if (s_alarm_task != NULL) {
        xTaskNotifyGive(s_alarm_task);
    }
}
static int plat_post_drain(void) { return btc_post_alarm_drain() ? 0 : -1; }
static const bt_alarm_platform_t k_alarm_platform = { plat_now_us, plat_enter, plat_exit, plat_wake, plat_post_drain };

static TickType_t wait_ticks(uint32_t us)
{
    if (us == UINT32_MAX) {
        return portMAX_DELAY;
    }
    uint32_t ms = us / 1000u + ((us % 1000u) != 0u);
    TickType_t t = pdMS_TO_TICKS(ms);
    return t == 0u ? 1u : t;
}

static void bt_alarm_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t us = bt_alarm_service();
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks(us));
    }
}

/* ---- hooks for Bluedroid patches 0005-0007 ---- */
void bt_port_alarm_drain(void) { bt_alarm_drain(); }
uint32_t bt_port_alarm_delivery_gen(void) { return bt_alarm_delivery_gen(); }
void bt_port_on_hci_command_timeout(uint16_t opcode)
{
    s_hci_cmd_timeouts++;
    bth_log("bt_host: HCI command %04X timed out (patch 0007 report)\n", opcode);
}
uint32_t bth_hci_cmd_timeouts(void) { return s_hci_cmd_timeouts; }

/* ---- locks ---- */
static void ent_lock(void *c) { (void)c; (void)xSemaphoreTake(s_ent_mutex, portMAX_DELAY); }
static void ent_unlock(void *c) { (void)c; (void)xSemaphoreGive(s_ent_mutex); }
static void store_lock(void *c) { (void)c; (void)xSemaphoreTake(s_store_mutex, portMAX_DELAY); }
static void store_unlock(void *c) { (void)c; (void)xSemaphoreGive(s_store_mutex); }
static void store_request_flush(void *c) { (void)c; s_flush_requests++; }
uint32_t bth_store_flush_requests(void) { return s_flush_requests; }

/* ---- TEST ONLY entropy source: deterministic, NOT random. Claims 4 bits/sample (bt_entropy cutoffs for H = 4:
 * RCT 6, APT 62, as printed by tests/test_bt_entropy.c). ---- */
static const bt_ent_source_cfg_t k_test_only_source = { "TEST-ONLY-xorshift", 64u, 6u, 62u };

static void seed_test_only_entropy(void)
{
    int id = bt_entropy_register_source(&k_test_only_source);
    uint32_t x = 0x0BADC0DEu;
    uint8_t buf[256];
    for (int round = 0; round < 8 && !bt_entropy_ready(); round++) {
        for (size_t i = 0; i < sizeof(buf); i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            buf[i] = (uint8_t)x;
        }
        if (bt_entropy_add_samples(id, buf, sizeof(buf)) != BT_ENT_OK) {
            break;
        }
    }
    if (!bt_entropy_ready()) {
        fprintf(stderr, "bt_host: TEST ONLY entropy source did not seed the DRBG\n");
        abort();
    }
}

void esp_fill_random(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0u) {
        size_t k = n > 4096u ? 4096u : n;
        if (bt_entropy_get(p, k) != BT_ENT_OK) {
            fprintf(stderr, "bt_host: entropy failed closed\n");   /* firmware: openfw_rtos_fatal(0xB0, 3) */
            abort();
        }
        p += k;
        n -= k;
    }
}

uint32_t esp_random(void)
{
    uint32_t v;
    esp_fill_random(&v, sizeof(v));
    return v;
}

int bth_port_init(void)
{
    static int done;
    if (done) {
        return 0;
    }
    s_ent_mutex = xSemaphoreCreateMutex();
    s_store_mutex = xSemaphoreCreateMutex();
    if (s_ent_mutex == NULL || s_store_mutex == NULL) {
        return -1;
    }
    bt_ent_platform_t ep = { ent_lock, ent_unlock, NULL };
    bt_entropy_init(&ep);
    seed_test_only_entropy();
    bt_store_platform_t sp = { store_lock, store_unlock, store_request_flush, NULL };
    bt_store_init(&sp);
    bt_alarm_init(&k_alarm_platform);
    bt_esp_timer_slot_lock_t tl = { plat_enter, plat_exit };
    bt_esp_timer_init(tl);
    if (xTaskCreate(bt_alarm_task, "bt_alarm", 4096u, NULL, OPENFW_PRIO_BT_ALARM, &s_alarm_task) != pdPASS) {
        return -1;
    }
    done = 1;
    return 0;
}

/* ---- esp_timer slot accounting (osi alarms held by Bluedroid), via -Wl,--wrap ---- */
esp_err_t __real_esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t __real_esp_timer_delete(esp_timer_handle_t t);
esp_err_t __wrap_esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    esp_err_t e = __real_esp_timer_create(a, h);
    if (e == ESP_OK) {
        taskENTER_CRITICAL();
        s_timers_in_use++;
        taskEXIT_CRITICAL();
    }
    return e;
}
esp_err_t __wrap_esp_timer_delete(esp_timer_handle_t t)
{
    esp_err_t e = __real_esp_timer_delete(t);
    if (e == ESP_OK) {
        taskENTER_CRITICAL();
        s_timers_in_use--;
        taskEXIT_CRITICAL();
    }
    return e;
}
uint32_t bth_timers_in_use(void) { return s_timers_in_use; }

/* ---- stubs (firmware: bt_port_fw_stubs.c) ---- */
#include "esp_vfs.h"
esp_err_t esp_vfs_register_fs_with_id(const esp_vfs_fs_ops_t *v, int f, void *c, esp_vfs_id_t *i) { (void)v; (void)f; (void)c; (void)i; return ESP_FAIL; }
esp_err_t esp_vfs_register_fd(esp_vfs_id_t i, int *fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_fd(esp_vfs_id_t i, int fd) { (void)i; (void)fd; return ESP_FAIL; }
esp_err_t esp_vfs_unregister_with_id(esp_vfs_id_t i) { (void)i; return ESP_FAIL; }
void esp_log_write(esp_log_level_t l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; }
uint32_t esp_log_timestamp(void) { return 0; }
int ets_printf(const char *f, ...) { (void)f; return 0; }

/* ---- FreeRTOS hooks ---- */
void bth_assert_failed(const char *file, int line)
{
    fprintf(stderr, "bt_host: configASSERT failed at %s:%d\n", file, line);
    abort();
}
