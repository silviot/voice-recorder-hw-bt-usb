/* Minimal FreeRTOS portmacro.h for HOST compilation of Bluedroid headers in unit tests (types only; no scheduler).
 * Used by test-bt-ctrl to compile Bluedroid's hci_packet_parser.c on x86-64. Not a runnable port.
 * SPDX-License-Identifier: MIT */
#ifndef OPENFW_TEST_PORTMACRO_H
#define OPENFW_TEST_PORTMACRO_H
#include <stddef.h>
#include <stdint.h>
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef size_t StackType_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portSTACK_TYPE size_t
#define portBASE_TYPE long
#define portBYTE_ALIGNMENT 8
#define portSTACK_GROWTH (-1)
#define portTICK_PERIOD_MS ((TickType_t)1)
#define portTICK_TYPE_IS_ATOMIC 1
#define portYIELD()
#define portNOP()
#define portDISABLE_INTERRUPTS()
#define portENABLE_INTERRUPTS()
#define portENTER_CRITICAL()
#define portEXIT_CRITICAL()
#define portSET_INTERRUPT_MASK_FROM_ISR() 0
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x) ((void)(x))
#define portYIELD_FROM_ISR(x) ((void)(x))
#define portEND_SWITCHING_ISR(x) ((void)(x))
#define portTASK_FUNCTION_PROTO(f, p) void f(void *p)
#define portTASK_FUNCTION(f, p) void f(void *p)
#endif
