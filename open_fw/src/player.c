/*
 * Smartlink SL6801 OpenFW On-Device WAV & Cassini Timed-Word (`CWX1`) Player
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw.h"
#include "sl6801_regs.h"
#include "openfw_wav.h"
#include "openfw_cwx.h"
#include "openfw_player.h"

#define PLAYER_DAC_CHUNK_FRAMES 1024u

/*
 * Main playback buffers (16 KiB total):
 *   g_file_buf : 4096 B raw SD read buffer (used directly for 16-bit PCM)
 *   g_dec_pcm  : 8192 B (4096 samples) IMA-ADPCM decode buffer: fits
 *                block_align=2048 for both mono (4089) and stereo (2041) frames
 *   g_mono_tx  : 4096 B (1024 stereo frames) mono->stereo expansion staging
 */
static uint8_t  g_file_buf[4096] __attribute__((aligned(4)));
static int16_t  g_dec_pcm[4096]  __attribute__((aligned(4)));
static int16_t  g_mono_tx[PLAYER_DAC_CHUNK_FRAMES * 2u] __attribute__((aligned(4)));

/* Retain the continuation of the previous grain for waveform matching. */
#define SCRUB_OVERLAP_MAX 256u
static int16_t        g_scrub_tail[2u * SCRUB_OVERLAP_MAX];
static cwx_tr_pager_t g_tr_pager;

typedef struct {
    exfat_vol_t   *vol;
    exfat_entry_t *ent;
    uint32_t sector_off;
    uint32_t valid;
    uint8_t sector[512];
} exfat_cwx_io_t;

static int exfat_cwx_read_cb(void *user, uint32_t offset, void *dst, uint32_t len, uint32_t *out_read)
{
    exfat_cwx_io_t *io = (exfat_cwx_io_t *)user;
    if (!io || !io->vol || !io->ent) {
        return -1;
    }
    *out_read = 0u;
    while (len > 0u && offset < io->ent->size) {
        uint32_t base = offset & ~511u;
        if (!io->valid || io->sector_off != base) {
            if (exfat_read_file(io->vol, io->ent, base, io->sector, sizeof(io->sector),
                                &io->valid) != 0 || !io->valid) {
                return -1;
            }
            io->sector_off = base;
        }
        uint32_t pos = offset - base;
        if (pos >= io->valid) return -1;
        uint32_t take = io->valid - pos;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) {
            ((uint8_t *)dst)[*out_read + i] = io->sector[pos + i];
        }
        *out_read += take;
        offset += take;
        len -= take;
    }
    return 0;
}

