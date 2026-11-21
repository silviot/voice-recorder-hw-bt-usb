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

static lv_image_dsc_t s_spec_dsc;
static enum ScreensEnum s_active_eez_scr = (enum ScreensEnum)0;

/* =========================================================================
 * Common Formatting & Helpers
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

static void bind_status_bar(lv_obj_t *bar, lv_obj_t *ic, lv_obj_t *lbl, lv_obj_t *sub,
                            lv_obj_t *r_lbl, lv_obj_t *hp, lv_obj_t *batt,
                            lv_obj_t *batt_fill, lv_obj_t *batt_nub,
                            const ui_model_t *m, uint16_t bg565, uint16_t fg565,
                            ui_icon_t icon, uint16_t icon565,
                            const char *left_txt, const char *sub_txt,
                            const char *right_txt, int no_clock)
{
    if (!bar) return;
    lv_obj_set_style_bg_color(bar, ui_color_from_rgb565(bg565), 0);

    if (ic) {
        if (icon != UI_ICON_NONE) {
            lv_obj_remove_flag(ic, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(ic, ui_icon_glyph(icon));
            lv_obj_set_style_text_color(ic, ui_color_from_rgb565(icon565), 0);
            if (lbl) lv_obj_set_x(lbl, 14);
        } else {
            lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
            if (lbl) lv_obj_set_x(lbl, 3);
        }
    }

    if (lbl) {
        lv_label_set_text(lbl, left_txt ? left_txt : "");
        lv_obj_set_style_text_color(lbl, ui_color_from_rgb565(fg565), 0);
    }

    if (sub) {
        if (sub_txt && sub_txt[0] != '\0') {
            lv_obj_remove_flag(sub, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(sub, sub_txt);
        } else {
            lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (r_lbl) {
        if (no_clock) {
            if (right_txt && right_txt[0] != '\0') {
                lv_obj_remove_flag(r_lbl, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(r_lbl, right_txt);
                lv_obj_set_style_text_color(r_lbl, ui_color_from_rgb565(fg565), 0);
                lv_obj_set_x(r_lbl, 86);
            } else {
                lv_obj_add_flag(r_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            char clk[8];
            ui_format_clock(clk, (int)sizeof(clk), m->now.hour, m->now.minute);
            lv_obj_remove_flag(r_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(r_lbl, clk);
            lv_obj_set_style_text_color(r_lbl, lv_color_hex(UI_HEX_DIM), 0);
            lv_obj_set_x(r_lbl, m->hp_plugged ? 70 : 86);
        }
    }

    if (hp) {
        if (m->hp_plugged && !no_clock) {
            lv_obj_remove_flag(hp, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hp, LV_OBJ_FLAG_HIDDEN);
        }
    }

    uint8_t pct = (m->batt_pct > 100u) ? 100u : m->batt_pct;
    int32_t fw  = (int32_t)((10u * pct + 50u) / 100u);
    if (pct > 0u && fw < 1) fw = 1;
    lv_color_t bcol = m->charging ? lv_color_hex(UI_HEX_GREEN) :
                      (pct <= 15u ? lv_color_hex(UI_HEX_RED) :
                       (pct <= 30u ? lv_color_hex(UI_HEX_AMBER) : lv_color_hex(UI_HEX_TEXT)));
    lv_color_t ocol = (fg565 == UI_COL_BLACK) ? lv_color_hex(UI_HEX_BLACK) : lv_color_hex(UI_HEX_DIM);
    if (batt)      lv_obj_set_style_border_color(batt, ocol, 0);
    if (batt_nub)  lv_obj_set_style_bg_color(batt_nub, ocol, 0);
    if (batt_fill) {
        lv_obj_set_width(batt_fill, fw);
        lv_obj_set_style_bg_color(batt_fill, bcol, 0);
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
        lv_obj_set_style_bg_color(segs[i], lv_color_hex(hex), 0);
    }
}

static void bind_scrollbar(lv_obj_t *thumb, int total, int scroll, int visible, int32_t track_h)
{
    if (!thumb) return;
    if (total <= visible) {
        lv_obj_set_y(thumb, 0);
        lv_obj_set_height(thumb, track_h);
        return;
    }
    int32_t th = (track_h * visible) / total;
    if (th < 6) th = 6;
    int32_t ty = ((track_h - th) * scroll) / (total - visible);
    lv_obj_set_y(thumb, ty);
    lv_obj_set_height(thumb, th);
}

/* =========================================================================
 * Per-Screen Data Binders
 * ========================================================================= */
