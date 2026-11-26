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

    /* Always delete the previous screen before building the next so target and
     * ASan/UBSan host tests share an identical single-screen TLSF heap footprint. */
    if (currentScreen >= 0) {
        delete_screen_by_id(prevId);
        currentScreen = -1;
    } else if (bootScreen) {
        lv_obj_delete(bootScreen);
    }

    lv_obj_t *screen = getLvglObjectFromIndex(nextScreen);
    if (!screen) {
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
