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

typedef struct {
    ui_file_entry_t *items;
    char           (*dirs)[12];
    int              count;
    int              total_seen;
    int              max_items;
    const char      *dir_path;
} menu_list_ctx_t;

static sd_card_t        g_sd_card;
static blkdev_t         g_sd_blkdev;
static blkdev_t         g_null_blkdev = { 0, 0, 0u, 0 };
static exfat_vol_t      g_exfat_vol;
static int              g_sd_mounted = 0;
static int              g_boot_strikes_cleared = 0;

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
    g_screen_off       = SCREEN_AWAKE;
    backlight_apply(user_bl_pct());
}

static void backlight_sleep_now(enum screen_sleep_state reason)
{
    g_screen_off = reason;
    backlight_apply(0u);
    ui_render_screen(UI_SCR_OFF, &g_ui);
}

int ui_poll_power_button(ui_screen_id_t screen, ui_model_t *model, uint32_t now)
{
    /* USB and audio service loops can run many times in a millisecond.
     * The PMU latches edges until acknowledged. */
    static uint32_t last_poll;
    if (now - last_poll >= 16u) {
        last_poll = now;
        uint8_t progress = 0;
        power_button_action_t action = power_button_update(key_onoff_poll(), now,
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
            }
            (void)recorder_stop();
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

static void refresh_telemetry(void)
{
    rtc_get(&g_ui.now);
    sanitize_rtc(&g_ui.now);
    uint32_t mv = pmu_read_vbat_mv();
    pmu_chg_state_t chg = pmu_get_charge_state();
    g_ui.batt_mv      = (uint16_t)mv;
    g_ui.batt_pct     = vbat_mv_to_pct(mv);
    g_ui.charging     = (chg == PMU_CHG_CHARGING) ? 1 : 0;
    g_ui.vbus_present = pmu_is_vbus_present() ? 1u : 0u;
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
                g_ui.last_file.dt             = g_ui.now;
                g_ui.last_file.duration_sec   = (ent.size > 44u) ? ((uint32_t)(ent.size - 44u) / 32000u) : 0u;
                g_ui.last_file.resume_sec     = (last_resume < g_ui.last_file.duration_sec) ? last_resume : 0u;
                g_ui.last_file.unplayed       = 0u;
                g_ui.last_file.has_transcript = 0u;
                g_ui.last_file.channels       = 1u;
                copy_cstr(g_last_dir, (int)sizeof(g_last_dir), "/record");
                g_ui.has_last = 1;
            }
        }
    }
    refresh_telemetry();
}

static void wait_key_release(key_id_t target)
{
    while (keys_state((uint32_t *)0) == target) {
        wdt_feed();
        mdelay(20u);
    }
    keys_flush_events();
}

