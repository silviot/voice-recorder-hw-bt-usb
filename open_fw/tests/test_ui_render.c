/*
 * Host MMIO visual renderer & unit test for the Cassini OpenFW 160x80 UI.
 *
 * Emulates the SL6801 8080 parallel LCDC (0x40095000) into a 160x80 RGB565
 * framebuffer, renders all 14 UI screens/sub-states using the exact firmware
 * C code (lcd_st7735s.c, ui_spectrogram.c, ui_screens.c, ui_theme.c, ui_port.c),
 * and writes a combined 2x contact sheet PPM for visual verification.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "board_config.h"
#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_ui.h"
#include "ui_port.h"
#include "eez/screens.h"
#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID
#include "openfw_i18n_bt.h"
#endif

/* Minimal host MMIO mock for LCDC (0x40095000) + ROM/GPIO stubs.
 *
 * Models the ST7735S GRAM in landscape (MADCTL MV=1): CASET addresses the 162
 * gate lines (logical x = address - 1, addresses 0/161 hidden), RASET the
 * source lines (logical y = address - 26). VSCSAD rotates the gate lines; with
 * MY=1 gate order is reversed relative to CASET. g_fb is what the panel shows. */
#define GRAM_LINES 162
static uint16_t g_gram[80][GRAM_LINES];
static uint16_t g_fb[80][160];
static uint32_t g_written_pixels;
static uint8_t  g_cmd = 0u;
static uint8_t  g_params[8];
static int      g_nparam = 0;
static uint16_t g_x0 = 0, g_x1 = 159, g_y0 = 0, g_y1 = 79;
static uint16_t g_cx = 0, g_cy = 0;
static uint32_t g_tx_data = 0u;
static uint32_t g_data_len = 0u;
static int      g_ramwr = 0;
static uint8_t  g_pix_hi = 0u;
static int      g_have_hi = 0;
static uint8_t  g_madctl = 0u;
static uint16_t g_ssa = 0u;
static unsigned g_ssa_writes = 0u;
static void   (*g_ssa_hook)(int before);

/* CASET address shown at logical screen column x under the current SSA. */
static int shown_addr(int x)
{
    int my = (g_madctl & 0x80u) != 0u;
    int a = x + 1;
    int gate = my ? GRAM_LINES - 1 - a : a;
    int row = (gate + g_ssa) % GRAM_LINES;
    return my ? GRAM_LINES - 1 - row : row;
}

static void mock_refresh_fb(void)
{
    for (int x = 0; x < 160; x++) {
        int a = shown_addr(x);
        for (int y = 0; y < 80; y++) g_fb[y][x] = g_gram[y][a];
    }
}

static void mock_put_byte(uint8_t b)
{
    if (g_ramwr) {
        if (!g_have_hi) {
            g_pix_hi = b;
            g_have_hi = 1;
        } else {
            uint16_t rgb565_val = (uint16_t)(((uint16_t)g_pix_hi << 8) | b);
            g_have_hi = 0;
            int lx = (int)g_cx - 1;
            int ly = (int)g_cy - 26;
            if (g_cx < GRAM_LINES && ly >= 0 && ly < 80) {
                if (lx >= 0 && lx < 160) g_written_pixels++;
                g_gram[ly][g_cx] = rgb565_val;
                for (int x = 0; x < 160; x++) {
                    if (shown_addr(x) == g_cx) g_fb[ly][x] = rgb565_val;
                }
            }
            g_cx++;
            if (g_cx > g_x1) {
                g_cx = g_x0;
                g_cy++;
                if (g_cy > g_y1) {
                    g_cy = g_y0;
                }
            }
        }
        return;
    }
    if (g_nparam < 8) {
        g_params[g_nparam++] = b;
    }
    if (g_cmd == 0x2Au && g_nparam == 4) {
        g_x0 = (uint16_t)((g_params[0] << 8) | g_params[1]);
        g_x1 = (uint16_t)((g_params[2] << 8) | g_params[3]);
    } else if (g_cmd == 0x2Bu && g_nparam == 4) {
        g_y0 = (uint16_t)((g_params[0] << 8) | g_params[1]);
        g_y1 = (uint16_t)((g_params[2] << 8) | g_params[3]);
    } else if (g_cmd == 0x36u && g_nparam == 1) {
        g_madctl = g_params[0];
    } else if (g_cmd == 0x33u && g_nparam == 6) {
        assert(g_params[0] == 0 && g_params[1] == 0);       /* TFA = 0 */
        assert(g_params[2] == 0 && g_params[3] == 162);     /* VSA = 162 */
        assert(g_params[4] == 0 && g_params[5] == 0);       /* BFA = 0 */
    } else if (g_cmd == 0x37u && g_nparam == 2) {
        uint16_t ssa = (uint16_t)((g_params[0] << 8) | g_params[1]);
        assert(ssa < GRAM_LINES);
        if (g_ssa_hook) g_ssa_hook(1);
        g_ssa = ssa;
        g_ssa_writes++;
        mock_refresh_fb();
        if (g_ssa_hook) g_ssa_hook(0);
    }
}

uint32_t openfw_mmio_rd32(uint32_t addr)
{
    if (addr == LCDC_STATUS_ADDR) {
        return 0x80000000u; /* DONE=1, IDLE */
    }
    return 0u;
}

void openfw_mmio_wr32(uint32_t addr, uint32_t val)
{
    if (addr == LCDC_CMD_REG_ADDR) {
        g_cmd = (uint8_t)(val & 0xFFu);
    } else if (addr == LCDC_TX_DATA_ADDR) {
        g_tx_data = val;
    } else if (addr == LCDC_DATA_LEN_ADDR) {
        g_data_len = (val & 0xFFu) + 1u;
    } else if (addr == LCDC_XFER_CTRL_ADDR) {
        if ((val & 0x0Fu) == 0x07u) {
            g_nparam = 0;
            g_ramwr = (g_cmd == 0x2Cu);
            g_have_hi = 0;
            if (g_ramwr) {
                g_cx = g_x0;
                g_cy = g_y0;
            }
        } else if ((val & 0x0Fu) == 0x0Bu) {
            for (uint32_t i = 0u; i < g_data_len && i < 4u; i++) {
                mock_put_byte((uint8_t)((g_tx_data >> (i * 8u)) & 0xFFu));
            }
        }
    }
}

static uint32_t s_now = 1000u;
uint32_t millis(void) { return s_now; }

void mdelay(uint32_t ms) { (void)ms; }
void rom_clk_enable(uint32_t clk_id) { (void)clk_id; }
void rom_clk_div(uint32_t clk_id, uint32_t div) { (void)clk_id; (void)div; }
void rom_clk_set_src(uint32_t clk_id, uint32_t src) { (void)clk_id; (void)src; }
void rom_rst_release(uint32_t rst_id) { (void)rst_id; }
int  rom_rst_is_released(uint32_t rst_id) { (void)rst_id; return 1; }

static void rgb565_to_rgb888(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((c >> 11) & 0x1Fu) * 255u / 31u);
    *g = (uint8_t)(((c >> 5)  & 0x3Fu) * 255u / 63u);
    *b = (uint8_t)((c         & 0x1Fu) * 255u / 31u);
}

#define NUM_SCREENS 14
static uint16_t g_shots[NUM_SCREENS][80][160];
static size_t   g_peak_heap_used = 0u;

static void sample_heap(const char *tag)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    size_t used = mon.total_size - mon.free_size;
    if (mon.max_used > g_peak_heap_used) {
        g_peak_heap_used = mon.max_used;
    }
    if (used > g_peak_heap_used) {
        g_peak_heap_used = used;
    }
    (void)tag;
}

static const uint16_t (*g_tr_old)[160];
static const uint16_t (*g_tr_new)[160];
static int g_tr_back, g_tr_d;

/* Expected panel at push offset d: old page, 2 px seam (hidden GRAM lines,
 * black), new page; mirrored for BACK. */
static uint16_t tr_expected(int x, int y, int d)
{
    int xf = g_tr_back ? 159 - x : x;
    if (xf < 160 - d) {
        int src = xf + d;
        return g_tr_old[y][g_tr_back ? 159 - src : src];
    }
    if (xf < 162 - d) return 0u;
    int src = xf - 162 + d;
    return g_tr_new[y][g_tr_back ? 159 - src : src];
}

static void tr_check_hook(int before)
{
    if (!before) g_tr_d += 2;
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < 160; x++) {
            assert(g_fb[y][x] == tr_expected(x, y, g_tr_d));
        }
    }
}

/* Optional: TR_FRAMES_DIR=/path writes the panel every 16 ms (≈ one 60 Hz
 * panel refresh) during the push test as 4x PPMs. */
