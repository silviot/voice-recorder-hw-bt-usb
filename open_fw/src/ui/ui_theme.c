/*
 * Cassini OpenFW — LVGL Design Tokens, Formatting Helpers & Toast Overlay
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "board_config.h"
#include "openfw_i18n.h"
#include "ui_theme.h"

static lv_obj_t *s_toast_pill       = NULL;
static lv_obj_t *s_toast_lbl        = NULL;
static int       s_toast_visible    = 0;
static uint16_t  s_toast_last_col   = 0u;
static char      s_toast_last_msg[64];

/* =========================================================================
 * Minimal Freestanding String Formatting Helpers (zero libc dependency)
 * ========================================================================= */
typedef struct {
    char *dst;
    int   pos;
    int   cap;
} fmt_buf_t;

static void fmt_init(fmt_buf_t *fb, char *dst, int cap)
{
    fb->dst = dst;
    fb->pos = 0;
    fb->cap = cap;
    if (dst && cap > 0) {
        dst[0] = '\0';
    }
}

static void fmt_putc(fmt_buf_t *fb, char c)
{
    if (!fb->dst || fb->pos + 1 >= fb->cap) {
        return;
    }
    fb->dst[fb->pos++] = c;
    fb->dst[fb->pos]   = '\0';
}

static void fmt_puts(fmt_buf_t *fb, const char *s)
{
    if (!s) return;
    while (*s) {
        fmt_putc(fb, *s++);
    }
}

static void fmt_put_u32(fmt_buf_t *fb, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        while (v > 0u && n < 11) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    while (n > 0) {
        fmt_putc(fb, tmp[--n]);
    }
}

static void fmt_put_2d(fmt_buf_t *fb, uint32_t v)
{
    fmt_putc(fb, (char)('0' + ((v / 10u) % 10u)));
    fmt_putc(fb, (char)('0' + (v % 10u)));
}

/* =========================================================================
 * Public Date / Time / Duration / Byte Formatters
 * ========================================================================= */
void ui_format_duration(char *dst, int dst_len, uint32_t secs)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);

    if (secs >= 3600u) {
        uint32_t hrs = secs / 3600u;
        uint32_t rem = secs % 3600u;
        fmt_put_u32(&fb, hrs);
        fmt_putc(&fb, ':');
        fmt_put_2d(&fb, rem / 60u);
        fmt_putc(&fb, ':');
        fmt_put_2d(&fb, rem % 60u);
    } else {
        fmt_put_2d(&fb, secs / 60u);
        fmt_putc(&fb, ':');
        fmt_put_2d(&fb, secs % 60u);
    }
}

void ui_format_clock(char *dst, int dst_len, uint8_t hour, uint8_t minute)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);
    if (hour > 23u)   hour   = (uint8_t)(hour % 24u);
    if (minute > 59u) minute = (uint8_t)(minute % 60u);
    fmt_put_2d(&fb, hour);
    fmt_putc(&fb, ':');
    fmt_put_2d(&fb, minute);
}

void ui_format_free_time(char *dst, int dst_len, uint32_t free_secs)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);
    uint32_t total_min = free_secs / 60u;
    uint32_t hrs       = total_min / 60u;
    uint32_t mins      = total_min % 60u;
    if (hrs > 0u) {
        fmt_put_u32(&fb, hrs);
        fmt_puts(&fb, "h ");
        fmt_put_u32(&fb, mins);
        fmt_putc(&fb, 'm');
    } else {
        fmt_put_u32(&fb, mins);
        fmt_putc(&fb, 'm');
    }
}

