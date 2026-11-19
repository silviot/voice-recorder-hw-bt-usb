/*
 * Smartlink SL6801 (pen133_2.0.65) Clean-Room C UI Replacement Example
 *
 * Demonstrates replacing or extending a stock LVGL v8.3 UI page on CPU1
 * (ViewTask) in pure C using fw_mod/sl6801_sdk.h.
 *
 * Example: renders a custom status/branding overlay (or full replacement
 * screen) on the 80x160 ST7735S IPS display showing custom title, live
 * battery voltage from PMU SRAM, and RTC time — zero stock UI code needed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl6801_sdk.h"

/*
 * Clean-room C replacement for an 80x160 status / splash screen.
 * Safe to call from any ViewTask (CPU1) page hook (e.g. page_device_info_create
 * or right after page_home_create).
 */
__attribute__((used))
void custom_render_cassini_splash(void)
{
    void *scr = lv_disp_get_scr_act((void *)0);
    if (!scr) {
        return;
    }

    /* Dark navy background */
    lv_obj_set_style_bg_color(scr, sl_rgb565(10, 14, 28), 0u);
    lv_obj_set_style_bg_opa(scr, 0xFFu, 0u);

    /* Title */
    void *title = lv_label_create(scr);
    if (title) {
        lv_label_set_text(title, "CASSINI");
        lv_obj_set_style_txt_clr(title, sl_rgb565(120, 220, 255), 0u);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    }

    /* Subtitle */
    void *sub = lv_label_create(scr);
    if (sub) {
        lv_label_set_text(sub, "HW_76 v5.1");
        lv_obj_set_style_txt_clr(sub, sl_rgb565(220, 220, 230), 0u);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, -4);
    }

    /* Live battery % and voltage from lock-free PMU SRAM */
    void *bat = lv_label_create(scr);
    if (bat) {
        uint8_t pct = PMU_STATE_ADDR[3];
        uint16_t mv = *PMU_VOLTAGE_MV_ADDR;
        lv_label_set_text_fmt(bat, "%u%% %umV", (unsigned)pct, (unsigned)mv);
        lv_obj_set_style_txt_clr(bat, sl_rgb565(140, 255, 160), 0u);
        lv_obj_align(bat, LV_ALIGN_BOTTOM_MID, 0, -18);
    }
}