static void dump_tr_frame(int forward, int idx)
{
    const char *dir = getenv("TR_FRAMES_DIR");
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%02d.ppm", dir, forward ? "fwd" : "back", idx);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", 160 * 4, 80 * 4);
    for (int y = 0; y < 80 * 4; y++) {
        for (int x = 0; x < 160 * 4; x++) {
            uint8_t rgb[3];
            rgb565_to_rgb888(g_fb[y / 4][x / 4], &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
}

static void capture_shot(int idx)
{
    for (int frame = 0; frame < 14; frame++) {
        s_now += 16u;
        ui_port_task_handler();
    }
    sample_heap("shot");
    memcpy(g_shots[idx], g_fb, sizeof(g_fb));
}


/* The visible toast's label (the pill on lv_layer_top() is the only visible child with a child). */
static lv_obj_t *toast_label(void)
{
    lv_obj_t *top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_child_count(c) > 0u) {
            return lv_obj_get_child(c, 0);
        }
    }
    return NULL;
}

/* Toasts show their text, not a "..." pill (LONG_DOT on a content-sized label used to dot against a 0 px width):
 * short messages are shown whole on one line and the rendered pill is as wide as the text; only a message wider
 * than 136 px is dotted. Runs for every image (default and BT=1). */
static void check_toast_text(ui_model_t *m)
{
    ui_model_t b = *m;
    static const char *const msgs[] = { "Saved", "Start of list", "PIN 246810" };
    const lv_font_t *font = NULL;
    for (unsigned k = 0; k < sizeof(msgs) / sizeof(msgs[0]); k++) {
        strcpy(b.toast_msg, msgs[k]);
        b.toast_col = UI_COL_AMBER;
        ui_render_screen(UI_SCR_HOME, &b);
        for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
        lv_obj_t *tl = toast_label();
        assert(tl != NULL);
        lv_obj_update_layout(tl);
        font = lv_obj_get_style_text_font(tl, 0);
        lv_point_t tsz;
        lv_text_get_size(&tsz, msgs[k], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int min_x = 160, max_x = -1;
        for (int y = 56; y < 80; y++) {
            for (int x = 0; x < 160; x++) {
                if (g_fb[y][x] == UI_COL_AMBER) {
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                }
            }
        }
        printf("toast: [%s] label w=%d (text %d) pill px %d..%d\n", lv_label_get_text(tl),
               (int)lv_obj_get_width(tl), (int)tsz.x, min_x, max_x);
        assert(strcmp(lv_label_get_text(tl), msgs[k]) == 0);
        assert(tsz.x < 136 && lv_obj_get_width(tl) == tsz.x);
        assert(lv_obj_get_height(tl) == lv_font_get_line_height(font));
        assert(max_x >= 0 && max_x - min_x + 1 >= tsz.x + 16);   /* text width + 2 x 8 px padding */
    }
    strcpy(b.toast_msg, "Long warning toast clipped cleanly!!!");
    ui_render_screen(UI_SCR_HOME, &b);
    for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
    lv_obj_t *tl = toast_label();
    assert(tl != NULL);
    lv_obj_update_layout(tl);
    printf("long toast: [%s] w=%d h=%d\n", lv_label_get_text(tl), (int)lv_obj_get_width(tl),
           (int)lv_obj_get_height(tl));
    assert(strstr(lv_label_get_text(tl), "Long warning") != NULL && strstr(lv_label_get_text(tl), "...") != NULL &&
           lv_obj_get_width(tl) == 136 && lv_obj_get_height(tl) == lv_font_get_line_height(font));
    b.toast_msg[0] = '\0';
    ui_render_screen(UI_SCR_HOME, &b);
    for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
}

#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID
/* BT=1 strings (include/openfw_i18n_bt.h): every language non-empty, every glyph in ui_font_sm/md, and each string
 * fits the buffer it is copied into and the pixels of the label that shows it (no "..." on a translated toast). */
static int utf8_glyphs_ok(const char *str)
{
    const uint8_t *p = (const uint8_t *)str;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80u) {
            cp = *p++;
        } else if ((*p & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((*p & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
            p += 3;
        } else {
            return 0;
        }
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(&ui_font_sm, &g, cp, 0u) || !lv_font_get_glyph_dsc(&ui_font_md, &g, cp, 0u)) {
            fprintf(stderr, "missing glyph U+%04X in \"%s\"\n", (unsigned)cp, str);
            return 0;
        }
    }
    return 1;
}

static int32_t px(const char *s, const lv_font_t *f)
{
    return lv_text_get_width(s, (uint32_t)strlen(s), f, 0);
}

enum { BK_TOAST, BK_FOOT, BK_BTFOOT, BK_BTL2, BK_LBL, BK_VAL, BK_SETVAL, BK_SETLBL, BK_PREFIX };
static const struct { ui_bt_str_id_t id; uint8_t kind; } k_bt_kinds[] = {
    { UI_BT_STR_PAIR_Q_FMT, BK_BTL2 },        { UI_BT_STR_PAIR_KEYS, BK_BTFOOT },
    { UI_BT_STR_PAIR_OPEN_FMT, BK_BTL2 },     { UI_BT_STR_BONDED, BK_BTL2 },
    { UI_BT_STR_PAIR_UNAVAIL, BK_TOAST },     { UI_BT_STR_AUDIO_OUTPUT, BK_SETLBL },
    { UI_BT_STR_OUT_SPEAKER, BK_SETVAL },     { UI_BT_STR_HEADPHONES, BK_LBL },
    { UI_BT_STR_OUT_BOTH, BK_SETVAL },        { UI_BT_STR_OUT_BT, BK_SETVAL },
    { UI_BT_STR_DESC_AUDIO_OUTPUT, BK_FOOT }, { UI_BT_STR_PHONE_SYNC, BK_LBL },
    { UI_BT_STR_DESC_PHONE, BK_FOOT },        { UI_BT_STR_DESC_HP, BK_FOOT },
    { UI_BT_STR_BT_FAULT, BK_LBL },           { UI_BT_STR_STARTING, BK_LBL },
    { UI_BT_STR_DISCONNECT, BK_LBL },         { UI_BT_STR_PAIRING, BK_LBL },
    { UI_BT_STR_CONNECTING, BK_LBL },         { UI_BT_STR_LINK_STALLED, BK_LBL },
    { UI_BT_STR_RECONNECTING, BK_LBL },       { UI_BT_STR_NOT_CONNECTED, BK_LBL },
    { UI_BT_STR_SEARCHING, BK_LBL },          { UI_BT_STR_SEARCH, BK_LBL },
    { UI_BT_STR_FORGET_PREFIX, BK_PREFIX },   { UI_BT_STR_AUDIO, BK_VAL },
    { UI_BT_STR_OTHER, BK_VAL },              { UI_BT_STR_HP_FOOT, BK_FOOT },
    { UI_BT_STR_HP_PAIR_HINT, BK_FOOT },      { UI_BT_STR_SEARCH_DONE, BK_FOOT },
    { UI_BT_STR_SEARCH_FAILED, BK_FOOT },     { UI_BT_STR_PAIR_FAILED, BK_FOOT },
    { UI_BT_STR_PAIR_REFUSED, BK_FOOT },      { UI_BT_STR_HP_CONNECTED, BK_FOOT },
    { UI_BT_STR_CONNECT_FAILED, BK_FOOT },    { UI_BT_STR_HP_DISCONNECTED, BK_TOAST },
    { UI_BT_STR_HP_FORGOTTEN, BK_FOOT },      { UI_BT_STR_LINK_STALLED_RECONN, BK_FOOT },
    { UI_BT_STR_HP_NOT_READY, BK_TOAST },     { UI_BT_STR_NOT_IN_BUILD, BK_FOOT },
    { UI_BT_STR_HP_NOT_CONNECTED, BK_TOAST }, { UI_BT_STR_HP_PAUSED, BK_TOAST },
    { UI_BT_STR_LOWBATT_BT_OFF, BK_TOAST },
};

static void check_bt_i18n(void)
{
    ui_model_t *mm = NULL;
    assert(sizeof(k_bt_kinds) / sizeof(k_bt_kinds[0]) == (size_t)UI_BT_STR_COUNT);
    int bad = 0;
    for (unsigned i = 0; i < (unsigned)UI_BT_STR_COUNT; i++) {
        assert(k_bt_kinds[i].id == (ui_bt_str_id_t)i);
    }
    for (uint8_t lang = 0u; lang < (uint8_t)UI_LANG_COUNT; lang++) {
        for (unsigned i = 0; i < (unsigned)UI_BT_STR_COUNT; i++) {
            const char *s = ui_tr_bt_lang(lang, (ui_bt_str_id_t)i);
            char f[64];
            size_t len = strlen(s);
            size_t cap = 0;          /* bytes incl. NUL of the buffer it is copied into (0: none) */
            int32_t w = 0, wmax = 0;
            if (s[0] == '\0' || !utf8_glyphs_ok(s)) {
                fprintf(stderr, "BT string %u lang %u empty or bad glyph\n", i, (unsigned)lang);
                bad++;
                continue;
            }
            switch (k_bt_kinds[i].kind) {
            case BK_TOAST:  cap = sizeof(mm->toast_msg); w = px(s, &ui_font_sm); wmax = 136; /* LONG_DOT max */ break;
            case BK_FOOT:   cap = sizeof(mm->list_foot); w = px(s, &ui_font_sm); wmax = 154; break;
            case BK_BTFOOT: w = px(s, &ui_font_sm); wmax = 160; break;
            case BK_BTL2:
                lv_snprintf(f, sizeof(f), s, 60u);
                cap = 32u;                   /* bind_bt l2s */
                len = strlen(f);
                w = px(f, &ui_font_sm);
                wmax = 120;
                break;
            case BK_LBL: {
                cap = sizeof(mm->list_lbl[0]);
                /* the widest value a row can carry next to it: "On"/"Off" or "60 s" */
                const char *v = (i == UI_BT_STR_HEADPHONES) ? (px(ui_tr_lang(lang, UI_STR_OFF), &ui_font_sm) >
                                                            px(ui_tr_lang(lang, UI_STR_ON), &ui_font_sm)
                                                                ? ui_tr_lang(lang, UI_STR_OFF)
                                                                : ui_tr_lang(lang, UI_STR_ON))
                              : (i == UI_BT_STR_RECONNECTING || i == UI_BT_STR_SEARCHING) ? "60 s" : "";
                int32_t vw = v[0] ? px(v, &ui_font_sm) + 2 : 0;
                w = px(s, &ui_font_md);
                wmax = 146 - vw - 8;
                if (i == UI_BT_STR_HEADPHONES && len + 1u > sizeof(mm->list_title)) {
                    cap = sizeof(mm->list_title);   /* also the headphones screen's title */
                }
                break;
            }
            case BK_VAL:    cap = sizeof(mm->list_val[0]); w = px(s, &ui_font_sm); wmax = 70; break;
            case BK_SETVAL:
            case BK_SETLBL:  break;      /* the Audio output row below */
            case BK_PREFIX: cap = 16u; break;
            default: break;
            }
            if ((cap != 0u && len + 1u > cap) || (wmax != 0 && w > wmax)) {
                fprintf(stderr, "BT string %u lang %u \"%s\": %zu B (buffer %zu), %d px (max %d)\n", i,
                        (unsigned)lang, s, len, cap, (int)w, (int)wmax);
                bad++;
            }
        }
        /* Settings "Audio output" row with its widest value selected ("- X +", ui_screens.c bind_settings) */
        static const ui_bt_str_id_t vals[3] = { UI_BT_STR_OUT_SPEAKER, UI_BT_STR_OUT_BT, UI_BT_STR_OUT_BOTH };
        for (unsigned k = 0; k < 3u; k++) {
            char v[40];
            const char *vs = ui_tr_bt_lang(lang, vals[k]);
            snprintf(v, sizeof(v), "- %s +", vs);
            int32_t vw = px(v, &ui_font_sm) + 2;
            int32_t lw = px(ui_tr_bt_lang(lang, UI_BT_STR_AUDIO_OUTPUT), &ui_font_md);
            if (vw < 54) vw = 54;
            if (strlen(vs) + 1u > 24u || vw > 96 || lw > 146 - vw - 8) {
                fprintf(stderr, "Settings Audio output row lang %u: label %d px, value \"%s\" %d px\n", (unsigned)lang,
                        (int)lw, v, (int)vw);
                bad++;
            }
        }
    }
    printf("BT=1 i18n: %u strings x %u languages checked, %d problems\n", (unsigned)UI_BT_STR_COUNT,
           (unsigned)UI_LANG_COUNT, bad);
    assert(bad == 0);
    assert(strcmp(ui_tr_bt_lang(200u, UI_BT_STR_BONDED), ui_tr_bt_lang(UI_LANG_EN, UI_BT_STR_BONDED)) == 0);
    assert(ui_tr_bt_lang(UI_LANG_DE, UI_BT_STR_COUNT)[0] == '\0');
}

/* BT=1 phone-mode UI checks (review findings): the toast label shows its text (not "...") on the BT screen, a long
 * peer name on the BT status line ends in "..." on one line instead of wrapping out of the 12 px label. */
static void check_bt_ui(ui_model_t *m)
{
    check_bt_i18n();
    ui_model_t b = *m;
    b.toast_msg[0] = '\0';
    b.bt_state = 1u;
    b.bt_bonded = 1u;
    strcpy(b.bt_peer, "Silvio's Pixel 8 Pro");
    ui_render_screen(UI_SCR_BT, &b);
    for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
    lv_obj_update_layout(objects.bt_l1_lbl);
    const char *l1 = lv_label_get_text(objects.bt_l1_lbl);
    printf("BT=1 status line: [%s] h=%d\n", l1, (int)lv_obj_get_height(objects.bt_l1_lbl));
    assert(strstr(l1, "Silvio") != NULL && strstr(l1, "...") != NULL);   /* dotted, not wrapped away */
    strcpy(b.bt_peer, "44:55:66");
    ui_render_screen(UI_SCR_BT, &b);
    l1 = lv_label_get_text(objects.bt_l1_lbl);
    assert(strstr(l1, "44:55:66") != NULL && strstr(l1, "...") == NULL);  /* a short peer is shown whole */

    /* Silicon session 6: the prompt's peer is the address until the name arrives, then the name in the same place
     * (the digits stay); after the prompt a link that is not connected shows the waiting line, not "connected". */
    b.bt_prompt = 1u;
    b.bt_prompt_s = 27u;
    strcpy(b.bt_digits, "333198");
    strcpy(b.bt_peer, "00:5B:1A:51:7E:E5");
    ui_render_screen(UI_SCR_BT, &b);
    assert(strcmp(lv_label_get_text(objects.bt_l1_lbl), "00:5B:1A:51:7E:E5") == 0);
    assert(strcmp(lv_label_get_text(objects.bt_name_lbl), "333198") == 0);
    strcpy(b.bt_peer, "martianbfg");
    ui_render_screen(UI_SCR_BT, &b);
    printf("BT=1 prompt peer after the late name: [%s] value [%s]\n", lv_label_get_text(objects.bt_l1_lbl),
           lv_label_get_text(objects.bt_name_lbl));
    assert(strcmp(lv_label_get_text(objects.bt_l1_lbl), "martianbfg") == 0);
    assert(strcmp(lv_label_get_text(objects.bt_name_lbl), "333198") == 0);
    char connected_line[52];
    lv_snprintf(connected_line, sizeof(connected_line), ui_tr(UI_STR_BT_CONNECTED_FMT), "martianbfg");
    b.bt_prompt = 0u;
    b.bt_digits[0] = '\0';
    b.bt_state = 0u;                                  /* BACK: the host is ending the link */
    b.bt_bonded = 0u;
    ui_render_screen(UI_SCR_BT, &b);
    assert(strcmp(lv_label_get_text(objects.bt_l1_lbl), connected_line) != 0);
    assert(strcmp(lv_label_get_text(objects.bt_l1_lbl), ui_tr(UI_STR_BT_WAITING)) == 0);

    static const char *const msgs[] = { "Pairing unavailable", "PIN 246810", "Saved" };
    b.bt_state = 0u;
    b.bt_bonded = 0u;
    for (unsigned k = 0; k < sizeof(msgs) / sizeof(msgs[0]); k++) {
        strcpy(b.toast_msg, msgs[k]);
        b.toast_col = UI_COL_AMBER;
        ui_render_screen(UI_SCR_BT, &b);
        for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
        lv_obj_t *tl = toast_label();
        assert(tl != NULL);
        lv_obj_update_layout(tl);
        printf("BT=1 toast: [%s] w=%d\n", lv_label_get_text(tl), (int)lv_obj_get_width(tl));
        assert(strcmp(lv_label_get_text(tl), msgs[k]) == 0);
    }
    strcpy(b.toast_msg, "Long warning toast clipped cleanly!!!");
    ui_render_screen(UI_SCR_HOME, &b);
    for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
    lv_obj_t *tl = toast_label();
    assert(tl != NULL);
    lv_obj_update_layout(tl);
    printf("BT=1 long toast: [%s] w=%d h=%d\n", lv_label_get_text(tl), (int)lv_obj_get_width(tl),
           (int)lv_obj_get_height(tl));
    assert(strstr(lv_label_get_text(tl), "Long warning") != NULL && strstr(lv_label_get_text(tl), "...") != NULL &&
           lv_obj_get_width(tl) == 136 && lv_obj_get_height(tl) == lv_font_get_line_height(lv_obj_get_style_text_font(tl, 0)));
    b.toast_msg[0] = '\0';
    ui_render_screen(UI_SCR_HOME, &b);
    for (int frame = 0; frame < 14; frame++) { s_now += 16u; ui_port_task_handler(); }
}
#endif

int main(int argc, char **argv)
{
    const char *out_ppm = (argc > 1) ? argv[1] : "/tmp/sl6801_openfw/ui_all_screens.ppm";

    lcd_set_rotation(1);

    /* Sanity-check LCD bounds/NULL/digits guards */
    lcd_draw_str(0u, 0u, NULL, UI_COL_TEXT, UI_COL_BG);
    lcd_draw_str_2x(0u, 0u, NULL, UI_COL_TEXT, UI_COL_BG);
    lcd_draw_hex(0u, 0u, 0xDEADBEEFu, 12u, UI_COL_TEXT, UI_COL_BG);
    lcd_fill_rect(65530u, 65530u, 20u, 20u, UI_COL_BG);
    {
        uint16_t tmp_px[16];
        for (int i = 0; i < 16; i++) tmp_px[i] = UI_COL_CYAN;
        lcd_blit_rgb565_stride(158u, 78u, 4u, 4u, tmp_px, 4u);
        lcd_blit_rgb565_alpha(158u, 78u, 4u, 4u, tmp_px, 255u);
        lcd_blit_rgb565_alpha(158u, 78u, 4u, 4u, tmp_px, 0u);
        lcd_blit_rgb565_alpha(158u, 78u, 4u, 4u, tmp_px, 128u);
    }

    ui_spectrogram_t spec;
    ui_spec_reset(&spec);
    /* Feed realistic synthetic voice sweep into spectrogram + drop 2 markers */
    for (int col = 0; col < 144; col++) {
        int16_t pcm[128];
        float f0 = 300.0f + 900.0f * (0.5f + 0.5f * sinf((float)col * 0.11f));
        float amp = ((col % 28) < 20) ? 14000.0f : 800.0f;
        for (int i = 0; i < 128; i++) {
            float t = (float)i / 16000.0f;
            float s = sinf(6.2831853f * f0 * t) * 0.6f
                    + sinf(6.2831853f * (f0 * 2.0f) * t) * 0.3f
                    + sinf(6.2831853f * (f0 * 3.0f) * t) * 0.15f;
            pcm[i] = (int16_t)(s * amp);
        }
        ui_spec_push_pcm(&spec, pcm, 128u, 1u, 0);
        if (col == 60 || col == 112) {
            ui_spec_drop_marker(&spec);
        }
    }

    static const ui_file_entry_t demo_files[4] = {
        { "REC_0043.wav", { 2026, 11, 20, 5, 14, 32, 0 }, 192,  65, 1, 1, 2, 0 },
        { "REC_0042.wav", { 2026, 11, 20, 5,  9,  5, 0 }, 1475, 0,  0, 1, 2, 0 },
        { "REC_0041.wav", { 2026, 11, 19, 4, 18, 47, 0 }, 38,   0,  1, 0, 1, 0 },
        { "REC_0040.wav", { 2026, 11, 16, 1,  8,  2, 0 }, 245,  0,  0, 0, 2, 0 },
    };

    ui_model_t m;
    memset(&m, 0, sizeof(m));
    ui_settings_defaults(&m.cfg);
    assert(m.cfg.sound_mode == AUDIO_DSP_VOICE_CLARITY);
    m.now          = (rtc_datetime_t){ 2026, 11, 20, 5, 14, 32, 0 };
    m.batt_pct     = 72;
    m.batt_mv      = 3920;
    m.hp_plugged   = 1;
    m.sd_ok        = 1;
    m.sd_total_mb  = 59596;
    m.sd_free_sec  = 18u * 3600u + 20u * 60u;
    strcpy(m.bt_name, "HW_76");
    m.home_focus   = 1;
    m.has_last     = 1;
    m.last_file    = demo_files[0];
    m.files        = demo_files;
    m.file_count   = 4;
    m.file_sel     = 0;
    m.file_scroll  = 0;
    m.spec         = &spec;

    /* 0: Home (Files focused) */
    ui_render_screen(UI_SCR_HOME, &m);
    capture_shot(0);

    /* Drive the real UI timers at 60 Hz. Only the footer and underline
     * should be flushed once the newly selected tiles have been painted. */
    lv_obj_t *footer = objects.home_details;
    int32_t page_width = lv_obj_get_width(footer);
    assert(lv_obj_get_scroll_x(footer) == page_width);
    m.home_focus = 0;
    ui_render_screen(UI_SCR_HOME, &m);
    int32_t last_scroll = page_width;
    unsigned moving_frames = 0;
    for (unsigned frame = 0; frame < 10; frame++) {
        uint32_t start = g_written_pixels;
        s_now += 16u;
        ui_port_task_handler();
        int32_t offset = lv_obj_get_scroll_x(footer);
        assert(offset <= last_scroll);
        if (g_written_pixels > start) moving_frames++;
        assert(g_written_pixels - start <= 160u * 27u);
        last_scroll = offset;
    }
    assert(last_scroll == 0 && moving_frames >= 5);
    assert(!ui_port_is_animating());
    int32_t mid_x = lv_obj_get_x(objects.home_focus_mark);
    uint32_t before;

    for (uint8_t focus = 1; focus <= 3; focus++) {
        m.home_focus = focus;
        ui_render_screen(UI_SCR_HOME, &m);
        assert(lv_obj_get_x(objects.home_focus_mark) == mid_x);
        assert(lv_anim_count_running() == 1u);
        s_now += 16u;
        ui_port_task_handler();
        ui_port_render_now();
        mid_x = lv_obj_get_x(objects.home_focus_mark);
    }
    s_now += 300u;
    ui_port_task_handler();
    ui_port_render_now();
    assert(lv_obj_get_x(objects.home_focus_mark) == lv_obj_get_x(objects.home_tile_3) +
           (lv_obj_get_width(objects.home_tile_3) - lv_obj_get_width(objects.home_focus_mark)) / 2);
    assert(lv_obj_get_scroll_x(footer) == 3 * page_width);
    assert(!ui_port_is_animating());
    before = g_written_pixels;
    ui_render_screen(UI_SCR_HOME, &m);
    assert(g_written_pixels == before);

    /* Clamped out-of-range home_focus stays safe */
    m.home_focus = 99;
    ui_render_screen(UI_SCR_HOME, &m);
    assert(lv_obj_get_scroll_x(footer) == 3 * page_width);

    /* Enter recording while the return animation is running. */
    m.home_focus = 1;
    ui_render_screen(UI_SCR_HOME, &m);

    /* 1: Recording (live spectrogram + markers) */
    m.rec_sec     = 13u * 60u + 21u;
    m.rec_blink   = 1;
    m.rec_vu_l    = 68;
    m.rec_vu_r    = 62;
    m.rec_peak_l  = 82;
    m.rec_peak_r  = 74;
    m.rec_markers = 2;
    strcpy(m.rec_filename, "REC_0043.wav");
    ui_render_screen(UI_SCR_REC, &m);
    capture_shot(1);

    /* Unchanged UI_SCR_REC must write 0 pixels */
    before = g_written_pixels;
    ui_render_screen(UI_SCR_REC, &m);
    assert(g_written_pixels == before);

    /* Blinking REC dot must NOT redraw the footer or spectrogram */
    m.rec_blink = 0;
    before = g_written_pixels;
    ui_render_screen(UI_SCR_REC, &m);
    assert(g_written_pixels > before && g_written_pixels - before < 160u * 14u);
    m.rec_blink = 1;
    ui_render_screen(UI_SCR_REC, &m);

    /* 2: Recording VOR Wait */
    m.rec_vor_wait = 1;
    ui_render_screen(UI_SCR_REC, &m);
    capture_shot(2);
    m.rec_vor_wait = 0;

    /* 3: Saved card */
    m.saved_sec      = 12u * 60u + 34u;
    m.saved_channels = 2;
    m.saved_frac_pct = 65;
    m.saved_dt       = m.now;
    strcpy(m.saved_filename, "REC_0043.wav");
    ui_render_screen(UI_SCR_SAVED, &m);
    capture_shot(3);

    /* Unchanged UI_SCR_SAVED must write 0 pixels */
    before = g_written_pixels;
    ui_render_screen(UI_SCR_SAVED, &m);
    assert(g_written_pixels == before);

    /* 4: Files list */
    ui_render_screen(UI_SCR_FILES, &m);
    capture_shot(4);
    before = g_written_pixels;
    ui_render_screen(UI_SCR_FILES, &m);
    assert(g_written_pixels == before);

    /* Negative file_scroll guard must not crash or read out of bounds */
    m.file_scroll = -2;
    ui_render_screen(UI_SCR_FILES, &m);
    m.file_scroll = 0;
    ui_render_screen(UI_SCR_FILES, &m);

    /* Exercise folder row + Italian accented audiobook title rendering */
    {
        static const ui_file_entry_t folder_files[2] = {
            { "Jekyll_Hyde",     { 2026, 11, 20, 5, 14, 32, 0 }, 41,  0, 0, 0, 0, 1 },
            { "JH_01a_Cap1.wav", { 2026, 11, 20, 5, 14, 32, 0 }, 185, 0, 1, 1, 1, 0 },
        };
        m.files = folder_files;
        m.file_count = 2;
        strcpy(m.files_title, "Jekyll Hyde");
        ui_render_screen(UI_SCR_FILES, &m);
        assert(strcmp(lv_label_get_text(objects.files_name_0), "Jekyll Hyde") == 0);
        assert(strcmp(lv_label_get_text(objects.files_name_1), "JH 01a Cap1") == 0);
        m.files = demo_files;
        m.file_count = 4;
        m.files_title[0] = '\0';
        ui_render_screen(UI_SCR_FILES, &m);
    }

    /* Verify European glyph coverage (Latin-1, Latin Ext-A, Greek, Cyrillic) across all 3 fonts */
    {
        static const uint32_t euro_cps[] = {
            0x00E4u, 0x00F6u, 0x00FCu, 0x00C4u, 0x00D6u, 0x00DFu, /* ä ö ü Ä Ö ß */
            0x0142u, 0x0159u, 0x0219u,                             /* ł ř ș */
            0x0394u, 0x03B1u, 0x03C9u, 0x03ACu,                    /* Δ α ω ά */
            0x0414u, 0x0416u, 0x044Fu, 0x0451u, 0x0491u,           /* Д Ж я ё ґ */
            0x20ACu,                                               /* € */
        };
        const lv_font_t *fonts[3] = { &ui_font_sm, &ui_font_md, &ui_font_lg };
        for (int f = 0; f < 3; f++) {
            for (size_t i = 0; i < sizeof(euro_cps) / sizeof(euro_cps[0]); i++) {
                lv_font_glyph_dsc_t gd;
                bool ok = lv_font_get_glyph_dsc(fonts[f], &gd, euro_cps[i], 0u);
                assert(ok && gd.box_w > 0u && gd.box_h > 0u);
            }
        }
    }

    /* 5: Player (Time-first) */
    m.play_file           = demo_files[0];
    m.play_pos_sec        = 120;
    m.play_state          = 0;
    m.play_vu_l           = 55;
    m.play_vu_r           = 48;
    m.play_has_transcript = 0;
    ui_render_screen(UI_SCR_PLAYER, &m);
    capture_shot(5);
    uint32_t player_pixels = g_written_pixels;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(g_written_pixels == player_pixels);
    m.play_vu_l = 20u;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(g_written_pixels > player_pixels && g_written_pixels - player_pixels < 160u * 10u);

    /* 5b: Player speed dial: speed pill, header state, and dial footer */
    {
        static const struct { int8_t speed; uint8_t state; const char *pill, *head, *foot; } dial[] = {
            {   0, 0u, NULL,   "Playing", "\xE2\x97\x80 \xE2\x88\x92" "1\xC3\x97 \xC2\xB7 OK Pause \xC2\xB7 2\xC3\x97 \xE2\x96\xB6" },
            {   0, 1u, NULL,   "Paused",  "\xE2\x97\x80 \xE2\x88\x92" "1\xC3\x97 \xC2\xB7 OK Play \xC2\xB7 2\xC3\x97 \xE2\x96\xB6" },
            {   2, 0u, "2\xC3\x97", "Fast", "\xE2\x97\x80 1\xC3\x97 \xC2\xB7 OK 1\xC3\x97 \xC2\xB7 4\xC3\x97 \xE2\x96\xB6" },
            {  -4, 0u, "\xE2\x88\x92" "4\xC3\x97", "Reverse",
               "\xE2\x97\x80 \xE2\x88\x92" "8\xC3\x97 \xC2\xB7 OK 1\xC3\x97 \xC2\xB7 \xE2\x88\x92" "2\xC3\x97 \xE2\x96\xB6" },
            {  16, 0u, "16\xC3\x97", "Seeking", "\xE2\x97\x80 8\xC3\x97 \xC2\xB7 OK 1\xC3\x97 \xC2\xB7 32\xC3\x97 \xE2\x96\xB6" },
            {  32, 0u, "32\xC3\x97", "Seeking", "\xE2\x97\x80 16\xC3\x97 \xC2\xB7 OK 1\xC3\x97" },
            { -32, 0u, "\xE2\x88\x92" "32\xC3\x97", "Seeking", "OK 1\xC3\x97 \xC2\xB7 \xE2\x88\x92" "16\xC3\x97 \xE2\x96\xB6" },
        };
        for (size_t i = 0; i < sizeof(dial) / sizeof(dial[0]); i++) {
            m.play_scrub_speed = dial[i].speed;
            m.play_state       = dial[i].state;
            ui_render_screen(UI_SCR_PLAYER, &m);
            assert(strcmp(lv_label_get_text(objects.play_bar_l), dial[i].head) == 0);
            if (strcmp(lv_label_get_text(objects.player_footer), dial[i].foot) != 0) {
                fprintf(stderr, "dial footer %d: \"%s\" != \"%s\"\n", dial[i].speed,
                        lv_label_get_text(objects.player_footer), dial[i].foot);
                assert(0);
            }
            if (dial[i].pill) assert(strcmp(lv_label_get_text(objects.player_vol_lbl), dial[i].pill) == 0);
            else assert(strstr(lv_label_get_text(objects.player_vol_lbl), "\xC3\x97") == NULL); /* volume */
            int32_t fw = lv_text_get_width(dial[i].foot, (uint32_t)strlen(dial[i].foot),
                                           lv_obj_get_style_text_font(objects.player_footer, LV_PART_MAIN), 0);
            assert(fw <= 156);
        }
        m.cfg.skip_mode = UI_SKIP_TRACK;
        m.play_scrub_speed = 0;
        ui_render_screen(UI_SCR_PLAYER, &m);
        assert(strcmp(lv_label_get_text(objects.player_footer),
                      "\xE2\x97\x80 Prev \xC2\xB7 OK Pause \xC2\xB7 \xE2\x96\xB6 Next") == 0);
        m.cfg.skip_mode = UI_SKIP_SPEED;
        /* Every language's dial header word fits where the longest play state does. */
        const lv_font_t *hf = lv_obj_get_style_text_font(objects.play_bar_l, LV_PART_MAIN);
        for (uint8_t lang = 0u; lang < (uint8_t)UI_LANG_COUNT; lang++) {
            static const ui_str_id_t ids[] = { UI_STR_FAST, UI_STR_REVERSE, UI_STR_SEEKING };
            for (size_t k = 0; k < 3; k++) {
                const char *t = ui_tr_lang(lang, ids[k]);
                int32_t w = lv_text_get_width(t, (uint32_t)strlen(t), hf, 0);
                if (w > 60) {
                    fprintf(stderr, "Player header too wide for lang=%u: \"%s\" %d px\n", (unsigned)lang, t, (int)w);
                    assert(0);
                }
            }
        }
        m.play_scrub_speed = 0;
        m.play_state       = 0;
        ui_render_screen(UI_SCR_PLAYER, &m);
    }

    /* 6: Player (Full-Height Smooth-Scrolling Transcript mode) */
    /* First verify cwx_measure_md_px matches ui_font_md glyph-for-glyph for all ASCII 0x20..0x7E */
    for (int cp = 0x20; cp <= 0x7E; cp++) {
        char ch[2] = { (char)cp, '\0' };
        uint32_t cwx_w  = cwx_measure_md_px(ch, 1u);
        int32_t  lvgl_w = lv_text_get_width(ch, 1u, &ui_font_md, 0);
        assert((int32_t)cwx_w == lvgl_w);
    }

    m.play_has_transcript = 1;
    m.play_pos_sec        = 28;
    m.play_pos_ms         = 28400u;
    m.play_dur_ms         = 64783u;
    m.play_file           = demo_files[1];
    m.play_file.duration_sec = 65u;
    m.play_state          = 0u;
    m.play_scrub_speed    = 0;
    m.play_vol_hi         = 0u;
    m.tr_speaker          = 3u;
    m.tr_speaker_color    = 0xFC15u; /* Coral/Pink (Earl Maize) */
    strcpy(m.tr_speaker_name, "Earl Maize");
    memset(m.tr_lines, 0, sizeof(m.tr_lines));
    m.tr_lines[0] = (cwx_tr_slot_t){ 0, 0x7792u, 2u, 1u, "Okay, we call loss of" };
    m.tr_lines[1] = (cwx_tr_slot_t){ 1, 0xFC15u, 3u, 1u, "left, but just heard" };
    m.tr_lines[2] = (cwx_tr_slot_t){ 2, 0xFC15u, 3u, 0u, "the signal from the" };
    m.tr_lines[3] = (cwx_tr_slot_t){ 3, 0xFC15u, 3u, 0u, "spacecraft is gone," };
    m.tr_lines[4] = (cwx_tr_slot_t){ 4, 0xFC15u, 3u, 0u, "and within the next" };
    m.tr_lines[5] = (cwx_tr_slot_t){ 5, 0xFC15u, 3u, 0u, "forty-five seconds," };
    m.tr_top_line_id = 1;
    m.tr_active_slot = 2; /* Row 1 = "the signal from the" */
    m.tr_spoken_len  = 10u; /* "the signal" */
    m.tr_word_start  = 4u;  /* "signal" */
    m.tr_word_len    = 6u;
    ui_render_screen(UI_SCR_PLAYER, &m);
    capture_shot(6);

    /* Verify spoken (white) vs upcoming (dim) color distinction on active line (Row 1: y=32..47)
     * and full 4-line utilization reaching the bottom of the screen (y=64..79) */
    {
        int has_spoken_bright = 0, has_upcoming_dim = 0, has_row3_ink = 0;
        for (int y = 32; y < 48; y++) {
            for (int x = 6; x < 75; x++) {
                if (g_fb[y][x] == UI_COL_TEXT) has_spoken_bright = 1;
            }
            for (int x = 75; x < 158; x++) {
                if (g_fb[y][x] == UI_COL_DIM) has_upcoming_dim = 1;
            }
        }
        for (int y = 65; y < 79; y++) {
            for (int x = 6; x < 150; x++) {
                if (g_fb[y][x] != UI_COL_BG) has_row3_ink = 1;
            }
        }
        assert(has_spoken_bright && has_upcoming_dim && has_row3_ink);
    }

    /* Unchanged transcript state produces 0 redraw pixels */
    before = g_written_pixels;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(g_written_pixels == before);

    /* Advance active word on same line -> smooth horizontal pill glide animation */
    int32_t pill_x0 = lv_obj_get_style_x(objects.ptr_word_pill, LV_PART_MAIN);
    m.tr_spoken_len = 15u; /* "the signal from" */
    m.tr_word_start = 11u; /* "from" */
    m.tr_word_len   = 4u;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(ui_port_is_animating());
    for (int f = 0; f < 10; f++) {
        s_now += 16u;
        ui_port_task_handler();
    }
    assert(!ui_port_is_animating());
    assert(lv_obj_get_style_x(objects.ptr_word_pill, LV_PART_MAIN) > pill_x0 + 20);

    /* Advance top_line_id by +1 -> smooth vertical upward scroll animation on ptr_track */
    m.tr_lines[0] = (cwx_tr_slot_t){ 1, 0xFC15u, 3u, 1u, "left, but just heard" };
    m.tr_lines[1] = (cwx_tr_slot_t){ 2, 0xFC15u, 3u, 0u, "the signal from the" };
    m.tr_lines[2] = (cwx_tr_slot_t){ 3, 0xFC15u, 3u, 0u, "spacecraft is gone," };
    m.tr_lines[3] = (cwx_tr_slot_t){ 4, 0xFC15u, 3u, 0u, "and within the next" };
    m.tr_lines[4] = (cwx_tr_slot_t){ 5, 0xFC15u, 3u, 0u, "forty-five seconds," };
    m.tr_lines[5] = (cwx_tr_slot_t){ 6, 0xFC15u, 3u, 0u, "so will be the" };
    m.tr_top_line_id = 2;
    m.tr_active_slot = 2;
    m.tr_spoken_len  = 10u; /* "spacecraft" */
    m.tr_word_start  = 0u;
    m.tr_word_len    = 10u;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(ui_port_is_animating());
    int32_t trk_y_prev = lv_obj_get_style_y(objects.ptr_track, LV_PART_MAIN);
    assert(trk_y_prev > -19);
    for (int f = 0; f < 16; f++) {
        s_now += 16u;
        ui_port_task_handler();
        int32_t trk_y_now = lv_obj_get_style_y(objects.ptr_track, LV_PART_MAIN);
        assert(trk_y_now <= trk_y_prev);
        trk_y_prev = trk_y_now;
    }
    assert(!ui_port_is_animating() && trk_y_prev == -19);

    /* Exercise volume pill + scrub badge + silence pause in transcript mode */
    m.play_vol_hi = 1u;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(!lv_obj_has_flag(objects.ptr_vol_pill, LV_OBJ_FLAG_HIDDEN));
    m.play_vol_hi      = 0u;
    m.play_scrub_speed = 3;
    m.tr_word_len      = 0u; /* silence hides pill */
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(lv_obj_has_flag(objects.ptr_word_pill, LV_OBJ_FLAG_HIDDEN));
    m.play_scrub_speed    = 0;
    m.play_has_transcript = 0;

    /* 7: Delete? */
    m.del_hold_pct = 55;
    m.del_holding  = 1;
    ui_render_screen(UI_SCR_DELETE, &m);
    capture_shot(7);
    before = g_written_pixels;
    ui_render_screen(UI_SCR_DELETE, &m);
    assert(g_written_pixels == before);

    /* 8: Settings */
    m.set_sel = 0;
    m.set_scroll = 0;
    ui_render_screen(UI_SCR_SETTINGS, &m);
    capture_shot(8);
    before = g_written_pixels;
    ui_render_screen(UI_SCR_SETTINGS, &m);
    assert(g_written_pixels == before);

    /* Negative set_scroll guard must not read out of bounds */
    m.set_scroll = -1;
    ui_render_screen(UI_SCR_SETTINGS, &m);
    m.set_scroll = 0;

    /* 9: Settings Clock Edit */
    m.set_sel        = 6;
    m.set_scroll     = 5;
    m.set_edit_clock = 1;
    m.set_edit_h     = 14;
    m.set_edit_m     = 32;
    ui_render_screen(UI_SCR_SETTINGS, &m);
    capture_shot(9);
    m.set_edit_clock = 0;

    /* 10: About + verify about_title matches BOARD_FW_TITLE_STR */
    ui_render_screen(UI_SCR_ABOUT, &m);
    capture_shot(10);
    assert(strcmp(lv_label_get_text(objects.about_title), BOARD_FW_TITLE_STR) == 0);
    before = g_written_pixels;
    ui_render_screen(UI_SCR_ABOUT, &m);
    assert(g_written_pixels == before);

    /* 11: USB Disk */
    m.charging      = 1;
    m.usb_read_act  = 1;
    m.usb_write_act = 1;
    m.usb_tx_bytes  = 14800000ull; /* 14.1 MB */
    m.usb_rx_bytes  = 262144ull;   /* 256 KB */
    ui_render_screen(UI_SCR_USB, &m);
    capture_shot(11);
    assert(strcmp(lv_label_get_text(objects.usb_tx_lbl), "\xE2\x96\xBC 14.1 MB") == 0);
    assert(strcmp(lv_label_get_text(objects.usb_rx_lbl), "\xE2\x96\xB2 256 KB") == 0);
    before = g_written_pixels;
    ui_render_screen(UI_SCR_USB, &m);
    assert(g_written_pixels == before);

    /* 12: Bluetooth Sync (sending) */
    m.charging = 0;
    m.bt_state = 2;
    m.bt_sent  = 3;
    m.bt_total = 12;
    m.bt_pct   = 45;
    ui_render_screen(UI_SCR_BT, &m);
    capture_shot(12);
    before = g_written_pixels;
    ui_render_screen(UI_SCR_BT, &m);
    assert(g_written_pixels == before);

    /* Headset button probe screen (About layout) incl. prompt + unknown plug, then About
     * must render exactly as before (probe binder must not leak label state). */
    {
        ui_render_screen(UI_SCR_ABOUT, &m);
        capture_shot(0);
        static uint16_t about_ref[80][160];
        memcpy(about_ref, g_fb, sizeof(about_ref));
        m.hsp_secs_left = 27; m.hsp_phase = 1; m.hsp_prompt = 0; m.hsp_plug = 1;
        ui_render_screen(UI_SCR_HSPROBE, &m);
        capture_shot(0);
        m.hsp_prompt = 2; m.hsp_phase = 2;
        ui_render_screen(UI_SCR_HSPROBE, &m);
        capture_shot(0);
        {
            int amber = 0;
            for (int y = 14; y < 34; y++)
                for (int x = 0; x < 160; x++)
                    if (g_fb[y][x] == UI_COL_AMBER) amber++;
            assert(amber > 20); /* "PRESS button now" highlighted */
        }
        m.hsp_prompt = 0; m.hsp_phase = 3; m.hsp_plug = -1;
        ui_render_screen(UI_SCR_HSPROBE, &m);
        sample_heap("hsprobe");
        ui_render_screen(UI_SCR_ABOUT, &m);
        capture_shot(0);
        assert(memcmp(g_fb, about_ref, sizeof(about_ref)) == 0);
        int saved_sel = m.set_sel, saved_scroll = m.set_scroll;
        m.set_sel = UI_SET_HSPROBE; m.set_scroll = UI_SET_COUNT - 3;
        ui_render_screen(UI_SCR_SETTINGS, &m);
        sample_heap("settings_hsprobe");
        m.set_sel = saved_sel; m.set_scroll = saved_scroll;
    }

    /* Exercise extra sub-screens for heap measurement */
    ui_render_screen(UI_SCR_CHARGE, &m);
    sample_heap("charge");
    before = g_written_pixels;
    ui_render_screen(UI_SCR_CHARGE, &m);
    assert(g_written_pixels == before);

    m.sd_ok = 0;
    ui_render_screen(UI_SCR_REC, &m);
    sample_heap("rec_err");
    m.sd_ok = 1;

    m.file_count = 0;
    ui_render_screen(UI_SCR_FILES, &m);
    sample_heap("files_empty");
    m.file_count = 4;

#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID
    check_bt_ui(&m);
#endif
    check_toast_text(&m);
    /* Verify visible toast (including long truncated toast) is a compact bottom pill
     * (never a full-screen 160x160 default-sized circle) and produces 0 redraw pixels when unchanged */
    strcpy(m.toast_msg, "Long warning toast clipped cleanly!!!");
    m.toast_col = UI_COL_AMBER;
    ui_render_screen(UI_SCR_HOME, &m);
    capture_shot(0); /* wait for any transition, then re-test toast stability */
    {
        int top_amber = 0, bot_amber = 0;
        for (int y = 0; y < 52; y++) {
            for (int x = 0; x < 160; x++) {
                if (g_fb[y][x] == UI_COL_AMBER) top_amber++;
            }
        }
        for (int y = 56; y < 80; y++) {
            for (int x = 0; x < 160; x++) {
                if (g_fb[y][x] == UI_COL_AMBER) bot_amber++;
            }
        }
        assert(top_amber == 0 && bot_amber > 100);
    }
    before = g_written_pixels;
    ui_render_screen(UI_SCR_HOME, &m);
    assert(g_written_pixels == before);
    sample_heap("home_with_toast");

    /* 13: Power Off — verify active toast is hidden on SCREEN_ID_POWER */
    m.power_hold_pct = 60;
    ui_render_screen(UI_SCR_POWER, &m);
    capture_shot(13);
    /* Compare against rendering UI_SCR_POWER with no toast message */
    m.toast_msg[0] = '\0';
    m.toast_col    = 0u;
    ui_render_screen(UI_SCR_POWER, &m);
    assert(memcmp(g_fb, g_shots[13], sizeof(g_fb)) == 0);

    /* Re-capture clean shot 0 (Home without toast) for contact sheet + reveal test */
    m.home_focus = 1;
    ui_render_screen(UI_SCR_HOME, &m);
    capture_shot(0);

    /* Drive the production push transition in both directions. The SSA hook
     * checks the panel before and after every scroll step: it must always show
     * exactly old page | 2 px seam | new page at the current offset. Forward
     * runs on a 1 ms tick (CPU1 cadence), back on 8 ms ticks (late-tick catch-up). */
    ui_render_screen(UI_SCR_HOME, &m);
    s_now += 300u;
    ui_port_task_handler();
    for (int forward = 1; forward >= 0; forward--) {
        int incoming = forward ? 4 : 0;
        int outgoing = forward ? 0 : 4;
        assert(memcmp(g_fb, g_shots[outgoing], sizeof(g_fb)) == 0);
        g_tr_old  = g_shots[outgoing];
        g_tr_new  = g_shots[incoming];
        g_tr_back = !forward;
        g_tr_d    = 0;
        g_ssa_hook = tr_check_hook;
        unsigned ssa0 = g_ssa_writes;
        uint32_t start = g_written_pixels;
        ui_render_screen(forward ? UI_SCR_FILES : UI_SCR_HOME, &m);
        sample_heap(forward ? "home->files" : "files->home");
        assert(ui_transition_busy() && ui_port_is_animating());
        uint32_t t0 = s_now, tick = forward ? 1u : 8u;
        int frame_no = 0;
        dump_tr_frame(forward, frame_no++);
        while (s_now - t0 < 200u) {
            s_now += tick;
            ui_port_task_handler();
            if (((s_now - t0) % 16u) == 0u) dump_tr_frame(forward, frame_no++);
            if (s_now - t0 == 80u) assert(g_tr_d >= 120 && g_tr_d < 162);
        }
        g_ssa_hook = NULL;
        assert(!ui_transition_busy() && !ui_port_is_animating());
        assert(g_tr_d == 162 && g_ssa == 0u && g_ssa_writes - ssa0 == 81u);
        assert(g_written_pixels - start == 160u * 80u);
        assert(memcmp(g_fb, g_shots[incoming], sizeof(g_fb)) == 0);
    }

    /* Folder navigation inside Files: enter = FORWARD, leave = BACK, driven by
     * ui_model_nav(); cursor moves, reloads and refreshes must not animate. */
    {
        static uint16_t root_shot[80][160], sub_shot[80][160];
        static ui_file_entry_t sub_files[2];
        memset(sub_files, 0, sizeof(sub_files));
        strcpy(sub_files[0].filename, "Chapter 01.mp3");
        sub_files[0].dt = m.now;
        sub_files[0].duration_sec = 1834;
        sub_files[0].channels = 2;
        strcpy(sub_files[1].filename, "Chapter 02.mp3");
        sub_files[1].dt = m.now;
        sub_files[1].duration_sec = 2101;
        sub_files[1].channels = 2;
        ui_model_t root = m, sub = m;
        sub.files = sub_files;
        sub.file_count = 2;
        sub.file_sel = 0;
        sub.file_scroll = 0;
        strcpy(sub.files_title, "Audiobook");

        /* Reference frames, switched without navigation (no push). */
        ui_render_screen(UI_SCR_FILES, &root);
        s_now += 300u;
        ui_port_task_handler();
        unsigned ssa0 = g_ssa_writes;
        memcpy(root_shot, g_fb, sizeof(g_fb));
        ui_render_screen(UI_SCR_FILES, &sub);
        s_now += 300u;
        ui_port_task_handler();
        memcpy(sub_shot, g_fb, sizeof(g_fb));
        ui_render_screen(UI_SCR_FILES, &root);
        s_now += 300u;
        ui_port_task_handler();
        assert(g_ssa_writes == ssa0 && !ui_transition_busy());
        assert(memcmp(g_fb, root_shot, sizeof(g_fb)) == 0);
        assert(memcmp(root_shot, sub_shot, sizeof(g_fb)) != 0);

        for (int enter = 1; enter >= 0; enter--) {
            ui_model_t *to = enter ? &sub : &root;
            to->nav_seq = root.nav_seq;
            ui_model_nav(to, enter ? UI_NAV_FORWARD : UI_NAV_BACK);
            root.nav_seq = sub.nav_seq = to->nav_seq;
            g_tr_old  = enter ? root_shot : sub_shot;
            g_tr_new  = enter ? sub_shot : root_shot;
            g_tr_back = !enter;
            g_tr_d    = 0;
            g_ssa_hook = tr_check_hook;
            unsigned w0 = g_ssa_writes;
            ui_render_screen(UI_SCR_FILES, to);
            assert(ui_transition_busy());
            /* A live refresh mid-push (same nav_seq) must not restart it. */
            s_now += 30u;
            ui_port_task_handler();
            ui_render_screen(UI_SCR_FILES, to);
            for (int i = 0; i < 200; i++) {
                s_now += 1u;
                ui_port_task_handler();
            }
            g_ssa_hook = NULL;
            assert(!ui_transition_busy() && g_ssa == 0u && g_tr_d == 162);
            assert(g_ssa_writes - w0 == 81u);
            assert(memcmp(g_fb, enter ? sub_shot : root_shot, sizeof(g_fb)) == 0);
        }

        /* Cursor move and same-folder reload after navigation: no push. */
        unsigned w1 = g_ssa_writes;
        root.file_sel = 1;
        ui_render_screen(UI_SCR_FILES, &root);
        ui_render_screen(UI_SCR_FILES, &root);
        s_now += 300u;
        ui_port_task_handler();
        root.file_sel = 0;
        ui_render_screen(UI_SCR_FILES, &root);
        s_now += 300u;
        ui_port_task_handler();
        assert(g_ssa_writes == w1 && !ui_transition_busy());
        assert(memcmp(g_fb, root_shot, sizeof(g_fb)) == 0);
        m.nav_seq = root.nav_seq;

        /* Entering an empty folder (Files -> empty-Files page) also pushes. */
        ui_model_t empty = root;
        empty.files = NULL;
        empty.file_count = 0;
        ui_model_nav(&empty, UI_NAV_FORWARD);
        w1 = g_ssa_writes;
        ui_render_screen(UI_SCR_FILES, &empty);
        assert(ui_transition_busy());
        s_now += 300u;
        ui_port_task_handler();
        assert(!ui_transition_busy() && g_ssa_writes - w1 == 81u);
        ui_model_nav(&root, UI_NAV_BACK);
        ui_render_screen(UI_SCR_FILES, &root);
        s_now += 300u;
        ui_port_task_handler();
        assert(memcmp(g_fb, root_shot, sizeof(g_fb)) == 0);
        m.nav_seq = root.nav_seq;
    }

    /* Measure worst-case two-screen peak heap during HOME <-> REC transition */
    ui_render_screen(UI_SCR_HOME, &m);
    ui_render_screen(UI_SCR_REC, &m);
    sample_heap("home->rec");
    ui_render_screen(UI_SCR_HOME, &m);
    sample_heap("rec->home");

    /* Interrupt an in-flight push with another push (fast-forwards the first),
     * then with an instant screen: the panel must end unscrolled and exact. */
    ui_render_screen(UI_SCR_FILES, &m);
    s_now += 40u;
    ui_port_task_handler();
    assert(ui_transition_busy() && g_ssa != 0u);
    ui_render_screen(UI_SCR_HOME, &m);
    s_now += 300u;
    ui_port_task_handler();
    assert(!ui_transition_busy() && g_ssa == 0u);
    assert(memcmp(g_fb, g_shots[0], sizeof(g_fb)) == 0);
    ui_render_screen(UI_SCR_FILES, &m);
    s_now += 80u;
    ui_port_task_handler();
    ui_render_screen(UI_SCR_POWER, &m);
    assert(!ui_transition_busy() && g_ssa == 0u);
    assert(memcmp(g_fb, g_shots[13], sizeof(g_fb)) == 0);
    s_now += 200u;
    ui_port_task_handler();
    assert(memcmp(g_fb, g_shots[13], sizeof(g_fb)) == 0);

    /* Verify UI_SCR_OFF cleanly renders and reports final LVGL heap stats */
    ui_render_screen(UI_SCR_OFF, &m);
    sample_heap("off");

    /* Verify i18n: all 17 languages have non-empty strings and 100% glyph coverage
     * in ui_font_sm and ui_font_md (zero missing glyphs / tofu boxes). */
    {
        size_t total_strings = 0u;
        size_t total_glyphs  = 0u;
        for (uint8_t lang = 0u; lang < (uint8_t)UI_LANG_COUNT; lang++) {
            const char *lname = ui_lang_native_name(lang);
            assert(lname && lname[0] != '\0');
            const char *check_list[UI_STR_COUNT + 1 + 7 + 12];
            size_t ncheck = 0u;
            check_list[ncheck++] = lname;
            for (uint8_t w = 0u; w < 7u; w++) {
                check_list[ncheck++] = ui_i18n_weekday(lang, w);
            }
            for (uint8_t mo = 1u; mo <= 12u; mo++) {
                check_list[ncheck++] = ui_i18n_month(lang, mo);
            }
            for (unsigned id = 0u; id < (unsigned)UI_STR_COUNT; id++) {
                check_list[ncheck++] = ui_tr_lang(lang, (ui_str_id_t)id);
            }
            for (size_t si = 0u; si < ncheck; si++) {
                const uint8_t *p = (const uint8_t *)check_list[si];
                assert(p && p[0] != '\0');
                total_strings++;
                while (*p) {
                    uint32_t cp = 0u;
                    if (*p < 0x80u) {
                        cp = *p++;
                    } else if ((*p & 0xE0u) == 0xC0u) {
                        cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
                        p += 2;
                    } else if ((*p & 0xF0u) == 0xE0u) {
                        cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
                             (uint32_t)(p[2] & 0x3Fu);
                        p += 3;
                    } else {
                        assert(!"Unexpected 4-byte UTF-8 in i18n table");
                    }
                    lv_font_glyph_dsc_t dsc_sm, dsc_md;
                    if (!lv_font_get_glyph_dsc(&ui_font_sm, &dsc_sm, cp, 0u) ||
                        !lv_font_get_glyph_dsc(&ui_font_md, &dsc_md, cp, 0u)) {
                        fprintf(stderr, "Missing glyph U+%04X in lang=%u (%s) string=\"%s\"\n",
                                (unsigned)cp, (unsigned)lang, lname, check_list[si]);
                        assert(0);
                    }
                    total_glyphs++;
                }
            }
        }
        /* Verify every language's "Language" label + "- <NativeName> +" fits on the 140px
         * settings row without wrapping or colliding. */
        for (uint8_t lang = 0u; lang < (uint8_t)UI_LANG_COUNT; lang++) {
            const char *lbl = ui_tr_lang(lang, UI_STR_SET_LANGUAGE);
            const char *nam = ui_lang_native_name(lang);
            char val_disp[40];
            snprintf(val_disp, sizeof(val_disp), "- %s +", nam);
            int32_t lw = lv_text_get_width(lbl, (uint32_t)strlen(lbl), &ui_font_md, 0);
            int32_t vw = lv_text_get_width(val_disp, (uint32_t)strlen(val_disp), &ui_font_sm, 0);
            if (lw + vw + 4 > 140) {
                fprintf(stderr, "Settings Language row overflow for lang=%u (%s): lw=%d vw=%d\n",
                        (unsigned)lang, nam, (int)lw, (int)vw);
                assert(0);
            }
        }
        /* Render key screens across representative languages (incl. Bulgarian Cyrillic,
         * Polish, Czech, Hungarian, Romanian, German, Italian) to verify zero layout/heap issues */
        static const uint8_t sample_langs[] = {
            UI_LANG_IT, UI_LANG_FR, UI_LANG_DE, UI_LANG_PL,
            UI_LANG_CS, UI_LANG_HU, UI_LANG_BG, UI_LANG_RO, UI_LANG_RU
        };
        for (size_t li = 0u; li < sizeof(sample_langs) / sizeof(sample_langs[0]); li++) {
            m.cfg.lang   = sample_langs[li];
            m.set_sel    = 8;
            m.set_scroll = 6;
            ui_render_screen(UI_SCR_SETTINGS, &m);
            ui_render_screen(UI_SCR_HOME, &m);
            ui_render_screen(UI_SCR_ABOUT, &m);
            ui_render_screen(UI_SCR_POWER, &m);
        }
        m.cfg.lang = UI_LANG_EN;
        ui_i18n_set_lang(UI_LANG_EN);
        ui_render_screen(UI_SCR_OFF, &m);
        printf("i18n check OK: %u languages, %zu strings, %zu glyphs verified in sm/md fonts\n",
               (unsigned)UI_LANG_COUNT, total_strings, total_glyphs);
    }

    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        printf("LVGL heap (host 64-bit): peak=%zu B / total=%zu B (%u%%), end_used=%zu B\n",
               g_peak_heap_used, mon.total_size,
               (unsigned)((g_peak_heap_used * 100u) / mon.total_size),
               mon.total_size - mon.free_size);
        assert(g_peak_heap_used < mon.total_size);
        assert(g_peak_heap_used <= (18u * 1024u));
    }

    /* Write 2-column x 7-row contact sheet at 2x scale (664 x 1184) */
    const int scale = 2;
    const int cols = 2, rows = 7;
    const int pad = 8;
    const int sw = 160 * scale, sh = 80 * scale;
    const int W = pad + cols * (sw + pad);
    const int H = pad + rows * (sh + pad);

    FILE *fp = fopen(out_ppm, "wb");
    if (!fp) {
        perror("fopen");
        return 1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    uint8_t *rowbuf = (uint8_t *)malloc((size_t)W * 3u);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t r = 24, g = 28, b = 36;
            int col = (x - pad) / (sw + pad);
            int row = (y - pad) / (sh + pad);
            int lx = (x - pad) - col * (sw + pad);
            int ly = (y - pad) - row * (sh + pad);
            if (col >= 0 && col < cols && row >= 0 && row < rows &&
                lx >= 0 && lx < sw && ly >= 0 && ly < sh) {
                int idx = row * cols + col;
                if (idx < NUM_SCREENS) {
                    rgb565_to_rgb888(g_shots[idx][ly / scale][lx / scale], &r, &g, &b);
                }
            }
            rowbuf[x * 3 + 0] = r;
            rowbuf[x * 3 + 1] = g;
            rowbuf[x * 3 + 2] = b;
        }
        fwrite(rowbuf, 1u, (size_t)W * 3u, fp);
    }
    free(rowbuf);
    fclose(fp);
    printf("Rendered all %d Cassini UI screens to %s (%dx%d)\n", NUM_SCREENS, out_ppm, W, H);
    return 0;
}
