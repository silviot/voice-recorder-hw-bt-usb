/*
 * Smartlink SL6801 OpenFW On-Device WAV & Cassini Timed-Word (`CWX1`) Player
 *
 * Streams 16-bit PCM or Microsoft IMA-ADPCM .wav files from exFAT over
 * Audio DAC0+DAC1 + DMAC CH0, rendering synchronized speaker-turn-ordered
 * captions on the 160x80 landscape ST7735S display when a .cwx sidecar exists.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_PLAYER_H
#define OPENFW_PLAYER_H

#include "openfw_fs.h"

void openfw_play_file(exfat_vol_t *vol, const char *dir_path, const char *wav_name);

#endif /* OPENFW_PLAYER_H */
