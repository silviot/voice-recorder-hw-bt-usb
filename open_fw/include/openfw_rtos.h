/*
 * open_fw RTOS glue (FreeRTOS-Kernel on CPU0). Only compiled with OPENFW_RTOS=1.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_RTOS_H
#define OPENFW_RTOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Task priorities (higher = more urgent). configMAX_PRIORITIES = 8. Idle = 0. */
#define OPENFW_PRIO_APP      1u   /* legacy super-loop (UI/recorder/player/USB poll) */
#define OPENFW_PRIO_BT_APP   3u   /* BT profile callbacks (BTC) */
#define OPENFW_PRIO_BT_STACK 4u   /* Bluedroid BTU */
#define OPENFW_PRIO_BT_HCI   5u   /* HCI host / controller glue bottom half */
/* priority 6 = FreeRTOS timer task (configTIMER_TASK_PRIORITY) */

#define OPENFW_APP_STACK_WORDS  4096u  /* 16 KiB: replaces the old MSP super-loop stack */

typedef int (*openfw_rtos_entry_t)(void);

/* Initialise heap regions, create the app task running `entry`, start the scheduler.
 * Never returns. If `entry` returns, the board is rebooted (as before). */
void openfw_rtos_start(openfw_rtos_entry_t entry) __attribute__((noreturn));

/* Called from SysTick_Handler (startup.c) on every 1 ms tick. Cheap no-op until scheduler runs. */
void openfw_rtos_tick(void);

/* Heap accounting for telemetry / tests. */
uint32_t openfw_rtos_heap_free(void);
uint32_t openfw_rtos_heap_min_free(void);

/* Fatal-error record (AON_BACKUP) + reboot. code: 0xF0 assert, 0xF1 malloc, 0xF2 stack overflow. */
void openfw_rtos_fatal(uint32_t code, uint32_t a, uint32_t b) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif /* OPENFW_RTOS_H */