static void bind_home(const ui_model_t *m)
{
    char date_str[20];
    lv_snprintf(date_str, sizeof(date_str), "%s %u %s",
                weekday_name(m->now.year, m->now.month, m->now.day),
                (unsigned)m->now.day, month_name(m->now.month));
    bind_status_bar(objects.home_bar, objects.home_bar_ic, objects.home_bar_l, objects.home_bar_sub,
                    objects.home_bar_r, objects.home_bar_hp, objects.home_batt,
                    objects.home_batt_fill, objects.home_batt_nub,
                    m, UI_COL_BAR, UI_COL_DIM, UI_ICON_NONE, UI_COL_TEXT, date_str, NULL, NULL, 0);

    lv_obj_t *tiles[4] = { objects.home_tile_0, objects.home_tile_1, objects.home_tile_2, objects.home_tile_3 };
    lv_obj_t *ics[4]   = { objects.home_tile_ic_0, objects.home_tile_ic_1, objects.home_tile_ic_2, objects.home_tile_ic_3 };
    lv_obj_t *lbls[4]  = { objects.home_tile_lbl_0, objects.home_tile_lbl_1, objects.home_tile_lbl_2, objects.home_tile_lbl_3 };
    for (int i = 0; i < 4; i++) {
        int f = ((int)m->home_focus == i);
        lv_obj_set_style_bg_color(tiles[i], lv_color_hex(f ? UI_HEX_PILL : UI_HEX_TILE), 0);
        lv_color_t ink = lv_color_hex(f ? UI_HEX_TEXT : UI_HEX_DIM);
        lv_obj_set_style_text_color(ics[i], ink, 0);
        lv_obj_set_style_text_color(lbls[i], ink, 0);
    }

    char l1[48] = "", l2[48] = "";
    uint32_t col1 = UI_HEX_TEXT;
    if (!m->sd_ok) {
        lv_snprintf(l1, sizeof(l1), "Storage error");
        lv_snprintf(l2, sizeof(l2), "Connect USB to check it");
        col1 = UI_HEX_RED;
    } else if (m->home_focus == 0u) {
        if (!m->has_last) {
            lv_snprintf(l1, sizeof(l1), "Nothing played yet");
            col1 = UI_HEX_DIM;
        } else {
            char dt_s[24], dur_s[12], res_s[12];
            ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->last_file.dt);
            ui_format_duration(dur_s, (int)sizeof(dur_s), m->last_file.duration_sec);
            lv_snprintf(l1, sizeof(l1), "%s \xC2\xB7 %s", dt_s, dur_s);
            if (m->last_file.resume_sec > 0u) {
                ui_format_duration(res_s, (int)sizeof(res_s), m->last_file.resume_sec);
                lv_snprintf(l2, sizeof(l2), "Resume at %s", res_s);
            } else {
                lv_snprintf(l2, sizeof(l2), "Play from start");
            }
        }
    } else if (m->home_focus == 1u) {
        char free_s[20];
        ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
        lv_snprintf(l1, sizeof(l1), "%d %s", m->file_count, (m->file_count == 1) ? "recording" : "recordings");
        lv_snprintf(l2, sizeof(l2), "%s of space left", free_s);
    } else if (m->home_focus == 2u) {
        lv_snprintf(l1, sizeof(l1), "Gain %u \xC2\xB7 %s \xC2\xB7 %s",
                    (unsigned)m->cfg.mic_gain, (m->cfg.channels == 1u) ? "1ch" : "2ch",
                    sound_mode_name(m->cfg.sound_mode));
        lv_snprintf(l2, sizeof(l2), "%s", m->cfg.vor_enabled ? "Voice trigger on" : "Voice trigger off");
    } else {
        lv_snprintf(l1, sizeof(l1), "Bluetooth file sync");
        lv_snprintf(l2, sizeof(l2), "Device name %s", m->bt_name[0] ? m->bt_name : "HW_76");
    }
    lv_label_set_text(objects.home_line1, l1);
    lv_obj_set_style_text_color(objects.home_line1, lv_color_hex(col1), 0);
    lv_label_set_text(objects.home_line2, l2);
}

