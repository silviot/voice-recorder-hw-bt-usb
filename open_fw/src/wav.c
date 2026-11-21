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
    if (!out || sample_rate == 0u || channels == 0u ||
        bits_per_sample == 0u || (bits_per_sample % 8u) != 0u) {
        return -1;
    }

    uint32_t ba32 = (uint32_t)channels * (uint32_t)(bits_per_sample / 8u);
    if (ba32 == 0u || ba32 > 0xFFFFu) {
        return -1;
    }
    uint64_t br64 = (uint64_t)sample_rate * (uint64_t)ba32;
    if (br64 > 0xFFFFFFFFull || data_bytes > (0xFFFFFFFFu - 36u)) {
        return -1;
    }

    uint16_t block_align = (uint16_t)ba32;
    uint32_t byte_rate   = (uint32_t)br64;

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

    uint16_t channels        = get_le16(&in[22]);
    uint32_t sample_rate     = get_le32(&in[24]);
    uint32_t byte_rate       = get_le32(&in[28]);
    uint16_t block_align     = get_le16(&in[32]);
    uint16_t bits_per_sample = get_le16(&in[34]);
    uint32_t data_bytes      = get_le32(&in[40]);

    if (channels == 0u || sample_rate == 0u || block_align == 0u ||
        bits_per_sample == 0u || (bits_per_sample % 8u) != 0u) {
        return -1;
    }
    if ((uint32_t)channels * (uint32_t)(bits_per_sample / 8u) != (uint32_t)block_align) {
        return -1;
    }

    out->channels        = channels;
    out->sample_rate     = sample_rate;
    out->byte_rate       = byte_rate;
    out->block_align     = block_align;
    out->bits_per_sample = bits_per_sample;
    out->data_bytes      = data_bytes;
    return 0;
}

static uint32_t compute_duration_ms(uint32_t total_samples, uint32_t rate)
{
    if (rate == 0u || total_samples == 0u) {
        return 0u;
    }
    uint64_t ms = ((uint64_t)total_samples * 1000ull) / (uint64_t)rate;
    return (ms > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)ms;
}

int wav_parse_stream(const uint8_t *hdr, uint32_t hdr_len, wav_stream_info_t *out)
{
    if (!hdr || !out || hdr_len < 20u) {
        return -1;
    }
    if (!tag_eq(&hdr[0], "RIFF") || !tag_eq(&hdr[8], "WAVE")) {
        return -1;
    }

    int      have_fmt     = 0;
    int      have_fact    = 0;
    uint32_t fact_samples = 0u;

    uint16_t fmt_tag      = 0u;
    uint16_t channels     = 0u;
    uint32_t sample_rate  = 0u;
    uint16_t block_align  = 0u;
    uint16_t bits         = 0u;
    uint16_t spb          = 0u;

    uint32_t pos = 12u;
    while (pos <= hdr_len - 8u) {
        const uint8_t *cid = &hdr[pos];
        uint32_t csize     = get_le32(&hdr[pos + 4u]);
        uint32_t payload   = pos + 8u;

        if (tag_eq(cid, "fmt ")) {
            if (csize < 16u || csize > (hdr_len - payload)) {
                return -1;
            }
            fmt_tag     = get_le16(&hdr[payload + 0u]);
            channels    = get_le16(&hdr[payload + 2u]);
            sample_rate = get_le32(&hdr[payload + 4u]);
            block_align = get_le16(&hdr[payload + 12u]);
            bits        = get_le16(&hdr[payload + 14u]);

            if (channels != 1u && channels != 2u) {
                return -1;
            }
            if (sample_rate == 0u) {
                return -1;
            }

            if (fmt_tag == WAV_FMT_PCM) {
                if (bits != 16u || block_align != (uint16_t)(channels * 2u)) {
                    return -1;
                }
                spb = 1u;
            } else if (fmt_tag == WAV_FMT_IMA_ADPCM) {
                uint32_t hdr_bytes = (uint32_t)channels * 4u;
                if (bits != 4u || block_align < hdr_bytes) {
                    return -1;
                }
                if (channels == 2u && ((block_align - hdr_bytes) & 7u) != 0u) {
                    return -1;
                }
                uint32_t calc_spb = ((uint32_t)(block_align - hdr_bytes) * 2u) / (uint32_t)channels + 1u;
                if (calc_spb == 0u || calc_spb > 0xFFFFu) {
                    return -1;
                }
                spb = (uint16_t)calc_spb;
                if (csize >= 20u) {
                    uint16_t cb_size = get_le16(&hdr[payload + 16u]);
                    if (cb_size >= 2u) {
                        uint16_t hdr_spb = get_le16(&hdr[payload + 18u]);
                        if (hdr_spb > 0u && hdr_spb != spb) {
                            return -1;
                        }
                    }
                }
            } else {
                return -1;
            }
            have_fmt = 1;
        } else if (tag_eq(cid, "fact")) {
            if (csize >= 4u && (hdr_len - payload) >= 4u) {
                fact_samples = get_le32(&hdr[payload]);
                have_fact    = 1;
            }
        } else if (tag_eq(cid, "data")) {
            if (!have_fmt) {
                return -1;
            }
            uint64_t data_samples;
            if (fmt_tag == WAV_FMT_PCM) {
                data_samples = (uint64_t)(csize / block_align);
            } else {
                uint32_t full_blocks = csize / block_align;
                uint32_t rem         = csize % block_align;
                uint32_t hdr_bytes   = (uint32_t)channels * 4u;
                uint32_t tail        = (rem >= hdr_bytes)
                                     ? (((rem - hdr_bytes) * 2u) / (uint32_t)channels + 1u)
                                     : 0u;
                data_samples = (uint64_t)full_blocks * (uint64_t)spb + (uint64_t)tail;
            }

            uint64_t total64 = data_samples;
            if (have_fact && fact_samples > 0u) {
                if (fmt_tag == WAV_FMT_PCM) {
                    /* Never allow a fact chunk to extend PCM total_samples past data bytes */
                    if ((uint64_t)fact_samples < data_samples) {
                        total64 = (uint64_t)fact_samples;
                    }
                } else {
                    total64 = (uint64_t)fact_samples;
                }
            }
            uint32_t total_samples = (total64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)total64;

            out->format_tag        = fmt_tag;
            out->channels          = channels;
            out->sample_rate       = sample_rate;
            out->bits_per_sample   = bits;
            out->block_align       = block_align;
            out->samples_per_block = spb;
            out->data_offset       = payload;
            out->data_bytes        = csize;
            out->total_samples     = total_samples;
            out->duration_ms       = compute_duration_ms(total_samples, sample_rate);
            return 0;
        }

        uint32_t step = csize + (csize & 1u);
        if (step < csize || step > (hdr_len - payload)) {
            break;
        }
        pos = payload + step;
    }

    return -1;
}

