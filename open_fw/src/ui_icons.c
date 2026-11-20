/*
 * Cassini OpenFW — Drawn AA Vector Icons, Shared Chrome & Freestanding Formatters
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_ui.h"

/* =========================================================================
 * Freestanding Buffer Writer (zero libc dependencies)
 * ========================================================================= */
typedef struct {
    char *dst;
    int   len;
    int   pos;
} fmt_buf_t;

static void fmt_init(fmt_buf_t *fb, char *dst, int dst_len)
{
    fb->dst = dst;
    fb->len = dst_len;
    fb->pos = 0;
    if (dst && dst_len > 0) {
        dst[0] = '\0';
    }
}

static void fmt_putc(fmt_buf_t *fb, char c)
{
    if (!fb->dst || fb->pos + 1 >= fb->len) {
        return;
    }
    fb->dst[fb->pos++] = c;
    fb->dst[fb->pos]   = '\0';
}

static void fmt_puts(fmt_buf_t *fb, const char *s)
{
    if (!s) {
        return;
    }
    while (*s != '\0') {
        fmt_putc(fb, *s++);
    }
}

static void fmt_put_u32(fmt_buf_t *fb, uint32_t v)
{
    char tmp[12];
    int  n = 0;
    if (v == 0u) {
        fmt_putc(fb, '0');
        return;
    }
    while (v > 0u && n < 11) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        fmt_putc(fb, tmp[--n]);
    }
}

static void fmt_put_2d(fmt_buf_t *fb, uint32_t v)
{
    fmt_putc(fb, (char)('0' + ((v / 10u) % 10u)));
    fmt_putc(fb, (char)('0' + (v % 10u)));
}

/* =========================================================================
 * 4x4 MSAA Mini-Canvas for Multi-Stroke / Compound Vector Icons
 *
 * Avoids bounding-box background clobbering when AA strokes intersect
 * (e.g. CHECK, BT, USB, MIC, HP, SPK, BOLT, SETUP, REW, FFW).
 * ========================================================================= */
#define ICON_CANVAS_MAX 24

typedef struct {
    int16_t  size;
    uint16_t mask[ICON_CANVAS_MAX * ICON_CANVAS_MAX]; /* 4x4 subpixel bitmask */
} icon_canvas_t;

static void ic_clear(icon_canvas_t *c, int16_t size)
{
    if (size > ICON_CANVAS_MAX) {
        size = ICON_CANVAS_MAX;
    }
    c->size = size;
    int total = (int)size * (int)size;
    for (int i = 0; i < total; i++) {
        c->mask[i] = 0u;
    }
}

/* Convert normalized [0..256] coordinate to 1/8-pixel units */
static inline int32_t ic_n(int16_t size, int32_t norm256)
{
    return (norm256 * (int32_t)size * 8 + 128) / 256;
}

static void ic_add_line(icon_canvas_t *c, int32_t ax, int32_t ay,
                        int32_t bx, int32_t by, int32_t half_w8)
{
    int32_t vx    = bx - ax;
    int32_t vy    = by - ay;
    int32_t vlen2 = vx * vx + vy * vy;
    int32_t rad2  = half_w8 * half_w8;

    for (int16_t py = 0; py < c->size; py++) {
        for (int16_t px = 0; px < c->size; px++) {
            uint16_t m = 0u;
            for (int32_t sy = 0; sy < 4; sy++) {
                int32_t qy = (int32_t)py * 8 + sy * 2 + 1;
                for (int32_t sx = 0; sx < 4; sx++) {
                    int32_t qx = (int32_t)px * 8 + sx * 2 + 1;
                    int32_t wx = qx - ax;
                    int32_t wy = qy - ay;
                    int32_t dx, dy;
                    if (vlen2 == 0) {
                        dx = wx;
                        dy = wy;
                    } else {
                        int32_t dot = wx * vx + wy * vy;
                        if (dot <= 0) {
                            dx = wx;
                            dy = wy;
                        } else if (dot >= vlen2) {
                            dx = qx - bx;
                            dy = qy - by;
                        } else {
                            int64_t cross = (int64_t)wx * (int64_t)vy
                                          - (int64_t)wy * (int64_t)vx;
                            if (cross * cross <= (int64_t)rad2 * (int64_t)vlen2) {
                                m |= (uint16_t)(1u << (sy * 4 + sx));
                            }
                            continue;
                        }
                    }
                    if (dx * dx + dy * dy <= rad2) {
                        m |= (uint16_t)(1u << (sy * 4 + sx));
                    }
                }
            }
            c->mask[py * c->size + px] |= m;
        }
    }
}

static void ic_add_circle(icon_canvas_t *c, int32_t cx, int32_t cy, int32_t r8)
{
    int32_t r2 = r8 * r8;
    for (int16_t py = 0; py < c->size; py++) {
        for (int16_t px = 0; px < c->size; px++) {
            uint16_t m = 0u;
            for (int32_t sy = 0; sy < 4; sy++) {
                int32_t dy = (int32_t)py * 8 + sy * 2 + 1 - cy;
                for (int32_t sx = 0; sx < 4; sx++) {
                    int32_t dx = (int32_t)px * 8 + sx * 2 + 1 - cx;
                    if (dx * dx + dy * dy <= r2) {
                        m |= (uint16_t)(1u << (sy * 4 + sx));
                    }
                }
            }
            c->mask[py * c->size + px] |= m;
        }
    }
}