static void bind_rec(const ui_model_t *m)
{
    char badge[24];
    lv_snprintf(badge, sizeof(badge), "%s \xC2\xB7 %s",
                (m->cfg.channels == 1u) ? "1ch" : "2ch", sound_mode_name(m->cfg.sound_mode));

    if (m->rec_vor_wait) {
        bind_status_bar(objects.rec_bar, objects.rec_bar_ic, objects.rec_bar_l, objects.rec_bar_sub,
                        objects.rec_bar_r, objects.rec_bar_hp, objects.rec_batt,
                        objects.rec_batt_fill, objects.rec_batt_nub,
                        m, UI_COL_AMBER, UI_COL_BLACK, UI_ICON_PAUSE, UI_COL_BLACK,
                        "Waiting for voice", NULL, NULL, 1);
    } else {
        bind_status_bar(objects.rec_bar, objects.rec_bar_ic, objects.rec_bar_l, objects.rec_bar_sub,
                        objects.rec_bar_r, objects.rec_bar_hp, objects.rec_batt,
                        objects.rec_batt_fill, objects.rec_batt_nub,
                        m, UI_COL_RED, UI_COL_TEXT, UI_ICON_DOT,
                        m->rec_blink ? UI_COL_TEXT : UI_COL_REC_DIM, "REC", NULL, badge, 1);
    }

    char tbuf[16], gbuf[16], free_s[24], ftxt[32];
    ui_format_duration(tbuf, (int)sizeof(tbuf), m->rec_sec);
    lv_label_set_text(objects.rec_timer, tbuf);
    lv_obj_set_style_text_color(objects.rec_timer,
                                lv_color_hex(m->rec_vor_wait ? UI_HEX_MUTE : UI_HEX_TEXT), 0);

    lv_snprintf(gbuf, sizeof(gbuf), "Gain %u", (unsigned)m->cfg.mic_gain);
    lv_label_set_text(objects.rec_gain_lbl, gbuf);
    lv_obj_set_style_bg_color(objects.rec_gain_pill,
                              lv_color_hex(m->rec_gain_bar ? UI_HEX_CYAN : UI_HEX_TILE), 0);
    lv_obj_set_style_text_color(objects.rec_gain_lbl,
                                lv_color_hex(m->rec_gain_bar ? UI_HEX_BLACK : UI_HEX_TEXT), 0);

    ui_format_free_time(free_s, (int)sizeof(free_s), m->sd_free_sec);
    lv_snprintf(ftxt, sizeof(ftxt), "%s left", free_s);
    lv_label_set_text(objects.rec_free_lbl, ftxt);
    lv_obj_set_style_text_color(objects.rec_free_lbl,
                                lv_color_hex((m->sd_free_sec < 600u) ? UI_HEX_AMBER : UI_HEX_DIM), 0);

    if (m->spec) {
        s_spec_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_spec_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_spec_dsc.header.w      = UI_SPEC_W;
        s_spec_dsc.header.h      = UI_SPEC_H;
        s_spec_dsc.header.stride = UI_SPEC_W * 2u;
        s_spec_dsc.data_size     = sizeof(m->spec->pixels);
        s_spec_dsc.data          = (const uint8_t *)m->spec->pixels;
        lv_image_set_src(objects.rec_spec_img, &s_spec_dsc);
        lv_obj_invalidate(objects.rec_spec_img);
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

    lv_obj_add_flag(objects.rec_foot_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(objects.rec_foot_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(objects.rec_foot_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(objects.rec_foot_box, LV_OPA_TRANSP, 0);

    if (m->rec_warn[0] != '\0') {
        lv_obj_set_style_bg_color(objects.rec_foot_box,
                                  ui_color_from_rgb565(m->rec_warn_col ? m->rec_warn_col : UI_COL_AMBER), 0);
        lv_obj_set_style_bg_opa(objects.rec_foot_box, LV_OPA_COVER, 0);
        lv_label_set_text(objects.rec_foot_left, m->rec_warn);
        lv_obj_set_style_text_color(objects.rec_foot_left,
                                    lv_color_hex((m->rec_warn_col == UI_COL_RED) ? UI_HEX_TEXT : UI_HEX_BLACK), 0);
        lv_obj_set_style_text_align(objects.rec_foot_left, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(objects.rec_foot_left, 152);
        lv_obj_add_flag(objects.rec_foot_right, LV_OBJ_FLAG_HIDDEN);
    } else if (m->rec_gain_bar) {
        lv_obj_set_style_bg_color(objects.rec_foot_box, lv_color_hex(UI_HEX_TILE), 0);
        lv_obj_set_style_bg_opa(objects.rec_foot_box, LV_OPA_COVER, 0);
        lv_obj_add_flag(objects.rec_foot_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.rec_foot_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.rec_foot_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(objects.rec_foot_bar, (int32_t)m->cfg.mic_gain, LV_ANIM_OFF);
    } else if (m->rec_vor_wait) {
        lv_label_set_text(objects.rec_foot_left, "Resumes when you speak");
        lv_obj_set_style_text_color(objects.rec_foot_left, lv_color_hex(UI_HEX_DIM), 0);
        lv_obj_set_style_text_align(objects.rec_foot_left, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(objects.rec_foot_left, 152);
        lv_obj_add_flag(objects.rec_foot_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(objects.rec_foot_left, m->rec_filename);
        lv_obj_set_style_text_color(objects.rec_foot_left, lv_color_hex(UI_HEX_MUTE), 0);
        lv_obj_set_style_text_align(objects.rec_foot_left, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(objects.rec_foot_left, 108);
        if (m->rec_markers > 0u) {
            char mb[16];
            lv_snprintf(mb, sizeof(mb), "# %u", (unsigned)m->rec_markers);
            lv_label_set_text(objects.rec_foot_right, mb);
            lv_obj_set_style_text_color(objects.rec_foot_right, lv_color_hex(UI_HEX_CYAN), 0);
        } else {
            lv_label_set_text(objects.rec_foot_right, "OK Mark");
            lv_obj_set_style_text_color(objects.rec_foot_right, lv_color_hex(UI_HEX_DIM), 0);
        }
    }
}

void ui_rec_update_live(const ui_model_t *m)
{
    if (!m || s_active_eez_scr != SCREEN_ID_REC) return;
    if (m->spec && objects.rec_spec_img) {
        lv_obj_invalidate(objects.rec_spec_img);
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
                        m, UI_COL_DRED, UI_COL_TEXT, UI_ICON_STOP, UI_COL_TEXT, "Not recording", NULL, NULL, 1);
        lv_label_set_text(objects.rec_err_title, "Storage error");
        lv_obj_set_style_text_color(objects.rec_err_title, lv_color_hex(UI_HEX_RED), 0);
        lv_label_set_text(objects.rec_err_l1, "Nothing is being recorded.");
        lv_label_set_text(objects.rec_err_l2, "Slide REC off, then connect");
        lv_label_set_text(objects.rec_err_l3, "USB to check the storage.");
    } else {
        bind_status_bar(objects.rec_err_bar, objects.rec_err_bar_ic, objects.rec_err_bar_l, objects.rec_err_bar_sub,
                        objects.rec_err_bar_r, objects.rec_err_bar_hp, objects.rec_err_batt,
                        objects.rec_err_batt_fill, objects.rec_err_batt_nub,
                        m, UI_COL_FAINT, UI_COL_TEXT, UI_ICON_STOP, UI_COL_TEXT, "Stopped", NULL, NULL, 1);
        lv_label_set_text(objects.rec_err_title, m->rec_stop_reason[0] ? m->rec_stop_reason : "Storage full");
        lv_obj_set_style_text_color(objects.rec_err_title, lv_color_hex(UI_HEX_AMBER), 0);
        char l1[44], dur[16], l2[32];
        lv_snprintf(l1, sizeof(l1), "Saved %s", m->rec_filename);
        ui_format_duration(dur, (int)sizeof(dur), m->rec_sec);
        lv_snprintf(l2, sizeof(l2), "%s recorded", dur);
        lv_label_set_text(objects.rec_err_l1, l1);
        lv_label_set_text(objects.rec_err_l2, l2);
        lv_label_set_text(objects.rec_err_l3, "Slide REC off to continue");
    }
}

static void bind_saved(const ui_model_t *m)
{
    bind_status_bar(objects.saved_bar, objects.saved_bar_ic, objects.saved_bar_l, objects.saved_bar_sub,
                    objects.saved_bar_r, objects.saved_bar_hp, objects.saved_batt,
                    objects.saved_batt_fill, objects.saved_batt_nub,
                    m, UI_COL_GREEN, UI_COL_BLACK, UI_ICON_CHECK, UI_COL_BLACK, "Saved", NULL, NULL, 1);
    char dur[16], dt_s[28], meta[44];
    ui_format_duration(dur, (int)sizeof(dur), m->saved_sec);
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->saved_dt);
    lv_snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s", dt_s, (m->saved_channels == 1u) ? "1ch" : "2ch");
    lv_label_set_text(objects.saved_dur, dur);
    lv_label_set_text(objects.saved_meta, meta);
    lv_label_set_text(objects.saved_file, m->saved_filename);
    lv_bar_set_value(objects.saved_prog, (int32_t)m->saved_frac_pct, LV_ANIM_OFF);
}

static void bind_files(const ui_model_t *m)
{
    char sub[16];
    lv_snprintf(sub, sizeof(sub), "%d / %d", m->file_sel + 1, m->file_count);
    bind_status_bar(objects.files_bar, objects.files_bar_ic, objects.files_bar_l, objects.files_bar_sub,
                    objects.files_bar_r, objects.files_bar_hp, objects.files_batt,
                    objects.files_batt_fill, objects.files_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, "Files", sub, NULL, 0);

    lv_obj_t *rows[3]   = { objects.files_row_0, objects.files_row_1, objects.files_row_2 };
    lv_obj_t *dots[3]   = { objects.files_dot_0, objects.files_dot_1, objects.files_dot_2 };
    lv_obj_t *names[3]  = { objects.files_name_0, objects.files_name_1, objects.files_name_2 };
    lv_obj_t *badges[3] = { objects.files_badge_0, objects.files_badge_1, objects.files_badge_2 };
    lv_obj_t *durs[3]   = { objects.files_dur_0, objects.files_dur_1, objects.files_dur_2 };

    for (int r = 0; r < 3; r++) {
        int idx = m->file_scroll + r;
        if (idx >= m->file_count) {
            lv_obj_add_flag(rows[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(rows[r], LV_OBJ_FLAG_HIDDEN);
        const ui_file_entry_t *f = &m->files[idx];
        int sel = (idx == m->file_sel);
        lv_obj_set_style_bg_opa(rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

        if (f->unplayed) {
            lv_obj_set_style_bg_opa(dots[r], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dots[r], lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_CYAN), 0);
        } else {
            lv_obj_set_style_bg_opa(dots[r], LV_OPA_TRANSP, 0);
        }

        char dt_lbl[28], dur[14];
        ui_format_date_label(dt_lbl, (int)sizeof(dt_lbl), &m->now, &f->dt);
        ui_format_duration(dur, (int)sizeof(dur), f->duration_sec);
        lv_label_set_text(names[r], dt_lbl);
        lv_label_set_text(durs[r], dur);
        lv_obj_set_style_text_color(durs[r], lv_color_hex(sel ? UI_HEX_CYAN : UI_HEX_DIM), 0);

        if (f->has_transcript) {
            lv_obj_remove_flag(badges[r], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(badges[r], lv_color_hex(sel ? UI_HEX_TEXT : UI_HEX_FAINT), 0);
            lv_obj_set_style_text_color(badges[r], lv_color_hex(sel ? UI_HEX_PILL : UI_HEX_TEXT), 0);
        } else {
            lv_obj_add_flag(badges[r], LV_OBJ_FLAG_HIDDEN);
        }
    }
    bind_scrollbar(objects.files_scroll_thumb, m->file_count, m->file_scroll, 3, 50);
}

static void bind_player(const ui_model_t *m)
{
    uint32_t dur = m->play_file.duration_sec ? m->play_file.duration_sec : 1u;
    uint32_t pos = (m->play_pos_sec <= dur) ? m->play_pos_sec : dur;
    int32_t  pct = (int32_t)((pos * 100u) / dur);

    if (m->play_has_transcript) {
        char pos_s[12], tmp[12], dur_s[16];
        ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
        ui_format_duration(tmp, (int)sizeof(tmp), dur);
        lv_snprintf(dur_s, sizeof(dur_s), "/ %s", tmp);
        bind_status_bar(objects.ptr_bar, objects.ptr_bar_ic, objects.ptr_bar_l, objects.ptr_bar_sub,
                        objects.ptr_bar_r, objects.ptr_bar_hp, objects.ptr_batt,
                        objects.ptr_batt_fill, objects.ptr_batt_nub,
                        m, UI_COL_BAR, UI_COL_TEXT,
                        (m->play_state == 0u) ? UI_ICON_PLAY : UI_ICON_PAUSE,
                        (m->play_state == 0u) ? UI_COL_CYAN : UI_COL_AMBER,
                        pos_s, dur_s, NULL, 0);
        lv_bar_set_value(objects.ptr_prog, pct, LV_ANIM_OFF);
        static const uint32_t spk_cols[4] = { UI_HEX_CYAN, UI_HEX_AMBER, UI_HEX_GREEN, UI_HEX_PINK };
        lv_obj_set_style_bg_color(objects.ptr_bar_curr, lv_color_hex(spk_cols[m->tr_speaker & 3u]), 0);
        lv_label_set_text(objects.ptr_prev, m->tr_prev);
        char curr_line[96];
        lv_snprintf(curr_line, sizeof(curr_line), "%s%s", m->tr_curr_spoken, m->tr_curr_upcoming);
        lv_label_set_text(objects.ptr_curr_spoken, curr_line);
        lv_label_set_text(objects.ptr_next, m->tr_next);
        lv_label_set_text(objects.ptr_footer,
                          (m->play_state == 0u)
                              ? "\xE2\x97\x80\xE2\x96\xB6 Line \xC2\xB7 OK Pause \xC2\xB7 \xC2\xB1 Vol"
                              : "\xE2\x97\x80\xE2\x96\xB6 Line \xC2\xB7 OK Play \xC2\xB7 \xC2\xB1 Vol");
        return;
    }

    ui_icon_t st_ic = (m->play_state == 0u) ? UI_ICON_PLAY : (m->play_state == 1u ? UI_ICON_PAUSE : UI_ICON_STOP);
    uint16_t  st_c  = (m->play_state == 0u) ? UI_COL_CYAN : (m->play_state == 1u ? UI_COL_AMBER : UI_COL_DIM);
    const char *st_t = (m->play_state == 0u) ? "Playing" : (m->play_state == 1u ? "Paused" : "End");
    bind_status_bar(objects.play_bar, objects.play_bar_ic, objects.play_bar_l, objects.play_bar_sub,
                    objects.play_bar_r, objects.play_bar_hp, objects.play_batt,
                    objects.play_batt_fill, objects.play_batt_nub,
                    m, UI_COL_BAR, st_c, st_ic, st_c, st_t, NULL, NULL, 0);

    char dt_s[28], vbuf[16], pos_s[14], tmp[12], tot_s[16], ftxt[48];
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &m->play_file.dt);
    lv_label_set_text(objects.player_date, dt_s);

    uint8_t  vol    = m->hp_plugged ? m->cfg.hp_vol : m->cfg.spk_vol;
    uint32_t tenths = (vol * 100u + 15u) / 31u;
    if (tenths % 10u != 0u) {
        lv_snprintf(vbuf, sizeof(vbuf), "%s %u.%u", m->hp_plugged ? "HP" : "SPK",
                    (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    } else {
        lv_snprintf(vbuf, sizeof(vbuf), "%s %u", m->hp_plugged ? "HP" : "SPK", (unsigned)(tenths / 10u));
    }
    lv_label_set_text(objects.player_vol_lbl, vbuf);
    lv_obj_set_style_bg_color(objects.player_vol_pill, lv_color_hex(m->play_vol_hi ? UI_HEX_CYAN : UI_HEX_TILE), 0);
    lv_obj_set_style_text_color(objects.player_vol_lbl, lv_color_hex(m->play_vol_hi ? UI_HEX_BLACK : UI_HEX_DIM), 0);

    ui_format_duration(pos_s, (int)sizeof(pos_s), pos);
    ui_format_duration(tmp, (int)sizeof(tmp), dur);
    lv_snprintf(tot_s, sizeof(tot_s), "/ %s", tmp);
    lv_label_set_text(objects.player_pos, pos_s);
    lv_obj_set_style_text_color(objects.player_pos, lv_color_hex((m->play_state == 2u) ? UI_HEX_MUTE : UI_HEX_TEXT), 0);
    lv_label_set_text(objects.player_tot, tot_s);
    lv_bar_set_value(objects.player_vu_l, (int32_t)m->play_vu_l, LV_ANIM_OFF);
    lv_bar_set_value(objects.player_vu_r, (int32_t)m->play_vu_r, LV_ANIM_OFF);
    lv_bar_set_value(objects.player_prog, pct, LV_ANIM_OFF);

    if (m->play_state == 2u) {
        lv_label_set_text(objects.player_footer, m->play_has_next ? "OK Replay \xC2\xB7 \xE2\x96\xB6 Next file" : "OK Replay");
    } else {
        const char *la = (m->cfg.skip_mode == UI_SKIP_5_10) ? "5s" : (m->cfg.skip_mode == UI_SKIP_10_30 ? "10s" : "Prev");
        const char *ra = (m->cfg.skip_mode == UI_SKIP_5_10) ? "10s" : (m->cfg.skip_mode == UI_SKIP_10_30 ? "30s" : "Next");
        lv_snprintf(ftxt, sizeof(ftxt), "\xE2\x97\x80 %s \xC2\xB7 OK %s \xC2\xB7 \xE2\x96\xB6 %s",
                    la, (m->play_state == 0u) ? "Pause" : "Play", ra);
        lv_label_set_text(objects.player_footer, ftxt);
    }
}

static void bind_delete(const ui_model_t *m)
{
    bind_status_bar(objects.del_bar, objects.del_bar_ic, objects.del_bar_l, objects.del_bar_sub,
                    objects.del_bar_r, objects.del_bar_hp, objects.del_batt,
                    objects.del_batt_fill, objects.del_batt_nub,
                    m, UI_COL_RED, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, "Delete?", NULL, NULL, 1);
    const ui_file_entry_t *f = &m->play_file;
    if (m->files && m->file_sel >= 0 && m->file_sel < m->file_count) f = &m->files[m->file_sel];
    char dt_s[28], dur_s[12], sub[52];
    ui_format_date_label(dt_s, (int)sizeof(dt_s), &m->now, &f->dt);
    ui_format_duration(dur_s, (int)sizeof(dur_s), f->duration_sec);
    lv_snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", dur_s, f->filename);
    lv_label_set_text(objects.del_date, dt_s);
    lv_label_set_text(objects.del_sub, sub);
    lv_bar_set_value(objects.del_hold_bar, (int32_t)m->del_hold_pct, LV_ANIM_OFF);
    lv_label_set_text(objects.del_footer, m->del_holding ? "Keep holding OK\xE2\x80\xA6" : "Hold OK Delete \xC2\xB7 BACK Keep");
    lv_obj_set_style_text_color(objects.del_footer, lv_color_hex(m->del_holding ? UI_HEX_TEXT : UI_HEX_DIM), 0);
}

static void format_setting_val(const ui_model_t *m, int idx, char *val, int vcap, int *adj)
{
    static const uint8_t brt_pct[5] = { 20u, 40u, 60u, 80u, 100u };
    *adj = 1;
    switch (idx) {
    case 0: lv_snprintf(val, (size_t)vcap, "%u", (unsigned)m->cfg.mic_gain); break;
    case 1: lv_snprintf(val, (size_t)vcap, "%s", sound_mode_name(m->cfg.sound_mode)); break;
    case 2: lv_snprintf(val, (size_t)vcap, "%s", (m->cfg.channels == 1u) ? "1ch" : "2ch"); break;
    case 3: lv_snprintf(val, (size_t)vcap, "%s", m->cfg.vor_enabled ? "On" : "Off"); break;
    case 4: lv_snprintf(val, (size_t)vcap, "%u%%", (unsigned)brt_pct[m->cfg.brightness_idx < 5u ? m->cfg.brightness_idx : 3u]); break;
    case 5: lv_snprintf(val, (size_t)vcap, "%s", screen_off_name(m->cfg.screen_off_idx)); break;
    case 6: ui_format_clock(val, vcap, m->now.hour, m->now.minute); *adj = 0; break;
    case 7: lv_snprintf(val, (size_t)vcap, "%s", skip_mode_name(m->cfg.skip_mode)); break;
    case 8: lv_snprintf(val, (size_t)vcap, "%s", m->vbus_present ? "Connect" : "Unplugged"); *adj = 0; break;
    default: lv_snprintf(val, (size_t)vcap, "%s", BOARD_FW_VERSION_STR); *adj = 0; break;
    }
}

static void bind_settings(const ui_model_t *m)
{
    static const char *const labels[10] = {
        "Mic gain", "Sound", "Mics", "Voice trigger",
        "Brightness", "Screen off", "Clock", "Skip", "USB disk", "About"
    };
    char sub[12];
    lv_snprintf(sub, sizeof(sub), "%d / 10", m->set_sel + 1);
    bind_status_bar(objects.set_bar, objects.set_bar_ic, objects.set_bar_l, objects.set_bar_sub,
                    objects.set_bar_r, objects.set_bar_hp, objects.set_batt,
                    objects.set_batt_fill, objects.set_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, "Settings", sub, NULL, 0);

    lv_obj_t *rows[3] = { objects.set_row_0, objects.set_row_1, objects.set_row_2 };
    lv_obj_t *lbls[3] = { objects.set_lbl_0, objects.set_lbl_1, objects.set_lbl_2 };
    lv_obj_t *vals[3] = { objects.set_val_0, objects.set_val_1, objects.set_val_2 };

    for (int r = 0; r < 3; r++) {
        int idx = m->set_scroll + r;
        if (idx >= 10) { lv_obj_add_flag(rows[r], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(rows[r], LV_OBJ_FLAG_HIDDEN);
        int sel = (idx == m->set_sel);
        lv_obj_set_style_bg_opa(rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_label_set_text(lbls[r], labels[idx]);

        if (idx == 6 && sel && m->set_edit_clock != 0u) {
            char cbuf[16];
            lv_snprintf(cbuf, sizeof(cbuf), (m->set_edit_clock == 1u) ? "[%02u]:%02u" : "%02u:[%02u]",
                        (unsigned)m->set_edit_h, (unsigned)m->set_edit_m);
            lv_label_set_text(vals[r], cbuf);
            lv_obj_set_style_text_color(vals[r], lv_color_hex(UI_HEX_CYAN), 0);
        } else {
            char val[16], disp[24];
            int adj = 0;
            format_setting_val(m, idx, val, (int)sizeof(val), &adj);
            if (sel && adj) {
                lv_snprintf(disp, sizeof(disp), "- %s +", val);
                lv_label_set_text(vals[r], disp);
                lv_obj_set_style_text_color(vals[r], lv_color_hex(UI_HEX_CYAN), 0);
            } else {
                lv_label_set_text(vals[r], val);
                lv_obj_set_style_text_color(vals[r], lv_color_hex((idx == 8 && sel && m->vbus_present) ? UI_HEX_CYAN : UI_HEX_DIM), 0);
            }
        }
    }
    bind_scrollbar(objects.set_scroll_thumb, 10, m->set_scroll, 3, 50);

    if (m->set_edit_clock != 0u) {
        lv_obj_add_flag(objects.set_foot_vu_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.set_foot_desc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(objects.set_foot_desc, "\xC2\xB1 Set \xC2\xB7 \xE2\x97\x80\xE2\x96\xB6 Field \xC2\xB7 OK Save");
    } else if (m->set_sel == 0) {
        lv_obj_add_flag(objects.set_foot_desc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.set_foot_vu_box, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(objects.set_foot_vu_bar, (int32_t)m->set_input_vu, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(objects.set_foot_vu_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.set_foot_desc, LV_OBJ_FLAG_HIDDEN);
        const char *desc = "Version and update mode";
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
        default: break;
        }
        lv_label_set_text(objects.set_foot_desc, desc);
    }
}

static void bind_about(const ui_model_t *m)
{
    bind_status_bar(objects.about_bar, objects.about_bar_ic, objects.about_bar_l, objects.about_bar_sub,
                    objects.about_bar_r, objects.about_bar_hp, objects.about_batt,
                    objects.about_batt_fill, objects.about_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, "About", NULL, NULL, 0);
    uint32_t gb10 = (m->sd_total_mb * 10u) / 1024u;
    char st[44], bt[44];
    lv_snprintf(st, sizeof(st), "Storage %u.%u GB \xC2\xB7 %uh left",
                (unsigned)(gb10 / 10u), (unsigned)(gb10 % 10u), (unsigned)(m->sd_free_sec / 3600u));
    lv_snprintf(bt, sizeof(bt), "Battery %u.%02u V \xC2\xB7 %u%%",
                (unsigned)(m->batt_mv / 1000u), (unsigned)((m->batt_mv % 1000u) / 10u), (unsigned)m->batt_pct);
    lv_label_set_text(objects.about_storage, st);
    lv_label_set_text(objects.about_batt_lbl, bt);
}

static void bind_usb(const ui_model_t *m)
{
    bind_status_bar(objects.usb_bar, objects.usb_bar_ic, objects.usb_bar_l, objects.usb_bar_sub,
                    objects.usb_bar_r, objects.usb_bar_hp, objects.usb_batt,
                    objects.usb_batt_fill, objects.usb_batt_nub,
                    m, UI_COL_BAR, UI_COL_CYAN, UI_ICON_NONE, UI_COL_TEXT, "USB", NULL, NULL, 1);
    char tx_s[16], rx_s[16], tx_f[24], rx_f[24], bp[12];
    ui_format_bytes_short(tx_s, (int)sizeof(tx_s), m->usb_tx_bytes);
    ui_format_bytes_short(rx_s, (int)sizeof(rx_s), m->usb_rx_bytes);
    lv_snprintf(tx_f, sizeof(tx_f), "\xE2\x96\xB2 %s", tx_s);
    lv_snprintf(rx_f, sizeof(rx_f), "\xE2\x96\xBC %s", rx_s);
    lv_snprintf(bp, sizeof(bp), "+ %u%%", (unsigned)m->batt_pct);
    lv_label_set_text(objects.usb_tx_lbl, tx_f);
    lv_obj_set_style_text_color(objects.usb_tx_lbl, lv_color_hex(m->usb_read_act ? UI_HEX_CYAN : UI_HEX_DIM), 0);
    lv_label_set_text(objects.usb_rx_lbl, rx_f);
    lv_obj_set_style_text_color(objects.usb_rx_lbl, lv_color_hex(m->usb_write_act ? UI_HEX_AMBER : UI_HEX_DIM), 0);
    lv_label_set_text(objects.usb_batt_lbl, bp);
}

static void bind_bt(const ui_model_t *m)
{
    bind_status_bar(objects.bt_bar, objects.bt_bar_ic, objects.bt_bar_l, objects.bt_bar_sub,
                    objects.bt_bar_r, objects.bt_bar_hp, objects.bt_batt,
                    objects.bt_batt_fill, objects.bt_batt_nub,
                    m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, "Bluetooth sync", NULL, NULL, 0);
    lv_label_set_text(objects.bt_name_lbl, m->bt_name[0] ? m->bt_name : "HW_76");
    if (m->bt_state == 0u) {
        lv_label_set_text(objects.bt_l1_lbl, "Waiting for phone\xE2\x80\xA6");
        lv_obj_set_style_text_color(objects.bt_l1_lbl, lv_color_hex(UI_HEX_DIM), 0);
        lv_obj_remove_flag(objects.bt_l2_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.bt_prog, LV_OBJ_FLAG_HIDDEN);
    } else if (m->bt_state == 1u) {
        char cs[44];
        lv_snprintf(cs, sizeof(cs), "\xE2\x9C\x93 Connected \xC2\xB7 %s", m->bt_peer[0] ? m->bt_peer : "Phone");
        lv_label_set_text(objects.bt_l1_lbl, cs);
        lv_obj_set_style_text_color(objects.bt_l1_lbl, lv_color_hex(UI_HEX_GREEN), 0);
        lv_obj_add_flag(objects.bt_l2_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.bt_prog, LV_OBJ_FLAG_HIDDEN);
    } else {
        char ss[32];
        lv_snprintf(ss, sizeof(ss), "Sending %u of %u", (unsigned)m->bt_sent, (unsigned)m->bt_total);
        lv_label_set_text(objects.bt_l1_lbl, ss);
        lv_obj_set_style_text_color(objects.bt_l1_lbl, lv_color_hex(UI_HEX_TEXT), 0);
        lv_obj_add_flag(objects.bt_l2_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.bt_prog, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(objects.bt_prog, (int32_t)m->bt_pct, LV_ANIM_OFF);
    }
}

static void bind_charge(const ui_model_t *m)
{
    bind_status_bar(objects.chg_bar, objects.chg_bar_ic, objects.chg_bar_l, objects.chg_bar_sub,
                    objects.chg_bar_r, objects.chg_bar_hp, objects.chg_batt,
                    objects.chg_batt_fill, objects.chg_batt_nub,
                    m, UI_COL_BAR, UI_COL_GREEN, UI_ICON_NONE, UI_COL_TEXT, "Charging", NULL, NULL, 0);
    char bp[16];
    lv_snprintf(bp, sizeof(bp), "+ %u%%", (unsigned)m->batt_pct);
    lv_label_set_text(objects.chg_pct_lbl, bp);
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
    case UI_SCR_OFF:
    default:              return SCREEN_ID_OFF;
    }
}

void ui_render_screen(ui_screen_id_t scr, const ui_model_t *m)
{
    if (!m) return;
    ui_port_init();

    enum ScreensEnum target = map_eez_screen(scr, m);
    loadScreen(target);
    s_active_eez_scr = target;

    switch (target) {
    case SCREEN_ID_HOME:           bind_home(m); break;
    case SCREEN_ID_REC:            bind_rec(m); break;
    case SCREEN_ID_REC_ERR:        bind_rec_err(m); break;
    case SCREEN_ID_SAVED:          bind_saved(m); break;
    case SCREEN_ID_FILES:          bind_files(m); break;
    case SCREEN_ID_FILES_EMPTY:
        bind_status_bar(objects.files_e_bar, objects.files_e_bar_ic, objects.files_e_bar_l, objects.files_e_bar_sub,
                        objects.files_e_bar_r, objects.files_e_bar_hp, objects.files_e_batt,
                        objects.files_e_batt_fill, objects.files_e_batt_nub,
                        m, UI_COL_BAR, UI_COL_TEXT, UI_ICON_NONE, UI_COL_TEXT, "Files", NULL, NULL, 0);
        break;
    case SCREEN_ID_PLAYER:
    case SCREEN_ID_PLAYER_TR:      bind_player(m); break;
    case SCREEN_ID_DELETE_CONFIRM: bind_delete(m); break;
    case SCREEN_ID_SETTINGS:       bind_settings(m); break;
    case SCREEN_ID_ABOUT:          bind_about(m); break;
    case SCREEN_ID_USB:            bind_usb(m); break;
    case SCREEN_ID_BT:             bind_bt(m); break;
    case SCREEN_ID_CHARGE:         bind_charge(m); break;
    case SCREEN_ID_POWER:
        lv_bar_set_value(objects.pwr_hold_bar, (int32_t)m->power_hold_pct, LV_ANIM_OFF);
        break;
    case SCREEN_ID_OFF:
    default:
        ui_toast_update(NULL, 0u);
        ui_port_render_now();
        return;
    }

    ui_toast_update(m->toast_msg, m->toast_col);
    ui_port_render_now();
    if (ui_port_is_animating()) ui_port_task_handler();
}
