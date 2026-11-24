/*
 * Cassini OpenFW — EEZ Studio LVGL Presenter / Data Binder
 *
 * All screen layouts, widget positions, sizes, and visual styles are defined
 * visually in open_fw/cassini.eez-project and generated into src/ui/eez/.
 *
 * This file contains ONLY the clean, layout-free binding from firmware state
 * (ui_model_t) onto EEZ Studio's generated `objects.*` LVGL widget handles.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "board_config.h"
#include "openfw_ui.h"
#include "ui_port.h"
#include "ui_theme.h"
#include "eez/ui.h"

static lv_image_dsc_t          s_spec_dsc;
static enum ScreensEnum        s_active_eez_scr = (enum ScreensEnum)0;
static const ui_spectrogram_t *s_last_spec_ptr  = NULL;
static uint32_t                s_last_spec_sig  = 0u;

#define TR_ROW_H         16
#define TR_TRACK_REST_Y  (-19)

static int32_t s_tr_top_line_id   = -1;
static int32_t s_tr_active_line   = -1;
static int32_t s_tr_pill_target_x = -1;
static int32_t s_tr_pill_target_y = -1;
static int32_t s_tr_pill_target_w = -1;
static int     s_tr_pill_visible  = 0;

static void anim_set_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
static void anim_set_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }
static void anim_set_w_cb(void *obj, int32_t v) { lv_obj_set_width((lv_obj_t *)obj, v); }

static void start_ease_out_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                                int32_t from, int32_t to, uint32_t dur_ms)
{
    lv_anim_delete(obj, cb);
    if (from == to) {
        cb(obj, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* =========================================================================
 * Common Formatting & Helpers
 * ========================================================================= */
static const char *sound_mode_name(uint8_t mode)
{
    if (mode == 0u) return ui_tr(UI_STR_SND_RAW);
    if (mode == 1u) return ui_tr(UI_STR_SND_CLARITY);
    return ui_tr(UI_STR_SND_BYPASS);
}

static const char *skip_mode_name(uint8_t mode)
{
    return ui_tr(mode == UI_SKIP_TRACK ? UI_STR_SKIP_TRACK : UI_STR_SKIP_SPEED);
}

static const char *screen_off_name(uint8_t idx)
{
    if (idx == 0u) return "15s";
    if (idx == 1u) return "30s";
    if (idx == 2u) return "1m";
    if (idx == 3u) return "5m";
    if (idx == 4u) return ui_tr(UI_STR_NEVER);
    return "30s";
}

static int is_generic_recording_name(const char *name)
{
    if (!name || name[0] == '\0') return 1;
    /* "REC_..." (case-insensitive) or timestamp "202..." -> show friendly date label */
    char c0 = name[0], c1 = name[1], c2 = name[2], c3 = name[3];
    if (c0 >= 'a' && c0 <= 'z') c0 = (char)(c0 - ('a' - 'A'));
    if (c1 >= 'a' && c1 <= 'z') c1 = (char)(c1 - ('a' - 'A'));
    if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - ('a' - 'A'));
    if (c0 == 'R' && c1 == 'E' && c2 == 'C' && c3 == '_') return 1;
    if (c0 >= '0' && c0 <= '9' && c1 >= '0' && c1 <= '9' &&
        c2 >= '0' && c2 <= '9' && c3 >= '0' && c3 <= '9') {
        return 1;
    }
    return 0;
}

static void format_file_title(char *dst, int cap,
                              const rtc_datetime_t *now, const ui_file_entry_t *f)
{
    if (!dst || cap <= 0) return;
    if (!f || is_generic_recording_name(f->filename)) {
        ui_format_date_label(dst, cap, now, f ? &f->dt : now);
        return;
    }
    int len = 0;
    while (f->filename[len] != '\0') len++;
    if (len >= 4 && f->filename[len - 4] == '.') {
        len -= 4;
    }
    int out = 0;
    for (int i = 0; i < len && out < cap - 1; i++) {
        char c = f->filename[i];
        if (c == '_') c = ' ';
        if (c == ' ' && (out == 0 || dst[out - 1] == ' ')) continue;
        dst[out++] = c;
    }
    dst[out] = '\0';
}

void ui_settings_defaults(ui_settings_t *cfg)
{
    cfg->lang           = (uint8_t)UI_LANG_EN;
    cfg->mic_gain       = 20u;
    cfg->sound_mode     = AUDIO_DSP_DEFAULT_PROFILE;
    cfg->channels       = 2u;
    cfg->vor_enabled    = 0u;
    cfg->brightness_idx = 3u;
    cfg->screen_off_idx = UI_SCR_OFF_30S;
    cfg->skip_mode      = UI_SKIP_SPEED;
    cfg->hp_vol         = 18u;
    cfg->spk_vol        = 18u;
}

static uint32_t hash_str_nonzero(const char *s)
{
    uint32_t h = 2166136261u;
    if (s) {
        while (*s) {
            h ^= (uint8_t)*s++;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;
}

static uint32_t spec_signature(const ui_spectrogram_t *sp)
{
    if (!sp) return 0u;
    uint32_t h = 2166136261u ^ (uint32_t)sp->col_count;
    h *= 16777619u;
    static const uint32_t sample_cols[3] = { 0u, UI_SPEC_W / 2u, UI_SPEC_W - 1u };
    for (uint32_t ci = 0u; ci < 3u; ci++) {
        uint32_t c = sample_cols[ci];
        h ^= (uint32_t)sp->markers[c];
        h *= 16777619u;
        for (uint32_t r = 0u; r < UI_SPEC_H; r++) {
            h ^= (uint32_t)sp->pixels[r][c];
            h *= 16777619u;
        }
    }
    return h;
}

/* LVGL invalidates a label even when its text is identical, and
 * LV_LABEL_LONG_DOT mutates label->text in place with "..." so comparing
 * lv_label_get_text() against the source string misfires on truncated text. */
static void set_changed_text(lv_obj_t *obj, const char *text)
{
    if (!obj) return;
    if (!text) text = "";
    if (lv_label_get_long_mode(obj) == LV_LABEL_LONG_DOT) {
        uintptr_t tag  = (uintptr_t)hash_str_nonzero(text);
        uintptr_t prev = (uintptr_t)lv_obj_get_user_data(obj);
        if (prev == tag) {
            return;
        }
        lv_obj_set_user_data(obj, (void *)tag);
        if (prev != 0u || lv_strcmp(lv_label_get_text(obj), text) != 0) {
            lv_label_set_text(obj, text);
        }
        return;
    }
    if (lv_strcmp(lv_label_get_text(obj), text) != 0) {
        lv_label_set_text(obj, text);
    }
}

static void set_changed_text_static(lv_obj_t *obj, const char *text)
{
    if (!obj || !text) return;
    if (lv_strcmp(lv_label_get_text(obj), text) != 0) {
        lv_label_set_text_static(obj, text);
    }
}

static void set_changed_text_color(lv_obj_t *obj, lv_color_t color)
{
    if (!obj) return;
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(obj, color, 0);
    }
}

static void set_changed_bg_color(lv_obj_t *obj, lv_color_t color)
{
    if (!obj) return;
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_bg_color(obj, color, 0);
    }
}

static void set_changed_ind_bg_color(lv_obj_t *obj, lv_color_t color)
{
    if (!obj) return;
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_INDICATOR), color)) {
        lv_obj_set_style_bg_color(obj, color, LV_PART_INDICATOR);
    }
}

static void set_changed_bg_opa(lv_obj_t *obj, lv_opa_t opa)
{
    if (!obj) return;
    if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) {
        lv_obj_set_style_bg_opa(obj, opa, 0);
    }
}

static void set_changed_border_color(lv_obj_t *obj, lv_color_t color)
{
    if (!obj) return;
    if (!lv_color_eq(lv_obj_get_style_border_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_border_color(obj, color, 0);
    }
}

static void set_changed_text_align(lv_obj_t *obj, lv_text_align_t align)
{
    if (!obj) return;
    if (lv_obj_get_style_text_align(obj, LV_PART_MAIN) != align) {
        lv_obj_set_style_text_align(obj, align, 0);
    }
}

static void set_changed_x(lv_obj_t *obj, int32_t x)
{
    if (!obj) return;
    if (lv_obj_get_style_x(obj, LV_PART_MAIN) != x) {
        lv_obj_set_x(obj, x);
    }
}

static void set_changed_y(lv_obj_t *obj, int32_t y)
{
    if (!obj) return;
    if (lv_obj_get_style_y(obj, LV_PART_MAIN) != y) {
        lv_obj_set_y(obj, y);
    }
}

static void set_changed_width(lv_obj_t *obj, int32_t w)
{
    if (!obj) return;
    if (lv_obj_get_style_width(obj, LV_PART_MAIN) != w) {
        lv_obj_set_width(obj, w);
    }
}

static void set_changed_height(lv_obj_t *obj, int32_t h)
{
    if (!obj) return;
    if (lv_obj_get_style_height(obj, LV_PART_MAIN) != h) {
        lv_obj_set_height(obj, h);
    }
}

static void set_changed_bar_value(lv_obj_t *obj, int32_t val)
{
    if (!obj) return;
    if (lv_bar_get_value(obj) != val) {
        lv_bar_set_value(obj, val, LV_ANIM_OFF);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden) {
        lv_obj_update_flag(obj, LV_OBJ_FLAG_HIDDEN, hidden);
    }
}

static void bind_status_bar(lv_obj_t *bar, lv_obj_t *ic, lv_obj_t *lbl, lv_obj_t *sub,
                            lv_obj_t *r_lbl, lv_obj_t *hp, lv_obj_t *batt,
                            lv_obj_t *batt_fill, lv_obj_t *batt_nub,
                            const ui_model_t *m, uint16_t bg565, uint16_t fg565,
                            ui_icon_t icon, uint16_t icon565,
                            const char *left_txt, const char *sub_txt,
                            const char *right_txt, int no_clock)
{
    if (!bar) return;
    set_changed_bg_color(bar, ui_color_from_rgb565(bg565));

    if (ic) {
        if (icon != UI_ICON_NONE) {
            set_hidden(ic, false);
            set_changed_text(ic, ui_icon_glyph(icon));
            set_changed_text_color(ic, ui_color_from_rgb565(icon565));
            if (lbl) set_changed_x(lbl, 14);
        } else {
            set_hidden(ic, true);
            if (lbl) set_changed_x(lbl, 3);
        }
    }

    if (lbl) {
        set_changed_text(lbl, left_txt ? left_txt : "");
        set_changed_text_color(lbl, ui_color_from_rgb565(fg565));
    }

    if (sub) {
        if (sub_txt && sub_txt[0] != '\0') {
            set_hidden(sub, false);
            set_changed_text(sub, sub_txt);
        } else {
            set_hidden(sub, true);
        }
    }

    if (r_lbl) {
        if (no_clock) {
            if (right_txt && right_txt[0] != '\0') {
                set_hidden(r_lbl, false);
                set_changed_text(r_lbl, right_txt);
                set_changed_text_color(r_lbl, ui_color_from_rgb565(fg565));
                set_changed_x(r_lbl, 86);
            } else {
                set_hidden(r_lbl, true);
            }
        } else {
            char clk[8];
            ui_format_clock(clk, (int)sizeof(clk), m->now.hour, m->now.minute);
            set_hidden(r_lbl, false);
            set_changed_text(r_lbl, clk);
            set_changed_text_color(r_lbl, lv_color_hex(UI_HEX_DIM));
            set_changed_x(r_lbl, m->hp_plugged ? 70 : 86);
        }
    }

    if (hp) {
        set_hidden(hp, !m->hp_plugged || no_clock);
    }

    uint8_t pct = (m->batt_pct > 100u) ? 100u : m->batt_pct;
    int32_t fw  = (int32_t)((10u * pct + 50u) / 100u);
    if (pct > 0u && fw < 1) fw = 1;
    lv_color_t bcol = m->charging ? lv_color_hex(UI_HEX_GREEN) :
                      (pct <= 15u ? lv_color_hex(UI_HEX_RED) :
                       (pct <= 30u ? lv_color_hex(UI_HEX_AMBER) : lv_color_hex(UI_HEX_TEXT)));
    lv_color_t ocol = (fg565 == UI_COL_BLACK) ? lv_color_hex(UI_HEX_BLACK) : lv_color_hex(UI_HEX_DIM);
    if (batt)      set_changed_border_color(batt, ocol);
    if (batt_nub)  set_changed_bg_color(batt_nub, ocol);
    if (batt_fill) {
        set_changed_width(batt_fill, fw);
        set_changed_bg_color(batt_fill, bcol);
    }
}

static void bind_vmeter_segs(lv_obj_t *segs[8], uint8_t pct, uint8_t peak_pct, int clip_latch)
{
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
        if (!segs[i]) continue;
        uint32_t hex;
        if (i == 7 && clip_latch)      hex = UI_HEX_RED;
        else if (i == peak_idx)        hex = UI_HEX_TEXT;
        else if (i < lit)              hex = (i == 7) ? UI_HEX_RED : (i >= 5 ? UI_HEX_AMBER : UI_HEX_GREEN);
        else                           hex = UI_HEX_OFF;
        set_changed_bg_color(segs[i], lv_color_hex(hex));
    }
}

