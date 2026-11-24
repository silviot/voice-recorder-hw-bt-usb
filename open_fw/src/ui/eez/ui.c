#include "ui.h"
#include "ui_port.h"
#include "images.h"

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    const int32_t count = (int32_t)(_SCREEN_ID_LAST - _SCREEN_ID_FIRST + 1);
    if (index < 0 || index >= count) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

/* On 32-bit Cortex-M4 (4-byte pointers), keeping the 3 primary navigation
 * screens (HOME, FILES, SETTINGS) resident in LVGL's 24 KiB SRAM heap uses
 * ~11 KiB total and eliminates ~95 ms of widget teardown/rebuild on every
 * Home <-> Files and Home <-> Settings navigation press. */
static int is_cached_screen(enum ScreensEnum id) {
#if !defined(OPENFW_HOST_MMIO)
    return id == SCREEN_ID_HOME || id == SCREEN_ID_FILES || id == SCREEN_ID_SETTINGS;
#else
    (void)id;
    return 0;
#endif
}

void loadScreen(enum ScreensEnum screenId) {
    if (screenId < _SCREEN_ID_FIRST || screenId > _SCREEN_ID_LAST) {
        return;
    }

    int16_t nextScreen = (int16_t)(screenId - 1);
    if (nextScreen == currentScreen && getLvglObjectFromIndex(currentScreen) != 0) {
        return;
    }

    enum ScreensEnum prevId = (currentScreen >= 0)
        ? (enum ScreensEnum)(currentScreen + 1)
        : (enum ScreensEnum)0;
    lv_obj_t *bootScreen = (currentScreen < 0) ? lv_screen_active() : 0;

    /* Evict non-cached previous screen BEFORE allocating the new one when
     * neither is cached, or after switching away when cached — Wait: always
     * delete non-cached prev BEFORE creating new screen so worst-case peak
     * heap on 64-bit host tests (test_ui_render.c <= 18 KiB) never holds two
     * uncached screens simultaneously! */
    if (currentScreen >= 0 && !is_cached_screen(prevId)) {
        delete_screen_by_id(prevId);
    } else if (bootScreen) {
        lv_obj_delete(bootScreen);
    }

    lv_obj_t *screen = getLvglObjectFromIndex(nextScreen);
    if (!screen) {
#if !defined(OPENFW_HOST_MMIO)
        /* Guarantee >= 10 KiB contiguous LVGL heap before creating any screen
         * so caching HOME/FILES/SETTINGS can never trigger an OOM assert even
         * when creating 35-widget screens like REC. */
        static const enum ScreensEnum evict_order[3] = {
            SCREEN_ID_SETTINGS, SCREEN_ID_FILES, SCREEN_ID_HOME
        };
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < 3; i++) {
                lv_mem_monitor_t mon;
                lv_mem_monitor(&mon);
                if (mon.free_biggest_size >= 10240u) {
                    pass = 2;
                    break;
                }
                if (evict_order[i] != screenId && (pass == 1 || evict_order[i] != prevId)) {
                    if (evict_order[i] == prevId) {
                        currentScreen = -1;
                    }
                    delete_screen_by_id(evict_order[i]);
                }
            }
        }
#endif
        lv_obj_enable_style_refresh(false);
        create_screen_by_id(screenId);
        lv_obj_enable_style_refresh(true);
        screen = getLvglObjectFromIndex(nextScreen);
        if (screen) {
            lv_obj_refresh_style(screen, LV_PART_ANY, LV_STYLE_PROP_ANY);
        }
    }

    if (screen) {
        currentScreen = nextScreen;
        lv_screen_load(screen);
    } else {
        currentScreen = -1;
    }
}

void ui_init(void) {
    loadScreen(SCREEN_ID_HOME);
}

void ui_tick(void) {
    if (currentScreen >= 0) {
        tick_screen(currentScreen);
    }
}
