/*
 * Cassini OpenFW — LVGL v9 UI Screens (All 13 Screens + Live Spectrogram)
 *
 * Uses a single-active-screen lifecycle to keep LVGL heap usage lean (~8-12 KiB)
 * and deterministic: switching screens rebuilds the active screen cleanly;
 * updates on the same screen mutate widgets in-place.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "board_config.h"
#include "openfw_ui.h"
#include "ui_port.h"
#include "ui_theme.h"

/* =========================================================================
 * Active Screen Tracking & Per-Screen Widget Handles
 * ========================================================================= */
static ui_screen_id_t s_cur_scr     = (ui_screen_id_t)0xFF;
static int            s_cur_submode = -1;
static lv_obj_t      *s_root        = NULL;

/* Reusable status bar on the active screen */
static ui_bar_widget_t s_bar;

/* Screen 1: Home */
static struct {
    lv_obj_t *tiles[4];
    lv_obj_t *tile_ic[4];
    lv_obj_t *tile_lbl[4];
    lv_obj_t *line1;
    lv_obj_t *line2;
} s_home;

/* Screen 2: Recording */
static struct {
    lv_obj_t           *timer_lbl;
    lv_obj_t           *gain_pill;
    lv_obj_t           *gain_lbl;
    lv_obj_t           *free_lbl;
    lv_obj_t           *spec_img;
    lv_image_dsc_t      spec_dsc;
    ui_vmeter_widget_t  vm_l;
    ui_vmeter_widget_t  vm_r;
    lv_obj_t           *foot_box;
    lv_obj_t           *foot_left;
    lv_obj_t           *foot_right;
    lv_obj_t           *foot_bar;
    /* Fault / Stopped submode */
    lv_obj_t           *err_title;
    lv_obj_t           *err_l1;
    lv_obj_t           *err_l2;
    lv_obj_t           *err_l3;
} s_rec;

/* Screen 3: Saved */
static struct {
    lv_obj_t *dur_lbl;
    lv_obj_t *meta_lbl;
    lv_obj_t *file_lbl;
    lv_obj_t *prog_bar;
} s_saved;

/* Screen 4: Files */
static struct {
    lv_obj_t *rows[BOARD_UI_LIST_ROWS];
    lv_obj_t *dot[BOARD_UI_LIST_ROWS];
    lv_obj_t *name[BOARD_UI_LIST_ROWS];
    lv_obj_t *badge[BOARD_UI_LIST_ROWS];
    lv_obj_t *dur[BOARD_UI_LIST_ROWS];
    lv_obj_t *scroll_thumb;
} s_files;

/* Screen 5: Player (Time-first & Transcript) */
static struct {
    /* Time-first */
    lv_obj_t *date_lbl;
    lv_obj_t *vol_pill;
    lv_obj_t *vol_lbl;
    lv_obj_t *pos_lbl;
    lv_obj_t *tot_lbl;
    lv_obj_t *vu_l;
    lv_obj_t *vu_r;
    lv_obj_t *prog_bar;
    lv_obj_t *footer;
    /* Transcript */
    lv_obj_t *tr_prog;
    lv_obj_t *tr_prev;
    lv_obj_t *tr_curr_bar;
    lv_obj_t *tr_curr_spoken;
    lv_obj_t *tr_curr_up;
    lv_obj_t *tr_next;
    lv_obj_t *tr_footer;
} s_play;

/* Screen 6: Delete */
static struct {
    lv_obj_t *date_lbl;
    lv_obj_t *sub_lbl;
    lv_obj_t *hold_bar;
    lv_obj_t *footer;
    lv_obj_t *hold_lbl;
} s_del;

/* Screen 7: Settings */
static struct {
    lv_obj_t *rows[BOARD_UI_LIST_ROWS];
    lv_obj_t *lbl[BOARD_UI_LIST_ROWS];
    lv_obj_t *val[BOARD_UI_LIST_ROWS];
    lv_obj_t *scroll_thumb;
    lv_obj_t *foot_desc;
    lv_obj_t *foot_vu_box;
    lv_obj_t *foot_vu_bar;
    lv_obj_t *foot_hints;
} s_set;

/* Screen 8: About */
static struct {
    lv_obj_t *storage_lbl;
    lv_obj_t *batt_lbl;
} s_about;

/* Screen 9: USB */
static struct {
    lv_obj_t *tx_lbl;
    lv_obj_t *rx_lbl;
    lv_obj_t *batt_lbl;
} s_usb;

/* Screen 10: Bluetooth */
static struct {
    lv_obj_t *name_lbl;
    lv_obj_t *l1_lbl;
    lv_obj_t *l2_lbl;
    lv_obj_t *prog_bar;
} s_bt;

/* Screen 11: Charge */
static struct {
    lv_obj_t *pct_lbl;
} s_chg;

/* Screen 12: Power Off */
static struct {
    lv_obj_t *hold_bar;
} s_pwr;

/* =========================================================================
 * Common Helpers
 * ========================================================================= */
static const char *sound_mode_name(uint8_t mode)
{
    if (mode == 0u) return "Raw";
    if (mode == 1u) return "Clarity";
    return "Bypass";
}

static const char *skip_mode_name(uint8_t mode)
{
    if (mode == UI_SKIP_5_10)  return "5 / 10s";
    if (mode == UI_SKIP_10_30) return "10 / 30s";
    return "Track";
}

static const char *screen_off_name(uint8_t idx)
{
    static const char *const names[5] = { "15s", "30s", "1m", "5m", "Never" };
    return (idx < 5u) ? names[idx] : "30s";
}

static const char *weekday_name(uint16_t y, uint8_t m, uint8_t d)
{
    static const char *const wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const uint8_t t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1u || m > 12u) m = 1u;
    int yy = (int)y - (m < 3u ? 1 : 0);
    int wd = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1u] + (int)d) % 7;
    if (wd < 0) wd += 7;
    return wdays[wd];
}

static const char *month_name(uint8_t m)
{
    static const char *const mons[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return (m >= 1u && m <= 12u) ? mons[m - 1u] : "Jan";
}

static void init_sb_common(ui_status_bar_t *sb, const ui_model_t *m)
{
    sb->bg_col         = UI_COL_BAR;
    sb->fg_col         = UI_COL_TEXT;
    sb->left_icon      = UI_ICON_NONE;
    sb->left_icon_col  = UI_COL_TEXT;
    sb->left_text      = "";
    sb->left_sub       = NULL;
    sb->left_sub_col   = UI_COL_DIM;
    sb->right_text     = NULL;
    sb->right_text_col = UI_COL_TEXT;
    sb->no_clock       = 0;
    sb->hour           = m->now.hour;
    sb->minute         = m->now.minute;
    sb->hp_plugged     = m->hp_plugged;
    sb->batt_pct       = m->batt_pct;
    sb->charging       = m->charging;
}

static lv_obj_t *create_prog_bar(lv_obj_t *parent, int32_t w, int32_t h, lv_style_t *ind_st)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &g_st_track, LV_PART_MAIN);
    lv_obj_add_style(bar, ind_st, LV_PART_INDICATOR);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

static lv_obj_t *create_scrollbar_widget(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *track = ui_box_create(parent);
    lv_obj_set_size(track, 2, h);
    lv_obj_set_pos(track, BOARD_DISP_HOR_RES - 5, BOARD_UI_BAR_H + 2);
    lv_obj_set_style_bg_color(track, lv_color_hex(UI_HEX_OFF), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 1, 0);

    lv_obj_t *thumb = ui_box_create(track);
    lv_obj_set_size(thumb, 2, h);
    lv_obj_set_pos(thumb, 0, 0);
    lv_obj_set_style_bg_color(thumb, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, 1, 0);
    return thumb;
}

static void update_scrollbar(lv_obj_t *thumb, int total, int scroll, int visible, int32_t track_h)
{
    if (!thumb) return;
    if (total <= visible) {
        lv_obj_set_y(thumb, 0);
        lv_obj_set_height(thumb, track_h);
        return;
    }
    int32_t thumb_h = (track_h * visible) / total;
    if (thumb_h < 6) thumb_h = 6;
    int32_t max_scroll = total - visible;
    int32_t thumb_y = ((track_h - thumb_h) * scroll) / max_scroll;
    lv_obj_set_y(thumb, thumb_y);
    lv_obj_set_height(thumb, thumb_h);
}

static void reset_screen_root(lv_color_t bg)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &g_st_screen, 0);
    lv_obj_set_style_bg_color(scr, bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_root = scr;
}

