/* Exercise the production playback loop with a 16 kHz DMA clock and a blocking LCD. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
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
static int16_t heard_l[FRAMES + 2048];
static unsigned heard_n;
static unsigned flush_at[2];
static int16_t slot_l[AUDIO_DAC_QUEUE_SLOTS][SLOT_FRAMES];
static unsigned slot_off[AUDIO_DAC_QUEUE_SLOTS];
static unsigned unaligned_reads_after_first;
static unsigned pcm_read_idx;
static key_id_t (*key_script)(unsigned ms);
static unsigned vol_calls;
static int drop_volume_release;
static unsigned lookup_delay;

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
        assert(now_ms < 6000);
        stub_keys_tick_1ms(now_ms);
    }
    unsigned frames = 16 * ms;
    while (frames && count) {
        unsigned take = slots[head] < frames ? slots[head] : frames;
        if (track_pcm) {
            for (unsigned i = 0; i < take && heard_n < (FRAMES + 2048); i++) {
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
        }
        slot_off[idx] = 0;
    }
    submitted += frames;
    slots[idx] = frames;
    count++;
    return (int)frames;
}
int audio_dac_is_busy(void) { return count != 0; }
void audio_dac_flush(void)
{
    if (flush_calls < 2u) flush_at[flush_calls] = heard_n;
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

int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *ent)
{
    (void)vol;
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
    if (offset > 0u) {
        if (pcm_read_idx > 0u && (offset & 511u) != 0u) {
            unaligned_reads_after_first++;
        }
        pcm_read_idx++;
    }
    memcpy(dst, recording + offset, len);
    *got = len;
    advance(1);
    return 0;
}
int ui_poll_power_button(ui_screen_id_t screen, ui_model_t *m, uint32_t now)
{
    (void)screen; (void)m; (void)now;
    return 1;
}

void ui_render_screen(ui_screen_id_t screen, const ui_model_t *m)
{
    assert(screen == UI_SCR_PLAYER);
    ended = m->play_state == 2;
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

    /* Single RIGHT tap: one seek step, only after the double-tap window */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 150, acts, &n);
    feed(&k, KEY_NONE, 150, 400, acts, &n);
    assert(n == 0);                              /* still waiting for a 2nd tap */
    feed(&k, KEY_NONE, 400, 1000, acts, &n);
    assert(n == 1 && acts[0] == PLAYER_ACT_SEEK_FWD);

    /* Double RIGHT tap: next file at the second press, and no seek */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 100, acts, &n);
    feed(&k, KEY_NONE, 100, 250, acts, &n);
    feed(&k, KEY_RIGHT, 250, 350, acts, &n);
    assert(n == 1 && acts[0] == PLAYER_ACT_NEXT_FILE);
    feed(&k, KEY_NONE, 350, 2000, acts, &n);
    assert(n == 1);

    /* Double LEFT tap: previous file; holding the second tap does nothing more */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_LEFT, 0, 100, acts, &n);
    feed(&k, KEY_NONE, 100, 250, acts, &n);
    feed(&k, KEY_LEFT, 250, 2000, acts, &n);
    feed(&k, KEY_NONE, 2000, 3000, acts, &n);
    assert(n == 1 && acts[0] == PLAYER_ACT_PREV_FILE);

    /* Two slow taps: two seek steps, no file skip */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 100, acts, &n);
    feed(&k, KEY_NONE, 100, 700, acts, &n);
    feed(&k, KEY_RIGHT, 700, 800, acts, &n);
    feed(&k, KEY_NONE, 800, 1500, acts, &n);
    assert(n == 2 && count_of(acts, n, PLAYER_ACT_SEEK_FWD) == 2);

    /* RIGHT tap then quick LEFT tap: forward step, then back step */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 100, acts, &n);
    feed(&k, KEY_NONE, 100, 200, acts, &n);
    feed(&k, KEY_LEFT, 200, 300, acts, &n);
    feed(&k, KEY_NONE, 300, 1000, acts, &n);
    assert(n == 2 && acts[0] == PLAYER_ACT_SEEK_FWD && acts[1] == PLAYER_ACT_SEEK_BACK);

    /* RIGHT hold: one continuous scrub gesture */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 1300, acts, &n);
    assert(n == 1 && count_of(acts, n, PLAYER_ACT_SCRUB_FWD) == 1);
    feed(&k, KEY_NONE, 1300, 3000, acts, &n);
    assert(n == 1);                              /* release is not a tap */

    /* LEFT hold: seek back */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_LEFT, 0, 600, acts, &n);
    assert(n == 1 && acts[0] == PLAYER_ACT_SCRUB_BACK);

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
    feed(&k, KEY_NONE, 10, 100, acts, &n);
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

    /* Timer wrap-around: double tap across the 32-bit rollover */
    player_keys_init(&k, KEY_NONE, 0xFFFFFF00u); n = 0;
    for (unsigned t = 0; t < 1000; t++) {
        key_id_t key = (t < 100 || (t >= 250 && t < 350)) ? KEY_RIGHT : KEY_NONE;
        player_action_t a = player_keys_step(&k, key, 0xFFFFFF00u + t);
        if (a != PLAYER_ACT_NONE) acts[n++] = a;
    }
    assert(n == 1 && acts[0] == PLAYER_ACT_NEXT_FILE);

    /* Direct edge+tick API: press/release with no tick in between -> SEEK after 350 ms */
    player_keys_init(&k, KEY_NONE, 0);
    assert(player_keys_edge(&k, KEY_RIGHT, 100u) == PLAYER_ACT_NONE);
    assert(player_keys_edge(&k, KEY_NONE,  120u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 469u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 470u) == PLAYER_ACT_SEEK_FWD);
    assert(player_keys_tick(&k, 900u) == PLAYER_ACT_NONE);

    const key_id_t controls[] = { KEY_ENTER, KEY_BACK, KEY_VOL_UP, KEY_VOL_DOWN };
    const player_action_t actions[] = {
        PLAYER_ACT_PLAY_PAUSE, PLAYER_ACT_BACK, PLAYER_ACT_VOL_UP, PLAYER_ACT_VOL_DOWN
    };
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        player_keys_init(&k, KEY_NONE, 0);
        assert(player_keys_edge(&k, KEY_RIGHT, 20) == PLAYER_ACT_NONE);
        assert(player_keys_edge(&k, KEY_NONE, 40) == PLAYER_ACT_NONE);
        assert(player_keys_edge(&k, controls[i], 60) == actions[i]);
        assert(player_keys_tick(&k, 61) == PLAYER_ACT_SEEK_FWD);
        assert(player_keys_edge(&k, KEY_NONE, 80) == PLAYER_ACT_NONE);
        assert(player_keys_tick(&k, 500) == PLAYER_ACT_NONE);
    }

    /* Direct edge+tick API: brief 20 ms VOL_UP tap -> one step, no repeat */
    player_keys_init(&k, KEY_NONE, 0);
    assert(player_keys_edge(&k, KEY_VOL_UP, 100u) == PLAYER_ACT_VOL_UP);
    assert(player_keys_edge(&k, KEY_NONE,   120u) == PLAYER_ACT_NONE);
    assert(player_keys_tick(&k, 700u) == PLAYER_ACT_NONE);

    puts("[OK] player key gestures (double tap file, tap/hold seek, VOL repeat, debounce, held-at-start, edge+tick)");
}

