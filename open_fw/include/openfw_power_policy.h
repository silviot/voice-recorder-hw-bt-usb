/*
 * Wrap-safe Idle & Auto Power-Off Policy (host-tested in tests/test_core.c)
 *
 * Compares live 32-bit SysTick timestamps with signed modular subtraction so a
 * key edge stamped inside 1 kHz SysTick_Handler after a caller sampled now_ms
 * (key_ms > now_ms) clamps elapsed idle time to 0 instead of underflowing
 * unsigned subtraction to 0xFFFFFFFF ms (~49.7 days).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_POWER_POLICY_H
#define OPENFW_POWER_POLICY_H

#include <stdint.h>

static inline uint32_t openfw_idle_elapsed_ms(uint32_t now_ms,
                                                uint32_t last_activity_ms,
                                                int has_key_ms,
                                                uint32_t key_ms)
{
    uint32_t last = last_activity_ms;
    if (has_key_ms && (int32_t)(key_ms - last) > 0) {
        last = key_ms;
    }
    int32_t idle = (int32_t)(now_ms - last);
    return (idle > 0) ? (uint32_t)idle : 0u;
}

static inline int openfw_auto_poweroff_due(uint32_t limit_ms,
                                           uint32_t now_ms,
                                           uint32_t last_activity_ms,
                                           int has_key_ms,
                                           uint32_t key_ms)
{
    if (limit_ms == 0u) {
        return 0;
    }
    return openfw_idle_elapsed_ms(now_ms, last_activity_ms, has_key_ms, key_ms) >= limit_ms;
}

/* Allow the phone four quiet seconds on an open SPP link after SYNC,0. */
typedef struct {
    uint32_t since_ms;
    uint8_t timing;
} openfw_sync_idle_t;

static inline int openfw_sync_idle_due(openfw_sync_idle_t *idle, uint32_t now_ms, int completed_and_idle)
{
    if (!completed_and_idle) {
        idle->timing = 0u;
        return 0;
    }
    if (!idle->timing) {
        idle->since_ms = now_ms;
        idle->timing = 1u;
    }
    return (uint32_t)(now_ms - idle->since_ms) >= 4000u;
}

#endif /* OPENFW_POWER_POLICY_H */
