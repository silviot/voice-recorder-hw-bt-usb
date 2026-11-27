/* Exercise the production playback loop with a 16 kHz DMA clock and a blocking LCD. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "openfw.h"
#include "openfw_player.h"
#include "openfw_wav.h"

enum { FRAMES = 16017, SLOT_FRAMES = 1024 };
static uint8_t recording[4096 + FRAMES * 4];
static unsigned channels, file_size, now_ms, submitted, consumed, gaps, flush_calls;
static unsigned slots[AUDIO_DAC_QUEUE_SLOTS], head, count;
static int ended;
static int strict = 1;           /* full-drain assertions (plain playback runs) */
static int track_pcm = 0;        /* capture heard PCM stream across pause/resume */
enum { HEARD_MAX = 16000 * 12 };
static int16_t heard_l[HEARD_MAX], heard_r[HEARD_MAX];
static uint16_t heard_t[HEARD_MAX];   /* ms at which each frame was heard */
static unsigned heard_n;
enum { FLUSH_LOG = 16 };
static unsigned flush_at[FLUSH_LOG], flush_ms[FLUSH_LOG];
static int16_t slot_l[AUDIO_DAC_QUEUE_SLOTS][SLOT_FRAMES], slot_r[AUDIO_DAC_QUEUE_SLOTS][SLOT_FRAMES];
static unsigned read_last_ms;
static unsigned fail_read_after_frames;
static unsigned win_from, win_to, win_writes, win_reads; /* activity inside [win_from, win_to) */
static int16_t before_win_l;     /* last left sample submitted before win_from */
static int max_scrub_speed;
static uint8_t last_play_state;
static uint32_t last_pos_ms;
static unsigned slot_off[AUDIO_DAC_QUEUE_SLOTS];
static unsigned unaligned_reads_after_first;
static unsigned pcm_read_idx;
static key_id_t (*key_script)(unsigned ms);
static unsigned vol_calls;
static int drop_volume_release;
static unsigned lookup_delay;
static unsigned time_limit_ms = 6000;
static uint32_t synth_frames;    /* >0: stereo file synthesized on read (L = frame % 30000, R = frame / 30000) */
static int synth_tri;            /* synthesized file is a 200 Hz triangle on both channels instead */

/* play_tick_cb probe */
static const ui_model_t *g_tick_model;
static unsigned tick_calls, tick_max_pos, tick_saw_end;
static void count_tick(uint32_t now_ms)
{
    (void)now_ms;
    tick_calls++;
    if (g_tick_model->play_pos_sec > tick_max_pos) tick_max_pos = g_tick_model->play_pos_sec;
    if (g_tick_model->play_state == 2u) tick_saw_end = 1;
}

/* Simulated 1 kHz SysTick key debouncer + SPSC queue */
static key_event_t stub_q[32];
static unsigned    stub_q_head, stub_q_tail;
static key_id_t    stub_stable = KEY_NONE;
static uint32_t    stub_stable_since = 0;
static key_id_t    stub_cand = KEY_NONE;
static unsigned    stub_cand_cnt = 0;

static void stub_keys_reset(void)
{
    stub_q_head = stub_q_tail = 0;
    stub_stable = KEY_NONE;
    stub_stable_since = 0;
    stub_cand = KEY_NONE;
    stub_cand_cnt = 0;
}

static void stub_keys_push(uint32_t t, key_id_t k, uint8_t pressed)
{
    if (drop_volume_release && k == KEY_VOL_UP && !pressed) return;
    if (stub_q_head - stub_q_tail < 32u) {
        stub_q[stub_q_head & 31u] = (key_event_t){ t, k, pressed };
        stub_q_head++;
    }
}

static void stub_keys_tick_1ms(unsigned t)
{
    key_id_t raw = key_script ? key_script(t) : KEY_NONE;
    if (raw == stub_stable) {
        stub_cand = raw;
        stub_cand_cnt = 0;
        return;
    }
    if (raw == stub_cand) {
        stub_cand_cnt++;
    } else {
        stub_cand = raw;
        stub_cand_cnt = 1;
    }
    if (stub_cand_cnt >= 8u) {
        key_id_t old = stub_stable;
        stub_stable = raw;
        stub_stable_since = t;
        stub_cand_cnt = 0;
        if (old != KEY_NONE) stub_keys_push(t, old, 0u);
        if (raw != KEY_NONE) stub_keys_push(t, raw, 1u);
    }
}

static int16_t sample(unsigned frame, unsigned channel)
{
    return (int16_t)((int)(frame % 30000) * (channel ? -1 : 1));
}

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        now_ms++;
        assert(now_ms < time_limit_ms);
        stub_keys_tick_1ms(now_ms);
    }
    unsigned frames = 16 * ms;
    while (frames && count) {
        unsigned take = slots[head] < frames ? slots[head] : frames;
        if (track_pcm) {
            for (unsigned i = 0; i < take && heard_n < HEARD_MAX; i++) {
                heard_r[heard_n] = slot_r[head][slot_off[head] + i];
                heard_t[heard_n] = (uint16_t)now_ms;
                heard_l[heard_n++] = slot_l[head][slot_off[head] + i];
            }
        }
        slot_off[head] += take;
        slots[head] -= take;
        frames -= take;
        consumed += take;
        if (!slots[head]) {
            slot_off[head] = 0;
            head = (head + 1u) % AUDIO_DAC_QUEUE_SLOTS;
            count--;
        }
    }
    if (submitted && consumed < FRAMES) gaps += frames;
}

int audio_dac_start(uint32_t rate, uint8_t level)
{
    assert(rate == 16000 && level == 18);
    return 0;
}
int audio_dac_write_stereo(const int16_t *pcm, uint32_t frames)
{
    if (count == AUDIO_DAC_QUEUE_SLOTS) return 0;
    if (frames > SLOT_FRAMES) frames = SLOT_FRAMES;
    for (unsigned i = 0; strict && i < frames; i++) { /* in-order samples */
        assert(pcm[2*i] == sample(submitted+i, 0));
        assert(pcm[2*i+1] == sample(submitted+i, channels-1));
    }
    unsigned idx = (head + count) % AUDIO_DAC_QUEUE_SLOTS;
    if (track_pcm) {
        for (unsigned i = 0; i < frames; i++) {
            slot_l[idx][i] = pcm[2*i];
            slot_r[idx][i] = pcm[2*i+1];
        }
        slot_off[idx] = 0;
    }
    submitted += frames;
    if (now_ms >= win_from && now_ms < win_to) win_writes++;
    if (now_ms < win_from) before_win_l = pcm[2 * (frames - 1)];
    slots[idx] = frames;
    count++;
    return (int)frames;
}
int audio_dac_is_busy(void) { return count != 0; }
void audio_dac_flush(void)
{
    if (flush_calls < FLUSH_LOG) {
        flush_at[flush_calls] = heard_n;
        flush_ms[flush_calls] = now_ms;
    }
    flush_calls++;
    head = count = 0;
    for (unsigned i = 0; i < AUDIO_DAC_QUEUE_SLOTS; i++) {
        slots[i] = 0;
        slot_off[i] = 0;
    }
}
uint32_t audio_dac_queued_frames(void)
{
    uint32_t total = 0u;
    for (unsigned i = 0u; i < count; i++) {
        total += slots[(head + i) % AUDIO_DAC_QUEUE_SLOTS];
    }
    return total;
}
void audio_pa_set(int on) { if (!on && strict) assert(consumed == FRAMES); }
void audio_dac_stop(void) { if (strict) assert(!count); count = 0; }
void audio_dac_set_volume(uint8_t level, uint16_t digital) { (void)level; (void)digital; vol_calls++; }
void audio_dac_set_mute(int mute) { (void)mute; }

