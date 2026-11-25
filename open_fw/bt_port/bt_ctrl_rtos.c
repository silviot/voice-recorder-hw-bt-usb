/* bt_ctrl_rtos: see bt_ctrl_rtos.h. Plan §3 priorities: bt_ctrl 8 (sole ROM owner, never blocks on the host),
 * bt_dlv 6 (only context that calls the host callbacks). IRQ 26: the top half lives in bt_rom_real.c (.fast_ramtext)
 * and reaches wake_ctrl_from_isr() through bt_rom_upcalls_t.on_irq.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "openfw_rtos.h"
#include "sl6801_regs.h"
#include "bt_ctrl_rtos.h"
#include "bt_cpu_guard.h"
#include "bt_rom_real.h"
#include "bt_ctrl_leak.h"

#define BT_CTRL_STACK_WORDS  BT_CTRL_RTOS_CTRL_STACK_WORDS   /* probe image: 4 KiB (see bt_ctrl_rtos.h) */
#define BT_DLV_STACK_WORDS   384u   /* 1.5 KiB (plan §4) */
#define RTOS_STATIC __attribute__((section(".rtos_static"), aligned(8)))

static StaticTask_t s_ctrl_tcb RTOS_STATIC;
static StackType_t  s_ctrl_stack[BT_CTRL_STACK_WORDS] RTOS_STATIC;
static StaticTask_t s_dlv_tcb RTOS_STATIC;
static StackType_t  s_dlv_stack[BT_DLV_STACK_WORDS] RTOS_STATIC;
static TaskHandle_t s_ctrl_task;
static TaskHandle_t s_dlv_task;
static const bt_rom_if_t *s_rom;
static volatile uint8_t s_bt_stop;
static volatile uint8_t s_in_run, s_hung;
static volatile uint32_t s_run_t0;
static bt_ctrl_rtos_stats_t s_st;
static bt_cpu_guard_t s_cg;
static volatile uint8_t s_cg_armed, s_cg_tripped;


static inline uint32_t now32(void)
{
    return US_TIMER_LO;
}

/* Plan §3 rule (e): no FreeRTOS call while the ROM PRIMASK lock is held. */
static bool rom_lock_free(void)
{
    if (s_rom != NULL && s_rom->lock_depth() != 0u) {
        s_st.lock_violations++;
        return false;
    }
    return true;
}

static void plat_enter(void) { taskENTER_CRITICAL(); }
static void plat_exit(void) { taskEXIT_CRITICAL(); }

static void plat_wake_ctrl(void)
{
    if (s_ctrl_task != NULL && rom_lock_free()) {
        xTaskNotifyGive(s_ctrl_task);
    }
}

static void plat_wake_ctrl_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (s_ctrl_task != NULL) {
        vTaskNotifyGiveFromISR(s_ctrl_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void plat_wake_dlv(void)
{
    if (s_dlv_task != NULL && rom_lock_free()) {
        xTaskNotifyGive(s_dlv_task);
    }
}

static bool plat_stop_requested(void)
{
    return s_bt_stop != 0u;
}

static void plat_fault(uint32_t code)
{
    taskENTER_CRITICAL();
    s_st.faults++;
    if (s_st.first_fault == 0u) {
        s_st.first_fault = code;
    }
    taskEXIT_CRITICAL();
}

/* bt_ctrl's forwarded-command deadline clock (bt_ctrl_set_clock). The bt_ctrl task is woken by IRQ 26 at the
 * 1600 Hz slot tick whenever the controller runs (BT_HW_RESULTS session 2), so the deadline is checked promptly. */
static uint32_t plat_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const bt_ctrl_platform_t k_platform = {
    plat_enter, plat_exit, plat_wake_ctrl, plat_wake_ctrl_from_isr, plat_wake_dlv, plat_stop_requested, plat_fault,
};

static bt_ctrl_leak_t s_leak;

static void leak_unwind(void)
{
    if (s_rom == &bt_rom_real) {
        (void)bt_rom_real_silence();
        bt_rom_real_force_unlock();
    }
}

static void bt_ctrl_task(void *arg)
{
    (void)arg;
    uint32_t more = 0u;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, more ? 0u : portMAX_DELAY);
        uint32_t t0 = now32();
        s_run_t0 = t0;
        s_in_run = 1u;
        more = bt_ctrl_run();
        /* A ROM path leaked PRIMASK: silence RW-BT and unwind PRIMASK BEFORE taskENTER_CRITICAL() or
         * ulTaskNotifyTake(), otherwise PendSV cannot pend/fire and the CPU deadlocks with IRQs masked. Unwound: run
         * again at once to finish the stop. Not unwound (a ROM without the real unwind): wait for a wake, no busy
         * loop and one fault per leak (bt_ctrl_leak.h). */
        int leak = bt_ctrl_leak_check(&s_leak, s_rom, bt_ctrl_rtos_hard_stop, leak_unwind, plat_fault);
        if (leak != BT_CTRL_LEAK_NONE) {
            s_st.lock_violations++;
            more = leak == BT_CTRL_LEAK_UNWOUND ? 1u : 0u;
        }
        uint32_t dt = now32() - t0;
        taskENTER_CRITICAL();
        s_in_run = 0u;               /* with the total update: the CPU guard never sees this run counted 0 times */
        s_st.ctrl_runs++;
        s_st.ctrl_run_total_us += dt;
        if (more) {
            s_st.ctrl_more++;
        }
        if (dt > s_st.ctrl_run_max_us) {
            s_st.ctrl_run_max_us = dt;
        }
        if (dt > s_st.ctrl_run_max_all_us) {
            s_st.ctrl_run_max_all_us = dt;
        }
        taskEXIT_CRITICAL();
    }
}

