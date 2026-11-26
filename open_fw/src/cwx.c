/*
 * Smartlink SL6801 OpenFW Streaming Cassini Timed-Word (`CWX1`) Engine
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_cwx.h"

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d       = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
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

static int stream_read_exact(cwx_stream_t *s, uint32_t offset, void *dst, uint32_t len)
{
    if (!s || !s->read_fn || !dst) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    if (offset > s->file_size || len > (s->file_size - offset)) {
        return -1;
    }

    uint8_t *out = (uint8_t *)dst;
    uint32_t rem = len;
    uint32_t off = offset;

    while (rem > 0u) {
        if (s->cache_len > 0u && off >= s->cache_off && off < (s->cache_off + s->cache_len)) {
            uint32_t in_cache = off - s->cache_off;
            uint32_t avail    = s->cache_len - in_cache;
            uint32_t take     = (rem < avail) ? rem : avail;
            mem_copy(out, &s->cache[in_cache], take);
            out += take;
            off += take;
            rem -= take;
            continue;
        }

        if (rem >= sizeof(s->cache)) {
            uint32_t got = 0u;
            if (s->read_fn(s->user, off, out, rem, &got) != 0 || got != rem) {
                return -1;
            }
            return 0;
        }

        uint32_t chunk = s->file_size - off;
        if (chunk > (uint32_t)sizeof(s->cache)) {
            chunk = (uint32_t)sizeof(s->cache);
        }
        uint32_t got = 0u;
        if (s->read_fn(s->user, off, s->cache, chunk, &got) != 0 || got != chunk) {
            s->cache_len = 0u;
            return -1;
        }
        s->cache_off = off;
        s->cache_len = chunk;
    }
    return 0;
}

int cwx_open(cwx_stream_t *s, cwx_read_fn read_fn, void *user, uint32_t file_size)
{
    if (!s || !read_fn || file_size < CWX_HEADER_SIZE) {
        return -1;
    }

    mem_zero(s, sizeof(*s));
    s->read_fn   = read_fn;
    s->user      = user;
    s->file_size = file_size;

    uint8_t hdr[CWX_HEADER_SIZE];
    if (stream_read_exact(s, 0u, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (hdr[0] != 'C' || hdr[1] != 'W' || hdr[2] != 'X' || hdr[3] != '1') {
        return -1;
    }

    uint16_t version       = get_le16(&hdr[4]);
    uint16_t speaker_count = get_le16(&hdr[6]);
    uint32_t duration_ms   = get_le32(&hdr[8]);
    uint32_t word_count    = get_le32(&hdr[12]);
    uint32_t speakers_off  = get_le32(&hdr[16]);
    uint32_t seek_off      = get_le32(&hdr[20]);
    uint32_t seek_count    = get_le32(&hdr[24]);
    uint32_t words_off     = get_le32(&hdr[28]);

    if (version != 1u || speaker_count > CWX_MAX_SPEAKERS) {
        return -1;
    }
    uint32_t speakers_bytes = (uint32_t)speaker_count * CWX_SPEAKER_SIZE;
    if (speakers_off < CWX_HEADER_SIZE || speakers_off > file_size ||
        speakers_bytes > (file_size - speakers_off)) {
        return -1;
    }
    if (seek_off < (speakers_off + speakers_bytes) || seek_off > file_size ||
        seek_count > ((file_size - seek_off) / CWX_SEEK_SIZE)) {
        return -1;
    }
    uint32_t seek_bytes = seek_count * CWX_SEEK_SIZE;
    if (words_off < (seek_off + seek_bytes) || words_off > file_size) {
        return -1;
    }

    s->speaker_count = speaker_count;
    s->duration_ms   = duration_ms;
    s->word_count    = word_count;
    s->speakers_off  = speakers_off;
    s->seek_off      = seek_off;
    s->seek_count    = seek_count;
    s->words_off     = words_off;
    mem_copy(s->title, &hdr[32], 32u);
    s->title[31] = '\0';

    for (uint16_t i = 0u; i < speaker_count; i++) {
        uint8_t sp_raw[CWX_SPEAKER_SIZE];
        if (stream_read_exact(s, speakers_off + (uint32_t)i * CWX_SPEAKER_SIZE,
                              sp_raw, sizeof(sp_raw)) != 0) {
            return -1;
        }
        s->speakers[i].color_rgb565 = get_le16(&sp_raw[0]);
        s->speakers[i].flags        = sp_raw[2];
        s->speakers[i].reserved     = sp_raw[3];
        mem_copy(s->speakers[i].label, &sp_raw[4], 20u);
        s->speakers[i].label[19] = '\0';
    }

    s->cur_byte_off = s->words_off;
    s->cur_word_idx = 0u;
    return 0;
}

int cwx_seek_ms(cwx_stream_t *s, uint32_t target_ms)
{
    if (!s || !s->read_fn) {
        return -1;
    }
    if (s->word_count == 0u || s->seek_count == 0u) {
        s->cur_byte_off = s->words_off;
        s->cur_word_idx = 0u;
        return 0;
    }

    uint32_t sec = target_ms / 1000u;
    if (sec >= s->seek_count) {
        sec = s->seek_count - 1u;
    }

    uint8_t ent[CWX_SEEK_SIZE];
    if (stream_read_exact(s, s->seek_off + sec * CWX_SEEK_SIZE, ent, sizeof(ent)) != 0) {
        return -1;
    }

    uint32_t rel_off  = get_le32(&ent[0]);
    uint32_t word_idx = get_le32(&ent[4]);
    if (rel_off > (s->file_size - s->words_off) || word_idx > s->word_count) {
        return -1;
    }

    s->cur_byte_off = s->words_off + rel_off;
    s->cur_word_idx = word_idx;
    return 0;
}

int cwx_next_word(cwx_stream_t *s, cwx_word_t *out)
{
    if (!s || !out || !s->read_fn) {
        return -1;
    }
    if (s->cur_word_idx >= s->word_count || (s->file_size - s->cur_byte_off) < 11u) {
        return 0;
    }

    uint8_t whdr[11];
    if (stream_read_exact(s, s->cur_byte_off, whdr, sizeof(whdr)) != 0) {
        return -1;
    }

    uint8_t tlen = whdr[10];
    if ((uint32_t)tlen > (s->file_size - (s->cur_byte_off + 11u))) {
        return -1;
    }

    out->start_ms    = get_le32(&whdr[0]);
    out->end_ms      = get_le32(&whdr[4]);
    out->speaker_idx = whdr[8];
    out->flags       = whdr[9];
    out->word_idx    = s->cur_word_idx;

    uint8_t copy_len;
    if (tlen < CWX_MAX_WORD_TEXT) {
        copy_len = tlen;
        if (copy_len > 0u) {
            if (stream_read_exact(s, s->cur_byte_off + 11u, out->text, copy_len) != 0) {
                return -1;
            }
        }
    } else {
        uint8_t raw[CWX_MAX_WORD_TEXT];
        if (stream_read_exact(s, s->cur_byte_off + 11u, raw, CWX_MAX_WORD_TEXT) != 0) {
            return -1;
        }
        copy_len = (uint8_t)(CWX_MAX_WORD_TEXT - 1u);
        while (copy_len > 0u && (raw[copy_len] & 0xC0u) == 0x80u) {
            copy_len--;
        }
        if (copy_len > 0u) {
            mem_copy(out->text, raw, copy_len);
        }
    }
    out->text_len       = copy_len;
    out->text[copy_len] = '\0';

    s->cur_byte_off += 11u + (uint32_t)tlen;
    s->cur_word_idx++;
    return 1;
}

static void cwx_reverse_words(cwx_word_t *arr, uint32_t lo, uint32_t hi)
{
    while (lo < hi) {
        cwx_word_t tmp = arr[lo];
        arr[lo]        = arr[hi];
        arr[hi]        = tmp;
        lo++;
        hi--;
    }
}

int cwx_read_window(cwx_stream_t *s,
                    uint32_t pos_ms,
                    cwx_word_t *out_words,
                    uint32_t max_words,
                    int *out_active_idx)
{
    if (!s || !out_words || max_words == 0u) {
        return -1;
    }
    if (out_active_idx) {
        *out_active_idx = -1;
    }
    if (s->word_count == 0u) {
        return 0;
    }

    uint32_t seek_ms = (pos_ms > 2000u) ? (pos_ms - 2000u) : 0u;
    if (cwx_seek_ms(s, seek_ms) != 0) {
        return -1;
    }

    uint32_t head       = 0u;
    uint32_t count      = 0u;
    uint32_t after_best = 0u;
    uint32_t lookahead  = (max_words > 2u) ? (max_words / 2u) : 1u;
    int      best_idx   = -1;
    int      best_exact = 0;

    cwx_word_t cur;
    int rc;
    while ((rc = cwx_next_word(s, &cur)) > 0) {
        int is_exact  = (cur.start_ms <= pos_ms && pos_ms < cur.end_ms) ? 1 : 0;
        int take_best = 0;
        if (is_exact) {
            if (!best_exact) {
                take_best = 1;
            }
        } else if (!best_exact && cur.start_ms <= pos_ms) {
            take_best = 1;
        }

        if (count == max_words) {
            if (best_idx == 0 && !take_best) {
                break;
            }
            head++;
            if (head == max_words) {
                head = 0u;
            }
            count--;
            if (best_idx > 0) {
                best_idx--;
            } else if (best_idx == 0) {
                best_idx = -1;
            }
        }

        uint32_t slot = head + count;
        if (slot >= max_words) {
            slot -= max_words;
        }
        out_words[slot] = cur;
        if (take_best) {
            best_idx   = (int)count;
            best_exact = is_exact;
            after_best = 0u;
        } else if (best_idx >= 0) {
            after_best++;
        }
        count++;

        if (best_idx >= 0 && after_best >= lookahead &&
            (count >= max_words || (cur.start_ms > pos_ms && (cur.start_ms - pos_ms) > 2500u))) {
            break;
        }
        if (best_idx < 0 && count >= 1u &&
            (count >= max_words || (cur.start_ms > pos_ms && (cur.start_ms - pos_ms) > 5000u))) {
            break;
        }
    }
    if (rc < 0) {
        return -1;
    }

    /* Unwrap circular buffer in-place via 3 reversals when head rotated */
    if (head != 0u && count > 1u) {
        cwx_reverse_words(out_words, 0u, head - 1u);
        cwx_reverse_words(out_words, head, count - 1u);
        cwx_reverse_words(out_words, 0u, count - 1u);
    }

    if (out_active_idx) {
        *out_active_idx = (best_idx >= 0) ? best_idx : ((count > 0u) ? 0 : -1);
    }
    return (int)count;
}

