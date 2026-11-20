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

static int16_t sample(unsigned frame, unsigned channel)
{
    return (int16_t)((int)(frame % 30000) * (channel ? -1 : 1));
}

static void advance(unsigned ms)
{
    now_ms += ms;
    assert(now_ms < 3000);
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
    for (unsigned i = 0; i < frames; i++) {
        assert(pcm[2*i] == sample(submitted+i, 0));
        assert(pcm[2*i+1] == sample(submitted+i, channels-1));
    }
    submitted += frames;
    slots[(head + count) % 2] = frames;
    count++;
    return (int)frames;
}
int audio_dac_is_busy(void) { return count != 0; }
void audio_pa_set(int on) { if (!on) assert(consumed == FRAMES); }
void audio_dac_stop(void) { assert(!count); }
void audio_dac_set_volume(uint8_t level, uint16_t digital) { (void)level; (void)digital; }
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
int keys_read_rec_switch(void) { return ended; }
int keys_read_hp_detect(void) { return 0; }
uint16_t keys_read_adc(void) { return 0; }
key_id_t keys_decode(uint16_t raw) { (void)raw; return KEY_NONE; }
void lcd_set_rotation(int rotation) { (void)rotation; }
void pmu_poll_charge_manage(void) { }
pmu_chg_state_t pmu_get_charge_state(void) { return PMU_CHG_DISCHARGING; }
uint32_t pmu_read_vbat_mv(void) { return 4000; }
void rtc_get(rtc_datetime_t *dt) { *dt = (rtc_datetime_t){0}; }

int main(void)
{
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
    return 0;
}
