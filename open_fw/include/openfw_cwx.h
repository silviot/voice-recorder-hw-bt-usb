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

/* =========================================================================
 * Streaming Deterministic Transcript Line Paginator (160x80 Teleprompter)
 * ========================================================================= */
#define CWX_TR_VIEW_LINES   6u   /* [0]=above, [1..4]=4 visible rows, [5]=below */
#define CWX_TR_BUF_LINES    10u  /* rolling line ring cached in RAM */
#define CWX_TR_MAX_LINE_W   150u /* max text px inside 152px content area (ui_font_md) */
#define CWX_TR_LINE_CAP     48u
#define CWX_TR_MAX_WPL      12u  /* max words per wrapped line */

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t  char_off;
    uint8_t  char_len;
} cwx_tr_wspan_t;

typedef struct {
    int32_t        line_id;
    uint32_t       first_word_idx;
    uint32_t       start_ms;
    uint32_t       end_ms;
    uint16_t       speaker_color;
    uint8_t        speaker_idx;
    uint8_t        turn_start;
    uint8_t        word_count;
    uint8_t        text_len;
    char           text[CWX_TR_LINE_CAP];
    cwx_tr_wspan_t words[CWX_TR_MAX_WPL];
} cwx_tr_line_t;

typedef struct {
    int32_t  line_id;
    uint16_t speaker_color;
    uint8_t  speaker_idx;
    uint8_t  turn_start;
    char     text[CWX_TR_LINE_CAP];
} cwx_tr_slot_t;

typedef struct {
    cwx_tr_slot_t lines[CWX_TR_VIEW_LINES];
    int8_t        active_slot;       /* 1..4 inside lines[], or -1 if empty */
    int32_t       top_line_id;       /* line_id of lines[1] (top visible row) */
    uint8_t       spoken_len;        /* spoken prefix bytes on active line */
    uint8_t       word_start;        /* active word byte offset in active line */
    uint8_t       word_len;          /* active word byte len (0 = silence/hidden) */
    uint8_t       speaker_idx;
    uint16_t      speaker_color;
    char          speaker_name[20];
} cwx_tr_view_t;

typedef struct {
    cwx_stream_t *cwx;
    uint16_t      max_w_px;
    uint8_t       initialized;
    uint8_t       at_eof;
    int32_t       next_line_id;
    int           has_pending;
    cwx_word_t    pending_word;
    uint8_t       has_prev_word;
    uint8_t       prev_word_spk;
    uint32_t      count;
    cwx_tr_line_t lines[CWX_TR_BUF_LINES];
} cwx_tr_pager_t;

uint32_t cwx_measure_md_px(const char *s, uint32_t len);
void     cwx_tr_pager_init(cwx_tr_pager_t *p, cwx_stream_t *cwx, uint16_t max_w_px);
int      cwx_tr_pager_update(cwx_tr_pager_t *p, uint32_t pos_ms, int paused, cwx_tr_view_t *out);

#endif /* OPENFW_CWX_H */
