/*
 * Smartlink SL6801 OpenFW Streaming Cassini Timed-Word (`CWX1`) Engine
 *
 * Reads precompiled binary `CWX1` sidecars generated from Cassini v1 .opus
 * transcripts (`cassini.words.v1`) in O(1) RAM while strictly preserving
 * speaker-turn order and 1-second seek offsets.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_CWX_H
#define OPENFW_CWX_H

#include <stdint.h>

#define CWX_HEADER_SIZE     64u
#define CWX_SPEAKER_SIZE    24u
#define CWX_SEEK_SIZE       8u
#define CWX_MAX_SPEAKERS    16u
#define CWX_MAX_WORD_TEXT   48u

#define CWX_FLAG_TURN_START 0x01u
#define CWX_FLAG_LOW_CONF   0x02u

typedef struct {
    uint16_t color_rgb565;
    uint8_t  flags;
    uint8_t  reserved;
    char     label[20];
} cwx_speaker_t;

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    uint32_t word_idx;
    uint8_t  speaker_idx;
    uint8_t  flags;
    uint8_t  text_len;
    char     text[CWX_MAX_WORD_TEXT];
} cwx_word_t;

typedef int (*cwx_read_fn)(void *user, uint32_t offset, void *dst, uint32_t len, uint32_t *out_read);

typedef struct {
    cwx_read_fn   read_fn;
    void         *user;
    uint32_t      file_size;

    uint16_t      speaker_count;
    uint32_t      duration_ms;
    uint32_t      word_count;
    uint32_t      speakers_off;
    uint32_t      seek_off;
    uint32_t      seek_count;
    uint32_t      words_off;
    char          title[32];
    cwx_speaker_t speakers[CWX_MAX_SPEAKERS];

    uint32_t      cur_byte_off;
    uint32_t      cur_word_idx;

    uint32_t      cache_off;
    uint32_t      cache_len;
    uint8_t       cache[512];
} cwx_stream_t;

int      cwx_open(cwx_stream_t *s, cwx_read_fn read_fn, void *user, uint32_t file_size);
int      cwx_seek_ms(cwx_stream_t *s, uint32_t target_ms);
int      cwx_next_word(cwx_stream_t *s, cwx_word_t *out);
int      cwx_read_window(cwx_stream_t *s,
                         uint32_t pos_ms,
                         cwx_word_t *out_words,
                         uint32_t max_words,
                         int *out_active_idx);
const char *cwx_speaker_label(const cwx_stream_t *s, uint8_t speaker_idx);
uint16_t    cwx_speaker_color(const cwx_stream_t *s, uint8_t speaker_idx);

#endif /* OPENFW_CWX_H */