static void ic_add_rect(icon_canvas_t *c, int32_t x0, int32_t y0,
                        int32_t x1, int32_t y1)
{
    for (int16_t py = 0; py < c->size; py++) {
        for (int16_t px = 0; px < c->size; px++) {
            uint16_t m = 0u;
            for (int32_t sy = 0; sy < 4; sy++) {
                int32_t qy = (int32_t)py * 8 + sy * 2 + 1;
                if (qy < y0 || qy > y1) {
                    continue;
                }
                for (int32_t sx = 0; sx < 4; sx++) {
                    int32_t qx = (int32_t)px * 8 + sx * 2 + 1;
                    if (qx >= x0 && qx <= x1) {
                        m |= (uint16_t)(1u << (sy * 4 + sx));
                    }
                }
            }
            c->mask[py * c->size + px] |= m;
        }
    }
}

static void ic_add_rrect(icon_canvas_t *c, int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1, int32_t r8)
{
    int32_t w = x1 - x0;
    int32_t h = y1 - y0;
    if (w <= 0 || h <= 0) {
        return;
    }
    if (r8 * 2 > w) r8 = w / 2;
    if (r8 * 2 > h) r8 = h / 2;
    int32_t lx = x0 + r8;
    int32_t rx = x1 - r8;
    int32_t ty = y0 + r8;
    int32_t by = y1 - r8;
    int32_t r2 = r8 * r8;

    for (int16_t py = 0; py < c->size; py++) {
        for (int16_t px = 0; px < c->size; px++) {
            uint16_t m = 0u;
            for (int32_t sy = 0; sy < 4; sy++) {
                int32_t qy = (int32_t)py * 8 + sy * 2 + 1;
                if (qy < y0 || qy > y1) {
                    continue;
                }
                int32_t cy = (qy < ty) ? ty : ((qy > by) ? by : qy);
                for (int32_t sx = 0; sx < 4; sx++) {
                    int32_t qx = (int32_t)px * 8 + sx * 2 + 1;
                    if (qx < x0 || qx > x1) {
                        continue;
                    }
                    if ((qx >= lx && qx <= rx) || (qy >= ty && qy <= by)) {
                        m |= (uint16_t)(1u << (sy * 4 + sx));
                    } else {
                        int32_t cx = (qx < lx) ? lx : rx;
                        int32_t dx = qx - cx;
                        int32_t dy = qy - cy;
                        if (dx * dx + dy * dy <= r2) {
                            m |= (uint16_t)(1u << (sy * 4 + sx));
                        }
                    }
                }
            }
            c->mask[py * c->size + px] |= m;
        }
    }
}

static inline int32_t ic_edge(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                              int32_t px, int32_t py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void ic_add_tri(icon_canvas_t *c, int32_t ax, int32_t ay,
                       int32_t bx, int32_t by, int32_t cx, int32_t cy)
{
    int32_t area = ic_edge(ax, ay, bx, by, cx, cy);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        int32_t tx = bx; bx = cx; cx = tx;
        int32_t ty = by; by = cy; cy = ty;
    }
    for (int16_t py = 0; py < c->size; py++) {
        for (int16_t px = 0; px < c->size; px++) {
            uint16_t m = 0u;
            for (int32_t sy = 0; sy < 4; sy++) {
                int32_t qy = (int32_t)py * 8 + sy * 2 + 1;
                for (int32_t sx = 0; sx < 4; sx++) {
                    int32_t qx = (int32_t)px * 8 + sx * 2 + 1;
                    if (ic_edge(ax, ay, bx, by, qx, qy) >= 0 &&
                        ic_edge(bx, by, cx, cy, qx, qy) >= 0 &&
                        ic_edge(cx, cy, ax, ay, qx, qy) >= 0) {
                        m |= (uint16_t)(1u << (sy * 4 + sx));
                    }
                }
            }
            c->mask[py * c->size + px] |= m;
        }
    }
}

/* Ring arc clipped by half-plane filter:
 * mode 0: upper half (qy <= cy)
 * mode 1: lower half (qy >= cy)
 * mode 2: right wedge (dx > 0 && |dy| <= dx) */
static void ic_add_arc(icon_canvas_t *c, int32_t cx, int32_t cy,
                       int32_t r_in8, int32_t r_out8, int mode)
{
    int32_t r_in2  = r_in8 * r_in8;
    int32_t r_out2 = r_out8 * r_out8;
    for (int16_t py = 0; py < c->size; py++) {
        for (int16_t px = 0; px < c->size; px++) {
            uint16_t m = 0u;
            for (int32_t sy = 0; sy < 4; sy++) {
                int32_t qy = (int32_t)py * 8 + sy * 2 + 1;
                int32_t dy = qy - cy;
                if (mode == 0 && dy > 0) continue;
                if (mode == 1 && dy < 0) continue;
                for (int32_t sx = 0; sx < 4; sx++) {
                    int32_t qx = (int32_t)px * 8 + sx * 2 + 1;
                    int32_t dx = qx - cx;
                    if (mode == 2) {
                        int32_t ady = (dy < 0) ? -dy : dy;
                        if (dx <= 0 || ady > dx) continue;
                    }
                    int32_t d2 = dx * dx + dy * dy;
                    if (d2 >= r_in2 && d2 <= r_out2) {
                        m |= (uint16_t)(1u << (sy * 4 + sx));
                    }
                }
            }
            c->mask[py * c->size + px] |= m;
        }
    }
}

