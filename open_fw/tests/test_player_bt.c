/* BT=1 player on the headset route (src/player.c with OPENFW_BT_BLUEDROID, the real src/audio_out.c and
 * bt_port/hp_sink.c; hp_tx's app-side calls are faked over the ring). Built by `make test-bt-hp`.
 *
 * Silicon 2026-11-25 (docs/BT_A2DP_HP_SESSION.md): a 16 kHz mono file played to the headset sent 5-17 packets per
 * second with no real PCM, while the bench tone (16 kHz mono through the same aout -> ring -> x3 resampler -> SBC
 * path) sent none. The player never told audio_out its file was mono, so the ring was stereo: half the depth
 * (4096 frames = 256 ms instead of 512 ms) for twice the BTC resampler work. The app task (priority 1, below every
 * Bluetooth task) is not scheduled for a few hundred ms at a time under that load; the ring must cover those gaps.
 *
 * Model: the BTC consumer pulls packets of 8 SBC frames x 128 output frames at 48 kHz in real time on a virtual
 * clock; the app task is descheduled for STALL_MS once a second (at a UI tick). Checks: no packet without real PCM
 * between the first and the last real one, every source frame consumed, the ring is mono for a mono file and fed
 * without expansion; a stereo file keeps a stereo ring; the DAC route still expands mono.
 * Output mode Both (Settings -> Audio output): mono and stereo files with the headset connected (both outputs get
 * every frame, sample exact; the speaker never runs ahead of the headset; one volume = bt_vol, the headset at unity
 * under absolute volume) and not connected (speaker only, speaker volume); with the app task out 400 ms/s the speaker
 * runs dry and must drop its backlog (audio_out.c pump_dac resync) instead of holding the headset's ring.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openfw.h"
#include "openfw_player.h"
#include "openfw_wav.h"
#include "openfw_i18n_bt.h"
#include "audio_out.h"
#include "hp_sink.h"
#include "hp_tx.h"
#include "bt_hp.h"

enum {
    RATE = 16000,
    FRAMES = 3 * RATE,          /* 3 s file */
    RING_BYTES = 16384,         /* bt_port/bt_hp_bluedroid.c HP_RING_BYTES */
    CONN_RATE = 48000,
    PKT_FRAMES = 8 * 128,       /* fpp 8 x 128 PCM per SBC frame (48 kHz, 16 blocks, 8 subbands) */
    STALL_MS = 400,             /* app task descheduled once a second (silicon: 300-480 ms loop lags) */
    MAX_PKTS = 4096,
};

static uint8_t recording[44 + FRAMES * 4];
static unsigned channels, file_size, now_ms, time_limit_ms;
static int ended, stall_on;
static aout_route_t seen_route;
static uint16_t seen_gain;
static unsigned last_stall_ms;

/* ---- headset side: real ring, fake hp_tx (app-side calls only), consumer clocked on the virtual time ---- */
static hp_sink_t sink;
static int16_t ring[RING_BYTES / 2];
static hp_tx_t fake_tx;
static uint32_t pkt_credit;                    /* output frames owed to the consumer, x1000 */
static uint8_t pkt_real[MAX_PKTS];
static unsigned pkts, src_consumed;
static int16_t pull_buf[PKT_FRAMES * 2];

uint32_t hp_tx_app_queued_frames(const hp_tx_t *t) { (void)t; return hp_sink_queued(&sink); }
int hp_tx_app_is_busy(const hp_tx_t *t) { (void)t; return hp_sink_queued(&sink) > 0u; }
void hp_tx_request_flush(hp_tx_t *t) { (void)t; hp_sink_flush(&sink); }
void hp_tx_request_drop(hp_tx_t *t) { (void)t; hp_sink_flush(&sink); }
static int bt_ready = 1;                     /* headset connected, stream OPEN / STARTED */
static int link_ready(void) { return bt_ready; }
static void link_start(void) { }
static const aout_bt_link_t k_link = { link_ready, link_start };

static void btc_run_ms(void)
{
    pkt_credit += CONN_RATE;                    /* 48 frames per ms, x1000 */
    while (pkt_credit >= PKT_FRAMES * 1000u) {
        pkt_credit -= PKT_FRAMES * 1000u;
        uint32_t real = 0;
        for (int f = 0; f < 8; f++) {
            uint32_t used = 0;
            (void)hp_sink_pull(&sink, pull_buf, 128, 2, &used);
            real += used;
        }
        src_consumed += real;
        assert(pkts < MAX_PKTS);
        pkt_real[pkts++] = real != 0u;
    }
}