static void bind_scrollbar(lv_obj_t *thumb, int total, int scroll, int visible, int32_t track_h)
{
    if (!thumb) return;
    if (total <= visible || visible <= 0) {
        set_changed_y(thumb, 0);
        set_changed_height(thumb, track_h);
        return;
    }
    int max_scroll = total - visible;
    if (scroll < 0) scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    int32_t th = (track_h * visible) / total;
    if (th < 6) th = 6;
    if (th > track_h) th = track_h;
    int32_t ty = ((track_h - th) * scroll) / max_scroll;
    set_changed_y(thumb, ty);
    set_changed_height(thumb, th);
}

/* =========================================================================
 * Per-Screen Data Binders
 * ========================================================================= */
/* The footer and underline share one native scroll animation. */
static void home_scroll_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_BEGIN) {
        lv_anim_t *anim = lv_event_get_param(event);
        if (anim) {
            lv_anim_set_duration(anim, 90u);
            lv_anim_set_path_cb(anim, lv_anim_path_ease_in_out);
        }
        return;
    }
    lv_obj_t *view = lv_event_get_target(event);
    int32_t first_x = lv_obj_get_x(objects.home_tile_0);
    int32_t stride = lv_obj_get_x(objects.home_tile_1) - first_x;
    int32_t center = (lv_obj_get_width(objects.home_tile_0) -
                      lv_obj_get_width(objects.home_focus_mark)) / 2;
    int32_t width = lv_obj_get_width(view);
    if (width > 0) {
        lv_obj_set_x(objects.home_focus_mark, first_x + center +
                     (lv_obj_get_scroll_x(view) * stride + width / 2) / width);
    }
}

static void bind_home_detail(const ui_model_t *m, unsigned page, lv_obj_t *title, lv_obj_t *detail)
{
    char l1[64] = "", l2[64] = "";
    uint32_t col1 = UI_HEX_TEXT;
    if (!m->sd_ok) {
        lv_snprintf(l1, sizeof(l1), "%s", ui_tr(UI_STR_STORAGE_ERROR));
        lv_snprintf(l2, sizeof(l2), "%s", ui_tr(UI_STR_CONNECT_USB_CHECK));
        col1 = UI_HEX_RED;
    } else if (page == 0u) {
        if (!m->has_last) {
            lv_snprintf(l1, sizeof(l1), "%s", ui_tr(UI_STR_NOTHING_PLAYED));
            col1 = UI_HEX_DIM;
        } else {
            char dt_s[32], dur_s[12], res_s[12];
            format_file_title(dt_s, (int)sizeof(dt_s), &m->now, &m->last_file);
            ui_format_duration(dur_s, (int)sizeof(dur_s), m->last_file.duration_sec);
            lv_snprintf(l1, sizeof(l1), "%s \xC2\xB7 %s", dt_s, dur_s);
            if (m->last_file.resume_sec > 0u) {
                ui_format_duration(res_s, (int)sizeof(res_s), m->last_file.resume_sec);
                lv_snprintf(l2, sizeof(l2), ui_tr(UI_STR_RESUME_AT_FMT), res_s);
            } else {
                lv_snprintf(l2, sizeof(l2), "%s", ui_tr(UI_STR_PLAY_FROM_START));
            }
        }
    } else if (page == 1u) {
        char free_s[20];
        ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
        lv_snprintf(l1, sizeof(l1), "%d %s", m->file_count,
                    ui_tr((m->file_count == 1) ? UI_STR_RECORDING_ONE : UI_STR_RECORDING_MANY));
        lv_snprintf(l2, sizeof(l2), ui_tr(UI_STR_SPACE_LEFT_FMT), free_s);
    } else if (page == 2u) {
        lv_snprintf(l1, sizeof(l1), "%s %u \xC2\xB7 %s \xC2\xB7 %s",
                    ui_tr(UI_STR_GAIN), (unsigned)m->cfg.mic_gain,
                    (m->cfg.channels == 1u) ? "1ch" : "2ch",
                    sound_mode_name(m->cfg.sound_mode));
        lv_snprintf(l2, sizeof(l2), "%s", ui_tr(m->cfg.vor_enabled ? UI_STR_VOR_ON : UI_STR_VOR_OFF));
    } else {
        lv_snprintf(l1, sizeof(l1), "%s", ui_tr(UI_STR_BT_FILE_SYNC));
        lv_snprintf(l2, sizeof(l2), ui_tr(UI_STR_DEVICE_NAME_FMT), m->bt_name[0] ? m->bt_name : "HW_76");
    }
    set_changed_text(title, l1);
    set_changed_text_color(title, lv_color_hex(col1));
    set_changed_text(detail, l2);
}

static void bind_home(const ui_model_t *m, bool entering)
{
    char date_str[28];
    lv_snprintf(date_str, sizeof(date_str), "%s %u %s",
                ui_weekday_name(m->now.year, m->now.month, m->now.day),
                (unsigned)m->now.day, ui_month_name(m->now.month));
    bind_status_bar(objects.home_bar, objects.home_bar_ic, objects.home_bar_l, objects.home_bar_sub,
                    objects.home_bar_r, objects.home_bar_hp, objects.home_batt,
                    objects.home_batt_fill, objects.home_batt_nub,
                    m, UI_COL_BAR, UI_COL_DIM, UI_ICON_NONE, UI_COL_TEXT, date_str, NULL, NULL, 0);

    lv_obj_t *tiles[4] = { objects.home_tile_0, objects.home_tile_1, objects.home_tile_2, objects.home_tile_3 };
    lv_obj_t *ics[4]   = { objects.home_tile_ic_0, objects.home_tile_ic_1, objects.home_tile_ic_2, objects.home_tile_ic_3 };
    lv_obj_t *lbls[4]  = { objects.home_tile_lbl_0, objects.home_tile_lbl_1, objects.home_tile_lbl_2, objects.home_tile_lbl_3 };
    set_changed_text(lbls[0], ui_tr(UI_STR_TILE_LAST));
    set_changed_text(lbls[1], ui_tr(UI_STR_TILE_FILES));
    set_changed_text(lbls[2], ui_tr(UI_STR_TILE_SETUP));
    set_changed_text(lbls[3], ui_tr(UI_STR_TILE_SYNC));
    static uint8_t previous_focus;
    uint8_t focus = (m->home_focus < 4u) ? m->home_focus : 3u;
    if (entering || focus != previous_focus) {
        for (int i = 0; i < 4; i++) {
            if (!entering && i != previous_focus && i != focus) continue;
            bool selected = i == focus;
            set_changed_bg_color(tiles[i], lv_color_hex(selected ? UI_HEX_PILL : UI_HEX_TILE));
            lv_color_t ink = lv_color_hex(selected ? UI_HEX_TEXT : UI_HEX_DIM);
            set_changed_text_color(ics[i], ink);
            set_changed_text_color(lbls[i], ink);
        }

        static lv_obj_t *s_home_bound_view = NULL;
        lv_obj_t *view = objects.home_details;
        if (entering) {
            if (view != s_home_bound_view) {
                s_home_bound_view = view;
                lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
                lv_obj_add_event_cb(view, home_scroll_event, LV_EVENT_SCROLL_BEGIN, NULL);
                lv_obj_add_event_cb(view, home_scroll_event, LV_EVENT_SCROLL, NULL);
            }
            int32_t first_x = lv_obj_get_style_x(tiles[0], LV_PART_MAIN);
            int32_t stride  = lv_obj_get_style_x(tiles[1], LV_PART_MAIN) - first_x;
            int32_t center  = (lv_obj_get_style_width(tiles[0], LV_PART_MAIN) -
                               lv_obj_get_style_width(objects.home_focus_mark, LV_PART_MAIN)) / 2;
            set_changed_x(objects.home_focus_mark, first_x + center + (int32_t)focus * stride);
        }
        lv_obj_scroll_to_x(view, focus * lv_obj_get_style_width(view, LV_PART_MAIN),
                           entering ? LV_ANIM_OFF : LV_ANIM_ON);
        previous_focus = focus;
    }

    lv_obj_t *titles[] = {objects.home_detail_0_0, objects.home_detail_1_0,
                          objects.home_detail_2_0, objects.home_detail_3_0};
    lv_obj_t *details[] = {objects.home_detail_0_1, objects.home_detail_1_1,
                           objects.home_detail_2_1, objects.home_detail_3_1};
    for (unsigned page = 0; page < 4; page++) {
        bind_home_detail(m, page, titles[page], details[page]);
    }
}

