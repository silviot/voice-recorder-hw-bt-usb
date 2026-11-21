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

static uint8_t  g_file_buf[4096]  __attribute__((aligned(4)));
static int16_t  g_dec_pcm[2048]   __attribute__((aligned(4)));
static int16_t  g_stereo_tx[4096] __attribute__((aligned(4))); /* up to 2048 stereo frames */

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
    if (len < 5) {
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
    uint32_t secs  = frames / sample_rate;
    uint32_t rem_f = frames % sample_rate;
    uint32_t ms    = (rem_f <= 0x3FFFFFu)
                   ? ((rem_f * 1000u) / sample_rate)
                   : (((rem_f / 100u) * 1000u) / ((sample_rate >= 100u) ? (sample_rate / 100u) : 1u));
    return secs * 1000u + ms;
}

static uint8_t vbat_mv_to_pct(uint32_t mv)
{
    if (mv <= 3400u) return 0u;
    if (mv >= 4180u) return 100u;
    return (uint8_t)(((mv - 3400u) * 100u) / (4180u - 3400u));
}

static void append_word_cap(char *dst, int cap, const char *w, int wlen)
{
    int len = 0;
    while (len < cap - 1 && dst[len] != '\0') {
        len++;
    }
    if (len > 0 && len < cap - 1) {
        dst[len++] = ' ';
    }
    for (int i = 0; i < wlen && len < cap - 1; i++) {
        dst[len++] = w[i];
    }
    dst[len] = '\0';
}