static uint32_t popcount16(uint16_t v)
{
    uint32_t n = 0u;
    while (v != 0u) {
        v &= (uint16_t)(v - 1u);
        n++;
    }
    return n;
}

static void ic_blit(const icon_canvas_t *c, int16_t x, int16_t y,
                    uint16_t fg, uint16_t bg)
{
    int16_t lcd_w = (int16_t)lcd_get_width();
    int16_t lcd_h = (int16_t)lcd_get_height();
    int16_t x0 = (x < 0) ? (int16_t)(-x) : 0;
    int16_t y0 = (y < 0) ? (int16_t)(-y) : 0;
    int16_t x1 = c->size;
    int16_t y1 = c->size;
    if (x + x1 > lcd_w) x1 = (int16_t)(lcd_w - x);
    if (y + y1 > lcd_h) y1 = (int16_t)(lcd_h - y);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    uint16_t row_buf[ICON_CANVAS_MAX];
    int16_t  w = (int16_t)(x1 - x0);
    for (int16_t py = y0; py < y1; py++) {
        for (int16_t px = x0; px < x1; px++) {
            uint32_t hits = popcount16(c->mask[py * c->size + px]);
            uint8_t  a4   = (uint8_t)((hits * 15u + 8u) / 16u);
            row_buf[px - x0] = lcd_blend_rgb565(fg, bg, a4);
        }
        lcd_blit_rgb565((uint16_t)(x + x0), (uint16_t)(y + py),
                        (uint16_t)w, 1u, row_buf);
    }
}

/* =========================================================================
 * Drawn AA Vector Icons
 * ========================================================================= */
