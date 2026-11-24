/*
 * On-target FreeRTOS self-test (OPENFW_RTOS_SELFTEST=1). Runs as a separate task above the app
 * task, publishes results in g_rtos_selftest for tools/run_qemu.py (or a debugger) to read.
 * Not part of production images.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "event_groups.h"
#include "openfw.h"
#include "openfw_rtos.h"

#define ST_MAGIC 0x52544F53u /* 'RTOS' */

enum {
    T_DELAY = 0, T_QUEUE, T_MUTEX_PI, T_TIMER, T_NOTIFY, T_EVENTGROUP,
    T_HEAP, T_PREEMPT, T_FROM_ISR, T_STACK, T_COUNT
};

typedef struct {
    uint32_t magic;
    uint32_t done;
    uint32_t pass_mask;
    uint32_t fail_mask;
    uint32_t heap_free_start;
    uint32_t heap_min_end;
    uint32_t detail[T_COUNT];
} rtos_selftest_t;

volatile rtos_selftest_t g_rtos_selftest = { ST_MAGIC, 0, 0, 0, 0, 0, { 0 } };

static void report(int t, int ok, uint32_t detail)
{
    g_rtos_selftest.detail[t] = detail;
    if (ok) {
        g_rtos_selftest.pass_mask |= 1u << t;
    } else {
        g_rtos_selftest.fail_mask |= 1u << t;
    }
}

/* ---- T_QUEUE ---- */
static QueueHandle_t g_q;
static volatile uint32_t g_q_ok;
static void q_producer(void *a)
{
    (void)a;
    for (uint32_t i = 0; i < 100; i++) {
        xQueueSend(g_q, &i, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}
static void q_consumer(void *a)
{
    (void)a;
    uint32_t v, expect = 0;
    while (expect < 100 && xQueueReceive(g_q, &v, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (v != expect) { break; }
        expect++;
    }
    g_q_ok = expect;
    vTaskDelete(NULL);
}

/* ---- T_MUTEX_PI ---- */
static SemaphoreHandle_t g_mtx;
static TaskHandle_t g_low;
static volatile UBaseType_t g_low_prio_seen;
static volatile uint32_t g_hi_got;
static void pi_low(void *a)
{
    (void)a;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(30)); /* hi blocks on the mutex meanwhile; low must be boosted */
    g_low_prio_seen = uxTaskPriorityGet(NULL);
    xSemaphoreGive(g_mtx);
    vTaskDelete(NULL);
}
static void pi_high(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(500)) == pdTRUE) {
        g_hi_got = 1;
        xSemaphoreGive(g_mtx);
    }
    vTaskDelete(NULL);
}

/* ---- T_TIMER ---- */
static volatile uint32_t g_timer_hits;
static void timer_cb(TimerHandle_t t) { (void)t; g_timer_hits++; }

/* ---- T_NOTIFY / T_FROM_ISR ---- */
static TaskHandle_t g_notify_target;
static volatile uint32_t g_notify_got;
static volatile uint32_t g_isr_kicks;
static volatile uint32_t g_isr_target_enable;
static void notify_waiter(void *a)
{
    (void)a;
    while (g_notify_got < 5) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)) == 0) { break; }
        g_notify_got++;
    }
    vTaskDelete(NULL);
}

static TaskHandle_t g_isr_waiter;
static volatile uint32_t g_isr_got;
static void isr_waiter(void *a)
{
    (void)a;
    while (g_isr_got < 10) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)) == 0) { break; }
        g_isr_got++;
    }
    vTaskDelete(NULL);
}

