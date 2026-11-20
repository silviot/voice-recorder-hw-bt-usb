/*
 * Experiment 2 (Interactive Keypad, PB6, PB11 & KEY_ONOFF Profiler):
 * Runs for `arg` seconds (default 8 s), displays live values on the LCD, and
 * logs per-press raw SARADC min/max/avg + switch/jack/power-button events.
 *
 * SPDX-License-Identifier: MIT
 */
#include "exp_sdk.h"

EXP_ENTRY
uint32_t exp_main(const openfw_exp_api_t *api, uint32_t arg)
{
    uint32_t secs = (arg >= 1u && arg <= 60u) ? arg : 8u;
    uint32_t dur_ms = secs * 1000u;
    uint32_t t0 = api->millis();
    uint32_t transitions = 0u;

    uint16_t bg = (uint16_t)0x0000u;
    uint16_t fg = (uint16_t)0xFFFFu;
    uint16_t cy = (uint16_t)0x07FFu;
    uint16_t yl = (uint16_t)0xFFE0u;

    api->lcd_set_rotation(0);
    api->lcd_fill_rect(0u, 44u, 80u, 116u, bg);
    api->lcd_draw_str(1u, 45u, "EXP KEYS", cy, bg);
    api->lcd_draw_str(1u, 147u, "PRESS BTNS", yl, bg);

    api->log_puts("[exp_keys] window_s=");
    api->log_u32(secs);
    api->log_putc('\n');

    int prev_rec = api->keys_read_rec_switch();
    int prev_hp  = api->keys_read_hp_detect();
    int prev_pwr = api->key_onoff_pressed(0);

    int      in_press  = 0;
    uint32_t p_min     = 4095u;
    uint32_t p_max     = 0u;
    uint32_t p_sum     = 0u;
    uint32_t p_cnt     = 0u;
    key_id_t p_key     = KEY_NONE;
    uint32_t last_ui   = 0u;

    while ((uint32_t)(api->millis() - t0) < dur_ms) {
        api->wdt_feed();
        uint16_t adc = api->keys_read_adc();
        key_id_t k   = api->keys_decode(adc);
        int active   = (k != KEY_NONE || adc < 3200u);

        if (active) {
            if (!in_press) {
                in_press = 1;
                p_min = adc;
                p_max = adc;
                p_sum = adc;
                p_cnt = 1u;
                p_key = k;
                transitions++;
            } else {
                if (adc < p_min) p_min = adc;
                if (adc > p_max) p_max = adc;
                p_sum += adc;
                p_cnt++;
                if (k != KEY_NONE) p_key = k;
            }
        } else if (in_press) {
            api->log_puts("KEY ");
            api->log_puts(exp_key_name(p_key));
            api->log_puts(" min=");
            api->log_u32(p_min);
            api->log_puts(" max=");
            api->log_u32(p_max);
            api->log_puts(" avg=");
            api->log_u32(p_cnt ? (p_sum / p_cnt) : 0u);
            api->log_puts(" n=");
            api->log_u32(p_cnt);
            api->log_putc('\n');
            in_press = 0;
        }

        int rec = api->keys_read_rec_switch();
        if (rec != prev_rec) {
            prev_rec = rec;
            transitions++;
            exp_log_kv_u32(api, "EVENT pb6_rec_sw", (uint32_t)rec);
        }

        int hp = api->keys_read_hp_detect();
        if (hp != prev_hp) {
            prev_hp = hp;
            transitions++;
            exp_log_kv_u32(api, "EVENT pb11_hp_det", (uint32_t)hp);
        }

        int pwr_lp = 0;
        int pwr    = api->key_onoff_pressed(&pwr_lp);
        if (pwr != prev_pwr || pwr_lp) {
            prev_pwr = pwr;
            transitions++;
            api->log_puts("EVENT key_onoff down=");
            api->log_u32((uint32_t)pwr);
            api->log_puts(" long=");
            api->log_u32((uint32_t)pwr_lp);
            api->log_putc('\n');
        }

        uint32_t now = api->millis();
        if ((uint32_t)(now - last_ui) >= 100u) {
            last_ui = now;
            char row[14];
            for (int i = 0; i < 13; i++) row[i] = ' ';
            row[13] = '\0';
            uint32_t v = adc;
            row[0] = (char)('0' + ((v / 1000u) % 10u));
            row[1] = (char)('0' + ((v / 100u) % 10u));
            row[2] = (char)('0' + ((v / 10u) % 10u));
            row[3] = (char)('0' + (v % 10u));
            const char *kn = exp_key_name(k);
            for (int i = 0; kn[i] && i < 6; i++) row[5 + i] = kn[i];
            api->lcd_draw_str(1u, 60u, row, yl, bg);

            row[0] = 'R'; row[1] = ':'; row[2] = rec ? '1' : '0';
            row[3] = ' '; row[4] = 'H'; row[5] = ':'; row[6] = hp ? '1' : '0';
            row[7] = ' '; row[8] = 'P'; row[9] = ':'; row[10] = pwr ? '1' : '0';
            row[11] = ' '; row[12] = ' ';
            api->lcd_draw_str(1u, 76u, row, fg, bg);
        }

        api->mdelay(20u);
    }

    if (in_press) {
        api->log_puts("KEY ");
        api->log_puts(exp_key_name(p_key));
        api->log_puts(" min=");
        api->log_u32(p_min);
        api->log_puts(" max=");
        api->log_u32(p_max);
        api->log_puts(" avg=");
        api->log_u32(p_cnt ? (p_sum / p_cnt) : 0u);
        api->log_puts(" n=");
        api->log_u32(p_cnt);
        api->log_puts(" (held)\n");
    }
    return transitions;
}
