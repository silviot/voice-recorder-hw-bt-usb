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

/* Task priorities (higher = more urgent). configMAX_PRIORITIES = 10. Idle = 0.
 * Bluedroid derives its own task priorities from configMAX_PRIORITIES (hciT = MAX-3, BTU = MAX-5, BTC = MAX-6);
 * the values below mirror them so a change of configMAX_PRIORITIES is caught by the static asserts in rtos_port.c.
 * See docs/BLUETOOTH_BLUEDROID_PLAN.md section 3. */
#define OPENFW_PRIO_APP      1u   /* legacy super-loop: UI, recorder, player, USB poll, ALL filesystem, XREC session */
#define OPENFW_PRIO_BT_BTC   4u   /* Bluedroid BTC (profile callbacks)                    = configMAX_PRIORITIES - 6 */
#define OPENFW_PRIO_BT_BTU   5u   /* Bluedroid BTU                                        = configMAX_PRIORITIES - 5 */
#define OPENFW_PRIO_BT_DLV   6u   /* bt_dlv: controller RX rings -> host callback (may block) */
#define OPENFW_PRIO_BT_ALARM 7u   /* bt_alarm deadline service (never blocks except waiting for work) */
#define OPENFW_PRIO_BT_HCI   7u   /* Bluedroid hciT                                       = configMAX_PRIORITIES - 3 */
#define OPENFW_PRIO_BT_CTRL  8u   /* bt_ctrl: sole owner of RW-BT ROM/EM, never blocks on the host */
/* priority 9 = FreeRTOS timer daemon (configTIMER_TASK_PRIORITY = configMAX_PRIORITIES - 1) */

#define OPENFW_APP_STACK_WORDS  4096u  /* 16 KiB: replaces the old MSP super-loop stack */

typedef int (*openfw_rtos_entry_t)(void);

/* Initialise heap regions, create the app task running `entry`, start the scheduler.
 * Never returns. If `entry` returns, the board is rebooted (as before). */
void openfw_rtos_start(openfw_rtos_entry_t entry) __attribute__((noreturn));

/* Called from SysTick_Handler (startup.c) on every 1 ms tick. Cheap no-op until scheduler runs. */
void openfw_rtos_tick(void);

/* Block the calling task until the next tick or a notification (replaces task-context WFI). Falls back to WFI
 * before the scheduler starts or from interrupt context. Use via OPENFW_TASK_WAIT() in sl6801_regs.h. */
void openfw_rtos_task_wait(void);

/* Heap accounting for telemetry / tests. */
uint32_t openfw_rtos_heap_free(void);
uint32_t openfw_rtos_heap_min_free(void);

/* Fatal-error record (AON_BACKUP) + reboot. code: 0xF0 assert, 0xF1 malloc, 0xF2 stack overflow. */
void openfw_rtos_fatal(uint32_t code, uint32_t a, uint32_t b) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif /* OPENFW_RTOS_H */
