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

static int screen_depth(enum ScreensEnum id) {
    switch (id) {
    case SCREEN_ID_HOME:
        return 0;
    case SCREEN_ID_FILES:
    case SCREEN_ID_FILES_EMPTY:
    case SCREEN_ID_SETTINGS:
    case SCREEN_ID_SAVED:
        return 1;
    case SCREEN_ID_DELETE_CONFIRM:
    case SCREEN_ID_ABOUT:
        return 2;
    default:
        return -1; /* Instant: REC, PLAYER, USB, BT, CHARGE, POWER, OFF */
    }
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

    int prev_depth = screen_depth(prevId);
    int next_depth = screen_depth(screenId);

    /* Delete the previous screen (or LVGL's default boot screen on first load)
     * before allocating the new one so peak LVGL heap only ever holds one
     * screen tree at a time (~15.3 KiB vs ~23.0 KiB on 64-bit host). This lets
     * BOARD_LVGL_MEM_SIZE stay at 24 KiB SRAM with generous headroom on both
     * 32-bit Cortex-M4 and 64-bit ASan/UBSan host tests. */
    if (currentScreen >= 0) {
        delete_screen_by_id(prevId);
    } else if (bootScreen) {
        lv_obj_delete(bootScreen);
    }
    currentScreen = nextScreen;
    create_screen_by_id(screenId);
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    if (screen) {
        lv_screen_load(screen);
        ui_port_set_reveal(prev_depth >= 0 && next_depth >= 0, next_depth >= prev_depth);
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
