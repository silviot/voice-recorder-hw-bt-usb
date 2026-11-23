/*
 * Cassini OpenFW — 160x80 Landscape Anti-Aliased UI Contract
 *
 * Defines the RGB565 palette, UI icon identifiers, live 128-pt FFT Mel
 * spectrogram, shared formatting helpers, and the hardware-agnostic UI
 * state model (ui_model_t) bound onto LVGL v9 / EEZ Studio screens.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_UI_H
#define OPENFW_UI_H

#include <stdint.h>
#include "openfw.h"
#include "openfw_cwx.h"

/* =========================================================================
 * 14-Color RGB565 Palette (exact hardware panel values)
 * ========================================================================= */
#define UI_COL_BG       0x0042u /* #070B14 Background everywhere */
#define UI_COL_BAR      0x10C4u /* #111827 Status bar, transcript footer */
#define UI_COL_TILE     0x10C5u /* #131B2B Unfocused tiles, quiet pills */
#define UI_COL_PILL     0x1A71u /* #1F4E8C Focus and selection pill */
#define UI_COL_PILL_HI  0x2B36u /* #2A64B0 Highlighted pill */
#define UI_COL_OFF      0x1906u /* #1A2232 Tracks, unlit meter segments */
#define UI_COL_FAINT    0x320Au /* #344055 Prev transcript line, idle arrows */
#define UI_COL_MUTE     0x5B50u /* #5D6B82 Tertiary text, upcoming words */
#define UI_COL_DIM      0x9D99u /* #9CB0CE Secondary text, hint labels */
#define UI_COL_TEXT     0xFFFFu /* #FFFFFF Primary text, hint keys */
#define UI_COL_BLACK    0x0000u /* #000000 Text on bright pills/bars */
#define UI_COL_CYAN     0x465Fu /* #42C8FF Playback, values, S1 */
#define UI_COL_RED      0xF9C7u /* #FF3B3B Recording, delete, overload */
#define UI_COL_DRED     0x58A3u /* #5A1418 Not-recording storage fault bar */
#define UI_COL_REC_DIM  0xA0C3u /* #A01818 Blinking REC dot dim phase */
#define UI_COL_AMBER    0xFD84u /* #FFB020 Paused, VOR wait, warnings, S2 */
#define UI_COL_GREEN    0x3EF0u /* #3DDC84 Saved, charging, good level, S3 */
#define UI_COL_PINK     0xFBD9u /* #FF7AC8 Speaker 4 (S4) */

#define UI_W            160
#define UI_H            80
#define UI_BAR_H        13

/* =========================================================================
 * UI Icon Identifiers (mapped to glyphs/widgets in ui_theme.c / EEZ Studio)
 * ========================================================================= */
typedef enum {
    UI_ICON_NONE = 0,
    UI_ICON_PLAY,
    UI_ICON_PAUSE,
    UI_ICON_STOP,
    UI_ICON_DOT,
    UI_ICON_LEFT,
    UI_ICON_RIGHT,
    UI_ICON_UP,
    UI_ICON_DOWN,
    UI_ICON_REW,
    UI_ICON_FFW,
    UI_ICON_CHECK,
    UI_ICON_SPK,
    UI_ICON_HP,
    UI_ICON_FLAG,
    UI_ICON_BOLT,
    UI_ICON_FILES,
    UI_ICON_SETUP,
    UI_ICON_BT,
    UI_ICON_USB,
    UI_ICON_MIC,
} ui_icon_t;

/* =========================================================================
 * Shared Formatting Helpers (src/ui/ui_theme.c)
 * ========================================================================= */
void ui_format_duration(char *dst, int dst_len, uint32_t secs);
void ui_format_clock(char *dst, int dst_len, uint8_t hour, uint8_t minute);
void ui_format_free_time(char *dst, int dst_len, uint32_t free_secs);
void ui_format_date_label(char *dst, int dst_len,
                          const rtc_datetime_t *now, const rtc_datetime_t *rec);

/* =========================================================================
 * Live 128-pt FFT Mel Spectrogram (ui_spectrogram.c)
 * ========================================================================= */
#define UI_SPEC_W       144u
#define UI_SPEC_H       24u
#define UI_SPEC_FFT_N   128u

typedef struct {
    uint16_t pixels[UI_SPEC_H][UI_SPEC_W]; /* scrolling RGB565 framebuffer */
    uint8_t  markers[UI_SPEC_W];           /* 1 if a cyan marker sits at column x */
    uint16_t col_count;
} ui_spectrogram_t;