static int16_t ima_decode_nibble(int32_t *pred, int8_t *step_idx, uint8_t nib)
{
    static const int16_t k_ima_step[89] = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
        19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
        50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
        337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
        876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
        5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };
    static const int8_t k_ima_index[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    };

    int32_t step = (int32_t)k_ima_step[*step_idx];
    int32_t diff = step >> 3;
    if (nib & 1u) {
        diff += step >> 2;
    }
    if (nib & 2u) {
        diff += step >> 1;
    }
    if (nib & 4u) {
        diff += step;
    }
    if (nib & 8u) {
        diff = -diff;
    }

    int32_t p = *pred + diff;
    if (p > 32767) {
        p = 32767;
    } else if (p < -32768) {
        p = -32768;
    }
    *pred = p;

    int32_t ni = (int32_t)(*step_idx) + (int32_t)k_ima_index[nib & 0x0Fu];
    if (ni < 0) {
        ni = 0;
    } else if (ni > 88) {
        ni = 88;
    }
    *step_idx = (int8_t)ni;
    return (int16_t)p;
}

int wav_decode_ima_adpcm_block(const uint8_t *block,
                               uint16_t block_align,
                               uint16_t channels,
                               int16_t *out_pcm,
                               uint32_t max_frames)
{
    if (!block || !out_pcm || (channels != 1u && channels != 2u)) {
        return -1;
    }
    if (block_align < (uint16_t)(channels * 4u)) {
        return -1;
    }

    if (channels == 1u) {
        uint32_t frames = 1u + ((uint32_t)(block_align - 4u) * 2u);
        if (frames > max_frames) {
            return -1;
        }
        int32_t pred = (int16_t)get_le16(&block[0]);
        int8_t  idx  = (int8_t)((block[2] > 88u) ? 88u : block[2]);
        out_pcm[0]   = (int16_t)pred;

        uint32_t f = 1u;
        for (uint16_t off = 4u; off < block_align; off++) {
            uint8_t b = block[off];
            out_pcm[f++] = ima_decode_nibble(&pred, &idx, (uint8_t)(b & 0x0Fu));
            out_pcm[f++] = ima_decode_nibble(&pred, &idx, (uint8_t)(b >> 4));
        }
        return (int)f;
    }

    if (((block_align - 8u) & 7u) != 0u) {
        return -1;
    }
    uint32_t frames = 1u + (uint32_t)(block_align - 8u);
    if (frames > max_frames) {
        return -1;
    }

    int32_t pred_l = (int16_t)get_le16(&block[0]);
    int8_t  idx_l  = (int8_t)((block[2] > 88u) ? 88u : block[2]);
    int32_t pred_r = (int16_t)get_le16(&block[4]);
    int8_t  idx_r  = (int8_t)((block[6] > 88u) ? 88u : block[6]);

    out_pcm[0] = (int16_t)pred_l;
    out_pcm[1] = (int16_t)pred_r;

    uint32_t f = 1u;
    for (uint16_t off = 8u; off + 8u <= block_align; off += 8u) {
        for (uint16_t b = 0u; b < 4u; b++) {
            uint8_t bl = block[off + b];
            uint8_t br = block[off + 4u + b];
            out_pcm[f * 2u + 0u] = ima_decode_nibble(&pred_l, &idx_l, (uint8_t)(bl & 0x0Fu));
            out_pcm[f * 2u + 1u] = ima_decode_nibble(&pred_r, &idx_r, (uint8_t)(br & 0x0Fu));
            f++;
            out_pcm[f * 2u + 0u] = ima_decode_nibble(&pred_l, &idx_l, (uint8_t)(bl >> 4));
            out_pcm[f * 2u + 1u] = ima_decode_nibble(&pred_r, &idx_r, (uint8_t)(br >> 4));
            f++;
        }
    }
    return (int)f;
}
