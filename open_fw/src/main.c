/*
 * Cassini OpenFW — Standalone Open-Source Firmware (Layer 4)
 *
 * Runs 100% standalone from Stage-1 HLKJ Bootloader via a <= 4 KiB .ram_boot
 * trampoline at 0x00804C00 that boosts CPU/PLL0 to 96 MHz and maps SPI NOR
 * XIP at 0x00C0F430+:
 *   - UART0 early boot banner & microSD diagnostic output
 *   - 1-bit PD4/PD5/PD6 SDIO block driver -> exFAT reader & writer
 *   - PB6 Record slide switch -> 16 kHz / 16-bit mono WAV recording into
 *     /record/REC_NNNN.wav via DMAC CH1 (IRQ 61) 4-slot ring buffer + live VU
 *   - ENTER button (when idle & microSD inserted) -> Clean-room Full-Speed
 *     Mentor MUSB + SCSI BOT USB Mass Storage mode (BACK exits cleanly)
 *   - RIGHT button (when idle) -> Classic BT SPP + +XREC v5 wireless service
 *   - LEFT button (when idle) -> On-device FILES & SETTINGS menu
 *   - Hold BACK for 2.0s -> PMU reboot into BootROM USB Bootloader (301a:2800)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "cassini_splash.h"

#define MENU_MAX_FILES 24

typedef struct {
    uint16_t bg;
    uint16_t hdr;
    uint16_t text;
    uint16_t green;
    uint16_t yellow;
    uint16_t cyan;
    uint16_t dim;
    uint16_t alert;
    uint16_t track;
} ui_theme_t;

typedef struct {
    char     dir_path[8];
    char     name[EXFAT_MAX_NAME];
    uint64_t size;
} menu_file_t;

typedef struct {
    menu_file_t *items;
    int          count;
    int          max_items;
    const char  *dir_path;
} menu_list_ctx_t;

static sd_card_t   g_sd_card;
static blkdev_t    g_sd_blkdev;
static exfat_vol_t g_exfat_vol;
static int         g_sd_mounted = 0;

static uint8_t     g_user_bl_pct      = 80u;
static uint8_t     g_cur_bl_pct       = 80u;
static uint32_t    g_last_activity_ms = 0u;

static void wfi_sleep_ms(uint32_t ms)
{
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < ms) {
        wdt_feed();
        __asm__ volatile ("wfi");
    }
    wdt_feed();
}

static void backlight_apply(uint8_t pct)
{
    if (pct != g_cur_bl_pct) {
        g_cur_bl_pct = pct;
        lcd_backlight_set(pct);
    }
}

static void backlight_wake(uint32_t now_ms)
{
    g_last_activity_ms = now_ms;
    backlight_apply(g_user_bl_pct);
}

static void backlight_update(uint32_t now_ms)
{
    uint32_t idle_ms = (uint32_t)(now_ms - g_last_activity_ms);
    if (idle_ms >= 30000u) {
        backlight_apply(0u);
    } else if (idle_ms >= 15000u) {
        uint8_t dim = (g_user_bl_pct < 15u) ? g_user_bl_pct : 15u;
        backlight_apply(dim);
    } else {
        backlight_apply(g_user_bl_pct);
    }
}

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

static void append_2d(char *dst, int *pos, int max, uint32_t v)
{
    if (*pos + 2 < max) {
        dst[(*pos)++] = (char)('0' + ((v / 10u) % 10u));
        dst[(*pos)++] = (char)('0' + (v % 10u));
    }
}

static void copy_cstr(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src[i] != '\0' && (i + 1) < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
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

static void clear_body_area(uint16_t bg)
{
    lcd_fill_rect(0u, 44u, LCD_WIDTH, (uint16_t)(LCD_HEIGHT - 44u), bg);
}

static void wait_key_release(key_id_t target)
{
    while (keys_decode(keys_read_adc()) == target) {
        wdt_feed();
        mdelay(20u);
    }
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

static void format_sd_status(char *sd_line, int cap, uint16_t *sd_col,
                             const ui_theme_t *theme)
{
    int pos = 0;
    if (!g_sd_card.initialized) {
        append_str(sd_line, &pos, cap, "SD: NONE");
        sd_line[pos] = '\0';
        *sd_col = theme->alert;
        return;
    }
    if (!g_sd_mounted) {
        append_str(sd_line, &pos, cap, "SD: RAW");
        sd_line[pos] = '\0';
        *sd_col = theme->yellow;
        return;
    }

    uint32_t file_cnt = 0u;
    exfat_entry_t rec_dir;
    if (exfat_find_path(&g_exfat_vol, "/record", &rec_dir) == 0 && rec_dir.is_dir) {
        (void)exfat_list_dir(&g_exfat_vol, rec_dir.first_cluster, rec_dir.no_fat_chain,
                             rec_count_cb, &file_cnt);
    }

    append_str(sd_line, &pos, cap, "SD: OK ");
    append_u32(sd_line, &pos, cap, file_cnt);
    append_str(sd_line, &pos, cap, "f");
    sd_line[pos] = '\0';
    *sd_col = theme->green;
}

static void probe_microsd_internal(int log_uart, char *sd_line, int cap, uint16_t *sd_col,
                                   const ui_theme_t *theme)
{
    g_sd_mounted = 0;
    if (sd_init(&g_sd_card) != 0) {
        if (log_uart) {
            uart0_puts("SD: NONE (sd_init failed)\n");
        }
        format_sd_status(sd_line, cap, sd_col, theme);
        return;
    }

    g_sd_blkdev = sd_as_blkdev(&g_sd_card);
    if (exfat_mount(&g_exfat_vol, &g_sd_blkdev) != 0) {
        if (log_uart) {
            uart0_puts("SD: RAW (card ready, no exFAT partition)\n");
        }
        format_sd_status(sd_line, cap, sd_col, theme);
        return;
    }

    g_sd_mounted = 1;
    format_sd_status(sd_line, cap, sd_col, theme);
    if (log_uart) {
        uart0_puts(sd_line);
        uart0_puts("\n");
    }
}

static void format_rtc_line(char *line, int cap, const rtc_datetime_t *dt)
{
    int pos = 0;
    append_str(line, &pos, cap, "CLK ");
    append_2d(line, &pos, cap, dt->hour);
    append_str(line, &pos, cap, ":");
    append_2d(line, &pos, cap, dt->minute);
    append_str(line, &pos, cap, ":");
    append_2d(line, &pos, cap, dt->second);
    line[pos] = '\0';
}

static int menu_collect_cb(const exfat_entry_t *entry, void *user)
{
    menu_list_ctx_t *ctx = (menu_list_ctx_t *)user;
    if (entry->is_dir) {
        return 0;
    }
    if (ctx->count >= ctx->max_items) {
        return 1;
    }
    menu_file_t *dst = &ctx->items[ctx->count++];
    copy_cstr(dst->dir_path, (int)sizeof(dst->dir_path), ctx->dir_path);
    copy_cstr(dst->name, (int)sizeof(dst->name), entry->name);
    dst->size = entry->size;
    return (ctx->count >= ctx->max_items) ? 1 : 0;
}

static int menu_load_files(menu_file_t *items, int max_items)
{
    if (!g_sd_mounted) {
        return 0;
    }
    menu_list_ctx_t ctx;
    ctx.items     = items;
    ctx.count     = 0;
    ctx.max_items = max_items;

    exfat_entry_t rec_dir;
    if (exfat_find_path(&g_exfat_vol, "/record", &rec_dir) == 0 && rec_dir.is_dir) {
        ctx.dir_path = "/record";
        (void)exfat_list_dir(&g_exfat_vol, rec_dir.first_cluster, rec_dir.no_fat_chain,
                             menu_collect_cb, &ctx);
    }
    if (ctx.count < ctx.max_items) {
        ctx.dir_path = "/";
        (void)exfat_list_dir(&g_exfat_vol, g_exfat_vol.root_dir_cluster, 0u,
                             menu_collect_cb, &ctx);
    }
    return ctx.count;
}

static void menu_draw_files(const ui_theme_t *theme, const menu_file_t *files,
                            int count, int sel, int top, int confirm_del)
{
    static const uint16_t row_y[5] = { 57u, 69u, 81u, 93u, 105u };
    char line[24];
    int pos = 0;

    append_str(line, &pos, (int)sizeof(line), "FILES ");
    if (count == 0) {
        append_str(line, &pos, (int)sizeof(line), "(0)");
    } else {
        append_2d(line, &pos, (int)sizeof(line), (uint32_t)(sel + 1));
        append_str(line, &pos, (int)sizeof(line), "/");
        append_2d(line, &pos, (int)sizeof(line), (uint32_t)count);
    }
    line[pos] = '\0';
    draw_line_padded(45u, line, theme->cyan, theme->bg);

    for (int r = 0; r < 5; r++) {
        int idx = top + r;
        if (idx < count) {
            char entry_str[14];
            entry_str[0] = (idx == sel) ? '>' : ' ';
            const char *src = files[idx].name;
            int nlen = 0;
            while (src[nlen] != '\0' && nlen < 13) {
                nlen++;
            }
            int copy_len = (nlen > 12) ? 11 : nlen;
            for (int c = 0; c < copy_len; c++) {
                entry_str[1 + c] = src[c];
            }
            if (nlen > 12) {
                entry_str[12] = '~';
                entry_str[13] = '\0';
            } else {
                entry_str[1 + copy_len] = '\0';
            }
            uint16_t fg = (idx == sel) ? theme->bg : theme->text;
            uint16_t bg = (idx == sel) ? theme->cyan : theme->bg;
            draw_line_padded(row_y[r], entry_str, fg, bg);
        } else {
            draw_line_padded(row_y[r], "", theme->bg, theme->bg);
        }
    }

    if (count == 0) {
        draw_line_padded(118u, g_sd_mounted ? "NO FILES" : "NO EXFAT SD", theme->dim, theme->bg);
        draw_line_padded(131u, "", theme->bg, theme->bg);
    } else {
        draw_line_padded(118u, files[sel].dir_path, theme->dim, theme->bg);
        if (confirm_del) {
            draw_line_padded(131u, "DEL? ENT=Y", theme->alert, theme->bg);
        } else {
            pos = 0;
            uint64_t sz = files[sel].size;
            append_str(line, &pos, (int)sizeof(line), "SZ: ");
            if (sz < 1024u) {
                append_u32(line, &pos, (int)sizeof(line), (uint32_t)sz);
                append_str(line, &pos, (int)sizeof(line), " B");
            } else if (sz < (1024u * 1024u)) {
                append_u32(line, &pos, (int)sizeof(line), (uint32_t)(sz / 1024u));
                append_str(line, &pos, (int)sizeof(line), " KB");
            } else {
                append_u32(line, &pos, (int)sizeof(line), (uint32_t)(sz / (1024u * 1024u)));
                append_str(line, &pos, (int)sizeof(line), " MB");
            }
            line[pos] = '\0';
            draw_line_padded(131u, line, theme->green, theme->bg);
        }
    }

    draw_line_padded(147u, confirm_del ? "ENT=Y  B=NO" : "L=SET  B=EXIT",
                     confirm_del ? theme->alert : theme->yellow, theme->bg);
}

static void menu_sanitize_rtc(rtc_datetime_t *dt)
{
    if (dt->year < 2000u || dt->year > 2099u) {
        dt->year = 2026u;
    }
    if (dt->month < 1u || dt->month > 12u) {
        dt->month = 1u;
    }
    if (dt->day < 1u || dt->day > 31u) {
        dt->day = 1u;
    }
    if (dt->hour > 23u) {
        dt->hour = 0u;
    }
    if (dt->minute > 59u) {
        dt->minute = 0u;
    }
    if (dt->second > 59u) {
        dt->second = 0u;
    }
}

static void menu_draw_settings(const ui_theme_t *theme, int item)
{
    rtc_datetime_t dt;
    rtc_get(&dt);
    menu_sanitize_rtc(&dt);

    draw_line_padded(45u, "SETTINGS", theme->cyan, theme->bg);

    char line[24];
    int pos;

    pos = 0;
    append_str(line, &pos, (int)sizeof(line), "GAIN: ");
    append_u32(line, &pos, (int)sizeof(line), recorder_get_gain());
    append_str(line, &pos, (int)sizeof(line), "/31");
    line[pos] = '\0';
    draw_line_padded(57u, line, (item == 0) ? theme->bg : theme->text,
                     (item == 0) ? theme->yellow : theme->bg);

    pos = 0;
    append_str(line, &pos, (int)sizeof(line), "BL:   ");
    append_u32(line, &pos, (int)sizeof(line), g_user_bl_pct);
    append_str(line, &pos, (int)sizeof(line), "%");
    line[pos] = '\0';
    draw_line_padded(69u, line, (item == 1) ? theme->bg : theme->text,
                     (item == 1) ? theme->yellow : theme->bg);

    pos = 0;
    append_str(line, &pos, (int)sizeof(line), "HOUR: ");
    append_2d(line, &pos, (int)sizeof(line), dt.hour);
    line[pos] = '\0';
    draw_line_padded(81u, line, (item == 2) ? theme->bg : theme->text,
                     (item == 2) ? theme->yellow : theme->bg);

    pos = 0;
    append_str(line, &pos, (int)sizeof(line), "MIN:  ");
    append_2d(line, &pos, (int)sizeof(line), dt.minute);
    line[pos] = '\0';
    draw_line_padded(93u, line, (item == 3) ? theme->bg : theme->text,
                     (item == 3) ? theme->yellow : theme->bg);

    format_rtc_line(line, (int)sizeof(line), &dt);
    draw_line_padded(105u, line, theme->green, theme->bg);
    draw_line_padded(118u, "VOL+/- ADJ", theme->dim, theme->bg);
    draw_line_padded(131u, "ENT=NEXT ITEM", theme->dim, theme->bg);
    draw_line_padded(147u, "L=FIL  B=EXIT", theme->yellow, theme->bg);
}

static void menu_adjust_setting(int item, int delta, uint32_t now_ms)
{
    if (item == 0) {
        int g = (int)recorder_get_gain() + delta;
        if (g < 0) {
            g = 0;
        } else if (g > 31) {
            g = 31;
        }
        recorder_set_gain((uint8_t)g);
    } else if (item == 1) {
        int bl = (int)g_user_bl_pct + delta * 10;
        if (bl < 20) {
            bl = 20;
        } else if (bl > 100) {
            bl = 100;
        }
        g_user_bl_pct = (uint8_t)bl;
        backlight_wake(now_ms);
    } else if (item == 2) {
        rtc_datetime_t dt;
        rtc_get(&dt);
        menu_sanitize_rtc(&dt);
        dt.hour = (uint8_t)((dt.hour + (delta > 0 ? 1u : 23u)) % 24u);
        rtc_set(&dt);
    } else if (item == 3) {
        rtc_datetime_t dt;
        rtc_get(&dt);
        menu_sanitize_rtc(&dt);
        dt.minute = (uint8_t)((dt.minute + (delta > 0 ? 1u : 59u)) % 60u);
        rtc_set(&dt);
    }
}

static void run_menu_mode(const ui_theme_t *theme)
{
    menu_file_t files[MENU_MAX_FILES];
    int tab         = 0; /* 0 = FILES, 1 = SETTINGS */
    int file_count  = menu_load_files(files, MENU_MAX_FILES);
    int file_sel    = 0;
    int file_top    = 0;
    int confirm_del = 0;
    int set_item    = 0;
    int dirty       = 1;
    uint32_t last_clk_draw = 0u;
    key_id_t prev_k = KEY_LEFT;

    clear_body_area(theme->bg);
    backlight_wake(millis());

    while (1) {
        wdt_feed();
        if (keys_read_rec_switch()) {
            backlight_wake(millis());
            break;
        }

        uint32_t now = millis();
        key_id_t k = keys_decode(keys_read_adc());
        if (k != KEY_NONE) {
            backlight_wake(now);
        }
        backlight_update(now);

        if (k != KEY_NONE && k != prev_k) {
            if (k == KEY_LEFT) {
                tab ^= 1;
                confirm_del = 0;
                if (tab == 0) {
                    file_count = menu_load_files(files, MENU_MAX_FILES);
                    if (file_sel >= file_count) {
                        file_sel = (file_count > 0) ? (file_count - 1) : 0;
                    }
                    if (file_top > file_sel) {
                        file_top = file_sel;
                    }
                }
                dirty = 1;
            } else if (tab == 0) {
                if (k == KEY_BACK) {
                    if (confirm_del) {
                        confirm_del = 0;
                        dirty = 1;
                    } else {
                        break;
                    }
                } else if (k == KEY_VOL_UP && file_count > 0) {
                    confirm_del = 0;
                    if (file_sel > 0) {
                        file_sel--;
                        if (file_sel < file_top) {
                            file_top = file_sel;
                        }
                    }
                    dirty = 1;
                } else if (k == KEY_VOL_DOWN && file_count > 0) {
                    confirm_del = 0;
                    if (file_sel + 1 < file_count) {
                        file_sel++;
                        if (file_sel >= file_top + 5) {
                            file_top = file_sel - 4;
                        }
                    }
                    dirty = 1;
                } else if (k == KEY_ENTER && file_count > 0) {
                    if (!confirm_del) {
                        confirm_del = 1;
                    } else {
                        (void)exfat_delete_file(&g_exfat_vol, files[file_sel].dir_path,
                                                files[file_sel].name);
                        confirm_del = 0;
                        file_count = menu_load_files(files, MENU_MAX_FILES);
                        if (file_sel >= file_count) {
                            file_sel = (file_count > 0) ? (file_count - 1) : 0;
                        }
                        if (file_top > file_sel) {
                            file_top = file_sel;
                        }
                        if (file_top > 0 && file_top + 5 > file_count) {
                            file_top = (file_count > 5) ? (file_count - 5) : 0;
                        }
                    }
                    dirty = 1;
                }
            } else {
                if (k == KEY_BACK) {
                    break;
                } else if (k == KEY_ENTER) {
                    set_item = (set_item + 1) & 3;
                    dirty = 1;
                } else if (k == KEY_VOL_UP) {
                    menu_adjust_setting(set_item, 1, now);
                    dirty = 1;
                } else if (k == KEY_VOL_DOWN) {
                    menu_adjust_setting(set_item, -1, now);
                    dirty = 1;
                }
            }
        }
        prev_k = k;

        if (dirty) {
            if (tab == 0) {
                menu_draw_files(theme, files, file_count, file_sel, file_top, confirm_del);
            } else {
                menu_draw_settings(theme, set_item);
                last_clk_draw = now;
            }
            dirty = 0;
        } else if (tab == 1 && (uint32_t)(now - last_clk_draw) >= 500u) {
            rtc_datetime_t dt;
            char line[24];
            rtc_get(&dt);
            format_rtc_line(line, (int)sizeof(line), &dt);
            draw_line_padded(105u, line, theme->green, theme->bg);
            last_clk_draw = now;
        }

        wfi_sleep_ms(25u);
    }

    wait_key_release(KEY_BACK);
}

