/*
 * Smartlink SL6801 OpenFW On-Device WAV & Cassini Timed-Word (`CWX1`) Player
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw.h"
#include "openfw_wav.h"
#include "openfw_cwx.h"
#include "openfw_player.h"

static uint8_t  g_file_buf[1024] __attribute__((aligned(4)));
static int16_t  g_dec_pcm[2048]  __attribute__((aligned(4)));
static int16_t  g_stereo_tx[2048] __attribute__((aligned(4))); /* up to 1024 stereo frames */

typedef struct {
    exfat_vol_t   *vol;
    exfat_entry_t *ent;
} exfat_cwx_io_t;

static int exfat_cwx_read_cb(void *user, uint32_t offset, void *dst, uint32_t len, uint32_t *out_read)
{
    exfat_cwx_io_t *io = (exfat_cwx_io_t *)user;
    if (!io || !io->vol || !io->ent) {
        return -1;
    }
    return exfat_read_file(io->vol, io->ent, (uint64_t)offset, dst, len, out_read);
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

static void append_u32(char *dst, int *pos, int max, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0u) {
        if (*pos < (max - 1)) {
            dst[(*pos)++] = '0';
        }
        return;
    }
    while (v > 0u && n < 11) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && *pos < (max - 1)) {
        dst[(*pos)++] = tmp[--n];
    }
}