static void bind_rec(const ui_model_t *m, bool entering)
{
    char badge[24];
    lv_snprintf(badge, sizeof(badge), "%s \xC2\xB7 %s",
                (m->cfg.channels == 1u) ? "1ch" : "2ch", sound_mode_name(m->cfg.sound_mode));

    if (m->rec_vor_wait) {
        bind_status_bar(objects.rec_bar, objects.rec_bar_ic, objects.rec_bar_l, objects.rec_bar_sub,
                        objects.rec_bar_r, objects.rec_bar_hp, objects.rec_batt,
                        objects.rec_batt_fill, objects.rec_batt_nub,
                        m, UI_COL_AMBER, UI_COL_BLACK, UI_ICON_PAUSE, UI_COL_BLACK,
                        ui_tr(UI_STR_WAITING_VOICE), NULL, NULL, 1);
    } else {
        bind_status_bar(objects.rec_bar, objects.rec_bar_ic, objects.rec_bar_l, objects.rec_bar_sub,
                        objects.rec_bar_r, objects.rec_bar_hp, objects.rec_batt,
                        objects.rec_batt_fill, objects.rec_batt_nub,
                        m, UI_COL_RED, UI_COL_TEXT, UI_ICON_DOT,
                        m->rec_blink ? UI_COL_TEXT : UI_COL_REC_DIM, "REC", NULL, badge, 1);
    }

    char tbuf[16], gbuf[24], free_s[24], ftxt[40];
    ui_format_duration(tbuf, (int)sizeof(tbuf), m->rec_sec);
    set_changed_text(objects.rec_timer, tbuf);
    set_changed_text_color(objects.rec_timer,
                           lv_color_hex(m->rec_vor_wait ? UI_HEX_MUTE : UI_HEX_TEXT));

    lv_snprintf(gbuf, sizeof(gbuf), "%s %u", ui_tr(UI_STR_GAIN), (unsigned)m->cfg.mic_gain);
    set_changed_text(objects.rec_gain_lbl, gbuf);
    set_changed_bg_color(objects.rec_gain_pill,
                         lv_color_hex(m->rec_gain_bar ? UI_HEX_CYAN : UI_HEX_TILE));
    set_changed_text_color(objects.rec_gain_lbl,
                           lv_color_hex(m->rec_gain_bar ? UI_HEX_BLACK : UI_HEX_TEXT));

    ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
    lv_snprintf(ftxt, sizeof(ftxt), ui_tr(UI_STR_LEFT_FMT), free_s);
    set_changed_text(objects.rec_free_lbl, ftxt);
    set_changed_text_color(objects.rec_free_lbl,
                           lv_color_hex((m->sd_free_sec < 600u) ? UI_HEX_AMBER : UI_HEX_DIM));

    if (m->spec) {
        uint32_t sig = spec_signature(m->spec);
        bool need_src = entering ||
                        s_spec_dsc.data != (const uint8_t *)m->spec->pixels ||
                        lv_image_get_src(objects.rec_spec_img) != &s_spec_dsc;
        if (need_src) {
            s_spec_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            s_spec_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            s_spec_dsc.header.w      = UI_SPEC_W;
            s_spec_dsc.header.h      = UI_SPEC_H;
            s_spec_dsc.header.stride = UI_SPEC_W * 2u;
            s_spec_dsc.data_size     = sizeof(m->spec->pixels);
            s_spec_dsc.data          = (const uint8_t *)m->spec->pixels;
            lv_image_set_src(objects.rec_spec_img, &s_spec_dsc);
            s_last_spec_ptr = m->spec;
            s_last_spec_sig = sig;
        } else if (m->spec != s_last_spec_ptr || sig != s_last_spec_sig) {
            lv_obj_invalidate(objects.rec_spec_img);
            s_last_spec_ptr = m->spec;
            s_last_spec_sig = sig;
        }
    }

    lv_obj_t *vml[8] = { objects.rec_vml_0, objects.rec_vml_1, objects.rec_vml_2, objects.rec_vml_3,
                         objects.rec_vml_4, objects.rec_vml_5, objects.rec_vml_6, objects.rec_vml_7 };
    lv_obj_t *vmr[8] = { objects.rec_vmr_0, objects.rec_vmr_1, objects.rec_vmr_2, objects.rec_vmr_3,
                         objects.rec_vmr_4, objects.rec_vmr_5, objects.rec_vmr_6, objects.rec_vmr_7 };
    bind_vmeter_segs(vml, m->rec_vu_l, m->rec_peak_l, m->rec_clip_l);
    bind_vmeter_segs(vmr,
                     (m->cfg.channels == 1u) ? m->rec_vu_l : m->rec_vu_r,
                     (m->cfg.channels == 1u) ? m->rec_peak_l : m->rec_peak_r,
                     (m->cfg.channels == 1u) ? m->rec_clip_l : m->rec_clip_r);

    if (m->rec_warn[0] != '\0') {
        set_changed_bg_color(objects.rec_foot_box,
                             ui_color_from_rgb565(m->rec_warn_col ? m->rec_warn_col : UI_COL_AMBER));
        set_changed_bg_opa(objects.rec_foot_box, LV_OPA_COVER);
        set_hidden(objects.rec_foot_bar, true);
        set_hidden(objects.rec_foot_right, true);
        set_hidden(objects.rec_foot_left, false);
        set_changed_text(objects.rec_foot_left, m->rec_warn);
        set_changed_text_color(objects.rec_foot_left,
                               lv_color_hex((m->rec_warn_col == UI_COL_RED) ? UI_HEX_TEXT : UI_HEX_BLACK));
        set_changed_text_align(objects.rec_foot_left, LV_TEXT_ALIGN_CENTER);
        set_changed_width(objects.rec_foot_left, 152);
    } else if (m->rec_gain_bar) {
        set_changed_bg_color(objects.rec_foot_box, lv_color_hex(UI_HEX_TILE));
        set_changed_bg_opa(objects.rec_foot_box, LV_OPA_COVER);
        set_hidden(objects.rec_foot_left, true);
        set_hidden(objects.rec_foot_right, true);
        set_hidden(objects.rec_foot_bar, false);
        set_changed_bar_value(objects.rec_foot_bar, (int32_t)m->cfg.mic_gain);
    } else if (m->rec_vor_wait) {
        set_changed_bg_opa(objects.rec_foot_box, LV_OPA_TRANSP);
        set_hidden(objects.rec_foot_bar, true);
        set_hidden(objects.rec_foot_right, true);
        set_hidden(objects.rec_foot_left, false);
        set_changed_text(objects.rec_foot_left, ui_tr(UI_STR_RESUMES_SPEAK));
        set_changed_text_color(objects.rec_foot_left, lv_color_hex(UI_HEX_DIM));
        set_changed_text_align(objects.rec_foot_left, LV_TEXT_ALIGN_CENTER);
        set_changed_width(objects.rec_foot_left, 152);
    } else {
        set_changed_bg_opa(objects.rec_foot_box, LV_OPA_TRANSP);
        set_hidden(objects.rec_foot_bar, true);
        set_hidden(objects.rec_foot_left, false);
        set_hidden(objects.rec_foot_right, false);
        set_changed_text(objects.rec_foot_left, m->rec_filename);
        set_changed_text_color(objects.rec_foot_left, lv_color_hex(UI_HEX_MUTE));
        set_changed_text_align(objects.rec_foot_left, LV_TEXT_ALIGN_LEFT);
        set_changed_width(objects.rec_foot_left, 108);
        if (m->rec_markers > 0u) {
            char mb[16];
            lv_snprintf(mb, sizeof(mb), "# %u", (unsigned)m->rec_markers);
            set_changed_text(objects.rec_foot_right, mb);
            set_changed_text_color(objects.rec_foot_right, lv_color_hex(UI_HEX_CYAN));
        } else {
            set_changed_text(objects.rec_foot_right, ui_tr(UI_STR_OK_MARK));
            set_changed_text_color(objects.rec_foot_right, lv_color_hex(UI_HEX_DIM));
        }
    }
}

void ui_rec_update_live_sync(const ui_model_t *m)
{
    if (!m || s_active_eez_scr != SCREEN_ID_REC) return;
    ui_i18n_set_lang(m->cfg.lang);
    if (m->spec && objects.rec_spec_img) {
        lv_obj_invalidate(objects.rec_spec_img);
        s_last_spec_ptr = m->spec;
        s_last_spec_sig = spec_signature(m->spec);
    }
    lv_obj_t *vml[8] = { objects.rec_vml_0, objects.rec_vml_1, objects.rec_vml_2, objects.rec_vml_3,
                         objects.rec_vml_4, objects.rec_vml_5, objects.rec_vml_6, objects.rec_vml_7 };
    lv_obj_t *vmr[8] = { objects.rec_vmr_0, objects.rec_vmr_1, objects.rec_vmr_2, objects.rec_vmr_3,
                         objects.rec_vmr_4, objects.rec_vmr_5, objects.rec_vmr_6, objects.rec_vmr_7 };
    bind_vmeter_segs(vml, m->rec_vu_l, m->rec_peak_l, m->rec_clip_l);
    bind_vmeter_segs(vmr,
                     (m->cfg.channels == 1u) ? m->rec_vu_l : m->rec_vu_r,
                     (m->cfg.channels == 1u) ? m->rec_peak_l : m->rec_peak_r,
                     (m->cfg.channels == 1u) ? m->rec_clip_l : m->rec_clip_r);
    ui_port_render_now();
}

static void bind_rec_err(const ui_model_t *m)
{
    if (!m->sd_ok) {
        bind_status_bar(objects.rec_err_bar, objects.rec_err_bar_ic, objects.rec_err_bar_l, objects.rec_err_bar_sub,
                        objects.rec_err_bar_r, objects.rec_err_bar_hp, objects.rec_err_batt,
                        objects.rec_err_batt_fill, objects.rec_err_batt_nub,
                        m, UI_COL_DRED, UI_COL_TEXT, UI_ICON_STOP, UI_COL_TEXT, ui_tr(UI_STR_NOT_RECORDING), NULL, NULL, 1);
        set_changed_text(objects.rec_err_title, ui_tr(UI_STR_STORAGE_ERROR));
        set_changed_text_color(objects.rec_err_title, lv_color_hex(UI_HEX_RED));
        set_changed_text(objects.rec_err_l1, ui_tr(UI_STR_NOTHING_RECORDED));
        set_changed_text(objects.rec_err_l2, ui_tr(UI_STR_SLIDE_REC_OFF_USB));
        set_changed_text(objects.rec_err_l3, ui_tr(UI_STR_USB_CHECK_STORAGE));
    } else {
        bind_status_bar(objects.rec_err_bar, objects.rec_err_bar_ic, objects.rec_err_bar_l, objects.rec_err_bar_sub,
                        objects.rec_err_bar_r, objects.rec_err_bar_hp, objects.rec_err_batt,
                        objects.rec_err_batt_fill, objects.rec_err_batt_nub,
                        m, UI_COL_FAINT, UI_COL_TEXT, UI_ICON_STOP, UI_COL_TEXT, ui_tr(UI_STR_STOPPED), NULL, NULL, 1);
        set_changed_text(objects.rec_err_title, m->rec_stop_reason[0] ? m->rec_stop_reason : ui_tr(UI_STR_STORAGE_FULL));
        set_changed_text_color(objects.rec_err_title, lv_color_hex(UI_HEX_AMBER));
        char l1[56], dur[16], l2[40];
        lv_snprintf(l1, sizeof(l1), ui_tr(UI_STR_SAVED_FILE_FMT), m->rec_filename);
        ui_format_duration(dur, (int)sizeof(dur), m->rec_sec);
        lv_snprintf(l2, sizeof(l2), ui_tr(UI_STR_RECORDED_FMT), dur);
        set_changed_text(objects.rec_err_l1, l1);
        set_changed_text(objects.rec_err_l2, l2);
        set_changed_text(objects.rec_err_l3, ui_tr(UI_STR_SLIDE_REC_CONT));
    }
}