void ui_settings_defaults(ui_settings_t *cfg)
{
    cfg->reserved0      = 0u;
    cfg->mic_gain       = 20u;
    cfg->sound_mode     = 0u;
    cfg->channels       = 2u;
    cfg->vor_enabled    = 0u;
    cfg->brightness_idx = 3u;
    cfg->screen_off_idx = UI_SCR_OFF_30S;
    cfg->skip_mode      = UI_SKIP_5_10;
    cfg->hp_vol         = 18u;
    cfg->spk_vol        = 18u;
}

/* =========================================================================
 * Screen 1: Home
 * ========================================================================= */
static void build_home(void)
{
    static const ui_icon_t icons[4]  = { UI_ICON_PLAY, UI_ICON_FILES, UI_ICON_SETUP, UI_ICON_BT };
    static const char *const lbl[4]  = { "Last", "Files", "Setup", "Sync" };

    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    /* 4-tile flex row */
    lv_obj_t *row = ui_box_create(s_root);
    lv_obj_set_size(row, BOARD_DISP_HOR_RES - 6, 37);
    lv_obj_set_pos(row, 3, BOARD_UI_BAR_H + 3);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int32_t tile_w = (BOARD_DISP_HOR_RES - 12) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = ui_box_create(row);
        lv_obj_add_style(t, &g_st_tile, 0);
        lv_obj_set_size(t, tile_w, 37);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(t, 2, 0);

        lv_obj_t *ic = lv_label_create(t);
        lv_obj_set_style_text_font(ic, &ui_font_md, 0);
        lv_label_set_text(ic, ui_icon_glyph(icons[i]));

        lv_obj_t *lb = lv_label_create(t);
        lv_obj_set_style_text_font(lb, &ui_font_sm, 0);
        lv_label_set_text(lb, lbl[i]);

        s_home.tiles[i]    = t;
        s_home.tile_ic[i]  = ic;
        s_home.tile_lbl[i] = lb;
    }

    /* Two centered context lines */
    s_home.line1 = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_home.line1, &ui_font_sm, 0);
    lv_obj_set_width(s_home.line1, BOARD_DISP_HOR_RES - 6);
    lv_obj_set_style_text_align(s_home.line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_home.line1, LV_LABEL_LONG_DOT);
    lv_obj_align(s_home.line1, LV_ALIGN_BOTTOM_MID, 0, -13);

    s_home.line2 = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_home.line2, &ui_font_sm, 0);
    lv_obj_set_width(s_home.line2, BOARD_DISP_HOR_RES - 6);
    lv_obj_set_style_text_align(s_home.line2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_home.line2, LV_LABEL_LONG_DOT);
    lv_obj_align(s_home.line2, LV_ALIGN_BOTTOM_MID, 0, -1);
}

static void update_home(const ui_model_t *m)
{
    char date_str[20];
    lv_snprintf(date_str, sizeof(date_str), "%s %u %s",
                weekday_name(m->now.year, m->now.month, m->now.day),
                (unsigned)m->now.day, month_name(m->now.month));

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.fg_col    = UI_COL_DIM;
    sb.left_text = date_str;
    ui_bar_update(&s_bar, &sb);

    for (int i = 0; i < 4; i++) {
        int focused = ((int)m->home_focus == i);
        lv_obj_set_style_bg_color(s_home.tiles[i],
                                  lv_color_hex(focused ? UI_HEX_PILL : UI_HEX_TILE), 0);
        lv_color_t ink = lv_color_hex(focused ? UI_HEX_TEXT : UI_HEX_DIM);
        lv_obj_set_style_text_color(s_home.tile_ic[i], ink, 0);
        lv_obj_set_style_text_color(s_home.tile_lbl[i], ink, 0);
    }

    char line1[48] = "";
    char line2[48] = "";
    uint32_t col1 = UI_HEX_TEXT;

    if (!m->sd_ok) {
        lv_snprintf(line1, sizeof(line1), "Storage error");
        lv_snprintf(line2, sizeof(line2), "Connect USB to check it");
        col1 = UI_HEX_RED;
    } else if (m->home_focus == 0u) {
        if (!m->has_last) {
            lv_snprintf(line1, sizeof(line1), "Nothing played yet");
            col1 = UI_HEX_DIM;
        } else {
            char dt_s[24], dur_s[12], res_s[12];
            ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->last_file.dt);
            ui_format_duration(dur_s, (int)sizeof(dur_s), m->last_file.duration_sec);
            lv_snprintf(line1, sizeof(line1), "%s \xC2\xB7 %s", dt_s, dur_s);
            if (m->last_file.resume_sec > 0u) {
                ui_format_duration(res_s, (int)sizeof(res_s), m->last_file.resume_sec);
                lv_snprintf(line2, sizeof(line2), "Resume at %s", res_s);
            } else {
                lv_snprintf(line2, sizeof(line2), "Play from start");
            }
        }
    } else if (m->home_focus == 1u) {
        char free_s[20];
        ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
        lv_snprintf(line1, sizeof(line1), "%d %s", m->file_count,
                    (m->file_count == 1) ? "recording" : "recordings");
        lv_snprintf(line2, sizeof(line2), "%s of space left", free_s);
    } else if (m->home_focus == 2u) {
        lv_snprintf(line1, sizeof(line1), "Gain %u \xC2\xB7 %s \xC2\xB7 %s",
                    (unsigned)m->cfg.mic_gain,
                    (m->cfg.channels == 1u) ? "1ch" : "2ch",
                    sound_mode_name(m->cfg.sound_mode));
        lv_snprintf(line2, sizeof(line2), "%s",
                    m->cfg.vor_enabled ? "Voice trigger on" : "Voice trigger off");
    } else {
        lv_snprintf(line1, sizeof(line1), "Bluetooth file sync");
        lv_snprintf(line2, sizeof(line2), "Device name %s",
                    m->bt_name[0] ? m->bt_name : "HW_76");
    }

    lv_label_set_text(s_home.line1, line1);
    lv_obj_set_style_text_color(s_home.line1, lv_color_hex(col1), 0);
    lv_label_set_text(s_home.line2, line2);
    lv_obj_set_style_text_color(s_home.line2, lv_color_hex(UI_HEX_DIM), 0);
}

/* =========================================================================
 * Screen 2: Recording (Active / VOR Wait / Stopped / Storage Fault)
 * ========================================================================= */
static void build_recording(int submode)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    if (submode != 0) {
        /* Storage error (submode=1) or Stopped (submode=2) */
        s_rec.err_title = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_rec.err_title, &ui_font_lg, 0);
        lv_obj_set_pos(s_rec.err_title, 4, 15);

        s_rec.err_l1 = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_rec.err_l1, &ui_font_sm, 0);
        lv_obj_set_pos(s_rec.err_l1, 4, 41);

        s_rec.err_l2 = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_rec.err_l2, &ui_font_sm, 0);
        lv_obj_set_style_text_color(s_rec.err_l2, lv_color_hex(UI_HEX_DIM), 0);
        lv_obj_set_pos(s_rec.err_l2, 4, 53);

        s_rec.err_l3 = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_rec.err_l3, &ui_font_sm, 0);
        lv_obj_set_style_text_color(s_rec.err_l3, lv_color_hex(UI_HEX_DIM), 0);
        lv_obj_set_pos(s_rec.err_l3, 4, 65);
        return;
    }

    /* Active Recording / VOR Wait */
    s_rec.timer_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_rec.timer_lbl, &ui_font_lg, 0);
    lv_obj_set_pos(s_rec.timer_lbl, 3, 15);

    s_rec.gain_pill = ui_box_create(s_root);
    lv_obj_add_style(s_rec.gain_pill, &g_st_pill, 0);
    lv_obj_set_height(s_rec.gain_pill, 13);
    lv_obj_set_width(s_rec.gain_pill, LV_SIZE_CONTENT);
    lv_obj_align(s_rec.gain_pill, LV_ALIGN_TOP_RIGHT, -3, 16);
    s_rec.gain_lbl = lv_label_create(s_rec.gain_pill);
    lv_obj_set_style_text_font(s_rec.gain_lbl, &ui_font_sm, 0);
    lv_obj_center(s_rec.gain_lbl);

    s_rec.free_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_rec.free_lbl, &ui_font_sm, 0);
    lv_obj_align(s_rec.free_lbl, LV_ALIGN_TOP_RIGHT, -4, 29);

    /* Live spectrogram lv_image (zero-copy over ui_spectrogram_t->pixels) */
    s_rec.spec_img = lv_image_create(s_root);
    lv_obj_set_pos(s_rec.spec_img, 2, 42);
    lv_obj_set_size(s_rec.spec_img, UI_SPEC_W, UI_SPEC_H);
    lv_obj_set_style_bg_color(s_rec.spec_img, lv_color_hex(UI_HEX_BLACK), 0);
    lv_obj_set_style_bg_opa(s_rec.spec_img, LV_OPA_COVER, 0);

    ui_vmeter_create(s_root, &s_rec.vm_l, 4, 24);
    lv_obj_set_pos(s_rec.vm_l.root, 148, 42);
    ui_vmeter_create(s_root, &s_rec.vm_r, 4, 24);
    lv_obj_set_pos(s_rec.vm_r.root, 153, 42);

    /* Footer container */
    s_rec.foot_box = ui_box_create(s_root);
    lv_obj_set_size(s_rec.foot_box, BOARD_DISP_HOR_RES - 4, 13);
    lv_obj_set_pos(s_rec.foot_box, 2, 67);
    lv_obj_set_style_radius(s_rec.foot_box, 6, 0);

    s_rec.foot_left = lv_label_create(s_rec.foot_box);
    lv_obj_set_style_text_font(s_rec.foot_left, &ui_font_sm, 0);
    lv_obj_align(s_rec.foot_left, LV_ALIGN_LEFT_MID, 2, 0);

    s_rec.foot_right = lv_label_create(s_rec.foot_box);
    lv_obj_set_style_text_font(s_rec.foot_right, &ui_font_sm, 0);
    lv_obj_align(s_rec.foot_right, LV_ALIGN_RIGHT_MID, -2, 0);

    s_rec.foot_bar = create_prog_bar(s_rec.foot_box, BOARD_DISP_HOR_RES - 12, 5, &g_st_ind_cyan);
    lv_bar_set_range(s_rec.foot_bar, 0, 31);
    lv_obj_center(s_rec.foot_bar);
    lv_obj_add_flag(s_rec.foot_bar, LV_OBJ_FLAG_HIDDEN);
}