static void append_2d(char *dst, int *pos, int max, uint32_t v)
{
    if (*pos + 2 < max) {
        dst[(*pos)++] = (char)('0' + ((v / 10u) % 10u));
        dst[(*pos)++] = (char)('0' + (v % 10u));
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

static void draw_str_clip(uint16_t x, uint16_t y, const char *s, int max_chars, uint16_t fg, uint16_t bg)
{
    char buf[32];
    int i = 0;
    if (max_chars > 30) {
        max_chars = 30;
    }
    while (s && s[i] != '\0' && i < max_chars) {
        buf[i] = s[i];
        i++;
    }
    while (i < max_chars) {
        buf[i++] = ' ';
    }
    buf[i] = '\0';
    lcd_draw_str(x, y, buf, fg, bg);
}

static void draw_player_chrome(const char *wav_name,
                               const wav_stream_info_t *winfo,
                               const cwx_stream_t *cwx,
                               int has_cwx,
                               uint32_t pos_ms,
                               uint32_t dur_ms,
                               uint8_t volume,
                               int paused,
                               int eof_reached,
                               int hp_in,
                               uint8_t peak_pct)
{
    const uint16_t bg     = rgb565(0u, 0u, 0u);
    const uint16_t bar_bg = rgb565(18u, 22u, 32u);
    const uint16_t text   = rgb565(235u, 240u, 245u);
    const uint16_t dim    = rgb565(135u, 145u, 160u);
    const uint16_t green  = rgb565(100u, 255u, 140u);
    const uint16_t yellow = rgb565(255u, 220u, 90u);
    const uint16_t cyan   = rgb565(90u, 220u, 255u);
    const uint16_t track  = rgb565(36u, 42u, 54u);

    /* Top Bar (y=0..10) */
    lcd_fill_rect(0u, 0u, 160u, 11u, bar_bg);

    const char *badge = wav_name;
    uint16_t badge_col = cyan;
    cwx_word_t words[18];
    int nwords = 0;
    int active_idx = -1;

    if (has_cwx) {
        nwords = cwx_read_window((cwx_stream_t *)cwx, pos_ms, words, 18u, &active_idx);
        if (nwords > 0 && active_idx >= 0 && active_idx < nwords) {
            uint8_t spk = words[active_idx].speaker_idx;
            badge     = cwx_speaker_label(cwx, spk);
            badge_col = cwx_speaker_color(cwx, spk);
            if (badge_col == 0u) {
                badge_col = cyan;
            }
        } else if (cwx->speaker_count > 0u) {
            badge     = cwx_speaker_label(cwx, 0u);
            badge_col = cwx_speaker_color(cwx, 0u);
        }
    }
    draw_str_clip(2u, 2u, badge, 10, badge_col, bar_bg);

    uint32_t cur_s = pos_ms / 1000u;
    uint32_t tot_s = dur_ms / 1000u;
    char tbuf[16];
    int tpos = 0;
    append_2d(tbuf, &tpos, (int)sizeof(tbuf), cur_s / 60u);
    append_str(tbuf, &tpos, (int)sizeof(tbuf), ":");
    append_2d(tbuf, &tpos, (int)sizeof(tbuf), cur_s % 60u);
    append_str(tbuf, &tpos, (int)sizeof(tbuf), "/");
    append_2d(tbuf, &tpos, (int)sizeof(tbuf), tot_s / 60u);
    append_str(tbuf, &tpos, (int)sizeof(tbuf), ":");
    append_2d(tbuf, &tpos, (int)sizeof(tbuf), tot_s % 60u);
    tbuf[tpos] = '\0';
    lcd_draw_str(66u, 2u, tbuf, text, bar_bg);

    lcd_draw_str(136u, 2u, hp_in ? "H" : "-", hp_in ? green : yellow, bar_bg);
    const char *st_icon = eof_reached ? "[]" : (paused ? "II" : "> ");
    lcd_draw_str(146u, 2u, st_icon, (paused || eof_reached) ? yellow : green, bar_bg);

    lcd_fill_rect(0u, 11u, 160u, 1u, rgb565(50u, 60u, 78u));

    /* Body area (y=13..56) */
    lcd_fill_rect(0u, 12u, 160u, 45u, bg);
    if (has_cwx && nwords > 0) {
        /* Start rendering from just before active_idx so it fits naturally inside 3 wrapped rows */
        int start_i = 0;
        if (active_idx > 4) {
            start_i = active_idx - 4;
        }
        int cur_row = 0;
        int cur_col = 0;
        const int max_cols = 26;
        const uint16_t row_y[3] = { 15u, 29u, 43u };

        for (int wi = start_i; wi < nwords && cur_row < 3; wi++) {
            int wlen = (int)words[wi].text_len;
            if (wlen <= 0) {
                continue;
            }
            if (wlen > max_cols) {
                wlen = max_cols;
            }
            int need_space = (cur_col > 0) ? 1 : 0;
            if (cur_col + need_space + wlen > max_cols) {
                cur_row++;
                cur_col = 0;
                need_space = 0;
                if (cur_row >= 3) {
                    break;
                }
            }
            if (need_space) {
                lcd_draw_char((uint16_t)(2u + (uint16_t)cur_col * 6u), row_y[cur_row], ' ', text, bg);
                cur_col++;
            }

            uint16_t wfg = (wi == active_idx) ? bg :
                           ((words[wi].flags & CWX_FLAG_LOW_CONF) ? dim : text);
            uint16_t wbg = (wi == active_idx) ? yellow : bg;

            for (int ci = 0; ci < wlen && cur_col < max_cols; ci++) {
                lcd_draw_char((uint16_t)(2u + (uint16_t)cur_col * 6u), row_y[cur_row],
                              words[wi].text[ci], wfg, wbg);
                cur_col++;
            }
        }
    } else if (has_cwx) {
        draw_str_clip(2u, 28u, cwx->title[0] ? cwx->title : wav_name, 26, cyan, bg);
    } else {
        draw_str_clip(2u, 16u, wav_name, 26, cyan, bg);
        char fmt[32];
        int fpos = 0;
        append_u32(fmt, &fpos, (int)sizeof(fmt), winfo->sample_rate / 1000u);
        append_str(fmt, &fpos, (int)sizeof(fmt), "k ");
        append_u32(fmt, &fpos, (int)sizeof(fmt), winfo->channels);
        append_str(fmt, &fpos, (int)sizeof(fmt), "CH ");
        append_str(fmt, &fpos, (int)sizeof(fmt),
                   (winfo->format_tag == WAV_FMT_IMA_ADPCM) ? "ADPCM" : "PCM16");
        fmt[fpos] = '\0';
        draw_str_clip(2u, 28u, fmt, 26, text, bg);

        /* Stereo Output VU Bar (y=42, w=156, h=8) */
        uint16_t vu_w = (uint16_t)(((uint32_t)peak_pct * 156u) / 100u);
        if (vu_w > 156u) {
            vu_w = 156u;
        }
        uint16_t vu_col = (peak_pct > 85u) ? rgb565(255u, 90u, 90u) :
                          (peak_pct > 55u) ? yellow : green;
        if (vu_w > 0u) {
            lcd_fill_rect(2u, 42u, vu_w, 8u, vu_col);
        }
        if (vu_w < 156u) {
            lcd_fill_rect((uint16_t)(2u + vu_w), 42u, (uint16_t)(156u - vu_w), 8u, track);
        }
    }

    /* Progress Bar (y=59..64) */
    uint16_t prog_w = 0u;
    if (dur_ms > 0u) {
        uint32_t p = pos_ms;
        uint32_t d = dur_ms;
        while (d > 0x00FFFFFFu) {
            p >>= 4;
            d >>= 4;
        }
        if (d > 0u) {
            prog_w = (uint16_t)((p * 156u) / d);
        }
        if (prog_w > 156u) {
            prog_w = 156u;
        }
    }
    if (prog_w > 0u) {
        lcd_fill_rect(2u, 59u, prog_w, 5u, cyan);
    }
    if (prog_w < 156u) {
        lcd_fill_rect((uint16_t)(2u + prog_w), 59u, (uint16_t)(156u - prog_w), 5u, track);
    }

    /* Bottom Footer (y=68..79) */
    lcd_fill_rect(0u, 66u, 160u, 14u, bg);
    char foot[32];
    int pos = 0;
    append_str(foot, &pos, (int)sizeof(foot), "V:");
    append_2d(foot, &pos, (int)sizeof(foot), volume);
    append_str(foot, &pos, (int)sizeof(foot), " E:PL LR:SK B:EX");
    foot[pos] = '\0';
    draw_str_clip(2u, 70u, foot, 26, yellow, bg);
}

void openfw_play_file(exfat_vol_t *vol, const char *dir_path, const char *wav_name)
{
    if (!vol || !wav_name || wav_name[0] == '\0') {
        return;
    }

    char wav_path[96];
    build_path(wav_path, (int)sizeof(wav_path), dir_path, wav_name);

    exfat_entry_t wav_ent;
    if (exfat_find_path(vol, wav_path, &wav_ent) != 0 || wav_ent.is_dir || wav_ent.size < 44u) {
        return;
    }

    uint32_t hdr_read = 0u;
    uint32_t hdr_want = (wav_ent.size > 512u) ? 512u : (uint32_t)wav_ent.size;
    if (exfat_read_file(vol, &wav_ent, 0u, g_file_buf, hdr_want, &hdr_read) != 0 || hdr_read < 44u) {
        return;
    }

    wav_stream_info_t winfo;
    if (wav_parse_stream(g_file_buf, hdr_read, &winfo) != 0) {
        return;
    }
    if (winfo.block_align == 0u || winfo.block_align > sizeof(g_file_buf)) {
        return;
    }

    /* Optional companion Cassini timed-word sidecar (.cwx) */
    int has_cwx = 0;
    cwx_stream_t cwx;
    exfat_entry_t cwx_ent;
    exfat_cwx_io_t cwx_io = { vol, &cwx_ent };
    char cwx_path[96];
    if (make_cwx_path(cwx_path, (int)sizeof(cwx_path), dir_path, wav_name) == 0 &&
        exfat_find_path(vol, cwx_path, &cwx_ent) == 0 && !cwx_ent.is_dir &&
        cwx_ent.size <= 0xFFFFFFFFu &&
        cwx_open(&cwx, exfat_cwx_read_cb, &cwx_io, (uint32_t)cwx_ent.size) == 0) {
        has_cwx = 1;
    }

    uint8_t volume = 18u;
    if (audio_dac_start(winfo.sample_rate, volume) != 0) {
        return;
    }

    lcd_set_rotation(1); /* Landscape 160x80 */
    lcd_fill_rect(0u, 0u, 160u, 80u, rgb565(0u, 0u, 0u));

    uint32_t data_off     = 0u;
    uint32_t played_frames = 0u;
    uint32_t buf_pos      = 0u;
    uint32_t buf_frames   = 0u;
    uint8_t  cur_peak_pct = 0u;
    int      paused       = 0;
    int      eof_reached  = 0;
    uint32_t last_ui_ms   = 0u;
    int      ui_dirty     = 1;
    key_id_t prev_k       = KEY_ENTER;

    while (1) {
        wdt_feed();
        if (keys_read_rec_switch()) {
            break;
        }

        uint32_t now = millis();
        key_id_t k = keys_decode(keys_read_adc());
        if (k != KEY_NONE && k != prev_k) {
            if (k == KEY_BACK) {
                break;
            } else if (k == KEY_ENTER) {
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
                ui_dirty = 1;
            } else if (k == KEY_VOL_UP) {
                if (volume < 31u) {
                    volume++;
                    audio_dac_set_volume(volume, 0x1FFu);
                }
                ui_dirty = 1;
            } else if (k == KEY_VOL_DOWN) {
                if (volume > 0u) {
                    volume--;
                    audio_dac_set_volume(volume, 0x1FFu);
                }
                ui_dirty = 1;
            } else if (k == KEY_LEFT || k == KEY_RIGHT) {
                int64_t delta = (int64_t)winfo.sample_rate * 10LL;
                int64_t target = (int64_t)played_frames + ((k == KEY_RIGHT) ? delta : -delta);
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
                ui_dirty    = 1;
            }
        }
        prev_k = k;

        /* Refill PCM decode staging buffer & push to double-buffered DMAC CH0 TX */
        if (!paused && !eof_reached) {
            for (int iter = 0; iter < 4; iter++) {
                if (buf_pos >= buf_frames) {
                    if (data_off >= winfo.data_bytes) {
                        eof_reached  = 1;
                        cur_peak_pct = 0u;
                        ui_dirty     = 1;
                        break;
                    }
                    uint32_t rem = winfo.data_bytes - data_off;
                    uint32_t chunk;
                    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
                        if (rem < winfo.block_align) {
                            eof_reached = 1;
                            ui_dirty    = 1;
                            break;
                        }
                        chunk = winfo.block_align;
                    } else {
                        uint32_t max_f = 1024u / (uint32_t)winfo.block_align;
                        if (max_f > 512u) {
                            max_f = 512u;
                        }
                        chunk = max_f * (uint32_t)winfo.block_align;
                        if (chunk > rem) {
                            chunk = (rem / (uint32_t)winfo.block_align) * (uint32_t)winfo.block_align;
                        }
                        if (chunk == 0u) {
                            eof_reached = 1;
                            ui_dirty    = 1;
                            break;
                        }
                    }

                    uint32_t got = 0u;
                    if (exfat_read_file(vol, &wav_ent, (uint64_t)(winfo.data_offset + data_off),
                                        g_file_buf, chunk, &got) != 0 || got < chunk) {
                        eof_reached = 1;
                        ui_dirty    = 1;
                        break;
                    }
                    data_off += chunk;

                    uint32_t decoded_frames = 0u;
                    if (winfo.format_tag == WAV_FMT_IMA_ADPCM) {
                        uint32_t dec_cap = (winfo.channels == 2u) ? 1024u : 2048u;
                        int n = wav_decode_ima_adpcm_block(g_file_buf, winfo.block_align,
                                                           winfo.channels, g_dec_pcm, dec_cap);
                        if (n <= 0) {
                            eof_reached = 1;
                            ui_dirty    = 1;
                            break;
                        }
                        decoded_frames = (uint32_t)n;
                    } else {
                        decoded_frames = chunk / (uint32_t)winfo.block_align;
                        const int16_t *src = (const int16_t *)(const void *)g_file_buf;
                        uint32_t total_s = decoded_frames * (uint32_t)winfo.channels;
                        for (uint32_t i = 0u; i < total_s; i++) {
                            g_dec_pcm[i] = src[i];
                        }
                    }

                    if (decoded_frames > 1024u) {
                        decoded_frames = 1024u;
                    }

                    int32_t peak = 0;
                    if (winfo.channels == 1u) {
                        for (uint32_t i = 0u; i < decoded_frames; i++) {
                            int16_t s = g_dec_pcm[i];
                            int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
                            if (a > peak) peak = a;
                            g_stereo_tx[i * 2u + 0u] = s;
                            g_stereo_tx[i * 2u + 1u] = s;
                        }
                    } else {
                        for (uint32_t i = 0u; i < decoded_frames; i++) {
                            int16_t l = g_dec_pcm[i * 2u + 0u];
                            int16_t r = g_dec_pcm[i * 2u + 1u];
                            int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
                            int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
                            if (al > peak) peak = al;
                            if (ar > peak) peak = ar;
                            g_stereo_tx[i * 2u + 0u] = l;
                            g_stereo_tx[i * 2u + 1u] = r;
                        }
                    }
                    cur_peak_pct = (uint8_t)((peak * 100) / 32767);
                    buf_pos      = 0u;
                    buf_frames   = decoded_frames;
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
            uint32_t pos_ms = frames_to_ms(played_frames, winfo.sample_rate);
            uint32_t dur_ms = (has_cwx && cwx.duration_ms > 0u) ? cwx.duration_ms : winfo.duration_ms;
            if (pos_ms > dur_ms && dur_ms > 0u) {
                pos_ms = dur_ms;
            }
            draw_player_chrome(wav_name, &winfo, &cwx, has_cwx, pos_ms, dur_ms,
                               volume, paused, eof_reached, keys_read_hp_detect(), cur_peak_pct);
        }

        if (paused || eof_reached || (buf_pos < buf_frames && audio_dac_is_busy())) {
            __asm__ volatile ("wfi");
        }
    }

    audio_dac_stop();
    lcd_set_rotation(1); /* Keep landscape 160x80 */

    while (keys_decode(keys_read_adc()) == KEY_BACK) {
        wdt_feed();
        mdelay(20u);
    }
}
