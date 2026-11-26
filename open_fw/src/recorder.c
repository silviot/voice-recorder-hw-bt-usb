/*
 * Smartlink SL6801 OpenFW Real-Time WAV Recorder + Clean-Room DSP
 *
 * Streams 16 kHz / 16-bit mono (AMIC1) or dual-mic stereo (AMIC1+AMIC2) PCM
 * frames from DMAC CH1 ring buffers through clean-room audio_dsp_process_i16()
 * (defaulting to Voice Clarity) and optional opt-in VOR pre-roll gating
 * into /record/REC_NNNN.wav on exFAT.
 *
 * Crash-safety & durability:
 *   - Periodically checkpoints a valid WAV header + exFAT directory entry
 *     (first_cluster + flushed length) so power loss leaves a playable prefix.
 *   - Automatically splits long recordings into sequential REC_NNNN.wav files
 *     before the 32-bit RIFF/WAV limit (~2 GiB).
 *   - Tracks persistent storage/write errors and reports authoritative
 *     start/stop results to the UI and Bluetooth transport.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "openfw.h"
#include "audio_dsp.h"
#include "openfw_wav.h"
#include "openfw_bench_meas.h"   /* F0 counters: bench images only (OPENFW_BENCH_MEAS), compiled out otherwise */
#if OPENFW_BENCH_MEAS
#if defined(OPENFW_RTOS) && OPENFW_RTOS
#ifdef OPENFW_HOST_MMIO
#define REC_LOG_CRIT_ENTER() openfw_host_rtos_crit_enter()
#define REC_LOG_CRIT_EXIT()  openfw_host_rtos_crit_exit()
#else
#include "FreeRTOS.h"            /* BASEPRI critical section (AGENTS.md: never mask priority-0 audio DMAC IRQs) */
#include "task.h"
#define REC_LOG_CRIT_ENTER() taskENTER_CRITICAL()
#define REC_LOG_CRIT_EXIT()  taskEXIT_CRITICAL()
#endif
#else
#include "sl6801_regs.h"
#define REC_LOG_CRIT_ENTER() uint32_t pm; OPENFW_IRQ_SAVE(pm)
#define REC_LOG_CRIT_EXIT()  OPENFW_IRQ_RESTORE(pm)
#endif
#endif

/* Checkpoint every 64 KiB (~2.0 s mono / ~1.0 s stereo) and after first 4 KiB */
#define REC_CKPT_INTERVAL_BYTES 65536u
#define REC_WRITE_RETRIES       2u      /* extra exfat_write attempts for one PCM block (rec_write_retry) */
/* Split WAV files safely below signed 32-bit / 4 GiB RIFF limit (~2 GiB) */
#define REC_MAX_PCM_BYTES       0x7FFF0000u
#if defined(OPENFW_HOST_MMIO)
/* Host tests force a 2 GiB split with a small threshold (recorder_test_set_split_bytes). */
static uint32_t g_rec_split_bytes = REC_MAX_PCM_BYTES;
void recorder_test_set_split_bytes(uint32_t n);
void recorder_test_set_split_bytes(uint32_t n)
{
    g_rec_split_bytes = (n != 0u) ? n : REC_MAX_PCM_BYTES;
}
#define REC_SPLIT_BYTES g_rec_split_bytes
#else
#define REC_SPLIT_BYTES REC_MAX_PCM_BYTES
#endif

static int                 g_rec_active      = 0;
static int                 g_rec_saving      = 0;
static int                 g_rec_error       = 0;
static uint8_t             g_mic_gain        = 20u;
static uint8_t             g_gain_locked     = 0u;
static uint8_t             g_active_pga      = 20u;
static uint8_t             g_pga_cooldown    = 0u;
static uint16_t            g_pga_quiet_blks  = 0u;
static uint8_t             g_cfg_channels    = 1u;
static uint8_t             g_active_channels = 1u;
static audio_dsp_profile_t g_dsp_profile     = AUDIO_DSP_DEFAULT_PROFILE;
static int                 g_vor_enabled     = 0;