void ui_draw_icon(int16_t x, int16_t y, int16_t size, ui_icon_t icon,
                  uint16_t fg, uint16_t bg)
{
    if (size <= 0 || icon == UI_ICON_NONE) {
        return;
    }

    /* Simple single/non-overlapping primitives drawn directly via lcd_*_aa */
    switch (icon) {
    case UI_ICON_PLAY:
    case UI_ICON_RIGHT: {
        int16_t pad = (size >= 9) ? 1 : 0;
        lcd_fill_tri_aa((int16_t)(x + pad), (int16_t)(y + pad),
                        (int16_t)(x + pad), (int16_t)(y + size - pad),
                        (int16_t)(x + size - pad), (int16_t)(y + size / 2),
                        fg, bg);
        return;
    }
    case UI_ICON_LEFT: {
        int16_t pad = (size >= 9) ? 1 : 0;
        lcd_fill_tri_aa((int16_t)(x + size - pad), (int16_t)(y + pad),
                        (int16_t)(x + size - pad), (int16_t)(y + size - pad),
                        (int16_t)(x + pad), (int16_t)(y + size / 2),
                        fg, bg);
        return;
    }
    case UI_ICON_UP: {
        int16_t pad = (size >= 9) ? 1 : 0;
        lcd_fill_tri_aa((int16_t)(x + size / 2), (int16_t)(y + pad),
                        (int16_t)(x + pad), (int16_t)(y + size - pad),
                        (int16_t)(x + size - pad), (int16_t)(y + size - pad),
                        fg, bg);
        return;
    }
    case UI_ICON_DOWN: {
        int16_t pad = (size >= 9) ? 1 : 0;
        lcd_fill_tri_aa((int16_t)(x + pad), (int16_t)(y + pad),
                        (int16_t)(x + size - pad), (int16_t)(y + pad),
                        (int16_t)(x + size / 2), (int16_t)(y + size - pad),
                        fg, bg);
        return;
    }
    case UI_ICON_PAUSE: {
        int16_t bw    = (size >= 10) ? (int16_t)((size * 3) / 10) : ((size >= 6) ? 2 : 1);
        int16_t gap   = (size >= 10) ? (int16_t)(size / 5) : 2;
        int16_t total = (int16_t)(2 * bw + gap);
        int16_t x0    = (int16_t)(x + (size - total) / 2);
        int16_t pad   = (size >= 8) ? 1 : 0;
        int16_t r     = (bw >= 3) ? 1 : 0;
        lcd_fill_rrect_aa(x0, (int16_t)(y + pad), bw, (int16_t)(size - 2 * pad), r, fg, bg);
        lcd_fill_rrect_aa((int16_t)(x0 + bw + gap), (int16_t)(y + pad), bw,
                          (int16_t)(size - 2 * pad), r, fg, bg);
        return;
    }
    case UI_ICON_STOP: {
        int16_t pad = (size >= 8) ? 1 : 0;
        int16_t r   = (size >= 12) ? 2 : 1;
        lcd_fill_rrect_aa((int16_t)(x + pad), (int16_t)(y + pad),
                          (int16_t)(size - 2 * pad), (int16_t)(size - 2 * pad),
                          r, fg, bg);
        return;
    }
    case UI_ICON_DOT: {
        int16_t r = (int16_t)((size - 2) / 2);
        if (r < 1) r = 1;
        lcd_fill_circle_aa((int16_t)(x + size / 2), (int16_t)(y + size / 2), r, fg, bg);
        return;
    }
    case UI_ICON_FILES: {
        int16_t bar_h = (size >= 12) ? 2 : 1;
        int16_t dot_w = bar_h;
        int16_t gap   = (size >= 10) ? 2 : 1;
        int16_t pad_x = (size >= 10) ? 1 : 0;
        int16_t pad_y = (size >= 10) ? 2 : 1;
        int16_t span  = (int16_t)(size - 2 * pad_y - bar_h);
        int16_t bar_x = (int16_t)(x + pad_x + dot_w + gap);
        int16_t bar_w = (int16_t)(size - 2 * pad_x - dot_w - gap);
        if (bar_w < 2) bar_w = 2;
        for (int i = 0; i < 3; i++) {
            int16_t ry = (int16_t)(y + pad_y + (i * span) / 2);
            if (x + pad_x >= 0 && ry >= 0) {
                lcd_fill_rect((uint16_t)(x + pad_x), (uint16_t)ry,
                              (uint16_t)dot_w, (uint16_t)bar_h, fg);
                lcd_fill_rect((uint16_t)bar_x, (uint16_t)ry,
                              (uint16_t)bar_w, (uint16_t)bar_h, fg);
            }
        }
        return;
    }
    default:
        break;
    }

    /* Compound / intersecting vector icons rendered via 4x4 MSAA canvas */
    icon_canvas_t cv;
    int16_t s = (size > ICON_CANVAS_MAX) ? ICON_CANVAS_MAX : size;
    ic_clear(&cv, s);

    /* Default stroke half-width in 1/8-px units: ~0.65px at size 7, scaling smoothly */
    int32_t hw = ((int32_t)s * 5 + 4) / 8;
    if (hw < 4) hw = 4;

    switch (icon) {
    case UI_ICON_REW: {
        int32_t mid = ic_n(s, 134);
        ic_add_tri(&cv, mid, ic_n(s, 32), mid, ic_n(s, 224), ic_n(s, 16), ic_n(s, 128));
        ic_add_tri(&cv, ic_n(s, 240), ic_n(s, 32), ic_n(s, 240), ic_n(s, 224), mid - 4, ic_n(s, 128));
        break;
    }
    case UI_ICON_FFW: {
        int32_t mid = ic_n(s, 122);
        ic_add_tri(&cv, ic_n(s, 16), ic_n(s, 32), ic_n(s, 16), ic_n(s, 224), mid + 4, ic_n(s, 128));
        ic_add_tri(&cv, mid, ic_n(s, 32), mid, ic_n(s, 224), ic_n(s, 240), ic_n(s, 128));
        break;
    }
    case UI_ICON_CHECK: {
        int32_t cw = (hw < 5) ? 5 : hw;
        ic_add_line(&cv, ic_n(s, 36), ic_n(s, 138), ic_n(s, 102), ic_n(s, 202), cw);
        ic_add_line(&cv, ic_n(s, 102), ic_n(s, 202), ic_n(s, 220), ic_n(s, 62), cw);
        break;
    }
    case UI_ICON_SPK: {
        /* Speaker cabinet + flared cone */
        ic_add_rect(&cv, ic_n(s, 20), ic_n(s, 88), ic_n(s, 78), ic_n(s, 168));
        ic_add_tri(&cv, ic_n(s, 70), ic_n(s, 88), ic_n(s, 146), ic_n(s, 36), ic_n(s, 146), ic_n(s, 220));
        ic_add_tri(&cv, ic_n(s, 70), ic_n(s, 88), ic_n(s, 146), ic_n(s, 220), ic_n(s, 70), ic_n(s, 168));
        /* Sound wave arc(s) */
        int32_t cx = ic_n(s, 146);
        int32_t cy = ic_n(s, 128);
        int32_t t  = (s >= 12) ? 9 : 7;
        ic_add_arc(&cv, cx, cy, ic_n(s, 42) - t / 2, ic_n(s, 42) + t / 2, 2);
        if (s >= 11) {
            ic_add_arc(&cv, cx, cy, ic_n(s, 84) - t / 2, ic_n(s, 84) + t / 2, 2);
        }
        break;
    }
    case UI_ICON_HP: {
        /* Arched headband */
        int32_t cx = ic_n(s, 128);
        int32_t cy = ic_n(s, 134);
        int32_t r  = ic_n(s, 92);
        int32_t t  = (hw < 5) ? 5 : hw;
        ic_add_arc(&cv, cx, cy, r - t, r + t, 0);
        /* Left & right earcups */
        ic_add_rrect(&cv, ic_n(s, 22), ic_n(s, 126), ic_n(s, 78), ic_n(s, 224), ic_n(s, 24));
        ic_add_rrect(&cv, ic_n(s, 178), ic_n(s, 126), ic_n(s, 234), ic_n(s, 224), ic_n(s, 24));
        break;
    }
    case UI_ICON_FLAG: {
        /* Pole + triangular pennant */
        ic_add_line(&cv, ic_n(s, 62), ic_n(s, 28), ic_n(s, 62), ic_n(s, 228), hw);
        ic_add_tri(&cv, ic_n(s, 64), ic_n(s, 26), ic_n(s, 218), ic_n(s, 84), ic_n(s, 64), ic_n(s, 148));
        break;
    }
    case UI_ICON_BOLT: {
        /* Classic two-triangle lightning bolt */
        ic_add_tri(&cv, ic_n(s, 152), ic_n(s, 14), ic_n(s, 46), ic_n(s, 142), ic_n(s, 136), ic_n(s, 142));
        ic_add_tri(&cv, ic_n(s, 120), ic_n(s, 114), ic_n(s, 210), ic_n(s, 114), ic_n(s, 104), ic_n(s, 242));
        break;
    }
    case UI_ICON_SETUP: {
        /* 3 horizontal slider tracks with knobs at varied positions */
        static const int32_t ys[3] = { 54, 128, 202 };
        static const int32_t kx[3] = { 84, 176, 114 };
        int32_t thw = (s >= 12) ? 5 : 4;
        int32_t kr  = ic_n(s, 26);
        if (kr < 7) kr = 7;
        for (int i = 0; i < 3; i++) {
            int32_t yy = ic_n(s, ys[i]);
            ic_add_line(&cv, ic_n(s, 24), yy, ic_n(s, 232), yy, thw);
            ic_add_circle(&cv, ic_n(s, kx[i]), yy, kr);
        }
        break;
    }
    case UI_ICON_BT: {
        /* Classic Bluetooth rune */
        int32_t bw = (s >= 14) ? 6 : 4;
        int32_t mx = ic_n(s, 122);
        int32_t ty = ic_n(s, 24);
        int32_t by = ic_n(s, 232);
        int32_t rx = ic_n(s, 202);
        int32_t lx = ic_n(s, 54);
        int32_t uy = ic_n(s, 76);
        int32_t ly = ic_n(s, 180);
        ic_add_line(&cv, mx, ty, mx, by, bw);
        ic_add_line(&cv, mx, ty, rx, uy, bw);
        ic_add_line(&cv, rx, uy, lx, ly, bw);
        ic_add_line(&cv, lx, uy, rx, ly, bw);
        ic_add_line(&cv, rx, ly, mx, by, bw);
        break;
    }
    case UI_ICON_USB: {
        /* USB trident */
        int32_t uw = (s >= 14) ? 6 : 4;
        int32_t mx = ic_n(s, 128);
        /* Vertical shaft */
        ic_add_line(&cv, mx, ic_n(s, 52), mx, ic_n(s, 210), uw);
        /* Top arrowhead */
        ic_add_tri(&cv, mx, ic_n(s, 16), ic_n(s, 92), ic_n(s, 64), ic_n(s, 164), ic_n(s, 64));
        /* Bottom circle */
        ic_add_circle(&cv, mx, ic_n(s, 214), ic_n(s, 28));
        /* Left branch -> circle */
        ic_add_line(&cv, mx, ic_n(s, 168), ic_n(s, 62), ic_n(s, 126), uw);
        ic_add_line(&cv, ic_n(s, 62), ic_n(s, 126), ic_n(s, 62), ic_n(s, 96), uw);
        ic_add_circle(&cv, ic_n(s, 62), ic_n(s, 88), ic_n(s, 22));
        /* Right branch -> square */
        ic_add_line(&cv, mx, ic_n(s, 148), ic_n(s, 194), ic_n(s, 110), uw);
        ic_add_line(&cv, ic_n(s, 194), ic_n(s, 110), ic_n(s, 194), ic_n(s, 82), uw);
        ic_add_rect(&cv, ic_n(s, 174), ic_n(s, 62), ic_n(s, 214), ic_n(s, 102));
        break;
    }
    case UI_ICON_MIC: {
        /* Capsule */
        int32_t mx = ic_n(s, 128);
        ic_add_rrect(&cv, ic_n(s, 86), ic_n(s, 22), ic_n(s, 170), ic_n(s, 146), ic_n(s, 42));
        /* U-stand */
        int32_t uw = (s >= 14) ? 6 : 4;
        int32_t ur = ic_n(s, 72);
        int32_t uy = ic_n(s, 116);
        ic_add_arc(&cv, mx, uy, ur - uw, ur + uw, 1);
        ic_add_line(&cv, mx - ur, ic_n(s, 96), mx - ur, uy, uw);
        ic_add_line(&cv, mx + ur, ic_n(s, 96), mx + ur, uy, uw);
        /* Stem & base */
        ic_add_line(&cv, mx, uy + ur, mx, ic_n(s, 224), uw);
        ic_add_line(&cv, ic_n(s, 82), ic_n(s, 224), ic_n(s, 174), ic_n(s, 224), uw);
        break;
    }
    default:
        return;
    }

    ic_blit(&cv, x, y, fg, bg);
}

