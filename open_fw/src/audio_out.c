/* Audio output dispatch (BT=1 only; with BT=0 include/audio_out.h maps every call onto the DAC). One route per file:
 * DAC, headset (A2DP sink: bt_port/hp_sink.c ring drained by the BTC tick in bt_port/hp_tx.c) or both.
 *
 * BOTH: the ring feeds the headset (BTC consumer) and, through the ring's tap (a second, app-owned reader), the DAC.
 * The tap holds frames back so the DAC plays frame f about when the headset does: the headset hears frame
 * wr - q_bt now (q_bt = aout_queued_frames of the A2DP path: ring + encoded packets + latency estimate), the DAC hears
 * tap_rd - q_dac, so the tap may run ahead only while wr - tap_rd > q_bt - q_dac, i.e. it trails the headset's ring
 * reader by (q_bt - ring) - q_dac. That trail is capped at half the ring so the headset never starves on a full ring:
 * with the default 150 ms latency estimate a 32 KiB ring covers it for every source up to 48 kHz stereo (8192 frames,
 * 85 ms per half at 48 kHz: there the DAC runs early by the rest; best effort, docs/BT_A2DP_DESIGN.md live log).
 * SPDX-License-Identifier: MIT */
#include "audio_out.h"
#include <string.h>
#include "hp_sink.h"
#include "hp_tx.h"

#if !(defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID)
#error "audio_out.c is BT=1 only"
#endif

#ifdef AOUT_HOST_TEST
#include <stdlib.h>
#define aout_alloc(n) malloc(n)
#else
#include "FreeRTOS.h"
#define aout_alloc(n) pvPortMalloc(n)
#endif

#define AOUT_EXPAND_FRAMES 128u   /* mono -> stereo for the DAC route, on the stack */
#define AOUT_TAP_FRAMES    512u   /* BOTH: DAC feed chunk (heap, allocated on first use) */

/* Same Q15 curve as the DAC's digital gain (audio_dac.c): level 0 mutes, 1..31 span -48 dB .. 0 dB. */
static const uint16_t s_gain_q15[32] = {
    0, 130, 157, 189, 227, 273, 328, 394,
    474, 569, 685, 823, 990, 1190, 1430, 1720,
    2068, 2486, 2988, 3593, 4320, 5193, 6244, 7507,
    9025, 10851, 13045, 15684, 18856, 22670, 27255, 32768,
};

static struct {
    aout_mode_t           mode;
    aout_route_t          route;
    hp_tx_t              *tx;
    hp_sink_t            *sink;
    const aout_bt_link_t *link;
    uint8_t               ring_is_dac_buf;
    uint8_t               next_ch;
    int16_t              *tap_buf;
    aout_stats_t          st;
} g_ao = { .next_ch = 2u };

void aout_set_mode(aout_mode_t mode)
{
    g_ao.mode = mode;
}

aout_mode_t aout_get_mode(void)
{
    return g_ao.mode;
}

aout_route_t aout_route(void)
{
    return g_ao.route;
}

void aout_set_src_channels(uint8_t ch)
{
    g_ao.next_ch = (ch == 1u) ? 1u : 2u;
}

void aout_bt_attach(void *tx, void *sink, const aout_bt_link_t *link, int ring_is_dac_buf)
{
    if (link == NULL || tx == NULL || sink == NULL) {
        g_ao.tx = NULL;
        g_ao.sink = NULL;
        g_ao.link = NULL;
        return;
    }
    g_ao.tx = (hp_tx_t *)tx;
    g_ao.sink = (hp_sink_t *)sink;
    g_ao.link = link;
    g_ao.ring_is_dac_buf = ring_is_dac_buf ? 1u : 0u;
}

void aout_get_stats(aout_stats_t *out)
{
    *out = g_ao.st;
}

static int bt_usable(void)
{
    return g_ao.link && g_ao.tx && g_ao.sink && g_ao.link->stream_ready && g_ao.link->stream_ready();
}