static const char *marker_path;   /* existing .autoplay file (exFAT: case-insensitive) */
static char last_lookup[128];
static uint8_t marker_is_dir;

int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *ent)
{
    (void)vol;
    snprintf(last_lookup, sizeof(last_lookup), "%s", path);
    if (marker_path && strcasecmp(path, marker_path) == 0) {
        *ent = (exfat_entry_t){ .size = 0u, .is_dir = marker_is_dir };
        return 0;
    }
    if (strcmp(path, "/record/test.wav")) return -1;
    advance(lookup_delay);
    *ent = (exfat_entry_t){ .size = file_size };
    return 0;
}
int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *ent,
                    uint64_t offset, void *dst, uint32_t len, uint32_t *got)
{
    (void)vol;
    assert(offset + len <= ent->size);
    if (fail_read_after_frames && consumed >= fail_read_after_frames) {
        *got = 0;
        return -1;
    }
    if (offset > 0u) {
        if (pcm_read_idx > 0u && (offset & 511u) != 0u) {
            unaligned_reads_after_first++;
        }
        pcm_read_idx++;
        read_last_ms = now_ms;
        if (now_ms >= win_from && now_ms < win_to) win_reads++;
    }
    if (synth_frames) {
        uint8_t *d = dst;
        for (uint32_t i = 0; i < len; i++) {
            uint64_t o = offset + i;
            if (o < 44u) { d[i] = recording[o]; continue; }
            uint32_t frame = (uint32_t)((o - 44u) / 4u);
            uint16_t v = (uint16_t)((o - 44u) % 4u < 2u ? frame % 30000u : frame / 30000u);
            if (synth_tri) {
                int ph = (int)(frame % 80u);
                v = (uint16_t)(int16_t)((ph < 40 ? ph : 80 - ph) * 400 - 8000);
            }
            d[i] = (uint8_t)(((o - 44u) & 1u) ? v >> 8 : v);
        }
    } else {
        memcpy(dst, recording + offset, len);
    }
    *got = len;
    advance(1);
    return 0;
}
static int (*screen_awake_script)(unsigned ms);

int ui_poll_power_button(ui_screen_id_t screen, ui_model_t *m, uint32_t now)
{
    (void)screen; (void)m;
    return screen_awake_script ? screen_awake_script(now) : 1;
}

void ui_render_screen(ui_screen_id_t screen, const ui_model_t *m)
{
    assert(screen == UI_SCR_PLAYER);
    ended = m->play_state == 2;
    int spd = m->play_scrub_speed < 0 ? -m->play_scrub_speed : m->play_scrub_speed;
    if (spd > max_scrub_speed) max_scrub_speed = spd;
    last_play_state = m->play_state;
    last_pos_ms = m->play_pos_ms;
    advance(strict ? 150u : 45u);
}
void ui_dispatch_tick(void) { }
int  ui_dispatch_is_animating(void) { return 0; }
void ui_settings_defaults(ui_settings_t *cfg) { cfg->spk_vol = 18; }
void openfw_mmio_wfi(void) { advance(1); }
uint32_t millis(void) { return now_ms; }
void wdt_feed(void) { }
void mdelay(uint32_t ms) { advance(ms); }
int keys_read_rec_switch(void) { return strict && ended; }
int keys_read_hp_detect(void) { return 0; }
uint16_t keys_read_adc(void) { return 0; }
key_id_t keys_decode(uint16_t raw) { (void)raw; return key_script ? key_script(now_ms) : KEY_NONE; }
int keys_event_pending(void) { return stub_q_head != stub_q_tail; }
int keys_peek_event(key_event_t *out)
{
    if (stub_q_head == stub_q_tail) return 0;
    if (out) *out = stub_q[stub_q_tail & 31u];
    return 1;
}
int keys_pop_event(key_event_t *out)
{
    if (stub_q_head == stub_q_tail) return 0;
    if (out) *out = stub_q[stub_q_tail & 31u];
    stub_q_tail++;
    return 1;
}
key_id_t keys_flush_events(void) { stub_q_tail = stub_q_head; return stub_stable; }
key_id_t keys_state(uint32_t *since_ms)
{
    if (since_ms) *since_ms = stub_stable_since;
    return stub_stable;
}
void lcd_set_rotation(int rotation) { (void)rotation; }
void pmu_poll_charge_manage(void) { }
pmu_chg_state_t pmu_get_charge_state(void) { return PMU_CHG_DISCHARGING; }
uint32_t pmu_read_vbat_mv(void) { return 4000; }
void rtc_get(rtc_datetime_t *dt) { *dt = (rtc_datetime_t){0}; }

/* ---- player_keys_t gesture state machine ---------------------------------- */

static player_action_t feed(player_keys_t *k, key_id_t key, unsigned from, unsigned to,
                            player_action_t *acts, unsigned *n)
{
    player_action_t last = PLAYER_ACT_NONE;
    for (unsigned t = from; t < to; t++) {
        player_action_t a = player_keys_step(k, key, t);
        if (a != PLAYER_ACT_NONE) {
            acts[(*n)++] = a;
            last = a;
        }
    }
    return last;
}

static unsigned count_of(const player_action_t *acts, unsigned n, player_action_t a)
{
    unsigned c = 0;
    for (unsigned i = 0; i < n; i++) c += (acts[i] == a);
    return c;
}

