/*
 * Cassini OpenFW — Bluetooth UI strings (BT=1 images only), 18 languages
 *
 * The BT=1 screens (phone pairing prompt, Bluetooth choice, headphones, Settings "Audio output", player headset
 * toasts, low-battery stop) take their text from here. A separate table keeps the default (BT=0) image byte-identical:
 * openfw_i18n.h's enum and table are unchanged, and this file is compiled only with BT=1. Same language order and
 * index as ui_i18n_get_lang(); .rodata in XIP flash, no SRAM. Byte and pixel limits per use: tests/test_ui_render.c
 * (BT=1 build).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_I18N_BT_H
#define OPENFW_I18N_BT_H

#include "openfw_i18n.h"

typedef enum {
    UI_BT_STR_PAIR_Q_FMT = 0, /* BT screen, prompt countdown: "Pair? %u s" */
    UI_BT_STR_PAIR_KEYS, /* BT screen footer while a prompt is up */
    UI_BT_STR_PAIR_OPEN_FMT, /* BT screen: pairing window open, seconds left */
    UI_BT_STR_BONDED, /* BT screen: the shown peer is bonded; headphones message "Paired" */
    UI_BT_STR_PAIR_UNAVAIL, /* toast / footer: pairing refused by the entropy gate */
    UI_BT_STR_AUDIO_OUTPUT, /* Settings row label */
    UI_BT_STR_OUT_SPEAKER, /* Settings Audio output value */
    UI_BT_STR_HEADPHONES, /* Bluetooth choice row, headphones screen title and on/off row */
    UI_BT_STR_OUT_BOTH, /* Settings Audio output value */
    UI_BT_STR_OUT_BT, /* Settings Audio output value: the BT headphones */
    UI_BT_STR_DESC_AUDIO_OUTPUT, /* Settings description of Audio output */
    UI_BT_STR_PHONE_SYNC, /* Bluetooth choice row */
    UI_BT_STR_DESC_PHONE, /* Bluetooth choice footer, Phone selected */
    UI_BT_STR_DESC_HP, /* Bluetooth choice footer, Headphones selected */
    UI_BT_STR_BT_FAULT, /* Headphones screen status: BT stopped after a fault */
    UI_BT_STR_STARTING, /* Headphones screen status */
    UI_BT_STR_DISCONNECT, /* Headphones screen row */
    UI_BT_STR_PAIRING, /* Headphones screen status */
    UI_BT_STR_CONNECTING, /* Headphones screen status */
    UI_BT_STR_LINK_STALLED, /* Headphones screen status */
    UI_BT_STR_RECONNECTING, /* Headphones screen status (value: seconds); headphones message */
    UI_BT_STR_NOT_CONNECTED, /* Headphones screen status */
    UI_BT_STR_SEARCHING, /* Headphones screen row while searching (value: seconds) */
    UI_BT_STR_SEARCH, /* Headphones screen row */
    UI_BT_STR_FORGET_PREFIX, /* Headphones screen row: prefix before the headset's name */
    UI_BT_STR_AUDIO, /* Search result value: an audio device */
    UI_BT_STR_OTHER, /* Search result value: another device class */
    UI_BT_STR_HP_FOOT, /* Headphones screen footer */
    UI_BT_STR_HP_PAIR_HINT, /* Headphones screen footer on the Search row */
    UI_BT_STR_SEARCH_DONE, /* Headphones message */
    UI_BT_STR_SEARCH_FAILED, /* Headphones message */
    UI_BT_STR_PAIR_FAILED, /* Headphones message */
    UI_BT_STR_PAIR_REFUSED, /* Headphones message */
    UI_BT_STR_HP_CONNECTED, /* Headphones message */
    UI_BT_STR_CONNECT_FAILED, /* Headphones message */
    UI_BT_STR_HP_DISCONNECTED, /* Headphones message and player toast */
    UI_BT_STR_HP_FORGOTTEN, /* Headphones message */
    UI_BT_STR_LINK_STALLED_RECONN, /* Headphones message */
    UI_BT_STR_HP_NOT_READY, /* Headphones message and player toast */
    UI_BT_STR_NOT_IN_BUILD, /* Headphones message: feature compiled out */
    UI_BT_STR_HP_NOT_CONNECTED, /* Player toast: play with no headphones stream */
    UI_BT_STR_HP_PAUSED, /* Player toast: the headset paused the stream */
    UI_BT_STR_LOWBATT_BT_OFF, /* Toast: Bluetooth stopped / refused at low battery */
    UI_BT_STR_HP_PAIR_AGAIN, /* Headphones message: the remembered headset lost its key, reconnects stopped */
    UI_BT_STR_HP_SPK_ONLY, /* Player toast: output Both with no headset: the speaker plays alone */
    UI_BT_STR_NO_SPEECH, /* Player transcript view: 0-word .cwx sidecar ("No speech") */
    UI_BT_STR_SYNC_DEFER_OFF, /* Toast: first 2s Power hold during active background sync defers shutdown */
    UI_BT_STR_AUTO_SYNC, /* Settings row label: automatic background phone sync */
    UI_BT_STR_DESC_AUTO_SYNC, /* Settings description of Auto sync */
    UI_BT_STR_AUTO_OFF, /* Settings row label: smart idle auto-power-off */
    UI_BT_STR_DESC_AUTO_OFF, /* Settings description of Auto off */
    UI_BT_STR_TRANSCRIBING, /* Home/BT status: phone app is transcribing queued recordings */
    UI_BT_STR_IN_QUEUE_FMT, /* Home/BT detail: "%u" items waiting in sync/transcription queue */
    UI_BT_STR_COUNT
} ui_bt_str_id_t;

const char *ui_tr_bt(ui_bt_str_id_t id);                    /* current UI language */
const char *ui_tr_bt_lang(uint8_t lang, ui_bt_str_id_t id); /* English for an unknown language or id */

#endif /* OPENFW_I18N_BT_H */
