/* Host tests for bt_port/bt_lowbatt.h (plan step 8 "power transitions"): debounce, hysteresis, VBUS, start refusal.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "bt_lowbatt.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            g_fail++;                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

int main(void)
{
    bt_lowbatt_t g;
    memset(&g, 0, sizeof(g));
    /* healthy battery: never latched, never refused */
    for (int i = 0; i < 20; i++) {
        CHECK(bt_lowbatt_step(&g, 3900u, 0) == 0, "latched at 3900 mV");
    }
    CHECK(!bt_lowbatt_refuse(&g, 3900u, 0), "refused at 3900 mV");
    /* single TX sags below the cutoff do not stop BT */
    for (int i = 0; i < 10; i++) {
        CHECK(bt_lowbatt_step(&g, 3400u, 0) == 0, "one sag latched");
        CHECK(bt_lowbatt_step(&g, 3600u, 0) == 0, "recovered sample latched");
    }
    CHECK(bt_lowbatt_step(&g, 3440u, 0) == 0 && bt_lowbatt_step(&g, 3440u, 0) == 0, "two samples latched");
    CHECK(bt_lowbatt_step(&g, 3500u, 0) == 0, "band sample latched");          /* the band resets the count */
    CHECK(bt_lowbatt_step(&g, 3440u, 0) == 0 && bt_lowbatt_step(&g, 3440u, 0) == 0, "count not reset by the band");
    CHECK(bt_lowbatt_step(&g, 3440u, 0) == 1, "three consecutive low samples not latched");
    CHECK(bt_lowbatt_refuse(&g, 3800u, 0), "latched but start not refused");
    /* the hysteresis band keeps the latch; >= 3550 mV clears it */
    CHECK(bt_lowbatt_step(&g, 3500u, 0) == 1 && bt_lowbatt_step(&g, 3549u, 0) == 1, "band cleared the latch");
    CHECK(bt_lowbatt_step(&g, BT_LOWBATT_CLEAR_MV, 0) == 0, "3550 mV did not clear");
    /* a single fresh low sample refuses a start without latching */
    CHECK(bt_lowbatt_refuse(&g, 3449u, 0) && g.latched == 0u, "fresh low sample");
    CHECK(!bt_lowbatt_refuse(&g, BT_LOWBATT_MV, 0), "3450 mV refused");
    /* VBUS: the charger carries the load: no latch, no refusal, an existing latch clears */
    for (int i = 0; i < 5; i++) {
        CHECK(bt_lowbatt_step(&g, 3300u, 1) == 0, "latched with VBUS");
    }
    CHECK(!bt_lowbatt_refuse(&g, 3300u, 1), "refused with VBUS");
    for (int i = 0; i < 3; i++) {
        (void)bt_lowbatt_step(&g, 3300u, 0);
    }
    CHECK(g.latched == 1u, "not latched after unplug");
    CHECK(bt_lowbatt_step(&g, 3300u, 1) == 0, "plug did not clear");
    printf("test_bt_lowbatt: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
