/* BT phone-mode screen pacing (src/main.c run_bt_xrec_mode; docs/BT_SYNC_THROUGHPUT.md).
 *
 * The BT screen's UI block (status, telemetry, toast, render) costs the app task time that the XREC GETS producer
 * shares (silicon 2026-11-26: skipping it raised a sync from ~39 to ~42 KB/s). It runs every BT_UI_PERIOD_MS, but at
 * most every BT_UI_XFER_PERIOD_MS while a file transfer is shown (bt_state 2: the progress still moves twice a second).
 * A key press, a pairing-prompt answer or the first pass force it at once (last_ui = 0). Charge management keeps its
 * own BT_UI_PERIOD_MS tick outside this block. Nothing is rendered while the screen is off or fading out.
 * SPDX-License-Identifier: MIT */
#ifndef OPENFW_BT_UI_PACE_H
#define OPENFW_BT_UI_PACE_H

#include <stdint.h>

#define BT_UI_PERIOD_MS      150u
#define BT_UI_XFER_PERIOD_MS 500u

/* 1 = run the UI block now. last_ui 0 = forced (first pass, key, prompt); bt_state as last shown (2 = transfer). */
static inline int bt_ui_due(uint32_t now, uint32_t last_ui, uint8_t bt_state)
{
    if (last_ui == 0u) {
        return 1;
    }
    uint32_t period = (bt_state == 2u) ? BT_UI_XFER_PERIOD_MS : BT_UI_PERIOD_MS;
    return (uint32_t)(now - last_ui) >= period;   /* wrap-safe */
}

/* 1 = render the BT screen: it is shown (not the power-hold screen), not off, and not fading out to off. */
static inline int bt_ui_render_ok(int shown, int screen_off, int fading_out)
{
    return shown && !screen_off && !fading_out;
}

#endif