static int menu_collect_cb(const exfat_entry_t *entry, void *user)
{
    menu_list_ctx_t *ctx = (menu_list_ctx_t *)user;
    if (entry->is_dir) {
        return 0;
    }
    if (!name_ends_with_ci(entry->name, ".wav")) {
        return 0;
    }
    /* Ring-buffer the newest max_items entries so recording #25+ is never lost */
    int idx = ctx->total_seen % ctx->max_items;
    ctx->total_seen++;
    if (ctx->count < ctx->max_items) {
        ctx->count++;
    }
    ui_file_entry_t *dst = &ctx->items[idx];
    copy_cstr(ctx->dirs[idx], 12, ctx->dir_path);
    copy_cstr(dst->filename, (int)sizeof(dst->filename), entry->name);
    dst->dt             = g_ui.now;
    dst->resume_sec     = 0u;
    dst->unplayed       = 1u;
    dst->has_transcript = 0u;
    dst->channels       = 1u;
    dst->duration_sec   = (entry->size > 44u) ? ((uint32_t)(entry->size - 44u) / 32000u) : 0u;

    /* Quick 64-byte WAV header peek for exact duration & channel count */
    uint8_t hdr[64];
    uint32_t got = 0u;
    if (entry->size >= 44u &&
        exfat_read_file(&g_exfat_vol, entry, 0u, hdr, 64u, &got) == 0 && got >= 44u) {
        wav_stream_info_t wi;
        if (wav_parse_stream(hdr, got, &wi) == 0) {
            dst->duration_sec = (wi.duration_ms + 500u) / 1000u;
            dst->channels     = (uint8_t)(wi.channels ? wi.channels : 1u);
        }
    }
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

    (void)exfat_stat_fs(&g_exfat_vol, &g_sd_free_mb, &g_sd_total_mb);

    static ui_file_entry_t ring_files[MENU_MAX_FILES];
    static char            ring_dirs[MENU_MAX_FILES][12];
    menu_list_ctx_t ctx;
    ctx.items      = ring_files;
    ctx.dirs       = ring_dirs;
    ctx.count      = 0;
    ctx.total_seen = 0;
    ctx.max_items  = MENU_MAX_FILES;

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

    /* Unwrap ring buffer in reverse chronological order (newest first) */
    for (int i = 0; i < ctx.count; i++) {
        int src_idx = (ctx.total_seen - 1 - i) % ctx.max_items;
        if (src_idx < 0) {
            src_idx += ctx.max_items;
        }
        g_files[i] = ring_files[src_idx];
        copy_cstr(g_file_dirs[i], 12, ring_dirs[src_idx]);
    }

    /* Check matching .cwx transcript sidecars */
    for (int i = 0; i < ctx.count; i++) {
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
                g_files[i].has_transcript = 1u;
            }
        }
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

static void probe_microsd_internal(int log_uart)
{
    g_sd_mounted = 0;
    if (sd_init(&g_sd_card) != 0) {
        if (log_uart) {
            uart0_puts("SD: NONE (sd_init failed)\n");
        }
        reload_files_and_sd_stats();
        return;
    }

    g_sd_blkdev = sd_as_blkdev(&g_sd_card);
    if (exfat_mount(&g_exfat_vol, &g_sd_blkdev) != 0) {
        if (log_uart) {
            uart0_puts("SD: RAW (card ready, no exFAT partition)\n");
        }
        reload_files_and_sd_stats();
        return;
    }

    g_sd_mounted = 1;
    reload_files_and_sd_stats();
    if (log_uart) {
        uart0_puts("SD: OK ");
        uart0_put_u32((uint32_t)g_ui.file_count);
        uart0_puts("f\n");
    }
}