/* =========================================================================
 * Battery Icon (14x8 rounded outline + 2x3 right nub + inner fill)
 * ========================================================================= */
void ui_draw_battery(int16_t x, int16_t y, uint8_t pct, int charging,
                     uint16_t outline_col, uint16_t bg)
{
    if (pct > 100u) {
        pct = 100u;
    }

    /* 14x8 rounded outline (r=2) */
    lcd_fill_rrect_aa(x, y, 14, 8, 2, outline_col, bg);
    lcd_fill_rrect_aa((int16_t)(x + 1), (int16_t)(y + 1), 12, 6, 1, bg, outline_col);

    /* 2x3 right nub at (x+14, y+2) */
    if ((x + 14) >= 0 && (y + 2) >= 0) {
        lcd_fill_rect((uint16_t)(x + 14), (uint16_t)(y + 2), 2u, 3u, outline_col);
    }

    /* Inner fill at (x+2, y+2, 1..10, 4) */
    uint16_t fill_w = (uint16_t)(((uint32_t)pct * 10u + 50u) / 100u);
    if (fill_w < 1u) {
        fill_w = 1u;
    }
    if (fill_w > 10u) {
        fill_w = 10u;
    }

    uint16_t fill_col = outline_col;
    if (charging) {
        fill_col = UI_COL_GREEN;
    } else if (pct <= 10u) {
        fill_col = UI_COL_RED;
    } else if (pct <= 25u) {
        fill_col = UI_COL_AMBER;
    }

    if ((x + 2) >= 0 && (y + 2) >= 0) {
        lcd_fill_rect((uint16_t)(x + 2), (uint16_t)(y + 2), fill_w, 4u, fill_col);
    }
}

