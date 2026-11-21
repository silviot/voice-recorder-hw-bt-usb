/* Exercise the production playback loop with a 16 kHz DMA clock and a blocking LCD. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "openfw.h"
#include "openfw_player.h"
#include "openfw_wav.h"

enum { FRAMES = 16017, SLOT_FRAMES = 1024 };
static uint8_t recording[44 + FRAMES * 4];
static unsigned channels, file_size, now_ms, submitted, consumed, gaps;
static unsigned slots[2], head, count;
static int ended;
static int strict = 1;           /* full-drain assertions (plain playback runs) */
static key_id_t (*key_script)(unsigned ms);
static unsigned vol_calls;

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
        slots[head] -= take;
        frames -= take;
        consumed += take;
        if (!slots[head]) {
            head ^= 1;
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
    if (count == 2) return 0;
    if (frames > SLOT_FRAMES) frames = SLOT_FRAMES;
    for (unsigned i = 0; strict && i < frames; i++) { /* in-order samples */
        assert(pcm[2*i] == sample(submitted+i, 0));
        assert(pcm[2*i+1] == sample(submitted+i, channels-1));
    }
    submitted += frames;
    slots[(head + count) % 2] = frames;
    count++;
    return (int)frames;
}
int audio_dac_is_busy(void) { return count != 0; }
void audio_pa_set(int on) { if (!on && strict) assert(consumed == FRAMES); }
void audio_dac_stop(void) { if (strict) assert(!count); count = 0; }
void audio_dac_set_volume(uint8_t level, uint16_t digital) { (void)level; (void)digital; vol_calls++; }
void audio_dac_set_mute(int mute) { (void)mute; }

int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *ent)
{
    (void)vol;
    if (strcmp(path, "/record/test.wav")) return -1;
    *ent = (exfat_entry_t){ .size = file_size };
    return 0;
}
int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *ent,
                    uint64_t offset, void *dst, uint32_t len, uint32_t *got)
{
    (void)vol;
    assert(offset + len <= ent->size);
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
    advance(45);
}
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
void keys_flush_events(void) { stub_q_tail = stub_q_head; }
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

    /* RIGHT hold: seek steps while held (first at 530 ms, then every 250 ms) */
    player_keys_init(&k, KEY_NONE, 0); n = 0;
    feed(&k, KEY_RIGHT, 0, 1300, acts, &n);
    assert(n == 4 && count_of(acts, n, PLAYER_ACT_SCRUB_FWD) == 4);
    feed(&k, KEY_NONE, 1300, 3000, acts, &n);
    assert(n == 4);                              /* release is not a tap */

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

static void reset_run(key_id_t (*script)(unsigned))
{
    now_ms = submitted = consumed = gaps = head = count = vol_calls = 0;
    ended = 0;
    strict = 0;
    key_script = script;
    stub_keys_reset();
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
        now_ms = submitted = consumed = gaps = head = count = 0;
        ended = 0;
        exfat_vol_t vol = {0};
        assert(openfw_play_file_ui(&vol, "/record", "test.wav", NULL) == 2);
        assert(consumed == FRAMES && gaps == 0);
    }
    puts("[OK] 16 kHz mono and stereo playback, blocking display, complete DMA drain");
    test_scripted_keys(); /* uses the stereo fixture left by the loop above */
    return 0;
}