const char *cwx_speaker_label(const cwx_stream_t *s, uint8_t speaker_idx)
{
    if (!s || speaker_idx >= s->speaker_count || s->speakers[speaker_idx].label[0] == '\0') {
        return "Speaker";
    }
    return s->speakers[speaker_idx].label;
}

uint16_t cwx_speaker_color(const cwx_stream_t *s, uint8_t speaker_idx)
{
    if (!s || speaker_idx >= s->speaker_count) {
        return 0xFFFFu; /* White fallback */
    }
    return s->speakers[speaker_idx].color_rgb565;
}

/* =========================================================================
 * Streaming Deterministic Transcript Line Paginator (160x80 Teleprompter)
 * ========================================================================= */

/* Exact integer pixel advances for ui_font_md (Noto Sans Medium 14px, U+0020..U+007E).
 * Verified glyph-for-glyph against ui_font_md in test_ui_render.c. */
static const uint8_t k_font_md_adv[95] = {
     4u,  4u,  6u,  9u,  8u, 12u, 10u,  3u,  4u,  4u,  8u,  8u,  4u,  5u,  4u,  5u,
     8u,  8u,  8u,  8u,  8u,  8u,  8u,  8u,  8u,  8u,  4u,  4u,  8u,  8u,  8u,  6u,
    13u,  9u,  9u,  9u, 10u,  8u,  7u, 10u, 10u,  5u,  4u,  9u,  8u, 13u, 11u, 11u,
     9u, 11u,  9u,  8u,  8u, 10u,  9u, 13u,  9u,  8u,  8u,  5u,  5u,  5u,  8u,  6u,
     4u,  8u,  9u,  7u,  9u,  8u,  5u,  9u,  9u,  4u,  4u,  8u,  4u, 13u,  9u,  9u,
     9u,  9u,  6u,  7u,  5u,  9u,  7u, 11u,  8u,  7u,  7u,  5u,  8u,  5u,  8u,
};

