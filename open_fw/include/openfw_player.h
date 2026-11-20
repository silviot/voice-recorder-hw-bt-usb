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