static int main_usb_audio_cfg_cb(int set, uint8_t cfg[4], int16_t *rms_x10, uint16_t *clips)
{
    if (set) {
        if (cfg[0] != 0xFFu) {
            recorder_set_dsp_profile((audio_dsp_profile_t)cfg[0]);
            g_ui.cfg.sound_mode = (uint8_t)recorder_get_dsp_profile();
        }
        if (cfg[1] != 0xFFu) {
            recorder_set_gain(cfg[1]);
            g_ui.cfg.mic_gain = recorder_get_gain();
        }
        if (cfg[2] != 0xFFu) {
            recorder_set_channels(cfg[2]);
            g_ui.cfg.channels = recorder_get_channels();
        }
        if (cfg[3] != 0xFFu) {
            recorder_set_vor((int)cfg[3]);
            g_ui.cfg.vor_enabled = (uint8_t)(recorder_get_vor() ? 1u : 0u);
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
    g_ui_screen        = UI_SCR_USB;
    g_ui.usb_read_act  = 0u;
    g_ui.usb_write_act = 0u;
    refresh_telemetry();
    ui_render_screen(UI_SCR_USB, &g_ui);

    usb_msc_set_diag_info((uint8_t)(g_sd_mounted ? 1u : 0u),
                          (uint8_t)(recorder_is_saving() ? 1u : 0u),
                          recorder_get_gain());

    blkdev_t *msc_dev = g_sd_card.initialized ? &g_sd_blkdev : &g_null_blkdev;
    if (usb_msc_start(msc_dev) != 0) {
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
    int      exit_armed      = 0;
    int      manual_exit     = 0;

    uint32_t last_render = 0u;
    keys_flush_events();
    while (1) {
        wdt_feed();
        for (int burst = 0; burst < 32; burst++) {
            usb_msc_poll();
        }

        uint32_t now = millis();
        int show_usb = ui_poll_power_button(UI_SCR_USB, &g_ui, now);

        key_event_t kev;
        while (keys_pop_event(&kev)) {
            if (kev.pressed) {
                backlight_wake(kev.t_ms);
                if (exit_armed && kev.key == KEY_BACK) {
                    back_press_ms = kev.t_ms ? kev.t_ms : 1u;
                } else {
                    back_press_ms = 0u;
                }
            } else {
                if (kev.key == KEY_BACK) {
                    back_press_ms = 0u;
                }
                if (keys_state((uint32_t *)0) == KEY_NONE) {
                    exit_armed = 1;
                }
            }
        }
        {
            uint32_t st_since = 0u;
            key_id_t st_k = keys_state(&st_since);
            if (st_k == KEY_NONE) {
                exit_armed    = 1;
                back_press_ms = 0u;
            } else if (exit_armed && st_k == KEY_BACK) {
                if (back_press_ms == 0u) {
                    back_press_ms = st_since ? st_since : (now ? now : 1u);
                }
                if ((uint32_t)(now - back_press_ms) >= 600u) {
                    manual_exit = 1;
                    break;
                }
            } else {
                back_press_ms = 0u;
            }
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
                if (frame != 0u) {
                    if (seen_sof && frame == last_frame) {
                        stale_ticks++;
                        if (stale_ticks >= 8u) { /* >= 800 ms without SOF */
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
            usb_msc_get_bytes(&g_ui.usb_rx_bytes, &g_ui.usb_tx_bytes);
            if (rxp > 0u || txp > 0u || now >= 3000u) {
                clear_boot_strikes_once();
            }
            if ((txp != prev_txp || rxp != prev_rxp) && g_screen_off != SCREEN_MANUALLY_OFF) {
                backlight_wake(now);
            }
            if (txp != prev_txp) {
                prev_txp       = txp;
                rd_blink_until = now + 250u;
            }
            if (rxp != prev_rxp) {
                prev_rxp       = rxp;
                wr_blink_until = now + 250u;
            }
            backlight_update();

            g_ui.usb_read_act  = ((int32_t)(now - rd_blink_until) < 0) ? 1u : 0u;
            g_ui.usb_write_act = ((int32_t)(now - wr_blink_until) < 0) ? 1u : 0u;
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

    usb_msc_stop();
    (void)sd_flush(&g_sd_card);
    wait_key_release(KEY_BACK);
    backlight_wake(millis());

    /* Remount exFAT because USB host may have written or deleted files */
    if (g_sd_card.initialized) {
        g_sd_mounted = (exfat_mount(&g_exfat_vol, &g_sd_blkdev) == 0) ? 1 : 0;
    }
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
    int      exit_armed  = 0;

    keys_flush_events();
    while (1) {
        wdt_feed();
        bt_xrec_poll();
        recorder_poll();

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
                if (exit_armed && kev.key == KEY_BACK) {
                    bt_exit = 1;
                    break;
                } else if (exit_armed && kev.key == KEY_ENTER) {
                    bt_xrec_open_pairing(60000u);
                }
            } else if (keys_state((uint32_t *)0) == KEY_NONE) {
                exit_armed = 1;
            }
        }
        if (bt_exit) {
            break;
        }
        if (!exit_armed && keys_state((uint32_t *)0) == KEY_NONE) {
            exit_armed = 1;
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
    }

    if (recorder_is_saving()) {
        (void)recorder_stop();
    }
    bt_xrec_stop();
    g_ui.toast_msg[0] = '\0';
    wait_key_release(KEY_BACK);
    backlight_wake(millis());
    reload_files_and_sd_stats();
}

static void play_boot_animation(void)
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
    const uint16_t col_bg   = UI_COL_BLACK;
    const uint16_t col_trk  = rgb565(32u, 24u, 20u);
    const uint16_t col_orb  = rgb565(232u, 120u, 64u); /* Cassini planet orange */
    const uint16_t col_moon = rgb565(255u, 212u, 168u); /* Warm peach moon glow */

    lcd_set_rotation(1);
    lcd_fill_rect(0u, 0u, 160u, 80u, col_bg);

    uint16_t prev_logo_y = 20u;
    uint32_t t_start     = millis();

    for (uint32_t frame = 0u; frame < 50u; frame++) {
        wdt_feed();
        if (keys_read_rec_switch() || keys_decode(keys_read_adc()) != KEY_NONE ||
            keys_event_pending()) {
            break;
        }

        uint8_t  alpha  = 255u;
        uint16_t logo_y = 14u;

        if (frame < 16u) {
            uint32_t inv = 15u - frame;
            logo_y = (uint16_t)(14u + (6u * inv * inv) / 225u);
            alpha  = (uint8_t)((frame * 255u) / 15u);
            uint8_t bl = (uint8_t)(((uint32_t)user_bl_pct() * (frame + 1u)) / 16u);
            if (bl < 5u) {
                bl = 5u;
            }
            backlight_apply(bl);
        } else if (frame >= 43u) {
            uint32_t rem = 49u - frame;
            alpha = (uint8_t)((rem * 255u) / 6u);
        } else {
            backlight_apply(user_bl_pct());
        }

        if (logo_y < prev_logo_y) {
            lcd_fill_rect(logo_x, (uint16_t)(logo_y + CASSINI_SPLASH_80X40_HEIGHT),
                          CASSINI_SPLASH_80X40_WIDTH, (uint16_t)(prev_logo_y - logo_y), col_bg);
        }
        prev_logo_y = logo_y;

        for (uint32_t s = 0u; s < 8u; s++) {
            uint32_t ph = (frame + s * 5u) & 15u;
            uint32_t lum = (ph < 8u) ? (40u + ph * 18u) : (40u + (15u - ph) * 18u);
            lum = (lum * (uint32_t)alpha) >> 8;
            uint16_t sc = rgb565((uint8_t)lum, (uint8_t)((lum * 220u) >> 8), (uint8_t)((lum * 190u) >> 8));
            lcd_fill_rect(star_x[s], star_y[s], 2u, 2u, sc);
        }

        if (frame <= 16u || frame >= 43u) {
            lcd_blit_rgb565_alpha(logo_x, logo_y,
                                  CASSINI_SPLASH_80X40_WIDTH, CASSINI_SPLASH_80X40_HEIGHT,
                                  CASSINI_SPLASH_80X40, alpha);
        }

        if (frame >= 16u) {
            uint16_t fill_w = bar_w;
            if (frame < 38u) {
                uint32_t p = frame - 16u;
                fill_w = (uint16_t)(((p + 1u) * bar_w) / 22u);
            }
            if (alpha < 255u) {
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
                lcd_fill_rect(bar_x, bar_y, bar_w, 1u, col_bg);
                lcd_fill_rect(bar_x, (uint16_t)(bar_y + 3u), bar_w, 1u, col_bg);
                uint16_t dot_x = (uint16_t)(bar_x + ((fill_w >= 4u) ? (fill_w - 4u) : 0u));
                lcd_fill_rect(dot_x, bar_y, 4u, 4u, col_moon);
            }
        }

        uint32_t target_ms = t_start + (frame + 1u) * 40u;
        while ((int32_t)(millis() - target_ms) < 0) {
            wdt_feed();
            if (keys_read_rec_switch() || keys_decode(keys_read_adc()) != KEY_NONE) {
                break;
            }
            wfi_sleep_ms(4u);
        }
    }

    lcd_fill_rect(0u, 0u, 160u, 80u, UI_COL_BG);
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
        ui_show_toast("Deleted", UI_COL_GREEN, now_ms, 1500u);
    } else {
        ui_show_toast("Delete failed", UI_COL_AMBER, now_ms, 1500u);
    }
}

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
    uart0_puts(" PMU_PON=");
    uart0_put_hex(pmu_read_reg(0x27u));
    uart0_puts("/");
    uart0_put_hex(pmu_read_reg(0x28u));
    uart0_puts(" PMU_POFF=");
    uart0_put_hex(pmu_read_reg(0x29u));
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

    lcd_init();
    lcd_set_rotation(1); /* Landscape 160x80 */
    lcd_fill_rect(0u, 0u, 160u, 80u, UI_COL_BLACK);
    g_cur_bl_pct = 255u;
    backlight_apply(0u); /* Backlight ramps up smoothly inside play_boot_animation() */

    probe_microsd_internal(1);
    load_persisted_state();

    uart0_puts("Switching PD0 to SARADC key ladder & entering main loop...\n");
    keys_init();
    key_onoff_init();
    audio_pa_set(0);
    play_boot_animation();
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

    ui_screen_id_t scr          = UI_SCR_HOME;
    ui_screen_id_t prev_scr     = UI_SCR_HOME;
    g_ui_screen                 = (uint32_t)scr;

    int      rec_active              = 0;
    int      prev_rec_sw             = 0;
    int      prev_vbus               = 0;
    int      prev_vbus_bl            = pmu_is_vbus_present();
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
    key_id_t prev_key                = KEY_NONE;
    int      swallow_until_release   = 0;
    int      dirty                   = 1;

    if (pmu_read_vbat_mv() < 3450u) {
        low_batt_cutoff = 1;
    }

    refresh_telemetry();
    ui_render_screen(scr, &g_ui);
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
            prev_rec_sw = rec_sw;
            if (rec_sw) {
                if (scr != UI_SCR_REC && scr != UI_SCR_SAVED) {
                    prev_scr = scr;
                }
                scr         = UI_SCR_REC;
                g_ui_screen = (uint32_t)scr;
                ui_spec_reset(&g_spec);
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
                reload_files_and_sd_stats();

                if (was_saving && stop_rc == 0 && saved_name[0] != '\0') {
                    g_ui.saved_sec      = rec_s;
                    g_ui.saved_channels = rec_ch;
                    g_ui.saved_frac_pct = 100u;
                    g_ui.saved_dt       = g_ui.now;
                    copy_cstr(g_ui.saved_filename, (int)sizeof(g_ui.saved_filename), saved_name);
                    int match_idx = -1;
                    for (int i = 0; i < g_ui.file_count; i++) {
                        if (str_eq_ci(g_files[i].filename, saved_name)) {
                            match_idx = i;
                            break;
                        }
                    }
                    if (match_idx >= 0) {
                        g_ui.last_file = g_files[match_idx];
                        copy_cstr(g_last_dir, (int)sizeof(g_last_dir), g_file_dirs[match_idx]);
                        g_ui.has_last = 1;
                    }
                    save_persisted_state();
                    saved_start_ms = now_ms;
                    scr            = UI_SCR_SAVED;
                } else {
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
                copy_cstr(g_ui.rec_stop_reason, (int)sizeof(g_ui.rec_stop_reason), "Card full / error");
                dirty            = 1;
            }
            prev_vbus   = pmu_is_vbus_present();
            usb_probing = 0;

            if ((uint32_t)(now_ms - last_spec_ms) >= 50u) {
                last_spec_ms = now_ms;
                int16_t pcm_peek[UI_SPEC_FFT_N * 2u];
                uint8_t ch = g_ui.cfg.channels;
                uint32_t nf = audio_dma_peek_pcm(pcm_peek, UI_SPEC_FFT_N, &ch);
                int vor_wait = recorder_is_vor_waiting();
                ui_spec_push_pcm(&g_spec, (nf > 0u) ? pcm_peek : (const int16_t *)0, nf, ch, vor_wait);

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
                    rec_clip_l_until_ms = now_ms + 1500u;
                }
                if (g_ui.rec_vu_r >= 98u || clips != prev_clip_events) {
                    rec_clip_r_until_ms = now_ms + 1500u;
                }
                prev_clip_events = clips;
                g_ui.rec_clip_l  = ((int32_t)(now_ms - rec_clip_l_until_ms) < 0) ? 1u : 0u;
                g_ui.rec_clip_r  = ((int32_t)(now_ms - rec_clip_r_until_ms) < 0) ? 1u : 0u;

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
                } else if (!g_screen_off && !dirty && scr == UI_SCR_REC && g_ui.sd_ok && !g_ui.rec_stopped) {
                    /* Fast 20 Hz partial update: spectrogram + L/R vertical meters via LVGL */
                    ui_rec_update_live(&g_ui);
                }
            }
        } else {
            /* Idle: VBUS auto-USB detection & SD hot-plug re-probe */
            int vbus_now       = pmu_is_vbus_present();
            int enter_usb_auto = 0;

            if (vbus_now != prev_vbus_bl) {
                prev_vbus_bl = vbus_now;
                backlight_wake(now_ms);
                dirty = 1;
            }

            if (!g_sd_card.initialized && (uint32_t)(now_ms - last_sd_probe) >= 2000u) {
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
                keys_flush_events();
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
                                reload_files_and_sd_stats();
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
                            ui_spec_drop_marker(&g_spec);
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
                            ui_file_entry_t saved = g_ui.last_file;
                            copy_cstr(saved.filename, (int)sizeof(saved.filename), g_ui.saved_filename);
                            play_named_file("/record", &saved);
                            g_ui.last_file.resume_sec = saved.resume_sec;
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
                    if (rising) {
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
                    } else if (key == KEY_ENTER && enter_press_ms != 0u && !enter_long_fired) {
                        if ((uint32_t)(t - enter_press_ms) >= 600u) {
                            enter_long_fired = 1;
                            enter_press_ms   = 0u;
                            del_armed        = 0;
                            g_ui.del_hold_pct = 0u;
                            g_ui.del_holding  = 0u;
                            scr               = UI_SCR_DELETE;
                            g_ui_screen       = (uint32_t)scr;
                            dirty             = 1;
                        }
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
                                delete_selected_file(t);
                                scr         = UI_SCR_FILES;
                                g_ui_screen = (uint32_t)scr;
                                wait_key_release(KEY_ENTER);
                                dirty       = 1;
                                modal_done  = 1;
                            }
                        }
                    }
                    break;

                case UI_SCR_SETTINGS:
                    if (rising) {
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
                    if (rising && key == KEY_BACK) {
                        enter_press_ms = 0u;
                        scr            = UI_SCR_SETTINGS;
                        g_ui_screen    = (uint32_t)scr;
                        dirty          = 1;
                    } else if (key == KEY_ENTER) {
                        if (enter_press_ms == 0u) {
                            enter_press_ms = t ? t : 1u;
                        } else if ((uint32_t)(t - enter_press_ms) >= 600u) {
                            (void)recorder_stop();
                            ui_show_toast("Entering update mode", UI_COL_CYAN, t, 1000u);
                            ui_render_screen(UI_SCR_ABOUT, &g_ui);
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

                if (modal_done) {
                    keys_flush_events();
                    prev_key = KEY_NONE;
                    break;
                }
                prev_key = key;
            }

            if (!have) {
                break;
            }
        }

        /* 5. Saved-card 8-second countdown */
        if (scr == UI_SCR_SAVED) {
            uint32_t elapsed = (uint32_t)(now_ms - saved_start_ms);
            if (elapsed >= 8000u) {
                scr         = (prev_scr == UI_SCR_REC || prev_scr == UI_SCR_SAVED) ? UI_SCR_HOME : prev_scr;
                g_ui_screen = (uint32_t)scr;
                dirty       = 1;
            } else {
                uint8_t frac = (uint8_t)(((8000u - elapsed) * 100u) / 8000u);
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
            /* Keep LVGL timers & animations ticking smoothly (~60 Hz) */
            ui_port_task_handler();
        }

        wfi_sleep_ms(ui_port_is_animating() ? 2u : 16u);
    }

    return 0;
}