static void test_key_gestures(void)
{
    player_keys_t k;
    player_action_t acts[64];
    unsigned n;

    /* Single RIGHT tap: one step as soon as the press is debounced */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 31, acts, &n);
    assert(n == 1 && acts[0] == PLAYER_ACT_SEEK_FWD);
    feed(&k, KEY_RIGHT, 31, 150, acts, &n);
    feed(&k, KEY_NONE, 150, 1000, acts, &n);
    assert(n == 1);

    /* Two quick taps: two steps, never a file change */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 100, acts, &n);
    feed(&k, KEY_NONE, 100, 250, acts, &n);
    feed(&k, KEY_RIGHT, 250, 350, acts, &n);
    feed(&k, KEY_NONE, 350, 2000, acts, &n);
    assert(n == 2 && count_of(acts, n, PLAYER_ACT_SEEK_FWD) == 2);

    /* RIGHT tap then quick LEFT tap: forward step, then back step, in order */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 100, acts, &n);
    feed(&k, KEY_NONE, 100, 200, acts, &n);
    feed(&k, KEY_LEFT, 200, 300, acts, &n);
    feed(&k, KEY_NONE, 300, 1000, acts, &n);
    assert(n == 2 && acts[0] == PLAYER_ACT_SEEK_FWD && acts[1] == PLAYER_ACT_SEEK_BACK);

    /* LEFT hold: step on press (30 ms), then every PLAYER_KEY_STEP_MS; release is silent */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_LEFT, 0, 30 + 3 * PLAYER_KEY_STEP_MS + 1, acts, &n);
    assert(n == 4 && count_of(acts, n, PLAYER_ACT_SEEK_BACK) == 4);
    feed(&k, KEY_NONE, 30 + 3 * PLAYER_KEY_STEP_MS + 1, 4000, acts, &n);
    assert(n == 4);

    /* VOL+ hold: one step on press, repeats after the delay, stops on release */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_VOL_UP, 0, 1030, acts, &n);
    /* press accepted at 30 ms, repeats from 430 ms every 100 ms up to 1029 ms */
    assert(n == 1 + 6 && count_of(acts, n, PLAYER_ACT_VOL_UP) == 7);
    feed(&k, KEY_NONE, 1030, 2000, acts, &n);
    assert(n == 7);

    /* VOL- quick taps: one step each */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    for (unsigned i = 0; i < 3; i++) {
        feed(&k, KEY_VOL_DOWN, i * 300, i * 300 + 150, acts, &n);
        feed(&k, KEY_NONE, i * 300 + 150, i * 300 + 300, acts, &n);
    }
    assert(n == 3);

    /* Debounce: a short glitch through another key's band does nothing */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_BACK, 0, 10, acts, &n);
    feed(&k, KEY_RIGHT, 10, 20, acts, &n);
    feed(&k, KEY_NONE, 20, 100, acts, &n);
    assert(n == 0);

    /* A key held when the player starts is ignored until released */
    player_keys_init(&k, KEY_RIGHT, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 2000, acts, &n);
    feed(&k, KEY_NONE, 2000, 3000, acts, &n);
    assert(n == 0);
    player_keys_init(&k, KEY_ENTER, 0); n = 0;
    feed(&k, KEY_ENTER, 0, 500, acts, &n);
    feed(&k, KEY_NONE, 500, 600, acts, &n);
    feed(&k, KEY_ENTER, 600, 700, acts, &n);
    assert(n == 1 && acts[0] == PLAYER_ACT_PLAY_PAUSE);

    /* Timer wrap-around: hold repeats across the 32-bit rollover */
    player_keys_init(&k, KEY_NONE, 0xFFFFFF00u); n = 0;
    for (unsigned t = 0; t < 1300; t++) {
        player_action_t a = player_keys_step(&k, KEY_RIGHT, 0xFFFFFF00u + t);
        if (a != PLAYER_ACT_NONE) acts[n++] = a;
    }
    assert(n == 3 && count_of(acts, n, PLAYER_ACT_SEEK_FWD) == 3);

    /* Direct edge+tick API: the press itself returns the step, on the same call */
    player_keys_init(&k, KEY_NONE, 0);
    assert(player_keys_edge(&k, KEY_RIGHT, 100u) == PLAYER_ACT_SEEK_FWD);
    assert(player_keys_tick(&k, 100u + PLAYER_KEY_STEP_MS - 1u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 100u + PLAYER_KEY_STEP_MS) == PLAYER_ACT_SEEK_FWD);
    assert(player_keys_edge(&k, KEY_NONE, 800u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 5000u) == PLAYER_ACT_NONE);
    /* A late tick repeats once and keeps the period from the tick */
    assert(player_keys_edge(&k, KEY_LEFT, 6000u) == PLAYER_ACT_SEEK_BACK);
    assert(player_keys_tick(&k, 9000u) == PLAYER_ACT_SEEK_BACK);
    assert(player_keys_tick(&k, 9001u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 9000u + PLAYER_KEY_STEP_MS) == PLAYER_ACT_SEEK_BACK);

    const key_id_t controls[] = { KEY_ENTER, KEY_BACK, KEY_VOL_UP, KEY_VOL_DOWN };
    const player_action_t actions[] = {
        PLAYER_ACT_PLAY_PAUSE, PLAYER_ACT_BACK, PLAYER_ACT_VOL_UP, PLAYER_ACT_VOL_DOWN
    };
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        player_keys_init(&k, KEY_NONE, 0);
        assert(player_keys_edge(&k, KEY_RIGHT, 20) == PLAYER_ACT_SEEK_FWD);
        assert(player_keys_edge(&k, KEY_NONE, 40) == PLAYER_ACT_NONE);
        assert(player_keys_edge(&k, controls[i], 60) == actions[i]);
        assert(player_keys_tick(&k, 61) == PLAYER_ACT_NONE);
        assert(player_keys_edge(&k, KEY_NONE, 80) == PLAYER_ACT_NONE);
        assert(player_keys_tick(&k, 2000) == PLAYER_ACT_NONE);
    }

    /* Direct edge+tick API: brief 20 ms VOL_UP tap -> one step, no repeat */
    player_keys_init(&k, KEY_NONE, 0);
    assert(player_keys_edge(&k, KEY_VOL_UP, 100u) == PLAYER_ACT_VOL_UP);
    assert(player_keys_edge(&k, KEY_NONE,   120u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 700u) == PLAYER_ACT_NONE);

    puts("[OK] player key gestures (LEFT/RIGHT act on press, repeat while held, VOL repeat, debounce, held-at-start, edge+tick)");
}

/* ---- playback loop with scripted keys --------------------------------------- */

static key_id_t tap_right(unsigned ms)
{
    return (ms >= 300 && ms < 400) ? KEY_RIGHT : KEY_NONE;
}
static key_id_t tap_left_then_back(unsigned ms)
{
    if (ms >= 300 && ms < 400) return KEY_LEFT;
    if (ms >= 2000 && ms < 2200) return KEY_BACK;
    return KEY_NONE;
}
static key_id_t hold_right_then_back(unsigned ms)
{
    if (ms >= 300 && ms < 1500) return KEY_RIGHT;
    if (ms >= 2000 && ms < 2200) return KEY_BACK;
    return KEY_NONE;
}
static key_id_t hold_vol_up(unsigned ms)
{
    if (ms >= 300 && ms < 1300) return KEY_VOL_UP;
    if (ms >= 1500 && ms < 1700) return KEY_BACK;
    return KEY_NONE;
}
static key_id_t brief_tap_during_render(unsigned ms)
{
    /* First ui_render_screen runs at t=1..46 ms (45 ms stall); a 20 ms VOL+ tap
     * at 10..30 ms starts and ends entirely inside that render stall. */
    if (ms >= 10 && ms < 30) return KEY_VOL_UP;
    if (ms >= 200 && ms < 220) return KEY_BACK;
    return KEY_NONE;
}

static key_id_t lost_release_then_back(unsigned ms)
{
    if (ms >= 50 && ms < 150) return KEY_VOL_UP;
    if (ms >= 1000 && ms < 2500) return KEY_BACK;
    return KEY_NONE;
}

static int screen_off_mid_playback(unsigned ms)
{
    return (ms < 100u || ms >= 1500u) ? 1 : 0;
}

static key_id_t keys_while_screen_off(unsigned ms)
{
    /* All buttons pressed while screen is off (100..1500 ms) must be no-ops. */
    if (ms >= 200 && ms < 300) return KEY_VOL_UP;
    if (ms >= 400 && ms < 500) return KEY_ENTER;
    if (ms >= 600 && ms < 700) return KEY_RIGHT;
    if (ms >= 800 && ms < 900) return KEY_BACK;
    /* Key held across the screen wake at 1500 ms must stay ignored until release. */
    if (ms >= 1400 && ms < 1700) return KEY_VOL_UP;
    /* Fresh BACK press after wake exits normally. */
    if (ms >= 1900 && ms < 2000) return KEY_BACK;
    return KEY_NONE;
}

static unsigned ended_at_ms;

static void reset_run(key_id_t (*script)(unsigned))
{
    now_ms = submitted = consumed = gaps = head = count = vol_calls = flush_calls = 0;
    heard_n = unaligned_reads_after_first = pcm_read_idx = ended_at_ms = 0;
    read_last_ms = 0;
    win_from = win_to = win_writes = win_reads = 0;
    before_win_l = 0x7FFF;
    max_scrub_speed = 0;
    ended = 0;
    strict = 0;
    track_pcm = 0;
    key_script = script;
    screen_awake_script = NULL;
    stub_keys_reset();
}

