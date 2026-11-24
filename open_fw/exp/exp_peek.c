/*
 * Read 24 words at address `arg` (e.g. a symbol address from the ELF) into the log. Read-only.
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(arg & ~3u);
    api->log_puts("[exp_peek] ");
    api->log_hex(arg);
    api->log_putc('\n');
    for (uint32_t i = 0u; i < 24u; i++) {
        api->log_hex(p[i]);
        api->log_putc((i % 6u) == 5u ? '\n' : ' ');
    }
    return p[0];
}