static void populate_transcript_lines(ui_model_t *m, cwx_stream_t *cwx, uint32_t pos_ms)
{
    m->tr_prev[0]          = '\0';
    m->tr_curr_spoken[0]   = '\0';
    m->tr_curr_upcoming[0] = '\0';
    m->tr_next[0]          = '\0';
    m->tr_speaker          = 0u;

    cwx_word_t words[18];
    int active_idx = -1;
    int nwords = cwx_read_window(cwx, pos_ms, words, 18u, &active_idx);
    if (nwords <= 0) {
        str_copy_cap(m->tr_curr_spoken, (int)sizeof(m->tr_curr_spoken),
                     cwx->title[0] ? cwx->title : m->play_file.filename);
        return;
    }

    if (active_idx < 0) {
        active_idx = 0;
    } else if (active_idx >= nwords) {
        active_idx = nwords - 1;
    }
    m->tr_speaker = words[active_idx].speaker_idx;

    /* Partition words into prev (up to 4 before curr), curr (around active_idx), next */
    int curr_start = (active_idx >= 2) ? (active_idx - 2) : 0;
    int prev_start = (curr_start >= 4) ? (curr_start - 4) : 0;
    int curr_end   = curr_start;

    for (int i = prev_start; i < curr_start; i++) {
        append_word_cap(m->tr_prev, (int)sizeof(m->tr_prev), words[i].text, (int)words[i].text_len);
    }

    /* Fill current line up to ~20 chars so it fits cleanly in 152px of 14px AA font */
    int cur_chars = 0;
    for (int i = curr_start; i < nwords; i++) {
        int wl = (int)words[i].text_len;
        if (i > active_idx && cur_chars + wl + 1 > 22) {
            break;
        }
        if (i <= active_idx) {
            append_word_cap(m->tr_curr_spoken, (int)sizeof(m->tr_curr_spoken), words[i].text, wl);
        } else {
            if (m->tr_curr_spoken[0] != '\0' && m->tr_curr_upcoming[0] == '\0') {
                str_copy_cap(m->tr_curr_upcoming, (int)sizeof(m->tr_curr_upcoming), " ");
            }
            append_word_cap(m->tr_curr_upcoming, (int)sizeof(m->tr_curr_upcoming), words[i].text, wl);
        }
        cur_chars += wl + 1;
        curr_end = i + 1;
        if (cur_chars >= 22 && i >= active_idx) {
            break;
        }
    }

    for (int i = curr_end; i < nwords && i < curr_end + 5; i++) {
        append_word_cap(m->tr_next, (int)sizeof(m->tr_next), words[i].text, (int)words[i].text_len);
    }
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
    if (is_seek_key(s->held) && time_reached(now_ms, s->next_repeat)) {
        s->hold_active = 1u;
        s->next_repeat = now_ms + PLAYER_KEY_SCRUB_MS;
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
        /* Another key pressed while a tap waits: settle the tap first and
         * make the new key wait for a fresh press. */
        key_id_t k = s->tap_key;
        s->tap_key = KEY_NONE;
        s->ignore  = 1u;
        return (k == KEY_RIGHT) ? PLAYER_ACT_SEEK_FWD : PLAYER_ACT_SEEK_BACK;
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

int openfw_play_file_ui(exfat_vol_t *vol, const char *dir_path, const char *wav_name,
                        ui_model_t *m)
{
    if (!vol || !wav_name || wav_name[0] == '\0') {
        return 0;
    }

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

    uint32_t hdr_read = 0u;
    uint32_t hdr_want = (wav_ent.size > 512u) ? 512u : (uint32_t)wav_ent.size;
    if (exfat_read_file(vol, &wav_ent, 0u, g_file_buf, hdr_want, &hdr_read) != 0 || hdr_read < 44u) {
        return 0;
    }

    wav_stream_info_t winfo;
    if (wav_parse_stream(g_file_buf, hdr_read, &winfo) != 0) {
        return 0;
    }
    if (winfo.block_align == 0u || winfo.block_align > sizeof(g_file_buf)) {
        return 0;
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
    }

    int hp_in = keys_read_hp_detect();
    uint8_t volume = hp_in ? m->cfg.hp_vol : m->cfg.spk_vol;
    if (volume > 31u) {
        volume = 18u;
    }
    if (audio_dac_start(winfo.sample_rate, volume) != 0) {
        return 0;
    }
    /* Route audio to built-in speaker PA (PB30) when no 3.5 mm jack is inserted */
    audio_pa_set(!hp_in);

    lcd_set_rotation(1); /* Landscape 160x80 */

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
        if (target < winfo.total_samples) {
            if (winfo.format_tag == WAV_FMT_PCM) {
                uint32_t f_idx = (uint32_t)target;
                data_off      = f_idx * (uint32_t)winfo.block_align;
                played_frames = f_idx;
            } else {
                uint32_t spb = (winfo.samples_per_block > 0u) ? (uint32_t)winfo.samples_per_block : 1u;
                uint32_t blk_idx = (uint32_t)target / spb;
                data_off      = blk_idx * (uint32_t)winfo.block_align;
                played_frames = blk_idx * spb;
            }
            if (data_off > winfo.data_bytes) {
                data_off = winfo.data_bytes;
            }
        }
    }
    uint32_t buf_pos       = 0u;
    uint32_t buf_frames    = 0u;
    uint8_t  peak_l_pct    = 0u;
    uint8_t  peak_r_pct    = 0u;
    int      paused        = 0;
    int      eof_reached   = 0;
    uint32_t last_ui_ms    = 0u;
    uint32_t last_vol_ms   = 0u;
    uint32_t toast_until   = 0u;
    int      ui_dirty      = 1;
    int      ret_code      = 0;
    keys_flush_events();
    player_keys_t keys;
    player_keys_init(&keys, keys_state((uint32_t *)0), millis());

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
        int cur_hp = keys_read_hp_detect();
        if (cur_hp != hp_in) {
            int was_hp = hp_in;
            hp_in  = cur_hp;
            volume = hp_in ? m->cfg.hp_vol : m->cfg.spk_vol;
            audio_dac_set_volume(volume, 0x1FFu);
            if (was_hp && !hp_in && !paused && !eof_reached) {
                paused = 1;
                audio_pa_set(0);
                audio_dac_set_mute(1);
                str_copy_cap(m->toast_msg, (int)sizeof(m->toast_msg),
                             "Headphones out \xC2\xB7 paused");
                m->toast_col = UI_COL_AMBER;
                toast_until  = now + 1500u;
            } else {
                audio_pa_set((!hp_in && !paused && !eof_reached) ? 1 : 0);
            }
            ui_dirty = 1;
        }

        player_action_t act = PLAYER_ACT_NONE;
        key_event_t ev;
        while (keys_peek_event(&ev)) {
            act = player_keys_tick(&keys, ev.t_ms);
            if (act != PLAYER_ACT_NONE) {
                break;
            }
            keys_pop_event(&ev);
            act = player_keys_edge(&keys, ev.pressed ? ev.key : KEY_NONE, ev.t_ms);
            if (act != PLAYER_ACT_NONE) {
                break;
            }
        }
        if (act == PLAYER_ACT_NONE) {
            act = player_keys_tick(&keys, now);
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
                eof_reached   = 0;
                paused        = 0;
            } else {
                paused ^= 1;
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
        } else if (act == PLAYER_ACT_SEEK_BACK || act == PLAYER_ACT_SEEK_FWD ||
                   act == PLAYER_ACT_SCRUB_BACK || act == PLAYER_ACT_SCRUB_FWD) {
            int fwd = (act == PLAYER_ACT_SEEK_FWD || act == PLAYER_ACT_SCRUB_FWD);
            int tap = (act == PLAYER_ACT_SEEK_BACK || act == PLAYER_ACT_SEEK_FWD);
            /* A tap keeps the old meaning in "Track" skip mode and at the end of
             * the file; a hold always seeks. */
            if (tap && ((eof_reached && fwd) || (!has_cwx && m->cfg.skip_mode == UI_SKIP_TRACK))) {
                file_dir = fwd ? 1 : -1;
            } else {
                int32_t step_s;
                if (m->cfg.skip_mode == UI_SKIP_5_10) {
                    step_s = fwd ? 10 : -5;
                } else {
                    step_s = fwd ? 30 : -10;
                }
                int64_t delta  = (int64_t)winfo.sample_rate * (int64_t)step_s;
                int64_t target = (int64_t)played_frames + delta;
                if (target < 0) {
                    target = 0;
                }
                if ((uint64_t)target >= winfo.total_samples) {
                    target = (winfo.total_samples > 0u) ? (int64_t)(winfo.total_samples - 1u) : 0;
                }
                if (winfo.format_tag == WAV_FMT_PCM) {
                    uint32_t f_idx = (uint32_t)target;
                    data_off      = f_idx * (uint32_t)winfo.block_align;
                    played_frames = f_idx;
                } else {
                    uint32_t spb = (winfo.samples_per_block > 0u) ? (uint32_t)winfo.samples_per_block : 1u;
                    uint32_t blk_idx = (uint32_t)target / spb;
                    data_off      = blk_idx * (uint32_t)winfo.block_align;
                    played_frames = blk_idx * spb;
                }
                if (data_off > winfo.data_bytes) {
                    data_off = winfo.data_bytes;
                }
                buf_pos     = 0u;
                buf_frames  = 0u;
                eof_reached = 0;
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

        /* Refill PCM decode staging buffer & push to double-buffered DMAC CH0 TX */
        if (!paused && !eof_reached) {
            for (int iter = 0; iter < 4; iter++) {
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
                    uint32_t frames_cap = (sizeof(g_dec_pcm) / sizeof(g_dec_pcm[0])) / winfo.channels;
                    uint32_t frames_per_block = 1u;
                    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
                        frames_per_block = 1u + (winfo.block_align - 4u * winfo.channels) * 2u / winfo.channels;
                    }
                    uint32_t blocks = frames_cap / frames_per_block;
                    uint32_t chunk = blocks * winfo.block_align;
                    if (chunk > sizeof(g_file_buf)) chunk = sizeof(g_file_buf);
                    if (chunk > rem) chunk = rem;
                    chunk -= chunk % winfo.block_align;
                    if (!chunk) {
                        data_off = winfo.data_bytes;
                        break;
                    }

                    uint32_t got = 0u;
                    if (exfat_read_file(vol, &wav_ent, (uint64_t)winfo.data_offset + data_off,
                                        g_file_buf, chunk, &got) != 0 || got < chunk) {
                        eof_reached = 1;
                        ui_dirty    = 1;
                        break;
                    }
                    data_off += chunk;

                    uint32_t decoded_frames = 0u;
                    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
                        for (uint32_t off = 0; off < chunk; off += winfo.block_align) {
                            int n = wav_decode_ima_adpcm_block(g_file_buf + off, winfo.block_align,
                                            winfo.channels, g_dec_pcm + decoded_frames * winfo.channels,
                                            frames_cap - decoded_frames);
                            if (n <= 0) {
                                data_off = winfo.data_bytes;
                                break;
                            }
                            decoded_frames += (uint32_t)n;
                        }
                    } else {
                        decoded_frames = chunk / (uint32_t)winfo.block_align;
                        const int16_t *src = (const int16_t *)(const void *)g_file_buf;
                        uint32_t total_s = decoded_frames * (uint32_t)winfo.channels;
                        for (uint32_t i = 0u; i < total_s; i++) {
                            g_dec_pcm[i] = src[i];
                        }
                    }

                    if (decoded_frames > winfo.total_samples - played_frames) {
                        decoded_frames = winfo.total_samples - played_frames;
                    }

                    int32_t pl = 0, pr = 0;
                    if (winfo.channels == 1u) {
                        for (uint32_t i = 0u; i < decoded_frames; i++) {
                            int16_t s = g_dec_pcm[i];
                            int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
                            if (a > pl) pl = a;
                            g_stereo_tx[i * 2u + 0u] = s;
                            g_stereo_tx[i * 2u + 1u] = s;
                        }
                        pr = pl;
                    } else {
                        for (uint32_t i = 0u; i < decoded_frames; i++) {
                            int16_t l = g_dec_pcm[i * 2u + 0u];
                            int16_t r = g_dec_pcm[i * 2u + 1u];
                            int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
                            int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
                            if (al > pl) pl = al;
                            if (ar > pr) pr = ar;
                            g_stereo_tx[i * 2u + 0u] = l;
                            g_stereo_tx[i * 2u + 1u] = r;
                        }
                    }
                    peak_l_pct = (uint8_t)((pl * 100) / 32767);
                    peak_r_pct = (uint8_t)((pr * 100) / 32767);
                    buf_pos    = 0u;
                    buf_frames = decoded_frames;
                }

                if (buf_pos < buf_frames) {
                    int sent = audio_dac_write_stereo(&g_stereo_tx[buf_pos * 2u],
                                                      buf_frames - buf_pos);
                    if (sent <= 0) {
                        break;
                    }
                    buf_pos       += (uint32_t)sent;
                    played_frames += (uint32_t)sent;
                }
            }
        }

        if (ui_dirty || (uint32_t)(now - last_ui_ms) >= 120u) {
            last_ui_ms = now;
            ui_dirty   = 0;
            pmu_poll_charge_manage();
            rtc_get(&m->now);
            uint32_t mv = pmu_read_vbat_mv();
            m->batt_mv    = (uint16_t)mv;
            m->batt_pct   = vbat_mv_to_pct(mv);
            m->charging   = (pmu_get_charge_state() == PMU_CHG_CHARGING) ? 1 : 0;
            m->hp_plugged = hp_in;

            uint32_t pos_ms = frames_to_ms(played_frames, winfo.sample_rate);
            if (pos_ms > dur_ms && dur_ms > 0u) {
                pos_ms = dur_ms;
            }
            m->play_pos_sec = pos_ms / 1000u;
            m->play_state   = eof_reached ? 2u : (paused ? 1u : 0u);
            m->play_vol_hi  = (last_vol_ms != 0u && (uint32_t)(now - last_vol_ms) < 1500u) ? 1u : 0u;
            m->play_vu_l    = (paused || eof_reached) ? 0u : peak_l_pct;
            m->play_vu_r    = (paused || eof_reached) ? 0u : peak_r_pct;

            if (has_cwx) {
                populate_transcript_lines(m, &cwx, pos_ms);
            }
            if (show_player) {
                ui_render_screen(UI_SCR_PLAYER, m);
            }
        }

        if (!keys_event_pending() &&
            (paused || eof_reached || (audio_dac_is_busy() &&
            (buf_pos < buf_frames || data_off >= winfo.data_bytes || played_frames >= winfo.total_samples)))) {
            OPENFW_WFI();
        }
    }

    audio_dac_stop();
    m->play_file.resume_sec = eof_reached ? 0u : m->play_pos_sec;
    lcd_set_rotation(1);

    while (keys_state((uint32_t *)0) == KEY_BACK) {
        wdt_feed();
        mdelay(20u);
    }
    keys_flush_events();
    return ret_code;
}

void openfw_play_file(exfat_vol_t *vol, const char *dir_path, const char *wav_name)
{
    (void)openfw_play_file_ui(vol, dir_path, wav_name, (ui_model_t *)0);
}