static void update_recording(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock = 1;

    if (!m->sd_ok) {
        sb.bg_col        = UI_COL_DRED;
        sb.fg_col        = UI_COL_TEXT;
        sb.left_icon     = UI_ICON_STOP;
        sb.left_icon_col = UI_COL_TEXT;
        sb.left_text     = "Not recording";
        ui_bar_update(&s_bar, &sb);

        lv_label_set_text(s_rec.err_title, "Storage error");
        lv_obj_set_style_text_color(s_rec.err_title, lv_color_hex(UI_HEX_RED), 0);
        lv_label_set_text(s_rec.err_l1, "Nothing is being recorded.");
        lv_label_set_text(s_rec.err_l2, "Slide REC off, then connect");
        lv_label_set_text(s_rec.err_l3, "USB to check the storage.");
        return;
    }

    if (m->rec_stopped) {
        sb.bg_col        = UI_COL_FAINT;
        sb.fg_col        = UI_COL_TEXT;
        sb.left_icon     = UI_ICON_STOP;
        sb.left_icon_col = UI_COL_TEXT;
        sb.left_text     = "Stopped";
        ui_bar_update(&s_bar, &sb);

        lv_label_set_text(s_rec.err_title, m->rec_stop_reason[0] ? m->rec_stop_reason : "Storage full");
        lv_obj_set_style_text_color(s_rec.err_title, lv_color_hex(UI_HEX_AMBER), 0);
        char l1[44], dur[16], l2[32];
        lv_snprintf(l1, sizeof(l1), "Saved %s", m->rec_filename);
        ui_format_duration(dur, (int)sizeof(dur), m->rec_sec);
        lv_snprintf(l2, sizeof(l2), "%s recorded", dur);
        lv_label_set_text(s_rec.err_l1, l1);
        lv_label_set_text(s_rec.err_l2, l2);
        lv_label_set_text(s_rec.err_l3, "Slide REC off to continue");
        return;
    }

    char badge[24];
    lv_snprintf(badge, sizeof(badge), "%s \xC2\xB7 %s",
                (m->cfg.channels == 1u) ? "1ch" : "2ch",
                sound_mode_name(m->cfg.sound_mode));

    if (m->rec_vor_wait) {
        sb.bg_col        = UI_COL_AMBER;
        sb.fg_col        = UI_COL_BLACK;
        sb.left_icon     = UI_ICON_PAUSE;
        sb.left_icon_col = UI_COL_BLACK;
        sb.left_text     = "Waiting for voice";
    } else {
        sb.bg_col         = UI_COL_RED;
        sb.fg_col         = UI_COL_TEXT;
        sb.left_icon      = UI_ICON_DOT;
        sb.left_icon_col  = m->rec_blink ? UI_COL_TEXT : UI_COL_REC_DIM;
        sb.left_text      = "REC";
        sb.right_text     = badge;
        sb.right_text_col = UI_COL_TEXT;
    }
    ui_bar_update(&s_bar, &sb);

    char tbuf[16];
    ui_format_duration(tbuf, (int)sizeof(tbuf), m->rec_sec);
    lv_label_set_text(s_rec.timer_lbl, tbuf);
    lv_obj_set_style_text_color(s_rec.timer_lbl,
                                lv_color_hex(m->rec_vor_wait ? UI_HEX_MUTE : UI_HEX_TEXT), 0);

    char gbuf[16];
    lv_snprintf(gbuf, sizeof(gbuf), "Gain %u", (unsigned)m->cfg.mic_gain);
    lv_label_set_text(s_rec.gain_lbl, gbuf);
    lv_obj_set_style_bg_color(s_rec.gain_pill,
                              lv_color_hex(m->rec_gain_bar ? UI_HEX_CYAN : UI_HEX_TILE), 0);
    lv_obj_set_style_text_color(s_rec.gain_lbl,
                                lv_color_hex(m->rec_gain_bar ? UI_HEX_BLACK : UI_HEX_TEXT), 0);

    char free_s[24], ftxt[32];
    ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
    lv_snprintf(ftxt, sizeof(ftxt), "%s left", free_s);
    lv_label_set_text(s_rec.free_lbl, ftxt);
    lv_obj_set_style_text_color(s_rec.free_lbl,
                                lv_color_hex((m->sd_free_sec < 600u) ? UI_HEX_AMBER : UI_HEX_DIM), 0);

    if (m->spec) {
        s_rec.spec_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_rec.spec_dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
        s_rec.spec_dsc.header.w     = UI_SPEC_W;
        s_rec.spec_dsc.header.h     = UI_SPEC_H;
        s_rec.spec_dsc.header.stride = UI_SPEC_W * 2u;
        s_rec.spec_dsc.data_size    = sizeof(m->spec->pixels);
        s_rec.spec_dsc.data         = (const uint8_t *)m->spec->pixels;
        lv_image_set_src(s_rec.spec_img, &s_rec.spec_dsc);
        lv_obj_invalidate(s_rec.spec_img);
    }

    ui_vmeter_update(&s_rec.vm_l, m->rec_vu_l, m->rec_peak_l, m->rec_clip_l);
    ui_vmeter_update(&s_rec.vm_r,
                     (m->cfg.channels == 1u) ? m->rec_vu_l : m->rec_vu_r,
                     (m->cfg.channels == 1u) ? m->rec_peak_l : m->rec_peak_r,
                     (m->cfg.channels == 1u) ? m->rec_clip_l : m->rec_clip_r);

    /* Footer variants */
    lv_obj_add_flag(s_rec.foot_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_rec.foot_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_rec.foot_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_rec.foot_box, LV_OPA_TRANSP, 0);

    if (m->rec_warn[0] != '\0') {
        lv_color_t wcol = ui_color_from_rgb565(m->rec_warn_col ? m->rec_warn_col : UI_COL_AMBER);
        lv_color_t tfg  = (m->rec_warn_col == UI_COL_RED) ? lv_color_hex(UI_HEX_TEXT) : lv_color_hex(UI_HEX_BLACK);
        lv_obj_set_style_bg_color(s_rec.foot_box, wcol, 0);
        lv_obj_set_style_bg_opa(s_rec.foot_box, LV_OPA_COVER, 0);
        lv_obj_align(s_rec.foot_left, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(s_rec.foot_left, m->rec_warn);
        lv_obj_set_style_text_color(s_rec.foot_left, tfg, 0);
        lv_obj_add_flag(s_rec.foot_right, LV_OBJ_FLAG_HIDDEN);
    } else if (m->rec_gain_bar) {
        lv_obj_set_style_bg_color(s_rec.foot_box, lv_color_hex(UI_HEX_TILE), 0);
        lv_obj_set_style_bg_opa(s_rec.foot_box, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_rec.foot_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rec.foot_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec.foot_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_rec.foot_bar, (int32_t)m->cfg.mic_gain, LV_ANIM_OFF);
    } else if (m->rec_vor_wait) {
        lv_obj_align(s_rec.foot_left, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(s_rec.foot_left, "Resumes when you speak");
        lv_obj_set_style_text_color(s_rec.foot_left, lv_color_hex(UI_HEX_DIM), 0);
        lv_obj_add_flag(s_rec.foot_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_align(s_rec.foot_left, LV_ALIGN_LEFT_MID, 2, 0);
        lv_label_set_text(s_rec.foot_left, m->rec_filename);
        lv_obj_set_style_text_color(s_rec.foot_left, lv_color_hex(UI_HEX_MUTE), 0);
        if (m->rec_markers > 0u) {
            char mb[16];
            lv_snprintf(mb, sizeof(mb), "# %u", (unsigned)m->rec_markers);
            lv_label_set_text(s_rec.foot_right, mb);
            lv_obj_set_style_text_color(s_rec.foot_right, lv_color_hex(UI_HEX_CYAN), 0);
        } else {
            lv_label_set_text(s_rec.foot_right, "OK Mark");
            lv_obj_set_style_text_color(s_rec.foot_right, lv_color_hex(UI_HEX_DIM), 0);
        }
    }
}

void ui_rec_update_live(const ui_model_t *m)
{
    if (!m || s_cur_scr != UI_SCR_REC || s_cur_submode != 0) {
        return;
    }
    if (m->spec && s_rec.spec_img) {
        lv_obj_invalidate(s_rec.spec_img);
    }
    ui_vmeter_update(&s_rec.vm_l, m->rec_vu_l, m->rec_peak_l, m->rec_clip_l);
    ui_vmeter_update(&s_rec.vm_r,
                     (m->cfg.channels == 1u) ? m->rec_vu_l : m->rec_vu_r,
                     (m->cfg.channels == 1u) ? m->rec_peak_l : m->rec_peak_r,
                     (m->cfg.channels == 1u) ? m->rec_clip_l : m->rec_clip_r);
    ui_port_render_now();
}

/* =========================================================================
 * Screen 3: Saved Card
 * ========================================================================= */
static void build_saved(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    s_saved.dur_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_saved.dur_lbl, &ui_font_lg, 0);
    lv_obj_align(s_saved.dur_lbl, LV_ALIGN_TOP_MID, 0, 14);

    s_saved.meta_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_saved.meta_lbl, &ui_font_sm, 0);
    lv_obj_align(s_saved.meta_lbl, LV_ALIGN_TOP_MID, 0, 39);

    s_saved.file_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_saved.file_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_saved.file_lbl, lv_color_hex(UI_HEX_MUTE), 0);
    lv_obj_align(s_saved.file_lbl, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *ft = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    lv_obj_set_y(ft, BOARD_DISP_VER_RES - 16);
    static const ui_hint_t h[2] = {
        { UI_ICON_NONE, UI_ICON_NONE, "OK",   "Play it" },
        { UI_ICON_NONE, UI_ICON_NONE, "BACK", "Done"    },
    };
    ui_footer_set_hints(ft, h, 2);

    s_saved.prog_bar = create_prog_bar(s_root, BOARD_DISP_HOR_RES, 3, &g_st_ind_green);
    lv_obj_set_style_radius(s_saved.prog_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_saved.prog_bar, 0, LV_PART_INDICATOR);
    lv_obj_align(s_saved.prog_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void update_saved(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock      = 1;
    sb.bg_col        = UI_COL_GREEN;
    sb.fg_col        = UI_COL_BLACK;
    sb.left_icon     = UI_ICON_CHECK;
    sb.left_icon_col = UI_COL_BLACK;
    sb.left_text     = "Saved";
    ui_bar_update(&s_bar, &sb);

    char dur[16], dt_s[28], meta[44];
    ui_format_duration(dur, (int)sizeof(dur), m->saved_sec);
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->saved_dt);
    lv_snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s", dt_s,
                (m->saved_channels == 1u) ? "1ch" : "2ch");

    lv_label_set_text(s_saved.dur_lbl, dur);
    lv_label_set_text(s_saved.meta_lbl, meta);
    lv_label_set_text(s_saved.file_lbl, m->saved_filename);
    lv_bar_set_value(s_saved.prog_bar, (int32_t)m->saved_frac_pct, LV_ANIM_OFF);
}

/* =========================================================================
 * Screen 4: Files List
 * ========================================================================= */
static void build_files(int empty)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    if (empty) {
        lv_obj_t *ic = lv_label_create(s_root);
        lv_obj_set_style_text_font(ic, &ui_font_md, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(UI_HEX_FAINT), 0);
        lv_label_set_text(ic, "MIC");
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 20);

        lv_obj_t *t = lv_label_create(s_root);
        lv_obj_set_style_text_font(t, &ui_font_md, 0);
        lv_label_set_text(t, "No recordings yet");
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 4);

        lv_obj_t *ft = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
        static const ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Slide REC", "to record" } };
        ui_footer_set_hints(ft, h, 1);
        return;
    }

    for (int r = 0; r < BOARD_UI_LIST_ROWS; r++) {
        lv_obj_t *row = ui_box_create(s_root);
        lv_obj_add_style(row, &g_st_row, 0);
        lv_obj_set_size(row, BOARD_DISP_HOR_RES - 9, BOARD_UI_ROW_H);
        lv_obj_set_pos(row, 2, BOARD_UI_BAR_H + 2 + r * (BOARD_UI_ROW_H + 1));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 3, 0);

        lv_obj_t *dot = ui_box_create(row);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

        lv_obj_t *nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, &ui_font_md, 0);
        lv_obj_set_flex_grow(nm, 1);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);

        lv_obj_t *bdg = lv_label_create(row);
        lv_obj_set_style_text_font(bdg, &ui_font_sm, 0);
        lv_obj_set_style_radius(bdg, 3, 0);
        lv_obj_set_style_bg_opa(bdg, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(bdg, 2, 0);
        lv_label_set_text(bdg, "T");

        lv_obj_t *dr = lv_label_create(row);
        lv_obj_set_style_text_font(dr, &ui_font_sm, 0);

        s_files.rows[r]  = row;
        s_files.dot[r]   = dot;
        s_files.name[r]  = nm;
        s_files.badge[r] = bdg;
        s_files.dur[r]   = dr;
    }

    s_files.scroll_thumb = create_scrollbar_widget(s_root, 50);

    lv_obj_t *ft = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    static const ui_hint_t h[2] = {
        { UI_ICON_NONE, UI_ICON_NONE, "OK",      "Play"   },
        { UI_ICON_NONE, UI_ICON_NONE, "Hold OK", "Delete" },
    };
    ui_footer_set_hints(ft, h, 2);
}

