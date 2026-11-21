/*
 * Cassini OpenFW — LVGL Platform Port (Display + Tick + Keypad Indev)
 *
 * Isolates LVGL initialization, partial draw buffer, display flush callback,
 * tick source, and keypad input bridge behind a small, board-portable API.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_UI_PORT_H
#define OPENFW_UI_PORT_H

#include <stdint.h>
#include "lvgl.h"

/* Initialize LVGL core, display driver, and theme (idempotent). */
void ui_port_init(void);

/* Accessors for the primary display, keypad input device, and default focus group. */
lv_display_t *ui_port_get_display(void);
lv_indev_t   *ui_port_get_indev(void);
lv_group_t   *ui_port_get_group(void);

/* Run LVGL timers/animations/refresh (call periodically from main loop). */
void ui_port_task_handler(void);

/* Returns non-zero while an LVGL animation is actively running. */
int ui_port_is_animating(void);

/* Force an immediate synchronous display refresh (used by screen switches & tests). */
void ui_port_render_now(void);

/* Feed an explicit key event into the LVGL keypad bridge (optional; keys.c feeds automatically on target). */
void ui_port_push_key(uint32_t lv_key, int pressed);

#endif /* OPENFW_UI_PORT_H */