static audio_dsp_t         g_dsp;
static audio_vor_t         g_vor;
static int16_t             g_proc_buf[4096] __attribute__((aligned(4)));
static exfat_vol_t        *g_rec_vol         = 0;
static exfat_writer_t      g_writer;
static uint32_t            g_pcm_bytes       = 0u;
static uint32_t            g_last_ckpt_bytes = 0u;
static uint32_t            g_flushed_wav_bytes = 0u;
static uint32_t            g_prev_split_sec  = 0u;
static uint32_t            g_last_overruns   = 0u;
static uint32_t            g_created_timestamp;
static char                g_filename[16];

static uint32_t parse_rec_index(const char *name)
{
    /* Match "REC_NNNN.wav" case-insensitively */
    const char *pfx = "REC_";
    for (int i = 0; i < 4; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        }
        if (c != pfx[i]) {
            return 0u;
        }
    }
    uint32_t num = 0u;
    for (int i = 4; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0u;
        }
        num = num * 10u + (uint32_t)(name[i] - '0');
    }
    if (name[8] != '.') {
        return 0u;
    }
    const char *ext = "wav";
    for (int i = 0; i < 3; i++) {
        char c = name[9 + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        if (c != ext[i]) {
            return 0u;
        }
    }
    return (name[12] == '\0') ? num : 0u;
}

static int max_rec_cb(const exfat_entry_t *entry, void *user)
{
    if (!entry->is_dir) {
        uint32_t *max_idx = (uint32_t *)user;
        uint32_t idx = parse_rec_index(entry->name);
        if (idx > *max_idx && idx <= 9999u) {
            *max_idx = idx;
        }
    }
    return 0;
}

static void format_rec_filename(uint32_t idx, char out[16])
{
    out[0]  = 'R';
    out[1]  = 'E';
    out[2]  = 'C';
    out[3]  = '_';
    out[4]  = (char)('0' + ((idx / 1000u) % 10u));
    out[5]  = (char)('0' + ((idx / 100u) % 10u));
    out[6]  = (char)('0' + ((idx / 10u) % 10u));
    out[7]  = (char)('0' + (idx % 10u));
    out[8]  = '.';
    out[9]  = 'w';
    out[10] = 'a';
    out[11] = 'v';
    out[12] = '\0';
}

static void rec_log_err(const char *stage, uint32_t v)
{
#if !defined(OPENFW_HOST_MMIO) || defined(OPENFW_WASM)
    openfw_log_puts("REC_ERR ");
    openfw_log_puts(stage);
    openfw_log_puts("=");
    openfw_log_u32(v);
    openfw_log_putc('\n');
#else
    (void)stage;
    (void)v;
#endif
}

