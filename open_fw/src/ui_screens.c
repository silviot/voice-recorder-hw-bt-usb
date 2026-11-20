/*
 * Cassini OpenFW — All 11 Landscape (160x80) Anti-Aliased UI Screens
 *
 * Implements the exact pixel geometry, 14-color RGB565 palette, typography,
 * vector icons, live spectrogram, transcript view, and system states.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw_ui.h"

/* Freestanding string helpers (zero libc dependencies) */
static void s_copy(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] != '\0' && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void s_append(char *dst, int cap, const char *src)
{
    int len = 0;
    while (len < cap - 1 && dst[len] != '\0') len++;
    s_copy(dst + len, cap - len, src);
}

static void s_append_u32(char *dst, int cap, uint32_t v)
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
    char out[12];
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    s_append(dst, cap, out);
}

static void s_append_2d(char *dst, int cap, uint32_t v)
{
    char tmp[3];
    tmp[0] = (char)('0' + ((v / 10u) % 10u));
    tmp[1] = (char)('0' + (v % 10u));
    tmp[2] = '\0';
    s_append(dst, cap, tmp);
}

static void draw_centered_bl(int16_t cx, int16_t bl, const char *s,
                             const aa_font_t *font, uint16_t fg, uint16_t bg)
{
    uint16_t w = lcd_measure_str_aa(s, font);
    lcd_draw_str_aa_bl((int16_t)(cx - (int16_t)(w / 2u)), bl, s, font, fg, bg);
}

static void draw_right_bl(int16_t rx, int16_t bl, const char *s,
                          const aa_font_t *font, uint16_t fg, uint16_t bg)
{
    uint16_t w = lcd_measure_str_aa(s, font);
    lcd_draw_str_aa_bl((int16_t)(rx - (int16_t)w), bl, s, font, fg, bg);
}

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
    static const char * const names[5] = { "15s", "30s", "1m", "5m", "Never" };
    return (idx < 5u) ? names[idx] : "30s";
}

static const char *weekday_name(uint16_t y, uint8_t m, uint8_t d)
{
    static const char * const wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const uint8_t t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1u || m > 12u) m = 1u;
    int yy = (int)y - (m < 3u ? 1 : 0);
    int wd = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1u] + (int)d) % 7;
    if (wd < 0) wd += 7;
    return wdays[wd];
}

static const char *month_name(uint8_t m)
{
    static const char * const mons[12] = {
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
    sb->left_sub       = (const char *)0;
    sb->left_sub_col   = UI_COL_DIM;
    sb->right_text     = (const char *)0;
    sb->right_text_col = UI_COL_TEXT;
    sb->no_clock       = 0;
    sb->hour           = m->now.hour;
    sb->minute         = m->now.minute;
    sb->hp_plugged     = m->hp_plugged;
    sb->batt_pct       = m->batt_pct;
    sb->charging       = m->charging;
}

static void draw_scrollbar(int total, int scroll, int visible)
{
    lcd_fill_rrect_aa(155, 15, 2, 50, 1, UI_COL_OFF, UI_COL_BG);
    if (total <= visible) {
        lcd_fill_rrect_aa(155, 15, 2, 50, 1, UI_COL_DIM, UI_COL_OFF);
        return;
    }
    int thumb_h = (50 * visible) / total;
    if (thumb_h < 6) thumb_h = 6;
    int max_scroll = total - visible;
    int thumb_y = 15 + ((50 - thumb_h) * scroll) / max_scroll;
    lcd_fill_rrect_aa(155, (int16_t)thumb_y, 2, (int16_t)thumb_h, 1, UI_COL_DIM, UI_COL_OFF);
}

void ui_settings_defaults(ui_settings_t *cfg)
{
    cfg->auto_level     = 1u;
    cfg->mic_gain       = 20u;
    cfg->sound_mode     = 0u; /* Raw */
    cfg->channels       = 2u; /* 2ch */
    cfg->vor_enabled    = 0u; /* Off */
    cfg->brightness_idx = 3u; /* 80% */
    cfg->screen_off_idx = UI_SCR_OFF_30S;
    cfg->skip_mode      = UI_SKIP_5_10;
    cfg->hp_vol         = 18u;
    cfg->spk_vol        = 18u;
}

/* =========================================================================
 * Screen 1: Home
 * ========================================================================= */
static void render_home(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);

    /* Status bar: "Fri 20 Nov" */
    char date_str[20];
    date_str[0] = '\0';
    s_append(date_str, (int)sizeof(date_str), weekday_name(m->now.year, m->now.month, m->now.day));
    s_append(date_str, (int)sizeof(date_str), " ");
    s_append_u32(date_str, (int)sizeof(date_str), m->now.day);
    s_append(date_str, (int)sizeof(date_str), " ");
    s_append(date_str, (int)sizeof(date_str), month_name(m->now.month));

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.fg_col    = UI_COL_DIM;
    sb.left_text = date_str;
    ui_draw_status_bar(&sb);

    /* Four 37x37 tiles at x = 3 + 39*i, y = 16..52 */
    static const ui_icon_t icons[4]  = { UI_ICON_PLAY, UI_ICON_FILES, UI_ICON_SETUP, UI_ICON_BT };
    static const char * const lbl[4] = { "Last", "Files", "Setup", "Sync" };

    for (int i = 0; i < 4; i++) {
        int16_t tx = (int16_t)(3 + i * 39);
        int focused = ((int)m->home_focus == i);
        uint16_t bg_tile = focused ? UI_COL_PILL : UI_COL_TILE;
        uint16_t ink     = focused ? UI_COL_TEXT : UI_COL_DIM;
        lcd_fill_rrect_aa(tx, 16, 37, 37, 6, bg_tile, UI_COL_BG);
        ui_draw_icon((int16_t)(tx + 11), 20, 14, icons[i], ink, bg_tile);
        draw_centered_bl((int16_t)(tx + 18), 49, lbl[i], &g_font_ui_sm, ink, bg_tile);
    }

    /* Two 11px context lines at baselines 65, 77 */
    char line1[44];
    char line2[44];
    uint16_t col1 = UI_COL_TEXT;
    uint16_t col2 = UI_COL_DIM;
    line1[0] = '\0';
    line2[0] = '\0';

    if (!m->sd_ok) {
        s_copy(line1, (int)sizeof(line1), "Storage error");
        s_copy(line2, (int)sizeof(line2), "Connect USB to check it");
        col1 = UI_COL_RED;
    } else if (m->home_focus == 0u) {
        if (!m->has_last) {
            s_copy(line1, (int)sizeof(line1), "Nothing played yet");
            col1 = UI_COL_DIM;
        } else {
            char dt_s[24], dur_s[12], res_s[12];
            ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->last_file.dt);
            ui_format_duration(dur_s, (int)sizeof(dur_s), m->last_file.duration_sec);
            s_copy(line1, (int)sizeof(line1), dt_s);
            s_append(line1, (int)sizeof(line1), " \xC2\xB7 ");
            s_append(line1, (int)sizeof(line1), dur_s);
            if (m->last_file.resume_sec > 0u) {
                ui_format_duration(res_s, (int)sizeof(res_s), m->last_file.resume_sec);
                s_copy(line2, (int)sizeof(line2), "Resume at ");
                s_append(line2, (int)sizeof(line2), res_s);
            } else {
                s_copy(line2, (int)sizeof(line2), "Play from start");
            }
        }
    } else if (m->home_focus == 1u) {
        s_append_u32(line1, (int)sizeof(line1), (uint32_t)m->file_count);
        s_append(line1, (int)sizeof(line1), (m->file_count == 1) ? " recording" : " recordings");
        char free_s[20];
        ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
        s_copy(line2, (int)sizeof(line2), free_s);
        s_append(line2, (int)sizeof(line2), " of space left");
    } else if (m->home_focus == 2u) {
        if (m->cfg.auto_level) {
            s_copy(line1, (int)sizeof(line1), "Auto level");
        } else {
            s_copy(line1, (int)sizeof(line1), "Gain ");
            s_append_u32(line1, (int)sizeof(line1), m->cfg.mic_gain);
        }
        s_append(line1, (int)sizeof(line1), " \xC2\xB7 ");
        s_append(line1, (int)sizeof(line1), (m->cfg.channels == 1u) ? "1ch" : "2ch");
        s_append(line1, (int)sizeof(line1), " \xC2\xB7 ");
        s_append(line1, (int)sizeof(line1), sound_mode_name(m->cfg.sound_mode));
        s_copy(line2, (int)sizeof(line2), m->cfg.vor_enabled ? "Voice trigger on" : "Voice trigger off");
    } else {
        s_copy(line1, (int)sizeof(line1), "Bluetooth file sync");
        s_copy(line2, (int)sizeof(line2), "Device name ");
        s_append(line2, (int)sizeof(line2), m->bt_name[0] ? m->bt_name : "HW_76");
    }

    draw_centered_bl(80, 65, line1, &g_font_ui_sm, col1, UI_COL_BG);
    if (line2[0] != '\0') {
        draw_centered_bl(80, 77, line2, &g_font_ui_sm, col2, UI_COL_BG);
    }
}