void ui_spec_reset(ui_spectrogram_t *sp);
void ui_spec_push_pcm(ui_spectrogram_t *sp, const int16_t *pcm, uint32_t frames,
                      uint8_t channels, int vor_paused);
void ui_spec_drop_marker(ui_spectrogram_t *sp);
void ui_spec_blit(const ui_spectrogram_t *sp, int16_t x, int16_t y);

/* =========================================================================
 * UI Screen Models & State Machine (ui_screens.c)
 * ========================================================================= */
typedef enum {
    UI_SCR_HOME = 0,
    UI_SCR_REC,
    UI_SCR_SAVED,
    UI_SCR_FILES,
    UI_SCR_PLAYER,
    UI_SCR_DELETE,
    UI_SCR_SETTINGS,
    UI_SCR_ABOUT,
    UI_SCR_USB,
    UI_SCR_BT,
    UI_SCR_CHARGE,
    UI_SCR_POWER,
    UI_SCR_OFF,
} ui_screen_id_t;

typedef enum {
    UI_SKIP_5_10 = 0,
    UI_SKIP_10_30,
    UI_SKIP_TRACK,
} ui_skip_mode_t;

typedef enum {
    UI_SCR_OFF_15S = 0,
    UI_SCR_OFF_30S,
    UI_SCR_OFF_1M,
    UI_SCR_OFF_5M,
    UI_SCR_OFF_NEVER,
} ui_screen_off_t;

typedef struct {
    uint8_t reserved0;      /* reserved (0); keeps openfw_persist_t 64-byte layout */
    uint8_t mic_gain;       /* 0..31 (default 20) */
    uint8_t sound_mode;     /* 0=Raw, 1=Clarity (default), 2=Bypass */
    uint8_t channels;       /* 1=1ch, 2=2ch (default) */
    uint8_t vor_enabled;    /* 0=Off (default), 1=On */
    uint8_t brightness_idx; /* 0..4 => 20,40,60,80(def),100% */
    uint8_t screen_off_idx; /* ui_screen_off_t (default 30s) */
    uint8_t skip_mode;      /* ui_skip_mode_t (default 5/10s) */
    uint8_t hp_vol;         /* 0..31 (default 18) */
    uint8_t spk_vol;        /* 0..31 (default 18) */
} ui_settings_t;

typedef struct {
    char           filename[32];
    rtc_datetime_t dt;
    uint32_t       duration_sec;
    uint32_t       resume_sec;
    uint8_t        unplayed;
    uint8_t        has_transcript;
    uint8_t        channels;
} ui_file_entry_t;