int aout_start(uint32_t src_rate, uint8_t vol)
{
    aout_route_t r = AOUT_ROUTE_DAC;
    if (g_ao.mode == AOUT_MODE_HEADPHONES) {
        if (!bt_usable()) {
            g_ao.st.start_no_bt++;
            g_ao.route = AOUT_ROUTE_NONE;
            return -1;   /* no stream: works like "rate not supported"; never falls back to the speaker */
        }
        r = AOUT_ROUTE_BT;
    } else if (g_ao.mode == AOUT_MODE_BOTH && bt_usable()) {
        r = AOUT_ROUTE_BOTH;
        if (g_ao.ring_is_dac_buf) {
            r = AOUT_ROUTE_BT;   /* the ring lives in the DAC's buffer: the DAC cannot run */
            g_ao.st.both_degraded++;
        } else if (g_ao.tap_buf == NULL) {
            g_ao.tap_buf = (int16_t *)aout_alloc(AOUT_TAP_FRAMES * 2u * sizeof(int16_t));
            if (g_ao.tap_buf == NULL) {
                r = AOUT_ROUTE_BT;
                g_ao.st.both_degraded++;
            }
        }
    }
    if (r == AOUT_ROUTE_DAC || r == AOUT_ROUTE_BOTH) {
        if (audio_dac_start(src_rate, vol) != 0) {
            g_ao.route = AOUT_ROUTE_NONE;
            return -1;
        }
    }
    if (r == AOUT_ROUTE_BT || r == AOUT_ROUTE_BOTH) {
        hp_sink_set_format(g_ao.sink, src_rate, g_ao.next_ch);
        hp_sink_set_mute(g_ao.sink, 0);
        hp_sink_set_gain_q15(g_ao.sink, s_gain_q15[vol & 31u]);
        hp_sink_tap_enable(g_ao.sink, r == AOUT_ROUTE_BOTH);
        g_ao.link->request_start();
    }
    g_ao.route = r;
    return 0;
}

/* BOTH: feed the DAC from the tap while it is ahead of its target delay (see the file comment). */
static void pump_dac(void)
{
    hp_sink_t *s = g_ao.sink;
    g_ao.st.dac_pump_calls++;
    for (;;) {
        uint32_t lag = hp_sink_tap_avail(s);
        if (lag == 0u) {
            return;
        }
        /* q_bt = ring + (encoded, downstream, latency): the ring part is common to both outputs, so the tap trails
         * the headset reader by the rest minus what the DAC itself still queues, capped at half the ring. */
        uint32_t q_bt = hp_tx_app_queued_frames(g_ao.tx), q_ring = hp_sink_queued(s), q_dac = audio_dac_queued_frames();
        uint32_t after = q_bt > q_ring ? q_bt - q_ring : 0u;
        uint32_t behind = after > q_dac ? after - q_dac : 0u;
        if (behind > s->cap / 2u) {
            behind = s->cap / 2u;
        }
        uint32_t hold = q_ring + behind;
        if (lag <= hold) {
            return;
        }
        uint32_t n = lag - hold;
        n = hp_sink_tap_peek(s, g_ao.tap_buf, n < AOUT_TAP_FRAMES ? n : AOUT_TAP_FRAMES);
        int took = audio_dac_write_stereo(g_ao.tap_buf, n);
        if (took <= 0) {
            return;   /* DAC queue full */
        }
        hp_sink_tap_advance(s, (uint32_t)took);
        g_ao.st.dac_pump_frames += (uint32_t)took;
    }
}

