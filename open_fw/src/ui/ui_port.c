/*
 * Cassini OpenFW — LVGL Platform Port (Display + Tick)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "board_config.h"
#include "openfw.h"
#include "ui_port.h"

/* RGB565 render buffer (BOARD_DISP_HOR_RES * BOARD_DISP_BUF_LINES * 2 B) */
static uint16_t s_draw_buf[BOARD_DISP_HOR_RES * BOARD_DISP_BUF_LINES]
    __attribute__((aligned(4)));

static int           s_port_inited = 0;
static lv_display_t *s_disp        = NULL;

/* Unrevealed columns stay in panel GRAM while LVGL draws the incoming screen. */
static int32_t s_visible_cols = BOARD_DISP_HOR_RES;
static bool s_from_right;
static bool s_reveal_pending;

static void blit_frame_area(int32_t left, int32_t top, int32_t width, int32_t height)
{
    const uint16_t *pixels = &s_draw_buf[top * BOARD_DISP_HOR_RES + left];
    lcd_blit_rgb565_stride((uint16_t)left, (uint16_t)top, (uint16_t)width, (uint16_t)height,
                           pixels, BOARD_DISP_HOR_RES);
}

static void reveal_columns(void *screen, int32_t cols)
{
    (void)screen;
    int32_t prev = s_visible_cols;
    if (cols <= prev) return;

    int32_t left = s_from_right ? BOARD_DISP_HOR_RES - cols : prev;
    blit_frame_area(left, 0, cols - prev, BOARD_DISP_VER_RES);
    s_visible_cols = cols;
}

static void reveal_deleted(lv_anim_t *anim)
{
    (void)anim;
    s_visible_cols = BOARD_DISP_HOR_RES;
}

void ui_port_set_reveal(bool enabled, bool from_right)
{
    s_reveal_pending = enabled && BOARD_DISP_BUF_LINES >= BOARD_DISP_VER_RES;
    s_visible_cols = s_reveal_pending ? 0 : BOARD_DISP_HOR_RES;
    s_from_right = from_right;
}

static void start_reveal(void)
{
    s_reveal_pending = false;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, lv_screen_active());
    lv_anim_set_exec_cb(&anim, reveal_columns);
    lv_anim_set_deleted_cb(&anim, reveal_deleted);
    lv_anim_set_values(&anim, 0, BOARD_DISP_HOR_RES);
    lv_anim_set_duration(&anim, 85u);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    if (!lv_anim_start(&anim)) reveal_columns(NULL, BOARD_DISP_HOR_RES);
}

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t x = (uint16_t)area->x1;
    uint16_t y = (uint16_t)area->y1;
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    if (BOARD_DISP_BUF_LINES < BOARD_DISP_VER_RES) {
        lcd_blit_rgb565(x, y, w, h, (const uint16_t *)(const void *)px_map);
    } else {
        int32_t left = x;
        int32_t right = x + w;
        if (s_from_right) {
            left = LV_MAX(left, BOARD_DISP_HOR_RES - s_visible_cols);
        } else {
            right = LV_MIN(right, s_visible_cols);
        }
        if (right > left) blit_frame_area(left, y, right - left, h);
    }
    /* Start the clock only once the bound destination is fully rendered. */
    if (s_reveal_pending && lv_display_flush_is_last(disp)) start_reveal();
    lv_display_flush_ready(disp);
}

void ui_port_init(void)
{
    if (s_port_inited) {
        return;
    }
    s_port_inited = 1;

    lv_init();
    lv_tick_set_cb(millis);

    s_disp = lv_display_create(BOARD_DISP_HOR_RES, BOARD_DISP_VER_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           BOARD_DISP_BUF_LINES >= BOARD_DISP_VER_RES ?
                           LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, ui_flush_cb);
    lv_display_set_default(s_disp);
}

lv_display_t *ui_port_get_display(void) { return s_disp; }

void ui_port_task_handler(void)
{
    if (!s_port_inited) {
        return;
    }
    lv_timer_handler();
}

int ui_port_is_animating(void)
{
    if (!s_port_inited) {
        return 0;
    }
    return lv_anim_count_running() > 0u ? 1 : 0;
}

void ui_port_render_now(void)
{
    if (!s_port_inited || !s_disp) {
        return;
    }
    lv_refr_now(s_disp);
}