static void update_files(const ui_model_t *m)
{
    char sub[16] = "";
    if (m->file_count > 0) {
        lv_snprintf(sub, sizeof(sub), "%d / %d", m->file_sel + 1, m->file_count);
    }
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "Files";
    sb.left_sub  = (m->file_count > 0) ? sub : NULL;
    ui_bar_update(&s_bar, &sb);

    if (m->file_count <= 0 || !m->files) {
        return;
    }

    for (int r = 0; r < BOARD_UI_LIST_ROWS; r++) {
        int idx = m->file_scroll + r;
        if (idx >= m->file_count) {
            lv_obj_add_flag(s_files.rows[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_files.rows[r], LV_OBJ_FLAG_HIDDEN);

        const ui_file_entry_t *f = &m->files[idx];
        int sel = (idx == m->file_sel);
        lv_obj_set_style_bg_opa(s_files.rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_files.rows[r], lv_color_hex(UI_HEX_PILL), 0);

        if (f->unplayed) {
            lv_obj_set_style_bg_opa(s_files.dot[r], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_files.dot[r],
                                      lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_CYAN), 0);
        } else {
            lv_obj_set_style_bg_opa(s_files.dot[r], LV_OPA_TRANSP, 0);
        }

        char dt_lbl[28], dur[14];
        ui_format_date_label(dt_lbl, (int)sizeof(dt_lbl), &m->now, &f->dt);
        ui_format_duration(dur, (int)sizeof(dur), f->duration_sec);
        lv_label_set_text(s_files.name[r], dt_lbl);
        lv_label_set_text(s_files.dur[r], dur);
        lv_obj_set_style_text_color(s_files.dur[r],
                                    lv_color_hex(sel ? UI_HEX_CYAN : UI_HEX_DIM), 0);

        if (f->has_transcript) {
            lv_obj_remove_flag(s_files.badge[r], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_files.badge[r],
                                      lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_FAINT), 0);
            lv_obj_set_style_text_color(s_files.badge[r],
                                        lv_color_hex(sel ? UI_HEX_PILL : UI_HEX_TEXT), 0);
        } else {
            lv_obj_add_flag(s_files.badge[r], LV_OBJ_FLAG_HIDDEN);
        }
    }

    update_scrollbar(s_files.scroll_thumb, m->file_count, m->file_scroll, BOARD_UI_LIST_ROWS, 50);
}

