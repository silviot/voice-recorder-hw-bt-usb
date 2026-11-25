/* Audio output sink API (docs/BT_A2DP_DESIGN.md §3.1). The player talks to "the output", not to the DAC, so the same
 * code can feed the DAC, a Bluetooth headset (A2DP source) or both.
 *
 * BT=0 (default image): macros only, aliasing the audio_dac_* calls one to one, so the default image is unchanged
 * (the gate is a byte compare of the image, not this comment). BT=1: src/audio_out.c dispatches per file between
 * the DAC sink, the A2DP sink (bt_port/hp_sink.c + hp_tx.c) and both at once.
 * SPDX-License-Identifier: MIT */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H
#include <stdint.h>
#include "openfw.h"

#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID

/* Output mode setting: applied at the next aout_start() (outputs switch only between files). */
typedef enum {
    AOUT_MODE_SPEAKER = 0,   /* DAC: speaker or wired headphones, as today */
    AOUT_MODE_HEADPHONES,    /* Bluetooth headset only; no fallback to the speaker when it is gone */
    AOUT_MODE_BOTH,          /* DAC and headset together; the DAC is delayed by the headset's latency estimate */
} aout_mode_t;

/* Route of the current file. */
typedef enum { AOUT_ROUTE_NONE = 0, AOUT_ROUTE_DAC, AOUT_ROUTE_BT, AOUT_ROUTE_BOTH } aout_route_t;

int      aout_start(uint32_t src_rate, uint8_t vol_0_31);       /* 0 = ok; never blocks on the headset (async START) */
int      aout_write(const int16_t *pcm, uint32_t n, uint8_t ch);/* frames accepted, 0 = full (partial ok) */
int      aout_is_busy(void);                                    /* real (non-pad) audio still to be heard */
void     aout_flush(void);                                      /* drop everything not yet heard (best effort) */
uint32_t aout_queued_frames(void);                              /* source-rate frames written but not yet heard */
void     aout_set_mute(int muted);
void     aout_set_volume(uint8_t vol_0_31, uint16_t dig_0_511);
/* BT=1: the DAC at dac_vol, the headset's PCM gain at bt_vol (31 = unity while the headset does absolute volume:
 * its own volume then sets the level; output mode BOTH must not take the speaker / jack to 31 with it). */
void     aout_set_volume_split(uint8_t dac_vol_0_31, uint8_t bt_vol_0_31, uint16_t dig_0_511);
void     aout_stop(void);                                       /* end of this file, not end of the BT session */
int      aout_is_bt(void);                                      /* the current file goes to a headset */

void         aout_set_mode(aout_mode_t mode);
aout_mode_t  aout_get_mode(void);
aout_route_t aout_route(void);
/* Channel count of the next file's ring (1 keeps mono sources mono up to the encoder); default 2. */
void         aout_set_src_channels(uint8_t ch);

/* ---- headset binding (the BT headphones session, bt_hp, milestone H5) ---- */
typedef struct {
    int  (*stream_ready)(void);    /* A2DP stream OPEN, STARTED or SUSPENDED */
    /* Want the stream STARTED: called by aout_start, every accepted write and unmute (demand start, rev 3 R-5).
     * Must be cheap and idempotent: bt_hp ignores it while STARTED or with a START outstanding. Never blocks. */
    void (*request_start)(void);
} aout_bt_link_t;
/* tx/sink: the session's transmit path and PCM ring (bt_port/hp_tx.h, hp_sink.h), sink already initialised with
 * its storage. ring_is_dac_buf: the ring borrows the DAC's slot buffer (audio_dac_borrow_buf), so BOTH cannot run
 * and degrades to the headset only. NULL link detaches. */
void aout_bt_attach(void *tx, void *sink, const aout_bt_link_t *link, int ring_is_dac_buf);

typedef struct {
    uint32_t both_degraded, start_no_bt, ch_expand, dac_pump_frames, dac_pump_calls;
    uint32_t dac_resync_frames;   /* BOTH: tap frames dropped after the DAC ran dry (pump_dac) */
} aout_stats_t;
void aout_get_stats(aout_stats_t *out);

/* audio_dac.c (BT=1 only): lend the 16 KiB DAC slot buffer while the DAC is stopped (NULL while it runs). */
int16_t *audio_dac_borrow_buf(uint32_t *out_bytes);

#else /* BT=0: the DAC, one to one */

#define aout_start(rate, vol)     audio_dac_start((rate), (vol))
#define aout_write(pcm, n, ch)    audio_dac_write_stereo((pcm), (n))
#define aout_is_busy()            audio_dac_is_busy()
#define aout_flush()              audio_dac_flush()
#define aout_queued_frames()      audio_dac_queued_frames()
#define aout_set_mute(m)          audio_dac_set_mute(m)
#define aout_set_volume(v, d)     audio_dac_set_volume((v), (d))
#define aout_stop()               audio_dac_stop()
#define aout_is_bt()              0

#endif
#endif
