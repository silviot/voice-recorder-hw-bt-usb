/*
 * Experiment 3 (ST7735S Landscape 160x80 MADCTL=0xA8 Validation):
 * Switches to landscape mode, draws a 1-px perimeter box + corner ticks +
 * Cassini speaker/timed-word UI mockup for `arg` seconds (default 5 s), then
 * restores portrait mode.
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint32_t secs = (arg >= 1u && arg <= 30u) ? arg : 5u;

    api->lcd_set_rotation(1);
    uint16_t w = api->lcd_get_width();
    uint16_t h = api->lcd_get_height();

    uint16_t bg   = (uint16_t)0x0000u;
    uint16_t cyan = (uint16_t)0x07FFu;
    uint16_t yl   = (uint16_t)0xFFE0u;
    uint16_t wht  = (uint16_t)0xFFFFu;
    uint16_t blue = (uint16_t)0x0015u;
    uint16_t dim  = (uint16_t)0x8410u;

    api->lcd_fill_rect(0u, 0u, w, h, bg);
    /* 1-px outer border */
    api->lcd_fill_rect(0u, 0u, w, 1u, cyan);
    api->lcd_fill_rect(0u, (uint16_t)(h - 1u), w, 1u, cyan);
    api->lcd_fill_rect(0u, 0u, 1u, h, cyan);
    api->lcd_fill_rect((uint16_t)(w - 1u), 0u, 1u, h, cyan);
    /* 4x4 yellow corner markers */
    api->lcd_fill_rect(1u, 1u, 4u, 4u, yl);
    api->lcd_fill_rect((uint16_t)(w - 5u), 1u, 4u, 4u, yl);
    api->lcd_fill_rect(1u, (uint16_t)(h - 5u), 4u, 4u, yl);
    api->lcd_fill_rect((uint16_t)(w - 5u), (uint16_t)(h - 5u), 4u, 4u, yl);

    api->lcd_draw_str(8u, 6u,  "[01:12] Speaker 1", cyan, bg);
    api->lcd_draw_str(8u, 20u, "Hello world from", wht, bg);
    api->lcd_fill_rect(7u, 31u, 46u, 11u, blue);
    api->lcd_draw_str(9u, 33u, "Cassini", yl, blue);
    api->lcd_draw_str(56u, 33u, "timed words!", wht, bg);

    api->lcd_fill_rect(8u, 52u, (uint16_t)(w - 16u), 4u, dim);
    api->lcd_fill_rect(8u, 52u, 72u, 4u, cyan);
    api->lcd_draw_str(8u, 62u, "CASSINI 160x80 MADCTL=A8", yl, bg);

    api->log_puts("[exp_lcd_land] madctl=0xA8 w=");
    api->log_u32(w);
    api->log_puts(" h=");
    api->log_u32(h);
    api->log_putc('\n');

    uint32_t t0 = api->millis();
    while ((uint32_t)(api->millis() - t0) < (secs * 1000u)) {
        api->wdt_feed();
        api->mdelay(50u);
    }

    api->lcd_set_rotation(0);
    return ((uint32_t)w << 16) | (uint32_t)h;
}
