/* Force-included (tests/bt_host/Makefile) into the PRISTINE FreeRTOS V11.1.0 POSIX port's utils/wait_for_event.c and
 * port.c, host harness only.
 *
 * V11.1.0 deletes a task that is not running (vTaskDelete(other): Bluedroid's osi_thread_stop() deletes its
 * suspended hciT/BTU/BTC tasks) with pthread_cancel() + event_signal(). The victim is blocked in pthread_cond_wait()
 * inside event_wait(); on cancellation glibc re-acquires the event mutex and the thread exits still holding it, so
 * the deleter's event_signal() -> pthread_mutex_lock() can block forever (a timing-dependent hang). Upstream fixed this
 * in V11.2.0 (FreeRTOS-Kernel ad4e723 "Mark mutex as robust to prevent deadlocks", issue #1217) by making the mutex
 * robust: the lock then returns EOWNERDEAD instead of blocking. This shim applies the same attribute without editing
 * the vendored file. Remove it when third_party/freertos moves to >= V11.2.0.
 * SPDX-License-Identifier: MIT */
#ifndef BT_HOST_POSIX_PORT_SHIM_H
#define BT_HOST_POSIX_PORT_SHIM_H
#include <pthread.h>

static inline int bth_robust_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    (void)a;
    pthread_mutexattr_t r;
    pthread_mutexattr_init(&r);
    pthread_mutexattr_setrobust(&r, PTHREAD_MUTEX_ROBUST);
    int e = pthread_mutex_init(m, &r);
    pthread_mutexattr_destroy(&r);
    return e;
}
#define pthread_mutex_init bth_robust_mutex_init
#endif
