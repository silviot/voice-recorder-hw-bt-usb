/*
 * Cassini OpenFW — Internationalization (i18n)
 *
 * 17 European languages stored in .rodata (Quad XIP NOR, 0 extra SRAM).
 * Language index is persisted in ui_settings_t.lang (/cassini/settings.bin).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_I18N_H
#define OPENFW_I18N_H

#include <stdint.h>

typedef enum {
    UI_LANG_EN = 0, /* English (default) */
    UI_LANG_IT,     /* Italiano */
    UI_LANG_FR,     /* Français */
    UI_LANG_ES,     /* Español */
    UI_LANG_DE,     /* Deutsch */
    UI_LANG_NL,     /* Nederlands */
    UI_LANG_DA,     /* Dansk */
    UI_LANG_PL,     /* Polski */
    UI_LANG_SK,     /* Slovenčina */
    UI_LANG_SL,     /* Slovenščina */
    UI_LANG_SV,     /* Svenska */
    UI_LANG_FI,     /* Suomi */
    UI_LANG_CS,     /* Čeština */
    UI_LANG_ET,     /* Eesti */
    UI_LANG_HU,     /* Magyar */
    UI_LANG_BG,     /* Български */
    UI_LANG_RO,     /* Română */
    UI_LANG_RU,     /* Русский */
    UI_LANG_COUNT
} ui_lang_t;