/* =========================================================================
 * Screen 2: Recording (+ VOR wait, Stopped, Storage fault)
 * ========================================================================= */
static void render_recording(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock = 1;

    if (!m->sd_ok) {
        sb.bg_col        = UI_COL_DRED;
        sb.fg_col        = UI_COL_TEXT;
        sb.left_icon     = UI_ICON_STOP;
        sb.left_icon_col = UI_COL_TEXT;
        sb.left_text     = "Not recording";
        ui_draw_status_bar(&sb);

        lcd_draw_str_aa_bl(4, 37, "Storage error", &g_font_ui_lg, UI_COL_RED, UI_COL_BG);
        lcd_draw_str_aa_bl(4, 52, "Nothing is being recorded.", &g_font_ui_sm, UI_COL_TEXT, UI_COL_BG);
        lcd_draw_str_aa_bl(4, 64, "Slide REC off, then connect", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
        lcd_draw_str_aa_bl(4, 76, "USB to check the storage.", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
        return;
    }

    if (m->rec_stopped) {
        sb.bg_col        = UI_COL_FAINT;
        sb.fg_col        = UI_COL_TEXT;
        sb.left_icon     = UI_ICON_STOP;
        sb.left_icon_col = UI_COL_TEXT;
        sb.left_text     = "Stopped";
        ui_draw_status_bar(&sb);

        const char *reason = m->rec_stop_reason[0] ? m->rec_stop_reason : "Storage full";
        lcd_draw_str_aa_bl(4, 37, reason, &g_font_ui_lg, UI_COL_AMBER, UI_COL_BG);
        char saved_line[44];
        s_copy(saved_line, (int)sizeof(saved_line), "Saved ");
        s_append(saved_line, (int)sizeof(saved_line), m->rec_filename);
        lcd_draw_str_aa_bl(4, 52, saved_line, &g_font_ui_sm, UI_COL_TEXT, UI_COL_BG);
        char dur_line[32];
        ui_format_duration(dur_line, (int)sizeof(dur_line), m->rec_sec);
        s_append(dur_line, (int)sizeof(dur_line), " recorded");
        lcd_draw_str_aa_bl(4, 64, dur_line, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
        ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Slide REC off", "to continue" } };
        ui_draw_footer_hints(77, h, 1, UI_COL_BG);
        return;
    }

    /* Active Recording or VOR waiting */
    char badge[24];
    s_copy(badge, (int)sizeof(badge), (m->cfg.channels == 1u) ? "1ch" : "2ch");
    s_append(badge, (int)sizeof(badge), " \xC2\xB7 ");
    s_append(badge, (int)sizeof(badge), sound_mode_name(m->cfg.sound_mode));

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
    ui_draw_status_bar(&sb);

    /* Elapsed time (22px bold tabular) */
    char tbuf[16];
    ui_format_duration(tbuf, (int)sizeof(tbuf), m->rec_sec);
    lcd_draw_str_aa_bl(3, 36, tbuf, &g_font_ui_lg,
                       m->rec_vor_wait ? UI_COL_MUTE : UI_COL_TEXT, UI_COL_BG);

    /* Right column: Auto level / Gain pill + free time left */
    char pill_txt[20];
    uint16_t pill_bg  = UI_COL_TILE;
    uint16_t pill_fg  = UI_COL_DIM;
    if (m->cfg.auto_level) {
        s_copy(pill_txt, (int)sizeof(pill_txt), "Auto level");
    } else {
        s_copy(pill_txt, (int)sizeof(pill_txt), "Gain ");
        s_append_u32(pill_txt, (int)sizeof(pill_txt), m->cfg.mic_gain);
        if (m->rec_gain_bar) {
            pill_bg = UI_COL_CYAN;
            pill_fg = UI_COL_BLACK;
        } else {
            pill_fg = UI_COL_TEXT;
        }
    }
    uint16_t pw = (uint16_t)(lcd_measure_str_aa(pill_txt, &g_font_ui_sm) + 10u);
    int16_t px = (int16_t)(157 - (int16_t)pw);
    lcd_fill_rrect_aa(px, 16, (int16_t)pw, 13, 6, pill_bg, UI_COL_BG);
    draw_right_bl(152, 26, pill_txt, &g_font_ui_sm, pill_fg, pill_bg);

    char free_s[24];
    ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
    s_append(free_s, (int)sizeof(free_s), " left");
    draw_right_bl(156, 38, free_s, &g_font_ui_sm,
                  (m->sd_free_sec < 600u) ? UI_COL_AMBER : UI_COL_DIM, UI_COL_BG);

    /* Live spectrogram (x=2..145, y=42..65) */
    if (m->spec) {
        ui_spec_blit(m->spec, 2, 42);
    } else {
        lcd_fill_rect(2u, 42u, UI_SPEC_W, UI_SPEC_H, UI_COL_BLACK);
    }

    /* Stereo vertical peak-hold meters (x=148, x=153, y=42..65) */
    ui_draw_vmeter(148, 42, 4, 24, m->rec_vu_l, m->rec_peak_l, m->rec_clip_l, UI_COL_BG);
    ui_draw_vmeter(153, 42, 4, 24,
                   (m->cfg.channels == 1u) ? m->rec_vu_l : m->rec_vu_r,
                   (m->cfg.channels == 1u) ? m->rec_peak_l : m->rec_peak_r,
                   (m->cfg.channels == 1u) ? m->rec_clip_l : m->rec_clip_r,
                   UI_COL_BG);

    /* Footer (y=67..79) */
    if (m->rec_warn[0] != '\0') {
        uint16_t wcol = m->rec_warn_col ? m->rec_warn_col : UI_COL_AMBER;
        uint16_t tfg  = (wcol == UI_COL_RED) ? UI_COL_TEXT : UI_COL_BLACK;
        lcd_fill_rrect_aa(2, 67, 156, 13, 6, wcol, UI_COL_BG);
        draw_centered_bl(80, 77, m->rec_warn, &g_font_ui_sm, tfg, wcol);
    } else if (m->rec_gain_bar && !m->cfg.auto_level) {
        lcd_fill_rrect_aa(2, 68, 156, 11, 5, UI_COL_TILE, UI_COL_BG);
        for (int i = 0; i < 31; i++) {
            int16_t bx = (int16_t)(8 + (i * 46) / 10);
            uint16_t col = (i < (int)m->cfg.mic_gain) ? UI_COL_CYAN : UI_COL_OFF;
            lcd_fill_rrect_aa(bx, 71, 3, 5, 1, col, UI_COL_TILE);
        }
    } else if (m->rec_vor_wait) {
        draw_centered_bl(80, 77, "Resumes when you speak", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
    } else {
        lcd_draw_str_aa_bl(4, 77, m->rec_filename, &g_font_ui_sm, UI_COL_MUTE, UI_COL_BG);
        if (m->rec_markers > 0u) {
            char mbuf[12];
            mbuf[0] = '\0';
            s_append_u32(mbuf, (int)sizeof(mbuf), m->rec_markers);
            uint16_t mw = lcd_measure_str_aa(mbuf, &g_font_ui_sm);
            ui_draw_icon((int16_t)(156 - (int16_t)mw - 10), 69, 8, UI_ICON_FLAG, UI_COL_CYAN, UI_COL_BG);
            draw_right_bl(156, 77, mbuf, &g_font_ui_sm, UI_COL_CYAN, UI_COL_BG);
        } else {
            uint16_t mw = lcd_measure_str_aa("Mark", &g_font_ui_sm);
            draw_right_bl(156, 77, "Mark", &g_font_ui_sm, UI_COL_MUTE, UI_COL_BG);
            draw_right_bl((int16_t)(156 - (int16_t)mw - 3), 77, "OK", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
        }
    }
}

/* =========================================================================
 * Screen 3: Saved Card
 * ========================================================================= */
static void render_saved(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock      = 1;
    sb.bg_col        = UI_COL_GREEN;
    sb.fg_col        = UI_COL_BLACK;
    sb.left_icon     = UI_ICON_CHECK;
    sb.left_icon_col = UI_COL_BLACK;
    sb.left_text     = "Saved";
    ui_draw_status_bar(&sb);

    char dur[16];
    ui_format_duration(dur, (int)sizeof(dur), m->saved_sec);
    draw_centered_bl(80, 36, dur, &g_font_ui_lg, UI_COL_TEXT, UI_COL_BG);

    char meta[36];
    ui_format_date_label(meta, (int)sizeof(meta), &m->now, &m->saved_dt);
    s_append(meta, (int)sizeof(meta), " \xC2\xB7 ");
    s_append(meta, (int)sizeof(meta), (m->saved_channels == 1u) ? "1ch" : "2ch");
    draw_centered_bl(80, 50, meta, &g_font_ui_sm, UI_COL_TEXT, UI_COL_BG);
    draw_centered_bl(80, 61, m->saved_filename, &g_font_ui_sm, UI_COL_MUTE, UI_COL_BG);

    ui_hint_t h[2] = {
        { UI_ICON_NONE, UI_ICON_NONE, "OK",   "Play it" },
        { UI_ICON_NONE, UI_ICON_NONE, "BACK", "Done"    },
    };
    ui_draw_footer_hints(73, h, 2, UI_COL_BG);

    uint16_t bar_w = (uint16_t)((160u * m->saved_frac_pct) / 100u);
    if (bar_w > 0u) {
        lcd_fill_rect(0u, 77u, bar_w, 3u, UI_COL_GREEN);
    }
}

/* =========================================================================
 * Screen 4: Files
 * ========================================================================= */
static void render_files(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);

    char sub[16];
    sub[0] = '\0';
    if (m->file_count > 0) {
        s_append_u32(sub, (int)sizeof(sub), (uint32_t)(m->file_sel + 1));
        s_append(sub, (int)sizeof(sub), " / ");
        s_append_u32(sub, (int)sizeof(sub), (uint32_t)m->file_count);
    }

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "Files";
    sb.left_sub  = (m->file_count > 0) ? sub : (const char *)0;
    ui_draw_status_bar(&sb);

    if (m->file_count <= 0 || !m->files) {
        ui_draw_icon(72, 18, 16, UI_ICON_MIC, UI_COL_FAINT, UI_COL_BG);
        draw_centered_bl(80, 50, "No recordings yet", &g_font_ui_md, UI_COL_TEXT, UI_COL_BG);
        ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Slide REC", "to record" } };
        ui_draw_footer_hints(70, h, 1, UI_COL_BG);
        return;
    }

    for (int r = 0; r < 3; r++) {
        int idx = m->file_scroll + r;
        if (idx >= m->file_count) break;
        const ui_file_entry_t *f = &m->files[idx];
        int16_t y = (int16_t)(15 + r * 17);
        int sel = (idx == m->file_sel);
        uint16_t row_bg = sel ? UI_COL_PILL : UI_COL_BG;
        if (sel) {
            lcd_fill_rrect_aa(2, y, 151, 16, 8, UI_COL_PILL, UI_COL_BG);
        }
        if (f->unplayed) {
            lcd_fill_circle_aa(8, (int16_t)(y + 8), 2, sel ? UI_COL_TEXT : UI_COL_CYAN, row_bg);
        }
        char dur[14];
        ui_format_duration(dur, (int)sizeof(dur), f->duration_sec);
        uint16_t dw = lcd_measure_str_aa(dur, &g_font_ui_sm);
        int16_t dx = (int16_t)(148 - (int16_t)dw);
        draw_right_bl(148, (int16_t)(y + 12), dur, &g_font_ui_sm,
                      sel ? UI_COL_CYAN : UI_COL_DIM, row_bg);

        int16_t right_lim = (int16_t)(dx - 4);
        if (f->has_transcript) {
            int16_t bx = (int16_t)(dx - 13);
            uint16_t bbg = sel ? UI_COL_TEXT : UI_COL_FAINT;
            uint16_t bfg = sel ? UI_COL_PILL : UI_COL_TEXT;
            lcd_fill_rrect_aa(bx, (int16_t)(y + 3), 10, 10, 3, bbg, row_bg);
            draw_centered_bl((int16_t)(bx + 5), (int16_t)(y + 12), "T", &g_font_ui_sm, bfg, bbg);
            right_lim = (int16_t)(bx - 4);
        }

        char dt_lbl[28];
        ui_format_date_label(dt_lbl, (int)sizeof(dt_lbl), &m->now, &f->dt);
        uint16_t max_w = (right_lim > 13) ? (uint16_t)(right_lim - 13) : 20u;
        lcd_draw_str_aa_trunc(13, (int16_t)(y + 12 - (int16_t)g_font_ui_md.ascent),
                              dt_lbl, &g_font_ui_md, max_w, UI_COL_TEXT, row_bg);
    }

    draw_scrollbar(m->file_count, m->file_scroll, 3);

    ui_hint_t h[2] = {
        { UI_ICON_NONE, UI_ICON_NONE, "OK",      "Play"   },
        { UI_ICON_NONE, UI_ICON_NONE, "Hold OK", "Delete" },
    };
    ui_draw_footer_hints(77, h, 2, UI_COL_BG);
}

/* =========================================================================
 * Screen 5: Player (Time-first + Transcript mode)
 * ========================================================================= */
static void render_player(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);
    uint32_t dur = m->play_file.duration_sec ? m->play_file.duration_sec : 1u;
    uint32_t pos = (m->play_pos_sec <= dur) ? m->play_pos_sec : dur;

    if (m->play_has_transcript) {
        char pos_s[12], dur_s[16];
        ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
        s_copy(dur_s, (int)sizeof(dur_s), "/ ");
        char tmp[12];
        ui_format_duration(tmp, (int)sizeof(tmp), dur);
        s_append(dur_s, (int)sizeof(dur_s), tmp);

        ui_status_bar_t sb;
        init_sb_common(&sb, m);
        sb.left_icon     = (m->play_state == 0u) ? UI_ICON_PLAY : UI_ICON_PAUSE;
        sb.left_icon_col = (m->play_state == 0u) ? UI_COL_CYAN : UI_COL_AMBER;
        sb.left_text     = pos_s;
        sb.left_sub      = dur_s;
        ui_draw_status_bar(&sb);

        /* 2px progress strip at y=13 */
        lcd_fill_rect(0u, 13u, UI_W, 2u, UI_COL_OFF);
        uint16_t pw = (uint16_t)((pos * (uint32_t)UI_W) / dur);
        if (pw > 0u) lcd_fill_rect(0u, 13u, pw, 2u, UI_COL_CYAN);

        /* 3 wrapped 14px lines at baselines 30, 46, 62 */
        static const uint16_t spk_cols[4] = { UI_COL_CYAN, UI_COL_AMBER, UI_COL_GREEN, UI_COL_PINK };
        uint16_t scol = spk_cols[m->tr_speaker & 3u];

        lcd_fill_rect(0u, 18u, 2u, 14u, UI_COL_OFF);
        lcd_draw_str_aa_trunc(6, (int16_t)(30 - (int16_t)g_font_ui_md.ascent),
                              m->tr_prev, &g_font_ui_md, 152u, UI_COL_FAINT, UI_COL_BG);

        lcd_fill_rect(0u, 34u, 2u, 14u, scol);
        int16_t cx = 6;
        if (m->tr_curr_spoken[0] != '\0') {
            lcd_draw_str_aa_bl(cx, 46, m->tr_curr_spoken, &g_font_ui_md, UI_COL_TEXT, UI_COL_BG);
            cx = (int16_t)(cx + (int16_t)lcd_measure_str_aa(m->tr_curr_spoken, &g_font_ui_md));
        }
        if (m->tr_curr_upcoming[0] != '\0' && cx < 156) {
            lcd_draw_str_aa_trunc(cx, (int16_t)(46 - (int16_t)g_font_ui_md.ascent),
                                  m->tr_curr_upcoming, &g_font_ui_md,
                                  (uint16_t)(158 - cx), UI_COL_MUTE, UI_COL_BG);
        }

        lcd_fill_rect(0u, 50u, 2u, 14u, UI_COL_OFF);
        lcd_draw_str_aa_trunc(6, (int16_t)(62 - (int16_t)g_font_ui_md.ascent),
                              m->tr_next, &g_font_ui_md, 152u, UI_COL_FAINT, UI_COL_BG);

        /* Footer bar (y=66..79) */
        lcd_fill_rect(0u, 66u, UI_W, 14u, UI_COL_BAR);
        ui_hint_t h[3] = {
            { UI_ICON_LEFT, UI_ICON_RIGHT, (const char *)0, "Line" },
            { UI_ICON_NONE, UI_ICON_NONE,  "OK", (m->play_state == 0u) ? "Pause" : "Play" },
            { UI_ICON_NONE, UI_ICON_NONE,  "\xC2\xB1", "Vol" },
        };
        ui_draw_footer_hints(77, h, 3, UI_COL_BAR);
        return;
    }

    /* Standard Time-First Player */
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    if (m->play_state == 0u) {
        sb.left_icon     = UI_ICON_PLAY;
        sb.left_icon_col = UI_COL_CYAN;
        sb.fg_col        = UI_COL_CYAN;
        sb.left_text     = "Playing";
    } else if (m->play_state == 1u) {
        sb.left_icon     = UI_ICON_PAUSE;
        sb.left_icon_col = UI_COL_AMBER;
        sb.fg_col        = UI_COL_AMBER;
        sb.left_text     = "Paused";
    } else {
        sb.left_icon     = UI_ICON_STOP;
        sb.left_icon_col = UI_COL_MUTE;
        sb.fg_col        = UI_COL_DIM;
        sb.left_text     = "End";
    }
    ui_draw_status_bar(&sb);

    /* Row 1: Date + Volume pill */
    char dt_s[28];
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->play_file.dt);
    lcd_draw_str_aa_trunc(4, (int16_t)(27 - (int16_t)g_font_ui_sm.ascent),
                          dt_s, &g_font_ui_sm, 100u, UI_COL_TEXT, UI_COL_BG);

    uint8_t vol = m->hp_plugged ? m->cfg.hp_vol : m->cfg.spk_vol;
    char vbuf[8];
    vbuf[0] = '\0';
    s_append_u32(vbuf, (int)sizeof(vbuf), vol);
    uint16_t vw = (uint16_t)(lcd_measure_str_aa(vbuf, &g_font_ui_sm) + 22u);
    int16_t  vx = (int16_t)(156 - (int16_t)vw);
    uint16_t vbg = m->play_vol_hi ? UI_COL_CYAN : UI_COL_TILE;
    uint16_t vfg = m->play_vol_hi ? UI_COL_BLACK : UI_COL_DIM;
    lcd_fill_rrect_aa(vx, 16, (int16_t)vw, 14, 7, vbg, UI_COL_BG);
    ui_draw_icon((int16_t)(vx + 5), 18, 9, m->hp_plugged ? UI_ICON_HP : UI_ICON_SPK, vfg, vbg);
    draw_right_bl(151, 27, vbuf, &g_font_ui_sm, vfg, vbg);

    /* Row 2: Elapsed lg + / total sm + stereo output bars */
    char pos_s[14], tot_s[16];
    ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
    lcd_draw_str_aa_bl(3, 50, pos_s, &g_font_ui_lg,
                       (m->play_state == 2u) ? UI_COL_MUTE : UI_COL_TEXT, UI_COL_BG);
    int16_t tx = (int16_t)(3 + (int16_t)lcd_measure_str_aa(pos_s, &g_font_ui_lg) + 4);
    s_copy(tot_s, (int)sizeof(tot_s), "/ ");
    char tmp[12];
    ui_format_duration(tmp, (int)sizeof(tmp), dur);
    s_append(tot_s, (int)sizeof(tot_s), tmp);
    lcd_draw_str_aa_bl(tx, 50, tot_s, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);

    int16_t mx = (int16_t)(tx + (int16_t)lcd_measure_str_aa(tot_s, &g_font_ui_sm) + 6);
    if (mx < 136) {
        int16_t mw = (int16_t)(156 - mx);
        ui_draw_hbar(mx, 37, mw, 3, m->play_vu_l, 0u, UI_COL_BG);
        ui_draw_hbar(mx, 43, mw, 3, m->play_vu_r, 0u, UI_COL_BG);
    }

    /* Progress bar (y=56..60) + white AA knob r=4 */
    lcd_fill_rrect_aa(6, 56, 148, 4, 2, UI_COL_OFF, UI_COL_BG);
    uint16_t fill_w = (uint16_t)((pos * 148u) / dur);
    if (fill_w < 4u) fill_w = 4u;
    lcd_fill_rrect_aa(6, 56, (int16_t)fill_w, 4, 2, UI_COL_CYAN, UI_COL_BG);
    lcd_fill_circle_aa((int16_t)(6 + (int16_t)((pos * 148u) / dur)), 58, 4, UI_COL_TEXT, UI_COL_BG);

    /* Footer hints */
    if (m->play_state == 2u) {
        ui_hint_t h[2] = {
            { UI_ICON_NONE,  UI_ICON_NONE, "OK", "Replay"    },
            { UI_ICON_RIGHT, UI_ICON_NONE, (const char *)0, "Next file" },
        };
        ui_draw_footer_hints(76, h, 2, UI_COL_BG);
    } else {
        const char *l_act = (m->cfg.skip_mode == UI_SKIP_5_10) ? "5s" :
                            (m->cfg.skip_mode == UI_SKIP_10_30 ? "10s" : "Prev");
        const char *r_act = (m->cfg.skip_mode == UI_SKIP_5_10) ? "10s" :
                            (m->cfg.skip_mode == UI_SKIP_10_30 ? "30s" : "Next");
        ui_hint_t h[3] = {
            { UI_ICON_LEFT,  UI_ICON_NONE, (const char *)0, l_act },
            { UI_ICON_NONE,  UI_ICON_NONE, "OK", (m->play_state == 0u) ? "Pause" : "Play" },
            { UI_ICON_RIGHT, UI_ICON_NONE, (const char *)0, r_act },
        };
        ui_draw_footer_hints(76, h, 3, UI_COL_BG);
    }
}

/* =========================================================================
 * Screen 6: Delete Confirmation
 * ========================================================================= */
static void render_delete(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock  = 1;
    sb.bg_col    = UI_COL_RED;
    sb.fg_col    = UI_COL_TEXT;
    sb.left_text = "Delete?";
    ui_draw_status_bar(&sb);

    const ui_file_entry_t *f = &m->play_file;
    if (m->files && m->file_sel >= 0 && m->file_sel < m->file_count) {
        f = &m->files[m->file_sel];
    }

    char dt_s[28];
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &f->dt);
    draw_centered_bl(80, 33, dt_s, &g_font_ui_md, UI_COL_TEXT, UI_COL_BG);

    char sub[48], dur_s[12];
    ui_format_duration(dur_s, (int)sizeof(dur_s), f->duration_sec);
    s_copy(sub, (int)sizeof(sub), dur_s);
    s_append(sub, (int)sizeof(sub), " \xC2\xB7 ");
    s_append(sub, (int)sizeof(sub), f->filename);
    draw_centered_bl(80, 46, sub, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);

    lcd_fill_rrect_aa(10, 53, 140, 7, 3, UI_COL_OFF, UI_COL_BG);
    uint16_t fw = (uint16_t)((140u * m->del_hold_pct) / 100u);
    if (fw > 0u) {
        lcd_fill_rrect_aa(10, 53, (int16_t)fw, 7, 3, UI_COL_RED, UI_COL_BG);
    }

    if (m->del_holding) {
        draw_centered_bl(80, 75, "Keep holding OK\xE2\x80\xA6", &g_font_ui_sm, UI_COL_TEXT, UI_COL_BG);
    } else {
        ui_hint_t h[2] = {
            { UI_ICON_NONE, UI_ICON_NONE, "Hold OK", "Delete" },
            { UI_ICON_NONE, UI_ICON_NONE, "BACK",    "Keep"   },
        };
        ui_draw_footer_hints(75, h, 2, UI_COL_BG);
    }
}

/* =========================================================================
 * Screen 7: Settings (+ Clock Edit) & Screen 8: About
 * ========================================================================= */
static void format_setting_val(const ui_model_t *m, int idx, char *val, int vcap, int *adj)
{
    static const uint8_t brt_pct[5] = { 20u, 40u, 60u, 80u, 100u };
    *adj = 1;
    val[0] = '\0';
    switch (idx) {
    case 0: s_copy(val, vcap, m->cfg.auto_level ? "On" : "Off"); break;
    case 1:
        if (m->cfg.auto_level) { s_copy(val, vcap, "Auto"); *adj = 0; }
        else { s_append_u32(val, vcap, m->cfg.mic_gain); }
        break;
    case 2: s_copy(val, vcap, sound_mode_name(m->cfg.sound_mode)); break;
    case 3: s_copy(val, vcap, (m->cfg.channels == 1u) ? "1ch" : "2ch"); break;
    case 4: s_copy(val, vcap, m->cfg.vor_enabled ? "On" : "Off"); break;
    case 5:
        s_append_u32(val, vcap, brt_pct[m->cfg.brightness_idx < 5u ? m->cfg.brightness_idx : 3u]);
        s_append(val, vcap, "%");
        break;
    case 6: s_copy(val, vcap, screen_off_name(m->cfg.screen_off_idx)); break;
    case 7:
        ui_format_clock(val, vcap, m->now.hour, m->now.minute);
        *adj = 0;
        break;
    case 8: s_copy(val, vcap, skip_mode_name(m->cfg.skip_mode)); break;
    default:
        s_copy(val, vcap, "v0.7");
        *adj = 0;
        break;
    }
}

static void render_settings(const ui_model_t *m)
{
    static const char * const labels[10] = {
        "Auto level", "Mic gain", "Sound", "Mics", "Voice trigger",
        "Brightness", "Screen off", "Clock", "Skip", "About"
    };
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);

    char sub[12];
    sub[0] = '\0';
    s_append_u32(sub, (int)sizeof(sub), (uint32_t)(m->set_sel + 1));
    s_append(sub, (int)sizeof(sub), " / 10");

    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "Settings";
    sb.left_sub  = sub;
    ui_draw_status_bar(&sb);

    for (int r = 0; r < 3; r++) {
        int idx = m->set_scroll + r;
        if (idx >= 10) break;
        int16_t y = (int16_t)(15 + r * 17);
        int sel = (idx == m->set_sel);
        int disabled = (idx == 1 && m->cfg.auto_level);
        uint16_t row_bg = sel ? UI_COL_PILL : UI_COL_BG;
        if (sel) {
            lcd_fill_rrect_aa(2, y, 151, 16, 8, UI_COL_PILL, UI_COL_BG);
        }

        lcd_draw_str_aa_bl(9, (int16_t)(y + 12), labels[idx], &g_font_ui_md,
                           disabled ? UI_COL_MUTE : UI_COL_TEXT, row_bg);

        if (idx == 7 && sel && m->set_edit_clock != 0u) {
            /* Inline HH:MM edit with white pill around active field */
            char hh[4], mm[4];
            hh[0] = '\0'; mm[0] = '\0';
            s_append_2d(hh, (int)sizeof(hh), m->set_edit_h);
            s_append_2d(mm, (int)sizeof(mm), m->set_edit_m);
            int16_t hx = 113, mx = 132;
            if (m->set_edit_clock == 1u) {
                lcd_fill_rrect_aa((int16_t)(hx - 2), (int16_t)(y + 2), 16, 12, 3, UI_COL_TEXT, UI_COL_PILL);
                lcd_draw_str_aa_bl(hx, (int16_t)(y + 12), hh, &g_font_ui_sm, UI_COL_PILL, UI_COL_TEXT);
                lcd_draw_str_aa_bl(128, (int16_t)(y + 12), ":", &g_font_ui_sm, UI_COL_TEXT, UI_COL_PILL);
                lcd_draw_str_aa_bl(mx, (int16_t)(y + 12), mm, &g_font_ui_sm, UI_COL_CYAN, UI_COL_PILL);
            } else {
                lcd_draw_str_aa_bl(hx, (int16_t)(y + 12), hh, &g_font_ui_sm, UI_COL_CYAN, UI_COL_PILL);
                lcd_draw_str_aa_bl(128, (int16_t)(y + 12), ":", &g_font_ui_sm, UI_COL_TEXT, UI_COL_PILL);
                lcd_fill_rrect_aa((int16_t)(mx - 2), (int16_t)(y + 2), 16, 12, 3, UI_COL_TEXT, UI_COL_PILL);
                lcd_draw_str_aa_bl(mx, (int16_t)(y + 12), mm, &g_font_ui_sm, UI_COL_PILL, UI_COL_TEXT);
            }
        } else {
            char val[16];
            int adj = 0;
            format_setting_val(m, idx, val, (int)sizeof(val), &adj);
            if (sel && adj) {
                uint16_t vw = lcd_measure_str_aa(val, &g_font_ui_sm);
                draw_right_bl(138, (int16_t)(y + 12), val, &g_font_ui_sm, UI_COL_CYAN, row_bg);
                lcd_draw_str_aa_bl(142, (int16_t)(y + 12), "+", &g_font_ui_sm, UI_COL_TEXT, row_bg);
                lcd_draw_str_aa_bl((int16_t)(138 - (int16_t)vw - 8), (int16_t)(y + 12),
                                   "-", &g_font_ui_sm, UI_COL_TEXT, row_bg);
            } else {
                draw_right_bl(148, (int16_t)(y + 12), val, &g_font_ui_sm,
                              disabled ? UI_COL_MUTE : UI_COL_DIM, row_bg);
            }
        }
    }

    draw_scrollbar(10, m->set_scroll, 3);

    /* Footer explanation / live input meter / clock edit hints */
    if (m->set_edit_clock != 0u) {
        ui_hint_t h[3] = {
            { UI_ICON_NONE, UI_ICON_NONE,  "\xC2\xB1", "Set"   },
            { UI_ICON_LEFT, UI_ICON_RIGHT, (const char *)0, "Field" },
            { UI_ICON_NONE, UI_ICON_NONE,  "OK", "Save"  },
        };
        ui_draw_footer_hints(77, h, 3, UI_COL_BG);
    } else if (m->set_sel == 1 && !m->cfg.auto_level) {
        lcd_draw_str_aa_bl(4, 77, "Input", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
        ui_draw_hbar(36, 70, 120, 5, m->set_input_vu, m->set_input_peak, UI_COL_BG);
    } else {
        const char *desc = "";
        switch (m->set_sel) {
        case 0: desc = m->cfg.auto_level ? "Evens out loud and quiet" : "Fixed gain, set it below"; break;
        case 1: desc = "Turn Auto level off to set"; break;
        case 2: desc = (m->cfg.sound_mode == 0u) ? "Best for transcripts" :
                       (m->cfg.sound_mode == 1u ? "Cleaner voice, less noise" : "No processing at all"); break;
        case 3: desc = (m->cfg.channels == 1u) ? "One mic, half the file size" : "Both mics, stereo"; break;
        case 4: desc = "Pauses the file during silence"; break;
        case 5: desc = "Lower saves battery"; break;
        case 6: desc = "Recording continues"; break;
        case 7: desc = "OK to edit \xC2\xB7 syncs over USB"; break;
        case 8: desc = "Jump size in the player"; break;
        default: desc = "Version and update mode"; break;
        }
        draw_centered_bl(80, 77, desc, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
    }
}

static void render_about(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "About";
    ui_draw_status_bar(&sb);

    lcd_draw_str_aa_bl(4, 30, "Cassini OpenFW v0.7", &g_font_ui_md, UI_COL_TEXT, UI_COL_BG);

    char st[40];
    s_copy(st, (int)sizeof(st), "Storage ");
    uint32_t gb10 = (m->sd_total_mb * 10u) / 1024u;
    s_append_u32(st, (int)sizeof(st), gb10 / 10u);
    s_append(st, (int)sizeof(st), ".");
    s_append_u32(st, (int)sizeof(st), gb10 % 10u);
    s_append(st, (int)sizeof(st), " GB \xC2\xB7 ");
    s_append_u32(st, (int)sizeof(st), m->sd_free_sec / 3600u);
    s_append(st, (int)sizeof(st), "h left");
    lcd_draw_str_aa_bl(4, 44, st, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);

    char bt[40];
    s_copy(bt, (int)sizeof(bt), "Battery ");
    s_append_u32(bt, (int)sizeof(bt), m->batt_mv / 1000u);
    s_append(bt, (int)sizeof(bt), ".");
    s_append_2d(bt, (int)sizeof(bt), (m->batt_mv % 1000u) / 10u);
    s_append(bt, (int)sizeof(bt), " V \xC2\xB7 ");
    s_append_u32(bt, (int)sizeof(bt), m->batt_pct);
    s_append(bt, (int)sizeof(bt), "%");
    lcd_draw_str_aa_bl(4, 56, bt, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);

    ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Hold OK", "Update mode" } };
    ui_draw_footer_hints(76, h, 1, UI_COL_BG);
}

/* =========================================================================
 * System States: USB, Bluetooth, Charge, Power Off, Screen Off
 * ========================================================================= */
static void render_usb(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.no_clock  = 1;
    sb.fg_col    = UI_COL_CYAN;
    sb.left_text = "USB";
    ui_draw_status_bar(&sb);

    ui_draw_icon(6, 18, 26, UI_ICON_USB, UI_COL_CYAN, UI_COL_BG);
    lcd_draw_str_aa_bl(40, 37, "USB disk", &g_font_ui_lg, UI_COL_TEXT, UI_COL_BG);

    ui_draw_icon(40, 46, 7, UI_ICON_UP, m->usb_read_act ? UI_COL_CYAN : UI_COL_FAINT, UI_COL_BG);
    lcd_draw_str_aa_bl(50, 53, "Read", &g_font_ui_sm,
                       m->usb_read_act ? UI_COL_TEXT : UI_COL_DIM, UI_COL_BG);
    ui_draw_icon(88, 46, 7, UI_ICON_DOWN, m->usb_write_act ? UI_COL_AMBER : UI_COL_FAINT, UI_COL_BG);
    lcd_draw_str_aa_bl(98, 53, "Write", &g_font_ui_sm,
                       m->usb_write_act ? UI_COL_TEXT : UI_COL_DIM, UI_COL_BG);

    ui_draw_icon(4, 68, 8, UI_ICON_BOLT, UI_COL_GREEN, UI_COL_BG);
    char bp[8];
    bp[0] = '\0';
    s_append_u32(bp, (int)sizeof(bp), m->batt_pct);
    s_append(bp, (int)sizeof(bp), "%");
    lcd_draw_str_aa_bl(14, 76, bp, &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);

    uint16_t ew = lcd_measure_str_aa("Eject", &g_font_ui_sm);
    draw_right_bl(156, 76, "Eject", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
    draw_right_bl((int16_t)(156 - (int16_t)ew - 3), 76, "Hold BACK", &g_font_ui_sm, UI_COL_TEXT, UI_COL_BG);
}

static void render_bt(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.left_text = "Bluetooth sync";
    ui_draw_status_bar(&sb);

    ui_draw_icon(4, 19, 24, UI_ICON_BT, UI_COL_CYAN, UI_COL_BG);
    lcd_draw_str_aa_bl(36, 37, m->bt_name[0] ? m->bt_name : "HW_76",
                       &g_font_ui_lg, UI_COL_TEXT, UI_COL_BG);

    if (m->bt_state == 0u) {
        lcd_draw_str_aa_bl(36, 52, "Waiting for phone\xE2\x80\xA6", &g_font_ui_sm, UI_COL_DIM, UI_COL_BG);
        lcd_draw_str_aa_bl(36, 64, "Open the Cassini app", &g_font_ui_sm, UI_COL_MUTE, UI_COL_BG);
    } else if (m->bt_state == 1u) {
        ui_draw_icon(36, 45, 8, UI_ICON_CHECK, UI_COL_GREEN, UI_COL_BG);
        char cs[40];
        s_copy(cs, (int)sizeof(cs), "Connected \xC2\xB7 ");
        s_append(cs, (int)sizeof(cs), m->bt_peer[0] ? m->bt_peer : "Phone");
        lcd_draw_str_aa_bl(47, 53, cs, &g_font_ui_sm, UI_COL_GREEN, UI_COL_BG);
    } else {
        char ss[32];
        s_copy(ss, (int)sizeof(ss), "Sending ");
        s_append_u32(ss, (int)sizeof(ss), m->bt_sent);
        s_append(ss, (int)sizeof(ss), " of ");
        s_append_u32(ss, (int)sizeof(ss), m->bt_total);
        lcd_draw_str_aa_bl(36, 52, ss, &g_font_ui_sm, UI_COL_TEXT, UI_COL_BG);
        lcd_fill_rrect_aa(36, 57, 118, 4, 2, UI_COL_OFF, UI_COL_BG);
        uint16_t pw = (uint16_t)((118u * m->bt_pct) / 100u);
        if (pw > 0u) lcd_fill_rrect_aa(36, 57, (int16_t)pw, 4, 2, UI_COL_CYAN, UI_COL_BG);
    }

    ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "BACK", "Stop sync" } };
    ui_draw_footer_hints(77, h, 1, UI_COL_BG);
}

static void render_charge(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BG);
    ui_status_bar_t sb;
    init_sb_common(&sb, m);
    sb.fg_col    = UI_COL_GREEN;
    sb.left_text = "Charging";
    ui_draw_status_bar(&sb);

    ui_draw_icon(30, 20, 26, UI_ICON_BOLT, UI_COL_GREEN, UI_COL_BG);
    char bp[8];
    bp[0] = '\0';
    s_append_u32(bp, (int)sizeof(bp), m->batt_pct);
    s_append(bp, (int)sizeof(bp), "%");
    lcd_draw_str_aa_bl(64, 46, bp, &g_font_ui_lg, UI_COL_TEXT, UI_COL_BG);

    ui_hint_t h[1] = { { UI_ICON_NONE, UI_ICON_NONE, "Any key", "Use the device" } };
    ui_draw_footer_hints(76, h, 1, UI_COL_BG);
}