static void bind_saved(const ui_model_t *m)
{
    bind_status_bar(objects.saved_bar, objects.saved_bar_ic, objects.saved_bar_l, objects.saved_bar_sub,
                    objects.saved_bar_r, objects.saved_bar_hp, objects.saved_batt,
                    objects.saved_batt_fill, objects.saved_batt_nub,
                    m, UI_COL_GREEN, UI_COL_BLACK, UI_ICON_CHECK, UI_COL_BLACK, ui_tr(UI_STR_SAVED), NULL, NULL, 1);
    char dur[16], dt_s[32], meta[48];
    ui_format_duration(dur, (int)sizeof(dur), m->saved_sec);
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->saved_dt);
    lv_snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s", dt_s, (m->saved_channels == 1u) ? "1ch" : "2ch");
    set_changed_text(objects.saved_dur, dur);
    set_changed_text(objects.saved_meta, meta);
    set_changed_text(objects.saved_file, m->saved_filename);
    set_changed_text(objects.saved_footer, ui_tr(UI_STR_SAVED_FOOTER));
    set_changed_bar_value(objects.saved_prog, (int32_t)m->saved_frac_pct);
}

static void bind_files(const ui_model_t *m)
{
    int sel_disp = m->file_sel;
    if (sel_disp < 0) sel_disp = 0;
    if (m->file_count > 0 && sel_disp >= m->file_count) sel_disp = m->file_count - 1;

    char sub[16];
    lv_snprintf(sub, sizeof(sub), "%d / %d", sel_disp + 1, m->file_count);
    bind_status_bar(objects.files_bar, objects.files_bar_ic, objects.files_bar_l, objects.files_bar_sub,
                    objects.files_bar_r, objects.files_bar_hp, objects.files_batt,
                    objects.files_batt_fill, objects.files_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT,
                    m->files_title[0] ? m->files_title : ui_tr(UI_STR_FILES), sub, NULL, 0);
    set_changed_text(objects.files_footer, ui_tr(UI_STR_FILES_FOOTER));

    lv_obj_t *rows[3]   = { objects.files_row_0, objects.files_row_1, objects.files_row_2 };
    lv_obj_t *dots[3]   = { objects.files_dot_0, objects.files_dot_1, objects.files_dot_2 };
    lv_obj_t *names[3]  = { objects.files_name_0, objects.files_name_1, objects.files_name_2 };
    lv_obj_t *badges[3] = { objects.files_badge_0, objects.files_badge_1, objects.files_badge_2 };
    lv_obj_t *durs[3]   = { objects.files_dur_0, objects.files_dur_1, objects.files_dur_2 };

    for (int r = 0; r < 3; r++) {
        int idx = m->file_scroll + r;
        if (idx < 0 || idx >= m->file_count) {
            set_hidden(rows[r], true);
            continue;
        }
        set_hidden(rows[r], false);
        const ui_file_entry_t *f = &m->files[idx];
        int sel = (idx == m->file_sel);
        set_changed_bg_opa(rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP);

        if (f->is_dir) {
            set_changed_bg_opa(dots[r], LV_OPA_COVER);
            set_changed_bg_color(dots[r], lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_AMBER));
        } else if (f->unplayed) {
            set_changed_bg_opa(dots[r], LV_OPA_COVER);
            set_changed_bg_color(dots[r], lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_CYAN));
        } else {
            set_changed_bg_opa(dots[r], LV_OPA_TRANSP);
        }

        char dt_lbl[28], dur[16];
        format_file_title(dt_lbl, (int)sizeof(dt_lbl), &m->now, f);
        if (f->is_dir) {
            if (f->duration_sec > 0u) {
                lv_snprintf(dur, sizeof(dur), "%u \xE2\x96\xB6", (unsigned)f->duration_sec);
            } else {
                lv_snprintf(dur, sizeof(dur), "DIR \xE2\x96\xB6");
            }
        } else {
            ui_format_duration(dur, (int)sizeof(dur), f->duration_sec);
        }
        set_changed_text(names[r], dt_lbl);
        set_changed_text(durs[r], dur);
        set_changed_text_color(durs[r], lv_color_hex(f->is_dir ? (sel ? UI_HEX_TEXT : UI_HEX_AMBER)
                                                               : (sel ? UI_HEX_CYAN : UI_HEX_DIM)));

        if (!f->is_dir && f->has_transcript) {
            set_hidden(badges[r], false);
            set_changed_bg_color(badges[r], lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_FAINT));
            set_changed_text_color(badges[r], lv_color_hex(sel ? UI_HEX_PILL : UI_HEX_TEXT));
        } else {
            set_hidden(badges[r], true);
        }
    }
    bind_scrollbar(objects.files_scroll_thumb, m->file_count, m->file_scroll, 3, 50);
}

static lv_color_t tr_speaker_lv_color(uint8_t idx, uint16_t rgb565)
{
    static const uint32_t fallback[4] = { UI_HEX_CYAN, UI_HEX_AMBER, UI_HEX_GREEN, UI_HEX_PINK };
    if (rgb565 != 0u && rgb565 != 0xFFFFu) {
        return ui_color_from_rgb565(rgb565);
    }
    return lv_color_hex(fallback[idx & 3u]);
}