/* ---- playback loop with scripted keys --------------------------------------- */

static key_id_t double_right(unsigned ms)
{
    return ((ms >= 300 && ms < 400) || (ms >= 500 && ms < 600)) ? KEY_RIGHT : KEY_NONE;
}
static key_id_t double_left_then_back(unsigned ms)
{
    if ((ms >= 300 && ms < 400) || (ms >= 500 && ms < 600)) return KEY_LEFT;
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

static unsigned ended_at_ms;

static void reset_run(key_id_t (*script)(unsigned))
{
    now_ms = submitted = consumed = gaps = head = count = vol_calls = flush_calls = 0;
    heard_n = unaligned_reads_after_first = pcm_read_idx = ended_at_ms = 0;
    ended = 0;
    strict = 0;
    track_pcm = 0;
    key_script = script;
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
static key_id_t scrub_then_release(unsigned ms)
{
    if (ms >= 40 && ms < 680) return scrub_key;
    if (ms >= 950 && ms < 1050) return KEY_BACK;
    return KEY_NONE;
}

/* The stereo fixture contains the source frame index in the left channel.
 * Inspect the samples actually drained from the simulated DAC across release. */
static void test_audible_scrub(void)
{
    exfat_vol_t vol = {0};
    for (int speed = -2; speed <= 2; speed += 4) {
        ui_model_t m = {0};
        m.cfg.spk_vol = 18;
        scrub_key = speed < 0 ? KEY_LEFT : KEY_RIGHT;
        reset_run(scrub_then_release);
        track_pcm = 1;
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        assert(flush_calls == 2);
        unsigned first = flush_at[0], end = flush_at[1];
        assert(end > first + 1000 && heard_n > end + 100);
        int direction = speed > 0 ? 1 : -1;
        unsigned native_steps = 0;
        for (unsigned i = first + 1; i < end; i++)
            if (heard_l[i] - heard_l[i - 1] == direction) native_steps++;
        assert(native_steps * 100u > (end - first) * 65u);
        assert(heard_l[end] == (int)first + speed * (int)(end - first));
        for (unsigned i = end + 1; i < heard_n; i++) assert(heard_l[i] == heard_l[i-1] + 1);
    }
    puts("[OK] +/-2x scrub progress, original-rate grains, and accurate release");
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
        reset_run(scrub_then_release);
        track_pcm = 1;
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
        assert(flush_calls == 2 && flush_at[1] > flush_at[0] + 1000);
        unsigned crossings = 0, span = flush_at[1] - flush_at[0];
        for (unsigned i = flush_at[0]+1; i < flush_at[1]; i++)
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
        uint32_t nblk = 2u;
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
    }

    /* 4. WAV header with 1024-byte JUNK chunk before data (>512 B) parses & plays */
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

    /* Double tap RIGHT with a next file: player returns 1 */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    m.play_has_next = 1;
    reset_run(double_right);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 1);
    assert(now_ms < 800);

    /* Double tap LEFT on the first file: toast, stay, then BACK leaves */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    reset_run(double_left_then_back);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(now_ms >= 2000);
    assert(strcmp(m.toast_msg, "Start of list") == 0 || m.toast_msg[0] == '\0');

    /* Hold RIGHT seeks, it never changes file even when a next one exists */
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 18;
    m.play_has_next = 1;
    reset_run(hold_right_then_back);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 0);
    assert(now_ms >= 2000);

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

    key_script = NULL;
    stub_keys_reset();
    strict = 1;
    puts("[OK] player loop: double tap RIGHT -> next file, double tap LEFT at start -> stays, hold RIGHT seeks, hold VOL+ repeats, brief 20ms tap in render stall");
}

int main(void)
{
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
    test_scripted_keys(); /* uses the stereo fixture left by the loop above */
    test_pause_zero_loss_and_ima2048();
    test_scrub_pitch();
    return 0;
}
