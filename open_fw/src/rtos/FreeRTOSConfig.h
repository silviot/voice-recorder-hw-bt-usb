/*
 * FreeRTOS-Kernel V11.1.0 configuration for the SL6801 open_fw (CPU0, Cortex-M4F).
 *
 * Design rules (see docs/BLUETOOTH_BLUEDROID_PLAN.md):
 *  - SysTick stays owned by startup.c (1 ms, g_systick_ms, keys_scan_tick). Its handler
 *    calls xPortSysTickHandler() when the scheduler runs; vPortSetupTimerInterrupt() starts it.
 *  - Audio DMA / USB / SD stay ISR-driven; only BT tasks and the app task run under the kernel.
 *  - ISRs that call *FromISR APIs must be configured at priority >= configMAX_SYSCALL_INTERRUPT_PRIORITY
 *    (numerically). ISRs left at priority 0 are never masked and MUST NOT call FreeRTOS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t g_openfw_cpu_hz;
extern void     openfw_rtos_assert_failed(const char *file, int line);
extern uint64_t us_timer_now(void);

#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       ( g_openfw_cpu_hz )
#define configTICK_RATE_HZ                       ( 1000U )
#define configMAX_PRIORITIES                     ( 10U )
#define configMINIMAL_STACK_SIZE                 ( 128U )   /* words */
#define configMAX_TASK_NAME_LEN                  ( 12U )
#define configUSE_16_BIT_TICKS                   0
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

/* Memory: heap_5 over linker-provided SRAM regions (see rtos_port.c / sl6801_firm.ld .rtos_heap) */
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configAPPLICATION_ALLOCATED_HEAP         0

/* Hooks & diagnostics */
#define configUSE_IDLE_HOOK                      1
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             1
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_TRACE_FACILITY                 1
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configGENERATE_RUN_TIME_STATS            1
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() do { } while (0)
#define portGET_RUN_TIME_COUNTER_VALUE()         ( (uint32_t)us_timer_now() )
#define configASSERT( x )                        do { if ( ( x ) == 0 ) { openfw_rtos_assert_failed( __FILE__, __LINE__ ); } } while ( 0 )

/* Software timers (daemon task) */
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                ( configMAX_PRIORITIES - 1U )
#define configTIMER_QUEUE_LENGTH                 16
#define configTIMER_TASK_STACK_DEPTH             ( 384U )

/* Interrupt priorities (top bits only; the port auto-detects implemented bits at start-up) */
#define configPRIO_BITS                          4   /* SL6801: 4 implemented NVIC priority bits (mask 0xF0), measured on silicon (Exp 1, 2026-11-24) */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY  ( ( 1U << configPRIO_BITS ) - 1U )
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 8U  /* = 0x80 (unchanged); ISRs at 0x80..0xF0 may call FromISR APIs */
#define configKERNEL_INTERRUPT_PRIORITY          ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/* Optional API */
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

/* Provided by startup.c / rtos_port.c (vector table entries) */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
/* xPortSysTickHandler is called from SysTick_Handler (startup.c), not installed directly */

#endif /* FREERTOS_CONFIG_H */