static void format_vol_label(char *dst, size_t cap, int hp_plugged, uint8_t vol)
{
    uint32_t tenths = ((uint32_t)vol * 100u + 15u) / 31u;
    if (tenths % 10u != 0u) {
        lv_snprintf(dst, cap, "%s %u.%u", hp_plugged ? "HP" : "SPK",
                    (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    } else {
        lv_snprintf(dst, cap, "%s %u", hp_plugged ? "HP" : "SPK", (unsigned)(tenths / 10u));
    }
}

/* "2×", "−4×" (U+2212 minus) */
static void format_speed(char *dst, size_t cap, int speed)
{
    lv_snprintf(dst, cap, "%s%d\xC3\x97", speed < 0 ? "\xE2\x88\x92" : "", speed < 0 ? -speed : speed);
}

static void bind_player_tr(const ui_model_t *m, bool entering)
{
    uint32_t dur = m->play_file.duration_sec ? m->play_file.duration_sec : 1u;
    uint32_t pos = (m->play_pos_sec <= dur) ? m->play_pos_sec : dur;

    /* 1. Compact Top Status Bar: icon + elapsed time + speaker/vol/scrub + battery */
    ui_icon_t ic;
    uint32_t  ic_hex;
    if (m->play_scrub_speed != 0) {
        ic     = (m->play_scrub_speed > 0) ? UI_ICON_RIGHT : UI_ICON_LEFT;
        ic_hex = UI_HEX_AMBER;
    } else if (m->play_state == 0u) {
        ic     = UI_ICON_PLAY;
        ic_hex = UI_HEX_CYAN;
    } else if (m->play_state == 1u) {
        ic     = UI_ICON_PAUSE;
        ic_hex = UI_HEX_AMBER;
    } else {
        ic     = UI_ICON_STOP;
        ic_hex = UI_HEX_DIM;
    }
    set_changed_text(objects.ptr_bar_ic, ui_icon_glyph(ic));
    set_changed_text_color(objects.ptr_bar_ic, lv_color_hex(ic_hex));

    char pos_s[12];
    ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
    set_changed_text(objects.ptr_bar_l, pos_s);
    set_changed_text_color(objects.ptr_bar_l,
                           lv_color_hex((m->play_state == 2u) ? UI_HEX_MUTE : UI_HEX_TEXT));

    int32_t dot_x = (pos >= 3600u) ? 51 : 43;
    if (m->play_vol_hi) {
        char vbuf[16];
        uint8_t vol = m->hp_plugged ? m->cfg.hp_vol : m->cfg.spk_vol;
        format_vol_label(vbuf, sizeof(vbuf), m->hp_plugged, vol);
        set_hidden(objects.ptr_spk_dot, true);
        set_hidden(objects.ptr_bar_sub, true);
        set_hidden(objects.ptr_vol_pill, false);
        set_changed_text(objects.ptr_vol_lbl, vbuf);
    } else if (m->play_scrub_speed != 0) {
        char sbuf[24], spd[12];
        format_speed(spd, sizeof(spd), m->play_scrub_speed < 0 ? -m->play_scrub_speed : m->play_scrub_speed);
        lv_snprintf(sbuf, sizeof(sbuf), "%s %s",
                    (m->play_scrub_speed > 0) ? "\xE2\x96\xB6\xE2\x96\xB6" : "\xE2\x97\x80\xE2\x97\x80", spd);
        set_hidden(objects.ptr_vol_pill, true);
        set_hidden(objects.ptr_spk_dot, true);
        set_hidden(objects.ptr_bar_sub, false);
        set_changed_x(objects.ptr_bar_sub, dot_x);
        set_changed_width(objects.ptr_bar_sub, 139 - dot_x);
        set_changed_text(objects.ptr_bar_sub, sbuf);
        set_changed_text_color(objects.ptr_bar_sub, lv_color_hex(UI_HEX_AMBER));
    } else {
        set_hidden(objects.ptr_vol_pill, true);
        if (m->tr_speaker_name[0] != '\0') {
            lv_color_t spk_col = tr_speaker_lv_color(m->tr_speaker, m->tr_speaker_color);
            set_hidden(objects.ptr_spk_dot, false);
            set_changed_x(objects.ptr_spk_dot, dot_x);
            set_changed_bg_color(objects.ptr_spk_dot, spk_col);
            set_hidden(objects.ptr_bar_sub, false);
            set_changed_x(objects.ptr_bar_sub, dot_x + 7);
            set_changed_width(objects.ptr_bar_sub, 139 - (dot_x + 7));
            set_changed_text(objects.ptr_bar_sub, m->tr_speaker_name);
            set_changed_text_color(objects.ptr_bar_sub, spk_col);
        } else {
            char tmp[12], dur_s[16];
            ui_format_duration(tmp, (int)sizeof(tmp), dur);
            lv_snprintf(dur_s, sizeof(dur_s), "/ %s", tmp);
            set_hidden(objects.ptr_spk_dot, true);
            set_hidden(objects.ptr_bar_sub, false);
            set_changed_x(objects.ptr_bar_sub, dot_x);
            set_changed_width(objects.ptr_bar_sub, 139 - dot_x);
            set_changed_text(objects.ptr_bar_sub, dur_s);
            set_changed_text_color(objects.ptr_bar_sub, lv_color_hex(UI_HEX_DIM));
        }
    }

    uint8_t pct = (m->batt_pct > 100u) ? 100u : m->batt_pct;
    int32_t fw  = (int32_t)((10u * pct + 50u) / 100u);
    if (pct > 0u && fw < 1) fw = 1;
    lv_color_t bcol = m->charging ? lv_color_hex(UI_HEX_GREEN) :
                      (pct <= 15u ? lv_color_hex(UI_HEX_RED) :
                       (pct <= 30u ? lv_color_hex(UI_HEX_AMBER) : lv_color_hex(UI_HEX_TEXT)));
    if (objects.ptr_batt_fill) {
        set_changed_width(objects.ptr_batt_fill, fw);
        set_changed_bg_color(objects.ptr_batt_fill, bcol);
    }

    /* 2. Full-width 2px smooth progress bar (0..1000) */
    int32_t prog_val;
    if (m->play_dur_ms > 0u) {
        uint32_t pms = (m->play_pos_ms <= m->play_dur_ms) ? m->play_pos_ms : m->play_dur_ms;
        prog_val = (int32_t)(((uint64_t)pms * 1000ull) / (uint64_t)m->play_dur_ms);
    } else {
        prog_val = (int32_t)((pos * 1000u) / dur);
    }
    set_changed_bar_value(objects.ptr_prog, prog_val);
    set_changed_ind_bg_color(objects.ptr_prog,
                             lv_color_hex((m->play_scrub_speed != 0 || m->play_state == 1u)
                                              ? UI_HEX_AMBER
                                              : (m->play_state == 2u ? UI_HEX_MUTE : UI_HEX_CYAN)));

    /* 3. Smooth vertical scroll animation on ptr_track */
    if (entering) {
        s_tr_top_line_id   = m->tr_top_line_id;
        s_tr_active_line   = -1;
        s_tr_pill_target_x = -1;
        s_tr_pill_target_y = -1;
        s_tr_pill_target_w = -1;
        s_tr_pill_visible  = 0;
        lv_anim_delete(objects.ptr_track, anim_set_y_cb);
        lv_anim_delete(objects.ptr_word_pill, NULL);
        set_changed_y(objects.ptr_track, TR_TRACK_REST_Y);
    }

    int scrolled = 0;
    if (!entering && m->tr_top_line_id != s_tr_top_line_id) {
        int32_t delta = (s_tr_top_line_id >= 0 && m->tr_top_line_id >= 0)
                      ? (m->tr_top_line_id - s_tr_top_line_id)
                      : 0;
        if (delta == 1 || delta == -1) {
            int32_t cur_y   = lv_obj_get_style_y(objects.ptr_track, LV_PART_MAIN);
            int32_t start_y = cur_y + delta * TR_ROW_H;
            if (start_y > TR_TRACK_REST_Y + 24) start_y = TR_TRACK_REST_Y + 24;
            if (start_y < TR_TRACK_REST_Y - 24) start_y = TR_TRACK_REST_Y - 24;
            lv_obj_set_y(objects.ptr_track, start_y);
            start_ease_out_anim(objects.ptr_track, anim_set_y_cb,
                                start_y, TR_TRACK_REST_Y, 190u);
            scrolled = 1;
        } else {
            lv_anim_delete(objects.ptr_track, anim_set_y_cb);
            set_changed_y(objects.ptr_track, TR_TRACK_REST_Y);
        }
        s_tr_top_line_id = m->tr_top_line_id;
    }

    /* 4. Bind 6-slot scrolling transcript rows (4 visible + 1 above + 1 below) */
    lv_obj_t *bars[CWX_TR_VIEW_LINES] = {
        objects.ptr_bar_0, objects.ptr_bar_1, objects.ptr_bar_2,
        objects.ptr_bar_3, objects.ptr_bar_4, objects.ptr_bar_5,
    };
    lv_obj_t *lines[CWX_TR_VIEW_LINES] = {
        objects.ptr_line_0, objects.ptr_line_1, objects.ptr_line_2,
        objects.ptr_line_3, objects.ptr_line_4, objects.ptr_line_5,
    };

    lv_color_t bg_col = lv_color_hex(UI_HEX_BG);
    int have_active_upcoming = 0;

    for (int i = 0; i < (int)CWX_TR_VIEW_LINES; i++) {
        const cwx_tr_slot_t *sl = &m->tr_lines[i];
        if (sl->line_id < 0 || sl->text[0] == '\0') {
            set_hidden(bars[i], true);
            set_hidden(lines[i], true);
            continue;
        }

        int is_active = (i == (int)m->tr_active_slot);
        set_hidden(bars[i], false);
        set_hidden(lines[i], false);

        lv_color_t spk_col = tr_speaker_lv_color(sl->speaker_idx, sl->speaker_color);
        lv_color_t bar_col = is_active ? spk_col : lv_color_mix(spk_col, bg_col, 130u);
        set_changed_bg_color(bars[i], bar_col);

        /* Continuous vertical speaker stripe within a turn; clean gap at turn start */
        int same_prev = (!sl->turn_start && i > 0 &&
                         m->tr_lines[i - 1].line_id >= 0 &&
                         m->tr_lines[i - 1].speaker_idx == sl->speaker_idx);
        set_changed_y(bars[i], i * TR_ROW_H + (same_prev ? 1 : 4));
        set_changed_height(bars[i], same_prev ? 15 : 12);

        if (is_active) {
            size_t tlen  = lv_strlen(sl->text);
            size_t split = (m->tr_spoken_len <= tlen) ? (size_t)m->tr_spoken_len : tlen;
            char spoken_buf[CWX_TR_LINE_CAP];
            if (split > 0u) {
                lv_memcpy(spoken_buf, sl->text, split);
            }
            spoken_buf[split] = '\0';

            set_changed_text(lines[i], spoken_buf);
            set_changed_text_color(lines[i], lv_color_hex(UI_HEX_TEXT));

            if (split < tlen && objects.ptr_curr_upcoming) {
                int32_t spoken_w = (split > 0u)
                    ? lv_text_get_width(spoken_buf, (uint32_t)split, &ui_font_md, 0)
                    : 0;
                if (spoken_w < 152) {
                    set_changed_x(objects.ptr_curr_upcoming, 6 + spoken_w);
                    set_changed_y(objects.ptr_curr_upcoming, i * TR_ROW_H);
                    set_changed_width(objects.ptr_curr_upcoming, 152 - spoken_w);
                    set_changed_text(objects.ptr_curr_upcoming, sl->text + split);
                    set_hidden(objects.ptr_curr_upcoming, false);
                    have_active_upcoming = 1;
                }
            }
        } else {
            uint32_t lcol = (m->tr_active_slot >= 1 && i == (int)m->tr_active_slot + 1)
                          ? UI_HEX_DIM
                          : UI_HEX_MUTE;
            set_changed_text(lines[i], sl->text);
            set_changed_text_color(lines[i], lv_color_hex(lcol));
        }
    }

    if (!have_active_upcoming && objects.ptr_curr_upcoming) {
        set_hidden(objects.ptr_curr_upcoming, true);
    }

    /* 5. Gliding speaker-tinted highlight pill behind the active word */
    int aslot = (int)m->tr_active_slot;
    if (objects.ptr_word_pill && aslot >= 1 && aslot <= 4 &&
        m->tr_lines[aslot].line_id >= 0 && m->tr_word_len > 0u) {
        const cwx_tr_slot_t *asl = &m->tr_lines[aslot];
        size_t tlen = lv_strlen(asl->text);
        size_t ws   = (m->tr_word_start < tlen) ? (size_t)m->tr_word_start : tlen;
        size_t wl   = m->tr_word_len;
        if (ws + wl > tlen) wl = tlen - ws;

        if (wl > 0u) {
            int32_t wx = (ws > 0u)
                       ? lv_text_get_width(asl->text, (uint32_t)ws, &ui_font_md, 0)
                       : 0;
            int32_t ww = lv_text_get_width(asl->text + ws, (uint32_t)wl, &ui_font_md, 0);
            int32_t tx = 6 + wx - 2;
            if (tx < 4) tx = 4;
            int32_t tw = ww + 4;
            if (tx + tw > 159) tw = 159 - tx;
            if (tw < 6) tw = 6;
            int32_t ty = aslot * TR_ROW_H + 3;

            lv_color_t spk_col  = tr_speaker_lv_color(asl->speaker_idx, asl->speaker_color);
            lv_color_t pill_col = lv_color_mix(spk_col, bg_col, 104u);
            set_changed_bg_color(objects.ptr_word_pill, pill_col);

            int32_t line_diff = (s_tr_active_line >= 0) ? (asl->line_id - s_tr_active_line) : 99;
            int snap = entering || !s_tr_pill_visible || scrolled ||
                       (line_diff > 1 || line_diff < -1);
            if (snap) {
                lv_anim_delete(objects.ptr_word_pill, NULL);
                set_changed_x(objects.ptr_word_pill, tx);
                set_changed_y(objects.ptr_word_pill, ty);
                set_changed_width(objects.ptr_word_pill, tw);
            } else if (tx != s_tr_pill_target_x || ty != s_tr_pill_target_y || tw != s_tr_pill_target_w) {
                int32_t cur_x = lv_obj_get_style_x(objects.ptr_word_pill, LV_PART_MAIN);
                int32_t cur_y = lv_obj_get_style_y(objects.ptr_word_pill, LV_PART_MAIN);
                int32_t cur_w = lv_obj_get_style_width(objects.ptr_word_pill, LV_PART_MAIN);
                uint32_t dur_ms = (ty != cur_y) ? 120u : 85u;
                start_ease_out_anim(objects.ptr_word_pill, anim_set_x_cb, cur_x, tx, dur_ms);
                start_ease_out_anim(objects.ptr_word_pill, anim_set_w_cb, cur_w, tw, dur_ms);
                if (ty != cur_y) {
                    start_ease_out_anim(objects.ptr_word_pill, anim_set_y_cb, cur_y, ty, dur_ms);
                }
            }
            set_hidden(objects.ptr_word_pill, false);
            s_tr_pill_target_x = tx;
            s_tr_pill_target_y = ty;
            s_tr_pill_target_w = tw;
            s_tr_pill_visible  = 1;
            s_tr_active_line   = asl->line_id;
        } else {
            lv_anim_delete(objects.ptr_word_pill, NULL);
            set_hidden(objects.ptr_word_pill, true);
            s_tr_pill_visible = 0;
        }
    } else if (objects.ptr_word_pill) {
        lv_anim_delete(objects.ptr_word_pill, NULL);
        set_hidden(objects.ptr_word_pill, true);
        s_tr_pill_visible = 0;
    }
}

static void bind_player(const ui_model_t *m, bool entering)
{
    uint32_t dur = m->play_file.duration_sec ? m->play_file.duration_sec : 1u;
    uint32_t pos = (m->play_pos_sec <= dur) ? m->play_pos_sec : dur;
    int32_t  pct = (int32_t)((pos * 100u) / dur);

    if (m->play_has_transcript) {
        bind_player_tr(m, entering);
        return;
    }

    int speed  = m->play_scrub_speed;                 /* 0 = 1x */
    int silent = speed >= UI_SPEED_SILENT || speed <= -UI_SPEED_SILENT;
    ui_icon_t st_ic = (m->play_state == 0u) ? UI_ICON_PLAY : (m->play_state == 1u ? UI_ICON_PAUSE : UI_ICON_STOP);
    uint16_t  st_c  = (m->play_state == 0u) ? UI_COL_CYAN : (m->play_state == 1u ? UI_COL_AMBER : UI_COL_DIM);
    const char *st_t = (m->play_state == 0u) ? ui_tr(UI_STR_PLAYING)
                     : (m->play_state == 1u ? ui_tr(UI_STR_PAUSED) : ui_tr(UI_STR_END));
    if (speed != 0) {
        st_ic = (speed > 0) ? UI_ICON_RIGHT : UI_ICON_LEFT;
        st_c  = UI_COL_AMBER;
        st_t  = ui_tr(silent ? UI_STR_SEEKING : (speed > 0 ? UI_STR_FAST : UI_STR_REVERSE));
    }
    bind_status_bar(objects.play_bar, objects.play_bar_ic, objects.play_bar_l, objects.play_bar_sub,
                    objects.play_bar_r, objects.play_bar_hp, objects.play_batt,
                    objects.play_batt_fill, objects.play_batt_nub,
                    m, UI_COL_BAR, st_c, st_ic, st_c, st_t, NULL, NULL, 0);

    char dt_s[32], vbuf[16], pos_s[14], tmp[12], tot_s[16], ftxt[64];
    format_file_title(dt_s, (int)sizeof(dt_s), &m->now, &m->play_file);
    set_changed_text(objects.player_date, dt_s);

    uint8_t  vol    = m->hp_plugged ? m->cfg.hp_vol : m->cfg.spk_vol;
    uint32_t tenths = (vol * 100u + 15u) / 31u;
    if (tenths % 10u != 0u) {
        lv_snprintf(vbuf, sizeof(vbuf), "%s %u.%u", m->hp_plugged ? "HP" : "SPK",
                    (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    } else {
        lv_snprintf(vbuf, sizeof(vbuf), "%s %u", m->hp_plugged ? "HP" : "SPK", (unsigned)(tenths / 10u));
    }
    if (speed != 0 && !m->play_vol_hi) {
        /* Speed dial: the pill shows the speed; amber when seeking silently. */
        format_speed(vbuf, sizeof(vbuf), speed);
        set_changed_text(objects.player_vol_lbl, vbuf);
        set_changed_bg_color(objects.player_vol_pill, lv_color_hex(silent ? UI_HEX_AMBER : UI_HEX_TILE));
        set_changed_text_color(objects.player_vol_lbl, lv_color_hex(silent ? UI_HEX_BLACK : UI_HEX_AMBER));
    } else {
        set_changed_text(objects.player_vol_lbl, vbuf);
        set_changed_bg_color(objects.player_vol_pill, lv_color_hex(m->play_vol_hi ? UI_HEX_CYAN : UI_HEX_TILE));
        set_changed_text_color(objects.player_vol_lbl, lv_color_hex(m->play_vol_hi ? UI_HEX_BLACK : UI_HEX_DIM));
    }

    ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
    ui_format_duration(tmp, (int)sizeof(tmp), dur);
    lv_snprintf(tot_s, sizeof(tot_s), "/ %s", tmp);
    set_changed_text(objects.player_pos, pos_s);
    set_changed_text_color(objects.player_pos, lv_color_hex((m->play_state == 2u) ? UI_HEX_MUTE
                                                            : (silent ? UI_HEX_AMBER : UI_HEX_TEXT)));
    set_changed_text(objects.player_tot, tot_s);
    set_changed_bar_value(objects.player_vu_l, (int32_t)m->play_vu_l);
    set_changed_bar_value(objects.player_vu_r, (int32_t)m->play_vu_r);
    set_changed_bar_value(objects.player_prog, pct);

    if (m->play_state == 2u) {
        set_changed_text(objects.player_footer, ui_tr(m->play_has_next ? UI_STR_OK_REPLAY_NEXT : UI_STR_OK_REPLAY));
    } else {
        if (m->cfg.skip_mode == UI_SKIP_TRACK) {
            lv_snprintf(ftxt, sizeof(ftxt), "\xE2\x97\x80 %s \xC2\xB7 OK %s \xC2\xB7 \xE2\x96\xB6 %s",
                        ui_tr(UI_STR_PREV), ui_tr((m->play_state == 0u) ? UI_STR_PAUSE : UI_STR_PLAY),
                        ui_tr(UI_STR_NEXT));
        } else {
            /* "◀ −1× · OK Pause · 2× ▶"; away from 1x OK reads "1×". A side
             * at the end of the dial is left out. */
            int cur = speed ? speed : 1;
            int lo = ui_speed_step(cur, -1), hi = ui_speed_step(cur, 1);
            char ls[28] = "", rs[28] = "", ok[32];
            if (lo != cur) {
                format_speed(tmp, sizeof(tmp), lo);
                lv_snprintf(ls, sizeof(ls), "\xE2\x97\x80 %s \xC2\xB7 ", tmp);
            }
            if (hi != cur) {
                format_speed(tmp, sizeof(tmp), hi);
                lv_snprintf(rs, sizeof(rs), " \xC2\xB7 %s \xE2\x96\xB6", tmp);
            }
            if (speed != 0) lv_snprintf(ok, sizeof(ok), "OK 1\xC3\x97");
            else lv_snprintf(ok, sizeof(ok), "OK %s", ui_tr((m->play_state == 0u) ? UI_STR_PAUSE : UI_STR_PLAY));
            lv_snprintf(ftxt, sizeof(ftxt), "%s%s%s", ls, ok, rs);
        }
        set_changed_text(objects.player_footer, ftxt);
    }
}

static void bind_delete(const ui_model_t *m)
{
    bind_status_bar(objects.del_bar, objects.del_bar_ic, objects.del_bar_l, objects.del_bar_sub,
                    objects.del_bar_r, objects.del_bar_hp, objects.del_batt,
                    objects.del_batt_fill, objects.del_batt_nub,
                    m, UI_COL_RED, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_DELETE_Q), NULL, NULL, 1);
    const ui_file_entry_t *f = &m->play_file;
    if (m->files && m->file_sel >= 0 && m->file_sel < m->file_count) f = &m->files[m->file_sel];
    char dt_s[32], dur_s[12], sub[52];
    format_file_title(dt_s, (int)sizeof(dt_s), &m->now, f);
    ui_format_duration(dur_s, (int)sizeof(dur_s), f->duration_sec);
    lv_snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", dur_s, f->filename);
    set_changed_text(objects.del_date, dt_s);
    set_changed_text(objects.del_sub, sub);
    set_changed_bar_value(objects.del_hold_bar, (int32_t)m->del_hold_pct);
    set_changed_text(objects.del_footer, ui_tr(m->del_holding ? UI_STR_KEEP_HOLD_OK : UI_STR_DEL_FOOTER));
    set_changed_text_color(objects.del_footer, lv_color_hex(m->del_holding ? UI_HEX_TEXT : UI_HEX_DIM));
}

static void format_setting_val(const ui_model_t *m, int idx, char *val, int vcap, int *adj)
{
    static const uint8_t brt_pct[5] = { 20u, 40u, 60u, 80u, 100u };
    *adj = 1;
    switch (idx) {
    case 0: lv_snprintf(val, (size_t)vcap, "%u", (unsigned)m->cfg.mic_gain); break;
    case 1: lv_snprintf(val, (size_t)vcap, "%s", sound_mode_name(m->cfg.sound_mode)); break;
    case 2: lv_snprintf(val, (size_t)vcap, "%s", (m->cfg.channels == 1u) ? "1ch" : "2ch"); break;
    case 3: lv_snprintf(val, (size_t)vcap, "%s", ui_tr(m->cfg.vor_enabled ? UI_STR_ON : UI_STR_OFF)); break;
    case 4: lv_snprintf(val, (size_t)vcap, "%u%%", (unsigned)brt_pct[m->cfg.brightness_idx < 5u ? m->cfg.brightness_idx : 3u]); break;
    case 5: lv_snprintf(val, (size_t)vcap, "%s", screen_off_name(m->cfg.screen_off_idx)); break;
    case 6: ui_format_clock(val, vcap, m->now.hour, m->now.minute); *adj = 0; break;
    case 7: lv_snprintf(val, (size_t)vcap, "%s", skip_mode_name(m->cfg.skip_mode)); break;
    case 8: lv_snprintf(val, (size_t)vcap, "%s", ui_lang_native_name(m->cfg.lang)); break;
    case 9: lv_snprintf(val, (size_t)vcap, "%s", ui_tr(m->vbus_present ? UI_STR_CONNECT : UI_STR_UNPLUGGED)); *adj = 0; break;
    case UI_SET_HSPROBE: lv_snprintf(val, (size_t)vcap, "%s", "OK"); *adj = 0; break; /* TODO(i18n) */
    default: lv_snprintf(val, (size_t)vcap, "%s", BOARD_FW_VERSION_STR); *adj = 0; break;
    }
}

static void bind_settings(const ui_model_t *m)
{
    static const ui_str_id_t label_ids[UI_SET_COUNT] = {
        UI_STR_SET_MIC_GAIN, UI_STR_SET_SOUND, UI_STR_SET_MICS, UI_STR_SET_VOR,
        UI_STR_SET_BRIGHTNESS, UI_STR_SET_SCREEN_OFF, UI_STR_SET_CLOCK,
        UI_STR_SET_SKIP, UI_STR_SET_LANGUAGE, UI_STR_SET_USB_DISK, UI_STR_SET_ABOUT,
        UI_STR_SET_HSPROBE
    };
    int sel_disp = m->set_sel;
    if (sel_disp < 0) sel_disp = 0;
    if (sel_disp > UI_SET_COUNT - 1) sel_disp = UI_SET_COUNT - 1;

    char sub[12];
    lv_snprintf(sub, sizeof(sub), "%d / %d", sel_disp + 1, UI_SET_COUNT);
    bind_status_bar(objects.set_bar, objects.set_bar_ic, objects.set_bar_l, objects.set_bar_sub,
                    objects.set_bar_r, objects.set_bar_hp, objects.set_batt,
                    objects.set_batt_fill, objects.set_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_SETTINGS), sub, NULL, 0);

    lv_obj_t *rows[3] = { objects.set_row_0, objects.set_row_1, objects.set_row_2 };
    lv_obj_t *lbls[3] = { objects.set_lbl_0, objects.set_lbl_1, objects.set_lbl_2 };
    lv_obj_t *vals[3] = { objects.set_val_0, objects.set_val_1, objects.set_val_2 };

    for (int r = 0; r < 3; r++) {
        int idx = m->set_scroll + r;
        if (idx < 0 || idx >= UI_SET_COUNT) { set_hidden(rows[r], true); continue; }
        set_hidden(rows[r], false);
        int sel = (idx == m->set_sel);
        set_changed_bg_opa(rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP);
        if (lv_label_get_long_mode(lbls[r]) != LV_LABEL_LONG_DOT) {
            lv_label_set_long_mode(lbls[r], LV_LABEL_LONG_DOT);
        }
        if (lv_label_get_long_mode(vals[r]) != LV_LABEL_LONG_CLIP) {
            lv_label_set_long_mode(vals[r], LV_LABEL_LONG_CLIP);
        }
        set_changed_text(lbls[r], ui_tr(label_ids[idx]));

        const char *val_str;
        char val_buf[32];
        if (idx == 6 && sel && m->set_edit_clock != 0u) {
            lv_snprintf(val_buf, sizeof(val_buf), (m->set_edit_clock == 1u) ? "[%02u]:%02u" : "%02u:[%02u]",
                        (unsigned)m->set_edit_h, (unsigned)m->set_edit_m);
            val_str = val_buf;
            set_changed_text(vals[r], val_str);
            set_changed_text_color(vals[r], lv_color_hex(UI_HEX_CYAN));
        } else {
            char val[24];
            int adj = 0;
            format_setting_val(m, idx, val, (int)sizeof(val), &adj);
            if (sel && adj) {
                lv_snprintf(val_buf, sizeof(val_buf), "- %s +", val);
                val_str = val_buf;
                set_changed_text(vals[r], val_str);
                set_changed_text_color(vals[r], lv_color_hex(UI_HEX_CYAN));
            } else {
                lv_snprintf(val_buf, sizeof(val_buf), "%s", val);
                val_str = val_buf;
                set_changed_text(vals[r], val_str);
                set_changed_text_color(vals[r], lv_color_hex((idx == 9 && sel && m->vbus_present) ? UI_HEX_CYAN : UI_HEX_DIM));
            }
        }
        int32_t vw = lv_text_get_width(val_str, (uint32_t)lv_strlen(val_str), &ui_font_sm, 0) + 2;
        if (vw < 54) vw = 54;
        if (vw > 96) vw = 96;
        int32_t vx = 146 - vw;
        int32_t lw = vx - 8;
        if (lw < 36) lw = 36;
        set_changed_width(lbls[r], lw);
        set_changed_x(vals[r], vx);
        set_changed_width(vals[r], vw);
    }
    bind_scrollbar(objects.set_scroll_thumb, UI_SET_COUNT, m->set_scroll, 3, 50);

    if (m->set_edit_clock != 0u) {
        set_hidden(objects.set_foot_vu_box, true);
        set_hidden(objects.set_foot_desc, false);
        set_changed_text(objects.set_foot_desc, ui_tr(UI_STR_CLOCK_EDIT_FOOT));
    } else if (m->set_sel == 0) {
        set_hidden(objects.set_foot_desc, true);
        set_hidden(objects.set_foot_vu_box, false);
        set_changed_text(objects.set_foot_vu_lbl, ui_tr(UI_STR_INPUT));
        set_changed_bar_value(objects.set_foot_vu_bar, (int32_t)m->set_input_vu);
    } else {
        set_hidden(objects.set_foot_vu_box, true);
        set_hidden(objects.set_foot_desc, false);
        const char *desc = ui_tr(UI_STR_DESC_ABOUT);
        switch (m->set_sel) {
        case 1: desc = ui_tr((m->cfg.sound_mode == 0u) ? UI_STR_DESC_SND_RAW :
                             (m->cfg.sound_mode == 1u ? UI_STR_DESC_SND_CLARITY : UI_STR_DESC_SND_BYPASS)); break;
        case 2: desc = ui_tr((m->cfg.channels == 1u) ? UI_STR_DESC_MIC_1CH : UI_STR_DESC_MIC_2CH); break;
        case 3: desc = ui_tr(UI_STR_DESC_VOR); break;
        case 4: desc = ui_tr(UI_STR_DESC_BRIGHTNESS); break;
        case 5: desc = ui_tr(UI_STR_DESC_SCREEN_OFF); break;
        case 6: desc = ui_tr(UI_STR_DESC_CLOCK); break;
        case 7: desc = ui_tr(UI_STR_DESC_SKIP); break;
        case 8: desc = ui_tr(UI_STR_DESC_LANGUAGE); break;
        case 9: desc = ui_tr(m->vbus_present ? UI_STR_DESC_USB_MOUNT : UI_STR_DESC_USB_PLUG); break;
        case UI_SET_HSPROBE: desc = ui_tr(UI_STR_DESC_HSPROBE); break;
        default: break;
        }
        set_changed_text(objects.set_foot_desc, desc);
    }
}

static void bind_about(const ui_model_t *m)
{
    bind_status_bar(objects.about_bar, objects.about_bar_ic, objects.about_bar_l, objects.about_bar_sub,
                    objects.about_bar_r, objects.about_bar_hp, objects.about_batt,
                    objects.about_batt_fill, objects.about_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_SET_ABOUT), NULL, NULL, 0);
    set_changed_text(objects.about_title, BOARD_FW_TITLE_STR);
    set_changed_text_color(objects.about_title, lv_color_hex(UI_HEX_TEXT));
    if (lv_label_get_long_mode(objects.about_storage) != LV_LABEL_LONG_WRAP) {
        lv_label_set_long_mode(objects.about_storage, LV_LABEL_LONG_WRAP);
    }
    uint32_t gb10 = (m->sd_total_mb * 10u) / 1024u;
    char st[52], bt[52];
    lv_snprintf(st, sizeof(st), ui_tr(UI_STR_ABOUT_STORAGE_FMT),
                (unsigned)(gb10 / 10u), (unsigned)(gb10 % 10u), (unsigned)(m->sd_free_sec / 3600u));
    lv_snprintf(bt, sizeof(bt), ui_tr(UI_STR_ABOUT_BATT_FMT),
                (unsigned)(m->batt_mv / 1000u), (unsigned)((m->batt_mv % 1000u) / 10u), (unsigned)m->batt_pct);
    set_changed_text(objects.about_storage, st);
    set_changed_text(objects.about_batt_lbl, bt);
    set_changed_text(objects.about_footer, ui_tr(UI_STR_ABOUT_FOOTER));
}

/* Headset button probe: reuses the About screen objects (title, 2 text rows, footer). */
static void bind_hsprobe(const ui_model_t *m)
{
    char sub[8];
    lv_snprintf(sub, sizeof(sub), "%us", (unsigned)m->hsp_secs_left);
    bind_status_bar(objects.about_bar, objects.about_bar_ic, objects.about_bar_l, objects.about_bar_sub,
                    objects.about_bar_r, objects.about_bar_hp, objects.about_batt,
                    objects.about_batt_fill, objects.about_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_SET_HSPROBE), sub, NULL, 0);
    char t1[48], t2[48];
    if (m->hsp_prompt != 0u) {
        lv_snprintf(t1, sizeof(t1), ui_tr(UI_STR_HSP_PRESS_FMT), (unsigned)m->hsp_prompt);
        set_changed_text_color(objects.about_title, lv_color_hex(UI_HEX_AMBER));
    } else {
        lv_snprintf(t1, sizeof(t1), ui_tr(UI_STR_HSP_WAIT_FMT), (unsigned)m->hsp_secs_left);
        set_changed_text_color(objects.about_title, lv_color_hex(UI_HEX_TEXT));
    }
    set_changed_text(objects.about_title, t1);
    if (lv_label_get_long_mode(objects.about_storage) != LV_LABEL_LONG_SCROLL_CIRCULAR) {
        lv_label_set_long_mode(objects.about_storage, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    set_changed_text(objects.about_storage, ui_tr(UI_STR_HSP_HINT));
    /* TODO(i18n): plug state words */
    const char *plug = (m->hsp_plug > 0) ? "YES" : (m->hsp_plug == 0 ? "no" : "?");
    lv_snprintf(t2, sizeof(t2), ui_tr(UI_STR_HSP_PLUG_FMT), plug, (unsigned)m->hsp_phase);
    set_changed_text(objects.about_batt_lbl, t2);
    set_changed_text(objects.about_footer, ui_tr(UI_STR_HSP_FOOTER));
}

static void bind_usb(const ui_model_t *m)
{
    bind_status_bar(objects.usb_bar, objects.usb_bar_ic, objects.usb_bar_l, objects.usb_bar_sub,
                    objects.usb_bar_r, objects.usb_bar_hp, objects.usb_batt,
                    objects.usb_batt_fill, objects.usb_batt_nub,
                    m, UI_COL_BAR, UI_COL_CYAN, UI_ICON_NONE, UI_COL_TEXT, "USB", NULL, NULL, 1);
    set_changed_text(objects.usb_title, ui_tr(UI_STR_SET_USB_DISK));
    set_changed_text(objects.usb_eject_lbl, ui_tr(UI_STR_USB_EJECT));
    char tx_s[16], rx_s[16], tx_f[24], rx_f[24], bp[12];
    ui_format_bytes_short(tx_s, (int)sizeof(tx_s), m->usb_tx_bytes);
    ui_format_bytes_short(rx_s, (int)sizeof(rx_s), m->usb_rx_bytes);
    /* Physical USB port is at the bottom: TX (device -> host) points down,
     * RX (host -> device) points up into the device. */
    lv_snprintf(tx_f, sizeof(tx_f), "\xE2\x96\xBC %s", tx_s);
    lv_snprintf(rx_f, sizeof(rx_f), "\xE2\x96\xB2 %s", rx_s);
    lv_snprintf(bp, sizeof(bp), "+ %u%%", (unsigned)m->batt_pct);
    set_changed_text(objects.usb_tx_lbl, tx_f);
    set_changed_text_color(objects.usb_tx_lbl, lv_color_hex(m->usb_read_act ? UI_HEX_CYAN : UI_HEX_DIM));
    set_changed_text(objects.usb_rx_lbl, rx_f);
    set_changed_text_color(objects.usb_rx_lbl, lv_color_hex(m->usb_write_act ? UI_HEX_AMBER : UI_HEX_DIM));
    set_changed_text(objects.usb_batt_lbl, bp);
}

static void bind_bt(const ui_model_t *m)
{
    bind_status_bar(objects.bt_bar, objects.bt_bar_ic, objects.bt_bar_l, objects.bt_bar_sub,
                    objects.bt_bar_r, objects.bt_bar_hp, objects.bt_batt,
                    objects.bt_batt_fill, objects.bt_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_BT_SYNC), NULL, NULL, 0);
    set_changed_text(objects.bt_name_lbl, m->bt_name[0] ? m->bt_name : "HW_76");
    set_changed_text(objects.bt_l2_lbl, ui_tr(UI_STR_BT_OPEN_APP));
    set_changed_text(objects.bt_footer, ui_tr(UI_STR_BT_FOOTER));
    if (m->bt_state == 0u) {
        set_changed_text(objects.bt_l1_lbl, ui_tr(UI_STR_BT_WAITING));
        set_changed_text_color(objects.bt_l1_lbl, lv_color_hex(UI_HEX_DIM));
        set_hidden(objects.bt_l2_lbl, false);
        set_hidden(objects.bt_prog, true);
    } else if (m->bt_state == 1u) {
        char cs[52];
        lv_snprintf(cs, sizeof(cs), ui_tr(UI_STR_BT_CONNECTED_FMT), m->bt_peer[0] ? m->bt_peer : ui_tr(UI_STR_PHONE));
        set_changed_text(objects.bt_l1_lbl, cs);
        set_changed_text_color(objects.bt_l1_lbl, lv_color_hex(UI_HEX_GREEN));
        set_hidden(objects.bt_l2_lbl, true);
        set_hidden(objects.bt_prog, true);
    } else {
        char ss[40];
        lv_snprintf(ss, sizeof(ss), ui_tr(UI_STR_BT_SENDING_FMT), (unsigned)m->bt_sent, (unsigned)m->bt_total);
        set_changed_text(objects.bt_l1_lbl, ss);
        set_changed_text_color(objects.bt_l1_lbl, lv_color_hex(UI_HEX_TEXT));
        set_hidden(objects.bt_l2_lbl, true);
        set_hidden(objects.bt_prog, false);
        set_changed_bar_value(objects.bt_prog, (int32_t)m->bt_pct);
    }
}

static void bind_charge(const ui_model_t *m)
{
    bind_status_bar(objects.chg_bar, objects.chg_bar_ic, objects.chg_bar_l, objects.chg_bar_sub,
                    objects.chg_bar_r, objects.chg_bar_hp, objects.chg_batt,
                    objects.chg_batt_fill, objects.chg_batt_nub,
                    m, UI_COL_BAR, UI_COL_GREEN, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_CHARGING), NULL, NULL, 0);
    char bp[16];
    lv_snprintf(bp, sizeof(bp), "+ %u%%", (unsigned)m->batt_pct);
    set_changed_text(objects.chg_pct_lbl, bp);
    set_changed_text(objects.chg_footer, ui_tr(UI_STR_CHARGE_FOOTER));
}

