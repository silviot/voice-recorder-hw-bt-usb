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

static void blit_frame_area(int32_t left, int32_t top, int32_t width, int32_t height)
{
    const uint16_t *pixels = &s_draw_buf[top * BOARD_DISP_HOR_RES + left];
#ifndef OPENFW_HOST_MMIO
    uint64_t t0 = us_timer_now();
#endif
    lcd_blit_rgb565_stride((uint16_t)left, (uint16_t)top, (uint16_t)width, (uint16_t)height,
                           pixels, BOARD_DISP_HOR_RES);
#ifndef OPENFW_HOST_MMIO
    g_ui_last_blit_us += (uint32_t)(us_timer_now() - t0);
#endif
}

/*
 * Screen transition: horizontal "push" on the ST7735S hardware scroll.
 *
 * The outgoing screen stays in panel GRAM and is scrolled out by the panel
 * itself (VSCSAD) while the incoming frame, already rendered once into
 * s_draw_buf, is written two columns at a time into the GRAM lines that have
 * just scrolled off screen. Each column lands at its normal address, so the
 * ring wraps back to scroll offset 0 after 162 px (160 + the 2 hidden margin
 * lines, which show as a thin black seam between the two pages) and nothing
 * else in the LCD path needs to know about scrolling.
 *
 * Cost per transition: one full-frame worth of LCD writes (12,800 px, ~10 ms of
 * bus time spread over the animation) + 81 two-byte scroll commands, no extra
 * frame buffer and no second LVGL render. Steps run from ui_port_task_handler()
 * (1 ms cadence on CPU1 while animating); a late tick simply catches up with
 * more 2-column steps, each of which keeps the panel consistent.
 */
#define TR_TRAVEL   ((int32_t)LCD_HSCROLL_LINES)
#define TR_STEP     2
#define TR_MS       160u

#define TR_SUPPORTED (BOARD_DISP_BUF_LINES >= BOARD_DISP_VER_RES && \
                      BOARD_DISP_HOR_RES + 2 == LCD_HSCROLL_LINES)

static ui_transition_t s_tr_dir;
static bool     s_tr_pending;  /* armed: waiting for the incoming frame's last flush */
static bool     s_tr_active;   /* scrolling */
static int32_t  s_tr_offset;   /* current scroll offset (0..TR_TRAVEL, even) */
static int32_t  s_tr_cols = BOARD_DISP_HOR_RES; /* incoming columns already in GRAM */
static uint32_t s_tr_start_ms;

/* Blit incoming columns [from, to) counted in push order (from the entering edge). */
static void tr_write_cols(int32_t from, int32_t to)
{
    if (to <= from) return;
    int32_t left = (s_tr_dir == UI_TRANSITION_BACK) ? BOARD_DISP_HOR_RES - to : from;
    blit_frame_area(left, 0, to - from, BOARD_DISP_VER_RES);
}

/* One 2-px step. Columns k < offset-2 are on screen as the new page, k >= offset
 * still show the old page, and the two in between sit in the hidden lines, so
 * those two can be written before the scroll moves them into view. */
static void tr_step(void)
{
    int32_t upto = LV_MIN(s_tr_offset, BOARD_DISP_HOR_RES);
    if (upto > s_tr_cols) {
        tr_write_cols(s_tr_cols, upto);
        s_tr_cols = upto;
    }
    s_tr_offset += TR_STEP;
    uint32_t off = (uint32_t)s_tr_offset % (uint32_t)TR_TRAVEL;
    if (s_tr_dir == UI_TRANSITION_BACK && off != 0u) off = (uint32_t)TR_TRAVEL - off;
    lcd_hscroll_set((uint16_t)off);
    if (s_tr_offset >= TR_TRAVEL) {
        s_tr_active = false;
        s_tr_cols = BOARD_DISP_HOR_RES;
    }
}

/* Ease-out cubic: moves immediately, settles gently. */
static int32_t tr_target(uint32_t now_ms)
{
    uint32_t t = now_ms - s_tr_start_ms;
    if (t >= TR_MS) return TR_TRAVEL;
    uint32_t r = ((TR_MS - t) << 10) / TR_MS;          /* remaining, Q10 */
    uint32_t e = 1024u - ((r * r >> 10) * r >> 10);     /* 1 - r^3, Q10 */
    return (int32_t)((uint32_t)TR_TRAVEL * e >> 10) & ~(TR_STEP - 1);
}

static void tr_advance(int32_t target)
{
    while (s_tr_active && s_tr_offset < target) tr_step();
}

void ui_transition(ui_transition_t dir)
{
    /* Fast-forward an unfinished push (its frame is still in s_draw_buf). */
    tr_advance(TR_TRAVEL);
    s_tr_pending = TR_SUPPORTED && dir != UI_TRANSITION_NONE;
    s_tr_dir = dir;
    s_tr_cols = s_tr_pending ? 0 : BOARD_DISP_HOR_RES;
}

bool ui_transition_busy(void)
{
    return s_tr_pending || s_tr_active;
}

static void tr_start(void)
{
    s_tr_pending = false;
    s_tr_active = true;
    s_tr_offset = 0;
    s_tr_start_ms = lv_tick_get();
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
        /* During a push only columns already handed to the panel are live;
         * the rest is written from s_draw_buf when the scroll reaches it. */
        int32_t left = x;
        int32_t right = x + w;
        if (s_tr_dir == UI_TRANSITION_BACK) {
            left = LV_MAX(left, BOARD_DISP_HOR_RES - s_tr_cols);
        } else {
            right = LV_MIN(right, s_tr_cols);
        }
        if (right > left) blit_frame_area(left, y, right - left, h);
    }
    /* Start the clock only once the incoming frame is fully rendered. */
    if (s_tr_pending && lv_display_flush_is_last(disp)) tr_start();
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
    bool was_tr = s_tr_active;
    if (was_tr) {
        tr_advance(tr_target(lv_tick_get()));
    }
    if (!was_tr || lv_anim_count_running() > 0u) {
        lv_timer_handler();
    }
}

uint32_t ui_port_anim_period_ms(void)
{
    if (!s_port_inited) {
        return 0u;
    }
    if (s_tr_active) {
        return 4u; /* 250 Hz hardware-scroll push steps from zero-wait SRAM */
    }
    if (lv_anim_count_running() > 0u) {
        return 16u; /* 60 Hz panel refresh parity */
    }
    return 0u;
}

int ui_port_is_animating(void)
{
    if (!s_port_inited) {
        return 0;
    }
    return (s_tr_active || lv_anim_count_running() > 0u) ? 1 : 0;
}

void ui_port_render_now(void)
{
    if (!s_port_inited || !s_disp) {
        return;
    }
    /* Loops that only render (single-core fallback) still advance a push. */
    if (s_tr_active) tr_advance(tr_target(lv_tick_get()));
    lv_refr_now(s_disp);
}
