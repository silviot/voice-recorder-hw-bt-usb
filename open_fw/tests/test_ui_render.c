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

/* Minimal host MMIO mock for LCDC (0x40095000) + ROM/GPIO stubs */
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
            if (lx >= 0 && lx < 160 && ly >= 0 && ly < 80) {
                g_written_pixels++;
                g_fb[ly][lx] = rgb565_val;
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

static void capture_shot(int idx)
{
    for (int frame = 0; frame < 14; frame++) {
        s_now += 16u;
        ui_port_task_handler();
    }
    sample_heap("shot");
    memcpy(g_shots[idx], g_fb, sizeof(g_fb));
}

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
        { "REC_0043.wav", { 2026, 11, 20, 5, 14, 32, 0 }, 192,  65, 1, 1, 2 },
        { "REC_0042.wav", { 2026, 11, 20, 5,  9,  5, 0 }, 1475, 0,  0, 1, 2 },
        { "REC_0041.wav", { 2026, 11, 19, 4, 18, 47, 0 }, 38,   0,  1, 0, 1 },
        { "REC_0040.wav", { 2026, 11, 16, 1,  8,  2, 0 }, 245,  0,  0, 0, 2 },
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

    /* Drive the real UI timers at 30 Hz. Only the footer and underline
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
        s_now += 33u;
        ui_port_task_handler();
        int32_t offset = lv_obj_get_scroll_x(footer);
        assert(offset <= last_scroll);
        if (g_written_pixels > start) moving_frames++;
        assert(g_written_pixels - start <= 160u * 27u);
        last_scroll = offset;
    }
    assert(last_scroll == 0 && moving_frames >= 4);
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

    /* 6: Player (Transcript mode) */
    m.play_has_transcript = 1;
    m.play_pos_sec        = 349;
    m.play_file           = demo_files[1];
    m.tr_speaker          = 0;
    strcpy(m.tr_prev,          "Great. And recording");
    strcpy(m.tr_curr_spoken,   "starts the moment ");
    strcpy(m.tr_curr_upcoming, "you");
    strcpy(m.tr_next,          "slide the switch?");
    ui_render_screen(UI_SCR_PLAYER, &m);
    capture_shot(6);

    /* Verify spoken (white) vs upcoming (muted) color distinction on current line */
    {
        int has_spoken_bright = 0, has_upcoming_muted = 0;
        for (int y = 33; y < 49; y++) {
            for (int x = 6; x < 100; x++) {
                if (g_fb[y][x] == UI_COL_TEXT) has_spoken_bright = 1;
            }
            for (int x = 100; x < 158; x++) {
                if (g_fb[y][x] == UI_COL_MUTE) has_upcoming_muted = 1;
            }
        }
        assert(has_spoken_bright && has_upcoming_muted);
    }

    /* Verify LV_LABEL_LONG_DOT truncated label produces 0 redraw pixels when unchanged */
    strcpy(m.tr_prev, "Very long transcript line overflowing 152px!!");
    ui_render_screen(UI_SCR_PLAYER, &m);
    {
        const char *shown = lv_label_get_text(objects.ptr_prev);
        size_t slen = strlen(shown);
        assert(slen >= 3u && strcmp(shown + slen - 3u, "...") == 0);
    }
    before = g_written_pixels;
    ui_render_screen(UI_SCR_PLAYER, &m);
    assert(g_written_pixels == before);
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

    /* Verify visible toast (including long truncated toast) produces 0 redraw pixels when unchanged */
    strcpy(m.toast_msg, "Long warning toast clipped cleanly!!!");
    m.toast_col = UI_COL_AMBER;
    ui_render_screen(UI_SCR_HOME, &m);
    capture_shot(0); /* wait for any transition, then re-test toast stability */
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

    /* Drive the production reveal on a 60 Hz clock in both directions. */
    ui_render_screen(UI_SCR_HOME, &m);
    for (int forward = 1; forward >= 0; forward--) {
        int incoming = forward ? 4 : 0;
        int outgoing = forward ? 0 : 4;
        uint32_t start = g_written_pixels;
        ui_render_screen(forward ? UI_SCR_FILES : UI_SCR_HOME, &m);
        sample_heap(forward ? "home->files" : "files->home");
        unsigned updates = 0;
        for (int frame = 0; frame < 14; frame++) {
            uint32_t prev_px = g_written_pixels;
            s_now += 16u;
            ui_port_task_handler();
            uint32_t written = g_written_pixels - start;
            assert(written <= 160u * 80u && written % 80u == 0);
            int cols = (int)(written / 80u);
            for (int y = 0; y < 80; y++) {
                for (int x = 0; x < 160; x++) {
                    int shown = forward ? x >= 160 - cols : x < cols;
                    assert(g_fb[y][x] == g_shots[shown ? incoming : outgoing][y][x]);
                }
            }
            if (g_written_pixels > prev_px) updates++;
        }
        assert(g_written_pixels - start == 160u * 80u && updates >= 8);
    }

    /* Measure worst-case two-screen peak heap during HOME <-> REC transition */
    ui_render_screen(UI_SCR_HOME, &m);
    ui_render_screen(UI_SCR_REC, &m);
    sample_heap("home->rec");
    ui_render_screen(UI_SCR_HOME, &m);
    sample_heap("rec->home");

    /* Interrupt an in-flight reveal with an instant screen. */
    ui_render_screen(UI_SCR_FILES, &m);
    s_now += 80u;
    ui_port_task_handler();
    ui_render_screen(UI_SCR_POWER, &m);
    assert(memcmp(g_fb, g_shots[13], sizeof(g_fb)) == 0);
    s_now += 200u;
    ui_port_task_handler();
    assert(memcmp(g_fb, g_shots[13], sizeof(g_fb)) == 0);

    /* Verify UI_SCR_OFF cleanly renders and reports final LVGL heap stats */
    ui_render_screen(UI_SCR_OFF, &m);
    sample_heap("off");
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