static key_id_t pause_resume_then_finish(unsigned ms)
{
    if (ended) {
        if (ended_at_ms == 0u) ended_at_ms = ms;
        return (ms < ended_at_ms + 40u) ? KEY_BACK : KEY_NONE;
    }
    if (ms >= 200 && ms < 240) return KEY_ENTER; /* pause while DAC queue has frames */
    if (ms >= 420 && ms < 460) return KEY_ENTER; /* resume */
    return KEY_NONE;
}

static key_id_t scrub_key;
static unsigned scrub_tap_at;
static key_id_t tap_then_ok(unsigned ms)
{
    if (ms >= scrub_tap_at && ms < scrub_tap_at + 60) return scrub_key;
    if (ms >= scrub_tap_at + 400 && ms < scrub_tap_at + 460) return KEY_ENTER;
    if (ms >= scrub_tap_at + 700 && ms < scrub_tap_at + 800) return KEY_BACK;
    return KEY_NONE;
}

/* Start of the last run of consecutive source frames in the heard stream. */
static unsigned final_run(int64_t (*frame_of)(unsigned))
{
    unsigned e = heard_n - 1;
    while (e > 0 && frame_of(e) == frame_of(e - 1) + 1) e--;
    return e;
}
static int64_t fixture_frame(unsigned i) { return heard_l[i]; }

/* The stereo fixture contains the source frame index in the left channel.
 * One RIGHT press: 2x. One LEFT press: -1x. OK returns to 1x. Neither way is
 * the DAC flushed: queued 1x audio plays out into the first grain, and the last
 * 1x grain hands over to normal playback sample-exactly. */
static void test_audible_scrub(void)
{
    exfat_vol_t vol = {0};
    static const int speeds[] = { 2, -1 };
    for (unsigned c = 0; c < 2; c++) {
        int speed = speeds[c];
        ui_model_t m = {0};
        m.cfg.spk_vol = 18;
        scrub_key = speed < 0 ? KEY_LEFT : KEY_RIGHT;
        scrub_tap_at = speed < 0 ? 500 : 40;
        reset_run(tap_then_ok);
        track_pcm = 1;
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        assert(flush_calls == 0 && max_scrub_speed == (speed < 0 ? -speed : speed));
        unsigned first = 0;
        while (first < heard_n && heard_l[first] == (int)first) first++;
        unsigned end = final_run(fixture_frame);
        assert(end > first + 1000 && heard_n > end + 1000);
        /* 1x audio queued at the press is heard first: about the 4-slot queue. */
        assert(heard_t[first] >= scrub_tap_at + 8 && heard_t[first] <= scrub_tap_at + 8 + 300);
        int direction = speed > 0 ? 1 : -1;
        unsigned native_steps = 0;
        for (unsigned i = first + 1; i < end; i++)
            if (heard_l[i] - heard_l[i - 1] == direction) native_steps++;
        assert(native_steps * 100u > (end - first) * 60u);
        if (speed > 0) assert(heard_l[end] > (int)end && heard_l[end] <= (int)(first + 2 * (end - first) + 600));
        else assert(heard_l[end] < (int)first + 600);
    }
    puts("[OK] 2x and -1x dial: no flush in or out, original-rate grains, contiguous 1x after OK");
}

/* Ladder stepping and the silent clock (pure functions). */
static void test_speed_dial_math(void)
{
    static const int ladder[] = { -32, -16, -8, -4, -2, -1, 1, 2, 4, 8, 16, 32 };
    for (unsigned i = 0; i < 12; i++) {
        assert(ui_speed_step(ladder[i], 1) == ladder[i < 11 ? i + 1 : 11]);
        assert(ui_speed_step(ladder[i], -1) == ladder[i > 0 ? i - 1 : 0]);
    }
    assert(ui_speed_step(1, 1) == 2 && ui_speed_step(1, -1) == -1 && ui_speed_step(-1, 1) == 1);
    assert(ui_speed_step(3, 1) == 4 && ui_speed_step(3, -1) == 2 && ui_speed_step(99, 1) == 32);

    uint32_t rem = 0;
    /* 1 ms ticks at 44.1 kHz carry the 0.1-frame remainder exactly, both ways. */
    int64_t pos = 1000;
    for (unsigned i = 0; i < 1000; i++) pos = player_scrub_advance(pos, 10000000u, 8, 44100, 1, &rem);
    assert(pos == 1000 + 8 * 44100 && rem == 0);
    for (unsigned i = 0; i < 1000; i++) pos = player_scrub_advance(pos, 10000000u, -8, 44100, 1, &rem);
    assert(pos == 1000);
    /* Clamp at both ends, from any distance, and stay clamped. */
    rem = 0;
    assert(player_scrub_advance(16000, 32000, 32, 16000, 1000, &rem) == 32000 && rem == 0);
    assert(player_scrub_advance(32000, 32000, 32, 16000, 7, &rem) == 32000);
    assert(player_scrub_advance(16000, 32000, -32, 16000, 1000, &rem) == 0 && rem == 0);
    assert(player_scrub_advance(0, 32000, -8, 16000, 3, &rem) == 0);
    assert(player_scrub_advance(5, 32000, -8, 16000, 0, &rem) == 5);
    /* A one-hour 16 kHz chapter at 32x: 1 s of real time is 32 s of audio. */
    rem = 0;
    assert(player_scrub_advance(0, 3600u * 16000u, 32, 16000, 1000, &rem) == 32 * 16000);
    puts("[OK] speed dial ladder (-32..-1, 1..32, clamped) and silent clock remainder/clamping");
}

/* ---- speed dial on a long synthetic file, against an ideal model ---------- */

typedef struct { key_id_t key; unsigned from, to; } press_t;
static const press_t *dial_presses;
static unsigned dial_npress;
static key_id_t dial_script(unsigned ms)
{
    for (unsigned i = 0; i < dial_npress; i++)
        if (ms >= dial_presses[i].from && ms < dial_presses[i].to) return dial_presses[i].key;
    return KEY_NONE;
}

enum { DIAL_RATE = 16000, DIAL_SECS = 120, DIAL_MS = 8000 };
static int64_t model_pos[DIAL_MS];
static int model_speed[DIAL_MS];

/* 1 ms model of the dial: a press acts 8 ms after it starts (debounce) and
 * repeats every PLAYER_KEY_STEP_MS while held. */
static void dial_model(unsigned start_s)
{
    int64_t pos = (int64_t)start_s * DIAL_RATE, total = (int64_t)DIAL_SECS * DIAL_RATE;
    int speed = 1, paused = 0, ended = 0;
    for (unsigned t = 0; t < DIAL_MS; t++) {
        for (unsigned i = 0; i < dial_npress; i++) {
            const press_t *p = &dial_presses[i];
            unsigned at = p->from + 8;
            int fire = t == at || (t > at && t < p->to + 8 && (t - at) % PLAYER_KEY_STEP_MS == 0);
            if (!fire || ended) continue;
            if (p->key == KEY_ENTER) {
                if (speed != 1) speed = 1, paused = 0;
                else if (t == at) paused ^= 1;
            } else if (p->key == KEY_LEFT || p->key == KEY_RIGHT) {
                speed = ui_speed_step(speed, p->key == KEY_RIGHT ? 1 : -1);
                paused = 0;
            }
        }
        if (!paused && !ended) {
            pos += speed * (DIAL_RATE / 1000);
            if (pos <= 0 && speed < 0) pos = 0, speed = 1, paused = 1;
            if (pos >= total) pos = total, speed = 1, ended = 1;
        }
        model_pos[t] = pos;
        model_speed[t] = paused ? 0 : speed;
    }
}

