/*
 * Shared SDK header for standalone Cassini OpenFW SRAM experiments.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_EXP_SDK_H
#define OPENFW_EXP_SDK_H

#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw_exp.h"

#define EXP_ENTRY __attribute__((section(".text.entry"), used))

static inline void exp_log_i32(const openfw_exp_api_t *api, int32_t v)
{
    if (v < 0) {
        api->log_putc('-');
        api->log_u32((uint32_t)(-(int64_t)v));
    } else {
        api->log_u32((uint32_t)v);
    }
}

static inline void exp_log_kv_u32(const openfw_exp_api_t *api, const char *k, uint32_t v)
{
    api->log_puts(k);
    api->log_putc('=');
    api->log_u32(v);
    api->log_putc('\n');
}

static inline void exp_log_kv_hex(const openfw_exp_api_t *api, const char *k, uint32_t v)
{
    api->log_puts(k);
    api->log_putc('=');
    api->log_hex(v);
    api->log_putc('\n');
}

static inline const char *exp_key_name(key_id_t key)
{
    switch (key) {
    case KEY_NONE:     return "NONE";
    case KEY_LEFT:     return "PREV";
    case KEY_RIGHT:    return "NEXT";
    case KEY_VOL_UP:   return "VOL+";
    case KEY_VOL_DOWN: return "VOL-";
    case KEY_BACK:     return "BACK";
    case KEY_ENTER:    return "PLAY";
    default:           return "UNK";
    }
}

#endif /* OPENFW_EXP_SDK_H */
