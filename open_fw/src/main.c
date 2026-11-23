/*
 * Cassini OpenFW — Standalone Open-Source Firmware (Layer 4)
 *
 * Runs 100% standalone from Stage-1 HLKJ Bootloader via a <= 4 KiB .ram_boot
 * trampoline at 0x00804C00 that boosts CPU/PLL0 to 96 MHz and maps SPI NOR
 * XIP at 0x00C0F430+:
 *   - UART0 early boot banner & microSD diagnostic output
 *   - 1-bit PD4/PD5/PD6 SDIO block driver -> exFAT reader & writer
 *   - 160x80 landscape anti-aliased Cassini UI (Home, Recording + 128-pt FFT
 *     Mel spectrogram, Saved, Files, Player + CWX captions, Delete, Settings,
 *     About, USB Disk, Bluetooth Sync, Power Off)
 *   - PB6 Record slide switch -> instant 16 kHz / 16-bit WAV recording into
 *     /record/REC_NNNN.wav via DMAC CH1 (IRQ 61) 4-slot ring buffer
 *   - Clean-room Full-Speed Mentor MUSB + SCSI BOT USB Mass Storage (+ 0xE0
 *     vendor SCSI / SRAM experiment runner API)
 *   - Classic BT SPP + +XREC v5 wireless sync service
 *   - 2-second Cassini boot animation & 2-second soft power-off
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_ui.h"
#include "ui/ui_port.h"
#include "openfw_exp.h"
#include "openfw_wav.h"
#include "openfw_player.h"
#include "cassini_splash.h"

#define MENU_MAX_FILES 32
#define MENU_MAX_CWX   64
#define LIST_SCROLL_DELAY_MS  400u
#define LIST_SCROLL_REPEAT_MS 100u

typedef struct {
    uint64_t size;
    uint64_t valid_data_len;
    uint32_t first_cluster;
    uint8_t  no_fat_chain;
} wav_loc_t;

typedef struct {
    ui_file_entry_t *items;
    char           (*dirs)[12];
    wav_loc_t       *locs;
    uint64_t        *cwx_hashes;
    int              cwx_count;
    int              cwx_cap;
    int              cwx_overflow;
    int              count;
    int              total_seen;
    int              max_items;
    const char      *dir_path;
} menu_list_ctx_t;

extern volatile uint8_t g_openfw_boot_strike_counted;

static sd_card_t        g_sd_card;
static blkdev_t         g_sd_blkdev;
static blkdev_t         g_null_blkdev = { 0, 0, 0u, 0 };
static exfat_vol_t      g_exfat_vol;
static int              g_sd_mounted = 0;
static int              g_boot_strikes_cleared = 0;
static int              g_usb_cfg_dirty = 0;
static int              g_cached_vbus = 0;
static uint32_t         g_vbus_poll_ms = 0u;
static int              g_vbus_cache_valid = 0;

static void save_persisted_state(void);

volatile uint32_t       g_ui_screen = UI_SCR_HOME;
static ui_model_t       g_ui;
static ui_spectrogram_t g_spec;
static ui_file_entry_t  g_files[MENU_MAX_FILES];
static char             g_file_dirs[MENU_MAX_FILES][12];
static char             g_last_dir[12] = "/record";
static uint32_t         g_sd_free_mb   = 0u;
static uint32_t         g_sd_total_mb  = 0u;

static const uint8_t    k_brt_pct[5]       = { 20u, 40u, 60u, 80u, 100u };
#if !OPENFW_UNATTENDED_DEV
static const uint32_t   k_scr_off_ms[5]    = { 15000u, 30000u, 60000u, 300000u, 0u };
#endif
static uint8_t          g_cur_bl_pct       = 255u;
static uint32_t         g_last_activity_ms = 0u;
enum screen_sleep_state {
    SCREEN_AWAKE,
    SCREEN_IDLE_TIMEOUT,
    SCREEN_MANUALLY_OFF
};
static enum screen_sleep_state g_screen_off = SCREEN_AWAKE;
static uint32_t         g_toast_until_ms   = 0u;
static int              g_cpu1_booted      = 0;

static void clear_boot_strikes_once(void)
{
    if (!g_boot_strikes_cleared) {
        AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC;
        g_openfw_boot_strike_counted = 0u;
        g_boot_strikes_cleared = 1;
    }
}

static void wfi_sleep_ms(uint32_t ms)
{
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < ms) {
        wdt_feed();
        if (keys_event_pending()) {
            break;
        }
        __asm__ volatile ("wfi");
    }
    wdt_feed();
}

static uint8_t user_bl_pct(void)
{
    uint8_t idx = g_ui.cfg.brightness_idx;
    return (idx < 5u) ? k_brt_pct[idx] : 80u;
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
    int was_off        = (g_screen_off != SCREEN_AWAKE);
    g_screen_off       = SCREEN_AWAKE;
    if (was_off && g_cpu1_booted && !cpu1_ui_is_active()) {
        (void)cpu1_ui_start();
    }
    backlight_apply(user_bl_pct());
}

static void backlight_sleep_now(enum screen_sleep_state reason)
{
    g_screen_off = reason;
    backlight_apply(0u);
    ui_render_screen(UI_SCR_OFF, &g_ui);
    cpu1_ui_sync(60u);
    /* When hardware IPC Mailbox WFE is active, CPU1 is already halted in
     * zero-power WFE sleep; only park CPU1 in reset when WFE is unavailable. */
    if (!cpu1_ui_wfe_active()) {
        cpu1_ui_stop();
    }
}

int ui_poll_power_button(ui_screen_id_t screen, ui_model_t *model, uint32_t now)
{
    /* USB and audio service loops can run many times in a millisecond.
     * The PMU latches edges until acknowledged. */
    static uint32_t last_poll;
    static int      seen_press_after_boot;
    if (now - last_poll >= 16u) {
        last_poll = now;
        uint8_t ev = key_onoff_poll();
        if (ev & KEY_ONOFF_PRESS) {
            seen_press_after_boot = 1;
        }
        /* Ignore residual PMU LK from the power-on hold during early boot */
        if (!seen_press_after_boot && now < 3000u) {
            ev &= (uint8_t)~KEY_ONOFF_LONG;
        }
        uint8_t progress = 0;
        power_button_action_t action = power_button_update(ev, now,
                                                           g_screen_off, &progress);
        switch (action) {
        case POWER_BUTTON_SLEEP:
            backlight_sleep_now(SCREEN_MANUALLY_OFF);
            break;
        case POWER_BUTTON_WAKE:
        case POWER_BUTTON_CANCEL:
            backlight_wake(now);
            g_ui_screen = screen;
            ui_render_screen(screen, model);
            break;
        case POWER_BUTTON_PROGRESS:
            if (g_ui_screen != UI_SCR_POWER || progress != model->power_hold_pct) {
                backlight_wake(now);
                model->power_hold_pct = progress;
                g_ui_screen = UI_SCR_POWER;
                ui_render_screen(UI_SCR_POWER, model);
            }
            break;
        case POWER_BUTTON_SHUTDOWN:
            lcd_shutdown();
            if (screen == UI_SCR_USB) {
                usb_msc_stop();
            } else if (screen == UI_SCR_BT) {
                bt_xrec_stop();
            } else if (screen == UI_SCR_PLAYER) {
                audio_dac_stop();
                g_ui.last_file.resume_sec = g_ui.play_file.resume_sec;
            }
            (void)recorder_stop();
            if (screen == UI_SCR_SETTINGS || screen == UI_SCR_PLAYER) {
                save_persisted_state();
            }
            (void)sd_flush(&g_sd_card);
            pmu_soft_poweroff();
        case POWER_BUTTON_IDLE:
            break;
        }
    }
    return !g_screen_off && g_ui_screen != UI_SCR_POWER;
}

static void backlight_update(void)
{
    if (g_screen_off) {
        backlight_apply(0u);
        return;
    }
#if OPENFW_UNATTENDED_DEV
    backlight_apply(user_bl_pct());
#else
    uint8_t idx = g_ui.cfg.screen_off_idx;
    uint32_t off_ms = (idx < 5u) ? k_scr_off_ms[idx] : 30000u;
    if (off_ms == 0u) {
        backlight_apply(user_bl_pct());
        return;
    }
    /* A queued key may have arrived after the main loop sampled its clock. */
    uint32_t idle_ms = millis() - g_last_activity_ms;
    if (idle_ms >= off_ms) {
        backlight_sleep_now(SCREEN_IDLE_TIMEOUT);
    } else if (off_ms >= 20000u && idle_ms >= (off_ms / 2u)) {
        uint8_t u = user_bl_pct();
        uint8_t dim = (u < 15u) ? u : 15u;
        backlight_apply(dim);
    } else {
        backlight_apply(user_bl_pct());
    }
#endif
}