static int64_t synth_frame(unsigned i) { return (int64_t)heard_r[i] * 30000 + heard_l[i]; }

/* play_tick_cb probe for the dial: what position memory would see at each tick */
enum { DIAL_TICKS = 1024 };
static const ui_model_t *dial_tick_model;
static unsigned dial_ticks, dial_tick_ms[DIAL_TICKS];
static uint32_t dial_tick_pos[DIAL_TICKS];
static int8_t dial_tick_speed[DIAL_TICKS];
static uint8_t dial_tick_state[DIAL_TICKS];
static void dial_tick(uint32_t now)
{
    if (dial_ticks >= DIAL_TICKS) return;
    dial_tick_ms[dial_ticks] = now;
    dial_tick_pos[dial_ticks] = dial_tick_model->play_pos_ms;
    dial_tick_speed[dial_ticks] = dial_tick_model->play_scrub_speed;
    dial_tick_state[dial_ticks] = dial_tick_model->play_state;
    dial_ticks++;
}

static void test_speed_dial(void)
{
    exfat_vol_t vol = {0};
    synth_frames = DIAL_RATE * DIAL_SECS;
    file_size = 44 + synth_frames * 4;
    assert(!wav_build_header(recording, DIAL_RATE, 2, 16, synth_frames * 4));
    channels = 2;
    time_limit_ms = DIAL_MS;
    /* Hold RIGHT 40..1300 ms: 2x (48), 4x (648), 8x (1248). */
    static const press_t fwd8[] = { {KEY_RIGHT, 40, 1300}, {KEY_ENTER, 3000, 3060}, {KEY_BACK, 3600, 3700} };
    static const press_t rev8[] = { {KEY_LEFT, 40, 1900}, {KEY_ENTER, 3500, 3560}, {KEY_BACK, 4100, 4200} };
    static const press_t to_zero[] = { {KEY_LEFT, 40, 3500}, {KEY_BACK, 4500, 4600} }; /* held on past 0:00 */
    static const press_t to_end[] = { {KEY_RIGHT, 40, 3100}, {KEY_BACK, 4500, 4600} };
    static const press_t from_pause[] = { {KEY_ENTER, 100, 160}, {KEY_RIGHT, 400, 460},
                                          {KEY_ENTER, 1400, 1460}, {KEY_BACK, 2000, 2100} };
    static const press_t through_1x[] = { {KEY_RIGHT, 100, 160}, {KEY_LEFT, 700, 760}, {KEY_LEFT, 1300, 1360},
                                          {KEY_RIGHT, 1900, 1960}, {KEY_BACK, 2500, 2600} };
    static const press_t down_from_8[] = { {KEY_RIGHT, 40, 1300}, {KEY_LEFT, 2500, 2560},
                                           {KEY_ENTER, 3200, 3260}, {KEY_BACK, 3800, 3900} };
    static const struct { const char *name; const press_t *p; unsigned n, start_s; } cases[] = {
        { "fwd 2-4-8x, OK", fwd8, 3, 60 },
        { "rev -1..-8x, OK", rev8, 3, 60 },
        { "rev to 0:00", to_zero, 2, 10 },
        { "fwd to end", to_end, 2, 100 },
        { "pause, RIGHT, OK", from_pause, 4, 60 },
        { "2x, 1x, -1x, 1x", through_1x, 5, 60 },
        { "8x down to 4x", down_from_8, 4, 30 },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        ui_model_t m = {0};
        m.cfg.spk_vol = 18;
        m.play_file.resume_sec = cases[c].start_s;
        m.play_tick_cb = dial_tick;
        dial_tick_model = &m;
        dial_ticks = 0;
        dial_presses = cases[c].p;
        dial_npress = cases[c].n;
        dial_model(cases[c].start_s);
        reset_run(dial_script);
        track_pcm = 1;
        /* Silent stretch in the model: nothing may be read or written inside it. */
        unsigned s0 = 0, s1 = 0;
        for (unsigned t = 0; t < DIAL_MS; t++) {
            int sp = model_speed[t] < 0 ? -model_speed[t] : model_speed[t];
            if (sp >= UI_SPEED_SILENT && !s0) s0 = t;
            if (s0 && !s1 && sp < UI_SPEED_SILENT) s1 = t;
        }
        if (s0) {
            win_from = s0 + 150;
            win_to = s1 ? s1 - 30 : DIAL_MS;
        }
        unsigned w_from = win_from, w_to = win_to;
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        win_from = w_from; win_to = w_to;
        assert(flush_calls <= FLUSH_LOG);

        if (s0) {
            assert(win_writes == 0 && win_reads == 0);
            assert(before_win_l == 0);                    /* last grain faded to zero */
        }
        /* The final run of contiguous frames is 1x playback again; compare where it
         * started with the model at that moment. */
        unsigned last = flush_calls ? flush_calls - 1 : 0;
        unsigned at = heard_n ? final_run(synth_frame) : 0;
        int64_t want = model_pos[heard_t[at]];
        int max_sp = 1;
        for (unsigned t = 0; t < heard_t[at]; t++)
            if (abs(model_speed[t]) > max_sp) max_sp = abs(model_speed[t]);
        int64_t tol = 16 * (450 + 150 * (int64_t)max_sp); /* key latency, render stalls, queue drains */
        int64_t got;
        if (model_pos[DIAL_MS - 1] == 0) {
            want = 0;
            at = flush_at[last];
            assert(heard_n == at && last_play_state == 1 && last_pos_ms == 0);   /* paused at 0:00 */
            unsigned zero_ms = 0;
            while (model_pos[zero_ms]) zero_ms++;
            assert(flush_ms[last] < zero_ms + 300);   /* hold repeats past 0:00 do nothing */
            got = 0;
        } else if (model_pos[DIAL_MS - 1] == (int64_t)synth_frames) {
            want = synth_frames;
            at = flush_at[last];
            assert(heard_n == at && last_play_state == 2 && m.play_file.resume_sec == 0);  /* normal EOF */
            got = want;
        } else {
            assert(heard_n > at + 1000);
            got = synth_frame(at);
        }
        printf("    %-18s from %3us: 1x again at %8.3fs (model %8.3fs), top %dx\n", cases[c].name,
               cases[c].start_s, got / (double)DIAL_RATE, want / (double)DIAL_RATE, max_scrub_speed);
        assert(got >= want - tol && got <= want + tol);
        /* Whenever the dial reads 1x (also while grains hand back over to 1x),
         * the position the tick callback sees (and position memory would store)
         * is the frame being heard at that moment. */
        unsigned checked = 0;
        for (unsigned k = 0; k < dial_ticks; k++) {
            if (dial_tick_speed[k] != 0 || dial_tick_state[k] != 0) continue;
            unsigned i = 0;
            while (i + 1 < heard_n && heard_t[i + 1] <= dial_tick_ms[k]) i++;
            if (!heard_n || (unsigned)heard_t[i] + 2u < dial_tick_ms[k]) continue;   /* nothing audible right then */
            int64_t live = synth_frame(i), shown = (int64_t)dial_tick_pos[k] * 16;
            assert(shown >= live - 16 * 60 && shown <= live + 16 * 60);
            checked++;
        }
        if (model_pos[DIAL_MS - 1] != 0 && model_pos[DIAL_MS - 1] != (int64_t)synth_frames)
            assert(checked >= 3);   /* cases that end back at 1x, playing */
    }
    synth_frames = 0;
    time_limit_ms = 6000;
    win_from = win_to = 0;
    puts("[OK] speed dial: silent >= 8x (no SD reads or DAC writes, faded last grain), OK/step back to 1x "
         "at the heard frame, pause -> RIGHT, reverse to 0:00 pauses, forward to the end ends");
}

