/*
 * Smartlink SL6801 Bare-Metal AON Hardware RTC Driver (0x40085000)
 *
 * Implements verified unsigned-binary RTC_DATE / RTC_TIME readout/programming
 * (LEAP_YEAR bit 24, base year 2000, Kim Larsen weekday) and exFAT DOS 32-bit
 * timestamp encoding.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

void rtc_read_raw(uint32_t *date_reg, uint32_t *time_reg)
{
    uint32_t d1, t, d2;
    do {
        d1 = RTC_DATE;
        t  = RTC_TIME;
        d2 = RTC_DATE;
    } while (d1 != d2);
    if (date_reg) {
        *date_reg = d1;
    }
    if (time_reg) {
        *time_reg = t;
    }
}

void rtc_ensure_running(void)
{
    while ((int32_t)AON_CLK_CFG < 0) {
    }
    AON_CLK_CFG = (AON_CLK_CFG & ~0x0001F000u) | (0x18u << 12);
    (void)AON_CLK_STAT;
    AON_CLK_EN   |= (1u << 2);
    AON_PWR_CTL0 &= ~(1u << 28);
    AON_RTC_CTRL &= ~((1u << 4) | (1u << 8)); /* mode=0, HALT=0 */
}

void rtc_get(rtc_datetime_t *dt)
{
    if (!dt) {
        return;
    }
    uint32_t d = 0u, t = 0u;
    rtc_read_raw(&d, &t);

    dt->year    = (uint16_t)(2000u + ((d >> 16) & 0x7Fu));
    dt->month   = (uint8_t)((d >> 8) & 0x0Fu);
    dt->day     = (uint8_t)(d & 0x1Fu);
    dt->weekday = (uint8_t)((t >> 24) & 0x07u);
    dt->hour    = (uint8_t)((t >> 16) & 0x1Fu);
    dt->minute  = (uint8_t)((t >> 8) & 0x3Fu);
    dt->second  = (uint8_t)(t & 0x3Fu);
}

void rtc_set(const rtc_datetime_t *dt)
{
    if (!dt) {
        return;
    }
    uint32_t y_full = dt->year;
    uint32_t y_off  = (y_full >= 2000u) ? (y_full - 2000u) : 0u;
    uint32_t is_leap = ((y_full % 4u == 0u && y_full % 100u != 0u) ||
                        (y_full % 400u == 0u)) ? 1u : 0u;

    /* Kim Larsen weekday (0 = Mon .. 6 = Sun) */
    uint32_t yw = y_full;
    uint32_t mw = dt->month;
    if (mw <= 2u) {
        mw += 12u;
        yw -= 1u;
    }
    uint32_t wday = (dt->day + 2u * mw + (3u * (mw + 1u)) / 5u +
                     yw + yw / 4u - yw / 100u + yw / 400u) % 7u;

    RTC_TIME = ((wday & 0x07u) << 24)
             | (((uint32_t)dt->hour   & 0x1Fu) << 16)
             | (((uint32_t)dt->minute & 0x3Fu) << 8)
             | ((uint32_t)dt->second  & 0x3Fu);
    RTC_DATE = (is_leap << 24)
             | ((y_off & 0x7Fu) << 16)
             | (((uint32_t)dt->month & 0x0Fu) << 8)
             | ((uint32_t)dt->day    & 0x1Fu);
}

uint32_t rtc_to_dos_timestamp(const rtc_datetime_t *dt)
{
    if (!dt || dt->year < 1980u || dt->month < 1u || dt->month > 12u ||
        dt->day < 1u || dt->day > 31u || dt->hour > 23u ||
        dt->minute > 59u || dt->second > 59u) {
        return 0x5C210000u; /* Valid fallback: 2026-01-01 00:00:00 */
    }
    uint32_t yr = (uint32_t)(dt->year - 1980u) & 0x7Fu;
    return (yr << 25)
         | ((uint32_t)dt->month << 21)
         | ((uint32_t)dt->day << 16)
         | ((uint32_t)dt->hour << 11)
         | ((uint32_t)dt->minute << 5)
         | ((uint32_t)dt->second >> 1);
}

void rtc_set_alarm0_seconds(uint32_t seconds)
{
    RTC_ALARM0_CTRL = 0u;
    RTC_ALARM0_VAL  = (seconds > 0u) ? (seconds - 1u) : 0u;
    RTC_ALARM0_CTRL = 1u;
}