static void copy_cstr(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) {
        return;
    }
    while (src && src[i] != '\0' && (i + 1) < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void append_str(char *dst, int *pos, int max, const char *src)
{
    while (src && *src != '\0' && *pos < (max - 1)) {
        dst[(*pos)++] = *src++;
    }
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

static void ui_show_toast(const char *msg, uint16_t col, uint32_t now_ms, uint32_t dur_ms)
{
    copy_cstr(g_ui.toast_msg, (int)sizeof(g_ui.toast_msg), msg);
    g_ui.toast_col   = col;
    g_toast_until_ms = now_ms + dur_ms;
}

static int ui_poll_toast_expire(uint32_t now_ms)
{
    if (g_ui.toast_msg[0] != '\0' && (int32_t)(now_ms - g_toast_until_ms) >= 0) {
        g_ui.toast_msg[0] = '\0';
        return 1;
    }
    return 0;
}

static uint8_t vbat_mv_to_pct(uint32_t mv)
{
    if (mv <= 3400u) return 0u;
    if (mv >= 4180u) return 100u;
    return (uint8_t)(((mv - 3400u) * 100u) / (4180u - 3400u));
}

static void sanitize_rtc(rtc_datetime_t *dt)
{
    if (dt->year < 2000u || dt->year > 2099u) dt->year = 2026u;
    if (dt->month < 1u || dt->month > 12u)   dt->month = 1u;
    if (dt->day < 1u || dt->day > 31u)       dt->day = 1u;
    if (dt->hour > 23u)                      dt->hour = 0u;
    if (dt->minute > 59u)                    dt->minute = 0u;
    if (dt->second > 59u)                    dt->second = 0u;
}

static int vbus_present_cached(uint32_t now_ms)
{
    if (!g_vbus_cache_valid || (uint32_t)(now_ms - g_vbus_poll_ms) >= 100u) {
        g_cached_vbus      = pmu_is_vbus_present();
        g_vbus_poll_ms     = now_ms;
        g_vbus_cache_valid = 1;
    }
    return g_cached_vbus;
}

static void refresh_telemetry(void)
{
    rtc_get(&g_ui.now);
    sanitize_rtc(&g_ui.now);
    uint32_t mv = pmu_read_vbat_mv();
    pmu_chg_state_t chg = pmu_get_charge_state();
    int vbus            = (chg != PMU_CHG_DISCHARGING) ? 1 : 0;
    g_cached_vbus       = vbus;
    g_vbus_poll_ms      = millis();
    g_vbus_cache_valid  = 1;
    g_ui.batt_mv      = (uint16_t)mv;
    g_ui.batt_pct     = vbat_mv_to_pct(mv);
    g_ui.charging     = (chg == PMU_CHG_CHARGING) ? 1 : 0;
    g_ui.vbus_present = (uint8_t)vbus;
    g_ui.hp_plugged   = keys_read_hp_detect();
    g_ui.sd_ok      = g_sd_mounted ? 1 : 0;
    g_ui.sd_total_mb = g_sd_total_mb;
    uint32_t sec_per_mb = (g_ui.cfg.channels == 1u) ? 33u : 16u;
    g_ui.sd_free_sec = g_sd_free_mb * sec_per_mb;
}

static void apply_cfg_to_recorder(void)
{
    recorder_set_gain(g_ui.cfg.mic_gain);
    recorder_set_dsp_profile((audio_dsp_profile_t)g_ui.cfg.sound_mode);
    recorder_set_channels(g_ui.cfg.channels);
    recorder_set_vor((int)g_ui.cfg.vor_enabled);
}

static int str_eq_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a != '\0' && *b != '\0') {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static void save_persisted_state(void)
{
    if (!g_sd_mounted) {
        return;
    }
    rtc_datetime_t now;
    rtc_get(&now);
    sanitize_rtc(&now);
    exfat_set_timestamp(&g_exfat_vol, rtc_to_dos_timestamp(&now));
    (void)ui_persist_save(&g_exfat_vol, &g_ui.cfg,
                          g_ui.has_last ? g_ui.last_file.filename : "",
                          g_ui.has_last ? g_ui.last_file.resume_sec : 0u,
                          g_ui.has_last);
}

static rtc_datetime_t file_datetime(const exfat_entry_t *entry)
{
    rtc_datetime_t created = rtc_from_dos_timestamp(entry->created_timestamp);
    return created.year ? created : rtc_from_dos_timestamp(entry->modified_timestamp);
}

static void fill_wav_metadata(const exfat_entry_t *entry, ui_file_entry_t *dst)
{
    dst->channels     = 1u;
    dst->duration_sec = (entry->size > 44u) ? ((uint32_t)(entry->size - 44u) / 32000u) : 0u;

    /* 512-byte WAV header read covers LIST/INFO/JUNK/fact chunks before data */
    uint8_t hdr[512];
    uint32_t got = 0u;
    if (entry->size >= 44u &&
        exfat_read_file(&g_exfat_vol, entry, 0u, hdr, (uint32_t)sizeof(hdr), &got) == 0 &&
        got >= 44u) {
        wav_stream_info_t wi;
        if (wav_parse_stream(hdr, got, &wi) == 0) {
            dst->duration_sec = (wi.duration_ms + 500u) / 1000u;
            dst->channels     = (uint8_t)(wi.channels ? wi.channels : 1u);
        }
    }
}

static void load_persisted_state(void)
{
    if (!g_sd_mounted) {
        return;
    }
    char     last_name[32];
    uint32_t last_resume = 0u;
    int      has_last    = 0;
    if (ui_persist_load(&g_exfat_vol, &g_ui.cfg, last_name, &last_resume, &has_last) != 0) {
        return;
    }

    apply_cfg_to_recorder();
    backlight_apply(user_bl_pct());

    if (has_last && last_name[0] != '\0') {
        int found = 0;
        for (int i = 0; i < g_ui.file_count; i++) {
            if (str_eq_ci(g_files[i].filename, last_name)) {
                g_files[i].resume_sec = (last_resume < g_files[i].duration_sec) ? last_resume : 0u;
                g_files[i].unplayed   = 0u;
                g_ui.last_file        = g_files[i];
                copy_cstr(g_last_dir, (int)sizeof(g_last_dir), g_file_dirs[i]);
                g_ui.has_last         = 1;
                found = 1;
                break;
            }
        }
        if (!found) {
            char full[48];
            int p = 0;
            append_str(full, &p, (int)sizeof(full), "/record/");
            append_str(full, &p, (int)sizeof(full), last_name);
            full[p] = '\0';
            exfat_entry_t ent;
            if (exfat_find_path(&g_exfat_vol, full, &ent) == 0 && !ent.is_dir) {
                copy_cstr(g_ui.last_file.filename, (int)sizeof(g_ui.last_file.filename), last_name);
                g_ui.last_file.dt             = file_datetime(&ent);
                fill_wav_metadata(&ent, &g_ui.last_file);
                g_ui.last_file.resume_sec     = (last_resume < g_ui.last_file.duration_sec) ? last_resume : 0u;
                g_ui.last_file.unplayed       = 0u;
                g_ui.last_file.has_transcript = 0u;
                copy_cstr(g_last_dir, (int)sizeof(g_last_dir), "/record");
                g_ui.has_last = 1;
            }
        }
    }
    refresh_telemetry();
}

static uint64_t hash_dir_stem_ci(const char *dir, const char *name)
{
    uint64_t h = 14695981039346656037ULL;
    if (dir) {
        for (const char *p = dir; *p != '\0'; p++) {
            uint8_t c = (uint8_t)*p;
            if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + ('a' - 'A'));
            h ^= c;
            h *= 1099511628211ULL;
        }
    }
    h ^= 0xFFu;
    h *= 1099511628211ULL;
    int len = 0;
    while (name && name[len] != '\0') {
        len++;
    }
    int stem_len = (len >= 4) ? (len - 4) : len;
    for (int i = 0; i < stem_len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + ('a' - 'A'));
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

static void reverse_file_range(ui_file_entry_t *items, char (*dirs)[12], wav_loc_t *locs,
                               int lo, int hi)
{
    while (lo < hi) {
        ui_file_entry_t tf = items[lo];
        items[lo] = items[hi];
        items[hi] = tf;

        char td[12];
        copy_cstr(td, 12, dirs[lo]);
        copy_cstr(dirs[lo], 12, dirs[hi]);
        copy_cstr(dirs[hi], 12, td);

        wav_loc_t tl = locs[lo];
        locs[lo] = locs[hi];
        locs[hi] = tl;

        lo++;
        hi--;
    }
}

static int menu_collect_cb(const exfat_entry_t *entry, void *user)
{
    menu_list_ctx_t *ctx = (menu_list_ctx_t *)user;
    wdt_feed();
    if (entry->is_dir) {
        return 0;
    }
    if (name_ends_with_ci(entry->name, ".cwx")) {
        if (ctx->cwx_count < ctx->cwx_cap) {
            ctx->cwx_hashes[ctx->cwx_count++] = hash_dir_stem_ci(ctx->dir_path, entry->name);
        } else {
            ctx->cwx_overflow = 1;
        }
        return 0;
    }
    if (!name_ends_with_ci(entry->name, ".wav")) {
        return 0;
    }
    /* Ring-buffer the newest max_items entries in-place; defer WAV header reads */
    int idx = ctx->total_seen % ctx->max_items;
    ctx->total_seen++;
    if (ctx->count < ctx->max_items) {
        ctx->count++;
    }
    ui_file_entry_t *dst = &ctx->items[idx];
    copy_cstr(ctx->dirs[idx], 12, ctx->dir_path);
    copy_cstr(dst->filename, (int)sizeof(dst->filename), entry->name);
    dst->dt             = file_datetime(entry);
    dst->resume_sec     = 0u;
    dst->unplayed       = 1u;
    dst->has_transcript = 0u;
    dst->channels       = 1u;
    dst->duration_sec   = (entry->size > 44u) ? ((uint32_t)(entry->size - 44u) / 32000u) : 0u;

    ctx->locs[idx].valid_data_len = entry->valid_data_len;
    ctx->locs[idx].size          = entry->size;
    ctx->locs[idx].first_cluster = entry->first_cluster;
    ctx->locs[idx].no_fat_chain  = entry->no_fat_chain;
    return 0;
}

static void reload_files_and_sd_stats(void)
{
    g_sd_free_mb  = 0u;
    g_sd_total_mb = 0u;
    g_ui.files      = g_files;
    g_ui.file_count = 0;

    if (!g_sd_mounted) {
        g_ui.sd_ok       = 0;
        g_ui.sd_total_mb = 0u;
        g_ui.sd_free_sec = 0u;
        return;
    }

    wdt_feed();
    (void)exfat_stat_fs(&g_exfat_vol, &g_sd_free_mb, &g_sd_total_mb);

    wav_loc_t locs[MENU_MAX_FILES];
    uint64_t  cwx_hashes[MENU_MAX_CWX];
    menu_list_ctx_t ctx;
    ctx.items        = g_files;
    ctx.dirs         = g_file_dirs;
    ctx.locs         = locs;
    ctx.cwx_hashes   = cwx_hashes;
    ctx.cwx_count    = 0;
    ctx.cwx_cap      = MENU_MAX_CWX;
    ctx.cwx_overflow = 0;
    ctx.count        = 0;
    ctx.total_seen   = 0;
    ctx.max_items    = MENU_MAX_FILES;

    exfat_entry_t dir_ent;
    if (exfat_find_path(&g_exfat_vol, "/record", &dir_ent) == 0 && dir_ent.is_dir) {
        ctx.dir_path = "/record";
        (void)exfat_list_dir_entry(&g_exfat_vol, &dir_ent, menu_collect_cb, &ctx);
    }
    if (exfat_find_path(&g_exfat_vol, "/cassini", &dir_ent) == 0 && dir_ent.is_dir) {
        ctx.dir_path = "/cassini";
        (void)exfat_list_dir_entry(&g_exfat_vol, &dir_ent, menu_collect_cb, &ctx);
    }
    ctx.dir_path = "/";
    (void)exfat_list_dir(&g_exfat_vol, g_exfat_vol.root_dir_cluster, 0u,
                         menu_collect_cb, &ctx);

    /* Unwrap ring buffer in-place into reverse chronological order (newest first) */
    if (ctx.count > 1) {
        int head = (ctx.total_seen > ctx.max_items) ? (ctx.total_seen % ctx.max_items) : 0;
        if (head > 1) {
            reverse_file_range(g_files, g_file_dirs, locs, 0, head - 1);
        }
        if (head < ctx.count - 1) {
            reverse_file_range(g_files, g_file_dirs, locs, head, ctx.count - 1);
        }
    }

    /* Read 512 B WAV headers & match .cwx transcripts only for the selected files */
    for (int i = 0; i < ctx.count; i++) {
        wdt_feed();
        exfat_entry_t wav_ent = {
            .size = locs[i].size,
            .valid_data_len = locs[i].valid_data_len,
            .first_cluster = locs[i].first_cluster,
            .no_fat_chain = locs[i].no_fat_chain
        };
        fill_wav_metadata(&wav_ent, &g_files[i]);

        int has_cwx = 0;
        if (ctx.cwx_count > 0) {
            uint64_t h = hash_dir_stem_ci(g_file_dirs[i], g_files[i].filename);
            for (int c = 0; c < ctx.cwx_count; c++) {
                if (cwx_hashes[c] == h) {
                    has_cwx = 1;
                    break;
                }
            }
        }
        if (!has_cwx && ctx.cwx_overflow) {
            char cpath[96];
            int p = 0;
            if (!(g_file_dirs[i][0] == '/' && g_file_dirs[i][1] == '\0')) {
                append_str(cpath, &p, (int)sizeof(cpath), g_file_dirs[i]);
            }
            append_str(cpath, &p, (int)sizeof(cpath), "/");
            append_str(cpath, &p, (int)sizeof(cpath), g_files[i].filename);
            cpath[p] = '\0';
            if (p >= 4) {
                cpath[p - 3] = 'c';
                cpath[p - 2] = 'w';
                cpath[p - 1] = 'x';
                exfat_entry_t cent;
                if (exfat_find_path(&g_exfat_vol, cpath, &cent) == 0 && !cent.is_dir) {
                    has_cwx = 1;
                }
            }
        }
        g_files[i].has_transcript = has_cwx ? 1u : 0u;
    }

    g_ui.file_count = ctx.count;
    if (g_ui.file_sel >= ctx.count) {
        g_ui.file_sel = (ctx.count > 0) ? (ctx.count - 1) : 0;
    }
    if (g_ui.file_scroll > g_ui.file_sel) {
        g_ui.file_scroll = g_ui.file_sel;
    }
    if (g_ui.file_count > 0 && !g_ui.has_last) {
        g_ui.last_file = g_files[0];
        copy_cstr(g_last_dir, (int)sizeof(g_last_dir), g_file_dirs[0]);
        g_ui.has_last = 1;
    }
    refresh_telemetry();
}

static void probe_microsd_mount_only(int log_uart, uint32_t *out_sd_us, uint32_t *out_mnt_us)
{
    g_sd_mounted = 0;
    uint64_t t0 = us_timer_now();
    if (sd_init(&g_sd_card) != 0) {
        if (out_sd_us) *out_sd_us = (uint32_t)(us_timer_now() - t0);
        if (out_mnt_us) *out_mnt_us = 0u;
        if (log_uart) {
            uart0_puts("SD: NONE (sd_init failed)\n");
        }
        return;
    }
    uint64_t t1 = us_timer_now();
    if (out_sd_us) *out_sd_us = (uint32_t)(t1 - t0);

    g_sd_blkdev = sd_as_blkdev(&g_sd_card);
    if (exfat_mount(&g_exfat_vol, &g_sd_blkdev) != 0) {
        if (out_mnt_us) *out_mnt_us = (uint32_t)(us_timer_now() - t1);
        if (log_uart) {
            uart0_puts("SD: RAW (card ready, no exFAT partition)\n");
        }
        return;
    }
    if (out_mnt_us) *out_mnt_us = (uint32_t)(us_timer_now() - t1);
    g_sd_mounted = 1;
}

static void probe_microsd_internal(int log_uart)
{
    probe_microsd_mount_only(log_uart, 0, 0);
    reload_files_and_sd_stats();
    if (log_uart && g_sd_mounted) {
        uart0_puts("SD: OK ");
        uart0_put_u32((uint32_t)g_ui.file_count);
        uart0_puts("f\n");
    }
}

static void prepend_saved_recording_fast(const char *saved_name, uint32_t rec_s,
                                         uint8_t rec_ch, uint32_t committed_bytes)
{
    /* Remove any pre-existing entry with the same name in /record */
    int existing = -1;
    for (int i = 0; i < g_ui.file_count; i++) {
        if (str_eq_ci(g_file_dirs[i], "/record") && str_eq_ci(g_files[i].filename, saved_name)) {
            existing = i;
            break;
        }
    }
    int shift_end = (existing >= 0) ? existing : g_ui.file_count;
    if (shift_end >= MENU_MAX_FILES) {
        shift_end = MENU_MAX_FILES - 1;
    }
    for (int i = shift_end; i > 0; i--) {
        g_files[i] = g_files[i - 1];
        copy_cstr(g_file_dirs[i], 12, g_file_dirs[i - 1]);
    }
    if (existing < 0 && g_ui.file_count < MENU_MAX_FILES) {
        g_ui.file_count++;
    }

    ui_file_entry_t *f = &g_files[0];
    copy_cstr(g_file_dirs[0], 12, "/record");
    copy_cstr(f->filename, (int)sizeof(f->filename), saved_name);
    f->dt             = rtc_from_dos_timestamp(recorder_get_created_timestamp());
    f->duration_sec   = rec_s;
    f->resume_sec     = 0u;
    f->unplayed       = 1u;
    f->has_transcript = 0u;
    f->channels       = rec_ch ? rec_ch : 1u;

    uint32_t used_mb = (committed_bytes + 0xFFFFFu) >> 20;
    if (g_sd_free_mb > used_mb) {
        g_sd_free_mb -= used_mb;
    } else {
        g_sd_free_mb = 0u;
    }
    g_ui.files     = g_files;
    g_ui.last_file = *f;
    copy_cstr(g_last_dir, (int)sizeof(g_last_dir), "/record");
    g_ui.has_last  = 1;
    refresh_telemetry();
}

static int main_usb_audio_cfg_cb(int set, uint8_t cfg[4], int16_t *rms_x10, uint16_t *clips)
{
    if (set) {
        if (cfg[0] != 0xFFu) {
            recorder_set_dsp_profile((audio_dsp_profile_t)cfg[0]);
            g_ui.cfg.sound_mode = (uint8_t)recorder_get_dsp_profile();
            g_usb_cfg_dirty = 1;
        }
        if (cfg[1] != 0xFFu) {
            recorder_set_gain(cfg[1]);
            g_ui.cfg.mic_gain = recorder_get_gain();
            g_usb_cfg_dirty = 1;
        }
        if (cfg[2] != 0xFFu) {
            recorder_set_channels(cfg[2]);
            g_ui.cfg.channels = recorder_get_channels();
            g_usb_cfg_dirty = 1;
        }
        if (cfg[3] != 0xFFu) {
            recorder_set_vor((int)cfg[3]);
            g_ui.cfg.vor_enabled = (uint8_t)(recorder_get_vor() ? 1u : 0u);
            g_usb_cfg_dirty = 1;
        }
    }
    cfg[0] = (uint8_t)recorder_get_dsp_profile();
    cfg[1] = recorder_get_gain();
    cfg[2] = recorder_get_channels();
    cfg[3] = (uint8_t)(recorder_get_vor() ? 1u : 0u);
    if (rms_x10) *rms_x10 = recorder_get_rms_dbfs_x10();
    if (clips)   *clips   = (uint16_t)recorder_get_clip_events();
    return 0;
}

static int run_usb_msc_mode(int auto_mode)
{
    keys_flush_events();
    g_ui_screen        = UI_SCR_USB;
    g_ui.usb_read_act  = 0u;
    g_ui.usb_write_act = 0u;
    g_usb_cfg_dirty    = 0;
    refresh_telemetry();
    ui_render_screen(UI_SCR_USB, &g_ui);

    usb_msc_set_diag_info((uint8_t)(g_sd_mounted ? 1u : 0u),
                          (uint8_t)(recorder_is_saving() ? 1u : 0u),
                          recorder_get_gain());

    blkdev_t *msc_dev = g_sd_card.initialized ? &g_sd_blkdev : &g_null_blkdev;
    if (usb_msc_start(msc_dev) != 0) {
        keys_ignore_held();
        return 0;
    }
    if (g_sd_card.initialized) {
        usb_msc_enable_sd_async(&g_sd_card);
    }

    backlight_wake(millis());

    uint32_t last_ui         = millis();
#if OPENFW_UNATTENDED_DEV
    uint32_t vbus_miss       = 0u;
#else
    uint16_t last_frame      = 0u;
    uint32_t stale_ticks     = 0u;
    int      seen_sof        = 0;
#endif
    uint32_t prev_rxp        = 0u;
    uint32_t prev_txp        = 0u;
    uint32_t rd_blink_until  = 0u;
    uint32_t wr_blink_until  = 0u;
    uint32_t back_press_ms   = 0u;
    int      back_down       = 0;
    int      manual_exit     = 0;

    uint32_t last_render = 0u;
    while (1) {
        wdt_feed();
        for (int burst = 0; burst < 32; burst++) {
            usb_msc_poll();
        }

        key_event_t kev;
        while (keys_pop_event(&kev)) {
            if (kev.pressed) {
                backlight_wake(kev.t_ms);
            }
            if (kev.key == KEY_BACK) {
                if (kev.pressed) {
                    back_down = 1;
                    back_press_ms = kev.t_ms;
                } else {
                    manual_exit = back_down && (uint32_t)(kev.t_ms - back_press_ms) >= 600u;
                    back_down = 0;
                    if (manual_exit) break;
                }
            }
        }
        uint32_t now = millis();
        int show_usb = ui_poll_power_button(UI_SCR_USB, &g_ui, now);
        if (keys_state(NULL) != KEY_BACK && !keys_event_pending()) back_down = 0;
        if (manual_exit || (back_down && (uint32_t)(now - back_press_ms) >= 600u)) {
            manual_exit = 1;
            break;
        }

        if ((uint32_t)(now - last_ui) >= 100u) {
            last_ui = now;

#if OPENFW_UNATTENDED_DEV
            if (auto_mode) {
                if (!pmu_is_vbus_present()) {
                    vbus_miss++;
                    if (vbus_miss >= 30u) { /* >= 3.0 s debounce against transient PMU glitch */
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
                if (!seen_sof) {
                    if (frame != 0u) {
                        seen_sof    = 1;
                        last_frame  = frame;
                        stale_ticks = 0u;
                    }
                } else if (frame == last_frame) {
                    stale_ticks++;
                    if (stale_ticks >= 8u) { /* >= 800 ms without SOF */
                        break;
                    }
                } else {
                    last_frame  = frame;
                    stale_ticks = 0u;
                }
            }
#endif

            uint32_t rxp = 0u, txp = 0u;
            usb_msc_get_stats(&rxp, &txp);
            usb_msc_get_bytes(&g_ui.usb_rx_bytes, &g_ui.usb_tx_bytes);
            if (rxp > 0u || txp > 0u || now >= 3000u) {
                clear_boot_strikes_once();
            }
            if ((txp != prev_txp || rxp != prev_rxp) && g_screen_off != SCREEN_MANUALLY_OFF) {
                backlight_wake(now);
            }
            if (txp != prev_txp) {
                prev_txp       = txp;
                uint32_t dl    = now + 250u;
                rd_blink_until = dl ? dl : 1u;
            }
            if (rxp != prev_rxp) {
                prev_rxp       = rxp;
                uint32_t dl    = now + 250u;
                wr_blink_until = dl ? dl : 1u;
            }
            backlight_update();

            if (rd_blink_until != 0u && (int32_t)(now - rd_blink_until) >= 0) {
                rd_blink_until = 0u;
            }
            if (wr_blink_until != 0u && (int32_t)(now - wr_blink_until) >= 0) {
                wr_blink_until = 0u;
            }
            g_ui.usb_read_act  = (rd_blink_until != 0u) ? 1u : 0u;
            g_ui.usb_write_act = (wr_blink_until != 0u) ? 1u : 0u;
            uint32_t render_period = (g_ui.usb_read_act || g_ui.usb_write_act) ? 500u : 300u;
            if ((uint32_t)(now - last_render) >= render_period) {
                last_render = now;
                pmu_poll_charge_manage();
                refresh_telemetry();
                if (show_usb && !g_screen_off) {
                    ui_render_screen(UI_SCR_USB, &g_ui);
                }
            }
        }
    }

    keys_ignore_held();
    usb_msc_stop();
    (void)sd_flush(&g_sd_card);
    backlight_wake(millis());

    /* Remount exFAT because USB host may have written or deleted files;
     * if mount fails (e.g. card state changed), re-initialize SDIO cleanly. */
    if (g_sd_card.initialized) {
        g_sd_mounted = (exfat_mount(&g_exfat_vol, &g_sd_blkdev) == 0) ? 1 : 0;
        if (!g_sd_mounted) {
            probe_microsd_internal(0);
        }
    }
    if (g_usb_cfg_dirty && g_sd_mounted) {
        g_usb_cfg_dirty = 0;
        save_persisted_state();
    }
    g_vbus_cache_valid = 0;
    reload_files_and_sd_stats();
    return manual_exit;
}

static void run_bt_xrec_mode(void)
{
    g_ui_screen   = UI_SCR_BT;
    g_ui.bt_state = 0u;
    g_ui.bt_sent  = 0u;
    g_ui.bt_total = 0u;
    g_ui.bt_pct   = 0u;
    copy_cstr(g_ui.bt_peer, (int)sizeof(g_ui.bt_peer), "Phone");
    refresh_telemetry();
    ui_render_screen(UI_SCR_BT, &g_ui);
    backlight_wake(millis());

    wdt_feed();
    (void)bt_rf_init_calibrated(0x53u, 32000000u);
    (void)bt_xrec_start(&g_exfat_vol, g_sd_mounted);

    uint32_t last_ui     = 0u;
    uint32_t prev_rx     = 0u;
    uint32_t prev_tx     = 0u;
    uint8_t  prev_acl    = 0u;
    uint8_t  prev_spp    = 0u;

    uint32_t iter_rx     = 0u;
    uint32_t iter_tx     = 0u;
    /* Home consumed the opening ENTER edge. Keep subsequent queued taps. */
    while (1) {
        wdt_feed();
        bt_xrec_poll();
        recorder_poll();

        /* Keep remote settings when the REC switch hands control to the menu. */
        if (g_ui.cfg.mic_gain != recorder_get_gain() ||
            g_ui.cfg.sound_mode != (uint8_t)recorder_get_dsp_profile() ||
            g_ui.cfg.channels != recorder_get_channels() ||
            g_ui.cfg.vor_enabled != (uint8_t)recorder_get_vor()) {
            g_ui.cfg.mic_gain = recorder_get_gain();
            g_ui.cfg.sound_mode = (uint8_t)recorder_get_dsp_profile();
            g_ui.cfg.channels = recorder_get_channels();
            g_ui.cfg.vor_enabled = (uint8_t)recorder_get_vor();
            save_persisted_state();
        }

        /* Sliding REC ON exits BT immediately so main loop starts recording */
        if (keys_read_rec_switch()) {
            backlight_wake(millis());
            break;
        }

        int bt_exit = 0;
        key_event_t kev;
        while (keys_pop_event(&kev)) {
            if (kev.pressed) {
                backlight_wake(kev.t_ms);
                if (kev.key == KEY_BACK) {
                    bt_exit = 1;
                    break;
                } else if (kev.key == KEY_ENTER) {
                    bt_xrec_open_pairing(60000u);
                }
            }
        }
        if (bt_exit) {
            break;
        }

        uint32_t now = millis();
        int show_bt = ui_poll_power_button(UI_SCR_BT, &g_ui, now);
        if ((uint32_t)(now - last_ui) >= 150u || last_ui == 0u) {
            last_ui = now;
            pmu_poll_charge_manage();

            bt_status_t st;
            bt_xrec_get_status(&st);

            if (st.dev_name[0] != '\0') {
                copy_cstr(g_ui.bt_name, (int)sizeof(g_ui.bt_name), st.dev_name);
            }

            if (st.rx_acl_pkts != prev_rx || st.tx_acl_pkts != prev_tx ||
                st.acl_connected != prev_acl || st.spp_open != prev_spp) {
                prev_rx  = st.rx_acl_pkts;
                prev_tx  = st.tx_acl_pkts;
                prev_acl = st.acl_connected;
                prev_spp = st.spp_open;
                if (g_screen_off != SCREEN_MANUALLY_OFF) {
                    backlight_wake(now);
                }
            }
            backlight_update();

            if (st.xrec_auth && st.tx_acl_pkts > 0u) {
                g_ui.bt_state = 2u;
                g_ui.bt_sent  = (uint16_t)(st.tx_acl_pkts & 0xFFFFu);
                g_ui.bt_total = (uint16_t)(g_ui.file_count ? g_ui.file_count : 1);
                g_ui.bt_pct   = (uint8_t)((st.tx_acl_pkts * 7u) % 101u);
            } else if (st.xrec_auth || st.spp_open || st.acl_connected) {
                g_ui.bt_state = 1u;
            } else {
                g_ui.bt_state = 0u;
            }

            if (st.pair_active && st.pair_pin[0] != '\0') {
                char pmsg[24];
                int p = 0;
                append_str(pmsg, &p, (int)sizeof(pmsg), "PIN ");
                append_str(pmsg, &p, (int)sizeof(pmsg), st.pair_pin);
                pmsg[p] = '\0';
                ui_show_toast(pmsg, UI_COL_CYAN, now, 300u);
            } else {
                (void)ui_poll_toast_expire(now);
            }

            refresh_telemetry();
            if (show_bt && !g_screen_off) {
                ui_render_screen(UI_SCR_BT, &g_ui);
            }
        }

        /* Sleep via WFI when idle (woken immediately by RW-BT IRQ or 1 ms SysTick) */
        if (!keys_event_pending() && !recorder_is_saving()) {
            bt_status_t cur_st;
            bt_xrec_get_status(&cur_st);
            if (cur_st.rx_acl_pkts == iter_rx && cur_st.tx_acl_pkts == iter_tx) {
                OPENFW_WFI();
            } else {
                iter_rx = cur_st.rx_acl_pkts;
                iter_tx = cur_st.tx_acl_pkts;
            }
        }
    }

    keys_ignore_held();
    if (recorder_is_saving()) {
        (void)recorder_stop();
    }
    bt_xrec_stop();
    g_ui.toast_msg[0] = '\0';
    backlight_wake(millis());
    reload_files_and_sd_stats();
}

static void play_boot_animation(void)
{
    /*
     * Crisp ~0.6 s Cassini boot animation on 160x80 landscape TFT (logo and
     * backlight are already lit right after lcd_init() for instant power-on
     * feedback). Skips immediately if REC slide switch is ON or any button is pressed.
     */
    const uint16_t bar_x    = 30u;
    const uint16_t bar_y    = 62u;
    const uint16_t bar_w    = 100u;
    const uint16_t col_bg   = UI_COL_BLACK;
    const uint16_t col_trk  = rgb565(32u, 24u, 20u);
    const uint16_t col_orb  = rgb565(232u, 120u, 64u); /* Cassini planet orange */
    const uint16_t col_moon = rgb565(255u, 212u, 168u); /* Warm peach moon glow */

    uint32_t t_start = millis();
    for (uint32_t frame = 0u; frame < 15u; frame++) {
        wdt_feed();
        if (keys_read_rec_switch() || keys_state((uint32_t *)0) != KEY_NONE ||
            keys_event_pending()) {
            break;
        }
        uint16_t fill_w = (uint16_t)(((frame + 1u) * bar_w) / 15u);
        lcd_fill_rect(bar_x, (uint16_t)(bar_y + 1u), bar_w, 2u, col_trk);
        if (fill_w > 0u) {
            lcd_fill_rect(bar_x, (uint16_t)(bar_y + 1u), fill_w, 2u, col_orb);
        }
        lcd_fill_rect(bar_x, bar_y, bar_w, 1u, col_bg);
        lcd_fill_rect(bar_x, (uint16_t)(bar_y + 3u), bar_w, 1u, col_bg);
        uint16_t dot_x = (uint16_t)(bar_x + ((fill_w >= 4u) ? (fill_w - 4u) : 0u));
        lcd_fill_rect(dot_x, bar_y, 4u, 4u, col_moon);

        uint32_t target_ms = t_start + (frame + 1u) * 35u;
        while ((int32_t)(millis() - target_ms) < 0) {
            wdt_feed();
            if (keys_read_rec_switch() || keys_state((uint32_t *)0) != KEY_NONE ||
                keys_event_pending()) {
                break;
            }
            wfi_sleep_ms(4u);
        }
    }

    backlight_wake(millis());
}

static void adjust_setting(int idx, int delta, uint32_t now_ms)
{
    switch (idx) {
    case 0: /* Mic gain */
    {
        int g = (int)g_ui.cfg.mic_gain + delta;
        if (g < 0) g = 0;
        if (g > 31) g = 31;
        g_ui.cfg.mic_gain = (uint8_t)g;
        recorder_set_gain(g_ui.cfg.mic_gain);
        break;
    }
    case 1: /* Sound: Raw / Clarity / Bypass */
        g_ui.cfg.sound_mode = (uint8_t)((g_ui.cfg.sound_mode + (delta > 0 ? 1u : 2u)) % 3u);
        recorder_set_dsp_profile((audio_dsp_profile_t)g_ui.cfg.sound_mode);
        break;
    case 2: /* Mics: 1ch / 2ch */
        g_ui.cfg.channels = (g_ui.cfg.channels == 1u) ? 2u : 1u;
        recorder_set_channels(g_ui.cfg.channels);
        break;
    case 3: /* Voice trigger */
        g_ui.cfg.vor_enabled ^= 1u;
        recorder_set_vor((int)g_ui.cfg.vor_enabled);
        break;
    case 4: /* Brightness */
    {
        int b = (int)g_ui.cfg.brightness_idx + delta;
        if (b < 0) b = 0;
        if (b > 4) b = 4;
        g_ui.cfg.brightness_idx = (uint8_t)b;
        backlight_wake(now_ms);
        break;
    }
    case 5: /* Screen off */
        g_ui.cfg.screen_off_idx = (uint8_t)((g_ui.cfg.screen_off_idx + (delta > 0 ? 1u : 4u)) % 5u);
        break;
    case 7: /* Skip */
        g_ui.cfg.skip_mode = (uint8_t)((g_ui.cfg.skip_mode + (delta > 0 ? 1u : 2u)) % 3u);
        break;
    default:
        break;
    }
}

static void play_file_at_index(int start_idx)
{
    int idx = start_idx;
    while (idx >= 0 && idx < g_ui.file_count) {
        g_ui_screen = UI_SCR_PLAYER;
        g_ui.play_file     = g_files[idx];
        g_ui.play_has_prev = (idx > 0) ? 1u : 0u;
        g_ui.play_has_next = (idx + 1 < g_ui.file_count) ? 1u : 0u;
        int rc = openfw_play_file_ui(&g_exfat_vol, g_file_dirs[idx], g_files[idx].filename, &g_ui);
        keys_ignore_held();
        g_files[idx].unplayed   = 0u;
        g_files[idx].resume_sec = g_ui.play_file.resume_sec;
        g_ui.last_file          = g_files[idx];
        copy_cstr(g_last_dir, (int)sizeof(g_last_dir), g_file_dirs[idx]);
        g_ui.has_last           = 1;
        g_ui.file_sel           = idx;
        if (g_ui.file_sel < g_ui.file_scroll) {
            g_ui.file_scroll = g_ui.file_sel;
        } else if (g_ui.file_sel >= g_ui.file_scroll + 3) {
            g_ui.file_scroll = g_ui.file_sel - 2;
        }

        if (rc == 1 && idx + 1 < g_ui.file_count) {
            idx++;
        } else if (rc == -1 && idx > 0) {
            idx--;
        } else {
            break;
        }
    }
    save_persisted_state();
}

/*
 * Play dir/name, going through the file list when the file is in it so that
 * holding LEFT/RIGHT can move to the neighbouring recordings.
 */
static void play_named_file(const char *dir, ui_file_entry_t *entry)
{
    for (int i = 0; i < g_ui.file_count; i++) {
        if (str_eq_ci(g_file_dirs[i], dir) && str_eq_ci(g_files[i].filename, entry->filename)) {
            g_files[i].resume_sec = entry->resume_sec;
            play_file_at_index(i);
            entry->resume_sec = g_ui.last_file.resume_sec;
            return;
        }
    }
    g_ui_screen        = UI_SCR_PLAYER;
    g_ui.play_file     = *entry;
    g_ui.play_has_prev = 0u;
    g_ui.play_has_next = 0u;
    (void)openfw_play_file_ui(&g_exfat_vol, dir, entry->filename, &g_ui);
    keys_ignore_held();
    entry->resume_sec = g_ui.play_file.resume_sec;
    save_persisted_state();
}

static void delete_selected_file(uint32_t now_ms)
{
    if (!g_sd_mounted || g_ui.file_sel < 0 || g_ui.file_sel >= g_ui.file_count) {
        return;
    }
    const char *dir  = g_file_dirs[g_ui.file_sel];
    const char *name = g_files[g_ui.file_sel].filename;
    int deleted_was_last = (g_ui.has_last &&
                            str_eq_ci(g_last_dir, dir) &&
                            str_eq_ci(g_ui.last_file.filename, name)) ? 1 : 0;
    int rc = exfat_delete_file(&g_exfat_vol, dir, name);
    if (rc == 0) {
        /* Also remove matching .cwx sidecar if present */
        char cwx_name[32];
        copy_cstr(cwx_name, (int)sizeof(cwx_name), name);
        int len = 0;
        while (cwx_name[len] != '\0') len++;
        if (len >= 5) {
            cwx_name[len - 3] = 'c';
            cwx_name[len - 2] = 'w';
            cwx_name[len - 1] = 'x';
            (void)exfat_delete_file(&g_exfat_vol, dir, cwx_name);
        }
        reload_files_and_sd_stats();
        if (deleted_was_last) {
            g_ui.has_last                 = 0;
            g_ui.last_file.filename[0]    = '\0';
            g_ui.last_file.resume_sec     = 0u;
            g_ui.last_file.duration_sec   = 0u;
            g_ui.last_file.has_transcript = 0u;
            save_persisted_state();
        }
        ui_show_toast("Deleted", UI_COL_GREEN, now_ms, 1500u);
    } else {
        ui_show_toast("Delete failed", UI_COL_AMBER, now_ms, 1500u);
    }
}

static const openfw_exp_api_t k_exp_api = {
    .magic                 = OPENFW_EXP_ABI_MAGIC,
    .arena_base            = 0,
    .arena_size            = OPENFW_EXP_ARENA_SIZE,
    .out_buf               = 0,
    .out_size              = OPENFW_EXP_OUT_SIZE,
    .out_len_ptr           = 0,
    .wdt_feed              = wdt_feed,
    .millis                = millis,
    .us_timer_now          = us_timer_now,
    .udelay                = udelay,
    .mdelay                = mdelay,
    .log_reset             = openfw_log_reset,
    .log_putc              = openfw_log_putc,
    .log_puts              = openfw_log_puts,
    .log_u32               = openfw_log_u32,
    .log_hex               = openfw_log_hex,
    .pmu_read_reg          = pmu_read_reg,
    .pmu_write_reg         = pmu_write_reg,
    .pmu_read_vbat_mv      = pmu_read_vbat_mv,
    .pmu_is_vbus_present   = pmu_is_vbus_present,
    .key_onoff_init        = key_onoff_init,
    .key_onoff_pressed     = key_onoff_pressed,
    .keys_read_adc         = keys_read_adc,
    .keys_decode           = keys_decode,
    .keys_read_rec_switch  = keys_read_rec_switch,
    .keys_read_hp_detect   = keys_read_hp_detect,
    .lcd_set_rotation      = lcd_set_rotation,
    .lcd_get_width         = lcd_get_width,
    .lcd_get_height        = lcd_get_height,
    .lcd_fill_rect         = lcd_fill_rect,
    .lcd_draw_str          = lcd_draw_str,
    .lcd_backlight_set     = lcd_backlight_set,
    .sd_write_file         = openfw_exp_sd_write_file,
    .sd_dump_log           = openfw_exp_sd_dump_log,
    .audio_capture_start   = audio_capture_start,
    .audio_capture_stop    = audio_capture_stop,
    .audio_pop_frame       = audio_pop_frame,
    .audio_get_peak_pct    = audio_get_peak_pct,
    .audio_get_frames      = audio_get_frames,
    .audio_get_overruns    = audio_get_overruns,
    .set_dmac_ch0_irq      = openfw_exp_set_dmac_ch0_irq,
    .bt_rf_init_calibrated = bt_rf_init_calibrated,
    .bt_xrec_start         = bt_xrec_start,
    .bt_xrec_poll          = bt_xrec_poll,
    .bt_xrec_stop          = bt_xrec_stop,
    .bt_xrec_open_pairing  = bt_xrec_open_pairing,
    .bt_xrec_get_status    = bt_xrec_get_status,
};

int openfw_main(void)
{
    wdt_feed();
    /* 3-strike boot-loop guard using AON_BACKUP(7) */
    uint32_t b7 = AON_BACKUP(7u);
    uint32_t boot_strikes = ((b7 & 0xFFFF0000u) == OPENFW_BOOT_STRIKE_MAGIC) ? (b7 & 0xFFFFu) : 0u;
    if (boot_strikes >= OPENFW_BOOT_MAX_STRIKES) {
        AON_BACKUP(7u) = 0u;
        pmu_reboot_to_usb_bootloader();
    }
    AON_BACKUP(7u) = OPENFW_BOOT_STRIKE_MAGIC | ((boot_strikes + 1u) & 0xFFFFu);
    g_openfw_boot_strike_counted = 1u;

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
    uint8_t boot_pmu24 = pmu_read_reg(0x24u);
    uint8_t boot_pmu26 = pmu_read_reg(0x26u);
    pmu_unattended_init();
    wdt_start_reset(32u, 0x2Fu);
    wdt_feed();

    /* Emit early UART0 banner before keys_init() switches PD0 to SARADC */
    uart0_init_115200();
    uart0_puts("\n=== Cassini OpenFW (SL6801 Dual-Core CPU1 + Quad XIP + SRAM FastPath) ===\n");
    uart0_puts("CPU=96MHz XIP=");
    if (g_openfw_xip_mode == OPENFW_XIP_MODE_QUAD_48M) {
        uart0_puts("4W@48M");
    } else if (g_openfw_xip_mode == OPENFW_XIP_MODE_DUAL_48M) {
        uart0_puts("2W@48M");
    } else {
        uart0_puts("1W@24M");
    }
    uart0_puts(" CPU_ID=");
    uart0_put_u32(SYSCTL_CPU_ID);
    uart0_puts(" VBAT_MV=");
    uart0_put_u32(pmu_read_vbat_mv());
    uart0_puts(" PMU_PON=");
    uart0_put_hex(pmu_read_reg(0x27u));
    uart0_puts("/");
    uart0_put_hex(pmu_read_reg(0x28u));
    uart0_puts(" PMU_POFF=");
    uart0_put_hex(pmu_read_reg(0x29u));
    uart0_puts(" PMU_24=");
    uart0_put_hex(boot_pmu24);
    uart0_puts(" PMU_26=");
    uart0_put_hex(boot_pmu26);
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

    ui_settings_defaults(&g_ui.cfg);
    g_ui.home_focus = 1u; /* Default focus: Files */
    g_ui.spec       = &g_spec;
    copy_cstr(g_ui.bt_name, (int)sizeof(g_ui.bt_name), "HW_76");
    apply_cfg_to_recorder();

    /* Overlap SD init + exFAT mount + file/bitmap scan inside ST7735S mandatory 120 ms SLPOUT wait */
    uint64_t t_boot0    = us_timer_now();
    uint64_t t_slpout   = lcd_init_begin();
    uint32_t lcd_b_us   = (uint32_t)(t_slpout - t_boot0);
    uint32_t sd_us      = 0u;
    uint32_t mnt_us     = 0u;
    probe_microsd_mount_only(1, &sd_us, &mnt_us);
    uint64_t t_scan0    = us_timer_now();
    reload_files_and_sd_stats();
    uint32_t scan_us    = (uint32_t)(us_timer_now() - t_scan0);
    uint64_t t_lcdf0    = us_timer_now();
    lcd_init_finish(t_slpout);
    uint32_t lcd_f_us   = (uint32_t)(us_timer_now() - t_lcdf0);
    lcd_set_rotation(1); /* Landscape 160x80 */
    g_cur_bl_pct = 255u;
    /* Light the Cassini splash immediately before loading persisted state */
    uint64_t t_fill0 = us_timer_now();
    lcd_fill_rect(0u, 0u, 160u, 80u, UI_COL_BLACK);
    uint32_t fill_us = (uint32_t)(us_timer_now() - t_fill0);
    openfw_log_puts("LCD_FILL_US=");
    openfw_log_u32(fill_us);
    openfw_log_putc('\n');
    lcd_blit_rgb565((uint16_t)((160u - CASSINI_SPLASH_80X40_WIDTH) / 2u), 14u,
                    CASSINI_SPLASH_80X40_WIDTH, CASSINI_SPLASH_80X40_HEIGHT,
                    CASSINI_SPLASH_80X40);
    backlight_apply(user_bl_pct());

    if (g_sd_mounted) {
        uart0_puts("SD: OK ");
        uart0_put_u32((uint32_t)g_ui.file_count);
        uart0_puts("f\n");
    }
    load_persisted_state();
    openfw_log_puts("BOOT_US LCDB=");
    openfw_log_u32(lcd_b_us);
    openfw_log_puts(" SD=");
    openfw_log_u32(sd_us);
    openfw_log_puts(" MNT=");
    openfw_log_u32(mnt_us);
    openfw_log_puts(" LCDF=");
    openfw_log_u32(lcd_f_us);
    openfw_log_puts(" SCAN=");
    openfw_log_u32(scan_us);
    openfw_log_putc('\n');

    uart0_puts("Switching PD0 to SARADC key ladder & entering main loop...\n");
    keys_init();
    key_onoff_init();
    audio_pa_set(0);
    /* Skip the 2-second boot animation when the Record slider is already ON */
    if (!keys_read_rec_switch()) {
        play_boot_animation();
    }
    /* Discard any residual power-on button edges latched during boot */
    (void)key_onoff_poll();
    /* Boot Cortex-M4F CPU1 as dedicated LVGL/LCD rendering core (auto-fallback on QEMU) */
    int cpu1_ok   = cpu1_ui_start();
    g_cpu1_booted = cpu1_ok;
    openfw_log_puts("CPU1=");
    openfw_log_puts(cpu1_ok ? "ON" : "OFF");
    openfw_log_puts(" WFE=");
    openfw_log_u32(g_cpu1_wfe_ok);
    if (g_cpu1_faulted) {
        openfw_log_puts(" FAULT_PC=");
        openfw_log_hex(g_cpu1_fault_pc);
        openfw_log_puts(" CFSR=");
        openfw_log_hex(g_cpu1_fault_cfsr);
    }
    openfw_log_putc('\n');
    usb_msc_set_audio_cfg_cb(main_usb_audio_cfg_cb);
    openfw_exp_bind_fs(&g_exfat_vol, &g_sd_mounted);
    openfw_exp_init_api(&k_exp_api);
    backlight_wake(millis());

    ui_screen_id_t scr          = UI_SCR_HOME;
    ui_screen_id_t prev_scr     = UI_SCR_HOME;
    g_ui_screen                 = (uint32_t)scr;

    int      rec_active              = 0;
    int      prev_rec_sw             = 0;
    int      prev_vbus               = 0;
    int      prev_vbus_bl            = vbus_present_cached(millis());
    int      usb_probing             = 0;
    int      usb_suppress_until_unpl = 0;
    int      low_batt_cutoff         = 0;
    uint32_t usb_probe_deadline      = 0u;
    uint32_t last_sd_probe           = millis();
    uint32_t last_slow_ms            = 0u;
    uint32_t last_spec_ms            = 0u;
    uint32_t rec_gain_until_ms       = 0u;
    uint32_t rec_clip_l_until_ms     = 0u;
    uint32_t rec_clip_r_until_ms     = 0u;
    uint32_t prev_clip_events        = 0u;
    uint32_t saved_start_ms          = 0u;
    uint32_t enter_press_ms          = 0u;
    int      enter_long_fired        = 0;
    int      del_armed               = 0;
    int      about_armed             = 0;
    key_id_t prev_key                = KEY_NONE;
    key_id_t scroll_repeat_key         = KEY_NONE;
    uint32_t scroll_repeat_at          = 0u;
    int      swallow_until_release   = 0;
    int      dirty                   = 1;

    if (pmu_read_vbat_mv() < 3450u) {
        low_batt_cutoff = 1;
    }

    refresh_telemetry();
    ui_render_screen(scr, &g_ui);
    cpu1_ui_sync(250u);
    openfw_log_puts("UI_FIRST_US=");
    openfw_log_u32(g_ui_last_render_us);
    openfw_log_puts(" LOAD=");
    openfw_log_u32(g_ui_last_load_us);
    openfw_log_puts(" DRAW=");
    openfw_log_u32(g_ui_last_draw_us);
    openfw_log_puts(" BLIT=");
    openfw_log_u32(g_ui_last_blit_us);
    openfw_log_puts(" HEAP=");
    openfw_log_u32(g_ui_heap_used);
    openfw_log_puts(" WFE_W=");
    openfw_log_u32(g_cpu1_wfe_wakes);
    openfw_log_putc('\n');
    dirty = 0;

    while (1) {
        wdt_feed();

        uint32_t now_ms = millis();
        if (now_ms >= 3000u) {
            clear_boot_strikes_once();
        }

        ui_poll_power_button(scr, &g_ui, now_ms);
        if (g_ui_screen == UI_SCR_POWER) {
            recorder_poll();
            wfi_sleep_ms(16u);
            continue;
        }

        /* 2. Record slide switch (overrides any screen immediately) */
        int rec_sw = keys_read_rec_switch();
        if (rec_sw != prev_rec_sw) {
            backlight_wake(now_ms);
            prev_rec_sw      = rec_sw;
            enter_press_ms   = 0u;
            enter_long_fired = 0;
            if (rec_sw) {
                if (scr != UI_SCR_REC && scr != UI_SCR_SAVED) {
                    prev_scr = scr;
                }
                scr         = UI_SCR_REC;
                g_ui_screen = (uint32_t)scr;
                ui_spec_reset_async(&g_spec);
                g_ui.rec_sec            = 0u;
                g_ui.rec_blink          = 1u;
                g_ui.rec_vor_wait       = 0u;
                g_ui.rec_stopped        = 0u;
                g_ui.rec_stop_reason[0] = '\0';
                g_ui.rec_filename[0]    = '\0';
                g_ui.rec_markers        = 0u;
                g_ui.rec_vu_l           = 0u;
                g_ui.rec_vu_r           = 0u;
                g_ui.rec_peak_l         = 0u;
                g_ui.rec_peak_r         = 0u;
                g_ui.rec_clip_l         = 0u;
                g_ui.rec_clip_r         = 0u;
                g_ui.rec_gain_bar       = 0u;
                g_ui.rec_warn[0]        = '\0';
                rec_gain_until_ms       = 0u;
                rec_clip_l_until_ms     = 0u;
                rec_clip_r_until_ms     = 0u;
                prev_clip_events        = 0u;

                apply_cfg_to_recorder();
                if (!g_sd_mounted) {
                    g_ui.sd_ok = 0;
                    rec_active = 0;
                } else if (low_batt_cutoff) {
                    g_ui.rec_stopped = 1u;
                    copy_cstr(g_ui.rec_stop_reason, (int)sizeof(g_ui.rec_stop_reason), "Low battery");
                    rec_active = 0;
                } else if (recorder_start(&g_exfat_vol, g_sd_mounted) == 0) {
                    rec_active = 1;
                    copy_cstr(g_ui.rec_filename, (int)sizeof(g_ui.rec_filename), recorder_get_filename());
                } else {
                    g_ui.rec_stopped = 1u;
                    copy_cstr(g_ui.rec_stop_reason, (int)sizeof(g_ui.rec_stop_reason), "Start failed");
                    rec_active = 0;
                }
                dirty = 1;
            } else {
                /* REC slide switch turned OFF */
                uint32_t rec_s  = recorder_get_seconds();
                uint8_t  rec_ch = recorder_get_channels();
                char     saved_name[32];
                copy_cstr(saved_name, (int)sizeof(saved_name), recorder_get_filename());
                int was_saving = recorder_is_saving();
                int stop_rc    = -1;
                if (rec_active) {
                    stop_rc    = recorder_stop();
                    rec_active = 0;
                }
                uint32_t committed_bytes = recorder_get_committed_bytes();

                if (was_saving && stop_rc == 0 && saved_name[0] != '\0') {
                    /* Incremental file list & free-space update avoids full SD rescan lag */
                    prepend_saved_recording_fast(saved_name, rec_s, rec_ch, committed_bytes);
                    g_ui.saved_sec      = rec_s;
                    g_ui.saved_channels = rec_ch;
                    g_ui.saved_frac_pct = 100u;
                    g_ui.saved_dt       = g_files[0].dt;
                    copy_cstr(g_ui.saved_filename, (int)sizeof(g_ui.saved_filename), saved_name);
                    save_persisted_state();
                    saved_start_ms = now_ms;
                    scr            = UI_SCR_SAVED;
                } else {
                    reload_files_and_sd_stats();
                    if (was_saving || recorder_has_error()) {
                        ui_show_toast("Save failed \xC2\xB7 card error", UI_COL_RED, now_ms, 2500u);
                    }
                    scr = (prev_scr == UI_SCR_REC || prev_scr == UI_SCR_SAVED) ? UI_SCR_HOME : prev_scr;
                }
                g_ui_screen = (uint32_t)scr;
                dirty = 1;
            }
        }

        /* 3. Active recording poll + 20 Hz spectrogram & peak meters */
        if (rec_active) {
            recorder_poll();
            if (recorder_has_error()) {
                (void)recorder_stop();
                rec_active       = 0;
                g_ui.rec_stopped = 1u;
                copy_cstr(g_ui.rec_stop_reason, (int)sizeof(g_ui.rec_stop_reason), "Write error");
                dirty            = 1;
            }
            prev_vbus   = vbus_present_cached(now_ms);
            usb_probing = 0;

            if (!g_screen_off && (uint32_t)(now_ms - last_spec_ms) >= 50u) {
                last_spec_ms = now_ms;
                int16_t pcm_peek[UI_SPEC_FFT_N * 2u];
                uint8_t ch = g_ui.cfg.channels;
                uint32_t nf = audio_dma_peek_pcm(pcm_peek, UI_SPEC_FFT_N, &ch);
                int vor_wait = recorder_is_vor_waiting();

                uint8_t vl = 0u, vr = 0u;
                audio_get_peak_lr_pct(&vl, &vr);
                g_ui.rec_vu_l = vl;
                g_ui.rec_vu_r = (ch == 1u) ? vl : vr;

                if (vl >= g_ui.rec_peak_l) g_ui.rec_peak_l = vl;
                else if (g_ui.rec_peak_l >= 2u) g_ui.rec_peak_l = (uint8_t)(g_ui.rec_peak_l - 2u);
                else g_ui.rec_peak_l = 0u;

                if (g_ui.rec_vu_r >= g_ui.rec_peak_r) g_ui.rec_peak_r = g_ui.rec_vu_r;
                else if (g_ui.rec_peak_r >= 2u) g_ui.rec_peak_r = (uint8_t)(g_ui.rec_peak_r - 2u);
                else g_ui.rec_peak_r = 0u;

                uint32_t clips = recorder_get_clip_events();
                if (vl >= 98u || clips != prev_clip_events) {
                    uint32_t dl = now_ms + 1500u;
                    rec_clip_l_until_ms = dl ? dl : 1u;
                }
                if (g_ui.rec_vu_r >= 98u || clips != prev_clip_events) {
                    uint32_t dl = now_ms + 1500u;
                    rec_clip_r_until_ms = dl ? dl : 1u;
                }
                prev_clip_events = clips;
                if (rec_clip_l_until_ms != 0u && (int32_t)(now_ms - rec_clip_l_until_ms) >= 0) {
                    rec_clip_l_until_ms = 0u;
                }
                if (rec_clip_r_until_ms != 0u && (int32_t)(now_ms - rec_clip_r_until_ms) >= 0) {
                    rec_clip_r_until_ms = 0u;
                }
                g_ui.rec_clip_l  = (rec_clip_l_until_ms != 0u) ? 1u : 0u;
                g_ui.rec_clip_r  = (rec_clip_r_until_ms != 0u) ? 1u : 0u;

                if (g_ui.rec_gain_bar && (int32_t)(now_ms - rec_gain_until_ms) >= 0) {
                    g_ui.rec_gain_bar = 0u;
                    dirty = 1;
                }

                uint32_t rsec = recorder_get_seconds();
                uint8_t  blk  = (uint8_t)((now_ms / 500u) & 1u);
                if (rsec != g_ui.rec_sec || blk != g_ui.rec_blink ||
                    (uint8_t)vor_wait != g_ui.rec_vor_wait) {
                    g_ui.rec_sec      = rsec;
                    g_ui.rec_blink    = blk;
                    g_ui.rec_vor_wait = (uint8_t)vor_wait;
                    dirty = 1;
                }
                if (scr == UI_SCR_REC && g_ui.sd_ok && !g_ui.rec_stopped) {
                    /* Offload 128-pt FFT + spectrogram scroll + live meter/full blit to CPU1 */
                    ui_rec_push_live(&g_ui, (nf > 0u) ? pcm_peek : (const int16_t *)0,
                                     nf, ch, vor_wait, dirty);
                    dirty = 0;
                }
            }
        } else {
            /* Idle: VBUS auto-USB detection & SD hot-plug re-probe */
            int vbus_now       = vbus_present_cached(now_ms);
            int enter_usb_auto = 0;

            if (vbus_now != prev_vbus_bl) {
                prev_vbus_bl = vbus_now;
                backlight_wake(now_ms);
                dirty = 1;
            }

            if (!g_sd_card.initialized &&
                !keys_event_pending() &&
                keys_state((uint32_t *)0) == KEY_NONE &&
                !ui_dispatch_is_animating() &&
                (uint32_t)(now_ms - last_sd_probe) >= 10000u) {
                last_sd_probe = now_ms;
                probe_microsd_internal(0);
                if (g_sd_card.initialized) {
                    dirty = 1;
                }
            }

            if (!vbus_now) {
                prev_vbus               = 0;
                usb_probing             = 0;
                usb_suppress_until_unpl = 0;
            } else if (!usb_suppress_until_unpl) {
#if OPENFW_UNATTENDED_DEV
                enter_usb_auto = 1;
                prev_vbus      = 1;
                usb_probing    = 0;
                (void)prev_vbus;
                (void)usb_probing;
                (void)usb_probe_deadline;
#else
                if (g_sd_card.initialized) {
                    if (!prev_vbus) {
                        prev_vbus          = 1;
                        usb_probing        = 1;
                        usb_probe_deadline = now_ms + 1500u;
                        usb_phy_det_arm();
                    } else if (usb_probing) {
                        if (usb_phy_det_poll_host()) {
                            usb_probing    = 0;
                            enter_usb_auto = 1;
                        } else if ((int32_t)(now_ms - usb_probe_deadline) >= 0) {
                            usb_probing = 0;
                        }
                    }
                }
#endif
            }

            if (enter_usb_auto) {
                int manual = run_usb_msc_mode(1);
                usb_suppress_until_unpl = (manual && pmu_is_vbus_present()) ? 1 : 0;
                g_ui_screen = (uint32_t)scr;
                prev_key    = KEY_NONE;
                dirty       = 1;
                continue;
            }
        }

        /* 4. Key input handling (drain queued debounced edges, then final state pass for holds) */
        for (;;) {
            key_event_t kev;
            int         have       = keys_pop_event(&kev);
            key_id_t    key        = have ? (kev.pressed ? kev.key : KEY_NONE) : keys_state((uint32_t *)0);
            uint32_t    t          = have ? kev.t_ms : millis();
            int         modal_done = 0;

            int browsing = scr == UI_SCR_FILES || (scr == UI_SCR_SETTINGS && !g_ui.set_edit_clock);
            if (!browsing || key != scroll_repeat_key || g_screen_off) {
                scroll_repeat_key = KEY_NONE;
            }
            if (g_screen_off) {
                if (key != KEY_NONE && prev_key == KEY_NONE) {
                    backlight_wake(t);
                    swallow_until_release = 1;
                    dirty = 1;
                }
                prev_key = key;
            } else if (swallow_until_release) {
                if (key == KEY_NONE) {
                    swallow_until_release = 0;
                }
                prev_key = key;
            } else {
                if (key != KEY_NONE) {
                    backlight_wake(t);
                }

                int rising = (key != KEY_NONE && key != prev_key) ? 1 : 0;
                int repeat = 0;
                if (browsing && rising && (key == KEY_LEFT || key == KEY_RIGHT)) {
                    scroll_repeat_key = key;
                    scroll_repeat_at = t + LIST_SCROLL_DELAY_MS;
                } else if (!have && key != KEY_NONE && key == scroll_repeat_key &&
                           (int32_t)(t - scroll_repeat_at) >= 0) {
                    repeat = 1;
                    scroll_repeat_at = t + LIST_SCROLL_REPEAT_MS;
                }
                ui_screen_id_t scr_before = scr;

                switch (scr) {
                case UI_SCR_HOME:
                    if (rising) {
                        if (key == KEY_LEFT) {
                            if (g_ui.home_focus > 0u) {
                                g_ui.home_focus--;
                                dirty = 1;
                            }
                        } else if (key == KEY_RIGHT) {
                            if (g_ui.home_focus < 3u) {
                                g_ui.home_focus++;
                                dirty = 1;
                            }
                        } else if (key == KEY_ENTER) {
                            if (g_ui.home_focus == 0u) {
                                if (g_ui.has_last && g_sd_mounted) {
                                    char dir[sizeof(g_last_dir)];
                                    copy_cstr(dir, (int)sizeof(dir), g_last_dir);
                                    play_named_file(dir, &g_ui.last_file);
                                    g_ui_screen = (uint32_t)scr;
                                    dirty       = 1;
                                    modal_done  = 1;
                                } else {
                                    ui_show_toast("Nothing played yet", UI_COL_AMBER, t, 1500u);
                                    dirty = 1;
                                }
                            } else if (g_ui.home_focus == 1u) {
                                /* Use cached file list immediately (already refreshed on boot,
                                 * recording stop, delete, USB exit, and BT exit) so Files opens
                                 * without synchronous SD I/O lag. */
                                scr         = UI_SCR_FILES;
                                g_ui_screen = (uint32_t)scr;
                                dirty       = 1;
                            } else if (g_ui.home_focus == 2u) {
                                g_ui.set_edit_clock = 0u;
                                scr                 = UI_SCR_SETTINGS;
                                g_ui_screen         = (uint32_t)scr;
                                dirty               = 1;
                            } else {
                                run_bt_xrec_mode();
                                g_ui_screen = (uint32_t)scr;
                                dirty       = 1;
                                modal_done  = 1;
                            }
                        }
                    }
                    break;

                case UI_SCR_REC:
                    if (rising) {
                        if (key == KEY_BACK) {
                            backlight_sleep_now(SCREEN_MANUALLY_OFF);
                        } else if (key == KEY_ENTER && rec_active) {
                            ui_spec_drop_marker_async(&g_spec);
                            g_ui.rec_markers++;
                            dirty = 1;
                        } else if ((key == KEY_VOL_UP || key == KEY_VOL_DOWN) && rec_active) {
                            int g = (int)g_ui.cfg.mic_gain + ((key == KEY_VOL_UP) ? 1 : -1);
                            if (g < 0) g = 0;
                            if (g > 31) g = 31;
                            g_ui.cfg.mic_gain = (uint8_t)g;
                            recorder_set_gain(g_ui.cfg.mic_gain);
                            g_ui.rec_gain_bar = 1u;
                            rec_gain_until_ms = t + 1500u;
                            dirty = 1;
                        }
                    }
                    break;

                case UI_SCR_SAVED:
                    if (rising) {
                        if (key == KEY_ENTER && g_sd_mounted && g_ui.saved_filename[0] != '\0') {
                            ui_file_entry_t saved;
                            saved.dt             = g_ui.saved_dt;
                            saved.duration_sec   = g_ui.saved_sec;
                            saved.resume_sec     = 0u;
                            saved.unplayed       = 0u;
                            saved.has_transcript = 0u;
                            saved.channels       = g_ui.saved_channels ? g_ui.saved_channels : 1u;
                            copy_cstr(saved.filename, (int)sizeof(saved.filename), g_ui.saved_filename);
                            play_named_file("/record", &saved);
                            if (g_ui.has_last && str_eq_ci(g_ui.last_file.filename, saved.filename)) {
                                g_ui.last_file.resume_sec = saved.resume_sec;
                            }
                            scr         = (prev_scr == UI_SCR_REC || prev_scr == UI_SCR_SAVED) ? UI_SCR_HOME : prev_scr;
                            g_ui_screen = (uint32_t)scr;
                            dirty       = 1;
                            modal_done  = 1;
                        } else if (key == KEY_BACK) {
                            scr         = (prev_scr == UI_SCR_REC || prev_scr == UI_SCR_SAVED) ? UI_SCR_HOME : prev_scr;
                            g_ui_screen = (uint32_t)scr;
                            dirty       = 1;
                        }
                    }
                    break;

                case UI_SCR_FILES:
                    if (rising || repeat) {
                        if (key == KEY_BACK) {
                            scr         = UI_SCR_HOME;
                            g_ui_screen = (uint32_t)scr;
                            dirty       = 1;
                        } else if ((key == KEY_LEFT || key == KEY_VOL_UP) && g_ui.file_count > 0) {
                            if (g_ui.file_sel > 0) {
                                g_ui.file_sel--;
                                if (g_ui.file_sel < g_ui.file_scroll) {
                                    g_ui.file_scroll = g_ui.file_sel;
                                }
                                dirty = 1;
                            }
                        } else if ((key == KEY_RIGHT || key == KEY_VOL_DOWN) && g_ui.file_count > 0) {
                            if (g_ui.file_sel + 1 < g_ui.file_count) {
                                g_ui.file_sel++;
                                if (g_ui.file_sel >= g_ui.file_scroll + 3) {
                                    g_ui.file_scroll = g_ui.file_sel - 2;
                                }
                                dirty = 1;
                            }
                        } else if (key == KEY_ENTER && g_ui.file_count > 0) {
                            enter_press_ms   = t ? t : 1u;
                            enter_long_fired = 0;
                        }
                    } else if (enter_press_ms != 0u && !enter_long_fired &&
                               (key == KEY_ENTER || prev_key == KEY_ENTER) &&
                               (uint32_t)(t - enter_press_ms) >= 600u) {
                        enter_long_fired = 1;
                        enter_press_ms   = 0u;
                        del_armed        = key == KEY_NONE;
                        g_ui.del_hold_pct = 0u;
                        g_ui.del_holding  = 0u;
                        scr               = UI_SCR_DELETE;
                        g_ui_screen       = (uint32_t)scr;
                        dirty             = 1;
                    } else if (key != KEY_ENTER && prev_key == KEY_ENTER && enter_press_ms != 0u && !enter_long_fired) {
                        enter_press_ms = 0u;
                        if (g_ui.file_count > 0) {
                            play_file_at_index(g_ui.file_sel);
                            g_ui_screen = (uint32_t)scr;
                            dirty       = 1;
                            modal_done  = 1;
                        }
                    }
                    break;

                case UI_SCR_DELETE:
                    if (key == KEY_NONE) {
                        del_armed = 1;
                        if (g_ui.del_holding || g_ui.del_hold_pct != 0u) {
                            g_ui.del_holding  = 0u;
                            g_ui.del_hold_pct = 0u;
                            enter_press_ms    = 0u;
                            dirty             = 1;
                        }
                    } else if (rising && key == KEY_BACK) {
                        scr         = UI_SCR_FILES;
                        g_ui_screen = (uint32_t)scr;
                        dirty       = 1;
                    } else if (del_armed && key == KEY_ENTER) {
                        if (enter_press_ms == 0u) {
                            enter_press_ms   = t ? t : 1u;
                            g_ui.del_holding = 1u;
                            dirty            = 1;
                        } else {
                            uint32_t held = (uint32_t)(t - enter_press_ms);
                            uint8_t  pct  = (held >= 1200u) ? 100u : (uint8_t)((held * 100u) / 1200u);
                            if (pct != g_ui.del_hold_pct) {
                                g_ui.del_hold_pct = pct;
                                dirty = 1;
                            }
                            if (held >= 1200u) {
                                enter_press_ms    = 0u;
                                g_ui.del_holding  = 0u;
                                g_ui.del_hold_pct = 0u;
                                keys_ignore_held();
                                delete_selected_file(t);
                                scr         = UI_SCR_FILES;
                                g_ui_screen = (uint32_t)scr;
                                dirty       = 1;
                                modal_done  = 1;
                            }
                        }
                    }
                    break;

                case UI_SCR_SETTINGS:
                    if (rising || repeat) {
                        if (g_ui.set_edit_clock != 0u) {
                            if (key == KEY_BACK) {
                                g_ui.set_edit_clock = 0u;
                                dirty = 1;
                            } else if (key == KEY_LEFT || key == KEY_RIGHT) {
                                g_ui.set_edit_clock = (g_ui.set_edit_clock == 1u) ? 2u : 1u;
                                dirty = 1;
                            } else if (key == KEY_VOL_UP || key == KEY_VOL_DOWN) {
                                int d = (key == KEY_VOL_UP) ? 1 : -1;
                                if (g_ui.set_edit_clock == 1u) {
                                    g_ui.set_edit_h = (uint8_t)((g_ui.set_edit_h + (d > 0 ? 1u : 23u)) % 24u);
                                } else {
                                    g_ui.set_edit_m = (uint8_t)((g_ui.set_edit_m + (d > 0 ? 1u : 59u)) % 60u);
                                }
                                dirty = 1;
                            } else if (key == KEY_ENTER) {
                                rtc_datetime_t dt;
                                rtc_get(&dt);
                                sanitize_rtc(&dt);
                                dt.hour   = g_ui.set_edit_h;
                                dt.minute = g_ui.set_edit_m;
                                dt.second = 0u;
                                rtc_set(&dt);
                                g_ui.set_edit_clock = 0u;
                                ui_show_toast("Clock saved", UI_COL_GREEN, t, 1200u);
                                dirty = 1;
                            }
                        } else {
                            if (key == KEY_BACK) {
                                save_persisted_state();
                                scr         = UI_SCR_HOME;
                                g_ui_screen = (uint32_t)scr;
                                dirty       = 1;
                            } else if (key == KEY_LEFT) {
                                if (g_ui.set_sel > 0) {
                                    g_ui.set_sel--;
                                    if (g_ui.set_sel < g_ui.set_scroll) {
                                        g_ui.set_scroll = g_ui.set_sel;
                                    }
                                    dirty = 1;
                                }
                            } else if (key == KEY_RIGHT) {
                                if (g_ui.set_sel < 9) {
                                    g_ui.set_sel++;
                                    if (g_ui.set_sel >= g_ui.set_scroll + 3) {
                                        g_ui.set_scroll = g_ui.set_sel - 2;
                                    }
                                    dirty = 1;
                                }
                            } else if (key == KEY_VOL_UP) {
                                adjust_setting(g_ui.set_sel, 1, t);
                                dirty = 1;
                            } else if (key == KEY_VOL_DOWN) {
                                adjust_setting(g_ui.set_sel, -1, t);
                                dirty = 1;
                            } else if (key == KEY_ENTER) {
                                if (g_ui.set_sel == 6) {
                                    refresh_telemetry();
                                    g_ui.set_edit_h     = g_ui.now.hour;
                                    g_ui.set_edit_m     = g_ui.now.minute;
                                    g_ui.set_edit_clock = 1u;
                                    dirty = 1;
                                } else if (g_ui.set_sel == 8) {
                                    if (!pmu_is_vbus_present()) {
                                        ui_show_toast("Plug in USB cable", UI_COL_AMBER, t, 1500u);
                                        dirty = 1;
                                    } else {
                                        save_persisted_state();
                                        int manual = run_usb_msc_mode(1);
                                        usb_suppress_until_unpl = (manual && pmu_is_vbus_present()) ? 1 : 0;
                                        g_ui_screen = (uint32_t)scr;
                                        dirty       = 1;
                                        modal_done  = 1;
                                    }
                                } else if (g_ui.set_sel == 9) {
                                    enter_press_ms = 0u;
                                    about_armed    = 0;
                                    scr            = UI_SCR_ABOUT;
                                    g_ui_screen    = (uint32_t)scr;
                                    dirty          = 1;
                                } else {
                                    adjust_setting(g_ui.set_sel, 1, t);
                                    dirty = 1;
                                }
                            }
                        }
                    }
                    break;

                case UI_SCR_ABOUT:
                    if (key == KEY_NONE) {
                        about_armed    = 1;
                        enter_press_ms = 0u;
                    } else if (rising && key == KEY_BACK) {
                        enter_press_ms = 0u;
                        scr            = UI_SCR_SETTINGS;
                        g_ui_screen    = (uint32_t)scr;
                        dirty          = 1;
                    } else if (about_armed && key == KEY_ENTER) {
                        if (enter_press_ms == 0u) {
                            enter_press_ms = t ? t : 1u;
                        } else if ((int32_t)(t - enter_press_ms) >= 600) {
                            (void)recorder_stop();
                            save_persisted_state();
                            (void)sd_flush(&g_sd_card);
                            ui_show_toast("Entering update mode", UI_COL_CYAN, t, 1000u);
                            ui_render_screen(UI_SCR_ABOUT, &g_ui);
                            cpu1_ui_sync(100u);
                            mdelay(150u);
                            pmu_reboot_to_usb_bootloader();
                        }
                    } else {
                        enter_press_ms = 0u;
                    }
                    break;

                default:
                    break;
                }

                if (scr != scr_before) {
                    enter_press_ms   = 0u;
                    enter_long_fired = 0;
                }

                if (modal_done) {
                    prev_key = KEY_NONE;
                    break;
                }
                prev_key = key;
            }

            if (!have) {
                break;
            }
        }

        /* 5. Saved-card 8-second countdown (5% / ~400 ms steps) */
        if (scr == UI_SCR_SAVED) {
            uint32_t elapsed = (uint32_t)(now_ms - saved_start_ms);
            if (elapsed >= 8000u) {
                scr              = (prev_scr == UI_SCR_REC || prev_scr == UI_SCR_SAVED) ? UI_SCR_HOME : prev_scr;
                g_ui_screen      = (uint32_t)scr;
                enter_press_ms   = 0u;
                enter_long_fired = 0;
                dirty            = 1;
            } else {
                uint32_t rem  = 8000u - elapsed;
                uint8_t  frac = (uint8_t)(((rem + 399u) / 400u) * 5u);
                if (frac > 100u) {
                    frac = 100u;
                }
                if (frac != g_ui.saved_frac_pct) {
                    g_ui.saved_frac_pct = frac;
                    dirty = 1;
                }
            }
        }

        /* 6. Toast expiration */
        if (ui_poll_toast_expire(now_ms)) {
            dirty = 1;
        }

        /* 7. Periodic 2 Hz telemetry & battery cutoff supervision */
        if ((uint32_t)(now_ms - last_slow_ms) >= 500u || last_slow_ms == 0u) {
            last_slow_ms = now_ms;
            pmu_poll_charge_manage();
            uint8_t prev_min = g_ui.now.minute;
            uint8_t prev_bat = g_ui.batt_pct;
            int     prev_chg = g_ui.charging;
            int     prev_hp  = g_ui.hp_plugged;
            refresh_telemetry();

            if (g_ui.batt_mv < 3450u) {
                low_batt_cutoff = 1;
                if (rec_active) {
                    (void)recorder_stop();
                    rec_active       = 0;
                    g_ui.rec_stopped = 1u;
                    copy_cstr(g_ui.rec_stop_reason, (int)sizeof(g_ui.rec_stop_reason), "Low battery");
                    dirty = 1;
                }
            } else if (g_ui.batt_mv >= 3550u) {
                low_batt_cutoff = 0;
            }

            if (g_ui.now.minute != prev_min || g_ui.batt_pct != prev_bat ||
                g_ui.charging != prev_chg || g_ui.hp_plugged != prev_hp) {
                dirty = 1;
            }
        }

        backlight_update();

        if (dirty && !g_screen_off) {
            ui_render_screen(scr, &g_ui);
            dirty = 0;
        } else if (!g_screen_off) {
            /* Keep LVGL timers & animations ticking (no-op when CPU1 drives UI) */
            ui_dispatch_tick();
        }

        wfi_sleep_ms(cpu1_ui_is_active() ? 16u : (ui_dispatch_is_animating() ? 2u : 8u));
    }

    return 0;
}
