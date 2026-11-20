/*
 * Experiment 7 (Unattended Recovery Self-Test):
 * Verifies Fault_Handler_Asm crash-dump + SysTick deadman watchdog recovery.
 *   arg == 0: Safe no-op log (default)
 *   arg == 1: Triggers UDF #0 UsageFault -> tests AON_BACKUP dump + auto-reboot
 *   arg == 2: Infinite busy loop -> tests SysTick 15s deadman timeout + auto-reboot
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    api->wdt_feed();
    if (arg == 1u) {
        api->log_puts("[exp_fault_test] FAULT_TEST TRIGGER UDF\n");
        __asm__ volatile ("dsb\nudf #0\n" ::: "memory");
    } else if (arg == 2u) {
        api->log_puts("[exp_fault_test] FAULT_TEST TRIGGER TIMEOUT\n");
        while (1) {
            __asm__ volatile ("nop");
        }
    }
    api->log_puts("[exp_fault_test] FAULT_TEST SAFE NOOP\n");
    return 0u;
}
