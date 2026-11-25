/* bt_lowbatt: low-battery rule for Bluetooth (plan step 8 "power transitions"; BT=1 images, src/main.c + src/player.c).
 *
 * The firmware has no automatic power-off at low battery; below 3450 mV it only refuses or stops recording (main.c
 * "battery cutoff supervision", hysteresis to 3550 mV). Bluetooth adds RF current on top (TX bursts make VBAT sag) and
 * phone mode can write the card (XREC PUT / DEL / OTA staging): a brown-out in the middle of an exFAT update is what
 * the recorder cutoff exists to avoid. So BT follows the same thresholds:
 *   - stop: VBAT below BT_LOWBATT_MV in BT_LOWBATT_SAMPLES consecutive samples BT_LOWBATT_PERIOD_MS apart (one TX
 *     sag must not end a session), no VBUS. Phone mode leaves BT mode, headphones mode stops (bt_hp_stop), both
 *     through the coordinated teardown (bt_shutdown, 2 s deadline), with a "Low battery: BT off" toast.
 *   - refuse a start: the same state, or a single fresh sample below BT_LOWBATT_MV without VBUS.
 *   - clear: a sample at or above BT_LOWBATT_CLEAR_MV, or VBUS present (the charger carries the load).
 * The thresholds are the recorder's, not measured under BT load: Exp 5 (docs/BT_HW_RESULTS.md) logs VBAT with BT
 * active and may move them.
 * SPDX-License-Identifier: MIT */
#ifndef BT_LOWBATT_H
#define BT_LOWBATT_H

#include <stdint.h>

#define BT_LOWBATT_MV         3450u
#define BT_LOWBATT_CLEAR_MV   3550u
#define BT_LOWBATT_SAMPLES    3u
#define BT_LOWBATT_PERIOD_MS  500u

typedef struct {
    uint8_t  low_n;     /* consecutive samples below BT_LOWBATT_MV */
    uint8_t  latched;   /* 1: BT must stop / not start */
    uint32_t last_mv;
} bt_lowbatt_t;

/* One sample (call every BT_LOWBATT_PERIOD_MS while BT may run). Returns the latched state. */
static inline int bt_lowbatt_step(bt_lowbatt_t *g, uint32_t mv, int vbus)
{
    g->last_mv = mv;
    if (vbus || mv >= BT_LOWBATT_CLEAR_MV) {
        g->low_n = 0u;
        g->latched = 0u;
    } else if (mv < BT_LOWBATT_MV) {
        if (g->low_n < 255u) {
            g->low_n++;
        }
        if (g->low_n >= BT_LOWBATT_SAMPLES) {
            g->latched = 1u;
        }
    } else {
        g->low_n = 0u;               /* the hysteresis band: no new count, a latch stays */
    }
    return g->latched;
}

/* Start refusal from one fresh sample (does not change the state). */
static inline int bt_lowbatt_refuse(const bt_lowbatt_t *g, uint32_t mv, int vbus)
{
    if (vbus) {
        return 0;
    }
    return g->latched || mv < BT_LOWBATT_MV;
}

#endif /* BT_LOWBATT_H */
