/*
 * FreeRTOS glue for open_fw: heap regions, SysTick bridge, idle hook, fatal-error reporting.
 *
 * SysTick is handled by startup.c:SysTick_Handler() (g_systick_ms, keys_scan_tick, experiment watchdog).
 * The kernel rides on that same 1 ms tick via openfw_rtos_tick(). vPortSetupTimerInterrupt() starts it
 * with the shared systick_init() so the tick runs from the first scheduler instant; main.c's later
 * systick_init() call is a no-op when the configuration is unchanged (wdt_timer.c).
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
extern uint8_t _srtos_heap_lo[];
extern uint8_t _ertos_heap_lo[];

extern void xPortSysTickHandler(void);
extern void systick_init(uint32_t cpu_hz);

/* Bluedroid computes its task priorities from configMAX_PRIORITIES; keep openfw_rtos.h in sync. */
_Static_assert(configMAX_PRIORITIES == 10u, "openfw_rtos.h priorities assume configMAX_PRIORITIES == 10");
_Static_assert(OPENFW_PRIO_BT_HCI == configMAX_PRIORITIES - 3u, "hciT priority mismatch (hci_layer.c)");
_Static_assert(OPENFW_PRIO_BT_BTU == configMAX_PRIORITIES - 5u, "BTU priority mismatch (btu_init.c)");
_Static_assert(OPENFW_PRIO_BT_BTC == configMAX_PRIORITIES - 6u, "BTC priority mismatch (btc_task.c)");
_Static_assert(OPENFW_PRIO_BT_CTRL < configTIMER_TASK_PRIORITY, "timer daemon must stay above bt_ctrl");
_Static_assert(OPENFW_PRIO_BT_DLV > OPENFW_PRIO_BT_BTU && OPENFW_PRIO_BT_DLV < OPENFW_PRIO_BT_HCI, "bt_dlv placement");

static openfw_rtos_entry_t g_app_entry;

#if OPENFW_RTOS_SELFTEST
void openfw_rtos_selftest_start(void);
void openfw_rtos_selftest_tick_hook(void);
#endif

void vPortSetupTimerInterrupt(void)
{
    systick_init(g_openfw_cpu_hz); /* 1 kHz == configTICK_RATE_HZ */
}

void openfw_rtos_task_wait(void)
{
    uint32_t ipsr;
    __asm__ volatile ("mrs %0, ipsr" : "=r"(ipsr));
    if (ipsr != 0u || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        OPENFW_WFI();
        return;
    }
    (void)ulTaskNotifyTake(pdTRUE, 1);
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

/* RAM copy of the last fatal record (AON_BACKUP survives reset on silicon; the QEMU model does not keep it). */
volatile uint32_t g_rtos_fatal[3];

void openfw_rtos_fatal(uint32_t code, uint32_t a, uint32_t b)
{
    __asm__ volatile ("cpsid i" ::: "memory");
    g_rtos_fatal[0] = OPENFW_FAULT_MAGIC_BASE | (code & 0xFFu);
    g_rtos_fatal[1] = a;
    g_rtos_fatal[2] = b;
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

#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID
/* BT=1: allocation failure must reach the caller (Bluedroid fail-soft paths, BT recovery ladder; plan M1). Count it
 * for telemetry and let pvPortMalloc() return NULL. Boot-time kernel object creation checks its own results. */
volatile uint32_t g_rtos_malloc_failures;
void vApplicationMallocFailedHook(void)
{
    g_rtos_malloc_failures++;
}
#else
void vApplicationMallocFailedHook(void)
{
    openfw_rtos_fatal(0xF1u, (uint32_t)xPortGetFreeHeapSize(), (uint32_t)xPortGetMinimumEverFreeHeapSize());
}
#endif

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
/* Placed in .rtos_static (RTOS_HEAP region, NOLOAD): main .bss is nearly full. Not zeroed at boot;
 * xTaskCreateStatic() initialises the TCB and fills the stack itself. */
#define RTOS_STATIC __attribute__((section(".rtos_static"), aligned(8)))
static StaticTask_t g_idle_tcb RTOS_STATIC;
static StackType_t  g_idle_stack[configMINIMAL_STACK_SIZE] RTOS_STATIC;
static StaticTask_t g_timer_tcb RTOS_STATIC;
static StackType_t  g_timer_stack[configTIMER_TASK_STACK_DEPTH] RTOS_STATIC;

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
    /* heap_5 needs regions in ascending address order: lower (Exp 0 validated) first. */
    HeapRegion_t regions[] = {
        { _srtos_heap_lo, (size_t)(_ertos_heap_lo - _srtos_heap_lo) },
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
