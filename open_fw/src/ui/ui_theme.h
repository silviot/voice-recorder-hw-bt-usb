/*
 * Cassini OpenFW — LVGL Design Tokens, Reusable Styles & Chrome Components
 *
 * Centralizes the 14-color dark-mode palette, typography, reusable LVGL
 * styles, top status bar, footer hint bar, vertical VU meter, and toast
 * overlay so all screens share a coherent visual language.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_UI_THEME_H
#define OPENFW_UI_THEME_H

#include <stdint.h>
#include "lvgl.h"
#include "openfw_ui.h"

/* =========================================================================
 * 14-Color Dark-Mode Palette (24-bit RGB hex for LVGL)
 * ========================================================================= */
#define UI_HEX_BG       0x070B14u
#define UI_HEX_BAR      0x111827u
#define UI_HEX_TILE     0x131B2Bu
#define UI_HEX_PILL     0x1F4E8Cu
#define UI_HEX_PILL_HI  0x2A64B0u
#define UI_HEX_OFF      0x1A2232u
#define UI_HEX_FAINT    0x344055u
#define UI_HEX_MUTE     0x5D6B82u
#define UI_HEX_DIM      0x9CB0CEu
#define UI_HEX_TEXT     0xFFFFFFu
#define UI_HEX_BLACK    0x000000u
#define UI_HEX_CYAN     0x42C8FFu
#define UI_HEX_RED      0xFF3B3Bu
#define UI_HEX_DRED     0x5A1418u
#define UI_HEX_REC_DIM  0xA01818u
#define UI_HEX_AMBER    0xFFB020u
#define UI_HEX_GREEN    0x3DDC84u
#define UI_HEX_PINK     0xFF7AC8u

/* Convert our legacy RGB565 color token (UI_COL_*) to lv_color_t */
static inline lv_color_t ui_color_from_rgb565(uint16_t c)
{
    uint8_t r = (uint8_t)(((c >> 11) & 0x1Fu) * 255u / 31u);
    uint8_t g = (uint8_t)(((c >> 5)  & 0x3Fu) * 255u / 63u);
    uint8_t b = (uint8_t)((c         & 0x1Fu) * 255u / 31u);
    return lv_color_make(r, g, b);
}

/* =========================================================================
 * Shared LVGL Styles (initialized once by ui_theme_init())
 * ========================================================================= */
extern lv_style_t g_st_screen;
extern lv_style_t g_st_bar;
extern lv_style_t g_st_tile;
extern lv_style_t g_st_tile_focus;
extern lv_style_t g_st_row;
extern lv_style_t g_st_row_focus;
extern lv_style_t g_st_pill;
extern lv_style_t g_st_track;
extern lv_style_t g_st_ind_cyan;
extern lv_style_t g_st_ind_red;
extern lv_style_t g_st_ind_green;
extern lv_style_t g_st_ind_white;

void ui_theme_init(void);

/* Helper: reset an lv_obj_t to a clean zero-padding, zero-border container */
lv_obj_t *ui_box_create(lv_obj_t *parent);

/* =========================================================================
 * Reusable Status Bar Widget (13px height on 160x80)
 * ========================================================================= */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *left_icon;
    lv_obj_t *left_lbl;
    lv_obj_t *left_sub;
    lv_obj_t *right_lbl;
    lv_obj_t *hp_lbl;
    lv_obj_t *batt_body;
    lv_obj_t *batt_fill;
    lv_obj_t *batt_nub;
} ui_bar_widget_t;

void ui_bar_create(lv_obj_t *parent, ui_bar_widget_t *bar);
void ui_bar_update(ui_bar_widget_t *bar, const ui_status_bar_t *sb);

/* =========================================================================
 * Reusable Footer Hint Bar (centered "KEY action · KEY action")
 * ========================================================================= */
lv_obj_t *ui_footer_create(lv_obj_t *parent, lv_color_t bg);
void      ui_footer_set_hints(lv_obj_t *footer, const ui_hint_t *hints, int count);

/* =========================================================================
 * Reusable 8-Segment Vertical VU Meter (with peak hold & clip latch)
 * ========================================================================= */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *segs[8]; /* 0 = bottom, 7 = top */
} ui_vmeter_widget_t;

void ui_vmeter_create(lv_obj_t *parent, ui_vmeter_widget_t *vm, int32_t w, int32_t h);
void ui_vmeter_update(ui_vmeter_widget_t *vm, uint8_t pct, uint8_t peak_pct, int clip_latch);

/* =========================================================================
 * Reusable Toast Overlay (rendered on lv_layer_top())
 * ========================================================================= */
void ui_toast_update(const char *msg, uint16_t rgb565_col);

/* =========================================================================
 * Shared Formatting Helpers
 * ========================================================================= */
const char *ui_icon_glyph(ui_icon_t icon);
void        ui_format_bytes_short(char *dst, int cap, uint64_t bytes);

#endif /* OPENFW_UI_THEME_H */