/* Packets with no real PCM between the first and the last real one (hp_tx's pad_pkts mid-stream). */
static unsigned mid_pads(void)
{
    unsigned first = 0, last = 0, n = 0;
    while (first < pkts && !pkt_real[first]) first++;
    for (unsigned i = 0; i < pkts; i++) if (pkt_real[i]) last = i;
    for (unsigned i = first; i < last; i++) n += !pkt_real[i];
    return n;
}

/* ---- fake DAC (speaker route): 4 slots, mono expansion check ---- */
static unsigned dac_slots[AUDIO_DAC_QUEUE_SLOTS], dac_head, dac_count, dac_submitted, dac_consumed;
static unsigned dac_level, dac_starved_ms, dac_writes, max_skew, dac_next, dac_skipped;
static int dac_running;

static int16_t sample(unsigned frame, unsigned ch) { return (int16_t)((int)(frame % 30000) * (ch ? -1 : 1)); }

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        now_ms++;
        assert(now_ms < time_limit_ms);
        btc_run_ms();
        unsigned frames = RATE / 1000;
        if (dac_running && dac_next && dac_next < FRAMES && !dac_count) {
            dac_starved_ms++;                   /* the DAC ran dry mid-file (an audible gap on the speaker) */
        }
        while (frames && dac_count) {
            unsigned take = dac_slots[dac_head] < frames ? dac_slots[dac_head] : frames;
            dac_slots[dac_head] -= take;
            frames -= take;
            dac_consumed += take;
            if (!dac_slots[dac_head]) {
                dac_head = (dac_head + 1u) % AUDIO_DAC_QUEUE_SLOTS;
                dac_count--;
            }
        }
        if (dac_consumed && src_consumed && dac_next < FRAMES) {
            /* BOTH: the speaker must not run ahead of what the headset already took from the ring */
            unsigned skew = dac_consumed > src_consumed ? dac_consumed - src_consumed : 0u;
            if (skew > max_skew) max_skew = skew;
        }
    }
}

int audio_dac_start(uint32_t rate, uint8_t level) { assert(rate == RATE); dac_level = level; dac_running = 1; return 0; }
int audio_dac_write_stereo(const int16_t *pcm, uint32_t frames)
{
    if (!dac_running || dac_count == AUDIO_DAC_QUEUE_SLOTS) return 0;
    if (frames > 1024u) frames = 1024u;
    /* BOTH may drop a backlog after the DAC ran dry (audio_out.c pump_dac): a forward jump, counted */
    unsigned at = dac_next;
    while (pcm[0] != sample(at, 0)) {
        assert(at < FRAMES);
        at++;
    }
    dac_skipped += at - dac_next;
    for (unsigned i = 0; i < frames; i++) {
        assert(pcm[2 * i] == sample(at + i, 0));
        assert(pcm[2 * i + 1] == sample(at + i, channels - 1));
    }
    dac_next = at + frames;
    dac_writes++;
    dac_slots[(dac_head + dac_count) % AUDIO_DAC_QUEUE_SLOTS] = frames;
    dac_count++;
    dac_submitted += frames;
    return (int)frames;
}
int audio_dac_is_busy(void) { return dac_count != 0; }
void audio_dac_flush(void) { dac_count = 0; }
uint32_t audio_dac_queued_frames(void)
{
    uint32_t q = 0;
    for (unsigned i = 0; i < dac_count; i++) q += dac_slots[(dac_head + i) % AUDIO_DAC_QUEUE_SLOTS];
    return q;
}
void audio_dac_set_mute(int m) { (void)m; }
void audio_dac_set_volume(uint8_t v, uint16_t d) { (void)d; dac_level = v; }
void audio_dac_stop(void) { dac_running = 0; dac_count = 0; }
void audio_pa_set(int on) { (void)on; }

/* ---- headphones session: always ready, no keys or events ---- */
void bt_hp_poll(void) { }
int bt_hp_stop(void) { return 0; }
int bt_hp_active(void) { return 1; }
int bt_hp_stream_ready(void) { return bt_ready; }
int bt_hp_take_key(void) { return BT_HP_KEY_NONE; }
uint32_t bt_hp_take_player_events(void) { return 0u; }
void bt_hp_player_begin(void) { }
static uint8_t bt_vol31 = 18u;
static int abs_vol;
uint8_t bt_hp_vol31(void) { return bt_vol31; }
void bt_hp_set_vol31(uint8_t v) { bt_vol31 = v; }
int bt_hp_abs_vol(void) { return abs_vol; }
int bt_lowbatt_poll(uint32_t now) { (void)now; return 0; }
const char *ui_tr_bt(ui_bt_str_id_t id) { (void)id; return ""; }

