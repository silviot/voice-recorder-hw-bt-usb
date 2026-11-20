/*
 * Smartlink SL6801 OpenFW Standalone RIFF/WAVE PCM Header Builder & Parser
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_WAV_H
#define OPENFW_WAV_H

#include <stdint.h>

#define WAV_FMT_PCM       0x0001u
#define WAV_FMT_IMA_ADPCM 0x0011u

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint32_t byte_rate;
    uint32_t data_bytes;
} wav_info_t;

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint16_t samples_per_block;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint32_t total_samples;
    uint32_t duration_ms;
} wav_stream_info_t;

int wav_build_header(uint8_t out[44],
                     uint32_t sample_rate,
                     uint16_t channels,
                     uint16_t bits_per_sample,
                     uint32_t data_bytes);

int wav_parse_header(const uint8_t in[44], wav_info_t *out);

int wav_parse_stream(const uint8_t *hdr, uint32_t hdr_len, wav_stream_info_t *out);

int wav_decode_ima_adpcm_block(const uint8_t *block,
                               uint16_t block_align,
                               uint16_t channels,
                               int16_t *out_pcm,
                               uint32_t max_frames);

#endif /* OPENFW_WAV_H */
