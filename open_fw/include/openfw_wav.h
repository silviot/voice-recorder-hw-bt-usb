/*
 * Smartlink SL6801 OpenFW Standalone RIFF/WAVE PCM Header Builder & Parser
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_WAV_H
#define OPENFW_WAV_H

#include <stdint.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint32_t byte_rate;
    uint32_t data_bytes;
} wav_info_t;

int wav_build_header(uint8_t out[44],
                     uint32_t sample_rate,
                     uint16_t channels,
                     uint16_t bits_per_sample,
                     uint32_t data_bytes);

int wav_parse_header(const uint8_t in[44], wav_info_t *out);

#endif /* OPENFW_WAV_H */