/* =========================================================================
 * Public Screen Render Entrypoint
 * ========================================================================= */
static enum ScreensEnum map_eez_screen(ui_screen_id_t scr, const ui_model_t *m)
{
    switch (scr) {
    case UI_SCR_HOME:     return SCREEN_ID_HOME;
    case UI_SCR_REC:      return (!m->sd_ok || m->rec_stopped) ? SCREEN_ID_REC_ERR : SCREEN_ID_REC;
    case UI_SCR_SAVED:    return SCREEN_ID_SAVED;
    case UI_SCR_FILES:    return (m->file_count <= 0 || !m->files) ? SCREEN_ID_FILES_EMPTY : SCREEN_ID_FILES;
    case UI_SCR_PLAYER:   return m->play_has_transcript ? SCREEN_ID_PLAYER_TR : SCREEN_ID_PLAYER;
    case UI_SCR_DELETE:   return SCREEN_ID_DELETE_CONFIRM;
    case UI_SCR_SETTINGS: return SCREEN_ID_SETTINGS;
    case UI_SCR_ABOUT:    return SCREEN_ID_ABOUT;
    case UI_SCR_USB:      return SCREEN_ID_USB;
    case UI_SCR_BT:       return SCREEN_ID_BT;
    case UI_SCR_CHARGE:   return SCREEN_ID_CHARGE;
    case UI_SCR_POWER:    return SCREEN_ID_POWER;
    case UI_SCR_HSPROBE:  return SCREEN_ID_ABOUT;
    case UI_SCR_OFF:
    default:              return SCREEN_ID_OFF;
    }
}

