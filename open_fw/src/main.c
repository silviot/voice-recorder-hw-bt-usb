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
#include "openfw_exp.h"
#include "openfw_player.h"
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
    char     dir_path[12];
    char     name[EXFAT_MAX_NAME];
    uint64_t size;
    uint8_t  has_cwx;
} menu_file_t;

typedef struct {
    menu_file_t *items;
    int          count;
    int          max_items;
    const char  *dir_path;
} menu_list_ctx_t;

static sd_card_t   g_sd_card;
static blkdev_t    g_sd_blkdev;
static blkdev_t    g_null_blkdev = { 0, 0, 0u, 0 };
static exfat_vol_t g_exfat_vol;
static int         g_sd_mounted = 0;
static int         g_boot_strikes_cleared = 0;

#if OPENFW_UNATTENDED_DEV
static uint8_t     g_user_bl_pct      = 40u;
#else
static uint8_t     g_user_bl_pct      = 80u;
#endif
static uint8_t     g_cur_bl_pct       = 80u;
static uint32_t    g_last_activity_ms = 0u;

static void clear_boot_strikes_once(void)
{
    if (!g_boot_strikes_cleared) {
        AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC;
        g_boot_strikes_cleared = 1;
    }
}

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
#if OPENFW_UNATTENDED_DEV
    (void)now_ms;
    backlight_apply(g_user_bl_pct);
#else
    uint32_t idle_ms = (uint32_t)(now_ms - g_last_activity_ms);
    if (idle_ms >= 30000u) {
        backlight_apply(0u);
    } else if (idle_ms >= 15000u) {
        uint8_t dim = (g_user_bl_pct < 15u) ? g_user_bl_pct : 15u;
        backlight_apply(dim);
    } else {
        backlight_apply(g_user_bl_pct);
    }
#endif
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
    char row[27];
    int i = 0;
    while (text[i] != '\0' && i < 26) {
        row[i] = text[i];
        i++;
    }
    while (i < 26) {
        row[i++] = ' ';
    }
    row[26] = '\0';
    lcd_draw_str(2u, y, row, fg, bg);
}

static void draw_line_2x_centered(uint16_t y, const char *text, uint16_t fg, uint16_t bg)
{
    int len = 0;
    while (text[len] != '\0' && len < 13) {
        len++;
    }
    uint16_t px_w = (uint16_t)(len * 12);
    uint16_t x0   = (px_w < 160u) ? (uint16_t)((160u - px_w) / 2u) : 0u;
    lcd_fill_rect(0u, y, 160u, 18u, bg);
    lcd_draw_str_2x(x0, (uint16_t)(y + 1u), text, fg, bg);
}

