/*
 * Cassini OpenFW — LVGL Design Tokens, Formatting Helpers & Toast Overlay
 *
 * Centralizes the 14-color dark-mode palette, date/time/byte formatters,
 * icon glyphs, and the shared toast overlay rendered on lv_layer_top().
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

/* Helper: reset an lv_obj_t to a clean zero-padding, zero-border container */
lv_obj_t *ui_box_create(lv_obj_t *parent);

/* =========================================================================
 * Reusable Toast Overlay (rendered on lv_layer_top())
 * ========================================================================= */
void ui_toast_update(const char *msg, uint16_t rgb565_col);

/* =========================================================================
 * Shared Formatting & Calendar Helpers
 * ========================================================================= */
const char *ui_icon_glyph(ui_icon_t icon);
const char *ui_weekday_name(uint16_t year, uint8_t month, uint8_t day);
const char *ui_month_name(uint8_t month);
void        ui_format_bytes_short(char *dst, int cap, uint64_t bytes);

#endif /* OPENFW_UI_THEME_H */