/* =========================================================================
 * Shared Chrome Helpers
 * ========================================================================= */
void ui_draw_status_bar(const ui_status_bar_t *sb)
{
    if (!sb) {
        return;
    }

    /* Full bar 0..159, y=0..12 (h=13) */
    lcd_fill_rect(0u, 0u, UI_W, UI_BAR_H, sb->bg_col);

    /* Left: optional icon + left_text + optional left_sub */
    int16_t lx = 4;
    if (sb->left_icon != UI_ICON_NONE) {
        ui_draw_icon(lx, 2, 8, sb->left_icon, sb->left_icon_col, sb->bg_col);
        lx = (int16_t)(lx + 11);
    }
    if (sb->left_text && sb->left_text[0] != '\0') {
        lcd_draw_str_aa_bl(lx, 10, sb->left_text, &g_font_ui_sm, sb->fg_col, sb->bg_col);
        lx = (int16_t)(lx + (int16_t)lcd_measure_str_aa(sb->left_text, &g_font_ui_sm));
    }
    if (sb->left_sub && sb->left_sub[0] != '\0') {
        lx = (int16_t)(lx + 5);
        lcd_draw_str_aa_bl(lx, 10, sb->left_sub, &g_font_ui_sm, sb->left_sub_col, sb->bg_col);
    }

    /* Right: battery at (142, 2) */
    ui_draw_battery(142, 2, sb->batt_pct, sb->charging, sb->fg_col, sb->bg_col);

    if (sb->charging && !sb->no_clock) {
        ui_draw_icon(134, 3, 7, UI_ICON_BOLT, UI_COL_GREEN, sb->bg_col);
    }

    if (!sb->no_clock) {
        char clk[8];
        ui_format_clock(clk, (int)sizeof(clk), sb->hour, sb->minute);
        uint16_t cw         = lcd_measure_str_aa(clk, &g_font_ui_sm);
        int16_t  right_edge = sb->charging ? 131 : 139;
        int16_t  clock_left = (int16_t)(right_edge - (int16_t)cw);
        lcd_draw_str_aa_bl(clock_left, 10, clk, &g_font_ui_sm, UI_COL_DIM, sb->bg_col);
        if (sb->hp_plugged) {
            ui_draw_icon((int16_t)(clock_left - 11), 2, 8, UI_ICON_HP, UI_COL_DIM, sb->bg_col);
        }
    } else if (sb->right_text && sb->right_text[0] != '\0') {
        uint16_t rw = lcd_measure_str_aa(sb->right_text, &g_font_ui_sm);
        int16_t  rx = (int16_t)(138 - (int16_t)rw);
        lcd_draw_str_aa_bl(rx, 10, sb->right_text, &g_font_ui_sm, sb->right_text_col, sb->bg_col);
    }
}

void ui_draw_footer_hints(int16_t baseline_y, const ui_hint_t *hints, int count, uint16_t bg)
{
    if (!hints || count <= 0) {
        return;
    }

    int32_t total_w = 0;
    for (int i = 0; i < count; i++) {
        const ui_hint_t *h = &hints[i];
        int32_t key_w = 0;
        if (h->icon != UI_ICON_NONE) {
            key_w += 7;
        }
        if (h->icon2 != UI_ICON_NONE) {
            if (h->icon != UI_ICON_NONE) {
                key_w += 1;
            }
            key_w += 7;
        }
        if (h->key && h->key[0] != '\0') {
            if (key_w > 0) {
                key_w += 2;
            }
            key_w += (int32_t)lcd_measure_str_aa(h->key, &g_font_ui_sm);
        }

        int32_t act_w = 0;
        if (h->action && h->action[0] != '\0') {
            act_w = (int32_t)lcd_measure_str_aa(h->action, &g_font_ui_sm);
        }

        if (i > 0) {
            total_w += 10;
        }
        total_w += key_w;
        if (key_w > 0 && act_w > 0) {
            total_w += 3;
        }
        total_w += act_w;
    }

    int16_t x  = (int16_t)((UI_W - total_w) / 2);
    int16_t iy = (int16_t)(baseline_y - 7);

    for (int i = 0; i < count; i++) {
        const ui_hint_t *h = &hints[i];
        int had_key = 0;

        if (i > 0) {
            x = (int16_t)(x + 10);
        }

        if (h->icon != UI_ICON_NONE) {
            ui_draw_icon(x, iy, 7, h->icon, UI_COL_TEXT, bg);
            x = (int16_t)(x + 7);
            had_key = 1;
        }
        if (h->icon2 != UI_ICON_NONE) {
            if (h->icon != UI_ICON_NONE) {
                x = (int16_t)(x + 1);
            }
            ui_draw_icon(x, iy, 7, h->icon2, UI_COL_TEXT, bg);
            x = (int16_t)(x + 7);
            had_key = 1;
        }
        if (h->key && h->key[0] != '\0') {
            if (had_key) {
                x = (int16_t)(x + 2);
            }
            lcd_draw_str_aa_bl(x, baseline_y, h->key, &g_font_ui_sm, UI_COL_TEXT, bg);
            x = (int16_t)(x + (int16_t)lcd_measure_str_aa(h->key, &g_font_ui_sm));
            had_key = 1;
        }
        if (h->action && h->action[0] != '\0') {
            if (had_key) {
                x = (int16_t)(x + 3);
            }
            lcd_draw_str_aa_bl(x, baseline_y, h->action, &g_font_ui_sm, UI_COL_DIM, bg);
            x = (int16_t)(x + (int16_t)lcd_measure_str_aa(h->action, &g_font_ui_sm));
        }
    }
}