static void clear_body_area(uint16_t bg)
{
    lcd_fill_rect(0u, 12u, 160u, 68u, bg);
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
    const uint16_t total_w = 152u;
    const uint16_t bar_h   = 12u;
    uint16_t fill_w = (uint16_t)(((uint32_t)pct * total_w) / 100u);
    if (fill_w > total_w) {
        fill_w = total_w;
    }

    uint16_t fill_col = (pct > 85u) ? col_high : (pct >= 60u) ? col_mid : col_low;
    if (fill_w > 0u) {
        lcd_fill_rect(4u, y, fill_w, bar_h, fill_col);
    }
    if (fill_w < total_w) {
        lcd_fill_rect((uint16_t)(4u + fill_w), y, (uint16_t)(total_w - fill_w), bar_h, col_track);
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

static int name_ends_with_ci(const char *s, const char *sfx)
{
    int sl = 0, xl = 0;
    while (s && s[sl] != '\0') sl++;
    while (sfx && sfx[xl] != '\0') xl++;
    if (xl == 0 || sl < xl) {
        return 0;
    }
    for (int i = 0; i < xl; i++) {
        char a = s[sl - xl + i];
        char b = sfx[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int menu_collect_cb(const exfat_entry_t *entry, void *user)
{
    menu_list_ctx_t *ctx = (menu_list_ctx_t *)user;
    if (entry->is_dir) {
        return 0;
    }
    if (name_ends_with_ci(entry->name, ".cwx")) {
        return 0; /* Hide .cwx sidecars from file list */
    }
    if (ctx->count >= ctx->max_items) {
        return 1;
    }
    menu_file_t *dst = &ctx->items[ctx->count++];
    copy_cstr(dst->dir_path, (int)sizeof(dst->dir_path), ctx->dir_path);
    copy_cstr(dst->name, (int)sizeof(dst->name), entry->name);
    dst->size    = entry->size;
    dst->has_cwx = 0u;
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

    exfat_entry_t dir_ent;
    if (exfat_find_path(&g_exfat_vol, "/cassini", &dir_ent) == 0 && dir_ent.is_dir) {
        ctx.dir_path = "/cassini";
        (void)exfat_list_dir(&g_exfat_vol, dir_ent.first_cluster, dir_ent.no_fat_chain,
                             menu_collect_cb, &ctx);
    }
    if (ctx.count < ctx.max_items &&
        exfat_find_path(&g_exfat_vol, "/record", &dir_ent) == 0 && dir_ent.is_dir) {
        ctx.dir_path = "/record";
        (void)exfat_list_dir(&g_exfat_vol, dir_ent.first_cluster, dir_ent.no_fat_chain,
                             menu_collect_cb, &ctx);
    }
    if (ctx.count < ctx.max_items) {
        ctx.dir_path = "/";
        (void)exfat_list_dir(&g_exfat_vol, g_exfat_vol.root_dir_cluster, 0u,
                             menu_collect_cb, &ctx);
    }

    for (int i = 0; i < ctx.count; i++) {
        if (!name_ends_with_ci(items[i].name, ".wav")) {
            continue;
        }
        char cpath[96];
        int p = 0;
        if (!(items[i].dir_path[0] == '/' && items[i].dir_path[1] == '\0')) {
            append_str(cpath, &p, (int)sizeof(cpath), items[i].dir_path);
        }
        append_str(cpath, &p, (int)sizeof(cpath), "/");
        append_str(cpath, &p, (int)sizeof(cpath), items[i].name);
        cpath[p] = '\0';
        if (p >= 4) {
            cpath[p - 3] = 'c';
            cpath[p - 2] = 'w';
            cpath[p - 1] = 'x';
            exfat_entry_t cent;
            if (exfat_find_path(&g_exfat_vol, cpath, &cent) == 0 && !cent.is_dir) {
                items[i].has_cwx = 1u;
            }
        }
    }
    return ctx.count;
}

static void menu_draw_files(const ui_theme_t *theme, const menu_file_t *files,
                            int count, int sel, int top, int confirm_del)
{
    static const uint16_t row_y[3] = { 16u, 33u, 50u };
    char line[32];
    int pos = 0;

    append_str(line, &pos, (int)sizeof(line), "FILES ");
    if (count == 0) {
        append_str(line, &pos, (int)sizeof(line), "(0)");
    } else {
        append_u32(line, &pos, (int)sizeof(line), (uint32_t)(sel + 1));
        append_str(line, &pos, (int)sizeof(line), "/");
        append_u32(line, &pos, (int)sizeof(line), (uint32_t)count);
        append_str(line, &pos, (int)sizeof(line), "  ");
        uint64_t sz = files[sel].size;
        if (sz < 1024u) {
            append_u32(line, &pos, (int)sizeof(line), (uint32_t)sz);
            append_str(line, &pos, (int)sizeof(line), "B");
        } else {
            append_u32(line, &pos, (int)sizeof(line), (uint32_t)(sz / 1024u));
            append_str(line, &pos, (int)sizeof(line), "KB");
        }
        if (files[sel].has_cwx) {
            append_str(line, &pos, (int)sizeof(line), " [C]");
        }
    }
    line[pos] = '\0';
    lcd_fill_rect(0u, 0u, 160u, 12u, theme->track);
    draw_line_padded(2u, line, theme->cyan, theme->track);

    if (count == 0) {
        clear_body_area(theme->bg);
        draw_line_2x_centered(26u, g_sd_mounted ? "NO FILES" : "NO SD CARD", theme->dim, theme->bg);
    } else {
        for (int r = 0; r < 3; r++) {
            int idx = top + r;
            uint16_t ry = row_y[r];
            if (idx < count) {
                char entry_str[27];
                entry_str[0] = (idx == sel) ? '>' : ' ';
                entry_str[1] = ' ';
                const char *src = files[idx].name;
                int c = 0;
                while (src[c] != '\0' && c < 23) {
                    entry_str[2 + c] = src[c];
                    c++;
                }
                entry_str[2 + c] = '\0';
                uint16_t fg = (idx == sel) ? theme->bg : theme->text;
                uint16_t bg = (idx == sel) ? theme->yellow : theme->bg;
                lcd_fill_rect(0u, ry, 160u, 15u, bg);
                draw_line_padded((uint16_t)(ry + 3u), entry_str, fg, bg);
            } else {
                lcd_fill_rect(0u, ry, 160u, 15u, theme->bg);
            }
        }
    }

    (void)confirm_del;
    draw_line_padded(69u, "</>/VOL:Sel OK:Play B:Back", theme->cyan, theme->bg);
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

static const char *dsp_short_tag(audio_dsp_profile_t p)
{
    if (p == AUDIO_DSP_VOICE_CLARITY) {
        return "CLR";
    }
    if (p == AUDIO_DSP_BYPASS) {
        return "BYP";
    }
    return "RAW";
}

static int main_usb_audio_cfg_cb(int set, uint8_t cfg[4], int16_t *rms_x10, uint16_t *clips)
{
    if (set) {
        if (cfg[0] != 0xFFu) recorder_set_dsp_profile((audio_dsp_profile_t)cfg[0]);
        if (cfg[1] != 0xFFu) recorder_set_gain(cfg[1]);
        if (cfg[2] != 0xFFu) recorder_set_channels(cfg[2]);
        if (cfg[3] != 0xFFu) recorder_set_vor((int)cfg[3]);
    }
    cfg[0] = (uint8_t)recorder_get_dsp_profile();
    cfg[1] = recorder_get_gain();
    cfg[2] = recorder_get_channels();
    cfg[3] = (uint8_t)(recorder_get_vor() ? 1u : 0u);
    if (rms_x10) *rms_x10 = recorder_get_rms_dbfs_x10();
    if (clips)   *clips   = (uint16_t)recorder_get_clip_events();
    return 0;
}

static void menu_draw_settings(const ui_theme_t *theme, int item)
{
    rtc_datetime_t dt;
    rtc_get(&dt);
    menu_sanitize_rtc(&dt);

    char hdr[32];
    int hpos = 0;
    append_str(hdr, &hpos, (int)sizeof(hdr), "SETTINGS ");
    append_u32(hdr, &hpos, (int)sizeof(hdr), (uint32_t)(item + 1));
    append_str(hdr, &hpos, (int)sizeof(hdr), "/7     B:BACK");
    hdr[hpos] = '\0';
    lcd_fill_rect(0u, 0u, 160u, 12u, theme->track);
    draw_line_padded(2u, hdr, theme->cyan, theme->track);

    char big[16];
    int pos = 0;
    const char *hint = "VOL+/- to adjust value";
    if (item == 0) {
        append_str(big, &pos, (int)sizeof(big), "GAIN ");
        append_u32(big, &pos, (int)sizeof(big), recorder_get_gain());
        append_str(big, &pos, (int)sizeof(big), "/31");
        hint = "Mic preamp gain (20=rec)";
    } else if (item == 1) {
        append_str(big, &pos, (int)sizeof(big), "DSP ");
        append_str(big, &pos, (int)sizeof(big), dsp_short_tag(recorder_get_dsp_profile()));
        hint = "RAW / CLR / BYP";
    } else if (item == 2) {
        append_str(big, &pos, (int)sizeof(big), "MIC ");
        append_u32(big, &pos, (int)sizeof(big), recorder_get_channels());
        append_str(big, &pos, (int)sizeof(big), "CH");
        hint = "1CH Mono / 2CH Stereo";
    } else if (item == 3) {
        append_str(big, &pos, (int)sizeof(big), "VOR ");
        append_str(big, &pos, (int)sizeof(big), recorder_get_vor() ? "ON" : "OFF");
        hint = "Voice-activated recording";
    } else if (item == 4) {
        append_str(big, &pos, (int)sizeof(big), "BL ");
        append_u32(big, &pos, (int)sizeof(big), g_user_bl_pct);
        append_str(big, &pos, (int)sizeof(big), "%");
        hint = "Screen brightness";
    } else if (item == 5) {
        append_str(big, &pos, (int)sizeof(big), "HOUR ");
        append_2d(big, &pos, (int)sizeof(big), dt.hour);
        hint = "Clock hour (00..23)";
    } else {
        append_str(big, &pos, (int)sizeof(big), "MIN ");
        append_2d(big, &pos, (int)sizeof(big), dt.minute);
        hint = "Clock minute (00..59)";
    }
    big[pos] = '\0';

    draw_line_2x_centered(20u, big, theme->bg, theme->yellow);
    draw_line_padded(46u, hint, theme->text, theme->bg);
    draw_line_padded(69u, "</>/OK:Next VOL+/-:Adj", theme->cyan, theme->bg);
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
        int p = (int)recorder_get_dsp_profile() + (delta > 0 ? 1 : 2);
        recorder_set_dsp_profile((audio_dsp_profile_t)(p % 3));
    } else if (item == 2) {
        recorder_set_channels((recorder_get_channels() == 1u) ? 2u : 1u);
    } else if (item == 3) {
        recorder_set_vor(!recorder_get_vor());
    } else if (item == 4) {
        int bl = (int)g_user_bl_pct + delta * 10;
        if (bl < 20) {
            bl = 20;
        } else if (bl > 100) {
            bl = 100;
        }
        g_user_bl_pct = (uint8_t)bl;
        backlight_wake(now_ms);
    } else if (item == 5) {
        rtc_datetime_t dt;
        rtc_get(&dt);
        menu_sanitize_rtc(&dt);
        dt.hour = (uint8_t)((dt.hour + (delta > 0 ? 1u : 23u)) % 24u);
        rtc_set(&dt);
    } else if (item == 6) {
        rtc_datetime_t dt;
        rtc_get(&dt);
        menu_sanitize_rtc(&dt);
        dt.minute = (uint8_t)((dt.minute + (delta > 0 ? 1u : 59u)) % 60u);
        rtc_set(&dt);
    }
}

static void run_menu_mode(const ui_theme_t *theme, int initial_tab)
{
    menu_file_t files[MENU_MAX_FILES];
    int tab         = initial_tab ? 1 : 0; /* 0 = FILES, 1 = SETTINGS */
    int file_count  = menu_load_files(files, MENU_MAX_FILES);
    /* Reverse so newest recordings appear at the top */
    for (int i = 0; i < file_count / 2; i++) {
        menu_file_t tmp = files[i];
        files[i] = files[file_count - 1 - i];
        files[file_count - 1 - i] = tmp;
    }
    int file_sel    = 0;
    int file_top    = 0;
    int confirm_del = 0;
    int set_item    = 0;
    int dirty       = 1;
    uint32_t last_clk_draw = 0u;
    key_id_t prev_k = KEY_ENTER;

    lcd_fill_rect(0u, 0u, 160u, 80u, theme->bg);
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
            if (tab == 0) {
                if (k == KEY_BACK) {
                    break;
                } else if ((k == KEY_VOL_UP || k == KEY_LEFT) && file_count > 0) {
                    if (file_sel > 0) {
                        file_sel--;
                        if (file_sel < file_top) {
                            file_top = file_sel;
                        }
                    } else {
                        file_sel = file_count - 1;
                        file_top = (file_count > 3) ? (file_count - 3) : 0;
                    }
                    dirty = 1;
                } else if ((k == KEY_VOL_DOWN || k == KEY_RIGHT) && file_count > 0) {
                    if (file_sel + 1 < file_count) {
                        file_sel++;
                        if (file_sel >= file_top + 3) {
                            file_top = file_sel - 2;
                        }
                    } else {
                        file_sel = 0;
                        file_top = 0;
                    }
                    dirty = 1;
                } else if (k == KEY_ENTER && file_count > 0) {
                    if (name_ends_with_ci(files[file_sel].name, ".wav")) {
                        openfw_play_file(&g_exfat_vol, files[file_sel].dir_path, files[file_sel].name);
                        lcd_set_rotation(1);
                        lcd_fill_rect(0u, 0u, 160u, 80u, theme->bg);
                        prev_k = KEY_BACK;
                        dirty  = 1;
                    }
                }
            } else {
                if (k == KEY_BACK) {
                    break;
                } else if (k == KEY_ENTER || k == KEY_RIGHT) {
                    set_item = (set_item + 1) % 7;
                    dirty = 1;
                } else if (k == KEY_LEFT) {
                    set_item = (set_item + 6) % 7;
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
            if (set_item >= 5) {
                menu_draw_settings(theme, set_item);
            }
            last_clk_draw = now;
        }

        wfi_sleep_ms(25u);
    }

    wait_key_release(KEY_BACK);
}

static int run_usb_msc_mode(const ui_theme_t *theme, int auto_mode)
{
    lcd_fill_rect(0u, 0u, 160u, 80u, theme->bg);
    lcd_fill_rect(0u, 0u, 160u, 12u, theme->track);
    draw_line_padded(2u, g_sd_card.initialized ? "USB MODE   SD: EXPOSED" : "USB MODE   SD: NONE",
                     g_sd_card.initialized ? theme->green : theme->yellow, theme->track);
    draw_line_2x_centered(20u, "USB DISK", theme->bg, theme->cyan);
    draw_line_padded(48u, "RX: 0    TX: 0", theme->text, theme->bg);
    draw_line_padded(69u, "Press BACK to exit USB", theme->yellow, theme->bg);

    usb_msc_set_diag_info((uint8_t)(g_sd_mounted ? 1u : 0u),
                          (uint8_t)(recorder_is_saving() ? 1u : 0u),
                          recorder_get_gain());

    blkdev_t *msc_dev = g_sd_card.initialized ? &g_sd_blkdev : &g_null_blkdev;
    if (usb_msc_start(msc_dev) != 0) {
        return 0;
    }

    backlight_wake(millis());

    uint32_t last_ui     = millis();
#if OPENFW_UNATTENDED_DEV
    uint32_t vbus_miss   = 0u;
#else
    uint16_t last_frame  = 0u;
    uint32_t stale_ticks = 0u;
    int      seen_sof    = 0;
#endif
    uint32_t prev_rxp    = 0u;
    uint32_t prev_txp    = 0u;
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

#if OPENFW_UNATTENDED_DEV
            if (auto_mode) {
                if (!pmu_is_vbus_present()) {
                    vbus_miss++;
                    if (vbus_miss >= 20u) { /* >= 3.0 s debounce against transient PMU glitch */
                        break;
                    }
                } else {
                    vbus_miss = 0u;
                }
            }
#else
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
#endif

            uint32_t rxp = 0u, txp = 0u;
            usb_msc_get_stats(&rxp, &txp);
            pmu_poll_charge_manage();
            if (rxp > 0u || txp > 0u || now >= 3000u) {
                clear_boot_strikes_once();
            }
            if (rxp != prev_rxp || txp != prev_txp) {
                prev_rxp = rxp;
                prev_txp = txp;
                backlight_wake(now);
            }
            backlight_update(now);

            char line[28];
            int pos = 0;
            append_str(line, &pos, (int)sizeof(line), "RX: ");
            append_u32(line, &pos, (int)sizeof(line), rxp);
            append_str(line, &pos, (int)sizeof(line), "   TX: ");
            append_u32(line, &pos, (int)sizeof(line), txp);
            line[pos] = '\0';
            draw_line_padded(48u, line, theme->text, theme->bg);
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

static void play_boot_animation(const ui_theme_t *theme)
{
    /*
     * 2.0-second Cassini boot animation on 160x80 landscape TFT:
     *   - 0.00..0.64 s (frames  0..15): Smooth backlight ramp + logo fade-in & 6 px rise
     *   - 0.64..1.52 s (frames 16..37): Warm Cassini-orange orbital sweep beneath logo
     *   - 1.52..1.72 s (frames 38..42): Crisp hold at full brightness
     *   - 1.72..2.00 s (frames 43..49): Clean fade-out to black
     * Skips immediately if REC slide switch is ON or any button is pressed.
     */
    static const uint8_t star_x[8] = { 14u, 26u, 134u, 146u, 18u, 142u, 32u, 128u };
    static const uint8_t star_y[8] = { 12u, 52u,  14u,  48u, 68u,  66u,  8u,  32u };

    const uint16_t logo_x   = (uint16_t)((160u - CASSINI_SPLASH_80X40_WIDTH) / 2u); /* 40 */
    const uint16_t bar_x    = 30u;
    const uint16_t bar_y    = 62u;
    const uint16_t bar_w    = 100u;
    const uint16_t col_bg   = theme->bg;
    const uint16_t col_trk  = rgb565(32u, 24u, 20u);
    const uint16_t col_orb  = rgb565(232u, 120u, 64u); /* Cassini planet orange */
    const uint16_t col_moon = rgb565(255u, 212u, 168u); /* Warm peach moon glow */

    lcd_set_rotation(1);
    lcd_fill_rect(0u, 0u, 160u, 80u, col_bg);

    uint16_t prev_logo_y = 20u;
    uint32_t t_start     = millis();

    for (uint32_t frame = 0u; frame < 50u; frame++) {
        wdt_feed();
        if (keys_read_rec_switch() || keys_decode(keys_read_adc()) != KEY_NONE) {
            break;
        }

        uint8_t  alpha  = 255u;
        uint16_t logo_y = 14u;

        if (frame < 16u) {
            /* Phase 1: quadratic ease-out rise (y=20 -> 14) + fade-in (0 -> 255) */
            uint32_t inv = 15u - frame;
            logo_y = (uint16_t)(14u + (6u * inv * inv) / 225u);
            alpha  = (uint8_t)((frame * 255u) / 15u);
            uint8_t bl = (uint8_t)(((uint32_t)g_user_bl_pct * (frame + 1u)) / 16u);
            if (bl < 5u) {
                bl = 5u;
            }
            backlight_apply(bl);
        } else if (frame >= 43u) {
            /* Phase 3: smooth fade-out (255 -> 0) over last 280 ms */
            uint32_t rem = 49u - frame;
            alpha = (uint8_t)((rem * 255u) / 6u);
        } else {
            backlight_apply(g_user_bl_pct);
        }

        /* Clear exposed strip below logo if it moved upward */
        if (logo_y < prev_logo_y) {
            lcd_fill_rect(logo_x, (uint16_t)(logo_y + CASSINI_SPLASH_80X40_HEIGHT),
                          CASSINI_SPLASH_80X40_WIDTH, (uint16_t)(prev_logo_y - logo_y), col_bg);
        }
        prev_logo_y = logo_y;

        /* Draw subtle twinkling stars */
        for (uint32_t s = 0u; s < 8u; s++) {
            uint32_t ph = (frame + s * 5u) & 15u;
            uint32_t lum = (ph < 8u) ? (40u + ph * 18u) : (40u + (15u - ph) * 18u);
            lum = (lum * (uint32_t)alpha) >> 8;
            uint16_t sc = rgb565((uint8_t)lum, (uint8_t)((lum * 220u) >> 8), (uint8_t)((lum * 190u) >> 8));
            lcd_fill_rect(star_x[s], star_y[s], 2u, 2u, sc);
        }

        /* Draw Cassini logo (only re-blit during fade/motion or first hold frame) */
        if (frame <= 16u || frame >= 43u) {
            lcd_blit_rgb565_alpha(logo_x, logo_y,
                                  CASSINI_SPLASH_80X40_WIDTH, CASSINI_SPLASH_80X40_HEIGHT,
                                  CASSINI_SPLASH_80X40, alpha);
        }

        /* Phase 2/3: sleek orbital sweep bar beneath logo */
        if (frame >= 16u) {
            uint16_t fill_w = bar_w;
            if (frame < 38u) {
                uint32_t p = frame - 16u; /* 0..21 */
                fill_w = (uint16_t)(((p + 1u) * bar_w) / 22u);
            }
            if (alpha < 255u) {
                /* Fade out bar smoothly */
                uint8_t r = (uint8_t)((232u * alpha) >> 8);
                uint8_t g = (uint8_t)((120u * alpha) >> 8);
                uint8_t b = (uint8_t)((64u  * alpha) >> 8);
                lcd_fill_rect(bar_x, bar_y, bar_w, 4u, col_bg);
                if (alpha > 16u) {
                    lcd_fill_rect(bar_x, (uint16_t)(bar_y + 1u), bar_w, 2u, rgb565(r, g, b));
                }
            } else {
                lcd_fill_rect(bar_x, (uint16_t)(bar_y + 1u), bar_w, 2u, col_trk);
                if (fill_w > 0u) {
                    lcd_fill_rect(bar_x, (uint16_t)(bar_y + 1u), fill_w, 2u, col_orb);
                }
                /* Clear previous moon dot strip above/below track and draw leading glow */
                lcd_fill_rect(bar_x, bar_y, bar_w, 1u, col_bg);
                lcd_fill_rect(bar_x, (uint16_t)(bar_y + 3u), bar_w, 1u, col_bg);
                uint16_t dot_x = (uint16_t)(bar_x + ((fill_w >= 4u) ? (fill_w - 4u) : 0u));
                lcd_fill_rect(dot_x, bar_y, 4u, 4u, col_moon);
            }
        }

        /* Pace cleanly to 40 ms per frame (50 * 40 ms = 2000 ms total) */
        uint32_t target_ms = t_start + (frame + 1u) * 40u;
        while ((int32_t)(millis() - target_ms) < 0) {
            wdt_feed();
            if (keys_read_rec_switch() || keys_decode(keys_read_adc()) != KEY_NONE) {
                break;
            }
            wfi_sleep_ms(4u);
        }
    }

    lcd_fill_rect(0u, 0u, 160u, 80u, col_bg);
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
    /* 3-strike boot-loop guard using AON_BACKUP(7) */
    uint32_t b7 = AON_BACKUP(7u);
    uint32_t boot_strikes = ((b7 & 0xFFFF0000u) == OPENFW_BOOT_STRIKE_MAGIC) ? (b7 & 0xFFFFu) : 0u;
    if (boot_strikes >= OPENFW_BOOT_MAX_STRIKES) {
        AON_BACKUP(7u) = 0u;
        pmu_reboot_to_usb_bootloader();
    }
    AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC | ((boot_strikes + 1u) & 0xFFFFu);

    uint32_t prev_f0     = AON_BACKUP(0u);
    uint32_t prev_pc     = AON_BACKUP(1u);
    uint32_t prev_lr     = AON_BACKUP(2u);
    uint32_t prev_cfsr   = AON_BACKUP(3u);
    uint32_t prev_bfar   = AON_BACKUP(4u);
    uint32_t prev_in_exp = AON_BACKUP(5u);
    int has_prev_fault = ((prev_f0 & OPENFW_FAULT_MAGIC_MASK) == OPENFW_FAULT_MAGIC_BASE) ? 1 : 0;
    if (has_prev_fault) {
        g_prev_fault_code = prev_f0;
        AON_BACKUP(0u)    = 0u;
    } else {
        g_prev_fault_code = 0u;
    }

    systick_init(96000000u); /* Boosted in .ram_boot: PLL0 192 MHz / 2 */
    rtc_ensure_running();
    pmu_unattended_init();
    wdt_start_reset(32u, pmu_read_reg(0x20u));
    wdt_feed();

    /* Emit early UART0 banner before keys_init() switches PD0 to SARADC */
    uart0_init_115200();
    uart0_puts("\n=== Cassini OpenFW (SL6801 Layer 5d: RTC + exFAT + WAV + USB MSC + Classic BT +XREC) ===\n");
    uart0_puts("CPU=96MHz XIP=0x00C0F430 CPU_ID=");
    uart0_put_u32(SYSCTL_CPU_ID);
    uart0_puts(" VBAT_MV=");
    uart0_put_u32(pmu_read_vbat_mv());
    uart0_puts(" STRIKE=");
    uart0_put_u32(boot_strikes + 1u);
    uart0_puts("\n");
    if (has_prev_fault) {
        uart0_puts("PREV_FAULT=");
        uart0_put_hex(prev_f0);
        uart0_puts(" PC=");
        uart0_put_hex(prev_pc);
        uart0_puts(" LR=");
        uart0_put_hex(prev_lr);
        uart0_puts(" CFSR=");
        uart0_put_hex(prev_cfsr);
        uart0_puts(" BFAR=");
        uart0_put_hex(prev_bfar);
        uart0_puts(" IN_EXP=");
        uart0_put_u32(prev_in_exp);
        uart0_puts("\n");
    }

    lcd_init();
    lcd_set_rotation(1); /* Landscape 160x80 */
    lcd_fill_rect(0u, 0u, 160u, 80u, theme.bg);
    g_cur_bl_pct = 255u;
    backlight_apply(0u); /* Backlight ramps up smoothly inside play_boot_animation() */

    char sd_line[20];
    uint16_t sd_col = theme.dim;
    probe_microsd_internal(1, sd_line, (int)sizeof(sd_line), &sd_col, &theme);

    uart0_puts("Switching PD0 to SARADC key ladder & entering main loop...\n");
    keys_init();
    key_onoff_init();
    play_boot_animation(&theme);
    usb_msc_set_audio_cfg_cb(main_usb_audio_cfg_cb);
    openfw_exp_bind_fs(&g_exfat_vol, &g_sd_mounted);
    {
        openfw_exp_api_t exp_api;
        exp_api.magic               = OPENFW_EXP_ABI_MAGIC;
        exp_api.arena_base          = 0;
        exp_api.arena_size          = OPENFW_EXP_ARENA_SIZE;
        exp_api.out_buf             = 0;
        exp_api.out_size            = OPENFW_EXP_OUT_SIZE;
        exp_api.out_len_ptr         = 0;
        exp_api.wdt_feed            = wdt_feed;
        exp_api.millis              = millis;
        exp_api.us_timer_now        = us_timer_now;
        exp_api.udelay              = udelay;
        exp_api.mdelay              = mdelay;
        exp_api.log_reset           = openfw_log_reset;
        exp_api.log_putc            = openfw_log_putc;
        exp_api.log_puts            = openfw_log_puts;
        exp_api.log_u32             = openfw_log_u32;
        exp_api.log_hex             = openfw_log_hex;
        exp_api.pmu_read_reg        = pmu_read_reg;
        exp_api.pmu_write_reg       = pmu_write_reg;
        exp_api.pmu_read_vbat_mv    = pmu_read_vbat_mv;
        exp_api.pmu_is_vbus_present = pmu_is_vbus_present;
        exp_api.key_onoff_init      = key_onoff_init;
        exp_api.key_onoff_pressed   = key_onoff_pressed;
        exp_api.keys_read_adc       = keys_read_adc;
        exp_api.keys_decode         = keys_decode;
        exp_api.keys_read_rec_switch = keys_read_rec_switch;
        exp_api.keys_read_hp_detect = keys_read_hp_detect;
        exp_api.lcd_set_rotation    = lcd_set_rotation;
        exp_api.lcd_get_width       = lcd_get_width;
        exp_api.lcd_get_height      = lcd_get_height;
        exp_api.lcd_fill_rect       = lcd_fill_rect;
        exp_api.lcd_draw_str        = lcd_draw_str;
        exp_api.lcd_backlight_set   = lcd_backlight_set;
        exp_api.sd_write_file       = openfw_exp_sd_write_file;
        exp_api.sd_dump_log         = openfw_exp_sd_dump_log;
        exp_api.audio_capture_start = audio_capture_start;
        exp_api.audio_capture_stop  = audio_capture_stop;
        exp_api.audio_pop_frame     = audio_pop_frame;
        exp_api.audio_get_peak_pct  = audio_get_peak_pct;
        exp_api.audio_get_frames    = audio_get_frames;
        exp_api.audio_get_overruns  = audio_get_overruns;
        exp_api.set_dmac_ch0_irq    = openfw_exp_set_dmac_ch0_irq;
        exp_api.bt_rf_init_calibrated = bt_rf_init_calibrated;
        exp_api.bt_xrec_start       = bt_xrec_start;
        exp_api.bt_xrec_poll        = bt_xrec_poll;
        exp_api.bt_xrec_stop        = bt_xrec_stop;
        exp_api.bt_xrec_open_pairing = bt_xrec_open_pairing;
        exp_api.bt_xrec_get_status  = bt_xrec_get_status;
        openfw_exp_init_api(&exp_api);
    }
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
        if (now_ms >= 3000u) {
            clear_boot_strikes_once();
        }
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
            } else if (!usb_suppress_until_unpl) {
#if OPENFW_UNATTENDED_DEV
                enter_usb_auto   = 1;
                prev_vbus        = 1;
                usb_probing      = 0;
                usb_charger_only = 0;
                (void)prev_vbus;
                (void)usb_probing;
                (void)usb_probe_deadline;
#else
                if (g_sd_card.initialized) {
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
#endif
            }

            if (enter_usb_auto || (key == KEY_ENTER && prev_key != KEY_ENTER)) {
                int manual = run_usb_msc_mode(&theme, enter_usb_auto);
                usb_suppress_until_unpl = (manual && pmu_is_vbus_present()) ? 1 : 0;
                clear_body_area(theme.bg);
                draw_line_padded(45u, "OpenFW 96M", theme.hdr, theme.bg);
                format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                draw_line_padded(57u, sd_line, sd_col, theme.bg);
                draw_line_padded(147u, "E:USB B2s:DFU", theme.dim, theme.bg);
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
                draw_line_padded(147u, "E:USB B2s:DFU", theme.dim, theme.bg);
                prev_key        = KEY_NONE;
                back_hold_ticks = 0u;
                last_slow_ui    = 0u;
                continue;
            }
            if (key == KEY_LEFT && prev_key != KEY_LEFT) {
                run_menu_mode(&theme, 0);
                clear_body_area(theme.bg);
                draw_line_padded(45u, "OpenFW 96M", theme.hdr, theme.bg);
                format_sd_status(sd_line, (int)sizeof(sd_line), &sd_col, &theme);
                draw_line_padded(57u, sd_line, sd_col, theme.bg);
                draw_line_padded(147u, "E:USB B2s:DFU", theme.dim, theme.bg);
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
            int onoff_long = 0;
            if (key_onoff_pressed(&onoff_long)) {
                backlight_wake(now_ms);
            }
            if (onoff_long && !pmu_is_vbus_present()) {
                (void)recorder_stop();
                rec_active = 0;
                lcd_fill_rect(0u, 0u, 160u, 80u, theme.bg);
                draw_line_2x_centered(30u, "POWER OFF", theme.alert, theme.bg);
                mdelay(300u);
                pmu_soft_poweroff();
            }
            mv  = pmu_read_vbat_mv();
            chg = pmu_get_charge_state();
            pmu_poll_charge_manage();

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
                draw_line_padded(93u, "L:MENU  R:BT", theme.cyan, theme.bg);
            }

            /* Row 6 (y=118): Active ch/DSP tag + VU or Idle config */
            pos = 0;
            if (rec_active) {
                append_str(line, &pos, (int)sizeof(line), "VU ");
                append_u32(line, &pos, (int)sizeof(line), peak_pct);
                append_str(line, &pos, (int)sizeof(line), "% ");
                append_u32(line, &pos, (int)sizeof(line), recorder_get_channels());
                append_str(line, &pos, (int)sizeof(line), "C ");
                append_str(line, &pos, (int)sizeof(line), dsp_short_tag(recorder_get_dsp_profile()));
            } else {
                append_str(line, &pos, (int)sizeof(line), "MIC ");
                append_u32(line, &pos, (int)sizeof(line), recorder_get_channels());
                append_str(line, &pos, (int)sizeof(line), "C ");
                append_str(line, &pos, (int)sizeof(line), dsp_short_tag(recorder_get_dsp_profile()));
                if (recorder_get_vor()) {
                    append_str(line, &pos, (int)sizeof(line), " V");
                }
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