typedef enum {
    /* Date / time / space */
    UI_STR_TODAY = 0,
    UI_STR_YESTERDAY,
    UI_STR_DATE_UNKNOWN,
    UI_STR_LEFT_FMT,           /* "%s left" */
    UI_STR_SPACE_LEFT_FMT,     /* "%s of space left" */

    /* Home tiles & details */
    UI_STR_TILE_LAST,
    UI_STR_TILE_FILES,
    UI_STR_TILE_SETUP,
    UI_STR_TILE_SYNC,
    UI_STR_STORAGE_ERROR,
    UI_STR_CONNECT_USB_CHECK,
    UI_STR_NOTHING_PLAYED,
    UI_STR_RESUME_AT_FMT,      /* "Resume at %s" */
    UI_STR_PLAY_FROM_START,
    UI_STR_RECORDING_ONE,
    UI_STR_RECORDING_MANY,
    UI_STR_GAIN,
    UI_STR_VOR_ON,
    UI_STR_VOR_OFF,
    UI_STR_BT_FILE_SYNC,
    UI_STR_DEVICE_NAME_FMT,    /* "Device name %s" */

    /* Recording & Rec error */
    UI_STR_WAITING_VOICE,
    UI_STR_RESUMES_SPEAK,
    UI_STR_OK_MARK,
    UI_STR_NOT_RECORDING,
    UI_STR_NOTHING_RECORDED,
    UI_STR_SLIDE_REC_OFF_USB,
    UI_STR_USB_CHECK_STORAGE,
    UI_STR_STOPPED,
    UI_STR_STORAGE_FULL,
    UI_STR_WRITE_ERROR,
    UI_STR_LOW_BATTERY,
    UI_STR_START_FAILED,
    UI_STR_SAVED_FILE_FMT,     /* "Saved %s" */
    UI_STR_RECORDED_FMT,       /* "%s recorded" */
    UI_STR_SLIDE_REC_CONT,

    /* Saved screen */
    UI_STR_SAVED,
    UI_STR_SAVED_FOOTER,

    /* Files & empty files */
    UI_STR_FILES,
    UI_STR_FILES_FOOTER,
    UI_STR_NO_RECORDINGS,
    UI_STR_SLIDE_REC_RECORD,

    /* Player */
    UI_STR_PLAYING,
    UI_STR_PAUSED,
    UI_STR_END,
    UI_STR_OK_REPLAY,
    UI_STR_OK_REPLAY_NEXT,
    UI_STR_PREV,
    UI_STR_NEXT,
    UI_STR_PAUSE,
    UI_STR_PLAY,

    /* Delete confirm */
    UI_STR_DELETE_Q,
    UI_STR_KEEP_HOLD_OK,
    UI_STR_DEL_FOOTER,

    /* Settings */
    UI_STR_SETTINGS,
    UI_STR_SET_MIC_GAIN,
    UI_STR_SET_SOUND,
    UI_STR_SET_MICS,
    UI_STR_SET_VOR,
    UI_STR_SET_BRIGHTNESS,
    UI_STR_SET_SCREEN_OFF,
    UI_STR_SET_CLOCK,
    UI_STR_SET_SKIP,
    UI_STR_SET_LANGUAGE,
    UI_STR_SET_USB_DISK,
    UI_STR_SET_ABOUT,
    UI_STR_SND_RAW,
    UI_STR_SND_CLARITY,
    UI_STR_SND_BYPASS,
    UI_STR_SKIP_TRACK,
    UI_STR_NEVER,
    UI_STR_ON,
    UI_STR_OFF,
    UI_STR_CONNECT,
    UI_STR_UNPLUGGED,
    UI_STR_INPUT,
    UI_STR_CLOCK_EDIT_FOOT,
    UI_STR_DESC_ABOUT,
    UI_STR_DESC_SND_RAW,
    UI_STR_DESC_SND_CLARITY,
    UI_STR_DESC_SND_BYPASS,
    UI_STR_DESC_MIC_1CH,
    UI_STR_DESC_MIC_2CH,
    UI_STR_DESC_VOR,
    UI_STR_DESC_BRIGHTNESS,
    UI_STR_DESC_SCREEN_OFF,
    UI_STR_DESC_CLOCK,
    UI_STR_DESC_SKIP,
    UI_STR_DESC_LANGUAGE,
    UI_STR_DESC_USB_MOUNT,
    UI_STR_DESC_USB_PLUG,

    /* About, USB, Bluetooth, Charge, Power */
    UI_STR_ABOUT_STORAGE_FMT,  /* "Storage %u.%u GB · %uh left" */
    UI_STR_ABOUT_BATT_FMT,     /* "Battery %u.%02u V · %u%%" */
    UI_STR_ABOUT_FOOTER,
    UI_STR_USB_EJECT,
    UI_STR_BT_SYNC,
    UI_STR_BT_WAITING,
    UI_STR_BT_OPEN_APP,
    UI_STR_BT_CONNECTED_FMT,   /* "✓ Connected · %s" */
    UI_STR_PHONE,
    UI_STR_BT_SENDING_FMT,     /* "Sending %u of %u" */
    UI_STR_BT_FOOTER,
    UI_STR_CHARGING,
    UI_STR_CHARGE_FOOTER,
    UI_STR_POWER_OFF,
    UI_STR_POWERING_OFF,
    UI_STR_KEEP_HOLD_PWR,

    /* Toasts */
    UI_STR_TOAST_DELETED,
    UI_STR_TOAST_DEL_FAILED,
    UI_STR_TOAST_SAVE_FAILED,
    UI_STR_TOAST_CLOCK_SAVED,
    UI_STR_TOAST_PLUG_USB,
    UI_STR_TOAST_UPDATE_MODE,
    UI_STR_TOAST_HP_PAUSED,
    UI_STR_TOAST_END_LIST,
    UI_STR_TOAST_START_LIST,

    UI_STR_COUNT
} ui_str_id_t;

void        ui_i18n_set_lang(uint8_t lang);
uint8_t     ui_i18n_get_lang(void);
const char *ui_lang_native_name(uint8_t lang);
const char *ui_tr(ui_str_id_t id);
const char *ui_tr_lang(uint8_t lang, ui_str_id_t id);
const char *ui_i18n_weekday(uint8_t lang, uint8_t wd);     /* 0=Mon..6=Sun */
const char *ui_i18n_month(uint8_t lang, uint8_t month);    /* 1=Jan..12=Dec */

#endif /* OPENFW_I18N_H */