static int open_next_wav_file(exfat_vol_t *vol)
{
    if (!vol) {
        return -1;
    }

    rtc_datetime_t now;
    rtc_get(&now);
    exfat_set_timestamp(vol, rtc_to_dos_timestamp(&now));

    exfat_entry_t rec_dir;
    if (exfat_find_path(vol, "/record", &rec_dir) != 0 || !rec_dir.is_dir) {
        if (exfat_mkdir(vol, "record") != 0 ||
            exfat_find_path(vol, "/record", &rec_dir) != 0 || !rec_dir.is_dir) {
            rec_log_err("mkdir", 0u);
            return -1;
        }
    }

    uint32_t max_idx = 0u;
    (void)exfat_list_dir_entry(vol, &rec_dir, max_rec_cb, &max_idx);
    uint32_t next_idx = (max_idx < 9999u) ? (max_idx + 1u) : 1u;

    for (uint32_t tries = 0u; tries < 9999u; tries++) {
        uint32_t cand_idx = ((next_idx - 1u + tries) % 9999u) + 1u;
        char name[16];
        format_rec_filename(cand_idx, name);

        /* Fast path: when max_idx < 9999, next_idx = max_idx + 1 is guaranteed unused,
         * so skip the redundant exfat_find_path() directory scan. */
        if (tries == 0u && max_idx < 9999u) {
            if (exfat_create_file(vol, "/record", name, &g_writer) != 0) {
                rec_log_err("create", cand_idx);
                return -1;
            }
        } else {
            char full[24];
            const char *pfx = "/record/";
            int p = 0;
            for (int i = 0; pfx[i] != '\0'; i++) {
                full[p++] = pfx[i];
            }
            for (int i = 0; name[i] != '\0'; i++) {
                full[p++] = name[i];
            }
            full[p] = '\0';

            exfat_entry_t existing;
            if (exfat_find_path(vol, full, &existing) == 0) {
                continue;
            }
            if (exfat_create_file(vol, "/record", name, &g_writer) != 0) {
                rec_log_err("create", cand_idx);
                return -1;
            }
        }

        uint8_t hdr[44];
        (void)wav_build_header(hdr, 16000u, (uint16_t)g_active_channels, 16u, 0u);
        if (exfat_write(&g_writer, hdr, 44u) != 0) {
            rec_log_err("hdr", cand_idx);
            (void)exfat_close_file(&g_writer);
            return -1;
        }
        for (int i = 0; i < 16; i++) {
            g_filename[i] = name[i];
        }
        g_created_timestamp = vol->default_timestamp;
        g_pcm_bytes         = 0u;
        g_last_ckpt_bytes   = 0u;
        g_flushed_wav_bytes = 0u;
        g_rec_saving        = 1;
        return 0;
    }
    return -1;
}

static int finalize_current_wav_file(void)
{
    if (!g_rec_saving) {
        return -1;
    }
    if (g_writer.vol) {
        rtc_datetime_t now;
        rtc_get(&now);
        exfat_set_timestamp(g_writer.vol, rtc_to_dos_timestamp(&now));
    }

    int rc_close = exfat_close_file(&g_writer);
    uint32_t committed_pcm = (g_writer.bytes_written > 44u)
                           ? (uint32_t)(g_writer.bytes_written - 44u)
                           : 0u;
    g_pcm_bytes = committed_pcm;
    uint8_t hdr[44];
    (void)wav_build_header(hdr, 16000u, (uint16_t)g_active_channels, 16u, committed_pcm);
    int rc_patch = (g_writer.first_cluster >= 2u)
                 ? exfat_patch_header(&g_writer, hdr, 44u)
                 : -1;
    g_flushed_wav_bytes = 0u;
    g_rec_saving        = 0;
    return (rc_patch == 0 && rc_close == 0) ? 0 : -1;
}

static void recorder_checkpoint_if_due(void)
{
    if (!g_rec_saving || g_writer.first_cluster < 2u) {
        return;
    }
    uint64_t flushed = (g_writer.bytes_written >= (uint64_t)g_writer.sec_buf_used)
                     ? (g_writer.bytes_written - (uint64_t)g_writer.sec_buf_used)
                     : 0u;
    if (flushed <= 44u) {
        return;
    }
    uint32_t flushed_pcm = (uint32_t)(flushed - 44u);
    int due = (g_last_ckpt_bytes == 0u && flushed >= 4096u) ||
              (flushed_pcm >= g_last_ckpt_bytes + REC_CKPT_INTERVAL_BYTES);
    if (!due) {
        return;
    }
    if (g_writer.vol) {
        rtc_datetime_t now;
        rtc_get(&now);
        exfat_set_timestamp(g_writer.vol, rtc_to_dos_timestamp(&now));
    }
    uint8_t hdr[44];
    (void)wav_build_header(hdr, 16000u, (uint16_t)g_active_channels, 16u, flushed_pcm);
    if (exfat_checkpoint_file(&g_writer, hdr, 44u) != 0) {
        rec_log_err("ckpt", flushed_pcm);
        g_rec_error = 1;
    } else {
        g_last_ckpt_bytes   = flushed_pcm;
        g_flushed_wav_bytes = (uint32_t)flushed;
    }
}