static void render_power(const ui_model_t *m)
{
    lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BLACK);
    draw_centered_bl(80, 34, "Power off", &g_font_ui_lg, UI_COL_TEXT, UI_COL_BLACK);
    lcd_fill_rrect_aa(24, 44, 112, 6, 3, UI_COL_OFF, UI_COL_BLACK);
    uint16_t fw = (uint16_t)((112u * m->power_hold_pct) / 100u);
    if (fw > 0u) {
        lcd_fill_rrect_aa(24, 44, (int16_t)fw, 6, 3, UI_COL_TEXT, UI_COL_BLACK);
    }
    draw_centered_bl(80, 68, "Keep holding POWER", &g_font_ui_sm, UI_COL_DIM, UI_COL_BLACK);
}

void ui_render_screen(ui_screen_id_t scr, const ui_model_t *m)
{
    if (!m) return;
    lcd_fb_begin();
    switch (scr) {
    case UI_SCR_HOME:     render_home(m);      break;
    case UI_SCR_REC:      render_recording(m); break;
    case UI_SCR_SAVED:    render_saved(m);     break;
    case UI_SCR_FILES:    render_files(m);     break;
    case UI_SCR_PLAYER:   render_player(m);    break;
    case UI_SCR_DELETE:   render_delete(m);    break;
    case UI_SCR_SETTINGS: render_settings(m);  break;
    case UI_SCR_ABOUT:    render_about(m);     break;
    case UI_SCR_USB:      render_usb(m);       break;
    case UI_SCR_BT:       render_bt(m);        break;
    case UI_SCR_CHARGE:   render_charge(m);    break;
    case UI_SCR_POWER:    render_power(m);     break;
    case UI_SCR_OFF:
    default:
        lcd_fill_rect(0u, 0u, UI_W, UI_H, UI_COL_BLACK);
        lcd_fb_flush();
        return;
    }

    if (m->toast_msg[0] != '\0') {
        uint16_t pcol = m->toast_col ? m->toast_col : UI_COL_AMBER;
        ui_draw_toast(m->toast_msg, pcol, UI_COL_BLACK, UI_COL_BG);
    }
    lcd_fb_flush();
}