/* ---- file, UI, keys, board ---- */
int exfat_find_path(const exfat_vol_t *vol, const char *path, exfat_entry_t *ent)
{
    (void)vol;
    if (strcmp(path, "/record/test.wav")) return -1;
    *ent = (exfat_entry_t){ .size = file_size };
    return 0;
}
int exfat_read_file(const exfat_vol_t *vol, const exfat_entry_t *ent, uint64_t offset, void *dst, uint32_t len,
                    uint32_t *got)
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
    seen_route = aout_route();
    seen_gain = sink.gain_q15;
    advance(2);
    if (stall_on && now_ms - last_stall_ms >= 1000u) {
        last_stall_ms = now_ms;
        advance(STALL_MS);   /* the app task is not scheduled: only the BTC consumer runs */
    }
}
void ui_dispatch_tick(void) { }
int  ui_dispatch_is_animating(void) { return 0; }
void ui_settings_defaults(ui_settings_t *cfg) { cfg->spk_vol = 18; }
void openfw_mmio_wfi(void) { advance(1); }
uint32_t millis(void) { return now_ms; }
void wdt_feed(void) { }
void mdelay(uint32_t ms) { advance(ms); }
int keys_read_rec_switch(void) { return ended; }
int keys_read_hp_detect(void) { return 0; }
uint16_t keys_read_adc(void) { return 0; }
key_id_t keys_decode(uint16_t raw) { (void)raw; return KEY_NONE; }
int keys_event_pending(void) { return 0; }
int keys_peek_event(key_event_t *out) { (void)out; return 0; }
int keys_pop_event(key_event_t *out) { (void)out; return 0; }
key_id_t keys_flush_events(void) { return KEY_NONE; }
key_id_t keys_state(uint32_t *since_ms) { if (since_ms) *since_ms = 0; return KEY_NONE; }
void lcd_set_rotation(int rotation) { (void)rotation; }
void pmu_poll_charge_manage(void) { }
pmu_chg_state_t pmu_get_charge_state(void) { return PMU_CHG_DISCHARGING; }
uint32_t pmu_read_vbat_mv(void) { return 4000; }
void rtc_get(rtc_datetime_t *dt) { *dt = (rtc_datetime_t){0}; }

static void make_file(unsigned ch)
{
    uint32_t bytes = FRAMES * ch * 2u;
    channels = ch;
    file_size = 44u + bytes;
    assert(!wav_build_header(recording, RATE, (uint16_t)ch, 16, bytes));
    for (unsigned i = 0; i < FRAMES; i++) {
        for (unsigned c = 0; c < ch; c++) {
            uint16_t s = (uint16_t)sample(i, c);
            unsigned off = 44u + (i * ch + c) * 2u;
            recording[off] = (uint8_t)s;
            recording[off + 1] = (uint8_t)(s >> 8);
        }
    }
}

