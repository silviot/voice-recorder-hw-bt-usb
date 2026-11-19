/*
 * Smartlink SL6801 Bare-Metal AON Hardware RTC Driver (0x40085000)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"

typedef struct {
    uint16_t year;    /* 2000..2099 */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint8_t  weekday; /* 0..6 */
    uint8_t  hour;    /* 0..23 */
    uint8_t  minute;  /* 0..59 */
    uint8_t  second;  /* 0..59 */
} rtc_datetime_t;

void rtc_read_raw(uint32_t *date_reg, uint32_t *time_reg)
{
    /* Re-read if time rolled over between register reads */
    uint32_t d1, t, d2;
    do {
        d1 = RTC_DATE;
        t  = RTC_TIME;
        d2 = RTC_DATE;
    } while (d1 != d2);
    if (date_reg) *date_reg = d1;
    if (time_reg) *time_reg = t;
}

void rtc_set_alarm0_seconds(uint32_t seconds)
{
    RTC_ALARM0_CTRL = 0u;
    RTC_ALARM0_VAL  = (seconds > 0u) ? (seconds - 1u) : 0u;
    RTC_ALARM0_CTRL = 1u;
}