typedef struct {
    /* Global hardware telemetry */
    rtc_datetime_t now;
    uint8_t        batt_pct;
    uint16_t       batt_mv;
    int            charging;
    int            hp_plugged;
    int            sd_ok;
    uint32_t       sd_total_mb;
    uint32_t       sd_free_sec;
    char           bt_name[16];

    /* Settings */
    ui_settings_t  cfg;

    /* Home */
    uint8_t        home_focus;      /* 0=Last, 1=Files (def), 2=Setup, 3=Sync */
    int            has_last;
    ui_file_entry_t last_file;

    /* Recording */
    uint32_t       rec_sec;
    uint8_t        rec_blink;       /* 0 or 1 at 1 Hz */
    uint8_t        rec_vor_wait;    /* 1 when VOR paused */
    uint8_t        rec_stopped;     /* 1 when stopped with REC switch still ON */
    char           rec_stop_reason[24];
    char           rec_filename[32];
    uint16_t       rec_markers;
    uint8_t        rec_vu_l;
    uint8_t        rec_vu_r;
    uint8_t        rec_peak_l;
    uint8_t        rec_peak_r;
    uint8_t        rec_clip_l;
    uint8_t        rec_clip_r;
    uint8_t        rec_gain_bar;    /* 1 while VOL+/- adjusting gain */
    char           rec_warn[36];    /* non-empty => amber/red warning pill */
    uint16_t       rec_warn_col;
    ui_spectrogram_t *spec;

    /* Saved card */
    uint32_t       saved_sec;
    uint8_t        saved_channels;
    uint8_t        saved_frac_pct;  /* 0..100 remaining green countdown bar */
    char           saved_filename[32];
    rtc_datetime_t saved_dt;

    /* Files */
    const ui_file_entry_t *files;
    int            file_count;
    int            file_sel;
    int            file_scroll;

    /* Player */
    ui_file_entry_t play_file;
    uint32_t       play_pos_sec;
    uint32_t       play_pos_ms;      /* millisecond playback position for smooth progress */
    uint32_t       play_dur_ms;      /* millisecond duration for smooth progress */
    uint8_t        play_state;       /* 0=Playing, 1=Paused, 2=End */
    int8_t         play_scrub_speed; /* 0=normal, +/-2..5 during audible scrub */
    uint8_t        play_vol_hi;      /* 1 for 1.5s after VOL+/- */
    uint8_t        play_vu_l;
    uint8_t        play_vu_r;
    int            play_has_transcript;
    uint8_t        play_has_prev;    /* set by caller: a previous file exists */
    uint8_t        play_has_next;    /* set by caller: a next file exists */
    cwx_tr_slot_t  tr_lines[CWX_TR_VIEW_LINES]; /* [0]=above, [1..4]=visible, [5]=below */
    int8_t         tr_active_slot;   /* 1..4 active row in tr_lines[], or -1 */
    int32_t        tr_top_line_id;   /* monotonic line_id of tr_lines[1] (drives scroll anim) */
    uint8_t        tr_spoken_len;    /* spoken prefix bytes in active line */
    uint8_t        tr_word_start;    /* active word byte offset in active line */
    uint8_t        tr_word_len;      /* active word byte len (0 = silence/no pill) */
    uint8_t        tr_speaker;       /* active speaker index */
    uint16_t       tr_speaker_color; /* active speaker RGB565 color (0 = palette fallback) */
    char           tr_speaker_name[20]; /* active speaker label */

    /* Delete confirmation */
    uint8_t        del_hold_pct;    /* 0..100 */
    uint8_t        del_holding;

    /* Settings */
    int            set_sel;         /* 0..9 */
    int            set_scroll;
    uint8_t        set_edit_clock;  /* 0=no, 1=hour, 2=min */
    uint8_t        set_edit_h;
    uint8_t        set_edit_m;
    uint8_t        set_input_vu;    /* live input meter on Mic gain row */
    uint8_t        set_input_peak;

    /* USB */
    uint8_t        vbus_present;
    uint8_t        usb_read_act;
    uint8_t        usb_write_act;
    uint64_t       usb_tx_bytes;    /* device -> host (IN) */
    uint64_t       usb_rx_bytes;    /* host -> device (OUT) */

    /* Bluetooth Sync */
    uint8_t        bt_state;        /* 0=wait, 1=conn, 2=send */
    char           bt_peer[24];
    uint16_t       bt_sent;
    uint16_t       bt_total;
    uint8_t        bt_pct;

    /* Power off */
    uint8_t        power_hold_pct;  /* 0..100 */

    /* Active Toast overlay */
    char           toast_msg[40];
    uint16_t       toast_col;       /* UI_COL_AMBER / UI_COL_CYAN / UI_COL_GREEN */
} ui_model_t;

void ui_settings_defaults(ui_settings_t *cfg);
int ui_poll_power_button(ui_screen_id_t screen, ui_model_t *model, uint32_t now);
void ui_render_screen(ui_screen_id_t scr, const ui_model_t *m);
void ui_rec_update_live(const ui_model_t *m);
void ui_rec_push_live(const ui_model_t *m, const int16_t *pcm, uint32_t frames,
                      uint8_t channels, int vor_paused, int full_render);
void ui_spec_reset_async(ui_spectrogram_t *sp);
void ui_spec_drop_marker_async(ui_spectrogram_t *sp);
void ui_render_screen_sync(ui_screen_id_t scr, const ui_model_t *m);
void ui_rec_update_live_sync(const ui_model_t *m);
void ui_dispatch_tick(void);
int  ui_dispatch_is_animating(void);

/* =========================================================================
 * Persistent Settings & Last-Played Resume State (/cassini/settings.bin)
 * ========================================================================= */
#define OPENFW_PERSIST_MAGIC   0x3143464Fu /* "OFC1" little-endian */
#define OPENFW_PERSIST_VERSION 1u

typedef struct {
    uint32_t      magic;
    uint16_t      version;
    uint8_t       has_last;
    uint8_t       reserved0;
    ui_settings_t cfg;
    uint8_t       reserved1[2];
    uint32_t      last_resume_sec;
    char          last_filename[32];
    uint8_t       reserved2[6];
    uint16_t      crc16;
} openfw_persist_t;

int ui_persist_load(exfat_vol_t *vol, ui_settings_t *cfg,
                    char *last_file, uint32_t *last_resume, int *has_last);
int ui_persist_save(exfat_vol_t *vol, const ui_settings_t *cfg,
                    const char *last_file, uint32_t last_resume, int has_last);

#endif /* OPENFW_UI_H */