static void str_copy_cap(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) {
        return;
    }
    while (src && src[i] != '\0' && (i + 1) < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void append_str(char *dst, int *pos, int max, const char *s)
{
    while (s && *s != '\0' && *pos < (max - 1)) {
        dst[(*pos)++] = *s++;
    }
}

static void build_path(char *out, int cap, const char *dir_path, const char *name)
{
    int pos = 0;
    if (!dir_path || dir_path[0] == '\0' || (dir_path[0] == '/' && dir_path[1] == '\0')) {
        append_str(out, &pos, cap, "/");
    } else {
        if (dir_path[0] != '/') {
            append_str(out, &pos, cap, "/");
        }
        append_str(out, &pos, cap, dir_path);
        if (pos > 0 && out[pos - 1] != '/') {
            append_str(out, &pos, cap, "/");
        }
    }
    append_str(out, &pos, cap, name);
    out[pos] = '\0';
}

static int make_cwx_path(char *out, int cap, const char *dir_path, const char *wav_name)
{
    char cwx_name[EXFAT_MAX_NAME];
    str_copy_cap(cwx_name, (int)sizeof(cwx_name), wav_name);
    int len = 0;
    while (cwx_name[len] != '\0') {
        len++;
    }
    if (len < 5 || cwx_name[len - 4] != '.') {
        return -1;
    }
    cwx_name[len - 3] = 'c';
    cwx_name[len - 2] = 'w';
    cwx_name[len - 1] = 'x';
    build_path(out, cap, dir_path, cwx_name);
    return 0;
}

static uint32_t frames_to_ms(uint32_t frames, uint32_t sample_rate)
{
    if (sample_rate == 0u || frames == 0u) {
        return 0u;
    }
    uint64_t ms = ((uint64_t)frames * 1000ull) / (uint64_t)sample_rate;
    return (ms > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)ms;
}

static uint8_t vbat_mv_to_pct(uint32_t mv)
{
    if (mv <= 3400u) return 0u;
    if (mv >= 4180u) return 100u;
    return (uint8_t)(((mv - 3400u) * 100u) / (4180u - 3400u));
}

static uint32_t populate_transcript_lines(ui_model_t *m, cwx_tr_pager_t *pager,
                                          uint32_t pos_ms, int paused)
{
    cwx_tr_view_t view;
    int rc = cwx_tr_pager_update(pager, pos_ms, paused, &view);

    for (uint32_t i = 0u; i < CWX_TR_VIEW_LINES; i++) {
        m->tr_lines[i] = view.lines[i];
    }
    m->tr_active_slot   = view.active_slot;
    m->tr_top_line_id   = view.top_line_id;
    m->tr_spoken_len    = view.spoken_len;
    m->tr_word_start    = view.word_start;
    m->tr_word_len      = view.word_len;
    m->tr_speaker       = view.speaker_idx;
    m->tr_speaker_color = view.speaker_color;
    str_copy_cap(m->tr_speaker_name, (int)sizeof(m->tr_speaker_name), view.speaker_name);

    if (rc <= 0) {
        const char *fallback = (pager && pager->cwx && pager->cwx->title[0])
                             ? pager->cwx->title
                             : m->play_file.filename;
        m->tr_lines[1].line_id       = 0;
        m->tr_lines[1].speaker_idx   = 0u;
        m->tr_lines[1].speaker_color = 0u;
        m->tr_lines[1].turn_start    = 1u;
        str_copy_cap(m->tr_lines[1].text, (int)sizeof(m->tr_lines[1].text), fallback);
        uint8_t flen = 0u;
        while (m->tr_lines[1].text[flen] != '\0') flen++;
        m->tr_active_slot = 1;
        m->tr_top_line_id = 0;
        m->tr_spoken_len  = flen;
        m->tr_word_start  = 0u;
        m->tr_word_len    = 0u;
    }

    /* Compact state signature to avoid redundant redraws between word/line transitions */
    uint32_t sig = (uint32_t)m->tr_top_line_id * 16777619u;
    sig ^= (uint32_t)(uint8_t)m->tr_active_slot;  sig *= 16777619u;
    sig ^= (uint32_t)m->tr_spoken_len;            sig *= 16777619u;
    sig ^= (uint32_t)m->tr_word_start << 8;       sig *= 16777619u;
    sig ^= (uint32_t)m->tr_word_len << 16;        sig *= 16777619u;
    sig ^= (uint32_t)m->tr_speaker << 24;         sig *= 16777619u;
    return sig;
}

void player_keys_init(player_keys_t *s, key_id_t initial, uint32_t now_ms)
{
    s->raw          = initial;
    s->raw_since    = now_ms;
    s->held         = initial;
    s->held_since   = now_ms;
    s->next_repeat  = now_ms;
    s->hold_active  = 0u;
    s->ignore       = (initial != KEY_NONE) ? 1u : 0u;
    s->tap_key      = KEY_NONE;
    s->tap_deadline = now_ms;
}

static int time_reached(uint32_t now_ms, uint32_t when_ms)
{
    return (int32_t)(now_ms - when_ms) >= 0;
}

static int is_seek_key(key_id_t k)
{
    return k == KEY_LEFT || k == KEY_RIGHT;
}

player_action_t player_keys_tick(player_keys_t *s, uint32_t now_ms)
{
    /* A single tap whose double-tap window ran out becomes one seek step */
    if (s->tap_key != KEY_NONE && time_reached(now_ms, s->tap_deadline)) {
        key_id_t k = s->tap_key;
        s->tap_key = KEY_NONE;
        return (k == KEY_RIGHT) ? PLAYER_ACT_SEEK_FWD : PLAYER_ACT_SEEK_BACK;
    }

    if (s->held == KEY_NONE || s->ignore) {
        return PLAYER_ACT_NONE;
    }
    if ((s->held == KEY_VOL_UP || s->held == KEY_VOL_DOWN) &&
        time_reached(now_ms, s->next_repeat)) {
        s->next_repeat = now_ms + PLAYER_KEY_REPEAT_MS;
        return (s->held == KEY_VOL_UP) ? PLAYER_ACT_VOL_UP : PLAYER_ACT_VOL_DOWN;
    }
    if (is_seek_key(s->held) && !s->hold_active && time_reached(now_ms, s->next_repeat)) {
        s->hold_active = 1u;
        return (s->held == KEY_RIGHT) ? PLAYER_ACT_SCRUB_FWD : PLAYER_ACT_SCRUB_BACK;
    }
    return PLAYER_ACT_NONE;
}

player_action_t player_keys_edge(player_keys_t *s, key_id_t key, uint32_t now_ms)
{
    if (key == s->held) {
        return PLAYER_ACT_NONE;
    }

    /* Debounced change. A LEFT/RIGHT let go before its hold started is a tap. */
    if (is_seek_key(s->held) && !s->hold_active && !s->ignore) {
        s->tap_key      = s->held;
        s->tap_deadline = now_ms + PLAYER_KEY_DOUBLE_TAP_MS;
    }

    s->held        = key;
    s->held_since  = now_ms;
    s->hold_active = 0u;
    s->ignore      = 0u;

    if (is_seek_key(key)) {
        if (s->tap_key == key) {
            /* Second tap of the same key inside the window */
            s->tap_key = KEY_NONE;
            s->ignore  = 1u;
            return (key == KEY_RIGHT) ? PLAYER_ACT_NEXT_FILE : PLAYER_ACT_PREV_FILE;
        }
        s->next_repeat = now_ms + PLAYER_KEY_HOLD_MS;
        if (s->tap_key != KEY_NONE) {
            /* Pending tap of the other key: settle it now */
            key_id_t k = s->tap_key;
            s->tap_key = KEY_NONE;
            return (k == KEY_RIGHT) ? PLAYER_ACT_SEEK_FWD : PLAYER_ACT_SEEK_BACK;
        }
        return PLAYER_ACT_NONE;
    }

    if (key != KEY_NONE && s->tap_key != KEY_NONE) {
        /* Deliver the new control immediately. A different key rules out a
         * double tap, so the next tick can settle the pending seek. */
        s->tap_deadline = now_ms;
    }

    switch (key) {
    case KEY_NONE:
        return PLAYER_ACT_NONE;
    case KEY_ENTER:
        return PLAYER_ACT_PLAY_PAUSE;
    case KEY_BACK:
        return PLAYER_ACT_BACK;
    case KEY_VOL_UP:
    case KEY_VOL_DOWN:
        s->next_repeat = now_ms + PLAYER_KEY_REPEAT_DELAY_MS;
        return (key == KEY_VOL_UP) ? PLAYER_ACT_VOL_UP : PLAYER_ACT_VOL_DOWN;
    default:
        s->ignore = 1u;
        return PLAYER_ACT_NONE;
    }
}

player_action_t player_keys_step(player_keys_t *s, key_id_t raw, uint32_t now_ms)
{
    /* A single tap whose double-tap window ran out becomes one seek step */
    if (s->tap_key != KEY_NONE && time_reached(now_ms, s->tap_deadline)) {
        key_id_t k = s->tap_key;
        s->tap_key = KEY_NONE;
        return (k == KEY_RIGHT) ? PLAYER_ACT_SEEK_FWD : PLAYER_ACT_SEEK_BACK;
    }

    /* Debounce: the resistor ladder passes through other keys' voltage bands
     * while a button moves, so act only on a decode that stayed put. */
    if (raw != s->raw) {
        s->raw       = raw;
        s->raw_since = now_ms;
        return PLAYER_ACT_NONE;
    }
    if (!time_reached(now_ms, s->raw_since + PLAYER_KEY_DEBOUNCE_MS)) {
        return PLAYER_ACT_NONE;
    }

    if (raw == s->held) {
        return player_keys_tick(s, now_ms);
    }
    return player_keys_edge(s, raw, now_ms);
}

static uint32_t player_heard_frames(uint32_t played_frames, uint32_t skip_frames)
{
    uint32_t queued = audio_dac_queued_frames();
    uint32_t base   = (played_frames > queued) ? (played_frames - queued) : 0u;
    return base + skip_frames;
}

static void player_seek_to_frame(const wav_stream_info_t *winfo, uint64_t target,
                                 uint32_t *data_off, uint32_t *played_frames)
{
    if (target >= (uint64_t)winfo->total_samples) {
        target = (winfo->total_samples > 0u) ? (uint64_t)(winfo->total_samples - 1u) : 0u;
    }
    uint64_t off64;
    if (winfo->format_tag == WAV_FMT_PCM) {
        off64          = target * (uint64_t)winfo->block_align;
        *played_frames = (uint32_t)target;
    } else {
        uint32_t spb     = (winfo->samples_per_block > 0u) ? (uint32_t)winfo->samples_per_block : 1u;
        uint64_t blk_idx = target / (uint64_t)spb;
        off64            = blk_idx * (uint64_t)winfo->block_align;
        *played_frames   = (uint32_t)(blk_idx * (uint64_t)spb);
    }
    if (off64 > (uint64_t)winfo->data_bytes) {
        off64 = (uint64_t)winfo->data_bytes;
    }
    *data_off = (uint32_t)off64;
}

/* Both directions use the normal PCM/IMA staging buffers. Read a group of
 * complete codec blocks, decode forwards, then walk the PCM in either direction. */
typedef struct {
    int speed;               /* signed source frames per output frame */
    uint32_t submitted;
    uint32_t rate_start[PLAYER_SCRUB_MAX_RATE - 2u]; /* frames submitted before each increase */
    int64_t next;            /* source boundary after the last submitted sample */
    uint32_t cache_start;
    uint32_t cache_frames;
    uint32_t pending_pos;
    uint32_t pending_count;
    uint32_t overlap;
    int finished;
} scrub_t;

static uint32_t scrub_heard(const scrub_t *s, uint32_t total)
{
    uint32_t queued = audio_dac_queued_frames();
    uint32_t rate = (uint32_t)(s->speed > 0 ? s->speed : -s->speed);
    int64_t distance = 2 * (int64_t)queued;
    for (uint32_t step = 0; step < rate - 2u; step++) {
        uint32_t frames = s->submitted - s->rate_start[step];
        distance += frames < queued ? frames : queued;
    }
    int64_t pos = s->next + (s->speed > 0 ? -distance : distance);
    if (pos < 0) return 0;
    if (pos > total) return total;
    return (uint32_t)pos;
}

static uint32_t decode_blocks(const wav_stream_info_t *w, uint32_t bytes)
{
    uint32_t decoded = 0;
    uint32_t capacity = (sizeof(g_dec_pcm) / sizeof(g_dec_pcm[0])) / w->channels;
    for (uint32_t off = 0; off < bytes; off += w->block_align) {
        uint32_t len = bytes - off;
        if (len > w->block_align) len = w->block_align;
        int n = wav_decode_ima_adpcm_block(g_file_buf + off, len, w->channels,
                                           g_dec_pcm + decoded * w->channels, capacity - decoded);
        if (n <= 0) return 0;
        decoded += (uint32_t)n;
    }
    return decoded;
}

static int scrub_load(scrub_t *s, exfat_vol_t *vol, const exfat_entry_t *file,
                      const wav_stream_info_t *w, uint32_t frame)
{
    uint32_t blocks = sizeof(g_file_buf) / w->block_align;
    if (w->format_tag == WAV_FMT_IMA_ADPCM) {
        uint32_t fit = (sizeof(g_dec_pcm) / sizeof(g_dec_pcm[0])) /
                       (w->samples_per_block * (uint32_t)w->channels);
        if (blocks > fit) blocks = fit;
    }
    uint32_t span = blocks * w->samples_per_block;
    s->cache_start = frame - frame % span;
    uint64_t off = (uint64_t)(s->cache_start / w->samples_per_block) * w->block_align;
    if (off >= w->data_bytes) return 0;
    uint32_t len = w->data_bytes - (uint32_t)off;
    if (len > blocks * w->block_align) len = blocks * w->block_align;
    uint32_t got = 0;
    if (exfat_read_file(vol, file, w->data_offset + off, g_file_buf, len, &got) || got != len)
        return 0;
    s->cache_frames = w->format_tag == WAV_FMT_PCM ? len / w->block_align : decode_blocks(w, len);
    return frame - s->cache_start < s->cache_frames;
}

/* Copy at the original sample rate, preserving stereo pairs in reverse. */
static int scrub_copy(scrub_t *s, exfat_vol_t *vol, const exfat_entry_t *file,
                      const wav_stream_info_t *w, const int16_t *pcm,
                      int64_t frame, int direction, int16_t *out, uint32_t count)
{
    while (count) {
        uint32_t index = (uint32_t)frame - s->cache_start;
        if (index >= s->cache_frames) {
            if (!scrub_load(s, vol, file, w, (uint32_t)frame)) return 0;
            index = (uint32_t)frame - s->cache_start;
        }
        uint32_t run = direction > 0 ? s->cache_frames - index : index + 1u;
        if (run > count) run = count;
        for (uint32_t n = 0; n < run; n++) {
            const int16_t *sample = pcm + index * w->channels;
            *out++ = sample[0];
            *out++ = sample[w->channels - 1];
            index += direction;
        }
        frame += (int32_t)run * direction;
        count -= run;
    }
    return 1;
}

/* Search a short window for the best match to both channels of the old tail.
 * Sparse comparisons bound CPU time; the splice itself is sample-accurate. */
static uint32_t scrub_match(uint32_t search, uint32_t overlap)
{
    uint32_t best = 0, best_error = UINT32_MAX;
    uint32_t stride = 1u + overlap / 32u;
    for (uint32_t shift = 0; shift <= search; shift++) {
        uint32_t error = 0;
        for (uint32_t i = 0; i < overlap; i += stride) {
            for (unsigned ch = 0; ch < 2; ch++) {
                int diff = g_mono_tx[2u * (i + shift) + ch] - g_scrub_tail[2u * i + ch];
                error += (uint32_t)(diff < 0 ? -diff : diff);
            }
            if (error >= best_error) break;
        }
        if (error < best_error) {
            best_error = error;
            best = shift;
        }
    }
    return best;
}

/* Waveform-similarity overlap-add: advance the file clock at seek speed, but play
 * contiguous audio at 1x inside each grain. Align then crossfade the joins. */
static int scrub_grain(scrub_t *s, exfat_vol_t *vol, const exfat_entry_t *file,
                       const wav_stream_info_t *w, const int16_t *pcm)
{
    int direction = s->speed > 0 ? 1 : -1;
    int64_t frame = s->next - (direction < 0);
    if (frame < 0 || frame >= w->total_samples) return 0;
    uint32_t available = direction > 0 ? w->total_samples - (uint32_t)frame : (uint32_t)frame + 1u;
    uint32_t hop = w->sample_rate / 32u;
    if (hop > PLAYER_DAC_CHUNK_FRAMES) hop = PLAYER_DAC_CHUNK_FRAMES;
    uint32_t rate = (uint32_t)(direction * s->speed);
    uint32_t output_left = 1u + (available - 1u) / rate;
    if (hop > output_left) hop = output_left;
    uint32_t blend = s->overlap < hop ? s->overlap : hop;
    uint32_t shift = 0;
    if (blend) {
        uint32_t search = w->sample_rate / 80u;
        if (search > PLAYER_DAC_CHUNK_FRAMES - blend) search = PLAYER_DAC_CHUNK_FRAMES - blend;
        if (search > available - hop) search = available - hop;
        if (!scrub_copy(s, vol, file, w, pcm, frame, direction, g_mono_tx, search + blend)) return 0;
        shift = scrub_match(search, blend);
    }
    frame += direction * (int32_t)shift;
    if (!scrub_copy(s, vol, file, w, pcm, frame, direction, g_mono_tx, hop)) return 0;
    for (uint32_t i = 0; i < blend; i++) {
        for (unsigned ch = 0; ch < 2; ch++) {
            uint32_t k = 2u * i + ch;
            int32_t delta = (int32_t)g_mono_tx[k] - g_scrub_tail[k];
            g_mono_tx[k] = (int16_t)(g_scrub_tail[k] + delta * (int32_t)(i + 1u) / (int32_t)blend);
        }
    }
    uint32_t tail = w->sample_rate / 160u;
    if (tail > SCRUB_OVERLAP_MAX) tail = SCRUB_OVERLAP_MAX;
    if (tail > available - shift - hop) tail = available - shift - hop;
    if (!scrub_copy(s, vol, file, w, pcm, frame + direction * (int32_t)hop, direction,
                    g_scrub_tail, tail)) return 0;
    s->overlap = tail;
    s->pending_count = hop;
    s->pending_pos = 0;
    return 1;
}

static void scrub_queue(scrub_t *s, exfat_vol_t *vol, const exfat_entry_t *file,
                        const wav_stream_info_t *w, const int16_t *pcm, uint32_t held_ms)
{
    for (unsigned slot = 0; slot < AUDIO_DAC_QUEUE_SLOTS; slot++) {
        if (!s->pending_count) {
            int direction = s->speed > 0 ? 1 : -1;
            unsigned rate = (unsigned)(direction * s->speed);
            while (rate < PLAYER_SCRUB_MAX_RATE &&
                   held_ms >= PLAYER_KEY_HOLD_MS + (rate - 1u) * PLAYER_SCRUB_STEP_MS) {
                s->rate_start[rate - 2u] = s->submitted;
                rate++;
            }
            s->speed = direction * (int)rate;
        }
        if (!s->pending_count && (s->finished || !scrub_grain(s, vol, file, w, pcm))) {
            s->finished = 1; /* End of this directional pass, or SD read error. */
            break;
        }
        int sent = audio_dac_write_stereo(g_mono_tx + 2 * s->pending_pos, s->pending_count);
        if (sent <= 0) break;
        s->pending_pos += (uint32_t)sent;
        s->pending_count -= (uint32_t)sent;
        s->submitted += (uint32_t)sent;
        s->next += (int64_t)sent * s->speed;
    }
}

static void player_pause_rewind(const wav_stream_info_t *winfo,
                                uint32_t *data_off,
                                uint32_t *played_frames,
                                uint32_t *buf_pos,
                                uint32_t *buf_frames,
                                uint32_t *skip_frames)
{
    uint32_t heard = player_heard_frames(*played_frames, *skip_frames);
    player_seek_to_frame(winfo, (uint64_t)heard, data_off, played_frames);
    *skip_frames = (heard > *played_frames) ? (heard - *played_frames) : 0u;
    *buf_pos     = 0u;
    *buf_frames  = 0u;
    audio_dac_flush();
}

int openfw_play_file_ui(exfat_vol_t *vol, const char *dir_path, const char *wav_name,
                        ui_model_t *m)
{
    if (!vol || !wav_name || wav_name[0] == '\0') {
        return 0;
    }

    player_keys_t keys;
    player_keys_init(&keys, keys_flush_events(), millis());

    ui_model_t local_m;
    if (!m) {
        for (uint32_t i = 0u; i < sizeof(local_m); i++) {
            ((uint8_t *)&local_m)[i] = 0u;
        }
        ui_settings_defaults(&local_m.cfg);
        m = &local_m;
    }

    char wav_path[96];
    build_path(wav_path, (int)sizeof(wav_path), dir_path, wav_name);

    exfat_entry_t wav_ent;
    if (exfat_find_path(vol, wav_path, &wav_ent) != 0 || wav_ent.is_dir || wav_ent.size < 44u) {
        return 0;
    }

    /* Fast path: parse standard WAV headers within the first 512 B sector.
     * Fall back to scanning up to 4096 B when LIST/bext/JUNK chunks precede data. */
    wav_stream_info_t winfo;
    uint32_t hdr_read = 0u;
    uint32_t hdr_want = (wav_ent.size > 512u) ? 512u : (uint32_t)wav_ent.size;
    if (exfat_read_file(vol, &wav_ent, 0u, g_file_buf, hdr_want, &hdr_read) != 0 || hdr_read < 44u) {
        return 0;
    }
    if (wav_parse_stream(g_file_buf, hdr_read, &winfo) != 0) {
        if (wav_ent.size <= 512u) {
            return 0;
        }
        uint32_t scan_want = (wav_ent.size > sizeof(g_file_buf))
                           ? (uint32_t)sizeof(g_file_buf)
                           : (uint32_t)wav_ent.size;
        if (exfat_read_file(vol, &wav_ent, 0u, g_file_buf, scan_want, &hdr_read) != 0 ||
            wav_parse_stream(g_file_buf, hdr_read, &winfo) != 0) {
            return 0;
        }
    }
    if (winfo.block_align == 0u || winfo.block_align > sizeof(g_file_buf)) {
        return 0;
    }
    if ((uint64_t)winfo.data_offset > wav_ent.size) {
        return 0;
    }
    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
        uint32_t dec_cap = (uint32_t)(sizeof(g_dec_pcm) / sizeof(g_dec_pcm[0])) / (uint32_t)winfo.channels;
        uint32_t hdr_bytes = (uint32_t)winfo.channels * 4u;
        uint32_t blk_frames = 1u + ((uint32_t)(winfo.block_align - hdr_bytes) * 2u) / (uint32_t)winfo.channels;
        if (blk_frames > dec_cap) {
            return 0;
        }
    }

    /* Clamp data_bytes to actual file size so truncated WAV headers never read past EOF */
    uint64_t avail_bytes = wav_ent.size - (uint64_t)winfo.data_offset;
    if ((uint64_t)winfo.data_bytes > avail_bytes) {
        winfo.data_bytes = (avail_bytes > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)avail_bytes;
        uint64_t max_samples;
        if (winfo.format_tag == WAV_FMT_PCM) {
            max_samples = (uint64_t)(winfo.data_bytes / winfo.block_align);
        } else {
            uint32_t full_blocks = winfo.data_bytes / winfo.block_align;
            max_samples = (uint64_t)full_blocks * (uint64_t)winfo.samples_per_block;
        }
        if ((uint64_t)winfo.total_samples > max_samples) {
            winfo.total_samples = (max_samples > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)max_samples;
        }
        winfo.duration_ms = frames_to_ms(winfo.total_samples, winfo.sample_rate);
    }

    /* Optional companion Cassini timed-word sidecar (.cwx) */
    int has_cwx = 0;
    cwx_stream_t cwx;
    exfat_entry_t cwx_ent;
    exfat_cwx_io_t cwx_io = { .vol = vol, .ent = &cwx_ent };
    char cwx_path[96];
    if (make_cwx_path(cwx_path, (int)sizeof(cwx_path), dir_path, wav_name) == 0 &&
        exfat_find_path(vol, cwx_path, &cwx_ent) == 0 && !cwx_ent.is_dir &&
        cwx_ent.size <= 0xFFFFFFFFu &&
        cwx_open(&cwx, exfat_cwx_read_cb, &cwx_io, (uint32_t)cwx_ent.size) == 0) {
        has_cwx = 1;
        cwx_tr_pager_init(&g_tr_pager, &cwx, CWX_TR_MAX_LINE_W);
    }

    int hp_in = keys_read_hp_detect();
    uint8_t volume = hp_in ? m->cfg.hp_vol : m->cfg.spk_vol;
    if (volume > 31u) {
        volume = 18u;
    }
    if (audio_dac_start(winfo.sample_rate, volume) != 0) {
        return 0;
    }
    /* Re-sample PB11 after codec analog bias is enabled by audio_dac_start() */
    int hp_post = keys_read_hp_detect();
    if (hp_post != hp_in) {
        hp_in  = hp_post;
        volume = hp_in ? m->cfg.hp_vol : m->cfg.spk_vol;
        if (volume > 31u) {
            volume = 18u;
        }
        audio_dac_set_volume(volume, 0x1FFu);
    }
    /* Route audio to built-in speaker PA (PB30) when no 3.5 mm jack is inserted */
    audio_pa_set(!hp_in);

    /* Panel is already configured for 160x80 landscape at boot; avoid touching LCDC
     * directly from CPU0 while CPU1 owns the display controller. */

    uint32_t dur_ms = (has_cwx && cwx.duration_ms > 0u) ? cwx.duration_ms : winfo.duration_ms;
    str_copy_cap(m->play_file.filename, (int)sizeof(m->play_file.filename), wav_name);
    m->play_file.duration_sec   = (dur_ms + 500u) / 1000u;
    m->play_file.has_transcript = (uint8_t)has_cwx;
    m->play_file.channels       = (uint8_t)winfo.channels;
    m->play_has_transcript      = has_cwx;
    m->play_vol_hi              = 0u;
    m->play_vu_l                = 0u;
    m->play_vu_r                = 0u;

    uint32_t data_off      = 0u;
    uint32_t played_frames = 0u;
    if (m->play_file.resume_sec > 0u && winfo.sample_rate > 0u) {
        uint64_t target = (uint64_t)m->play_file.resume_sec * (uint64_t)winfo.sample_rate;
        if (target < (uint64_t)winfo.total_samples) {
            player_seek_to_frame(&winfo, target, &data_off, &played_frames);
        }
    }
    const int16_t *pcm_buf = (winfo.format_tag == WAV_FMT_PCM)
                           ? (const int16_t *)(const void *)g_file_buf
                           : g_dec_pcm;
    uint32_t buf_pos       = 0u;
    uint32_t buf_frames    = 0u;
    uint32_t skip_frames   = 0u;
    uint8_t  peak_l_pct    = 0u;
    uint8_t  peak_r_pct    = 0u;
    scrub_t  scrub         = {0};
    int      paused        = 0;
    int      eof_reached   = 0;
    uint32_t telemetry_ms    = millis() - 1000u;
    uint32_t last_ui_ms      = 0u;
    uint32_t last_tr_poll_ms = 0u;
    uint32_t last_anim_ms    = 0u;
    uint32_t last_tr_sig     = 0u;
    uint32_t last_vol_ms     = 0u;
    uint32_t toast_until     = 0u;
    int      hp_cand         = hp_in;
    uint32_t hp_cand_since   = millis();
    int      ui_dirty        = 1;
    int      ret_code        = 0;


    while (1) {
        wdt_feed();
        if (keys_read_rec_switch()) {
            ret_code = 2;
            break;
        }

        uint32_t now = millis();
        int show_player = ui_poll_power_button(UI_SCR_PLAYER, m, now);
        if (toast_until != 0u && (int32_t)(now - toast_until) >= 0) {
            toast_until     = 0u;
            m->toast_msg[0] = '\0';
            ui_dirty        = 1;
        }
        player_action_t act = PLAYER_ACT_NONE;
        if (!show_player) {
            /* Screen is off (or power-off hold active): button presses are no-ops. */
            key_id_t held_now = keys_flush_events();
            player_keys_init(&keys, held_now, now);
        } else {
            key_event_t ev;
            while (keys_peek_event(&ev)) {
                act = player_keys_tick(&keys, ev.t_ms);
                if (act != PLAYER_ACT_NONE) {
                    break;
                }
                if (!keys_pop_event(&ev)) break;
                act = player_keys_edge(&keys, ev.pressed ? ev.key : KEY_NONE, ev.t_ms);
                if (act != PLAYER_ACT_NONE) {
                    break;
                }
            }
            if (act == PLAYER_ACT_NONE) {
                /* Queue overflow suppresses the physical key until release. Cancel
                 * any stale hold before it can generate another timed action. */
                key_id_t stable = keys_state(NULL);
                if (stable != keys.held && !keys_event_pending()) {
                    player_keys_init(&keys, stable, now);
                }
                act = player_keys_tick(&keys, now);
            }
        }
        int raw_hp = keys_read_hp_detect();
        if (raw_hp != hp_cand) {
            hp_cand       = raw_hp;
            hp_cand_since = now;
        }
        int cur_hp = ((uint32_t)(now - hp_cand_since) >= 30u) ? hp_cand : hp_in;
        int unplug = hp_in && !cur_hp;
        if (scrub.speed && (!keys.hold_active || unplug ||
                            (!keys_event_pending() && keys_state(NULL) != keys.held))) {
            uint32_t heard = scrub_heard(&scrub, winfo.total_samples);
            audio_dac_flush();
            scrub.speed = 0;
            player_seek_to_frame(&winfo, heard, &data_off, &played_frames);
            skip_frames = heard - played_frames;
            buf_pos = buf_frames = 0;
            eof_reached = heard == winfo.total_samples;
            if (eof_reached) played_frames = heard;
            audio_dac_set_mute(paused || eof_reached);
            audio_pa_set(!paused && !eof_reached && !hp_in && !unplug);
            ui_dirty = 1;
        }
        if (cur_hp != hp_in) {
            int was_hp = hp_in;
            hp_in  = cur_hp;
            volume = hp_in ? m->cfg.hp_vol : m->cfg.spk_vol;
            audio_dac_set_volume(volume, 0x1FFu);
            if (was_hp && !hp_in && !paused && !eof_reached) {
                paused = 1;
                audio_pa_set(0);
                audio_dac_set_mute(1);
                player_pause_rewind(&winfo, &data_off, &played_frames,
                                    &buf_pos, &buf_frames, &skip_frames);
                str_copy_cap(m->toast_msg, (int)sizeof(m->toast_msg),
                             "Headphones out \xC2\xB7 paused");
                m->toast_col = UI_COL_AMBER;
                toast_until  = now + 1500u;
            } else {
                audio_pa_set((!hp_in && !paused && !eof_reached) ? 1 : 0);
            }
            ui_dirty = 1;
        }

        int file_dir = 0; /* -1: previous file, +1: next file */
        if (act == PLAYER_ACT_BACK) {
            ret_code = 0;
            break;
        } else if (act == PLAYER_ACT_PLAY_PAUSE) {
            if (eof_reached) {
                data_off      = 0u;
                played_frames = 0u;
                buf_pos       = 0u;
                buf_frames    = 0u;
                skip_frames   = 0u;
                eof_reached   = 0;
                paused        = 0;
                audio_dac_flush();
            } else {
                paused ^= 1;
                if (paused) {
                    audio_dac_set_mute(1);
                    audio_pa_set(0);
                    player_pause_rewind(&winfo, &data_off, &played_frames,
                                        &buf_pos, &buf_frames, &skip_frames);
                }
            }
            audio_dac_set_mute(paused);
            audio_pa_set((!hp_in && !paused && !eof_reached) ? 1 : 0);
            ui_dirty = 1;
        } else if (act == PLAYER_ACT_VOL_UP || act == PLAYER_ACT_VOL_DOWN) {
            if (act == PLAYER_ACT_VOL_UP && volume < 31u) {
                volume++;
            } else if (act == PLAYER_ACT_VOL_DOWN && volume > 0u) {
                volume--;
            }
            if (hp_in) {
                m->cfg.hp_vol = volume;
            } else {
                m->cfg.spk_vol = volume;
            }
            audio_dac_set_volume(volume, 0x1FFu);
            last_vol_ms = now ? now : 1u;
            ui_dirty    = 1;
        } else if (act == PLAYER_ACT_PREV_FILE || act == PLAYER_ACT_NEXT_FILE) {
            file_dir = (act == PLAYER_ACT_NEXT_FILE) ? 1 : -1;
        } else if ((act == PLAYER_ACT_SCRUB_BACK || act == PLAYER_ACT_SCRUB_FWD) && !unplug) {
            uint32_t heard = eof_reached ? played_frames : player_heard_frames(played_frames, skip_frames);
            audio_dac_flush();
            scrub = (scrub_t){ .speed = act == PLAYER_ACT_SCRUB_FWD ? 2 : -2, .next = heard };
            audio_dac_set_mute(0);
            audio_pa_set(!hp_in);
            ui_dirty = 1;
        } else if (act == PLAYER_ACT_SEEK_BACK || act == PLAYER_ACT_SEEK_FWD) {
            int fwd = act == PLAYER_ACT_SEEK_FWD;
            /* A tap keeps the old meaning in "Track" skip mode and at the end of
             * the file; a hold always seeks. */
            if ((eof_reached && fwd) || (!has_cwx && m->cfg.skip_mode == UI_SKIP_TRACK)) {
                file_dir = fwd ? 1 : -1;
            } else {
                int32_t step_s;
                if (m->cfg.skip_mode == UI_SKIP_5_10) {
                    step_s = fwd ? 10 : -5;
                } else {
                    step_s = fwd ? 30 : -10;
                }
                uint32_t heard = player_heard_frames(played_frames, skip_frames);
                int64_t delta  = (int64_t)winfo.sample_rate * (int64_t)step_s;
                int64_t target = (int64_t)heard + delta;
                if (target < 0) {
                    target = 0;
                }
                player_seek_to_frame(&winfo, (uint64_t)target, &data_off, &played_frames);
                buf_pos     = 0u;
                buf_frames  = 0u;
                skip_frames = 0u;
                eof_reached = 0;
                audio_dac_flush();
                audio_pa_set((!hp_in && !paused) ? 1 : 0);
                ui_dirty    = 1;
            }
        }
        if (file_dir != 0) {
            if ((file_dir > 0) ? m->play_has_next : m->play_has_prev) {
                ret_code = file_dir;
                break;
            }
            str_copy_cap(m->toast_msg, (int)sizeof(m->toast_msg),
                         (file_dir > 0) ? "End of list" : "Start of list");
            m->toast_col = UI_COL_AMBER;
            toast_until  = now + 1500u;
            ui_dirty     = 1;
        }

        /* Refill PCM decode staging buffer & fill the DAC queue before doing display work */
        if (scrub.speed) {
            scrub_queue(&scrub, vol, &wav_ent, &winfo, pcm_buf, millis() - keys.held_since);
        } else if (!paused && !eof_reached) {
            for (unsigned iter = 0; iter < AUDIO_DAC_QUEUE_SLOTS; iter++) {
                if (buf_pos >= buf_frames) {
                    if (data_off >= winfo.data_bytes || played_frames >= winfo.total_samples) {
                        if (audio_dac_is_busy()) break;
                        eof_reached = 1;
                        peak_l_pct  = 0u;
                        peak_r_pct  = 0u;
                        audio_pa_set(0);
                        ui_dirty    = 1;
                        break;
                    }
                    uint32_t rem = winfo.data_bytes - data_off;
                    uint32_t chunk;
                    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
                        uint32_t frames_cap = (uint32_t)(sizeof(g_dec_pcm) / sizeof(g_dec_pcm[0])) / winfo.channels;
                        uint32_t blocks = frames_cap / winfo.samples_per_block;
                        chunk = blocks * (uint32_t)winfo.block_align;
                        if (chunk > sizeof(g_file_buf)) chunk = (uint32_t)sizeof(g_file_buf);
                    } else {
                        chunk = (uint32_t)sizeof(g_file_buf);
                        /* Align the first unaligned PCM read to a 512 B file boundary so
                         * subsequent reads take exfat_read_file()'s zero-copy aligned path. */
                        uint32_t file_off = winfo.data_offset + data_off;
                        uint32_t misalign = file_off & 511u;
                        if (misalign != 0u) {
                            uint32_t to_align = 512u - misalign;
                            if ((to_align % (uint32_t)winfo.block_align) == 0u && to_align < chunk) {
                                chunk = to_align;
                            }
                        }
                    }
                    if (chunk > rem) chunk = rem;
                    chunk -= chunk % winfo.block_align;
                    if (!chunk) {
                        data_off = winfo.data_bytes;
                        if (!audio_dac_is_busy()) {
                            eof_reached = 1;
                            peak_l_pct  = 0u;
                            peak_r_pct  = 0u;
                            audio_pa_set(0);
                            ui_dirty    = 1;
                        }
                        break;
                    }

                    uint32_t got = 0u;
                    if (exfat_read_file(vol, &wav_ent, (uint64_t)winfo.data_offset + data_off,
                                        g_file_buf, chunk, &got) != 0 || got < chunk) {
                        data_off = winfo.data_bytes;
                        if (!audio_dac_is_busy()) {
                            eof_reached = 1;
                            peak_l_pct  = 0u;
                            peak_r_pct  = 0u;
                            audio_pa_set(0);
                            ui_dirty    = 1;
                        }
                        break;
                    }
                    data_off += chunk;

                    uint32_t decoded_frames = 0u;
                    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
                        decoded_frames = decode_blocks(&winfo, chunk);
                        if (!decoded_frames) data_off = winfo.data_bytes;
                    } else {
                        decoded_frames = chunk / (uint32_t)winfo.block_align;
                    }

                    if (decoded_frames > winfo.total_samples - played_frames) {
                        decoded_frames = winfo.total_samples - played_frames;
                    }

                    uint32_t start_pos = 0u;
                    if (skip_frames > 0u) {
                        start_pos      = (skip_frames > decoded_frames) ? decoded_frames : skip_frames;
                        played_frames += start_pos;
                        skip_frames   -= start_pos;
                    }

                    int32_t pl = 0, pr = 0;
                    if (winfo.channels == 1u) {
                        for (uint32_t i = start_pos; i < decoded_frames; i++) {
                            int16_t s = pcm_buf[i];
                            int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
                            if (a > pl) pl = a;
                        }
                        pr = pl;
                    } else {
                        for (uint32_t i = start_pos; i < decoded_frames; i++) {
                            int16_t l = pcm_buf[i * 2u + 0u];
                            int16_t r = pcm_buf[i * 2u + 1u];
                            int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
                            int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
                            if (al > pl) pl = al;
                            if (ar > pr) pr = ar;
                        }
                    }
                    peak_l_pct = (uint8_t)((pl * 100) / 32767);
                    peak_r_pct = (uint8_t)((pr * 100) / 32767);
                    buf_pos    = start_pos;
                    buf_frames = decoded_frames;
                }

                if (buf_pos < buf_frames) {
                    int sent;
                    if (winfo.channels == 2u) {
                        sent = audio_dac_write_stereo(&pcm_buf[buf_pos * 2u],
                                                      buf_frames - buf_pos);
                    } else {
                        uint32_t n = buf_frames - buf_pos;
                        if (n > PLAYER_DAC_CHUNK_FRAMES) {
                            n = PLAYER_DAC_CHUNK_FRAMES;
                        }
                        const int16_t *src = &pcm_buf[buf_pos];
                        for (uint32_t i = 0u; i < n; i++) {
                            int16_t s = src[i];
                            g_mono_tx[i * 2u + 0u] = s;
                            g_mono_tx[i * 2u + 1u] = s;
                        }
                        sent = audio_dac_write_stereo(g_mono_tx, n);
                    }
                    if (sent <= 0) {
                        break;
                    }
                    buf_pos       += (uint32_t)sent;
                    played_frames += (uint32_t)sent;
                }
            }
        }

        /* Drive smooth LVGL animations on single-core / WASM / QEMU (no-op on dual-core CPU1) */
        if (show_player && ui_dispatch_is_animating() && (uint32_t)(now - last_anim_ms) >= 8u) {
            last_anim_ms = now;
            ui_dispatch_tick();
        }

        /* Fast zero-I/O transcript word tracking (~33 Hz from RAM pager cache) */
        int tr_populated = 0;
        uint32_t tr_pos_ms = 0u;
        if (has_cwx && !ui_dirty && (uint32_t)(now - last_tr_poll_ms) >= 30u) {
            last_tr_poll_ms = now;
            uint32_t heard = eof_reached ? played_frames : player_heard_frames(played_frames, skip_frames);
            if (scrub.speed) heard = scrub_heard(&scrub, winfo.total_samples);
            tr_pos_ms = frames_to_ms(heard, winfo.sample_rate);
            if (tr_pos_ms > dur_ms && dur_ms > 0u) {
                tr_pos_ms = dur_ms;
            }
            uint32_t sig = populate_transcript_lines(m, &g_tr_pager, tr_pos_ms, paused && !scrub.speed);
            tr_populated = 1;
            if (sig != last_tr_sig) {
                last_tr_sig = sig;
                ui_dirty    = 1;
            }
        }

        if (ui_dirty || (uint32_t)(now - last_ui_ms) >= 120u) {
            last_ui_ms = now;
            ui_dirty   = 0;
            if ((uint32_t)(now - telemetry_ms) >= 1000u) {
                telemetry_ms = now;
                pmu_poll_charge_manage();
                rtc_get(&m->now);
                uint32_t mv = pmu_read_vbat_mv();
                m->batt_mv    = (uint16_t)mv;
                m->batt_pct   = vbat_mv_to_pct(mv);
                m->charging   = (pmu_get_charge_state() == PMU_CHG_CHARGING) ? 1 : 0;
            }
            m->hp_plugged = hp_in;

            uint32_t pos_ms;
            if (tr_populated) {
                pos_ms = tr_pos_ms;
            } else {
                uint32_t heard  = eof_reached ? played_frames : player_heard_frames(played_frames, skip_frames);
                if (scrub.speed) heard = scrub_heard(&scrub, winfo.total_samples);
                pos_ms = frames_to_ms(heard, winfo.sample_rate);
                if (pos_ms > dur_ms && dur_ms > 0u) {
                    pos_ms = dur_ms;
                }
            }
            m->play_pos_sec     = pos_ms / 1000u;
            m->play_pos_ms      = pos_ms;
            m->play_dur_ms      = dur_ms;
            m->play_state       = scrub.speed ? 0u : eof_reached ? 2u : (paused ? 1u : 0u);
            m->play_scrub_speed = (int8_t)scrub.speed;
            m->play_vol_hi      = (last_vol_ms != 0u && (uint32_t)(now - last_vol_ms) < 1500u) ? 1u : 0u;
            m->play_vu_l        = (paused || eof_reached) ? 0u : peak_l_pct;
            m->play_vu_r        = (paused || eof_reached) ? 0u : peak_r_pct;

            if (has_cwx && !tr_populated) {
                last_tr_sig     = populate_transcript_lines(m, &g_tr_pager, pos_ms, paused && !scrub.speed);
                last_tr_poll_ms = now;
            }
            if (show_player) {
                ui_render_screen(UI_SCR_PLAYER, m);
            }
        }

        if (!keys_event_pending() &&
            (scrub.speed || paused || eof_reached || (audio_dac_is_busy() &&
            (buf_pos < buf_frames || data_off >= winfo.data_bytes || played_frames >= winfo.total_samples)))) {
            OPENFW_WFI();
        }
    }

    if (scrub.speed) {
        uint32_t heard = scrub_heard(&scrub, winfo.total_samples);
        m->play_pos_sec = frames_to_ms(heard, winfo.sample_rate) / 1000u;
        eof_reached = heard == winfo.total_samples;
    }
    audio_dac_stop();
    m->play_file.resume_sec = eof_reached ? 0u : m->play_pos_sec;

    return ret_code;
}

void openfw_play_file(exfat_vol_t *vol, const char *dir_path, const char *wav_name)
{
    (void)openfw_play_file_ui(vol, dir_path, wav_name, (ui_model_t *)0);
}
