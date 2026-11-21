/*
 * Cassini OpenFW — LVGL Platform Port (Display + Tick + Keypad Indev)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "board_config.h"
#include "openfw.h"
#include "ui_port.h"
#include "ui_theme.h"

/* RGB565 render buffer (BOARD_DISP_HOR_RES * BOARD_DISP_BUF_LINES * 2 B) */
static uint16_t s_draw_buf[BOARD_DISP_HOR_RES * BOARD_DISP_BUF_LINES]
    __attribute__((aligned(4)));

static int           s_port_inited = 0;
static lv_display_t *s_disp        = NULL;
static lv_indev_t   *s_indev       = NULL;
static lv_group_t   *s_group       = NULL;

/* Lightweight SPSC queue for feeding LVGL keypad indev without dropping fast taps */
#define UI_KEY_Q_SIZE 8u
#define UI_KEY_Q_MASK (UI_KEY_Q_SIZE - 1u)

typedef struct {
    uint32_t key;
    uint8_t  pressed;
} ui_key_msg_t;

static ui_key_msg_t s_key_q[UI_KEY_Q_SIZE];
static uint32_t     s_key_head = 0u;
static uint32_t     s_key_tail = 0u;
static uint32_t     s_last_key = 0u;
static uint8_t      s_last_pr  = 0u;

/* Unrevealed columns stay in panel GRAM while LVGL draws the incoming screen. */
static int32_t s_visible_cols = BOARD_DISP_HOR_RES;
static bool s_from_right;
static bool s_reveal_pending;

static void blit_frame_area(int32_t left, int32_t top, int32_t width, int32_t height)
{
    const uint16_t *pixels = &s_draw_buf[top * BOARD_DISP_HOR_RES + left];
    lcd_blit_rgb565_stride(left, top, width, height, pixels, BOARD_DISP_HOR_RES);
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
    lv_anim_set_duration(&anim, 180u);
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

static void ui_keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    if (s_key_head != s_key_tail) {
        ui_key_msg_t msg = s_key_q[s_key_tail & UI_KEY_Q_MASK];
        s_key_tail++;
        s_last_key = msg.key;
        s_last_pr  = msg.pressed;
    }

    data->key           = s_last_key;
    data->state         = s_last_pr ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = (s_key_head != s_key_tail);
}

void ui_port_push_key(uint32_t lv_key, int pressed)
{
    if ((uint32_t)(s_key_head - s_key_tail) >= UI_KEY_Q_SIZE) {
        return;
    }
    s_key_q[s_key_head & UI_KEY_Q_MASK].key     = lv_key;
    s_key_q[s_key_head & UI_KEY_Q_MASK].pressed = pressed ? 1u : 0u;
    s_key_head++;
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

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, ui_keypad_read_cb);
    lv_indev_set_display(s_indev, s_disp);

    s_group = lv_group_create();
    lv_group_set_default(s_group);
    lv_indev_set_group(s_indev, s_group);

    ui_theme_init();
}

lv_display_t *ui_port_get_display(void) { return s_disp; }
lv_indev_t   *ui_port_get_indev(void)   { return s_indev; }
lv_group_t   *ui_port_get_group(void)   { return s_group; }

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