/* =========================================================================
 * Screen 5: Player (Time-first & Transcript)
 * ========================================================================= */
static void build_player(int has_tr)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    if (has_tr) {
        s_play.tr_prog = create_prog_bar(s_root, BOARD_DISP_HOR_RES, 2, &g_st_ind_cyan);
        lv_obj_set_style_radius(s_play.tr_prog, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_play.tr_prog, 0, LV_PART_INDICATOR);
        lv_obj_set_pos(s_play.tr_prog, 0, BOARD_UI_BAR_H);

        /* Line 1 (prev) */
        lv_obj_t *b1 = ui_box_create(s_root);
        lv_obj_set_size(b1, 2, 14);
        lv_obj_set_pos(b1, 0, 18);
        lv_obj_set_style_bg_color(b1, lv_color_hex(UI_HEX_OFF), 0);
        lv_obj_set_style_bg_opa(b1, LV_OPA_COVER, 0);

        s_play.tr_prev = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_play.tr_prev, &ui_font_md, 0);
        lv_obj_set_style_text_color(s_play.tr_prev, lv_color_hex(UI_HEX_FAINT), 0);
        lv_obj_set_width(s_play.tr_prev, BOARD_DISP_HOR_RES - 8);
        lv_label_set_long_mode(s_play.tr_prev, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_play.tr_prev, 6, 17);

        /* Line 2 (current spoken + upcoming) */
        s_play.tr_curr_bar = ui_box_create(s_root);
        lv_obj_set_size(s_play.tr_curr_bar, 2, 14);
        lv_obj_set_pos(s_play.tr_curr_bar, 0, 34);
        lv_obj_set_style_bg_opa(s_play.tr_curr_bar, LV_OPA_COVER, 0);

        lv_obj_t *crow = ui_box_create(s_root);
        lv_obj_set_size(crow, BOARD_DISP_HOR_RES - 8, 16);
        lv_obj_set_pos(crow, 6, 33);
        lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_play.tr_curr_spoken = lv_label_create(crow);
        lv_obj_set_style_text_font(s_play.tr_curr_spoken, &ui_font_md, 0);
        lv_obj_set_style_text_color(s_play.tr_curr_spoken, lv_color_hex(UI_HEX_TEXT), 0);

        s_play.tr_curr_up = lv_label_create(crow);
        lv_obj_set_style_text_font(s_play.tr_curr_up, &ui_font_md, 0);
        lv_obj_set_style_text_color(s_play.tr_curr_up, lv_color_hex(UI_HEX_MUTE), 0);
        lv_obj_set_flex_grow(s_play.tr_curr_up, 1);
        lv_label_set_long_mode(s_play.tr_curr_up, LV_LABEL_LONG_DOT);

        /* Line 3 (next) */
        lv_obj_t *b3 = ui_box_create(s_root);
        lv_obj_set_size(b3, 2, 14);
        lv_obj_set_pos(b3, 0, 50);
        lv_obj_set_style_bg_color(b3, lv_color_hex(UI_HEX_OFF), 0);
        lv_obj_set_style_bg_opa(b3, LV_OPA_COVER, 0);

        s_play.tr_next = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_play.tr_next, &ui_font_md, 0);
        lv_obj_set_style_text_color(s_play.tr_next, lv_color_hex(UI_HEX_FAINT), 0);
        lv_obj_set_width(s_play.tr_next, BOARD_DISP_HOR_RES - 8);
        lv_label_set_long_mode(s_play.tr_next, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_play.tr_next, 6, 49);

        s_play.tr_footer = ui_footer_create(s_root, lv_color_hex(UI_HEX_BAR));
        return;
    }

    /* Standard Time-First Player */
    s_play.date_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_play.date_lbl, &ui_font_sm, 0);
    lv_obj_set_width(s_play.date_lbl, 100);
    lv_label_set_long_mode(s_play.date_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_play.date_lbl, 4, 16);

    s_play.vol_pill = ui_box_create(s_root);
    lv_obj_add_style(s_play.vol_pill, &g_st_pill, 0);
    lv_obj_set_height(s_play.vol_pill, 14);
    lv_obj_set_width(s_play.vol_pill, LV_SIZE_CONTENT);
    lv_obj_align(s_play.vol_pill, LV_ALIGN_TOP_RIGHT, -4, 15);
    s_play.vol_lbl = lv_label_create(s_play.vol_pill);
    lv_obj_set_style_text_font(s_play.vol_lbl, &ui_font_sm, 0);
    lv_obj_center(s_play.vol_lbl);

    s_play.pos_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_play.pos_lbl, &ui_font_lg, 0);
    lv_obj_set_pos(s_play.pos_lbl, 3, 29);

    s_play.tot_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_play.tot_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_play.tot_lbl, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_set_pos(s_play.tot_lbl, 66, 39);

    s_play.vu_l = create_prog_bar(s_root, 34, 3, &g_st_ind_green);
    lv_obj_align(s_play.vu_l, LV_ALIGN_TOP_RIGHT, -4, 37);
    s_play.vu_r = create_prog_bar(s_root, 34, 3, &g_st_ind_green);
    lv_obj_align(s_play.vu_r, LV_ALIGN_TOP_RIGHT, -4, 43);

    s_play.prog_bar = create_prog_bar(s_root, BOARD_DISP_HOR_RES - 12, 5, &g_st_ind_cyan);
    lv_obj_set_pos(s_play.prog_bar, 6, 56);

    s_play.footer = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
}