/* 200 Hz triangle (slope 400 per sample) through every kind of transition:
 * 1x -> 2x -> 4x -> 8x (silent) -> 4x -> OK -> -1x -> 1x -> 2x -> OK. No heard
 * sample may jump far beyond the signal's own slope. */
static void test_dial_clicks(void)
{
    static const press_t script[] = {
        {KEY_RIGHT, 300, 360}, {KEY_RIGHT, 900, 960}, {KEY_RIGHT, 1500, 1560}, {KEY_LEFT, 2500, 2560},
        {KEY_ENTER, 3000, 3060}, {KEY_LEFT, 3600, 3660}, {KEY_RIGHT, 4200, 4260}, {KEY_RIGHT, 4800, 4860},
        {KEY_ENTER, 5400, 5460}, {KEY_BACK, 6300, 6400},
    };
    exfat_vol_t vol = {0};
    synth_frames = 16000 * 120;
    synth_tri = 1;
    file_size = 44 + synth_frames * 4;
    assert(!wav_build_header(recording, 16000, 2, 16, synth_frames * 4));
    channels = 2;
    time_limit_ms = 8000;
    ui_model_t m = {0};
    m.cfg.spk_vol = 18;
    m.play_file.resume_sec = 60;
    dial_presses = script;
    dial_npress = sizeof(script) / sizeof(script[0]);
    reset_run(dial_script);
    track_pcm = 1;
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(flush_calls == 0 && max_scrub_speed == 8);
    int worst = 0;
    unsigned worst_at = 0;
    for (unsigned i = 1; i < heard_n; i++) {
        int d = abs(heard_l[i] - heard_l[i - 1]);
        if (d > worst) worst = d, worst_at = i;
        d = abs(heard_r[i] - heard_r[i - 1]);
        if (d > worst) worst = d, worst_at = i;
    }
    printf("    largest step %d (signal slope 400) at %u ms over %u heard frames\n", worst,
           (unsigned)heard_t[worst_at], heard_n);
    assert(worst <= 1600);
    synth_frames = 0;
    synth_tri = 0;
    time_limit_ms = 6000;
    puts("[OK] speed dial transitions are click-free (no flushes, crossfades and fades only)");
}

/* The dial's end of file is the normal one: with .autoplay a fast-forward into
 * the end continues with the next track; without it the player stays on End
 * even while RIGHT is still held. */
static void test_dial_autoplay(void)
{
    static const press_t hold_to_end[] = { {KEY_RIGHT, 40, 3500}, {KEY_BACK, 4500, 4600} };
    exfat_vol_t vol = {0};
    synth_frames = 16000 * 120;
    file_size = 44 + synth_frames * 4;
    assert(!wav_build_header(recording, 16000, 2, 16, synth_frames * 4));
    channels = 2;
    time_limit_ms = 8000;
    for (int autoplay = 1; autoplay >= 0; autoplay--) {
        ui_model_t m = {0};
        m.cfg.spk_vol = 18;
        m.play_file.resume_sec = 100;
        m.play_has_next = 1;
        m.play_autoplay = (uint8_t)autoplay;
        dial_presses = hold_to_end;
        dial_npress = 2;
        reset_run(dial_script);
        int rc = openfw_play_file_ui(&vol, "/record", "test.wav", &m);
        if (autoplay) {
            assert(rc == PLAYER_RC_AUTO_NEXT && now_ms < 3500 && m.play_file.resume_sec == 0);
        } else {
            assert(rc == 0 && last_play_state == 2 && now_ms >= 4500);   /* End screen, no file change */
        }
    }
    synth_frames = 0;
    time_limit_ms = 6000;
    puts("[OK] speed dial into the end: .autoplay continues with the next track, otherwise End (held RIGHT never changes file)");
}

