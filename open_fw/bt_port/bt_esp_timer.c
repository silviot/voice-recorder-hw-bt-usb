/* esp_timer API on bt_alarm (plan M2 "Alarms"). Used only by Bluedroid's osi/alarm.c.
 *
 * Callbacks are delivered by bt_alarm_drain() in BTC context, never from a timer task or ISR (patched osi/alarm.c
 * calls the osi callback directly instead of hopping through btc_transfer_context). Static pool, no heap.
 * Re-arming a running timer is allowed (bt_alarm_set re-arms atomically). Timeouts above UINT32_MAX us are rejected.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "bt_alarm.h"
#include "bt_esp_timer.h"

struct esp_timer {
    bt_alarm_t alarm;
    esp_timer_cb_t cb;
    void *arg;
    uint8_t in_use;
};

static struct esp_timer s_pool[BT_ESP_TIMER_MAX];
static bt_esp_timer_slot_lock_t s_lock;

void bt_esp_timer_init(bt_esp_timer_slot_lock_t lock)
{
    s_lock = lock;
    memset(s_pool, 0, sizeof(s_pool)); /* zero before first bt_alarm_setup (bt_alarm.h contract) */
}

static void trampoline(void *p)
{
    struct esp_timer *t = (struct esp_timer *)p;
    t->cb(t->arg);
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    if (a == NULL || h == NULL || a->callback == NULL || a->dispatch_method != ESP_TIMER_TASK) {
        return ESP_ERR_INVALID_ARG;
    }
    struct esp_timer *t = NULL;
    if (s_lock.enter) s_lock.enter();
    for (unsigned i = 0; i < BT_ESP_TIMER_MAX; i++) {
        if (!s_pool[i].in_use) {
            t = &s_pool[i];
            t->in_use = 1u;
            break;
        }
    }
    if (s_lock.exit) s_lock.exit();
    if (t == NULL) {
        return ESP_ERR_NO_MEM;
    }
    t->cb = a->callback;
    t->arg = a->arg;
    bt_alarm_setup(&t->alarm, trampoline, t);
    *h = t;
    return ESP_OK;
}

static int valid(esp_timer_handle_t t)
{
    return t != NULL && t >= &s_pool[0] && t < &s_pool[BT_ESP_TIMER_MAX] && t->in_use;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    if (!valid(t) || us > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)bt_alarm_set(&t->alarm, (uint32_t)us, 0u);
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    if (!valid(t) || us == 0u || us > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)bt_alarm_set(&t->alarm, (uint32_t)us, (uint32_t)us);
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!valid(t)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool was = bt_alarm_is_armed(&t->alarm);
    (void)bt_alarm_cancel(&t->alarm);
    return was ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (!valid(t)) {
        return ESP_ERR_INVALID_ARG;
    }
    bt_alarm_free(&t->alarm);
    if (s_lock.enter) s_lock.enter();
    t->cb = NULL;
    t->arg = NULL;
    t->in_use = 0u;
    if (s_lock.exit) s_lock.exit();
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    return valid(t) && bt_alarm_is_armed(&t->alarm);
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)bt_alarm_now_us();
}

int64_t esp_timer_get_expiry_time(esp_timer_handle_t t)
{
    return (valid(t) && bt_alarm_is_armed(&t->alarm)) ? (int64_t)t->alarm.deadline_us : 0;
}

uint32_t bt_esp_timer_generation(esp_timer_handle_t t)
{
    return valid(t) ? bt_alarm_generation(&t->alarm) : 0u;
}