/* Navigation depth: entering a deeper screen pushes FORWARD, a shallower one
 * BACK. Event screens (REC, SAVED, USB, BT) sit deepest so they always slide
 * in from the right and slide back out. -1 = never animated (boot, screen
 * off/wake, power-off hold, charge). */
static int nav_depth(enum ScreensEnum id)
{
    switch (id) {
    case SCREEN_ID_HOME:           return 0;
    case SCREEN_ID_FILES:
    case SCREEN_ID_FILES_EMPTY:
    case SCREEN_ID_SETTINGS:       return 1;
    case SCREEN_ID_PLAYER:
    case SCREEN_ID_PLAYER_TR:
    case SCREEN_ID_ABOUT:          return 2;
    case SCREEN_ID_DELETE_CONFIRM: return 3;
    case SCREEN_ID_REC:
    case SCREEN_ID_REC_ERR:        return 4;
    case SCREEN_ID_SAVED:          return 5;
    case SCREEN_ID_USB:
    case SCREEN_ID_BT:             return 6;
    default:                       return -1;
    }
}

/* Variants of one page swap in place (list <-> empty list, rec <-> rec error,
 * player <-> player with transcript). */
static enum ScreensEnum nav_family(enum ScreensEnum id)
{
    switch (id) {
    case SCREEN_ID_FILES_EMPTY: return SCREEN_ID_FILES;
    case SCREEN_ID_PLAYER_TR:   return SCREEN_ID_PLAYER;
    case SCREEN_ID_REC_ERR:     return SCREEN_ID_REC;
    default:                    return id;
    }
}

