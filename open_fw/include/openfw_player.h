/*
 * Smartlink SL6801 OpenFW On-Device WAV & Cassini Timed-Word (`CWX1`) Player
 *
 * Streams 16-bit PCM or Microsoft IMA-ADPCM .wav files from exFAT over
 * Audio DAC0+DAC1 + DMAC CH0, rendering synchronized speaker-turn-ordered
 * captions or the time-first player screen via UI_SCR_PLAYER on 160x80 LCD.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_PLAYER_H
#define OPENFW_PLAYER_H

#include "openfw_fs.h"
#include "openfw_ui.h"

/*
 * Player key gestures (debounced, time-based; pure logic so it is host-testable):
 *   OK            play / pause (replay at end)
 *   BACK          leave the player
 *   VOL+/VOL-     one step on press, then auto-repeat while held
 *   LEFT/RIGHT    double tap: previous / next file
 *                 single tap: one seek step (sent once the double-tap window
 *                 has passed without a second tap)
 *                 hold: seek steps, repeated while held
 * A key already held when the player starts is ignored until it is released,
 * so the OK press that opened the player does not trigger anything.
 */
#define PLAYER_KEY_DEBOUNCE_MS      30u
#define PLAYER_KEY_DOUBLE_TAP_MS   350u  /* release -> second press */
#define PLAYER_KEY_HOLD_MS         500u  /* press -> first hold seek step */
#define PLAYER_KEY_SCRUB_MS        250u  /* hold seek repeat period */
#define PLAYER_KEY_REPEAT_DELAY_MS 400u  /* VOL press -> first repeat */
#define PLAYER_KEY_REPEAT_MS       100u  /* VOL repeat period */

typedef enum {
    PLAYER_ACT_NONE = 0,
    PLAYER_ACT_PLAY_PAUSE,
    PLAYER_ACT_BACK,
    PLAYER_ACT_VOL_UP,
    PLAYER_ACT_VOL_DOWN,
    PLAYER_ACT_SEEK_BACK,   /* single tap */
    PLAYER_ACT_SEEK_FWD,
    PLAYER_ACT_SCRUB_BACK,  /* one step of a hold */
    PLAYER_ACT_SCRUB_FWD,
    PLAYER_ACT_PREV_FILE,   /* double tap */
    PLAYER_ACT_NEXT_FILE,
} player_action_t;

typedef struct {
    key_id_t raw;          /* last raw decode */
    uint32_t raw_since;    /* when raw last changed */
    key_id_t held;         /* debounced key */
    uint32_t held_since;   /* when held was accepted */
    uint32_t next_repeat;  /* next VOL repeat / hold seek step */
    uint8_t  hold_active;  /* LEFT/RIGHT hold has started seeking */
    uint8_t  ignore;       /* held key is ignored until released */
    key_id_t tap_key;      /* single tap waiting for a possible second tap */
    uint32_t tap_deadline;
} player_keys_t;

void            player_keys_init(player_keys_t *s, key_id_t initial, uint32_t now_ms);
player_action_t player_keys_tick(player_keys_t *s, uint32_t now_ms);
player_action_t player_keys_edge(player_keys_t *s, key_id_t key, uint32_t now_ms);
player_action_t player_keys_step(player_keys_t *s, key_id_t raw, uint32_t now_ms);

/*
 * Before calling, set m->play_has_prev / m->play_has_next to say whether a
 * previous / next file exists; without one the player shows a toast instead
 * of returning -1 / 1.
 *
 * Returns:
 *   0 : exited via KEY_BACK
 *   1 : requested next track
 *  -1 : requested previous track
 *   2 : interrupted by REC slide switch ON
 */
int  openfw_play_file_ui(exfat_vol_t *vol, const char *dir_path, const char *wav_name,
                         ui_model_t *m);
void openfw_play_file(exfat_vol_t *vol, const char *dir_path, const char *wav_name);

#endif /* OPENFW_PLAYER_H */