/* exfat_write() with bounded retries. The SD driver already retries every sector (state recovery, re-init) and reports
 * BLKDEV_ERR_FINAL when it gave up; that is not retried here. This layer covers a failure that surfaced anyway (e.g.
 * a cluster allocation that failed and was rolled back). exfat_write
 * advances bytes_written by exactly the bytes it consumed, also when it fails part-way, so a retry continues with the
 * unconsumed remainder: nothing is written twice, skipped or reordered. */
static int rec_write_retry(const int16_t *pcm, uint32_t nbytes)
{
    const uint8_t *src = (const uint8_t *)pcm;
    uint64_t bw0 = g_writer.bytes_written;
    for (uint32_t attempt = 0u; attempt <= REC_WRITE_RETRIES; attempt++) {
        uint64_t took = g_writer.bytes_written - bw0;
        if (took > (uint64_t)nbytes) {
            return -1;                               /* cannot happen: the writer took more than it was given */
        }
        if (attempt > 0u) {
            rec_log_err("wretry", g_pcm_bytes + (uint32_t)took);
        }
        int rc = exfat_write(&g_writer, src + took, nbytes - (uint32_t)took);
        if (rc == 0) {
            return 0;
        }
        if (rc == BLKDEV_ERR_FINAL || !g_writer.active) {
            return -1;     /* the SD driver already retried and re-initialised: another round only stalls longer */
        }
    }
    return -1;
}

static void rec_vor_emit_cb(void *user, const int16_t *pcm, size_t num_frames)
{
    (void)user;
    if (!g_rec_saving || !pcm || num_frames == 0u) {
        return;
    }
    uint32_t nbytes = (uint32_t)(num_frames * (size_t)g_active_channels * sizeof(int16_t));

    /* Split seamlessly before 32-bit RIFF/WAV limit */
    if (g_pcm_bytes > REC_SPLIT_BYTES - nbytes) {
        uint32_t bps = 32000u * (uint32_t)(g_active_channels ? g_active_channels : 1u);
        g_prev_split_sec += g_pcm_bytes / bps;
        if (finalize_current_wav_file() != 0 || open_next_wav_file(g_rec_vol) != 0) {
            g_rec_error  = 1;
            g_rec_saving = 0;
            return;
        }
    }

#if OPENFW_BENCH_MEAS
    uint32_t bm_t0 = (uint32_t)us_timer_now();
#endif
    if (rec_write_retry(pcm, nbytes) != 0) {
        rec_log_err("write", g_pcm_bytes);
        g_rec_error = 1;
        /* Best-effort commit of whatever was already flushed */
        (void)finalize_current_wav_file();
        g_rec_saving = 0;
#if OPENFW_BENCH_MEAS
        bench_meas_write(&g_bench_meas, 0u, (uint32_t)us_timer_now() - bm_t0, 0u);
#endif
    } else {
        g_pcm_bytes += nbytes;
#if OPENFW_BENCH_MEAS
        uint32_t bm_t1 = (uint32_t)us_timer_now();
        recorder_checkpoint_if_due();
        uint32_t bm_t2 = (uint32_t)us_timer_now();
        bench_meas_write(&g_bench_meas, (uint32_t)num_frames, bm_t1 - bm_t0, bm_t2 - bm_t1);
#else
        recorder_checkpoint_if_due();
#endif
    }
}

void recorder_set_gain(uint8_t gain_0_31)
{
    g_gain_locked = 1u;
    g_mic_gain    = (gain_0_31 > 31u) ? 31u : gain_0_31;
    g_active_pga  = g_mic_gain;
    audio_capture_set_gain(g_mic_gain);
}

void recorder_enable_auto_gain(void)
{
    g_gain_locked    = 0u;
    g_mic_gain       = 20u;
    g_active_pga     = 20u;
    g_pga_cooldown   = 0u;
    g_pga_quiet_blks = 0u;
    audio_capture_set_gain(g_mic_gain);
}

uint8_t recorder_get_gain(void)
{
    return g_mic_gain;
}

void recorder_set_dsp_profile(audio_dsp_profile_t profile)
{
    if ((uint32_t)profile > (uint32_t)AUDIO_DSP_BYPASS) {
        profile = AUDIO_DSP_DEFAULT_PROFILE;
    }
    g_dsp_profile = profile;
}

