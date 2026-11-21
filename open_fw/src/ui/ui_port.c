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

/* Partial RGB565 render buffer (BOARD_DISP_HOR_RES * BOARD_DISP_BUF_LINES * 2 B) */
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

static uint32_t ui_tick_cb(void)
{
#ifdef OPENFW_HOST_MMIO
    static uint32_t s_host_tick = 0u;
    s_host_tick += 20u;
    return s_host_tick;
#else
    return millis();
#endif
}

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t x = (uint16_t)area->x1;
    uint16_t y = (uint16_t)area->y1;
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    lcd_blit_rgb565(x, y, w, h, (const uint16_t *)(const void *)px_map);
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
    lv_tick_set_cb(ui_tick_cb);

    s_disp = lv_display_create(BOARD_DISP_HOR_RES, BOARD_DISP_VER_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
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

void ui_port_render_now(void)
{
    if (!s_port_inited || !s_disp) {
        return;
    }
    lv_refr_now(s_disp);
}