static void put_le16_t(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* A 37-sample period doesn't divide the grain length. Check the frequency
 * actually heard between the two DMA flushes, including the splice regions. */
static void test_scrub_pitch(void)
{
    exfat_vol_t vol = {0};
    channels = 1;
    file_size = 44 + FRAMES * 2;
    assert(!wav_build_header(recording, 16000, 1, 16, FRAMES * 2));
    for (unsigned i = 0; i < FRAMES; i++) {
        int phase = (int)(i % 37);
        int16_t v = (int16_t)(phase < 19 ? phase * 1000 - 9000 : (37 - phase) * 1000 - 9000);
        put_le16_t(recording + 44 + 2*i, (uint16_t)v);
    }
    for (int dir = -1; dir <= 1; dir += 2) {
        ui_model_t m = {0};
        m.cfg.spk_vol = 18;
        scrub_key = dir > 0 ? KEY_RIGHT : KEY_LEFT;
        scrub_tap_at = dir > 0 ? 40 : 500;
        reset_run(tap_then_ok);
        track_pcm = 1;
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        /* Dial audio only: after the queued 1x audio, before OK's queued grains end. */
        unsigned from = 0, to;
        while (from < heard_n && heard_t[from] < scrub_tap_at + 300) from++;
        to = from;
        while (to < heard_n && heard_t[to] < scrub_tap_at + 480) to++;
        assert(to > from + 1000);
        unsigned crossings = 0, span = to - from;
        for (unsigned i = from + 1; i < to; i++)
            if (heard_l[i-1] <= 0 && heard_l[i] > 0) crossings++;
        assert(crossings * 37 + 38 >= span && crossings * 37 <= span + 38);
        printf("[OK] %s scrub: %u Hz (input 432 Hz)\n", dir > 0 ? "Forward" : "Reverse",
               (crossings * 16000u + span / 2u) / span);
    }
}

static void put_le32_t(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void test_pause_zero_loss_and_ima2048(void)
{
    static ui_model_t m;
    exfat_vol_t vol = {0};

    /* 1. Pause/Resume zero-sample-loss on 16-bit stereo PCM (already in recording[]) */
    channels = 2;
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    reset_run(pause_resume_then_finish);
    track_pcm = 1;
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(ended == 1);
    assert(flush_calls >= 1u);
    assert(heard_n == FRAMES);
    for (unsigned i = 0; i < FRAMES; i++) {
        assert(heard_l[i] == sample(i, 0));
    }

    /* 2. Seek flushes DAC queue immediately */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    reset_run(hold_right_then_back);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(flush_calls >= 1u);

    /* 3. IMA-ADPCM block_align=2048 (both mono 4089 spb & stereo 2041 spb) decodes cleanly */
    for (unsigned ch = 1; ch <= 2; ch++) {
        uint16_t ba  = 2048u;
        uint16_t spb = (ch == 1u) ? 4089u : 2041u;
        uint32_t nblk = 10u;
        uint32_t data_bytes = nblk * ba;
        memset(recording, 0, 60u + data_bytes);
        memcpy(&recording[0], "RIFF", 4u);
        put_le32_t(&recording[4], 52u + data_bytes);
        memcpy(&recording[8], "WAVEfmt ", 8u);
        put_le32_t(&recording[16], 20u);
        put_le16_t(&recording[20], WAV_FMT_IMA_ADPCM);
        put_le16_t(&recording[22], (uint16_t)ch);
        put_le32_t(&recording[24], 16000u);
        put_le32_t(&recording[28], 16000u * ba / spb);
        put_le16_t(&recording[32], ba);
        put_le16_t(&recording[34], 4u);
        put_le16_t(&recording[36], 2u);
        put_le16_t(&recording[38], spb);
        memcpy(&recording[40], "fact", 4u);
        put_le32_t(&recording[44], 4u);
        put_le32_t(&recording[48], nblk * spb);
        memcpy(&recording[52], "data", 4u);
        put_le32_t(&recording[56], data_bytes);
        for (uint32_t b = 0u; b < nblk; b++) {
            uint8_t *blk = &recording[60u + b * ba];
            put_le16_t(&blk[0], (uint16_t)1200);
            blk[2] = 10u;
            if (ch == 2u) {
                put_le16_t(&blk[4], (uint16_t)-1200);
                blk[6] = 10u;
            }
        }
        file_size = 60u + data_bytes;
        channels  = ch;
        memset(&m, 0, sizeof(m));
        m.cfg.spk_vol = 18;
        reset_run(pause_resume_then_finish);
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        assert(ended == 1);
        assert(consumed == nblk * spb);

        /* One second lands inside a block in both formats. */
        m.play_file.resume_sec = 1u;
        reset_run(pause_resume_then_finish);
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        assert(ended && consumed == nblk * spb - 16000u);
    }

    /* 4. Unaligned IMA-ADPCM (data_offset=60, block_align=256, spb=505, multi-read) keeps every read 512B aligned */
    {
        uint16_t ba  = 256u;
        uint16_t spb = 505u;
        uint32_t nblk = 20u; /* 5120 bytes -> 3 aligned multi-sector chunks of 2048/2048/1024 */
        uint32_t data_bytes = nblk * ba;
        memset(recording, 0, 60u + data_bytes);
        memcpy(&recording[0], "RIFF", 4u);
        put_le32_t(&recording[4], 52u + data_bytes);
        memcpy(&recording[8], "WAVEfmt ", 8u);
        put_le32_t(&recording[16], 20u);
        put_le16_t(&recording[20], WAV_FMT_IMA_ADPCM);
        put_le16_t(&recording[22], 1u);
        put_le32_t(&recording[24], 16000u);
        put_le32_t(&recording[28], 16000u * ba / spb);
        put_le16_t(&recording[32], ba);
        put_le16_t(&recording[34], 4u);
        put_le16_t(&recording[36], 2u);
        put_le16_t(&recording[38], spb);
        memcpy(&recording[40], "fact", 4u);
        put_le32_t(&recording[44], 4u);
        put_le32_t(&recording[48], nblk * spb);
        memcpy(&recording[52], "data", 4u);
        put_le32_t(&recording[56], data_bytes);
        file_size = 60u + data_bytes;
        channels  = 1u;
        memset(&m, 0, sizeof(m));
        m.cfg.spk_vol = 18;
        reset_run(pause_resume_then_finish);
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        assert(ended == 1 && consumed == nblk * spb);
        assert(unaligned_reads_after_first == 0u);
    }

    /* 5. WAV header with 1024-byte JUNK chunk before data (>512 B) parses & plays */
    uint32_t junk_len = 1024u;
    uint32_t pcm_frames = 2048u;
    uint32_t pcm_bytes = pcm_frames * 2u;
    memset(recording, 0, 44u + 8u + junk_len + pcm_bytes);
    memcpy(&recording[0], "RIFF", 4u);
    put_le32_t(&recording[4], 36u + 8u + junk_len + pcm_bytes);
    memcpy(&recording[8], "WAVEfmt ", 8u);
    put_le32_t(&recording[16], 16u);
    put_le16_t(&recording[20], WAV_FMT_PCM);
    put_le16_t(&recording[22], 1u);
    put_le32_t(&recording[24], 16000u);
    put_le32_t(&recording[28], 32000u);
    put_le16_t(&recording[32], 2u);
    put_le16_t(&recording[34], 16u);
    memcpy(&recording[36], "JUNK", 4u);
    put_le32_t(&recording[40], junk_len);
    uint32_t data_hdr = 44u + junk_len;
    memcpy(&recording[data_hdr], "data", 4u);
    put_le32_t(&recording[data_hdr + 4u], pcm_bytes);
    file_size = data_hdr + 8u + pcm_bytes;
    channels  = 1u;
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    reset_run(pause_resume_then_finish);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(ended == 1 && consumed == pcm_frames);

    puts("[OK] player pause/resume 0-loss, seek flush, IMA-ADPCM block_align=2048 (1ch/2ch), >512B JUNK header");
}

static void test_scripted_keys(void)
{
    static ui_model_t m;
    exfat_vol_t vol = {0};
    channels = 2; /* the main() loop leaves the stereo fixture in place */

    /* "Track" mode: one RIGHT press with a next file returns 1 right away */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    m.cfg.skip_mode = UI_SKIP_TRACK;
    m.play_has_next = 1;
    reset_run(tap_right);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 1);
    assert(now_ms < 400);

    /* "Speed" mode: the same press steps the dial instead and never changes file */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    m.play_has_next = 1;
    reset_run(hold_right_then_back);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(now_ms >= 2000);

    /* "Track" mode: LEFT on the first file: toast, stay, then BACK leaves */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    m.cfg.skip_mode = UI_SKIP_TRACK;
    reset_run(tap_left_then_back);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(now_ms >= 2000);
    assert(strcmp(m.toast_msg, "Start of list") == 0 || m.toast_msg[0] == '\0');

    /* Hold VOL+ for 1 s: several steps, capped below 31 */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    reset_run(hold_vol_up);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(m.cfg.spk_vol >= 22 && m.cfg.spk_vol <= 31);
    assert(vol_calls == (unsigned)(m.cfg.spk_vol - 18));

    /* Brief 20 ms VOL+ tap during a 45 ms render stall, then brief 20 ms BACK tap */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    reset_run(brief_tap_during_render);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(m.cfg.spk_vol == 19 && vol_calls == 1);
    assert(now_ms < 400);

    /* The same volume tap now lands entirely inside file lookup. */
    reset_run(brief_tap_during_render);
    m.cfg.spk_vol = 18;
    lookup_delay = 60;
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(m.cfg.spk_vol == 19 && vol_calls == 1);
    lookup_delay = 0;

    /* A lost release must stop repeats, and a held Back must return promptly. */
    reset_run(lost_release_then_back);
    drop_volume_release = 1;
    m.cfg.spk_vol = 18;
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(m.cfg.spk_vol == 19 && vol_calls == 1);
    assert(now_ms < 1200);
    drop_volume_release = 0;

    /* While screen is off, all non-power buttons (and keys held across wake) are no-ops. */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    m.play_has_next = 1;
    reset_run(keys_while_screen_off);
    screen_awake_script = screen_off_mid_playback;
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(m.cfg.spk_vol == 18 && vol_calls == 0);
    assert(flush_calls == 0);
    assert(now_ms >= 1900);
    screen_awake_script = NULL;

    key_script = NULL;
    stub_keys_reset();
    strict = 1;
    puts("[OK] player loop: Track RIGHT -> next file at once, Track LEFT at start -> stays, Speed RIGHT never changes file, hold VOL+ repeats, brief 20ms tap in render stall");
}

/* ---- per-folder .autoplay ------------------------------------------------- */

static key_id_t back_after_end(unsigned ms)
{
    if (ended) {
        if (ended_at_ms == 0u) ended_at_ms = ms;
        return (ms < ended_at_ms + 40u) ? KEY_BACK : KEY_NONE;
    }
    return KEY_NONE;
}

static void test_autoplay(void)
{
    /* Marker detection: one lookup of <dir>/.autoplay (exFAT matches it
     * case-insensitively, so .AUTOPLAY counts too); a folder named .autoplay does not */
    exfat_vol_t vol = {0};
    marker_path = "/audiobooks/Jekyll_Hyde/.AUTOPLAY";
    assert(player_dir_has_autoplay(&vol, "/audiobooks/Jekyll_Hyde") == 1);
    assert(strcmp(last_lookup, "/audiobooks/Jekyll_Hyde/.autoplay") == 0);
    assert(player_dir_has_autoplay(&vol, "/audiobooks/jekyll_hyde/") == 1);
    assert(player_dir_has_autoplay(&vol, "/audiobooks/Other") == 0);
    marker_is_dir = 1;
    assert(player_dir_has_autoplay(&vol, "/audiobooks/Jekyll_Hyde") == 0);
    marker_is_dir = 0;
    assert(player_dir_has_autoplay(&vol, "") == 0);
    assert(player_dir_has_autoplay(NULL, "/audiobooks/Jekyll_Hyde") == 0);
    marker_path = "/.autoplay";
    assert(player_dir_has_autoplay(&vol, "/") == 1);
    marker_path = NULL;

    /* Next-track selection in Files-list order: skips folder rows, never
     * leaves the folder, stops (-1) after the last track. */
    enum { N = 7, CAP = 48 };
    static ui_file_entry_t files[N];
    static char dirs[N][CAP];
    static const struct { const char *dir; const char *name; uint8_t is_dir; } rows[N] = {
        { "/audiobooks/JH/Extras", "Extras",          1 },
        { "/audiobooks/JH",        "JH_01a_Cap1.wav", 0 },
        { "/audiobooks/JH",        "JH_01b_Cap1.wav", 0 },
        { "/audiobooks/JH/Sub",    "Sub",             1 },
        { "/audiobooks/jh",        "JH_02a_Cap2.wav", 0 },
        { "/record",               "REC_0001.wav",    0 },
        { "/record",               "REC_0002.wav",    0 },
    };
    for (int i = 0; i < N; i++) {
        memset(&files[i], 0, sizeof(files[i]));
        snprintf(files[i].filename, sizeof(files[i].filename), "%s", rows[i].name);
        files[i].is_dir = rows[i].is_dir;
        snprintf(dirs[i], CAP, "%s", rows[i].dir);
    }
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, 1) == 2);
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, 2) == 4); /* skips folder row */
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, 4) == -1); /* next is /record */
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, 5) == 6);
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, 6) == -1); /* last: no wrap */
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, -1) == -1);
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, N, N) == -1);
    assert(player_autoplay_next_index(files, &dirs[0][0], CAP, 3, 2) == -1); /* end of list */

    /* Playback: natural end of track with autoplay returns PLAYER_RC_AUTO_NEXT
     * once the DAC queue has fully drained, with the resume point cleared. */
    static ui_model_t m;
    channels = 2; /* rebuild the plain 16 kHz stereo PCM fixture */
    {
        uint32_t bytes = FRAMES * channels * 2;
        file_size = 44 + bytes;
        assert(!wav_build_header(recording, 16000, channels, 16, bytes));
        for (unsigned i = 0; i < FRAMES; i++) {
            for (unsigned ch = 0; ch < channels; ch++) {
                uint16_t sv = (uint16_t)sample(i, ch);
                unsigned off = 44 + (i * channels + ch) * 2;
                recording[off] = (uint8_t)sv;
                recording[off + 1] = (uint8_t)(sv >> 8);
            }
        }
    }
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol      = 18;
    m.play_has_next    = 1;
    m.play_autoplay    = 1;
    reset_run(back_after_end);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == PLAYER_RC_AUTO_NEXT);
    assert(consumed == FRAMES && count == 0);
    assert(m.play_file.resume_sec == 0u);
    assert(ended == 0); /* never sat on the End screen */

    /* An SD error during playback must preserve the audible position. */
    synth_frames = 60u * 16000u;
    file_size = 44u + synth_frames * 4u;
    assert(!wav_build_header(recording, 16000, 2, 16, file_size - 44u));
    reset_run(NULL);
    m.play_file.resume_sec = 10u;
    fail_read_after_frames = 17000u;
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(!ended && consumed >= fail_read_after_frames);
    assert(m.play_file.resume_sec == 10u + consumed / 16000u);
    fail_read_after_frames = 0;
    synth_frames = 0;
    file_size = 44u + FRAMES * 4u;
    assert(!wav_build_header(recording, 16000, 2, 16, file_size - 44u));

    /* Autoplay folder but last track: stops at the end as before. The tick
     * callback (position memory) sees the live position and the End state. */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol      = 18;
    m.play_has_next    = 0;
    m.play_autoplay    = 1;
    m.play_tick_cb     = count_tick;
    tick_calls = tick_max_pos = tick_saw_end = 0;
    g_tick_model = &m;
    reset_run(back_after_end);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(ended == 1 && consumed == FRAMES);
    assert(tick_calls > 0 && tick_saw_end && tick_max_pos == FRAMES / 16000);

    /* No marker: a next file exists but the player waits on the End screen */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol      = 18;
    m.play_has_next    = 1;
    m.play_autoplay    = 0;
    reset_run(back_after_end);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(ended == 1 && consumed == FRAMES);

    key_script = NULL;
    stub_keys_reset();
    strict = 1;
    puts("[OK] .autoplay: marker lookup (case-insensitive, file only), same-folder next-track order, EOF auto-advance only with marker and next track");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_key_gestures();
    for (channels = 1; channels <= 2; channels++) {
        uint32_t bytes = FRAMES * channels * 2;
        file_size = 44 + bytes;
        assert(!wav_build_header(recording, 16000, channels, 16, bytes));
        for (unsigned i = 0; i < FRAMES; i++) {
            for (unsigned ch = 0; ch < channels; ch++) {
                uint16_t s = (uint16_t)sample(i, ch);
                unsigned off = 44 + (i * channels + ch) * 2;
                recording[off] = (uint8_t)s;
                recording[off+1] = (uint8_t)(s >> 8);
            }
        }
        now_ms = submitted = consumed = gaps = head = count = flush_calls = 0;
        unaligned_reads_after_first = pcm_read_idx = 0;
        ended = 0;
        exfat_vol_t vol = {0};
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", NULL) == 2);
        assert(consumed == FRAMES && gaps == 0);
        assert(unaligned_reads_after_first == 0u);
    }
    puts("[OK] 16 kHz mono and stereo playback, 512B aligned SD reads, blocking display, complete DMA drain");
    test_audible_scrub();
    test_speed_dial_math();
    test_scripted_keys(); /* uses the stereo fixture left by the loop above */
    test_pause_zero_loss_and_ima2048();
    test_scrub_pitch();
    test_autoplay();
    test_speed_dial();   /* replaces the fixture with a synthetic 2-minute file */
    test_dial_clicks();
    test_dial_autoplay();
    return 0;
}
