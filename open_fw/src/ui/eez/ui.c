#include "ui.h"
#include "images.h"

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index < 0) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

void loadScreen(enum ScreensEnum screenId) {
    int16_t nextScreen = (int16_t)(screenId - 1);
    if (nextScreen == currentScreen && getLvglObjectFromIndex(currentScreen) != 0) {
        return;
    }
    if (currentScreen >= 0) {
        delete_screen_by_id((enum ScreensEnum)(currentScreen + 1));
    }
    currentScreen = nextScreen;
    create_screen_by_id(screenId);
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    if (screen) {
        lv_screen_load(screen);
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