static int run_usb_msc_mode(const ui_theme_t *theme, int auto_mode)
{
    clear_body_area(theme->bg);
    draw_line_padded(45u, "USB MSC MODE", theme->cyan, theme->bg);
    draw_line_padded(57u, "DISK: EXPOSED", theme->green, theme->bg);
    draw_line_padded(69u, "RX: 0", theme->text, theme->bg);
    draw_line_padded(81u, "TX: 0", theme->text, theme->bg);
    draw_line_padded(147u, "BACK=EXIT USB", theme->yellow, theme->bg);

    usb_msc_set_diag_info((uint8_t)(g_sd_mounted ? 1u : 0u),
                          (uint8_t)(recorder_is_saving() ? 1u : 0u),
                          recorder_get_gain());

    if (usb_msc_start(&g_sd_blkdev) != 0) {
        return 0;
    }

    backlight_wake(millis());

    uint32_t last_ui     = millis();
    uint16_t last_frame  = 0u;
    uint32_t stale_ticks = 0u;
    uint32_t prev_rxp    = 0u;
    uint32_t prev_txp    = 0u;
    int      seen_sof    = 0;
    int      exit_armed  = 0;
    int      manual_exit = 0;

    while (1) {
        wdt_feed();
        usb_msc_poll();

        uint32_t now = millis();
        if ((uint32_t)(now - last_ui) >= 150u) {
            last_ui = now;

            key_id_t k = keys_decode(keys_read_adc());
            if (k != KEY_NONE) {
                backlight_wake(now);
            }
            if (k == KEY_NONE) {
                exit_armed = 1;
            } else if (exit_armed && k == KEY_BACK) {
                manual_exit = 1;
                break;
            }

            if (auto_mode) {
                if (!pmu_is_vbus_present()) {
                    break;
                }
                uint16_t frame = usb_musb_frame();
                if (frame != 0u) {
                    if (seen_sof && frame == last_frame) {
                        stale_ticks++;
                        if (stale_ticks >= 5u) { /* >= 750 ms without SOF */
                            break;
                        }
                    } else {
                        seen_sof    = 1;
                        last_frame  = frame;
                        stale_ticks = 0u;
                    }
                }
            }

            uint32_t rxp = 0u, txp = 0u;
            usb_msc_get_stats(&rxp, &txp);
            if (rxp != prev_rxp || txp != prev_txp) {
                prev_rxp = rxp;
                prev_txp = txp;
                backlight_wake(now);
            }
            backlight_update(now);

            char line[24];
            int pos = 0;
            append_str(line, &pos, (int)sizeof(line), "RX: ");
            append_u32(line, &pos, (int)sizeof(line), rxp);
            line[pos] = '\0';
            draw_line_padded(69u, line, theme->text, theme->bg);

            pos = 0;
            append_str(line, &pos, (int)sizeof(line), "TX: ");
            append_u32(line, &pos, (int)sizeof(line), txp);
            line[pos] = '\0';
            draw_line_padded(81u, line, theme->text, theme->bg);
        }
    }

    usb_msc_stop();
    wait_key_release(KEY_BACK);
    backlight_wake(millis());

    /* Remount exFAT because USB host may have written or deleted files */
    if (g_sd_card.initialized) {
        g_sd_mounted = (exfat_mount(&g_exfat_vol, &g_sd_blkdev) == 0) ? 1 : 0;
    }
    return manual_exit;
}

