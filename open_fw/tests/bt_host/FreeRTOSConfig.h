/*
 * FreeRTOS-Kernel V11.1.0 configuration for the HOST harness (make test-bt-host): the same kernel sources as the
 * firmware (third_party/freertos) on the POSIX port (third_party/freertos/portable/ThirdParty/GCC/Posix).
 *
 * Mirrors src/rtos/FreeRTOSConfig.h wherever Bluedroid or bt_port can observe the difference: 1 kHz tick, 10
 * priorities (Bluedroid derives hciT/BTU/BTC from configMAX_PRIORITIES), preemption + time slicing, mutexes,
 * recursive mutexes, counting semaphores, notifications, static + dynamic allocation, timers, same INCLUDE_ set.
 * Host-only differences: no Cortex-M interrupt priorities, no stack-overflow check (each task runs on its own pthread
 * stack, not on the FreeRTOS stack buffer), assert -> abort with a message (sanitizers print the stack), the kernel's
 * own idle/timer task memory (configKERNEL_PROVIDED_STATIC_MEMORY).
 * SPDX-License-Identifier: MIT
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern void bth_assert_failed(const char *file, int line);

/* TSan build (make test-bt-host-tsan): cooperative. The POSIX port switches tasks from inside its SIGALRM handler,
 * which ThreadSanitizer (it defers async signals) deadlocks on; without preemption the handler only counts ticks and
 * every switch happens at a blocking FreeRTOS call, where TSan sees the port's mutex/condvar hand-over. */
#ifndef BTH_PREEMPTION
#define BTH_PREEMPTION 1
#endif
#define configUSE_PREEMPTION                     BTH_PREEMPTION
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       ( 96000000U )
#define configTICK_RATE_HZ                       ( 1000U )
#define configMAX_PRIORITIES                     ( 10U )
#define configMINIMAL_STACK_SIZE                 ( 2048U )  /* words; the POSIX port raises it to PTHREAD_STACK_MIN */
#define configMAX_TASK_NAME_LEN                  ( 12U )
#define configTICK_TYPE_WIDTH_IN_BITS            TICK_TYPE_WIDTH_64_BITS
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_TASK_NOTIFICATIONS             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_QUEUE_SETS                     0
#define configQUEUE_REGISTRY_SIZE                0
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configUSE_APPLICATION_TASK_TAG           0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  1

#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configKERNEL_PROVIDED_STATIC_MEMORY      1
#define configAPPLICATION_ALLOCATED_HEAP         0

#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             1   /* counts only, like the BT=1 firmware (plan M1) */
#define configCHECK_FOR_STACK_OVERFLOW           0
#define configUSE_TRACE_FACILITY                 1
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configGENERATE_RUN_TIME_STATS            0
#define configASSERT( x )                        do { if ( ( x ) == 0 ) { bth_assert_failed( __FILE__, __LINE__ ); } } while ( 0 )

#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                ( configMAX_PRIORITIES - 1U )
#define configTIMER_QUEUE_LENGTH                 16
#define configTIMER_TASK_STACK_DEPTH             ( 4096U )

#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_xResumeFromISR                   1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xTaskAbortDelay                  1
#define INCLUDE_xTaskGetHandle                   1
#define INCLUDE_xSemaphoreGetMutexHolder         1
#define INCLUDE_xEventGroupSetBitFromISR         1

#endif /* FREERTOS_CONFIG_H */
