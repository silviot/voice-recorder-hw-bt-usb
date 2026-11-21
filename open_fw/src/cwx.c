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

    uint8_t copy_len = (tlen < (CWX_MAX_WORD_TEXT - 1u)) ? tlen : (uint8_t)(CWX_MAX_WORD_TEXT - 1u);
    out->text_len = copy_len;
    if (copy_len > 0u) {
        if (stream_read_exact(s, s->cur_byte_off + 11u, out->text, copy_len) != 0) {
            return -1;
        }
    }
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
