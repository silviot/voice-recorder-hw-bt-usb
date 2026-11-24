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
 *   OK            play / pause at 1x (replay at end); at any other speed: back to 1x
 *   BACK          leave the player
 *   VOL+/VOL-     one step on press, then auto-repeat while held
 *   LEFT/RIGHT    one action on press, repeated every PLAYER_KEY_STEP_MS while
 *                 held. With the "Speed" skip setting each action steps the
 *                 speed dial (-32x .. -1x, 1x .. 32x, see ui_speed_step()); with
 *                 "Track" it changes file.
 * A key already held when the player starts is ignored until it is released,
 * so the OK press that opened the player does not trigger anything.
 */
#define PLAYER_KEY_DEBOUNCE_MS      30u
#define PLAYER_KEY_STEP_MS         600u  /* LEFT/RIGHT held: repeat period */
#define PLAYER_KEY_REPEAT_DELAY_MS 400u  /* VOL press -> first repeat */
#define PLAYER_KEY_REPEAT_MS       100u  /* VOL repeat period */

typedef enum {
    PLAYER_ACT_NONE = 0,
    PLAYER_ACT_PLAY_PAUSE,
    PLAYER_ACT_BACK,
    PLAYER_ACT_VOL_UP,
    PLAYER_ACT_VOL_DOWN,
    PLAYER_ACT_SEEK_BACK,   /* LEFT press or hold repeat */
    PLAYER_ACT_SEEK_FWD,    /* RIGHT press or hold repeat */
} player_action_t;

typedef struct {
    key_id_t raw;          /* last raw decode */
    uint32_t raw_since;    /* when raw last changed */
    key_id_t held;         /* debounced key */
    uint32_t held_since;   /* when held was accepted */
    uint32_t next_repeat;  /* next VOL / LEFT / RIGHT repeat */
    uint8_t  ignore;       /* held key is ignored until released */
    uint8_t  repeat;       /* the last LEFT/RIGHT action was a hold repeat */
} player_keys_t;

void            player_keys_init(player_keys_t *s, key_id_t initial, uint32_t now_ms);
player_action_t player_keys_tick(player_keys_t *s, uint32_t now_ms);
player_action_t player_keys_edge(player_keys_t *s, key_id_t key, uint32_t now_ms);
player_action_t player_keys_step(player_keys_t *s, key_id_t raw, uint32_t now_ms);

/*
 * Silent seek clock (|speed| >= UI_SPEED_SILENT): move pos (a frame boundary in
 * [0, total]) by elapsed_ms * |speed| * sample_rate / 1000 in the direction of
 * speed, clamped to the file. *rem_milli carries the sub-frame remainder.
 */
int64_t player_scrub_advance(int64_t pos, uint32_t total, int speed, uint32_t sample_rate,
                             uint32_t elapsed_ms, uint32_t *rem_milli);

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
