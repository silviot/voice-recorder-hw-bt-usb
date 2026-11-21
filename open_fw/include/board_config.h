/*
 * Board & Device Hardware Profile Configuration
 *
 * Centralizes display geometry, color depth, partial-render buffer sizing,
 * and logical UI parameters so adapting open_fw to similar voice-recorder
 * hardware requires only small, localized edits here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_BOARD_CONFIG_H
#define OPENFW_BOARD_CONFIG_H

/* =========================================================================
 * Target Device Identity
 * ========================================================================= */
#ifndef BOARD_NAME
#define BOARD_NAME              "SL6801 HW_76"
#endif

#ifndef BOARD_FW_VERSION_STR
#define BOARD_FW_VERSION_STR    "v0.7"
#endif

#ifndef BOARD_FW_TITLE_STR
#define BOARD_FW_TITLE_STR      "Cassini OpenFW " BOARD_FW_VERSION_STR
#endif

/* =========================================================================
 * Display Panel Geometry (landscape logical coordinates)
 * ========================================================================= */
#ifndef BOARD_DISP_HOR_RES
#define BOARD_DISP_HOR_RES      160
#endif

#ifndef BOARD_DISP_VER_RES
#define BOARD_DISP_VER_RES      80
#endif

#ifndef BOARD_DISP_COLOR_DEPTH
#define BOARD_DISP_COLOR_DEPTH  16
#endif

/* Partial draw buffer height in scanlines (160 * 20 * 2 B = 6,400 B SRAM) */
#ifndef BOARD_DISP_BUF_LINES
#define BOARD_DISP_BUF_LINES    20
#endif

/* Compact UI mode for small strip displays (e.g., 160x80 / 128x64) */
#define BOARD_UI_COMPACT        ((BOARD_DISP_VER_RES) <= 100)

/* Shared UI Chrome Dimensions (adapt cleanly to larger displays) */
#if BOARD_UI_COMPACT
#define BOARD_UI_BAR_H          13
#define BOARD_UI_FOOTER_H       13
#define BOARD_UI_LIST_ROWS      3
#define BOARD_UI_ROW_H          16
#else
#define BOARD_UI_BAR_H          20
#define BOARD_UI_FOOTER_H       18
#define BOARD_UI_LIST_ROWS      5
#define BOARD_UI_ROW_H          24
#endif

/* =========================================================================
 * LVGL Memory Pool Budget (bytes, static SRAM via LV_STDLIB_BUILTIN)
 * ========================================================================= */
#ifndef BOARD_LVGL_MEM_SIZE
#define BOARD_LVGL_MEM_SIZE     (36u * 1024u)
#endif

#endif /* OPENFW_BOARD_CONFIG_H */
