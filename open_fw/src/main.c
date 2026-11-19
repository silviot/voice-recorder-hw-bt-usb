/*
 * Cassini OpenFW — Standalone Open-Source Firmware (Layers 1c, 2 & 3)
 *
 * Runs 100% standalone from Stage-1 HLKJ Bootloader via a <= 4 KiB .ram_boot
 * trampoline at 0x00804C00 that boosts CPU/PLL0 to 96 MHz and maps SPI NOR
 * XIP at 0x00C0F430+:
 *   - UART0 early boot banner (96 MHz, XIP address, battery mV, microSD probe)
 *   - 1-bit PD4/PD5/PD6 SDIO block driver -> exFAT mount & /record file count
 *   - 8080 ST7735S 80x160 RGB565 LCD + PWM3 backlight + Cassini splash
 *   - AMIC1 16 kHz 16-bit audio capture via DMAC CH1 (IRQ 61) + live VU bar
 *     controlled by the physical PB6 Record slide switch
 *   - Hold BACK for 2.0s -> clean PMU reboot into USB Bootloader (301a:2800)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "cassini_splash.h"

static sd_card_t   g_sd_card;
static blkdev_t    g_sd_blkdev;
static exfat_vol_t g_exfat_vol;

static void append_str(char *dst, int *pos, int max, const char *src)
{
    while (*src != '\0' && *pos < (max - 1)) {
        dst[(*pos)++] = *src++;
    }
}

static void append_u32(char *dst, int *pos, int max, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0u) {
        if (*pos < (max - 1)) {
            dst[(*pos)++] = '0';
        }
        return;
    }
    while (v > 0u && n < 11) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && *pos < (max - 1)) {
        dst[(*pos)++] = tmp[--n];
    }
}

static void draw_line_padded(uint16_t y, const char *text, uint16_t fg, uint16_t bg)
{
    char row[14];
    int i = 0;
    while (text[i] != '\0' && i < 13) {
        row[i] = text[i];
        i++;
    }
    while (i < 13) {
        row[i++] = ' ';
    }
    row[13] = '\0';
    lcd_draw_str(1u, y, row, fg, bg);
}

static void draw_vu_bar(uint16_t y, uint8_t pct, uint16_t col_track,
                        uint16_t col_low, uint16_t col_mid, uint16_t col_high)
{
    const uint16_t total_w = 78u;
    const uint16_t bar_h   = 10u;
    uint16_t fill_w = (uint16_t)(((uint32_t)pct * total_w) / 100u);
    if (fill_w > total_w) {
        fill_w = total_w;
    }

    uint16_t fill_col = (pct > 85u) ? col_high : (pct >= 60u) ? col_mid : col_low;
    if (fill_w > 0u) {
        lcd_fill_rect(1u, y, fill_w, bar_h, fill_col);
    }
    if (fill_w < total_w) {
        lcd_fill_rect((uint16_t)(1u + fill_w), y, (uint16_t)(total_w - fill_w), bar_h, col_track);
    }
}

static int rec_count_cb(const exfat_entry_t *entry, void *user)
{
    if (!entry->is_dir) {
        uint32_t *cnt = (uint32_t *)user;
        (*cnt)++;
    }
    return 0;
}

static void probe_microsd_status(char *sd_line, int sd_line_cap, uint16_t *sd_col,
                                 uint16_t col_green, uint16_t col_yellow, uint16_t col_alert)
{
    int pos = 0;
    if (sd_init(&g_sd_card) != 0) {
        append_str(sd_line, &pos, sd_line_cap, "SD: NONE");
        sd_line[pos] = '\0';
        *sd_col = col_alert;
        uart0_puts("SD: NONE (sd_init failed)\n");
        return;
    }

    g_sd_blkdev = sd_as_blkdev(&g_sd_card);
    if (exfat_mount(&g_exfat_vol, &g_sd_blkdev) != 0) {
        append_str(sd_line, &pos, sd_line_cap, "SD: RAW");
        sd_line[pos] = '\0';
        *sd_col = col_yellow;
        uart0_puts("SD: RAW (card ready, no exFAT partition)\n");
        return;
    }

    uint32_t file_cnt = 0u;
    exfat_entry_t rec_dir;
    if (exfat_find_path(&g_exfat_vol, "/record", &rec_dir) == 0 && rec_dir.is_dir) {
        (void)exfat_list_dir(&g_exfat_vol, rec_dir.first_cluster, rec_dir.no_fat_chain,
                             rec_count_cb, &file_cnt);
    }

    append_str(sd_line, &pos, sd_line_cap, "SD: OK ");
    append_u32(sd_line, &pos, sd_line_cap, file_cnt);
    append_str(sd_line, &pos, sd_line_cap, "f");
    sd_line[pos] = '\0';
    *sd_col = col_green;

    uart0_puts("SD: exFAT mounted, /record files=");
    uart0_put_u32(file_cnt);
    uart0_puts("\n");
}

int openfw_main(void)
{
    const uint16_t col_bg     = rgb565(0u, 0u, 0u);
    const uint16_t col_hdr    = rgb565(255u, 150u, 80u);
    const uint16_t col_text   = rgb565(235u, 240u, 245u);
    const uint16_t col_green  = rgb565(100u, 255u, 140u);
    const uint16_t col_yellow = rgb565(255u, 220u, 90u);
    const uint16_t col_cyan   = rgb565(90u, 220u, 255u);
    const uint16_t col_dim    = rgb565(140u, 150u, 165u);
    const uint16_t col_alert  = rgb565(255u, 90u, 90u);
    const uint16_t col_track  = rgb565(32u, 36u, 44u);

    wdt_feed();
    systick_init(96000000u); /* Boosted in .ram_boot: PLL0 192 MHz / 2 */

    /* Emit early UART0 banner before keys_init() switches PD0 to SARADC */
    uart0_init_115200();
    uart0_puts("\n=== Cassini OpenFW (SL6801 XIP + SDIO + Audio DMA) ===\n");
    uart0_puts("CPU=96MHz XIP=0x00C0F430 CPU_ID=");
    uart0_put_u32(SYSCTL_CPU_ID);
    uart0_puts(" VBAT_MV=");
    uart0_put_u32(pmu_read_vbat_mv());
    uart0_puts("\n");

    lcd_init();
    lcd_fill_rect(0u, 0u, LCD_WIDTH, LCD_HEIGHT, col_bg);
    lcd_backlight_set(80u);
    lcd_blit_rgb565(0u, 2u, CASSINI_SPLASH_80X40_WIDTH, CASSINI_SPLASH_80X40_HEIGHT, CASSINI_SPLASH_80X40);

    draw_line_padded(45u, "OpenFW 96M", col_hdr, col_bg);
    draw_line_padded(57u, "SD: PROBE...", col_dim, col_bg);
    draw_line_padded(147u, "HOLD BACK=USB", col_dim, col_bg);

    char sd_line[16];
    uint16_t sd_col = col_dim;
    probe_microsd_status(sd_line, (int)sizeof(sd_line), &sd_col, col_green, col_yellow, col_alert);
    draw_line_padded(57u, sd_line, sd_col, col_bg);

    uart0_puts("Switching PD0 to SARADC key ladder & entering telemetry loop...\n");
    keys_init();

    int rec_active = 0;
    uint32_t back_hold_ticks = 0u;

    while (1) {
        wdt_feed();

        uint32_t mv = pmu_read_vbat_mv();
        pmu_chg_state_t chg = pmu_get_charge_state();
        const char *chg_tag = (chg == PMU_CHG_CHARGING) ? "CHG" :
                              (chg == PMU_CHG_FULL)     ? "FUL" : "BAT";

        uint16_t adc = keys_read_adc();
        key_id_t key = keys_decode(adc);
        int rec_sw   = keys_read_rec_switch();
        uint32_t sec = millis() / 1000u;

        if (rec_sw && !rec_active) {
            if (audio_capture_start(24u) == 0) {
                rec_active = 1;
            }
        } else if (!rec_sw && rec_active) {
            audio_capture_stop();
            rec_active = 0;
        }

        uint8_t  peak_pct = rec_active ? audio_get_peak_pct() : 0u;
        uint32_t frames   = audio_get_frames();

        char line[24];
        int pos;

        /* Row 2 (y=69): Battery voltage + state */
        pos = 0;
        append_u32(line, &pos, sizeof(line), mv);
        append_str(line, &pos, sizeof(line), "mV ");
        append_str(line, &pos, sizeof(line), chg_tag);
        line[pos] = '\0';
        draw_line_padded(69u, line, (chg == PMU_CHG_CHARGING) ? col_yellow : col_green, col_bg);

        /* Row 3 (y=81): Uptime seconds */
        pos = 0;
        append_str(line, &pos, sizeof(line), "UP ");
        append_u32(line, &pos, sizeof(line), sec);
        append_str(line, &pos, sizeof(line), "s");
        line[pos] = '\0';
        draw_line_padded(81u, line, col_text, col_bg);

        /* Row 4 (y=93): Record slide switch PB6 */
        draw_line_padded(93u, rec_sw ? "SW: REC ON" : "SW: SAVE", rec_sw ? col_alert : col_cyan, col_bg);

        /* Row 5 (y=105..114): Live AMIC1 VU meter bar */
        draw_vu_bar(105u, peak_pct, col_track, col_green, col_yellow, col_alert);

        /* Row 6 (y=118): Microphone peak % and DMA frame count */
        pos = 0;
        if (rec_active) {
            append_str(line, &pos, sizeof(line), "VU ");
            append_u32(line, &pos, sizeof(line), peak_pct);
            append_str(line, &pos, sizeof(line), "% ");
            append_u32(line, &pos, sizeof(line), frames);
        } else {
            append_str(line, &pos, sizeof(line), "MIC IDLE");
        }
        line[pos] = '\0';
        draw_line_padded(118u, line, rec_active ? col_green : col_dim, col_bg);

        /* Row 7 (y=131): Raw SARADC + decoded button */
        pos = 0;
        append_u32(line, &pos, sizeof(line), adc);
        append_str(line, &pos, sizeof(line), " ");
        append_str(line, &pos, sizeof(line), keys_name(key));
        line[pos] = '\0';
        draw_line_padded(131u, line, (key == KEY_NONE) ? col_dim : col_yellow, col_bg);

        if (key == KEY_BACK) {
            back_hold_ticks++;
            if (back_hold_ticks >= 40u) { /* ~2.0 s hold @ 50 ms poll */
                audio_capture_stop();
                draw_line_padded(147u, "REBOOT->USB", col_alert, col_bg);
                mdelay(150u);
                pmu_reboot_to_usb_bootloader();
            }
        } else {
            back_hold_ticks = 0u;
        }

        mdelay(50u);
    }

    return 0;
}
