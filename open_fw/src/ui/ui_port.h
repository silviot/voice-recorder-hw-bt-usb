/*
 * Cassini OpenFW — LVGL Platform Port (Display + Tick)
 *
 * Isolates LVGL initialization, draw buffer, display flush callback, and
 * tick source behind a small, board-portable API.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_UI_PORT_H
#define OPENFW_UI_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* Initialize LVGL core and display driver (idempotent). */
void ui_port_init(void);

/* Accessor for the primary display. */
lv_display_t *ui_port_get_display(void);

/* Run LVGL timers/animations/refresh (call periodically from main loop). */
void ui_port_task_handler(void);

/* Reveal the newly loaded navigation screen from the specified edge. */
void ui_port_set_reveal(bool enabled, bool from_right);

/* Returns non-zero while an LVGL animation is actively running. */
int ui_port_is_animating(void);

/* Force an immediate synchronous display refresh (used by screen switches & tests). */
void ui_port_render_now(void);

#endif /* OPENFW_UI_PORT_H */
