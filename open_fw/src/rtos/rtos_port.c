/*
 * FreeRTOS glue for open_fw: heap regions, SysTick bridge, idle hook, fatal-error reporting.
 *
 * SysTick is configured by main.c:systick_init() and handled by startup.c:SysTick_Handler()
 * (g_systick_ms, keys_scan_tick, experiment watchdog). The kernel rides on that same 1 ms tick
 * via openfw_rtos_tick(); vPortSetupTimerInterrupt() therefore does nothing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_rtos.h"

uint32_t g_openfw_cpu_hz = 96000000u;

extern uint8_t _srtos_heap0[];
extern uint8_t _ertos_heap0[];

extern void xPortSysTickHandler(void);

static openfw_rtos_entry_t g_app_entry;

#if OPENFW_RTOS_SELFTEST
void openfw_rtos_selftest_start(void);
void openfw_rtos_selftest_tick_hook(void);
#endif

void vPortSetupTimerInterrupt(void)
{
    /* SysTick already running at configTICK_RATE_HZ (1 kHz) from systick_init(). */
}

void openfw_rtos_tick(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xPortSysTickHandler();
#if OPENFW_RTOS_SELFTEST
        openfw_rtos_selftest_tick_hook();
#endif
    }
}

void vApplicationIdleHook(void)
{
    OPENFW_WFI();
}

void openfw_rtos_fatal(uint32_t code, uint32_t a, uint32_t b)
{
    __asm__ volatile ("cpsid i" ::: "memory");
    AON_BACKUP(0u) = OPENFW_FAULT_MAGIC_BASE | (code & 0xFFu);
    AON_BACKUP(1u) = a;
    AON_BACKUP(2u) = b;
    AON_BACKUP(3u) = 0u;
    AON_BACKUP(4u) = 0u;
    AON_BACKUP(5u) = 0u;
    __asm__ volatile ("dsb" ::: "memory");
    system_reboot();
    for (;;) {
        __asm__ volatile ("nop");
    }
}

void openfw_rtos_assert_failed(const char *file, int line)
{
    openfw_rtos_fatal(0xF0u, (uint32_t)line, (uint32_t)(uintptr_t)file);
}

void vApplicationMallocFailedHook(void)
{
    openfw_rtos_fatal(0xF1u, (uint32_t)xPortGetFreeHeapSize(), (uint32_t)xPortGetMinimumEverFreeHeapSize());
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    openfw_rtos_fatal(0xF2u, (uint32_t)(uintptr_t)task, (uint32_t)(uintptr_t)name);
}

uint32_t openfw_rtos_heap_free(void)
{
    return (uint32_t)xPortGetFreeHeapSize();
}

uint32_t openfw_rtos_heap_min_free(void)
{
    return (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

/* Static memory for the kernel's own tasks (configSUPPORT_STATIC_ALLOCATION=1). */
static StaticTask_t g_idle_tcb;
static StackType_t  g_idle_stack[configMINIMAL_STACK_SIZE] __attribute__((aligned(8)));
static StaticTask_t g_timer_tcb;
static StackType_t  g_timer_stack[configTIMER_TASK_STACK_DEPTH] __attribute__((aligned(8)));

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *depth)
{
    *tcb = &g_idle_tcb;
    *stack = g_idle_stack;
    *depth = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *depth)
{
    *tcb = &g_timer_tcb;
    *stack = g_timer_stack;
    *depth = configTIMER_TASK_STACK_DEPTH;
}

static void app_task(void *arg)
{
    (void)arg;
    (void)g_app_entry();
    /* Mirror startup.c: reboot cleanly if the app ever returns. */
    __asm__ volatile ("cpsid i\ndsb" ::: "memory");
    system_reboot();
    for (;;) {
        __asm__ volatile ("nop");
    }
}

void openfw_rtos_start(openfw_rtos_entry_t entry)
{
    HeapRegion_t regions[] = {
        { _srtos_heap0, (size_t)(_ertos_heap0 - _srtos_heap0) },
        { NULL, 0 },
    };
    vPortDefineHeapRegions(regions);
    g_app_entry = entry;

    if (xTaskCreate(app_task, "app", OPENFW_APP_STACK_WORDS, NULL, OPENFW_PRIO_APP, NULL) != pdPASS) {
        openfw_rtos_fatal(0xF1u, 0u, 0u);
    }
#if OPENFW_RTOS_SELFTEST
    openfw_rtos_selftest_start();
#endif
    vTaskStartScheduler();
    openfw_rtos_fatal(0xF1u, 1u, 0u); /* scheduler returned: no memory for idle/timer task */
}
