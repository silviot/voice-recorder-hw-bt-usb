/*
 * Cassini OpenFW — LVGL Design Tokens, Reusable Styles & Chrome Components
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "board_config.h"
#include "ui_theme.h"

lv_style_t g_st_screen;
lv_style_t g_st_bar;
lv_style_t g_st_tile;
lv_style_t g_st_tile_focus;
lv_style_t g_st_row;
lv_style_t g_st_row_focus;
lv_style_t g_st_pill;
lv_style_t g_st_track;
lv_style_t g_st_ind_cyan;
lv_style_t g_st_ind_red;
lv_style_t g_st_ind_green;
lv_style_t g_st_ind_white;

static int       s_theme_inited = 0;
static lv_obj_t *s_toast_pill   = NULL;
static lv_obj_t *s_toast_lbl    = NULL;

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

void ui_format_date_label(char *dst, int dst_len,
                          const rtc_datetime_t *now, const rtc_datetime_t *rec)
{
    static const char *const WDAYS[7]   = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const char *const MONTHS[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);
    if (!rec) return;

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
            fmt_puts(&fb, "Today "); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
        if (diff == 1) {
            fmt_puts(&fb, "Yest. "); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
        if (diff >= 2 && diff <= 6) {
            int32_t wd = (rec_days + 3) % 7;
            if (wd < 0) wd += 7;
            fmt_puts(&fb, WDAYS[wd]); fmt_putc(&fb, ' '); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
        if (rec->year == now->year) {
            fmt_put_u32(&fb, d); fmt_putc(&fb, ' '); fmt_puts(&fb, MONTHS[m - 1u]);
            fmt_putc(&fb, ' '); fmt_put_2d(&fb, hh); fmt_putc(&fb, ':'); fmt_put_2d(&fb, mm);
            return;
        }
    }

    fmt_put_u32(&fb, d); fmt_putc(&fb, ' '); fmt_puts(&fb, MONTHS[m - 1u]);
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

/* =========================================================================
 * Theme Initialization
 * ========================================================================= */
static void style_reset_box(lv_style_t *st)
{
    lv_style_init(st);
    lv_style_set_border_width(st, 0);
    lv_style_set_outline_width(st, 0);
    lv_style_set_shadow_width(st, 0);
    lv_style_set_pad_all(st, 0);
    lv_style_set_radius(st, 0);
}

void ui_theme_init(void)
{
    if (s_theme_inited) {
        return;
    }
    s_theme_inited = 1;

    style_reset_box(&g_st_screen);
    lv_style_set_bg_color(&g_st_screen, lv_color_hex(UI_HEX_BG));
    lv_style_set_bg_opa(&g_st_screen, LV_OPA_COVER);
    lv_style_set_text_color(&g_st_screen, lv_color_hex(UI_HEX_TEXT));
    lv_style_set_text_font(&g_st_screen, &ui_font_sm);

    style_reset_box(&g_st_bar);
    lv_style_set_bg_color(&g_st_bar, lv_color_hex(UI_HEX_BAR));
    lv_style_set_bg_opa(&g_st_bar, LV_OPA_COVER);
    lv_style_set_pad_left(&g_st_bar, 3);
    lv_style_set_pad_right(&g_st_bar, 3);

    style_reset_box(&g_st_tile);
    lv_style_set_bg_color(&g_st_tile, lv_color_hex(UI_HEX_TILE));
    lv_style_set_bg_opa(&g_st_tile, LV_OPA_COVER);
    lv_style_set_radius(&g_st_tile, 6);

    style_reset_box(&g_st_tile_focus);
    lv_style_set_bg_color(&g_st_tile_focus, lv_color_hex(UI_HEX_PILL));
    lv_style_set_bg_opa(&g_st_tile_focus, LV_OPA_COVER);
    lv_style_set_radius(&g_st_tile_focus, 6);

    style_reset_box(&g_st_row);
    lv_style_set_bg_opa(&g_st_row, LV_OPA_TRANSP);
    lv_style_set_radius(&g_st_row, 8);
    lv_style_set_pad_left(&g_st_row, 6);
    lv_style_set_pad_right(&g_st_row, 5);

    style_reset_box(&g_st_row_focus);
    lv_style_set_bg_color(&g_st_row_focus, lv_color_hex(UI_HEX_PILL));
    lv_style_set_bg_opa(&g_st_row_focus, LV_OPA_COVER);
    lv_style_set_radius(&g_st_row_focus, 8);
    lv_style_set_pad_left(&g_st_row_focus, 6);
    lv_style_set_pad_right(&g_st_row_focus, 5);

    style_reset_box(&g_st_pill);
    lv_style_set_bg_color(&g_st_pill, lv_color_hex(UI_HEX_TILE));
    lv_style_set_bg_opa(&g_st_pill, LV_OPA_COVER);
    lv_style_set_radius(&g_st_pill, 6);
    lv_style_set_pad_left(&g_st_pill, 5);
    lv_style_set_pad_right(&g_st_pill, 5);

    style_reset_box(&g_st_track);
    lv_style_set_bg_color(&g_st_track, lv_color_hex(UI_HEX_OFF));
    lv_style_set_bg_opa(&g_st_track, LV_OPA_COVER);
    lv_style_set_radius(&g_st_track, LV_RADIUS_CIRCLE);

    style_reset_box(&g_st_ind_cyan);
    lv_style_set_bg_color(&g_st_ind_cyan, lv_color_hex(UI_HEX_CYAN));
    lv_style_set_bg_opa(&g_st_ind_cyan, LV_OPA_COVER);
    lv_style_set_radius(&g_st_ind_cyan, LV_RADIUS_CIRCLE);

    style_reset_box(&g_st_ind_red);
    lv_style_set_bg_color(&g_st_ind_red, lv_color_hex(UI_HEX_RED));
    lv_style_set_bg_opa(&g_st_ind_red, LV_OPA_COVER);
    lv_style_set_radius(&g_st_ind_red, LV_RADIUS_CIRCLE);

    style_reset_box(&g_st_ind_green);
    lv_style_set_bg_color(&g_st_ind_green, lv_color_hex(UI_HEX_GREEN));
    lv_style_set_bg_opa(&g_st_ind_green, LV_OPA_COVER);
    lv_style_set_radius(&g_st_ind_green, LV_RADIUS_CIRCLE);

    style_reset_box(&g_st_ind_white);
    lv_style_set_bg_color(&g_st_ind_white, lv_color_hex(UI_HEX_TEXT));
    lv_style_set_bg_opa(&g_st_ind_white, LV_OPA_COVER);
    lv_style_set_radius(&g_st_ind_white, LV_RADIUS_CIRCLE);
}