/* Called from SysTick context (lowest priority, valid for FromISR APIs). */
void openfw_rtos_selftest_tick_hook(void)
{
    if (g_isr_target_enable && g_isr_waiter && g_isr_kicks < 10) {
        BaseType_t woken = pdFALSE;
        g_isr_kicks++;
        vTaskNotifyGiveFromISR(g_isr_waiter, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/* ---- T_EVENTGROUP ---- */
static EventGroupHandle_t g_eg;
static volatile uint32_t g_eg_ok;
static void eg_waiter(void *a)
{
    (void)a;
    EventBits_t b = xEventGroupWaitBits(g_eg, 0x7, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
    g_eg_ok = (b & 0x7) == 0x7;
    vTaskDelete(NULL);
}

/* ---- T_PREEMPT ---- */
static volatile uint32_t g_pre_spin;
static volatile uint32_t g_pre_hi_ms;
static volatile uint32_t g_pre_start_ms;
extern volatile uint32_t g_systick_ms;
static void pre_hi(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(20));
    g_pre_hi_ms = g_systick_ms - g_pre_start_ms; /* should be ~20 even though a lower task busy-spins */
    vTaskDelete(NULL);
}
static void pre_lo(void *a)
{
    (void)a;
    uint32_t end = g_systick_ms + 60;
    while ((int32_t)(g_systick_ms - end) < 0) { g_pre_spin++; }
    vTaskDelete(NULL);
}

static void selftest_main(void *arg)
{
    (void)arg;
    g_rtos_selftest.heap_free_start = openfw_rtos_heap_free();

    /* T_DELAY: vTaskDelay(50) measured against the 1 ms tick counter */
    {
        uint32_t t0 = g_systick_ms;
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t dt = g_systick_ms - t0;
        report(T_DELAY, dt >= 49 && dt <= 70, dt);
    }
    /* T_QUEUE */
    {
        g_q = xQueueCreate(8, sizeof(uint32_t));
        g_q_ok = 0;
        xTaskCreate(q_consumer, "qc", 256, NULL, 3, NULL);
        xTaskCreate(q_producer, "qp", 256, NULL, 2, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
        report(T_QUEUE, g_q_ok == 100, g_q_ok);
        vQueueDelete(g_q);
    }
    /* T_MUTEX_PI: low=2, high=4 -> low must observe priority 4 while high waits */
    {
        g_mtx = xSemaphoreCreateMutex();
        g_hi_got = 0; g_low_prio_seen = 0;
        xTaskCreate(pi_low, "pil", 256, NULL, 2, &g_low);
        xTaskCreate(pi_high, "pih", 256, NULL, 4, NULL);
        vTaskDelay(pdMS_TO_TICKS(120));
        report(T_MUTEX_PI, g_hi_got == 1 && g_low_prio_seen == 4, (uint32_t)g_low_prio_seen);
        vSemaphoreDelete(g_mtx);
    }
    /* T_TIMER: 10 ms period over ~80 ms */
    {
        g_timer_hits = 0;
        TimerHandle_t t = xTimerCreate("t", pdMS_TO_TICKS(10), pdTRUE, NULL, timer_cb);
        xTimerStart(t, 0);
        vTaskDelay(pdMS_TO_TICKS(85));
        xTimerStop(t, portMAX_DELAY);
        xTimerDelete(t, portMAX_DELAY);
        report(T_TIMER, g_timer_hits >= 6 && g_timer_hits <= 10, g_timer_hits);
    }
    /* T_NOTIFY: 5 gives from a task */
    {
        g_notify_got = 0;
        xTaskCreate(notify_waiter, "nw", 256, NULL, 3, &g_notify_target);
        for (int i = 0; i < 5; i++) {
            xTaskNotifyGive(g_notify_target);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        report(T_NOTIFY, g_notify_got == 5, g_notify_got);
    }
    /* T_FROM_ISR: 10 notifications issued from the SysTick handler */
    {
        g_isr_got = 0; g_isr_kicks = 0;
        xTaskCreate(isr_waiter, "iw", 256, NULL, 3, (TaskHandle_t *)&g_isr_waiter);
        g_isr_target_enable = 1;
        vTaskDelay(pdMS_TO_TICKS(60));
        g_isr_target_enable = 0;
        report(T_FROM_ISR, g_isr_got == 10, g_isr_got);
    }
    /* T_EVENTGROUP */
    {
        g_eg = xEventGroupCreate();
        g_eg_ok = 0;
        xTaskCreate(eg_waiter, "eg", 256, NULL, 3, NULL);
        xEventGroupSetBits(g_eg, 0x1);
        vTaskDelay(pdMS_TO_TICKS(2));
        xEventGroupSetBits(g_eg, 0x6);
        vTaskDelay(pdMS_TO_TICKS(20));
        report(T_EVENTGROUP, g_eg_ok == 1, g_eg_ok);
        vEventGroupDelete(g_eg);
    }
    /* T_PREEMPT: high task wakes on time while a lower task spins */
    {
        g_pre_hi_ms = 0; g_pre_spin = 0;
        g_pre_start_ms = g_systick_ms;
        xTaskCreate(pre_lo, "pl", 256, NULL, 2, NULL);
        xTaskCreate(pre_hi, "ph", 256, NULL, 5, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
        report(T_PREEMPT, g_pre_hi_ms >= 19 && g_pre_hi_ms <= 25 && g_pre_spin > 0, g_pre_hi_ms);
    }
    /* T_HEAP: create/delete leaves the heap where it was */
    {
        uint32_t before = openfw_rtos_heap_free();
        for (int i = 0; i < 20; i++) {
            TaskHandle_t h;
            xTaskCreate(notify_waiter, "hp", 256, NULL, 2, &h);
            vTaskDelay(pdMS_TO_TICKS(1));
            vTaskDelete(h);
            vTaskDelay(pdMS_TO_TICKS(1)); /* let idle free TCB/stack */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t after = openfw_rtos_heap_free();
        report(T_HEAP, after + 64u >= before, before - after);
    }
    /* T_STACK: this task's high-water mark is sane (not overflowed, not untouched) */
    {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        report(T_STACK, hw > 20 && hw < 500, (uint32_t)hw);
    }

    g_rtos_selftest.heap_min_end = openfw_rtos_heap_min_free();
    g_rtos_selftest.done = 1;
    vTaskDelete(NULL);
}

void openfw_rtos_selftest_start(void)
{
    xTaskCreate(selftest_main, "selftest", 512, NULL, 2, NULL);
}