static void play(aout_mode_t mode, unsigned ch, int stall)
{
    static ui_model_t m;
    exfat_vol_t vol = {0};
    make_file(ch);
    memset(&m, 0, sizeof(m));
    m.cfg.spk_vol = 10;                  /* != the headset's bt_vol (18 by default) */
    now_ms = 0;
    time_limit_ms = 60000;
    ended = 0;
    stall_on = stall;
    last_stall_ms = 0;
    pkts = src_consumed = pkt_credit = 0;
    dac_head = dac_count = dac_submitted = dac_consumed = 0;
    dac_level = dac_starved_ms = dac_writes = max_skew = dac_next = dac_skipped = 0;
    hp_sink_init(&sink, ring, sizeof(ring));
    hp_sink_set_out_rate(&sink, CONN_RATE);
    aout_bt_attach(&fake_tx, &sink, &k_link, 0);
    aout_set_mode(mode);
    assert(openfw_play_file_ui(&vol, "/record", "test.wav", &m) == 2);
    assert(ended);
    advance(50);   /* the encoder takes the last chunk */
    aout_set_src_channels(2u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 16 kHz mono to the headset, app descheduled 400 ms every second: a mono ring (512 ms) covers it */
    play(AOUT_MODE_HEADPHONES, 1, 1);
    printf("mono/headset: ring ch=%u frames=%u, packets=%u, mid-stream pads=%u, src=%u/%u\n",
           (unsigned)hp_sink_channels(&sink), (unsigned)sink.cap, pkts, mid_pads(), src_consumed, (unsigned)FRAMES);
    assert(mid_pads() == 0u);                /* without the pass-through: 12 (stereo ring, 256 ms) */
    assert(src_consumed == FRAMES);
    assert(hp_sink_channels(&sink) == 1u);   /* mono pass-through: the ring keeps the source channel count */
    assert(sink.ch_mismatch == 0u);
    puts("[OK] 16 kHz mono file to the headset: mono ring, no starved packet with the app task out 400 ms/s");

    /* stereo file: stereo ring, plays complete (no stall: 256 ms of 16 kHz stereo) */
    play(AOUT_MODE_HEADPHONES, 2, 0);
    assert(hp_sink_channels(&sink) == 2u);
    assert(src_consumed == FRAMES && mid_pads() == 0u);
    puts("[OK] 16 kHz stereo file to the headset: stereo ring, complete");

    /* speaker route on a BT=1 image: the player still expands mono for the DAC */
    play(AOUT_MODE_SPEAKER, 1, 0);
    assert(dac_submitted == FRAMES && src_consumed == 0u);
    puts("[OK] 16 kHz mono file to the speaker on BT=1: expanded to stereo for the DAC");

    /* ---- output mode Both (Settings -> Audio output -> Both): speaker / jack and headset together ---- */
    for (unsigned ch = 1; ch <= 2; ch++) {
        /* headset connected: one file reaches both outputs complete, sample exact (the fake DAC checks every
         * sample, so a mono ring's tap must expand to L = R), the headset never starves, and the speaker never
         * runs ahead of the headset's ring reader */
        play(AOUT_MODE_BOTH, ch, 0);
        printf("both/%s: route=%d ring ch=%u, headset src=%u pads=%u, DAC=%u in %u writes, DAC dry %u ms, "
               "DAC ahead max %u frames, DAC level %u\n", ch == 1 ? "mono" : "stereo", (int)seen_route,
               (unsigned)hp_sink_channels(&sink), src_consumed, mid_pads(), dac_submitted, dac_writes,
               dac_starved_ms, max_skew, dac_level);
        assert(seen_route == AOUT_ROUTE_BOTH);
        assert(hp_sink_channels(&sink) == ch && sink.ch_mismatch == 0u);
        assert(src_consumed == FRAMES && mid_pads() == 0u);
        assert(dac_submitted == FRAMES && dac_consumed == FRAMES && dac_skipped == 0u);
        assert(max_skew == 0u);
        assert(dac_level == 18u);           /* one volume for both outputs: the headset's bt_vol */
        printf("[OK] Both, %s file, headset connected: speaker and headset each get every frame\n",
               ch == 1 ? "mono" : "stereo");

        /* headset not connected: the file plays on the speaker alone, at the speaker volume */
        bt_ready = 0;
        play(AOUT_MODE_BOTH, ch, 0);
        bt_ready = 1;
        assert(seen_route == AOUT_ROUTE_DAC);
        assert(dac_submitted == FRAMES && src_consumed == 0u);
        assert(dac_level == 10u);
        printf("[OK] Both, %s file, no headset: speaker only, speaker volume\n", ch == 1 ? "mono" : "stereo");
    }

    /* Both with absolute volume: the headset's PCM gain stays at unity (its own volume sets its level), the speaker
     * follows bt_vol (never taken to 31 by the unity gain) */
    abs_vol = 1;
    bt_vol31 = 12u;
    play(AOUT_MODE_BOTH, 1, 0);
    assert(seen_route == AOUT_ROUTE_BOTH && seen_gain == 32768u && dac_level == 12u);
    abs_vol = 0;
    play(AOUT_MODE_BOTH, 1, 0);
    assert(seen_route == AOUT_ROUTE_BOTH && seen_gain == 990u && dac_level == 12u);   /* level 12 Q15 */
    bt_vol31 = 18u;
    puts("[OK] Both volume: absolute volume -> headset gain unity, speaker at bt_vol; else both at bt_vol");

    /* Both, mono file, app task descheduled 400 ms/s: the headset keeps its 512 ms mono ring */
    play(AOUT_MODE_BOTH, 1, 1);
    aout_stats_t st;
    aout_get_stats(&st);
    printf("both/mono stall: headset pads=%u src=%u, DAC=%u + %u dropped (resync %u), dry %u ms\n", mid_pads(),
           src_consumed, dac_submitted, dac_skipped, (unsigned)st.dac_resync_frames, dac_starved_ms);
    assert(hp_sink_channels(&sink) == 1u && src_consumed == FRAMES);
    assert(mid_pads() == 0u);                /* without the DAC resync: 23 (the tap's backlog holds the ring) */
    assert(dac_submitted + dac_skipped == FRAMES && dac_skipped == st.dac_resync_frames);
    puts("[OK] Both, mono file, app task out 400 ms/s: the headset does not starve; the speaker drops its backlog");
    return 0;
}