int aout_write(const int16_t *pcm, uint32_t n, uint8_t ch)
{
    switch (g_ao.route) {
    case AOUT_ROUTE_DAC:
        if (ch == 2u) {
            return audio_dac_write_stereo(pcm, n);
        } else {
            int16_t lr[AOUT_EXPAND_FRAMES * 2u];
            uint32_t k = n < AOUT_EXPAND_FRAMES ? n : AOUT_EXPAND_FRAMES;
            for (uint32_t i = 0; i < k; i++) {
                lr[2u * i] = lr[2u * i + 1u] = pcm[i];
            }
            g_ao.st.ch_expand++;
            return audio_dac_write_stereo(lr, k);
        }
    case AOUT_ROUTE_BT: {
        uint32_t k = hp_sink_write(g_ao.sink, pcm, n, ch);
        if (k) {
            g_ao.link->request_start();   /* demand start (rev 3 R-5): unpause / scrub after a suspend */
        }
        return (int)k;
    }
    case AOUT_ROUTE_BOTH: {
        uint32_t k = hp_sink_write(g_ao.sink, pcm, n, ch);
        if (k) {
            g_ao.link->request_start();
        }
        pump_dac();
        return (int)k;
    }
    default:
        return 0;
    }
}

int aout_is_busy(void)
{
    switch (g_ao.route) {
    case AOUT_ROUTE_DAC:
        return audio_dac_is_busy();
    case AOUT_ROUTE_BT:
        return hp_tx_app_is_busy(g_ao.tx);
    case AOUT_ROUTE_BOTH:
        pump_dac();
        return hp_tx_app_is_busy(g_ao.tx) || audio_dac_is_busy() || hp_sink_tap_avail(g_ao.sink) > 0u;
    default:
        return 0;
    }
}

void aout_flush(void)
{
    if (g_ao.route == AOUT_ROUTE_DAC || g_ao.route == AOUT_ROUTE_BOTH) {
        audio_dac_flush();
    }
    if (g_ao.route == AOUT_ROUTE_BT || g_ao.route == AOUT_ROUTE_BOTH) {
        hp_tx_request_flush(g_ao.tx);   /* ring at once (tap included), held packet and audio_tx_q at the next tick */
    }
}

uint32_t aout_queued_frames(void)
{
    switch (g_ao.route) {
    case AOUT_ROUTE_DAC:
        return audio_dac_queued_frames();
    case AOUT_ROUTE_BT:
    case AOUT_ROUTE_BOTH:
        return hp_tx_app_queued_frames(g_ao.tx);   /* BOTH: the DAC is aligned to the headset */
    default:
        return 0u;
    }
}

void aout_set_mute(int muted)
{
    if (g_ao.route == AOUT_ROUTE_DAC || g_ao.route == AOUT_ROUTE_BOTH) {
        audio_dac_set_mute(muted);
    }
    if (g_ao.route == AOUT_ROUTE_BT || g_ao.route == AOUT_ROUTE_BOTH) {
        hp_sink_set_mute(g_ao.sink, muted);
        if (!muted) {
            g_ao.link->request_start();   /* unmute counts as demand */
        }
    }
}

void aout_set_volume(uint8_t vol, uint16_t dig)
{
    if (g_ao.route == AOUT_ROUTE_DAC || g_ao.route == AOUT_ROUTE_BOTH) {
        audio_dac_set_volume(vol, dig);
    }
    if (g_ao.route == AOUT_ROUTE_BT || g_ao.route == AOUT_ROUTE_BOTH) {
        hp_sink_set_gain_q15(g_ao.sink, s_gain_q15[vol & 31u]);   /* absolute volume (bt_vol) comes with H5/H6 */
    }
}

void aout_stop(void)
{
    if (g_ao.route == AOUT_ROUTE_DAC || g_ao.route == AOUT_ROUTE_BOTH) {
        audio_dac_stop();
    }
    if (g_ao.route == AOUT_ROUTE_BT || g_ao.route == AOUT_ROUTE_BOTH) {
        /* rev 3 R-M7: BACK or REC is silent at once: drop the ring and audio_tx_q; the stream stays STARTED and the
         * tick sends silence; the session suspends on idle. */
        hp_tx_request_drop(g_ao.tx);
        hp_sink_tap_enable(g_ao.sink, 0);
    }
    g_ao.route = AOUT_ROUTE_NONE;
}

int aout_is_bt(void)
{
    return g_ao.route == AOUT_ROUTE_BT || g_ao.route == AOUT_ROUTE_BOTH;
}