static void update_player(const ui_model_t *m)
{
    uint32_t dur = m->play_file.duration_sec ? m->play_file.duration_sec : 1u;
    uint32_t pos = (m->play_pos_sec <= dur) ? m->play_pos_sec : dur;
    int32_t  pct = (int32_t)((pos * 100u) / dur);

    if (m->play_has_transcript) {
        char pos_s[12], tmp[12], dur_s[16];
        ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
        ui_format_duration(tmp, (int)sizeof(tmp), dur);
        lv_snprintf(dur_s, sizeof(dur_s), "/ %s", tmp);

        ui_status_bar_t sb;
        init_sb_common(&sb, m);
        sb.left_icon     = (m->play_state == 0u) ? UI_ICON_PLAY : UI_ICON_PAUSE;
        sb.left_icon_col = (m->play_state == 0u) ? UI_COL_CYAN : UI_COL_AMBER;
        sb.left_text     = pos_s;
        sb.left_sub      = dur_s;
        ui_bar_update(&s_bar, &sb);

        lv_bar_set_value(s_play.tr_prog, pct, LV_ANIM_OFF);

        static const uint32_t spk_cols[4] = { UI_HEX_CYAN, UI_HEX_AMBER, UI_HEX_GREEN, UI_HEX_PINK };
        lv_obj_set_style_bg_color(s_play.tr_curr_bar, lv_color_hex(spk_cols[m->tr_speaker & 3u]), 0);
        lv_label_set_text(s_play.tr_prev, m->tr_prev);
        lv_label_set_text(s_play.tr_curr_spoken, m->tr_curr_spoken);
        lv_label_set_text(s_play.tr_curr_up, m->tr_curr_upcoming);
        lv_label_set_text(s_play.tr_next, m->tr_next);

        ui_hint_t h[3] = {
            { UI_ICON_LEFT, UI_ICON_RIGHT, NULL, "Line" },
            { UI_ICON_NONE, UI_ICON_NONE,  "OK", (m->play_state == 0u) ? "Pause" : "Play" },
            { UI_ICON_NONE, UI_ICON_NONE,  "\xC2\xB1", "Vol" },
        };
        ui_footer_set_hints(s_play.tr_footer, h, 3);
        return;
    }

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    if (m->play_state == 0u) {
        sb.left_icon = UI_ICON_PLAY; sb.left_icon_col = UI_COL_CYAN;
        sb.fg_col = UI_COL_CYAN;     sb.left_text = "Playing";
    } else if (m->play_state == 1u) {
        sb.left_icon = UI_ICON_PAUSE; sb.left_icon_col = UI_COL_AMBER;
        sb.fg_col = UI_COL_AMBER;     sb.left_text = "Paused";
    } else {
        sb.left_icon = UI_ICON_STOP; sb.left_icon_col = UI_COL_MUTE;
        sb.fg_col = UI_COL_DIM;      sb.left_text = "End";
    }
    ui_bar_update(&s_bar, &sb);

    char dt_s[28];
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->play_file.dt);
    lv_label_set_text(s_play.date_lbl, dt_s);

    uint8_t  vol    = m->hp_plugged ? m->cfg.hp_vol : m->cfg.spk_vol;
    uint32_t tenths = (vol * 100u + 15u) / 31u;
    char vbuf[16];
    if (tenths % 10u != 0u) {
        lv_snprintf(vbuf, sizeof(vbuf), "%s %u.%u", m->hp_plugged ? "HP" : "SPK",
                    (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    } else {
        lv_snprintf(vbuf, sizeof(vbuf), "%s %u", m->hp_plugged ? "HP" : "SPK",
                    (unsigned)(tenths / 10u));
    }
    lv_label_set_text(s_play.vol_lbl, vbuf);
    lv_obj_set_style_bg_color(s_play.vol_pill,
                              lv_color_hex(m->play_vol_hi ? UI_HEX_CYAN : UI_HEX_TILE), 0);
    lv_obj_set_style_text_color(s_play.vol_lbl,
                                lv_color_hex(m->play_vol_hi ? UI_HEX_BLACK : UI_HEX_DIM), 0);

    char pos_s[14], tmp[12], tot_s[16];
    ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
    ui_format_duration(tmp, (int)sizeof(tmp), dur);
    lv_snprintf(tot_s, sizeof(tot_s), "/ %s", tmp);
    lv_label_set_text(s_play.pos_lbl, pos_s);
    lv_obj_set_style_text_color(s_play.pos_lbl,
                                lv_color_hex((m->play_state == 2u) ? UI_HEX_MUTE : UI_HEX_TEXT), 0);
    lv_label_set_text(s_play.tot_lbl, tot_s);

    lv_bar_set_value(s_play.vu_l, (int32_t)m->play_vu_l, LV_ANIM_OFF);
    lv_bar_set_value(s_play.vu_r, (int32_t)m->play_vu_r, LV_ANIM_OFF);
    lv_bar_set_value(s_play.prog_bar, pct, LV_ANIM_OFF);

    if (m->play_state == 2u) {
        ui_hint_t h[2] = {
            { UI_ICON_NONE,  UI_ICON_NONE, "OK", "Replay"    },
            { UI_ICON_RIGHT, UI_ICON_NONE, NULL, "Next file" },
        };
        ui_footer_set_hints(s_play.footer, h, m->play_has_next ? 2 : 1);
    } else {
        const char *l_act = (m->cfg.skip_mode == UI_SKIP_5_10) ? "5s" :
                            (m->cfg.skip_mode == UI_SKIP_10_30 ? "10s" : "Prev");
        const char *r_act = (m->cfg.skip_mode == UI_SKIP_5_10) ? "10s" :
                            (m->cfg.skip_mode == UI_SKIP_10_30 ? "30s" : "Next");
        ui_hint_t h[3] = {
            { UI_ICON_LEFT,  UI_ICON_NONE, NULL, l_act },
            { UI_ICON_NONE,  UI_ICON_NONE, "OK", (m->play_state == 0u) ? "Pause" : "Play" },
            { UI_ICON_RIGHT, UI_ICON_NONE, NULL, r_act },
        };
        ui_footer_set_hints(s_play.footer, h, 3);
    }
}

/* =========================================================================
 * Screen 6: Delete Confirmation
 * ========================================================================= */
static void build_delete(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    s_del.date_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_del.date_lbl, &ui_font_md, 0);
    lv_obj_align(s_del.date_lbl, LV_ALIGN_TOP_MID, 0, 18);

    s_del.sub_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_del.sub_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_del.sub_lbl, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_align(s_del.sub_lbl, LV_ALIGN_TOP_MID, 0, 35);

    s_del.hold_bar = create_prog_bar(s_root, BOARD_DISP_HOR_RES - 20, 7, &g_st_ind_red);
    lv_obj_align(s_del.hold_bar, LV_ALIGN_TOP_MID, 0, 52);

    s_del.footer = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    static const ui_hint_t h[2] = {
        { UI_ICON_NONE, UI_ICON_NONE, "Hold OK", "Delete" },
        { UI_ICON_NONE, UI_ICON_NONE, "BACK",    "Keep"   },
    };
    ui_footer_set_hints(s_del.footer, h, 2);

    s_del.hold_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_del.hold_lbl, &ui_font_sm, 0);
    lv_label_set_text(s_del.hold_lbl, "Keep holding OK\xE2\x80\xA6");
    lv_obj_align(s_del.hold_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void update_delete(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock  = 1;
    sb.bg_col    = UI_COL_RED;
    sb.fg_col    = UI_COL_TEXT;
    sb.left_text = "Delete?";
    ui_bar_update(&s_bar, &sb);

    const ui_file_entry_t *f = &m->play_file;
    if (m->files && m->file_sel >= 0 && m->file_sel < m->file_count) {
        f = &m->files[m->file_sel];
    }

    char dt_s[28], dur_s[12], sub[52];
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &f->dt);
    ui_format_duration(dur_s, (int)sizeof(dur_s), f->duration_sec);
    lv_snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", dur_s, f->filename);

    lv_label_set_text(s_del.date_lbl, dt_s);
    lv_label_set_text(s_del.sub_lbl, sub);
    lv_bar_set_value(s_del.hold_bar, (int32_t)m->del_hold_pct, LV_ANIM_OFF);

    if (m->del_holding) {
        lv_obj_add_flag(s_del.footer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_del.hold_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_del.footer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_del.hold_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* =========================================================================
 * Screen 7: Settings (+ Inline Clock Edit & Live Mic VU)
 * ========================================================================= */
static void format_setting_val(const ui_model_t *m, int idx, char *val, int vcap, int *adj)
{
    static const uint8_t brt_pct[5] = { 20u, 40u, 60u, 80u, 100u };
    *adj = 1;
    switch (idx) {
    case 0: lv_snprintf(val, (size_t)vcap, "%u", (unsigned)m->cfg.mic_gain); break;
    case 1: lv_snprintf(val, (size_t)vcap, "%s", sound_mode_name(m->cfg.sound_mode)); break;
    case 2: lv_snprintf(val, (size_t)vcap, "%s", (m->cfg.channels == 1u) ? "1ch" : "2ch"); break;
    case 3: lv_snprintf(val, (size_t)vcap, "%s", m->cfg.vor_enabled ? "On" : "Off"); break;
    case 4:
        lv_snprintf(val, (size_t)vcap, "%u%%",
                    (unsigned)brt_pct[m->cfg.brightness_idx < 5u ? m->cfg.brightness_idx : 3u]);
        break;
    case 5: lv_snprintf(val, (size_t)vcap, "%s", screen_off_name(m->cfg.screen_off_idx)); break;
    case 6:
        ui_format_clock(val, vcap, m->now.hour, m->now.minute);
        *adj = 0;
        break;
    case 7: lv_snprintf(val, (size_t)vcap, "%s", skip_mode_name(m->cfg.skip_mode)); break;
    case 8:
        lv_snprintf(val, (size_t)vcap, "%s", m->vbus_present ? "Connect" : "Unplugged");
        *adj = 0;
        break;
    default:
        lv_snprintf(val, (size_t)vcap, "%s", BOARD_FW_VERSION_STR);
        *adj = 0;
        break;
    }
}

static void build_settings(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    for (int r = 0; r < BOARD_UI_LIST_ROWS; r++) {
        lv_obj_t *row = ui_box_create(s_root);
        lv_obj_add_style(row, &g_st_row, 0);
        lv_obj_set_size(row, BOARD_DISP_HOR_RES - 9, BOARD_UI_ROW_H);
        lv_obj_set_pos(row, 2, BOARD_UI_BAR_H + 2 + r * (BOARD_UI_ROW_H + 1));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lb = lv_label_create(row);
        lv_obj_set_style_text_font(lb, &ui_font_md, 0);

        lv_obj_t *vl = lv_label_create(row);
        lv_obj_set_style_text_font(vl, &ui_font_sm, 0);

        s_set.rows[r] = row;
        s_set.lbl[r]  = lb;
        s_set.val[r]  = vl;
    }

    s_set.scroll_thumb = create_scrollbar_widget(s_root, 50);

    /* Footer: description text */
    s_set.foot_desc = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_set.foot_desc, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_set.foot_desc, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_set_width(s_set.foot_desc, BOARD_DISP_HOR_RES - 6);
    lv_obj_set_style_text_align(s_set.foot_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_set.foot_desc, LV_ALIGN_BOTTOM_MID, 0, -1);

    /* Footer: live input VU (Mic gain row) */
    s_set.foot_vu_box = ui_box_create(s_root);
    lv_obj_set_size(s_set.foot_vu_box, BOARD_DISP_HOR_RES - 8, 12);
    lv_obj_align(s_set.foot_vu_box, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_flex_flow(s_set.foot_vu_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_set.foot_vu_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_set.foot_vu_box, 5, 0);

    lv_obj_t *ilbl = lv_label_create(s_set.foot_vu_box);
    lv_obj_set_style_text_font(ilbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(ilbl, lv_color_hex(UI_HEX_DIM), 0);
    lv_label_set_text(ilbl, "Input");

    s_set.foot_vu_bar = create_prog_bar(s_set.foot_vu_box, 118, 5, &g_st_ind_green);
    lv_obj_set_flex_grow(s_set.foot_vu_bar, 1);

    /* Footer: clock edit hints */
    s_set.foot_hints = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    static const ui_hint_t ch[3] = {
        { UI_ICON_NONE, UI_ICON_NONE,  "\xC2\xB1", "Set"   },
        { UI_ICON_LEFT, UI_ICON_RIGHT, NULL,       "Field" },
        { UI_ICON_NONE, UI_ICON_NONE,  "OK",       "Save"  },
    };
    ui_footer_set_hints(s_set.foot_hints, ch, 3);
}

static void update_settings(const ui_model_t *m)
{
    static const char *const labels[10] = {
        "Mic gain", "Sound", "Mics", "Voice trigger",
        "Brightness", "Screen off", "Clock", "Skip", "USB disk", "About"
    };

    char sub[12];
    lv_snprintf(sub, sizeof(sub), "%d / 10", m->set_sel + 1);
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "Settings";
    sb.left_sub  = sub;
    ui_bar_update(&s_bar, &sb);

    for (int r = 0; r < BOARD_UI_LIST_ROWS; r++) {
        int idx = m->set_scroll + r;
        if (idx >= 10) {
            lv_obj_add_flag(s_set.rows[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_set.rows[r], LV_OBJ_FLAG_HIDDEN);

        int sel = (idx == m->set_sel);
        lv_obj_set_style_bg_opa(s_set.rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_set.rows[r], lv_color_hex(UI_HEX_PILL), 0);
        lv_label_set_text(s_set.lbl[r], labels[idx]);

        if (idx == 6 && sel && m->set_edit_clock != 0u) {
            char cbuf[16];
            if (m->set_edit_clock == 1u) {
                lv_snprintf(cbuf, sizeof(cbuf), "[%02u]:%02u",
                            (unsigned)m->set_edit_h, (unsigned)m->set_edit_m);
            } else {
                lv_snprintf(cbuf, sizeof(cbuf), "%02u:[%02u]",
                            (unsigned)m->set_edit_h, (unsigned)m->set_edit_m);
            }
            lv_label_set_text(s_set.val[r], cbuf);
            lv_obj_set_style_text_color(s_set.val[r], lv_color_hex(UI_HEX_CYAN), 0);
        } else {
            char val[16], disp[24];
            int adj = 0;
            format_setting_val(m, idx, val, (int)sizeof(val), &adj);
            if (sel && adj) {
                lv_snprintf(disp, sizeof(disp), "- %s +", val);
                lv_label_set_text(s_set.val[r], disp);
                lv_obj_set_style_text_color(s_set.val[r], lv_color_hex(UI_HEX_CYAN), 0);
            } else {
                lv_label_set_text(s_set.val[r], val);
                uint32_t vcol = (idx == 8 && sel && m->vbus_present) ? UI_HEX_CYAN : UI_HEX_DIM;
                lv_obj_set_style_text_color(s_set.val[r], lv_color_hex(vcol), 0);
            }
        }
    }

    update_scrollbar(s_set.scroll_thumb, 10, m->set_scroll, BOARD_UI_LIST_ROWS, 50);

    lv_obj_add_flag(s_set.foot_desc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_set.foot_vu_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_set.foot_hints, LV_OBJ_FLAG_HIDDEN);

    if (m->set_edit_clock != 0u) {
        lv_obj_remove_flag(s_set.foot_hints, LV_OBJ_FLAG_HIDDEN);
    } else if (m->set_sel == 0) {
        lv_obj_remove_flag(s_set.foot_vu_box, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_set.foot_vu_bar, (int32_t)m->set_input_vu, LV_ANIM_OFF);
    } else {
        const char *desc = "";
        switch (m->set_sel) {
        case 1: desc = (m->cfg.sound_mode == 0u) ? "Best for transcripts" :
                       (m->cfg.sound_mode == 1u ? "Cleaner voice, less noise" : "No processing at all"); break;
        case 2: desc = (m->cfg.channels == 1u) ? "One mic, half the file size" : "Both mics, stereo"; break;
        case 3: desc = "Pauses the file during silence"; break;
        case 4: desc = "Lower saves battery"; break;
        case 5: desc = "Recording continues"; break;
        case 6: desc = "OK to edit \xC2\xB7 syncs over USB"; break;
        case 7: desc = "Jump size in the player"; break;
        case 8: desc = m->vbus_present ? "OK to mount as USB drive" : "Plug in USB cable first"; break;
        default: desc = "Version and update mode"; break;
        }
        lv_label_set_text(s_set.foot_desc, desc);
        lv_obj_remove_flag(s_set.foot_desc, LV_OBJ_FLAG_HIDDEN);
    }
}

/* =========================================================================
 * Screen 8: About
 * ========================================================================= */
static void build_about(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    lv_obj_t *title = lv_label_create(s_root);
    lv_obj_set_style_text_font(title, &ui_font_md, 0);
    lv_label_set_text(title, BOARD_FW_TITLE_STR);
    lv_obj_set_pos(title, 4, 17);

    s_about.storage_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_about.storage_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_about.storage_lbl, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_set_pos(s_about.storage_lbl, 4, 34);

    s_about.batt_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_about.batt_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_about.batt_lbl, lv_color_hex(UI_HEX_DIM), 0);
    lv_obj_set_pos(s_about.batt_lbl, 4, 46);

    lv_obj_t *ft = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    static const ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Hold OK", "Update mode" } };
    ui_footer_set_hints(ft, h, 1);
}

static void update_about(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "About";
    ui_bar_update(&s_bar, &sb);

    uint32_t gb10 = (m->sd_total_mb * 10u) / 1024u;
    char st[44], bt[44];
    lv_snprintf(st, sizeof(st), "Storage %u.%u GB \xC2\xB7 %uh left",
                (unsigned)(gb10 / 10u), (unsigned)(gb10 % 10u),
                (unsigned)(m->sd_free_sec / 3600u));
    lv_snprintf(bt, sizeof(bt), "Battery %u.%02u V \xC2\xB7 %u%%",
                (unsigned)(m->batt_mv / 1000u), (unsigned)((m->batt_mv % 1000u) / 10u),
                (unsigned)m->batt_pct);
    lv_label_set_text(s_about.storage_lbl, st);
    lv_label_set_text(s_about.batt_lbl, bt);
}

/* =========================================================================
 * Screen 9: USB Disk
 * ========================================================================= */
static void build_usb(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    lv_obj_t *badge = ui_box_create(s_root);
    lv_obj_add_style(badge, &g_st_tile, 0);
    lv_obj_set_size(badge, 30, 26);
    lv_obj_set_pos(badge, 4, 18);
    lv_obj_t *blbl = lv_label_create(badge);
    lv_obj_set_style_text_font(blbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(blbl, lv_color_hex(UI_HEX_CYAN), 0);
    lv_label_set_text(blbl, "USB");
    lv_obj_center(blbl);

    lv_obj_t *title = lv_label_create(s_root);
    lv_obj_set_style_text_font(title, &ui_font_lg, 0);
    lv_label_set_text(title, "USB disk");
    lv_obj_set_pos(title, 40, 15);

    s_usb.tx_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_usb.tx_lbl, &ui_font_sm, 0);
    lv_obj_set_pos(s_usb.tx_lbl, 40, 43);

    s_usb.rx_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_usb.rx_lbl, &ui_font_sm, 0);
    lv_obj_set_pos(s_usb.rx_lbl, 100, 43);

    s_usb.batt_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_usb.batt_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_usb.batt_lbl, lv_color_hex(UI_HEX_GREEN), 0);
    lv_obj_align(s_usb.batt_lbl, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    lv_obj_t *ej = lv_label_create(s_root);
    lv_obj_set_style_text_font(ej, &ui_font_sm, 0);
    lv_obj_set_style_text_color(ej, lv_color_hex(UI_HEX_DIM), 0);
    lv_label_set_text(ej, "Hold BACK Eject");
    lv_obj_align(ej, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
}

static void update_usb(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock  = 1;
    sb.fg_col    = UI_COL_CYAN;
    sb.left_text = "USB";
    ui_bar_update(&s_bar, &sb);

    char tx_s[16], rx_s[16], tx_f[24], rx_f[24], bp[12];
    ui_format_bytes_short(tx_s, (int)sizeof(tx_s), m->usb_tx_bytes);
    ui_format_bytes_short(rx_s, (int)sizeof(rx_s), m->usb_rx_bytes);
    lv_snprintf(tx_f, sizeof(tx_f), "\xE2\x96\xB2 %s", tx_s);
    lv_snprintf(rx_f, sizeof(rx_f), "\xE2\x96\xBC %s", rx_s);
    lv_snprintf(bp, sizeof(bp), "+ %u%%", (unsigned)m->batt_pct);

    lv_label_set_text(s_usb.tx_lbl, tx_f);
    lv_obj_set_style_text_color(s_usb.tx_lbl,
                                lv_color_hex(m->usb_read_act ? UI_HEX_CYAN : UI_HEX_DIM), 0);
    lv_label_set_text(s_usb.rx_lbl, rx_f);
    lv_obj_set_style_text_color(s_usb.rx_lbl,
                                lv_color_hex(m->usb_write_act ? UI_HEX_AMBER : UI_HEX_DIM), 0);
    lv_label_set_text(s_usb.batt_lbl, bp);
}

/* =========================================================================
 * Screen 10: Bluetooth Sync
 * ========================================================================= */
static void build_bt(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    lv_obj_t *badge = ui_box_create(s_root);
    lv_obj_add_style(badge, &g_st_tile, 0);
    lv_obj_set_size(badge, 26, 26);
    lv_obj_set_pos(badge, 4, 18);
    lv_obj_t *blbl = lv_label_create(badge);
    lv_obj_set_style_text_font(blbl, &ui_font_md, 0);
    lv_obj_set_style_text_color(blbl, lv_color_hex(UI_HEX_CYAN), 0);
    lv_label_set_text(blbl, "BT");
    lv_obj_center(blbl);

    s_bt.name_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_bt.name_lbl, &ui_font_lg, 0);
    lv_obj_set_pos(s_bt.name_lbl, 36, 15);

    s_bt.l1_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_bt.l1_lbl, &ui_font_sm, 0);
    lv_obj_set_pos(s_bt.l1_lbl, 36, 41);

    s_bt.l2_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_bt.l2_lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_bt.l2_lbl, lv_color_hex(UI_HEX_MUTE), 0);
    lv_obj_set_pos(s_bt.l2_lbl, 36, 53);

    s_bt.prog_bar = create_prog_bar(s_root, 118, 4, &g_st_ind_cyan);
    lv_obj_set_pos(s_bt.prog_bar, 36, 56);

    lv_obj_t *ft = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    static const ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "BACK", "Stop sync" } };
    ui_footer_set_hints(ft, h, 1);
}

static void update_bt(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "Bluetooth sync";
    ui_bar_update(&s_bar, &sb);

    lv_label_set_text(s_bt.name_lbl, m->bt_name[0] ? m->bt_name : "HW_76");

    if (m->bt_state == 0u) {
        lv_label_set_text(s_bt.l1_lbl, "Waiting for phone\xE2\x80\xA6");
        lv_obj_set_style_text_color(s_bt.l1_lbl, lv_color_hex(UI_HEX_DIM), 0);
        lv_label_set_text(s_bt.l2_lbl, "Open the Cassini app");
        lv_obj_remove_flag(s_bt.l2_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bt.prog_bar, LV_OBJ_FLAG_HIDDEN);
    } else if (m->bt_state == 1u) {
        char cs[44];
        lv_snprintf(cs, sizeof(cs), "\xE2\x9C\x93 Connected \xC2\xB7 %s",
                    m->bt_peer[0] ? m->bt_peer : "Phone");
        lv_label_set_text(s_bt.l1_lbl, cs);
        lv_obj_set_style_text_color(s_bt.l1_lbl, lv_color_hex(UI_HEX_GREEN), 0);
        lv_obj_add_flag(s_bt.l2_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bt.prog_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        char ss[32];
        lv_snprintf(ss, sizeof(ss), "Sending %u of %u",
                    (unsigned)m->bt_sent, (unsigned)m->bt_total);
        lv_label_set_text(s_bt.l1_lbl, ss);
        lv_obj_set_style_text_color(s_bt.l1_lbl, lv_color_hex(UI_HEX_TEXT), 0);
        lv_obj_add_flag(s_bt.l2_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_bt.prog_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bt.prog_bar, (int32_t)m->bt_pct, LV_ANIM_OFF);
    }
}

/* =========================================================================
 * Screen 11: Charge, Screen 12: Power Off, Screen 13: Off
 * ========================================================================= */
static void build_charge(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BG));
    ui_bar_create(s_root, &s_bar);

    s_chg.pct_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_chg.pct_lbl, &ui_font_lg, 0);
    lv_obj_align(s_chg.pct_lbl, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *ft = ui_footer_create(s_root, lv_color_hex(UI_HEX_BG));
    static const ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Any key", "Use the device" } };
    ui_footer_set_hints(ft, h, 1);
}

