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
        d1 = openfw_mmio_rd32(RTC_DATE_ADDR);
        t  = openfw_mmio_rd32(RTC_TIME_ADDR);
        d2 = openfw_mmio_rd32(RTC_DATE_ADDR);
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
    uint32_t wait = 100000u;
    while ((int32_t)openfw_mmio_rd32(AON_CLK_CFG_ADDR) < 0 && --wait > 0u) {
    }
    openfw_mmio_rmw32(AON_CLK_CFG_ADDR, 0x0001F000u, (0x18u << 12));
    (void)openfw_mmio_rd32(AON_CLK_STAT_ADDR);
    openfw_mmio_set32(AON_CLK_EN_ADDR, 1u << 2);
    openfw_mmio_clr32(AON_PWR_CTL0_ADDR, 1u << 28);
    openfw_mmio_clr32(AON_RTC_CTRL_ADDR, (1u << 4) | (1u << 8)); /* mode=0, HALT=0 */
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

static uint8_t rtc_weekday(uint32_t year, uint32_t month, uint32_t day)
{
    /* Kim Larsen weekday (0 = Mon .. 6 = Sun) */
    uint32_t yw = year;
    uint32_t mw = month;
    if (mw <= 2u) {
        mw += 12u;
        yw -= 1u;
    }
    return (day + 2u * mw + (3u * (mw + 1u)) / 5u +
                     yw + yw / 4u - yw / 100u + yw / 400u) % 7u;
}

void rtc_set(const rtc_datetime_t *dt)
{
    if (!dt) {
        return;
    }
    uint32_t y_full = dt->year;
    if (y_full < 2000u) {
        y_full = 2000u;
    } else if (y_full > 2127u) {
        y_full = 2127u;
    }
    uint32_t y_off   = y_full - 2000u;
    uint32_t m_val   = (dt->month >= 1u && dt->month <= 12u) ? dt->month : 1u;
    uint32_t d_val   = (dt->day >= 1u && dt->day <= 31u) ? dt->day : 1u;
    uint32_t is_leap = ((y_full % 4u == 0u && y_full % 100u != 0u) ||
                        (y_full % 400u == 0u)) ? 1u : 0u;

    uint32_t wday = rtc_weekday(y_full, m_val, d_val);

    /* Halt RTC counter across TIME + DATE writes for atomic update */
    uint32_t ctrl = openfw_mmio_rd32(AON_RTC_CTRL_ADDR);
    openfw_mmio_wr32(AON_RTC_CTRL_ADDR, ctrl | (1u << 8));
    openfw_mmio_wr32(RTC_TIME_ADDR,
                     ((wday & 0x07u) << 24)
                   | (((uint32_t)dt->hour   & 0x1Fu) << 16)
                   | (((uint32_t)dt->minute & 0x3Fu) << 8)
                   | ((uint32_t)dt->second  & 0x3Fu));
    openfw_mmio_wr32(RTC_DATE_ADDR,
                     (is_leap << 24)
                   | ((y_off & 0x7Fu) << 16)
                   | ((m_val & 0x0Fu) << 8)
                   | (d_val & 0x1Fu));
    openfw_mmio_wr32(AON_RTC_CTRL_ADDR, ctrl & ~(1u << 8));
}

uint32_t rtc_to_dos_timestamp(const rtc_datetime_t *dt)
{
    if (!dt || dt->year < 1980u || dt->month < 1u || dt->month > 12u ||
        dt->day < 1u || dt->day > 31u || dt->hour > 23u ||
        dt->minute > 59u || dt->second > 59u) {
        return 0x5C210000u; /* Valid fallback: 2026-01-01 00:00:00 */
    }
    uint32_t yr = (uint32_t)(dt->year - 1980u);
    if (yr > 127u) {
        yr = 127u; /* Clamp to 2107 instead of wrapping */
    }
    return (yr << 25)
         | ((uint32_t)dt->month << 21)
         | ((uint32_t)dt->day << 16)
         | ((uint32_t)dt->hour << 11)
         | ((uint32_t)dt->minute << 5)
         | ((uint32_t)dt->second >> 1);
}

/* File timestamps store local civil time with two-second resolution. */
rtc_datetime_t rtc_from_dos_timestamp(uint32_t stamp)
{
    rtc_datetime_t dt = {
        .year   = 1980u + (stamp >> 25),
        .month  = (stamp >> 21) & 15u,
        .day    = (stamp >> 16) & 31u,
        .hour   = (stamp >> 11) & 31u,
        .minute = (stamp >> 5) & 63u,
        .second = (stamp & 31u) * 2u
    };
    static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (dt.month < 1 || dt.month > 12 || dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
        return (rtc_datetime_t){0};
    }
    unsigned days = month_days[dt.month - 1];
    if (dt.month == 2 && dt.year % 4 == 0 && (dt.year % 100 != 0 || dt.year % 400 == 0)) {
        days++;
    }
    if (dt.day < 1 || dt.day > days) {
        return (rtc_datetime_t){0};
    }
    dt.weekday = rtc_weekday(dt.year, dt.month, dt.day);
    return dt;
}

void rtc_set_alarm0_seconds(uint32_t seconds)
{
    openfw_mmio_wr32(RTC_ALARM0_CTRL_ADDR, 0u);
    openfw_mmio_wr32(RTC_ALARM0_VAL_ADDR, (seconds > 0u) ? (seconds - 1u) : 0u);
    openfw_mmio_wr32(RTC_ALARM0_CTRL_ADDR, 1u);
}
