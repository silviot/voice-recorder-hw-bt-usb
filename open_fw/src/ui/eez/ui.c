#include "ui.h"
#include "ui_port.h"
#include "images.h"

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index < 0) {
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
    int16_t nextScreen = (int16_t)(screenId - 1);
    if (nextScreen == currentScreen && getLvglObjectFromIndex(currentScreen) != 0) {
        return;
    }

    enum ScreensEnum prevId = (currentScreen >= 0)
        ? (enum ScreensEnum)(currentScreen + 1)
        : (enum ScreensEnum)0;

    int prev_depth = screen_depth(prevId);
    int next_depth = screen_depth(screenId);
    if (currentScreen >= 0) {
        delete_screen_by_id(prevId);
    }
    currentScreen = nextScreen;
    create_screen_by_id(screenId);
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    if (screen) {
        lv_screen_load(screen);
        ui_port_set_reveal(prev_depth >= 0 && next_depth >= 0, next_depth >= prev_depth);
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