static void update_charge(const ui_model_t *m)
{
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.fg_col    = UI_COL_GREEN;
    sb.left_text = "Charging";
    ui_bar_update(&s_bar, &sb);

    char bp[16];
    lv_snprintf(bp, sizeof(bp), "+ %u%%", (unsigned)m->batt_pct);
    lv_label_set_text(s_chg.pct_lbl, bp);
    lv_obj_set_style_text_color(s_chg.pct_lbl, lv_color_hex(UI_HEX_GREEN), 0);
}

static void build_power(void)
{
    reset_screen_root(lv_color_hex(UI_HEX_BLACK));

    lv_obj_t *t = lv_label_create(s_root);
    lv_obj_set_style_text_font(t, &ui_font_lg, 0);
    lv_label_set_text(t, "Power off");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);

    s_pwr.hold_bar = create_prog_bar(s_root, BOARD_DISP_HOR_RES - 48, 6, &g_st_ind_white);
    lv_obj_align(s_pwr.hold_bar, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *sub = lv_label_create(s_root);
    lv_obj_set_style_text_font(sub, &ui_font_sm, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_HEX_DIM), 0);
    lv_label_set_text(sub, "Keep holding POWER");
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void update_power(const ui_model_t *m)
{
    lv_bar_set_value(s_pwr.hold_bar, (int32_t)m->power_hold_pct, LV_ANIM_OFF);
}

