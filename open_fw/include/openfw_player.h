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
 *                 hold: audible, pitch-preserving seek from 2x through 5x
 * A key already held when the player starts is ignored until it is released,
 * so the OK press that opened the player does not trigger anything.
 */
#define PLAYER_KEY_DEBOUNCE_MS      30u
#define PLAYER_KEY_DOUBLE_TAP_MS   350u  /* release -> second press */
#define PLAYER_KEY_HOLD_MS         500u  /* press -> first hold seek step */
#define PLAYER_SCRUB_STEP_MS       500u  /* time at each speed before accelerating */
#define PLAYER_SCRUB_MAX_RATE        5u
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
    PLAYER_ACT_SCRUB_BACK,  /* start audible 2x scrub */
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
 * Per-folder "autoplay next track": a file named .autoplay (any content, may be
 * empty, matched case-insensitively) in a folder makes the end of one track
 * continue with the next track of that folder, in Files-list order.
 */
#define PLAYER_AUTOPLAY_MARKER ".autoplay"
#define PLAYER_RC_AUTO_NEXT    3

/* 1 when dir_path/.autoplay exists as a file (one exFAT path lookup). */
int player_dir_has_autoplay(exfat_vol_t *vol, const char *dir_path);
/*
 * Next track after files[idx] for autoplay: skips folder entries and returns
 * -1 at the end of the list or when the next track lives in another folder.
 * dirs points at count rows of dir_stride bytes (one folder path per entry).
 */
int player_autoplay_next_index(const ui_file_entry_t *files, const char *dirs,
                               unsigned dir_stride, int count, int idx);

/*
 * Before calling, set m->play_has_prev / m->play_has_next to say whether a
 * previous / next file exists; without one the player shows a toast instead
 * of returning -1 / 1. Set m->play_autoplay (with play_has_next) to have the
 * natural end of the track return PLAYER_RC_AUTO_NEXT.
 *
 * Returns:
 *   0 : exited via KEY_BACK
 *   1 : requested next track
 *  -1 : requested previous track
 *   2 : interrupted by REC slide switch ON
 *   3 : PLAYER_RC_AUTO_NEXT, track ended and m->play_autoplay was set
 */
int  openfw_play_file_ui(exfat_vol_t *vol, const char *dir_path, const char *wav_name,
                         ui_model_t *m);
void openfw_play_file(exfat_vol_t *vol, const char *dir_path, const char *wav_name);

#endif /* OPENFW_PLAYER_H */
