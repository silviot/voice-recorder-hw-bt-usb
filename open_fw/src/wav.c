/*
 * Smartlink SL6801 OpenFW Standalone RIFF/WAVE PCM Header Builder & Parser
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_wav.h"

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int tag_eq(const uint8_t *p, const char tag[4])
{
    return p[0] == (uint8_t)tag[0] &&
           p[1] == (uint8_t)tag[1] &&
           p[2] == (uint8_t)tag[2] &&
           p[3] == (uint8_t)tag[3];
}

int wav_build_header(uint8_t out[44],
                     uint32_t sample_rate,
                     uint16_t channels,
                     uint16_t bits_per_sample,
                     uint32_t data_bytes)
{
    if (!out || sample_rate == 0u || channels == 0u || (bits_per_sample % 8u) != 0u || bits_per_sample == 0u) {
        return -1;
    }

    uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8u));
    uint32_t byte_rate   = sample_rate * (uint32_t)block_align;

    out[0] = 'R'; out[1] = 'I'; out[2] = 'F'; out[3] = 'F';
    put_le32(&out[4], 36u + data_bytes);
    out[8]  = 'W'; out[9]  = 'A'; out[10] = 'V'; out[11] = 'E';
    out[12] = 'f'; out[13] = 'm'; out[14] = 't'; out[15] = ' ';
    put_le32(&out[16], 16u);
    put_le16(&out[20], 1u); /* PCM */
    put_le16(&out[22], channels);
    put_le32(&out[24], sample_rate);
    put_le32(&out[28], byte_rate);
    put_le16(&out[32], block_align);
    put_le16(&out[34], bits_per_sample);
    out[36] = 'd'; out[37] = 'a'; out[38] = 't'; out[39] = 'a';
    put_le32(&out[40], data_bytes);
    return 0;
}

int wav_parse_header(const uint8_t in[44], wav_info_t *out)
{
    if (!in || !out) {
        return -1;
    }
    if (!tag_eq(&in[0], "RIFF") || !tag_eq(&in[8], "WAVE") ||
        !tag_eq(&in[12], "fmt ") || !tag_eq(&in[36], "data")) {
        return -1;
    }
    if (get_le32(&in[16]) != 16u || get_le16(&in[20]) != 1u) {
        return -1;
    }

    out->channels        = get_le16(&in[22]);
    out->sample_rate     = get_le32(&in[24]);
    out->byte_rate       = get_le32(&in[28]);
    out->block_align     = get_le16(&in[32]);
    out->bits_per_sample = get_le16(&in[34]);
    out->data_bytes      = get_le32(&in[40]);
    return 0;
}