/* =========================================================================
 * Public Screen Render Entrypoint
 * ========================================================================= */
static int screen_submode(ui_screen_id_t scr, const ui_model_t *m)
{
    switch (scr) {
    case UI_SCR_REC:
        if (!m->sd_ok) return 1;
        if (m->rec_stopped) return 2;
        return 0;
    case UI_SCR_FILES:
        return (m->file_count <= 0 || !m->files) ? 1 : 0;
    case UI_SCR_PLAYER:
        return m->play_has_transcript ? 1 : 0;
    default:
        return 0;
    }
}

void ui_render_screen(ui_screen_id_t scr, const ui_model_t *m)
{
    if (!m) return;
    ui_port_init();

    int sub = screen_submode(scr, m);
    if (scr != s_cur_scr || sub != s_cur_submode) {
        s_cur_scr     = scr;
        s_cur_submode = sub;
        switch (scr) {
        case UI_SCR_HOME:     build_home();             break;
        case UI_SCR_REC:      build_recording(sub);     break;
        case UI_SCR_SAVED:    build_saved();            break;
        case UI_SCR_FILES:    build_files(sub);         break;
        case UI_SCR_PLAYER:   build_player(sub);        break;
        case UI_SCR_DELETE:   build_delete();           break;
        case UI_SCR_SETTINGS: build_settings();         break;
        case UI_SCR_ABOUT:    build_about();            break;
        case UI_SCR_USB:      build_usb();              break;
        case UI_SCR_BT:       build_bt();               break;
        case UI_SCR_CHARGE:   build_charge();           break;
        case UI_SCR_POWER:    build_power();            break;
        case UI_SCR_OFF:
        default:
            reset_screen_root(lv_color_hex(UI_HEX_BLACK));
            break;
        }
    }

    switch (scr) {
    case UI_SCR_HOME:     update_home(m);      break;
    case UI_SCR_REC:      update_recording(m); break;
    case UI_SCR_SAVED:    update_saved(m);     break;
    case UI_SCR_FILES:    update_files(m);     break;
    case UI_SCR_PLAYER:   update_player(m);    break;
    case UI_SCR_DELETE:   update_delete(m);    break;
    case UI_SCR_SETTINGS: update_settings(m);  break;
    case UI_SCR_ABOUT:    update_about(m);     break;
    case UI_SCR_USB:      update_usb(m);       break;
    case UI_SCR_BT:       update_bt(m);        break;
    case UI_SCR_CHARGE:   update_charge(m);    break;
    case UI_SCR_POWER:    update_power(m);     break;
    case UI_SCR_OFF:
    default:
        ui_toast_update(NULL, 0u);
        ui_port_render_now();
        return;
    }

    ui_toast_update(m->toast_msg, m->toast_col);
    ui_port_render_now();
}
