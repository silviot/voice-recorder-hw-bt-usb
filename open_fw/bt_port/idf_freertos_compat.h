/* ESP-IDF-only FreeRTOS API used by Bluedroid / esp_ringbuf, mapped onto upstream FreeRTOS-Kernel.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include <task.h>
#include <queue.h>
#include <semphr.h>

#define tskNO_AFFINITY 0x7FFFFFFF
#define portNUM_PROCESSORS 1
#define ESP_STATIC_ANALYZER_CHECK(cond, ret) ((void)0)

/* Single-core CPU0 build. IDF "spinlock" critical sections become kernel critical sections
 * (task context) or BASEPRI masking with the saved state kept inside the lock object (ISR context). */
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
#define xPortInIsrContext() ( ( ( *(volatile uint32_t *)0xE000ED04u ) & 0x1FFu ) != 0u )
#define xPortCanYield() (!xPortInIsrContext())

/* IDF passes stack size in BYTES; upstream xTaskCreate takes WORDS. Convert at this boundary. */
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                                 void *arg, UBaseType_t prio, TaskHandle_t *h, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, (configSTACK_DEPTH_TYPE)((stack_bytes + 3u) / 4u), arg, prio, h);
}

#define xPortGetCoreID() 0
/* IDF allows portYIELD_FROM_ISR() with no argument (= always yield). Upstream requires the flag. */
#undef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR(...) portEND_SWITCHING_ISR( (sizeof(#__VA_ARGS__) > 1) ? (BaseType_t)(__VA_ARGS__ + 0) : pdTRUE )