audio_dsp_profile_t recorder_get_dsp_profile(void)
{
    return g_dsp_profile;
}

void recorder_set_channels(uint8_t channels)
{
    g_cfg_channels = (channels == 2u) ? 2u : 1u;
}

uint8_t recorder_get_channels(void)
{
    return g_rec_active ? g_active_channels : g_cfg_channels;
}

void recorder_set_vor(int enabled)
{
    g_vor_enabled = enabled ? 1 : 0;
}

int recorder_get_vor(void)
{
    return g_vor_enabled;
}

uint32_t recorder_get_clip_events(void)
{
    return g_dsp.clip_events;
}

int16_t recorder_get_rms_dbfs_x10(void)
{
    float db = g_dsp.rms_dbfs * 10.0f;
    if (db < -960.0f) {
        db = -960.0f;
    } else if (db > 0.0f) {
        db = 0.0f;
    }
    return (db >= 0.0f) ? (int16_t)(db + 0.5f) : (int16_t)(db - 0.5f);
}

int recorder_start(exfat_vol_t *vol, int sd_mounted)
{
    if (g_rec_active) {
        return 0;
    }
    g_rec_error         = 0;
    g_rec_saving        = 0;
    g_rec_vol           = vol;
    g_pcm_bytes         = 0u;
    g_last_ckpt_bytes   = 0u;
    g_flushed_wav_bytes = 0u;
    g_prev_split_sec    = 0u;
    g_filename[0]       = '\0';

    if (!sd_mounted || !vol) {
        g_rec_error = 1;
        return -1;
    }

    if (!g_gain_locked) {
        g_active_pga     = 20u;
        g_mic_gain       = 20u;
        g_pga_cooldown   = 0u;
        g_pga_quiet_blks = 0u;
    } else {
        g_active_pga = g_mic_gain;
    }

    g_active_channels = g_cfg_channels;
    audio_dsp_init(&g_dsp, g_dsp_profile, 16000u, g_active_channels);
    audio_dsp_set_agc(&g_dsp, (g_dsp_profile != AUDIO_DSP_BYPASS) ? 1 : 0);
    audio_vor_init(&g_vor, 16000u, g_active_channels, g_vor_enabled);

    if (open_next_wav_file(vol) != 0) {
        g_rec_error = 1;
        return -1;
    }

    if (audio_capture_start_ex(g_mic_gain, g_active_channels) != 0) {
        (void)finalize_current_wav_file();
        g_rec_error = 1;
        return -1;
    }

    g_last_overruns = audio_get_overruns();
    g_rec_active    = 1;
#if OPENFW_BENCH_MEAS
    bench_meas_rec_start(&g_bench_meas, g_filename, g_active_channels, g_mic_gain, (uint8_t)g_dsp_profile,
                         (uint8_t)g_vor_enabled, (uint32_t)us_timer_now());
#endif
    return 0;
}

#if OPENFW_BENCH_MEAS
/* The DMA ISR's latest block count and its us_timer stamp, read as a consistent pair (block clock). */
static void bm_dma_stamp(void)
{
    uint32_t f1, us;
    do {
        f1 = audio_get_frames();
        us = audio_bench_dma_us();
    } while (audio_get_frames() != f1);
    bench_meas_dma_stamp(&g_bench_meas, f1, us);
}
#endif