_Static_assert(UI_NAV_FORWARD == UI_TRANSITION_FORWARD && UI_NAV_BACK == UI_TRANSITION_BACK,
               "UI_NAV_* must match ui_transition_t");
static uint8_t s_nav_seq_seen;

static ui_transition_t nav_transition(enum ScreensEnum from, enum ScreensEnum to)
{
    int d_from = nav_depth(from);
    int d_to = nav_depth(to);
    if (d_from < 0 || d_to < 0 || nav_family(from) == nav_family(to)) {
        return UI_TRANSITION_NONE;
    }
    return d_to < d_from ? UI_TRANSITION_BACK : UI_TRANSITION_FORWARD;
}

void ui_render_screen_sync(ui_screen_id_t scr, const ui_model_t *m)
{
    if (!m) return;
    ui_i18n_set_lang(m->cfg.lang);
    ui_port_init();
#ifndef OPENFW_HOST_MMIO
    uint64_t t_start = us_timer_now();
    g_ui_last_blit_us = 0u;
#endif

    enum ScreensEnum target = map_eez_screen(scr, m);
    bool entering = target != s_active_eez_scr;
    /* A new nav_seq (folder enter/leave) pushes even within one page family;
     * plain refreshes, cursor moves and reloads keep nav_seq and never animate. */
    bool nav = m->nav_seq != s_nav_seq_seen;
    s_nav_seq_seen = m->nav_seq;
    if (nav && (m->nav_dir == UI_NAV_FORWARD || m->nav_dir == UI_NAV_BACK) &&
        (!entering || nav_family(s_active_eez_scr) == nav_family(target))) {
        ui_transition((ui_transition_t)m->nav_dir);
        /* Force a flush even if nothing changed, so the push starts now. */
        if (!entering && lv_screen_active()) lv_obj_invalidate(lv_screen_active());
    } else if (entering) {
        ui_transition(nav_transition(s_active_eez_scr, target));
    }
    loadScreen(target);
    s_active_eez_scr = target;

    switch (target) {
    case SCREEN_ID_HOME:           bind_home(m, entering); break;
    case SCREEN_ID_REC:            bind_rec(m, entering); break;
    case SCREEN_ID_REC_ERR:        bind_rec_err(m); break;
    case SCREEN_ID_SAVED:          bind_saved(m); break;
    case SCREEN_ID_FILES:          bind_files(m); break;
    case SCREEN_ID_FILES_EMPTY:
        bind_status_bar(objects.files_e_bar, objects.files_e_bar_ic, objects.files_e_bar_l, objects.files_e_bar_sub,
                        objects.files_e_bar_r, objects.files_e_bar_hp, objects.files_e_batt,
                        objects.files_e_batt_fill, objects.files_e_batt_nub,
                        m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, ui_tr(UI_STR_FILES), NULL, NULL, 0);
        set_changed_text(objects.files_empty_title, ui_tr(UI_STR_NO_RECORDINGS));
        set_changed_text(objects.files_empty_foot, ui_tr(UI_STR_SLIDE_REC_RECORD));
        break;
    case SCREEN_ID_PLAYER:
    case SCREEN_ID_PLAYER_TR:      bind_player(m, entering); break;
    case SCREEN_ID_DELETE_CONFIRM: bind_delete(m); break;
    case SCREEN_ID_SETTINGS:       bind_settings(m); break;
    case SCREEN_ID_ABOUT:
        if (scr == UI_SCR_HSPROBE) bind_hsprobe(m);
        else bind_about(m);
        break;
    case SCREEN_ID_USB:            bind_usb(m); break;
    case SCREEN_ID_BT:             bind_bt(m); break;
    case SCREEN_ID_CHARGE:         bind_charge(m); break;
    case SCREEN_ID_POWER:
        set_changed_text_static(objects.pwr_title, ui_tr(UI_STR_POWER_OFF));
        set_changed_text_static(objects.pwr_sub,
                                ui_tr(m->power_hold_pct == 100u ? UI_STR_POWERING_OFF : UI_STR_KEEP_HOLD_PWR));
        set_changed_bar_value(objects.pwr_hold_bar, (int32_t)m->power_hold_pct);
        ui_toast_update(NULL, 0u);
        ui_port_render_now();
        return;
    case SCREEN_ID_OFF:
    default:
        ui_toast_update(NULL, 0u);
        ui_port_render_now();
        return;
    }

    ui_toast_update(m->toast_msg, m->toast_col);
#ifndef OPENFW_HOST_MMIO
    uint64_t t_mid = us_timer_now();
    g_ui_last_load_us = (uint32_t)(t_mid - t_start);
#endif
    ui_port_render_now();
#ifndef OPENFW_HOST_MMIO
    g_ui_last_draw_us = (uint32_t)(us_timer_now() - t_mid);
#endif
    if (ui_port_is_animating()) ui_port_task_handler();
}
