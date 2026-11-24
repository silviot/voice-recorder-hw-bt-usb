/* Firmware glue for the Bluedroid port (plan M2): FreeRTOS bindings for bt_alarm / esp_timer, bt_entropy, bt_store,
 * the bt_alarm task, and the hooks used by Bluedroid patches 0005-0007. Host harnesses provide their own glue.
 *
 * bt_port_init() must run in the app task before esp_bluedroid_init() (bonds are loaded by the app with
 * bt_store_load() right after it). Nothing here starts Bluedroid.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "openfw_rtos.h"
#include "esp_err.h"
#include "bt_alarm.h"
#include "bt_esp_timer.h"
#include "bt_entropy.h"
#include "bt_store.h"
#include "bt_port_hooks.h"
#include "bt_port.h"

extern uint64_t us_timer_now(void);
extern bool btc_post_alarm_drain(void); /* Bluedroid patch 0006 */

#define BT_ALARM_STACK_WORDS 256u
#define RTOS_STATIC __attribute__((section(".rtos_static"), aligned(8)))

static StaticTask_t s_alarm_tcb RTOS_STATIC;
static StackType_t  s_alarm_stack[BT_ALARM_STACK_WORDS] RTOS_STATIC;
static TaskHandle_t s_alarm_task;
static StaticSemaphore_t s_ent_mutex_buf;
static SemaphoreHandle_t s_ent_mutex;
static StaticSemaphore_t s_store_mutex_buf;
static SemaphoreHandle_t s_store_mutex;
static volatile uint32_t s_hci_cmd_timeouts;

/* ---- bt_alarm platform ---- */
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

static const bt_alarm_platform_t k_alarm_platform = {
    plat_now_us, plat_enter, plat_exit, plat_wake, plat_post_drain,
};

/* Convert the time to the next deadline into a wait: any positive remainder rounds UP to >= 1 tick so the task
 * never spins inside a tick; "no deadline" blocks until a set() wakes it. */
static TickType_t wait_ticks(uint32_t us)
{
    if (us == UINT32_MAX) {
        return portMAX_DELAY;
    }
    uint32_t ms = (us + 999u) / 1000u;
    TickType_t t = pdMS_TO_TICKS(ms);
    if (t == 0u) {
        t = 1u;
    }
    return (t >= portMAX_DELAY) ? (portMAX_DELAY - 1u) : t;
}

static void bt_alarm_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t us = bt_alarm_service();
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks(us));
    }
}

/* ---- hooks for Bluedroid patches ---- */
void bt_port_alarm_drain(void) { bt_alarm_drain(); }
uint32_t bt_port_alarm_delivery_gen(void) { return bt_alarm_delivery_gen(); }
void bt_port_on_hci_command_timeout(uint16_t opcode)
{
    (void)opcode;
    s_hci_cmd_timeouts++; /* step 3: also posts a BT fault to the app (recovery ladder) */
}

/* ---- locks ---- */
static void ent_lock(void *c) { (void)c; (void)xSemaphoreTake(s_ent_mutex, portMAX_DELAY); }
static void ent_unlock(void *c) { (void)c; (void)xSemaphoreGive(s_ent_mutex); }
static void store_lock(void *c) { (void)c; (void)xSemaphoreTake(s_store_mutex, portMAX_DELAY); }
static void store_unlock(void *c) { (void)c; (void)xSemaphoreGive(s_store_mutex); }

static bt_store_flush_fn_t s_store_flush;
static void store_request_flush(void *c) { (void)c; if (s_store_flush) s_store_flush(); }

int bt_port_init(bt_store_flush_fn_t request_store_flush)
{
    static int done;
    if (done) {
        return 0;
    }
    s_ent_mutex = xSemaphoreCreateMutexStatic(&s_ent_mutex_buf);
    s_store_mutex = xSemaphoreCreateMutexStatic(&s_store_mutex_buf);
    bt_ent_platform_t ep = { ent_lock, ent_unlock, NULL };
    bt_entropy_init(&ep);
    s_store_flush = request_store_flush;
    bt_store_platform_t sp = { store_lock, store_unlock, store_request_flush, NULL };
    bt_store_init(&sp);
    bt_alarm_init(&k_alarm_platform);
    bt_esp_timer_slot_lock_t tl = { plat_enter, plat_exit };
    bt_esp_timer_init(tl);
    s_alarm_task = xTaskCreateStatic(bt_alarm_task, "bt_alarm", BT_ALARM_STACK_WORDS, NULL, OPENFW_PRIO_BT_ALARM,
                                     s_alarm_stack, &s_alarm_tcb);
    if (s_alarm_task == NULL) {
        return -1;
    }
    done = 1;
    return 0;
}

uint32_t bt_port_hci_command_timeouts(void) { return s_hci_cmd_timeouts; }

/* ---- entropy for code that cannot fail (never used by the Classic-only build; plan M2) ---- */
void esp_fill_random(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0u) {
        size_t k = n > 4096u ? 4096u : n;
        if (bt_entropy_get(p, k) != BT_ENT_OK) {
            openfw_rtos_fatal(0xB0u, 3u, 0u); /* fail closed: no fallback randomness */
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