static void bt_dlv_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            s_st.dlv_runs++;
            if (bt_ctrl_dlv_run() != BT_CTRL_DLV_RETRY) {
                break;
            }
            s_st.dlv_retries++;
            vTaskDelay(1);                                   /* plan M1: retry with a 1-tick backoff */
        }
    }
}

int bt_ctrl_rtos_init(const bt_rom_if_t *rom, const uint8_t bd_addr[6])
{
    if (s_ctrl_task != NULL) {
        return 0;
    }
    s_rom = rom;
    int err = bt_ctrl_init(rom, &k_platform, bd_addr);
    bt_ctrl_set_clock(plat_now_ms);
    s_ctrl_task = xTaskCreateStatic(bt_ctrl_task, "bt_ctrl", BT_CTRL_STACK_WORDS, NULL, OPENFW_PRIO_BT_CTRL,
                                    s_ctrl_stack, &s_ctrl_tcb);
    s_dlv_task = xTaskCreateStatic(bt_dlv_task, "bt_dlv", BT_DLV_STACK_WORDS, NULL, OPENFW_PRIO_BT_DLV,
                                   s_dlv_stack, &s_dlv_tcb);
    if (s_ctrl_task == NULL || s_dlv_task == NULL) {
        return -1;
    }
    return err;
}

void bt_ctrl_rtos_hard_stop(void)
{
    s_bt_stop = 1u;
    nvic_disable_irq(IRQ_BT);
}

static void demote(void)
{
    s_hung = 1u;
    bt_ctrl_rtos_hard_stop();
    vTaskPrioritySet(s_ctrl_task, tskIDLE_PRIORITY);       /* the app (and the probe) run again */
}

int bt_ctrl_rtos_guard(uint32_t limit_us)
{
    if (s_hung) {
        return 1;
    }
    if (s_ctrl_task == NULL || !s_in_run || now32() - s_run_t0 <= limit_us) {
        return 0;
    }
    demote();
    return 1;
}

int bt_ctrl_rtos_cpu_guard(uint32_t limit_pm, uint32_t samples)
{
    if (s_cg_tripped) {
        return 1;
    }
    if (s_ctrl_task == NULL || s_hung) {
        return 0;
    }
    taskENTER_CRITICAL();                                  /* bt_ctrl updates these in its own critical section */
    int trip = 0;
    if (s_cg_armed) {
        uint32_t now = now32();
        uint32_t busy = s_st.ctrl_run_total_us + (s_in_run ? now - s_run_t0 : 0u);
        trip = bt_cpu_guard_step(&s_cg, now, busy, limit_pm, samples);
        if (s_cg.peak_pm > s_st.cpu_peak_pm) {
            s_st.cpu_peak_pm = s_cg.peak_pm;
        }
    }
    taskEXIT_CRITICAL();
    if (!trip) {
        return 0;
    }
    s_cg_tripped = 1u;
    demote();
    return 1;
}

void bt_ctrl_rtos_cpu_guard_arm(int on)
{
    taskENTER_CRITICAL();
    bt_cpu_guard_reset(&s_cg);
    s_cg_armed = on ? 1u : 0u;
    taskEXIT_CRITICAL();
}

void bt_ctrl_rtos_stats(bt_ctrl_rtos_stats_t *out)
{
    taskENTER_CRITICAL();
    *out = s_st;
    taskEXIT_CRITICAL();
    out->hung = s_hung;
    out->cpu_tripped = s_cg_tripped;
    out->ctrl_stack_free_words = s_ctrl_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_ctrl_task) : 0u;
    out->dlv_stack_free_words = s_dlv_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_dlv_task) : 0u;
}

void bt_ctrl_rtos_reset_max(void)
{
    taskENTER_CRITICAL();
    s_st.ctrl_run_max_us = 0u;
    s_st.cpu_peak_pm = 0u;
    s_cg.peak_pm = 0u;
    taskEXIT_CRITICAL();
}