uint32_t cwx_measure_md_px(const char *s, uint32_t len)
{
    if (!s || len == 0u) {
        return 0u;
    }
    uint32_t w = 0u;
    uint32_t i = 0u;
    while (i < len && s[i] != '\0') {
        uint8_t c = (uint8_t)s[i];
        if (c >= 0x20u && c <= 0x7Eu) {
            w += k_font_md_adv[c - 0x20u];
            i++;
        } else if (c < 0x80u) {
            i++;
        } else {
            /* Step over multi-byte UTF-8 sequence as one glyph (~9px fallback) */
            uint32_t step = 1u;
            if ((c & 0xE0u) == 0xC0u)      step = 2u;
            else if ((c & 0xF0u) == 0xE0u) step = 3u;
            else if ((c & 0xF8u) == 0xF0u) step = 4u;
            w += 9u;
            i = (i + step <= len) ? (i + step) : len;
        }
    }
    return w;
}

static void cwx_str_copy_cap(char *dst, uint32_t cap, const char *src)
{
    if (!dst || cap == 0u) return;
    uint32_t i = 0u;
    while (src && src[i] != '\0' && (i + 1u) < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int tr_pull_word(cwx_tr_pager_t *p, cwx_word_t *out)
{
    if (p->has_pending) {
        *out           = p->pending_word;
        p->has_pending = 0;
        return 1;
    }
    if (p->at_eof || !p->cwx) {
        return 0;
    }
    int rc = cwx_next_word(p->cwx, out);
    if (rc <= 0) {
        p->at_eof = 1u;
        return 0;
    }
    return 1;
}

static int tr_wrap_next_line(cwx_tr_pager_t *p, cwx_tr_line_t *ln)
{
    cwx_word_t w;
    if (!tr_pull_word(p, &w)) {
        return 0;
    }

    mem_zero(ln, sizeof(*ln));
    ln->line_id        = p->next_line_id++;
    ln->first_word_idx = w.word_idx;
    ln->start_ms       = w.start_ms;
    ln->end_ms         = w.end_ms;
    ln->speaker_idx    = w.speaker_idx;
    ln->speaker_color  = cwx_speaker_color(p->cwx, w.speaker_idx);
    ln->turn_start     = ((w.flags & CWX_FLAG_TURN_START) != 0u ||
                          !p->has_prev_word ||
                          w.speaker_idx != p->prev_word_spk) ? 1u : 0u;

    uint32_t cur_px = 0u;
    const uint32_t sp_px = k_font_md_adv[0]; /* ' ' = 4 px */

    for (;;) {
        uint32_t wlen = (uint32_t)w.text_len;
        uint32_t wpx  = cwx_measure_md_px(w.text, wlen);
        uint32_t need_sp = (ln->word_count > 0u) ? 1u : 0u;

        if (ln->word_count > 0u) {
            int spk_break = ((w.flags & CWX_FLAG_TURN_START) != 0u ||
                             w.speaker_idx != ln->speaker_idx);
            if (spk_break ||
                ln->word_count >= CWX_TR_MAX_WPL ||
                (uint32_t)ln->text_len + need_sp + wlen >= CWX_TR_LINE_CAP ||
                cur_px + (need_sp ? sp_px : 0u) + wpx > (uint32_t)p->max_w_px) {
                p->pending_word = w;
                p->has_pending  = 1;
                break;
            }
        }

        if (need_sp) {
            ln->text[ln->text_len++] = ' ';
            cur_px += sp_px;
        }
        uint8_t off = ln->text_len;
        uint32_t copy_n = wlen;
        if (off + copy_n >= CWX_TR_LINE_CAP) {
            copy_n = (CWX_TR_LINE_CAP - 1u) - off;
        }
        if (copy_n > 0u) {
            mem_copy(&ln->text[off], w.text, copy_n);
            ln->text_len = (uint8_t)(off + copy_n);
        }
        ln->text[ln->text_len] = '\0';

        cwx_tr_wspan_t *ws = &ln->words[ln->word_count++];
        ws->start_ms = w.start_ms;
        ws->end_ms   = w.end_ms;
        ws->char_off = off;
        ws->char_len = (uint8_t)copy_n;

        if (w.end_ms > ln->end_ms) {
            ln->end_ms = w.end_ms;
        }
        cur_px += wpx;
        p->prev_word_spk = w.speaker_idx;
        p->has_prev_word = 1u;

        if (!tr_pull_word(p, &w)) {
            break;
        }
    }
    return 1;
}

static void tr_push_line(cwx_tr_pager_t *p, const cwx_tr_line_t *ln)
{
    if (p->count == CWX_TR_BUF_LINES) {
        for (uint32_t i = 1u; i < CWX_TR_BUF_LINES; i++) {
            p->lines[i - 1u] = p->lines[i];
        }
        p->lines[CWX_TR_BUF_LINES - 1u] = *ln;
    } else {
        p->lines[p->count++] = *ln;
    }
}

static int tr_find_active(const cwx_tr_pager_t *p, uint32_t pos_ms, int *out_line, int *out_word)
{
    int best_l = -1;
    int best_w = -1;

    for (uint32_t li = 0u; li < p->count; li++) {
        const cwx_tr_line_t *ln = &p->lines[li];
        for (uint8_t wi = 0u; wi < ln->word_count; wi++) {
            const cwx_tr_wspan_t *ws = &ln->words[wi];
            if (ws->start_ms <= pos_ms && pos_ms < ws->end_ms) {
                *out_line = (int)li;
                *out_word = (int)wi;
                return 1; /* exact speaking match */
            }
            if (ws->start_ms <= pos_ms) {
                best_l = (int)li;
                best_w = (int)wi;
            }
        }
    }

    if (best_l >= 0) {
        *out_line = best_l;
        *out_word = best_w;
        return 0; /* latest spoken word before pos_ms */
    }
    *out_line = 0;
    *out_word = -1; /* pos_ms precedes first word */
    return -1;
}

static int tr_reanchor(cwx_tr_pager_t *p, uint32_t pos_ms)
{
    if (!p || !p->cwx) return -1;

    p->count         = 0u;
    p->at_eof        = 0u;
    p->has_pending   = 0;
    p->has_prev_word = 0u;
    p->prev_word_spk = 0u;

    /* For short transcripts or near start of file, wrap cleanly from word 0 for
     * exact global line IDs; otherwise anchor 12 s before pos_ms. */
    uint32_t seek_ms = (pos_ms <= 20000u || p->cwx->word_count <= 320u)
                     ? 0u
                     : (pos_ms - 12000u);
    if (cwx_seek_ms(p->cwx, seek_ms) != 0) {
        return -1;
    }

    /* Advance next_line_id by a large jump on reanchor so UI snaps cleanly
     * instead of animating across unrelated seek targets. */
    if (p->initialized) {
        p->next_line_id += 1000;
    } else {
        p->next_line_id = 0;
    }

    cwx_tr_line_t ln;
    while (tr_wrap_next_line(p, &ln)) {
        tr_push_line(p, &ln);
        int act_l = 0, act_w = -1;
        tr_find_active(p, pos_ms, &act_l, &act_w);
        /* Keep reading until we have 4 lookahead lines past active (or >= 6 lines total) */
        if (p->count >= 6u && (int)p->count - 1 - act_l >= 4 && ln.start_ms >= pos_ms) {
            break;
        }
    }

    p->initialized = 1u;
    return 0;
}

void cwx_tr_pager_init(cwx_tr_pager_t *p, cwx_stream_t *cwx, uint16_t max_w_px)
{
    if (!p) return;
    mem_zero(p, sizeof(*p));
    p->cwx      = cwx;
    p->max_w_px = (max_w_px >= 40u) ? max_w_px : (uint16_t)CWX_TR_MAX_LINE_W;
}

int cwx_tr_pager_update(cwx_tr_pager_t *p, uint32_t pos_ms, int paused, cwx_tr_view_t *out)
{
    if (!p || !p->cwx || !out) {
        return -1;
    }
    mem_zero(out, sizeof(*out));
    for (uint32_t i = 0u; i < CWX_TR_VIEW_LINES; i++) {
        out->lines[i].line_id = -1;
    }
    out->active_slot = -1;
    out->top_line_id = -1;

    if (p->cwx->word_count == 0u) {
        return 0;
    }

    int need_reanchor = !p->initialized || (p->count == 0u);
    if (!need_reanchor) {
        if (pos_ms < p->lines[0].start_ms && p->lines[0].first_word_idx > 0u) {
            need_reanchor = 1;
        } else if (!p->at_eof && pos_ms > p->lines[p->count - 1u].end_ms + 8000u) {
            need_reanchor = 1;
        }
    }

    if (need_reanchor) {
        if (tr_reanchor(p, pos_ms) != 0) {
            return -1;
        }
    }

    if (p->count == 0u) {
        return 0;
    }

    /* Pull additional lookahead lines smoothly during forward playback */
    int act_l = 0, act_w = -1;
    tr_find_active(p, pos_ms, &act_l, &act_w);
    while (!p->at_eof && ((int)p->count - 1 - act_l) < 4) {
        cwx_tr_line_t ln;
        if (!tr_wrap_next_line(p, &ln)) {
            break;
        }
        tr_push_line(p, &ln);
        tr_find_active(p, pos_ms, &act_l, &act_w);
    }

    /* Choose top visible line index (maps to out->lines[1]):
     *  - Row 0 = previous line, Row 1 = active line, Rows 2..3 = next 2 lines
     *  - Clamped at start (act_l == 0 -> top = 0) and end (at_eof -> fill all 4 rows) */
    int top_i = (act_l > 0) ? (act_l - 1) : 0;
    if (p->at_eof) {
        int max_top = ((int)p->count > 4) ? ((int)p->count - 4) : 0;
        if (top_i > max_top) {
            top_i = max_top;
        }
    } else if (top_i > (int)p->count - 1) {
        top_i = (int)p->count - 1;
    }
    if (top_i < 0) top_i = 0;

    /* Copy 6-slot window: [0]=above, [1..4]=visible rows, [5]=below */
    for (uint32_t s = 0u; s < CWX_TR_VIEW_LINES; s++) {
        int idx = top_i - 1 + (int)s;
        if (idx >= 0 && idx < (int)p->count) {
            const cwx_tr_line_t *src = &p->lines[idx];
            out->lines[s].line_id       = src->line_id;
            out->lines[s].speaker_color = src->speaker_color;
            out->lines[s].speaker_idx   = src->speaker_idx;
            out->lines[s].turn_start    = src->turn_start;
            cwx_str_copy_cap(out->lines[s].text, CWX_TR_LINE_CAP, src->text);
        }
    }

    out->top_line_id = out->lines[1].line_id;
    int slot = act_l - top_i + 1;
    if (slot < 1) slot = 1;
    if (slot > 4) slot = 4;
    out->active_slot = (int8_t)slot;

    const cwx_tr_line_t *aln = &p->lines[act_l];
    out->speaker_idx   = aln->speaker_idx;
    out->speaker_color = aln->speaker_color;
    cwx_str_copy_cap(out->speaker_name, (uint32_t)sizeof(out->speaker_name),
                     cwx_speaker_label(p->cwx, aln->speaker_idx));

    if (act_w >= 0 && act_w < (int)aln->word_count) {
        const cwx_tr_wspan_t *ws = &aln->words[act_w];
        out->spoken_len = (uint8_t)(ws->char_off + ws->char_len);
        /* Keep word pill lit during active word and brief inter-word gaps (<= 320 ms),
         * or while paused so user sees exact resume position; hide during longer silences. */
        int speaking = (pos_ms >= ws->start_ms && pos_ms <= ws->end_ms + 320u) ? 1 : 0;
        if (speaking || paused) {
            out->word_start = ws->char_off;
            out->word_len   = ws->char_len;
        }
    } else {
        /* Before first word: show entire first line as upcoming */
        out->spoken_len = 0u;
        out->word_start = 0u;
        out->word_len   = 0u;
    }

    return 1;
}