void ui_draw_toast(const char *msg, uint16_t pill_col, uint16_t text_col, uint16_t bg)
{
    if (!msg) {
        return;
    }
    lcd_fill_rrect_aa(4, 64, 152, 14, 7, pill_col, bg);
    uint16_t tw = lcd_measure_str_aa(msg, &g_font_ui_sm);
    if (tw <= 140u) {
        int16_t tx = (int16_t)((UI_W - (int16_t)tw) / 2);
        lcd_draw_str_aa_bl(tx, 75, msg, &g_font_ui_sm, text_col, pill_col);
    } else {
        lcd_draw_str_aa_trunc(10, (int16_t)(75 - (int16_t)g_font_ui_sm.ascent),
                              msg, &g_font_ui_sm, 140u, text_col, pill_col);
    }
}

void ui_draw_hbar(int16_t x, int16_t y, int16_t w, int16_t h,
                  uint8_t pct, uint8_t peak_pct, uint16_t bg)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (pct > 100u)      pct = 100u;
    if (peak_pct > 100u) peak_pct = 100u;

    int16_t r = (int16_t)(h / 2);
    lcd_fill_rrect_aa(x, y, w, h, r, UI_COL_OFF, bg);

    uint16_t fill_col = UI_COL_GREEN;
    if (pct > 85u) {
        fill_col = UI_COL_RED;
    } else if (pct >= 60u) {
        fill_col = UI_COL_AMBER;
    }

    int16_t fw = (int16_t)(((uint32_t)w * (uint32_t)pct + 50u) / 100u);
    if (pct > 0u && fw < 1) {
        fw = 1;
    }
    if (fw > w) {
        fw = w;
    }

    if (fw > 0) {
        uint16_t fill_bg = (fw >= w) ? bg : UI_COL_OFF;
        lcd_fill_rrect_aa(x, y, fw, h, r, fill_col, fill_bg);
    }

    if (peak_pct > 0u && w >= 2) {
        int16_t px = (int16_t)(x + (int16_t)(((uint32_t)(w - 2) * (uint32_t)peak_pct + 50u) / 100u));
        if (px < x)          px = x;
        if (px > x + w - 2)  px = (int16_t)(x + w - 2);
        if (px >= 0 && y >= 0) {
            lcd_fill_rect((uint16_t)px, (uint16_t)y, 2u, (uint16_t)h, UI_COL_TEXT);
        }
    }
}

void ui_draw_vmeter(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint8_t pct, uint8_t peak_pct, int clip_latch, uint16_t bg)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (pct > 100u)      pct = 100u;
    if (peak_pct > 100u) peak_pct = 100u;

    int lit = (int)(((uint32_t)pct * 8u + 50u) / 100u);
    if (pct > 0u && lit == 0) {
        lit = 1;
    }
    if (lit > 8) {
        lit = 8;
    }

    int peak_idx = -1;
    if (peak_pct > 0u) {
        peak_idx = (int)(((uint32_t)peak_pct * 8u + 50u) / 100u) - 1;
        if (peak_idx < 0) peak_idx = 0;
        if (peak_idx > 7) peak_idx = 7;
    }

    int16_t gap = (h >= 16) ? 1 : 0;

    for (int i = 0; i < 8; i++) {
        /* i=0 is bottom segment, i=7 is top segment */
        int16_t sy0 = (int16_t)(y + ((7 - i) * (h + gap)) / 8);
        int16_t sy1 = (int16_t)(y + ((8 - i) * (h + gap)) / 8 - gap);
        int16_t sh  = (int16_t)(sy1 - sy0);
        if (sh < 1) {
            sh = 1;
        }

        uint16_t col;
        if (i == 7 && clip_latch != 0) {
            col = UI_COL_RED;
        } else if (i == peak_idx) {
            col = UI_COL_TEXT;
        } else if (i < lit) {
            if (i == 7) {
                col = UI_COL_RED;
            } else if (i >= 5) {
                col = UI_COL_AMBER;
            } else {
                col = UI_COL_GREEN;
            }
        } else {
            col = UI_COL_OFF;
        }

        int16_t r = (sh >= 3 && w >= 3) ? 1 : 0;
        lcd_fill_rrect_aa(x, sy0, w, sh, r, col, bg);
    }
}

