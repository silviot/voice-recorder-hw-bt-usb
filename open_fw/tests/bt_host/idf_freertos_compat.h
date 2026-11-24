/* HOST counterpart of bt_port/idf_freertos_compat.h for the FreeRTOS POSIX port (make test-bt-host).
 * tests/bt_host/ precedes bt_port/ on the harness include path, so this file replaces the firmware one; keep the two in
 * sync (same macro set). Differences, each because the firmware version cannot work on the host:
 *  - xPortInIsrContext() reads the Cortex-M ICSR (0xE000ED04); here it asks the harness whether the caller is the
 *    simulated IRQ context (always false: Bluedroid code never runs from the POSIX port's tick signal).
 *  - xTaskCreatePinnedToCore() converts IDF stack BYTES to words like the firmware, then multiplies by
 *    BTH_STACK_SCALE: the POSIX port sizes each task's pthread stack from the FreeRTOS depth (x sizeof(StackType_t)
 *    = 8), and ASan/UBSan frames are several times larger than Cortex-M4 -Os frames.
 *  - portENTER_CRITICAL_ISR/portEXIT_CRITICAL_ISR use the POSIX port's (no-op) interrupt mask.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include <task.h>
#include <queue.h>
#include <semphr.h>

#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY 0x7FFFFFFF
#endif
#define portNUM_PROCESSORS 1
#define ESP_STATIC_ANALYZER_CHECK(cond, ret) ((void)0)

typedef struct { UBaseType_t isr_state; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portMUX_FREE_VAL 0
#define portMUX_INITIALIZE(m)      do { (m)->isr_state = 0; } while (0)

#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(...)     vPortEnterCritical()
#define portEXIT_CRITICAL(...)      vPortExitCritical()
#define portENTER_CRITICAL_ISR(m)   do { (m)->isr_state = portSET_INTERRUPT_MASK_FROM_ISR(); } while (0)
#define portEXIT_CRITICAL_ISR(m)    do { portCLEAR_INTERRUPT_MASK_FROM_ISR((m)->isr_state); } while (0)

#ifndef portYIELD_WITHIN_API
#define portYIELD_WITHIN_API portYIELD
#endif
int bth_in_isr(void);
#define xPortInIsrContext() (bth_in_isr())
#define xPortCanYield() (!xPortInIsrContext())

#ifndef BTH_STACK_SCALE
#define BTH_STACK_SCALE 8u
#endif
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                                 void *arg, UBaseType_t prio, TaskHandle_t *h, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, (configSTACK_DEPTH_TYPE)(((stack_bytes + 3u) / 4u) * BTH_STACK_SCALE), arg, prio, h);
}

#define xPortGetCoreID() 0
#undef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR(...) portEND_SWITCHING_ISR( (sizeof(#__VA_ARGS__) > 1) ? (BaseType_t)(__VA_ARGS__ + 0) : pdTRUE )