static int32_t civil_to_days(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t  y   = (int32_t)year - ((month <= 2u) ? 1 : 0);
    int32_t  era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t mp  = (uint32_t)month + ((month > 2u) ? (uint32_t)(-3) : 9u);
    uint32_t doy = (153u * mp + 2u) / 5u + (uint32_t)day - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static const char *weekday_from_days(int32_t days)
{
    int32_t wd = (days + 3) % 7;
    if (wd < 0) wd += 7;
    return ui_i18n_weekday(ui_i18n_get_lang(), (uint8_t)wd);
}

const char *ui_weekday_name(uint16_t year, uint8_t month, uint8_t day)
{
    uint8_t m = (month >= 1u && month <= 12u) ? month : 1u;
    uint8_t d = (day >= 1u && day <= 31u) ? day : 1u;
    return weekday_from_days(civil_to_days(year, m, d));
}

const char *ui_month_name(uint8_t month)
{
    return ui_i18n_month(ui_i18n_get_lang(), month);
}

void ui_format_date_label(char *dst, int dst_len,
                          const rtc_datetime_t *now, const rtc_datetime_t *rec)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);
    if (!rec || !rec->year) {
        fmt_puts(&fb, ui_tr(UI_STR_DATE_UNKNOWN));
        return;
    }

    uint8_t m  = (rec->month >= 1u && rec->month <= 12u) ? rec->month : 1u;
    uint8_t d  = (rec->day >= 1u && rec->day <= 31u) ? rec->day : 1u;
    uint8_t hh = (rec->hour <= 23u) ? rec->hour : (uint8_t)(rec->hour % 24u);
    uint8_t mm = (rec->minute <= 59u) ? rec->minute : (uint8_t)(rec->minute % 60u);

    if (now) {
        uint8_t nm = (now->month >= 1u && now->month <= 12u) ? now->month : 1u;
        uint8_t nd = (now->day >= 1u && now->day <= 31u) ? now->day : 1u;
        int32_t now_days = civil_to_days(now->year, nm, nd);
        int32_t rec_days = civil_to_days(rec->year, m, d);
        int32_t diff     = now_days - rec_days;

        if (diff == 0) {
            fmt_puts(&fb, ui_tr(UI_STR_TODAY)); fmt_putc(&fb, ' '); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
        if (diff == 1) {
            fmt_puts(&fb, ui_tr(UI_STR_YESTERDAY)); fmt_putc(&fb, ' '); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
        if (diff >= 2 && diff <= 6) {
            fmt_puts(&fb, weekday_from_days(rec_days));
            fmt_putc(&fb, ' '); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
        if (rec->year == now->year) {
            fmt_put_u32(&fb, d); fmt_putc(&fb, ' '); fmt_puts(&fb, ui_month_name(m));
            fmt_putc(&fb, ' '); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
    }

    fmt_put_u32(&fb, d); fmt_putc(&fb, ' '); fmt_puts(&fb, ui_month_name(m));
    fmt_putc(&fb, ' '); fmt_put_u32(&fb, rec->year);
}

void ui_format_bytes_short(char *dst, int cap, uint64_t bytes)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, cap);
    if (bytes < 1024ull) {
        fmt_put_u32(&fb, (uint32_t)bytes);
        fmt_puts(&fb, " B");
    } else if (bytes < (1024ull << 10)) {
        uint32_t kb = (uint32_t)(bytes >> 10);
        fmt_put_u32(&fb, kb);
        if (kb < 10u) {
            uint32_t tenths = (uint32_t)(((bytes & 1023ull) * 10ull) >> 10);
            fmt_putc(&fb, '.');
            fmt_put_u32(&fb, tenths);
        }
        fmt_puts(&fb, " KB");
    } else if (bytes < (1024ull << 20)) {
        uint32_t mb = (uint32_t)(bytes >> 20);
        fmt_put_u32(&fb, mb);
        if (mb < 100u) {
            uint32_t tenths = (uint32_t)((((bytes >> 10) & 1023ull) * 10ull) >> 10);
            fmt_putc(&fb, '.');
            fmt_put_u32(&fb, tenths);
        }
        fmt_puts(&fb, " MB");
    } else {
        uint32_t gb = (uint32_t)(bytes >> 30);
        uint32_t tenths = (uint32_t)((((bytes >> 20) & 1023ull) * 10ull) >> 10);
        fmt_put_u32(&fb, gb);
        fmt_putc(&fb, '.');
        fmt_put_u32(&fb, tenths);
        fmt_puts(&fb, " GB");
    }
}

const char *ui_icon_glyph(ui_icon_t icon)
{
    switch (icon) {
    case UI_ICON_PLAY:  return "\xE2\x96\xB6"; /* ▶ */
    case UI_ICON_PAUSE: return "||";
    case UI_ICON_STOP:  return "\xE2\x96\xA0"; /* ■ */
    case UI_ICON_DOT:   return "\xE2\x97\x8F"; /* ● */
    case UI_ICON_LEFT:  return "\xE2\x97\x80"; /* ◀ */
    case UI_ICON_RIGHT: return "\xE2\x96\xB6"; /* ▶ */
    case UI_ICON_UP:    return "\xE2\x96\xB2"; /* ▲ */
    case UI_ICON_DOWN:  return "\xE2\x96\xBC"; /* ▼ */
    case UI_ICON_CHECK: return "\xE2\x9C\x93"; /* ✓ */
    case UI_ICON_SPK:   return "SPK";
    case UI_ICON_HP:    return "HP";
    case UI_ICON_FLAG:  return "#";
    case UI_ICON_BOLT:  return "+";
    case UI_ICON_FILES: return "=";
    case UI_ICON_SETUP: return "*";
    case UI_ICON_BT:    return "BT";
    case UI_ICON_USB:   return "USB";
    case UI_ICON_MIC:   return "MIC";
    default:            return "";
    }
}

lv_obj_t *ui_box_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* =========================================================================
 * Toast Overlay on lv_layer_top()
 * ========================================================================= */
void ui_toast_update(const char *msg, uint16_t rgb565_col)
{
    if (!msg || msg[0] == '\0') {
        if (s_toast_pill && s_toast_visible) {
            lv_obj_add_flag(s_toast_pill, LV_OBJ_FLAG_HIDDEN);
            s_toast_visible = 0;
        }
        return;
    }

    uint16_t eff_col = rgb565_col ? rgb565_col : UI_COL_AMBER;
    size_t msg_len = lv_strlen(msg);
    if (s_toast_pill && s_toast_visible &&
        s_toast_last_col == eff_col &&
        msg_len < sizeof(s_toast_last_msg) &&
        lv_strcmp(s_toast_last_msg, msg) == 0) {
        return;
    }

    if (!s_toast_pill) {
        lv_obj_t *top = lv_layer_top();
        s_toast_pill = ui_box_create(top);
        lv_obj_set_size(s_toast_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(s_toast_pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_toast_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_toast_pill, 8, 0);
        lv_obj_set_style_pad_ver(s_toast_pill, 2, 0);
        lv_obj_set_style_max_width(s_toast_pill, 152, 0);
        lv_obj_align(s_toast_pill, LV_ALIGN_BOTTOM_MID, 0, -4);

        s_toast_lbl = lv_label_create(s_toast_pill);
        lv_obj_set_style_text_font(s_toast_lbl, &ui_font_sm, 0);
        lv_obj_set_style_max_width(s_toast_lbl, 136, 0);
        lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_DOT);
    }

    if (!s_toast_visible || s_toast_last_col != eff_col) {
        lv_color_t pcol = ui_color_from_rgb565(eff_col);
        lv_color_t tcol = (eff_col == UI_COL_RED) ? lv_color_hex(UI_HEX_TEXT) : lv_color_hex(UI_HEX_BLACK);
        lv_obj_set_style_bg_color(s_toast_pill, pcol, 0);
        lv_obj_set_style_text_color(s_toast_lbl, tcol, 0);
        s_toast_last_col = eff_col;
    }

    if (!s_toast_visible || msg_len >= sizeof(s_toast_last_msg) ||
        lv_strcmp(s_toast_last_msg, msg) != 0) {
        lv_label_set_text(s_toast_lbl, msg);
        if (msg_len < sizeof(s_toast_last_msg)) {
            lv_memcpy(s_toast_last_msg, msg, msg_len + 1u);
        } else {
            s_toast_last_msg[0] = '\0';
        }
    }

    if (!s_toast_visible) {
        lv_obj_remove_flag(s_toast_pill, LV_OBJ_FLAG_HIDDEN);
        s_toast_visible = 1;
    }
}
