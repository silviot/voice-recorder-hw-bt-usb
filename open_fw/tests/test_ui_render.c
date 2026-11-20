/*
 * Host MMIO visual renderer & unit test for the Cassini OpenFW 160x80 UI.
 *
 * Emulates the SL6801 8080 parallel LCDC (0x40095000) into a 160x80 RGB565
 * framebuffer, renders all 14 UI screens/sub-states using the exact firmware
 * C code (lcd_st7735s.c, aa_fonts.c, ui_icons.c, ui_spectrogram.c, ui_screens.c),
 * and writes a combined 3x contact sheet PPM for visual verification.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_ui.h"

/* Minimal host MMIO mock for LCDC (0x40095000) + ROM/GPIO stubs */
static uint16_t g_fb[80][160];
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

static void capture_shot(int idx)
{
    memcpy(g_shots[idx], g_fb, sizeof(g_fb));
}

int main(int argc, char **argv)
{
    const char *out_ppm = (argc > 1) ? argv[1] : "/tmp/sl6801_openfw/ui_all_screens.ppm";

    lcd_set_rotation(1);

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

    /* 4: Files list */
    ui_render_screen(UI_SCR_FILES, &m);
    capture_shot(4);

    /* 5: Player (Time-first) */
    m.play_file           = demo_files[0];
    m.play_pos_sec        = 120;
    m.play_state          = 0;
    m.play_vu_l           = 55;
    m.play_vu_r           = 48;
    m.play_has_transcript = 0;
    ui_render_screen(UI_SCR_PLAYER, &m);
    capture_shot(5);

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
    m.play_has_transcript = 0;

    /* 7: Delete? */
    m.del_hold_pct = 55;
    m.del_holding  = 1;
    ui_render_screen(UI_SCR_DELETE, &m);
    capture_shot(7);

    /* 8: Settings */
    m.set_sel = 0;
    m.set_scroll = 0;
    ui_render_screen(UI_SCR_SETTINGS, &m);
    capture_shot(8);

    /* 9: Settings Clock Edit */
    m.set_sel        = 7;
    m.set_scroll     = 6;
    m.set_edit_clock = 1;
    m.set_edit_h     = 14;
    m.set_edit_m     = 32;
    ui_render_screen(UI_SCR_SETTINGS, &m);
    capture_shot(9);
    m.set_edit_clock = 0;

    /* 10: About */
    ui_render_screen(UI_SCR_ABOUT, &m);
    capture_shot(10);

    /* 11: USB Disk */
    m.charging      = 1;
    m.usb_read_act  = 1;
    m.usb_write_act = 1;
    ui_render_screen(UI_SCR_USB, &m);
    capture_shot(11);

    /* 12: Bluetooth Sync (sending) */
    m.charging = 0;
    m.bt_state = 2;
    m.bt_sent  = 3;
    m.bt_total = 12;
    m.bt_pct   = 45;
    ui_render_screen(UI_SCR_BT, &m);
    capture_shot(12);

    /* 13: Power Off */
    m.power_hold_pct = 60;
    ui_render_screen(UI_SCR_POWER, &m);
    capture_shot(13);

    /* Write 2-column x 7-row contact sheet at 2x scale (660 x 1168) */
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
