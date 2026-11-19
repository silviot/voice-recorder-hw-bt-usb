/*
 * Cassini OpenFW — Standalone Open-Source Firmware (Layer 1b Bare-Metal UI & Drivers)
 *
 * Runs 100% standalone from Stage-1 HLKJ Bootloader at 24 MHz HOSC with zero
 * stock FIRM code linked:
 *   - UART0 early boot banner (PD0 before ADC takes over)
 *   - 8080 8-bit parallel ST7735S 80x160 RGB565 LCD + PWM3 backlight
 *   - Cassini splash artwork + live battery/RTC/ADC button/Rec-switch telemetry
 *   - Hold BACK for 2.0s -> clean PMU reboot into USB Bootloader (301a:2800)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "cassini_splash.h"

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

    wdt_feed();
    systick_init(24000000u); /* Stage-1 hands off on 24 MHz HOSC */

    /* Emit early UART0 banner before PD0 is switched to SARADC kadc_ch1 */
    uart0_init_115200();
    uart0_puts("\n=== Cassini OpenFW (SL6801 Bare-Metal Layer 1b) ===\n");
    uart0_puts("CPU_ID=");
    uart0_put_u32(SYSCTL_CPU_ID);
    uart0_puts(" VBAT_MV=");
    uart0_put_u32(pmu_read_vbat_mv());
    uart0_puts("\nSwitching PD0 to SARADC key ladder & initializing ST7735S LCD...\n");

    lcd_init();
    lcd_fill_rect(0u, 0u, LCD_WIDTH, LCD_HEIGHT, col_bg);
    lcd_backlight_set(80u);
    lcd_blit_rgb565(0u, 6u, CASSINI_SPLASH_80X40_WIDTH, CASSINI_SPLASH_80X40_HEIGHT, CASSINI_SPLASH_80X40);

    draw_line_padded(54u, "OpenFW v1b", col_hdr, col_bg);
    draw_line_padded(146u, "HOLD BACK=USB", col_dim, col_bg);

    keys_init();

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

        char line[24];
        int pos;

        /* Line 1 (y=72): Battery voltage + state */
        pos = 0;
        append_u32(line, &pos, sizeof(line), mv);
        append_str(line, &pos, sizeof(line), "mV ");
        append_str(line, &pos, sizeof(line), chg_tag);
        line[pos] = '\0';
        draw_line_padded(72u, line, (chg == PMU_CHG_CHARGING) ? col_yellow : col_green, col_bg);

        /* Line 2 (y=88): Uptime seconds */
        pos = 0;
        append_str(line, &pos, sizeof(line), "UP ");
        append_u32(line, &pos, sizeof(line), sec);
        append_str(line, &pos, sizeof(line), "s");
        line[pos] = '\0';
        draw_line_padded(88u, line, col_text, col_bg);

        /* Line 3 (y=104): Record slide switch PB6 */
        draw_line_padded(104u, rec_sw ? "SW: REC ON" : "SW: SAVE", rec_sw ? col_alert : col_cyan, col_bg);

        /* Line 4 (y=120): Raw SARADC + decoded button */
        pos = 0;
        append_u32(line, &pos, sizeof(line), adc);
        append_str(line, &pos, sizeof(line), " ");
        append_str(line, &pos, sizeof(line), keys_name(key));
        line[pos] = '\0';
        draw_line_padded(120u, line, (key == KEY_NONE) ? col_dim : col_yellow, col_bg);

        if (key == KEY_BACK) {
            back_hold_ticks++;
            if (back_hold_ticks >= 20u) { /* ~2.0 s hold */
                draw_line_padded(146u, "REBOOT->USB", col_alert, col_bg);
                mdelay(150u);
                pmu_reboot_to_usb_bootloader();
            }
        } else {
            back_hold_ticks = 0u;
        }

        mdelay(100u);
    }

    return 0;
}