lv_obj_t *ui_box_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* =========================================================================
 * Reusable Status Bar Widget
 * ========================================================================= */
void ui_bar_create(lv_obj_t *parent, ui_bar_widget_t *bar)
{
    bar->root = ui_box_create(parent);
    lv_obj_add_style(bar->root, &g_st_bar, 0);
    lv_obj_set_size(bar->root, BOARD_DISP_HOR_RES, BOARD_UI_BAR_H);
    lv_obj_set_pos(bar->root, 0, 0);
    lv_obj_set_flex_flow(bar->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar->root, 3, 0);

    bar->left_icon = lv_label_create(bar->root);
    lv_obj_set_style_text_font(bar->left_icon, &ui_font_sm, 0);
    lv_label_set_text(bar->left_icon, "");

    bar->left_lbl = lv_label_create(bar->root);
    lv_obj_set_style_text_font(bar->left_lbl, &ui_font_sm, 0);
    lv_label_set_text(bar->left_lbl, "");

    bar->left_sub = lv_label_create(bar->root);
    lv_obj_set_style_text_font(bar->left_sub, &ui_font_sm, 0);
    lv_label_set_text(bar->left_sub, "");

    /* Spacer pushes right-side items to the right edge */
    lv_obj_t *spacer = ui_box_create(bar->root);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);

    bar->right_lbl = lv_label_create(bar->root);
    lv_obj_set_style_text_font(bar->right_lbl, &ui_font_sm, 0);
    lv_label_set_text(bar->right_lbl, "");

    bar->hp_lbl = lv_label_create(bar->root);
    lv_obj_set_style_text_font(bar->hp_lbl, &ui_font_sm, 0);
    lv_label_set_text(bar->hp_lbl, "HP");

    /* Compact battery icon (14x7 + 2px nub) */
    lv_obj_t *batt_wrap = ui_box_create(bar->root);
    lv_obj_set_size(batt_wrap, 16, 7);
    lv_obj_set_flex_flow(batt_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    bar->batt_body = ui_box_create(batt_wrap);
    lv_obj_set_size(bar->batt_body, 14, 7);
    lv_obj_set_style_border_width(bar->batt_body, 1, 0);
    lv_obj_set_style_border_color(bar->batt_body, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_set_style_radius(bar->batt_body, 1, 0);
    lv_obj_set_style_pad_all(bar->batt_body, 1, 0);

    bar->batt_fill = ui_box_create(bar->batt_body);
    lv_obj_set_size(bar->batt_fill, 10, 3);
    lv_obj_set_style_bg_opa(bar->batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar->batt_fill, lv_color_hex(UI_HEX_GREEN), 0);

    bar->batt_nub = ui_box_create(batt_wrap);
    lv_obj_set_size(bar->batt_nub, 2, 3);
    lv_obj_set_style_bg_opa(bar->batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar->batt_nub, lv_color_hex(UI_HEX_DIM), 0);
}

void ui_bar_update(ui_bar_widget_t *bar, const ui_status_bar_t *sb)
{
    lv_color_t bg = ui_color_from_rgb565(sb->bg_col);
    lv_color_t fg = ui_color_from_rgb565(sb->fg_col);
    lv_obj_set_style_bg_color(bar->root, bg, 0);

    if (sb->left_icon != UI_ICON_NONE) {
        lv_obj_remove_flag(bar->left_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(bar->left_icon, ui_icon_glyph(sb->left_icon));
        lv_obj_set_style_text_color(bar->left_icon, ui_color_from_rgb565(sb->left_icon_col), 0);
    } else {
        lv_obj_add_flag(bar->left_icon, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(bar->left_lbl, sb->left_text ? sb->left_text : "");
    lv_obj_set_style_text_color(bar->left_lbl, fg, 0);

    if (sb->left_sub && sb->left_sub[0] != '\0') {
        lv_obj_remove_flag(bar->left_sub, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(bar->left_sub, sb->left_sub);
        lv_obj_set_style_text_color(bar->left_sub, ui_color_from_rgb565(sb->left_sub_col), 0);
    } else {
        lv_obj_add_flag(bar->left_sub, LV_OBJ_FLAG_HIDDEN);
    }

    if (sb->no_clock) {
        if (sb->right_text && sb->right_text[0] != '\0') {
            lv_obj_remove_flag(bar->right_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(bar->right_lbl, sb->right_text);
            lv_obj_set_style_text_color(bar->right_lbl, ui_color_from_rgb565(sb->right_text_col), 0);
        } else {
            lv_obj_add_flag(bar->right_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        char clk[8];
        ui_format_clock(clk, (int)sizeof(clk), sb->hour, sb->minute);
        lv_obj_remove_flag(bar->right_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(bar->right_lbl, clk);
        lv_obj_set_style_text_color(bar->right_lbl, lv_color_hex(UI_HEX_DIM), 0);
    }

    if (sb->hp_plugged && !sb->no_clock) {
        lv_obj_remove_flag(bar->hp_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(bar->hp_lbl, lv_color_hex(UI_HEX_CYAN), 0);
    } else {
        lv_obj_add_flag(bar->hp_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* Battery fill & color */
    uint8_t pct = (sb->batt_pct > 100u) ? 100u : sb->batt_pct;
    int32_t fw  = (int32_t)((10u * pct + 50u) / 100u);
    if (pct > 0u && fw < 1) fw = 1;
    lv_color_t bcol = sb->charging ? lv_color_hex(UI_HEX_GREEN) :
                      (pct <= 15u ? lv_color_hex(UI_HEX_RED) :
                       (pct <= 30u ? lv_color_hex(UI_HEX_AMBER) : lv_color_hex(UI_HEX_TEXT)));
    lv_color_t ocol = (sb->fg_col == UI_COL_BLACK) ? lv_color_hex(UI_HEX_BLACK) : lv_color_hex(UI_HEX_DIM);
    lv_obj_set_style_border_color(bar->batt_body, ocol, 0);
    lv_obj_set_style_bg_color(bar->batt_nub, ocol, 0);
    lv_obj_set_width(bar->batt_fill, fw);
    lv_obj_set_style_bg_color(bar->batt_fill, bcol, 0);
}

/* =========================================================================
 * Reusable Footer Hint Bar
 * ========================================================================= */
lv_obj_t *ui_footer_create(lv_obj_t *parent, lv_color_t bg)
{
    lv_obj_t *f = ui_box_create(parent);
    lv_obj_set_size(f, BOARD_DISP_HOR_RES, BOARD_UI_FOOTER_H);
    lv_obj_set_pos(f, 0, BOARD_DISP_VER_RES - BOARD_UI_FOOTER_H);
    lv_obj_set_style_bg_color(f, bg, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f, 3, 0);
    return f;
}

void ui_footer_set_hints(lv_obj_t *footer, const ui_hint_t *hints, int count)
{
    lv_obj_clean(footer);
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            lv_obj_t *sep = lv_label_create(footer);
            lv_obj_set_style_text_font(sep, &ui_font_sm, 0);
            lv_obj_set_style_text_color(sep, lv_color_hex(UI_HEX_FAINT), 0);
            lv_label_set_text(sep, "\xC2\xB7");
        }
        if (hints[i].icon != UI_ICON_NONE) {
            lv_obj_t *ic = lv_label_create(footer);
            lv_obj_set_style_text_font(ic, &ui_font_sm, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(UI_HEX_TEXT), 0);
            lv_label_set_text(ic, ui_icon_glyph(hints[i].icon));
        }
        if (hints[i].icon2 != UI_ICON_NONE) {
            lv_obj_t *ic2 = lv_label_create(footer);
            lv_obj_set_style_text_font(ic2, &ui_font_sm, 0);
            lv_obj_set_style_text_color(ic2, lv_color_hex(UI_HEX_TEXT), 0);
            lv_label_set_text(ic2, ui_icon_glyph(hints[i].icon2));
        }
        if (hints[i].key && hints[i].key[0] != '\0') {
            lv_obj_t *kl = lv_label_create(footer);
            lv_obj_set_style_text_font(kl, &ui_font_sm, 0);
            lv_obj_set_style_text_color(kl, lv_color_hex(UI_HEX_TEXT), 0);
            lv_label_set_text(kl, hints[i].key);
        }
        if (hints[i].action && hints[i].action[0] != '\0') {
            lv_obj_t *al = lv_label_create(footer);
            lv_obj_set_style_text_font(al, &ui_font_sm, 0);
            lv_obj_set_style_text_color(al, lv_color_hex(UI_HEX_DIM), 0);
            lv_label_set_text(al, hints[i].action);
        }
    }
}

/* =========================================================================
 * Reusable 8-Segment Vertical VU Meter
 * ========================================================================= */
void ui_vmeter_create(lv_obj_t *parent, ui_vmeter_widget_t *vm, int32_t w, int32_t h)
{
    vm->root = ui_box_create(parent);
    lv_obj_set_size(vm->root, w, h);
    lv_obj_set_flex_flow(vm->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vm->root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int32_t seg_h = (h >= 16) ? ((h - 7) / 8) : (h / 8);
    if (seg_h < 1) seg_h = 1;

    /* Create top-to-bottom (seg 7 down to seg 0) */
    for (int i = 7; i >= 0; i--) {
        lv_obj_t *s = ui_box_create(vm->root);
        lv_obj_set_size(s, w, seg_h);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(UI_HEX_OFF), 0);
        lv_obj_set_style_radius(s, 1, 0);
        vm->segs[i] = s;
    }
}

void ui_vmeter_update(ui_vmeter_widget_t *vm, uint8_t pct, uint8_t peak_pct, int clip_latch)
{
    if (!vm->root) return;
    if (pct > 100u)      pct = 100u;
    if (peak_pct > 100u) peak_pct = 100u;

    int lit = (int)(((uint32_t)pct * 8u + 50u) / 100u);
    if (pct > 0u && lit == 0) lit = 1;
    if (lit > 8) lit = 8;

    int peak_idx = -1;
    if (peak_pct > 0u) {
        peak_idx = (int)(((uint32_t)peak_pct * 8u + 50u) / 100u) - 1;
        if (peak_idx < 0) peak_idx = 0;
        if (peak_idx > 7) peak_idx = 7;
    }

    for (int i = 0; i < 8; i++) {
        uint32_t hex;
        if (i == 7 && clip_latch) {
            hex = UI_HEX_RED;
        } else if (i == peak_idx) {
            hex = UI_HEX_TEXT;
        } else if (i < lit) {
            hex = (i == 7) ? UI_HEX_RED : (i >= 5 ? UI_HEX_AMBER : UI_HEX_GREEN);
        } else {
            hex = UI_HEX_OFF;
        }
        lv_obj_set_style_bg_color(vm->segs[i], lv_color_hex(hex), 0);
    }
}

/* =========================================================================
 * Toast Overlay on lv_layer_top()
 * ========================================================================= */
void ui_toast_update(const char *msg, uint16_t rgb565_col)
{
    if (!msg || msg[0] == '\0') {
        if (s_toast_pill) {
            lv_obj_add_flag(s_toast_pill, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (!s_toast_pill) {
        lv_obj_t *top = lv_layer_top();
        s_toast_pill = ui_box_create(top);
        lv_obj_set_style_radius(s_toast_pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_toast_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_toast_pill, 8, 0);
        lv_obj_set_style_pad_ver(s_toast_pill, 2, 0);
        lv_obj_align(s_toast_pill, LV_ALIGN_BOTTOM_MID, 0, -4);

        s_toast_lbl = lv_label_create(s_toast_pill);
        lv_obj_set_style_text_font(s_toast_lbl, &ui_font_sm, 0);
    }

    lv_color_t pcol = ui_color_from_rgb565(rgb565_col ? rgb565_col : UI_COL_AMBER);
    lv_color_t tcol = (rgb565_col == UI_COL_RED) ? lv_color_hex(UI_HEX_TEXT) : lv_color_hex(UI_HEX_BLACK);
    lv_obj_set_style_bg_color(s_toast_pill, pcol, 0);
    lv_obj_set_style_text_color(s_toast_lbl, tcol, 0);
    lv_label_set_text(s_toast_lbl, msg);
    lv_obj_remove_flag(s_toast_pill, LV_OBJ_FLAG_HIDDEN);
}
