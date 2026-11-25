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

/* Screen-change transition (horizontal push on the LCD hardware scroll).
 * Call right before loading a different screen: FORWARD pushes the new screen
 * in from the right, BACK from the left, NONE shows it instantly. Same-screen
 * refreshes must not call it. Any push still running is completed first. */
typedef enum {
    UI_TRANSITION_NONE = 0,
    UI_TRANSITION_FORWARD,
    UI_TRANSITION_BACK,
} ui_transition_t;

void ui_transition(ui_transition_t dir);

/* True from ui_transition() until the push has finished. */
bool ui_transition_busy(void);

/* Returns non-zero while an LVGL animation is actively running. */
int ui_port_is_animating(void);

/* Force an immediate synchronous display refresh (used by screen switches & tests). */
void ui_port_render_now(void);

#endif /* OPENFW_UI_PORT_H */