void recorder_poll(void)
{
    if (!g_rec_active) {
        return;
    }
#if OPENFW_BENCH_MEAS
    uint32_t bm_enter = (uint32_t)us_timer_now();
    bench_meas_poll_enter(&g_bench_meas, bm_enter);
#endif

    /* Compensate any DMA ring overruns with silence (only when not paused by VOR gate,
     * and capped per poll so an SD hiccup cannot trigger a cascading zero-write storm). */
    uint32_t ov = audio_get_overruns();
    if (ov != g_last_overruns) {
        uint32_t dropped = ov - g_last_overruns;
        g_last_overruns  = ov;
#if OPENFW_BENCH_MEAS
        uint32_t bm_dropped = dropped, bm_comp = 0u;
#endif
        if (g_rec_saving && (!g_vor.enabled || g_vor.gate_open)) {
            if (dropped > 2u) {
                dropped = 2u;
            }
            uint32_t samps = 2048u * (uint32_t)g_active_channels;
            for (uint32_t i = 0u; i < samps; i++) {
                g_proc_buf[i] = 0;
            }
            for (uint32_t d = 0u; d < dropped && g_rec_saving; d++) {
                rec_vor_emit_cb(0, g_proc_buf, 2048u);
#if OPENFW_BENCH_MEAS
                bm_comp++;
#endif
            }
        }
#if OPENFW_BENCH_MEAS
        bench_meas_overrun(&g_bench_meas, bm_dropped, bm_comp);
#endif
    }

    uint32_t nbytes = 0u;
    while (audio_pop_frame_copy(g_proc_buf, (uint32_t)sizeof(g_proc_buf), &nbytes)) {
        uint32_t total_samples = nbytes / (uint32_t)sizeof(int16_t);
        size_t num_frames = total_samples / (size_t)g_active_channels;
        if (!g_gain_locked && g_dsp_profile != AUDIO_DSP_BYPASS && g_rec_saving) {
            int32_t raw_peak = 0;
            for (uint32_t i = 0u; i < total_samples; i++) {
                int32_t a = g_proc_buf[i];
                if (a < 0) a = -a;
                if (a > raw_peak) raw_peak = a;
            }
            if (g_pga_cooldown > 0u) {
                g_pga_cooldown--;
            }
            if (raw_peak >= 31200 && g_active_pga > 8u && g_pga_cooldown == 0u) {
                g_active_pga = (g_active_pga >= 10u) ? (uint8_t)(g_active_pga - 2u) : 8u;
                g_mic_gain   = g_active_pga;
                audio_capture_set_gain(g_active_pga);
                g_pga_cooldown   = 4u;
                g_pga_quiet_blks = 0u;
            } else if (raw_peak < 2000 && (!g_vor.enabled || g_vor.gate_open) && g_active_pga < 20u) {
                g_pga_quiet_blks++;
                if (g_pga_quiet_blks >= 64u) {
                    g_pga_quiet_blks = 0u;
                    g_active_pga++;
                    g_mic_gain = g_active_pga;
                    audio_capture_set_gain(g_active_pga);
                }
            } else {
                g_pga_quiet_blks = 0u;
            }
        }
        audio_dsp_process_i16(&g_dsp, g_proc_buf, num_frames);
        if (g_rec_saving) {
            audio_vor_feed(&g_vor, g_proc_buf, num_frames, rec_vor_emit_cb, 0);
        }
    }
#if OPENFW_BENCH_MEAS
    bm_dma_stamp();
    bench_meas_poll_exit(&g_bench_meas, bm_enter, (uint32_t)us_timer_now(), audio_get_frames());
#endif
}

#if OPENFW_BENCH_MEAS
/* "BENCH rec ..." at every REC stop (docs/BT_HW_RESULTS.md, M2/M3 recipe; read with dev_cycle.py log or +XREC:LOG).
 * Pushed whole under FreeRTOS BASEPRI (no task switch or BT IRQ, priority-0 audio DMAC IRQs stay unmasked). */
static void bm_log_rec(void)
{
    static char bm_line[16u + BENCH_MEAS_LINE_MAX + BENCH_MEAS_SDW_LINE_MAX];
    static const char k_tag[] = "BENCH rec ";
    uint32_t n = 0u;
    while (k_tag[n] != '\0') {
        bm_line[n] = k_tag[n];
        n++;
    }
    n += bench_meas_format_rec(&g_bench_meas.cur, ' ', &bm_line[n], (uint32_t)sizeof(bm_line) - n - 2u);
    bm_line[n++] = ' ';
    n += bench_meas_format_sdw(&g_bench_meas.cur, ' ', &bm_line[n], (uint32_t)sizeof(bm_line) - n - 1u);
    bm_line[n++] = '\n';
    bm_line[n] = '\0';
    REC_LOG_CRIT_ENTER();
    uint32_t w = g_openfw_log.write_idx;
    if (w != 0u && g_openfw_log.buf[(w - 1u) & (OPENFW_LOG_SIZE - 1u)] != '\n') {
        openfw_log_putc('\n');   /* another task was mid-line */
    }
    openfw_log_puts(bm_line);
    REC_LOG_CRIT_EXIT();
}
#endif

