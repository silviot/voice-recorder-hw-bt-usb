/* main() and the step watchdog of the host harness (make test-bt-host). Built with the kernel flags (system
 * <pthread.h>, not bt_port's empty JV shim). The watchdog is a plain pthread with every signal blocked: it never runs
 * FreeRTOS code and never receives the POSIX port's SIGALRM tick or SIGUSR1 resume signal. When a step exceeds its
 * deadline it prints the step, a bt_ctrl/Bluedroid state line, and exits 124.
 * SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#define APP_PRIORITY 1u   /* OPENFW_PRIO_APP: the harness drives Bluedroid from the app task's priority */

extern int s_argc;
extern char **s_argv;
void bth_test_task(void *arg);
void bth_hang_report(void);

static const char *s_step = "start";
static uint64_t s_deadline_us;
static int s_step_active;

static uint64_t mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void bth_step(const char *what, uint32_t timeout_ms)
{
    __atomic_store_n(&s_step, what, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_deadline_us, mono_us() + (uint64_t)timeout_ms * 1000u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_step_active, 1, __ATOMIC_SEQ_CST);
}

void bth_step_done(void)
{
    __atomic_store_n(&s_step_active, 0, __ATOMIC_SEQ_CST);
}

static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(50000);
        if (__atomic_load_n(&s_step_active, __ATOMIC_SEQ_CST) &&
            mono_us() > __atomic_load_n(&s_deadline_us, __ATOMIC_SEQ_CST)) {
            fprintf(stderr, "HANG: step '%s' exceeded its deadline\n", __atomic_load_n(&s_step, __ATOMIC_SEQ_CST));
            bth_hang_report();
            fflush(stderr);
            fflush(stdout);
            _exit(124);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    s_argc = argc;
    s_argv = argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    pthread_t wd;
    int e = pthread_create(&wd, NULL, watchdog, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (e != 0) {
        return 2;
    }
    if (xTaskCreate(bth_test_task, "test", 16384u, NULL, APP_PRIORITY, NULL) != pdPASS) {
        return 2;
    }
    vTaskStartScheduler();
    return 3;
}