static void run_bt_xrec_mode(const ui_theme_t *theme)
{
    clear_body_area(theme->bg);
    draw_line_padded(45u, "BT RF CAL...", theme->yellow, theme->bg);
    draw_line_padded(147u, "WAIT...", theme->dim, theme->bg);
    backlight_wake(millis());

    wdt_feed();
    (void)bt_rf_init_calibrated(0x53u, 32000000u);
    (void)bt_xrec_start(&g_exfat_vol, g_sd_mounted);

    draw_line_padded(45u, "BT SPP +XREC", theme->cyan, theme->bg);
    draw_line_padded(147u, "BACK=EXIT BT", theme->yellow, theme->bg);

    uint32_t last_ui     = 0u;
    uint32_t prev_rx     = 0u;
    uint32_t prev_tx     = 0u;
    uint8_t  prev_acl    = 0u;
    uint8_t  prev_spp    = 0u;
    int      exit_armed  = 0;
    int      prev_rec_sw = 0;
    int      low_batt    = 0;
    key_id_t prev_bt_key = KEY_NONE;

    while (1) {
        wdt_feed();
        bt_xrec_poll();
        recorder_poll();

        int rec_sw = keys_read_rec_switch();
        if (rec_sw && !prev_rec_sw) {
            backlight_wake(millis());
            if (pmu_read_vbat_mv() >= 3450u) {
                low_batt = 0;
                (void)recorder_start(&g_exfat_vol, g_sd_mounted);
            } else {
                low_batt = 1;
            }
        } else if (!rec_sw && prev_rec_sw) {
            backlight_wake(millis());
            (void)recorder_stop();
        }
        prev_rec_sw = rec_sw;

        uint32_t now = millis();
        if ((uint32_t)(now - last_ui) >= 150u || last_ui == 0u) {
            last_ui = now;

            uint32_t mv = pmu_read_vbat_mv();
            if (mv < 3450u) {
                low_batt = 1;
                if (recorder_is_saving()) {
                    (void)recorder_stop();
                }
            } else if (mv >= 3550u) {
                low_batt = 0;
            }

            key_id_t k = keys_decode(keys_read_adc());
            if (k != KEY_NONE) {
                backlight_wake(now);
            }
            if (k == KEY_NONE) {
                exit_armed = 1;
            } else if (exit_armed && k == KEY_BACK) {
                break;
            } else if (exit_armed && k == KEY_ENTER && prev_bt_key != KEY_ENTER) {
                bt_xrec_open_pairing(60000u);
            }
            prev_bt_key = k;

            bt_status_t st;
            bt_xrec_get_status(&st);

            if (st.rx_acl_pkts != prev_rx || st.tx_acl_pkts != prev_tx ||
                st.acl_connected != prev_acl || st.spp_open != prev_spp) {
                prev_rx  = st.rx_acl_pkts;
                prev_tx  = st.tx_acl_pkts;
                prev_acl = st.acl_connected;
                prev_spp = st.spp_open;
                backlight_wake(now);
            }
            backlight_update(now);

            draw_line_padded(57u, st.dev_name, theme->text, theme->bg);

            const char *link_str = "HCI INIT";
            uint16_t    link_col = theme->dim;
            if (st.xrec_auth) {
                link_str = "AUTH OK";
                link_col = theme->green;
            } else if (st.spp_open) {
                link_str = "SPP OPEN";
                link_col = theme->cyan;
            } else if (st.acl_connected) {
                link_str = "ACL CONN";
                link_col = theme->yellow;
            } else if (st.hci_ready) {
                link_str = "WAIT CONN";
                link_col = theme->dim;
            }
            draw_line_padded(69u, link_str, link_col, theme->bg);

            char line[24];
            int pos = 0;
            rtc_datetime_t dt;
            rtc_get(&dt);
            format_rtc_line(line, (int)sizeof(line), &dt);
            draw_line_padded(81u, line, theme->text, theme->bg);

            if (st.pair_active) {
                pos = 0;
                append_str(line, &pos, (int)sizeof(line), "PIN: ");
                append_str(line, &pos, (int)sizeof(line), st.pair_pin);
                line[pos] = '\0';
                draw_line_padded(93u, line, theme->yellow, theme->bg);
            } else {
                draw_line_padded(93u, "ENT=PAIR 60s", theme->yellow, theme->bg);
            }

            if (low_batt) {
                draw_line_padded(105u, "LOW BATT!", theme->alert, theme->bg);
            } else if (recorder_is_saving() || rec_sw) {
                pos = 0;
                uint32_t rsec = recorder_get_seconds();
                append_str(line, &pos, (int)sizeof(line), "REC ");
                append_2d(line, &pos, (int)sizeof(line), rsec / 60u);
                append_str(line, &pos, (int)sizeof(line), ":");
                append_2d(line, &pos, (int)sizeof(line), rsec % 60u);
                line[pos] = '\0';
                draw_line_padded(105u, line, theme->alert, theme->bg);
            } else {
                draw_line_padded(105u, "", theme->bg, theme->bg);
            }

            pos = 0;
            append_str(line, &pos, (int)sizeof(line), "RX: ");
            append_u32(line, &pos, (int)sizeof(line), st.rx_acl_pkts);
            line[pos] = '\0';
            draw_line_padded(118u, line, theme->text, theme->bg);

            pos = 0;
            append_str(line, &pos, (int)sizeof(line), "TX: ");
            append_u32(line, &pos, (int)sizeof(line), st.tx_acl_pkts);
            line[pos] = '\0';
            draw_line_padded(131u, line, theme->text, theme->bg);
        }
    }

    if (!keys_read_rec_switch()) {
        (void)recorder_stop();
    }
    bt_xrec_stop();
    wait_key_release(KEY_BACK);
    backlight_wake(millis());
}