int recorder_stop(void)
{
    if (!g_rec_active) {
        return g_rec_error ? -1 : 0;
    }

    audio_capture_stop();
#if OPENFW_BENCH_MEAS
    bench_meas_capture_end(&g_bench_meas, (uint32_t)us_timer_now());   /* the clock side of the sample accounting */
#endif
    recorder_poll();

    int fin_rc = finalize_current_wav_file();
    g_rec_active = 0;
#if OPENFW_BENCH_MEAS
    bench_meas_rec_stop(&g_bench_meas, audio_get_frames(), audio_get_overruns());
    bm_log_rec();
#endif

    if (fin_rc != 0 || g_rec_error != 0) {
        g_rec_error = 1;
        return -1;
    }
    return 0;
}

int recorder_is_saving(void)
{
    return g_rec_saving;
}

int recorder_has_error(void)
{
    return g_rec_error;
}

int recorder_is_vor_waiting(void)
{
    return (g_rec_active && g_vor.enabled && !g_vor.gate_open) ? 1 : 0;
}

uint32_t recorder_get_seconds(void)
{
    uint32_t bps = 32000u * (uint32_t)(g_active_channels ? g_active_channels : 1u);
    if (g_pcm_bytes > 0u || g_prev_split_sec > 0u) {
        return g_prev_split_sec + (g_pcm_bytes / bps);
    }
    if (g_vor.enabled) {
        return 0u;
    }
    return (audio_get_frames() * 128u) / 1000u;
}

uint32_t recorder_get_committed_bytes(void)
{
    return (g_pcm_bytes > 0u) ? (g_pcm_bytes + 44u) : 0u;
}

uint32_t recorder_get_flushed_wav_bytes(void)
{
    return g_rec_saving ? g_flushed_wav_bytes : 0u;
}

uint32_t recorder_get_created_timestamp(void)
{
    return g_created_timestamp;
}

const char *recorder_get_filename(void)
{
    return g_filename;
}

/* NNNN of "REC_NNNN.wav" (1..9999, any case), 0 for any other name. Pure (host-tested). */
uint32_t recorder_rec_index_of(const char *name)
{
    static const char pfx[] = "rec_";
    static const char ext[] = ".wav";
    if (name == NULL) {
        return 0u;
    }
    uint32_t idx = 0u;
    for (uint32_t i = 0u; i < 12u; i++) {
        char c = name[i];
        if (c == '\0') {
            return 0u;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (i < 4u) {
            if (c != pfx[i]) {
                return 0u;
            }
        } else if (i < 8u) {
            if (c < '0' || c > '9') {
                return 0u;
            }
            idx = idx * 10u + (uint32_t)(c - '0');
        } else if (c != ext[i - 8u]) {
            return 0u;
        }
    }
    return (name[12] == '\0') ? idx : 0u;
}

/* STAT rec_idx (F7): the index of the file being written, 0 when none (also after a split: the new file's). */
uint32_t recorder_get_rec_index(void)
{
    return g_rec_saving ? recorder_rec_index_of(g_filename) : 0u;
}

/* XREC DEL guard (review STEP10 A1): non-zero while /record/<name> (case-insensitive, as exFAT) is being written. */
int recorder_is_recording_file(const char *name)
{
    if (!g_rec_saving || name == NULL) {
        return 0;
    }
    for (uint32_t i = 0u; i < sizeof(g_filename); i++) {
        char a = g_filename[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
        if (a == '\0') {
            return 1;
        }
    }
    return 0;
}