/* =========================================================================
 * Freestanding Date / Time / Duration Formatters
 * ========================================================================= */
void ui_format_duration(char *dst, int dst_len, uint32_t secs)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);

    if (secs >= 3600u) {
        uint32_t hrs = secs / 3600u;
        uint32_t rem = secs % 3600u;
        fmt_put_u32(&fb, hrs);
        fmt_putc(&fb, ':');
        fmt_put_2d(&fb, rem / 60u);
        fmt_putc(&fb, ':');
        fmt_put_2d(&fb, rem % 60u);
    } else {
        fmt_put_2d(&fb, secs / 60u);
        fmt_putc(&fb, ':');
        fmt_put_2d(&fb, secs % 60u);
    }
}

void ui_format_clock(char *dst, int dst_len, uint8_t hour, uint8_t minute)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);

    if (hour > 23u)   hour   = (uint8_t)(hour % 24u);
    if (minute > 59u) minute = (uint8_t)(minute % 60u);
    fmt_put_2d(&fb, hour);
    fmt_putc(&fb, ':');
    fmt_put_2d(&fb, minute);
}

void ui_format_free_time(char *dst, int dst_len, uint32_t free_secs)
{
    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);

    uint32_t total_min = free_secs / 60u;
    uint32_t hrs       = total_min / 60u;
    uint32_t mins      = total_min % 60u;

    if (hrs > 0u) {
        fmt_put_u32(&fb, hrs);
        fmt_puts(&fb, "h ");
        fmt_put_u32(&fb, mins);
        fmt_putc(&fb, 'm');
    } else {
        fmt_put_u32(&fb, mins);
        fmt_putc(&fb, 'm');
    }
}

/* Howard Hinnant civil-to-days algorithm (days since 1970-01-01, exact for leap years) */
static int32_t civil_to_days(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t  y   = (int32_t)year - ((month <= 2u) ? 1 : 0);
    int32_t  era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t mp  = (uint32_t)month + ((month > 2u) ? (uint32_t)(-3) : 9u);
    uint32_t doy = (153u * mp + 2u) / 5u + (uint32_t)day - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void ui_format_date_label(char *dst, int dst_len,
                          const rtc_datetime_t *now, const rtc_datetime_t *rec)
{
    static const char *const WDAYS[7] = {
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
    };
    static const char *const MONTHS[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    fmt_buf_t fb;
    fmt_init(&fb, dst, dst_len);

    if (!rec) {
        return;
    }

    uint8_t m = (rec->month >= 1u && rec->month <= 12u) ? rec->month : 1u;
    uint8_t d = (rec->day >= 1u && rec->day <= 31u) ? rec->day : 1u;
    uint8_t hh = (rec->hour <= 23u) ? rec->hour : (uint8_t)(rec->hour % 24u);
    uint8_t mm = (rec->minute <= 59u) ? rec->minute : (uint8_t)(rec->minute % 60u);

    if (now) {
        uint8_t nm = (now->month >= 1u && now->month <= 12u) ? now->month : 1u;
        uint8_t nd = (now->day >= 1u && now->day <= 31u) ? now->day : 1u;
        int32_t now_days = civil_to_days(now->year, nm, nd);
        int32_t rec_days = civil_to_days(rec->year, m, d);
        int32_t diff     = now_days - rec_days;

        if (diff == 0) {
            fmt_puts(&fb, "Today ");
            fmt_put_2d(&fb, hh);
            fmt_putc(&fb, ':');
            fmt_put_2d(&fb, mm);
            return;
        }
        if (diff == 1) {
            fmt_puts(&fb, "Yest. ");
            fmt_put_2d(&fb, hh);
            fmt_putc(&fb, ':');
            fmt_put_2d(&fb, mm);
            return;
        }
        if (diff >= 2 && diff <= 6) {
            /* 1970-01-01 was Thursday (index 3 in 0=Mon..6=Sun) */
            int32_t wd = (rec_days + 3) % 7;
            if (wd < 0) wd += 7;
            fmt_puts(&fb, WDAYS[wd]);
            fmt_putc(&fb, ' ');
            fmt_put_2d(&fb, hh);
            fmt_putc(&fb, ':');
            fmt_put_2d(&fb, mm);
            return;
        }
        if (rec->year == now->year) {
            fmt_put_u32(&fb, d);
            fmt_putc(&fb, ' ');
            fmt_puts(&fb, MONTHS[m - 1u]);
            fmt_putc(&fb, ' ');
            fmt_put_2d(&fb, hh);
            fmt_putc(&fb, ':');
            fmt_put_2d(&fb, mm);
            return;
        }
    }

    /* Older year (or now == NULL) */
    fmt_put_u32(&fb, d);
    fmt_putc(&fb, ' ');
    fmt_puts(&fb, MONTHS[m - 1u]);
    fmt_putc(&fb, ' ');
    fmt_put_u32(&fb, rec->year);
}