int openfw_main(void)
{
    const ui_theme_t theme = {
        rgb565(0u, 0u, 0u),       /* bg */
        rgb565(255u, 150u, 80u),  /* hdr */
        rgb565(235u, 240u, 245u), /* text */
        rgb565(100u, 255u, 140u), /* green */
        rgb565(255u, 220u, 90u),  /* yellow */
        rgb565(90u, 220u, 255u),  /* cyan */
        rgb565(140u, 150u, 165u), /* dim */
        rgb565(255u, 90u, 90u),   /* alert */
        rgb565(32u, 36u, 44u),    /* track */
    };

    wdt_feed();
    systick_init(96000000u); /* Boosted in .ram_boot: PLL0 192 MHz / 2 */
    rtc_ensure_running();

    /* Emit early UART0 banner before keys_init() switches PD0 to SARADC */
    uart0_init_115200();
    uart0_puts("\n=== Cassini OpenFW (SL6801 Layer 5d: RTC + exFAT + WAV + USB MSC + Classic BT +XREC) ===\n");
    uart0_puts("CPU=96MHz XIP=0x00C0F430 CPU_ID=");
    uart0_put_u32(SYSCTL_CPU_ID);
    uart0_puts(" VBAT_MV=");
    uart0_put_u32(pmu_read_vbat_mv());
    uart0_puts("\n");

    lcd_init();
    lcd_fill_rect(0u, 0u, LCD_WIDTH, LCD_HEIGHT, theme.bg);
    g_cur_bl_pct = 255u;
    backlight_wake(millis());
    lcd_blit_rgb565(0u, 2u, CASSINI_SPLASH_80X40_WIDTH, CASSINI_SPLASH_80X40_HEIGHT, CASSINI_SPLASH_80X40);

    draw_line_padded(45u, "OpenFW 96M", theme.hdr, theme.bg);
    draw_line_padded(57u, "SD: PROBE...", theme.dim, theme.bg);
    draw_line_padded(147u, "HOLD BACK=USB", theme.dim, theme.bg);

    char sd_line[16];
    uint16_t sd_col = theme.dim;
    probe_microsd_internal(1, sd_line, (int)sizeof(sd_line), &sd_col, &theme);
    draw_line_padded(57u, sd_line, sd_col, theme.bg);

    uart0_puts("Switching PD0 to SARADC key ladder & entering main loop...\n");
    keys_init();
    backlight_wake(millis());

    int      rec_active              = 0;
    int      prev_rec_sw             = 0;
    int      prev_vbus               = 0;
    int      prev_vbus_bl            = pmu_is_vbus_present();
    int      usb_probing             = 0;
    int      usb_charger_only        = 0;
    int      usb_suppress_until_unpl = 0;
    int      low_batt_cutoff         = 0;
    uint32_t usb_probe_deadline      = 0u;
    uint32_t last_sd_probe           = millis();
    key_id_t prev_key                = KEY_NONE;
    uint32_t back_hold_ticks         = 0u;
    uint32_t last_slow_ui            = 0u;
    uint32_t mv                      = pmu_read_vbat_mv();
    pmu_chg_state_t chg              = pmu_get_charge_state();
    if (mv < 3450u) {
        low_batt_cutoff = 1;
    }

    while (1) {
        wdt_feed();

        uint32_t now_ms = millis();
        uint16_t adc    = keys_read_adc();
        key_id_t key    = keys_decode(adc);
        int      rec_sw = keys_read_rec_switch();

        if (key != KEY_NONE || rec_sw != prev_rec_sw) {
            backlight_wake(now_ms);
        }
        prev_rec_sw = rec_sw;

        if (rec_sw && !rec_active && !low_batt_cutoff) {
            if (recorder_start(&g_exfat_vol, g_sd_mounted) == 0) {
                rec_active = 1;
            }
        } else if (!rec_sw && rec_active) {
            (void)recorder_stop();
            rec_active = 0;
            format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
            draw_line_padded(57u, sd_line, sd_col, theme.bg);
        }

        if (rec_active) {
            recorder_poll();
            prev_vbus   = pmu_is_vbus_present();
            usb_probing = 0;
        } else {
            int vbus_now       = pmu_is_vbus_present();
            int enter_usb_auto = 0;

            if (vbus_now != prev_vbus_bl) {
                prev_vbus_bl = vbus_now;
                backlight_wake(now_ms);
            }

            /* Periodic silent SD hot-plug re-probe every 2 s (never call uart0_* after keys_init!) */
            if (!g_sd_card.initialized && (uint32_t)(now_ms - last_sd_probe) >= 2000u) {
                last_sd_probe = now_ms;
                probe_microsd_internal(0, sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                if (g_sd_card.initialized) {
                    draw_line_padded(57u, sd_line, sd_col, theme.bg);
                    last_slow_ui = 0u;
                }
            }

            if (!vbus_now) {
                prev_vbus               = 0;
                usb_probing             = 0;
                usb_charger_only        = 0;
                usb_suppress_until_unpl = 0;
            } else if (g_sd_card.initialized && !usb_suppress_until_unpl) {
                if (!prev_vbus) {
                    prev_vbus          = 1;
                    usb_charger_only   = 0;
                    usb_probing        = 1;
                    usb_probe_deadline = now_ms + 1500u;
                    usb_phy_det_arm();
                } else if (usb_probing) {
                    if (usb_phy_det_poll_host()) {
                        usb_probing    = 0;
                        enter_usb_auto = 1;
                    } else if ((int32_t)(now_ms - usb_probe_deadline) >= 0) {
                        usb_probing      = 0;
                        usb_charger_only = 1;
                    }
                }
            }

            if (enter_usb_auto || (key == KEY_ENTER && prev_key != KEY_ENTER && g_sd_card.initialized)) {
                int manual = run_usb_msc_mode(&theme, enter_usb_auto);
                if (manual || enter_usb_auto) {
                    usb_suppress_until_unpl = pmu_is_vbus_present();
                }
                clear_body_area(theme.bg);
                draw_line_padded(45u, "OpenFW 96M", theme.hdr, theme.bg);
                format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                draw_line_padded(57u, sd_line, sd_col, theme.bg);
                draw_line_padded(147u, "HOLD BACK=USB", theme.dim, theme.bg);
                prev_key        = KEY_NONE;
                back_hold_ticks = 0u;
                last_slow_ui    = 0u;
                continue;
            }
            if (key == KEY_RIGHT && prev_key != KEY_RIGHT) {
                run_bt_xrec_mode(&theme);
                clear_body_area(theme.bg);
                draw_line_padded(45u, "OpenFW 96M", theme.hdr, theme.bg);
                format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                draw_line_padded(57u, sd_line, sd_col, theme.bg);
                draw_line_padded(147u, "HOLD BACK=USB", theme.dim, theme.bg);
                prev_key        = KEY_NONE;
                back_hold_ticks = 0u;
                last_slow_ui    = 0u;
                continue;
            }
            if (key == KEY_LEFT && prev_key != KEY_LEFT) {
                run_menu_mode(&theme);
                clear_body_area(theme.bg);
                draw_line_padded(45u, "OpenFW 96M", theme.hdr, theme.bg);
                format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                draw_line_padded(57u, sd_line, sd_col, theme.bg);
                draw_line_padded(147u, "HOLD BACK=USB", theme.dim, theme.bg);
                prev_key        = KEY_NONE;
                back_hold_ticks = 0u;
                last_slow_ui    = 0u;
                continue;
            }
        }
        prev_key = key;

        backlight_update(now_ms);

        uint8_t peak_pct = rec_active ? audio_get_peak_pct() : 0u;
        char line[24];
        int pos;

        /* Live VU meter updates smoothly every 25 ms tick */
        draw_vu_bar(105u, peak_pct, theme.track, theme.green, theme.yellow, theme.alert);

        /* Throttle text/RTWI updates to 10 Hz so SDIO audio writes never stall */
        if ((uint32_t)(now_ms - last_slow_ui) >= 100u || last_slow_ui == 0u) {
            last_slow_ui = now_ms;
            mv  = pmu_read_vbat_mv();
            chg = pmu_get_charge_state();

            if (mv < 3450u) {
                low_batt_cutoff = 1;
                if (rec_active) {
                    (void)recorder_stop();
                    rec_active = 0;
                    format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                    draw_line_padded(57u, sd_line, sd_col, theme.bg);
                }
            } else if (mv >= 3550u) {
                low_batt_cutoff = 0;
            }

            const char *chg_tag = (chg == PMU_CHG_CHARGING) ? "CHG" :
                                  (chg == PMU_CHG_FULL)     ? "FUL" : "BAT";

            /* Row 2 (y=69): Active WAV filename when saving, else Battery mV */
            if (rec_active && recorder_is_saving()) {
                draw_line_padded(69u, recorder_get_filename(), theme.alert, theme.bg);
            } else {
                pos = 0;
                append_u32(line, &pos, (int)sizeof(line), mv);
                append_str(line, &pos, (int)sizeof(line), "mV ");
                append_str(line, &pos, (int)sizeof(line), chg_tag);
                line[pos] = '\0';
                uint16_t bat_col = (mv < 3550u)              ? theme.alert :
                                   (chg == PMU_CHG_CHARGING) ? theme.yellow : theme.green;
                draw_line_padded(69u, line, bat_col, theme.bg);
            }

            /* Row 3 (y=81): Recording MM:SS when recording, else live AON RTC clock */
            pos = 0;
            if (rec_active) {
                uint32_t rsec = recorder_get_seconds();
                append_str(line, &pos, (int)sizeof(line), "REC ");
                append_2d(line, &pos, (int)sizeof(line), rsec / 60u);
                append_str(line, &pos, (int)sizeof(line), ":");
                append_2d(line, &pos, (int)sizeof(line), rsec % 60u);
                line[pos] = '\0';
                draw_line_padded(81u, line, theme.alert, theme.bg);
            } else {
                rtc_datetime_t dt;
                rtc_get(&dt);
                format_rtc_line(line, (int)sizeof(line), &dt);
                draw_line_padded(81u, line, theme.text, theme.bg);
            }

            /* Row 4 (y=93): Low battery, record switch, charger state, or menu/USB/BT prompt */
            if (low_batt_cutoff || mv < 3450u) {
                draw_line_padded(93u, "LOW BATT!", theme.alert, theme.bg);
            } else if (rec_sw) {
                draw_line_padded(93u, recorder_is_saving() ? "SAVING WAV" : "SW: REC ON",
                                 theme.alert, theme.bg);
            } else if (usb_charger_only) {
                draw_line_padded(93u, "USB: CHG", theme.yellow, theme.bg);
            } else {
                draw_line_padded(93u, g_sd_card.initialized ? "L=MNU E=U R=B" : "L=MNU R=BT",
                                 theme.cyan, theme.bg);
            }

            /* Row 6 (y=118): Microphone peak % and ring overruns */
            pos = 0;
            if (rec_active) {
                append_str(line, &pos, (int)sizeof(line), "VU ");
                append_u32(line, &pos, (int)sizeof(line), peak_pct);
                append_str(line, &pos, (int)sizeof(line), "% O:");
                append_u32(line, &pos, (int)sizeof(line), audio_get_overruns());
            } else {
                append_str(line, &pos, (int)sizeof(line), "MIC IDLE");
            }
            line[pos] = '\0';
            draw_line_padded(118u, line, rec_active ? theme.green : theme.dim, theme.bg);

            /* Row 7 (y=131): Raw SARADC + decoded button */
            pos = 0;
            append_u32(line, &pos, (int)sizeof(line), adc);
            append_str(line, &pos, (int)sizeof(line), " ");
            append_str(line, &pos, (int)sizeof(line), keys_name(key));
            line[pos] = '\0';
            draw_line_padded(131u, line, (key == KEY_NONE) ? theme.dim : theme.yellow, theme.bg);
        }

        if (key == KEY_BACK) {
            back_hold_ticks++;
            if (back_hold_ticks >= 80u) { /* ~2.0 s hold @ 25 ms loop */
                (void)recorder_stop();
                draw_line_padded(147u, "REBOOT->USB", theme.alert, theme.bg);
                mdelay(150u);
                pmu_reboot_to_usb_bootloader();
            }
        } else {
            back_hold_ticks = 0u;
        }

        wfi_sleep_ms(25u);
    }

    return 0;
}
