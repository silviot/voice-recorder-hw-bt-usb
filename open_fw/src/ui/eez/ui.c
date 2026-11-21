#include "ui.h"
#include "images.h"

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index < 0) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

#ifndef OPENFW_HOST_MMIO
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
#endif

void loadScreen(enum ScreensEnum screenId) {
    int16_t nextScreen = (int16_t)(screenId - 1);
    if (nextScreen == currentScreen && getLvglObjectFromIndex(currentScreen) != 0) {
        return;
    }

    enum ScreensEnum prevId = (currentScreen >= 0)
        ? (enum ScreensEnum)(currentScreen + 1)
        : (enum ScreensEnum)0;

#ifdef OPENFW_HOST_MMIO
    if (currentScreen >= 0) {
        delete_screen_by_id(prevId);
    }
    currentScreen = nextScreen;
    create_screen_by_id(screenId);
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    if (screen) {
        lv_screen_load(screen);
    }
#else
    int d_prev = (prevId != 0) ? screen_depth(prevId) : -1;
    int d_next = screen_depth(screenId);
    int animate = (prevId != 0 && d_prev >= 0 && d_next >= 0);

    if (currentScreen >= 0 && !animate) {
        delete_screen_by_id(prevId);
    } else if (currentScreen >= 0) {
        /* Detach old screen handle; LVGL auto_del frees the old tree after slide */
        ((lv_obj_t **)&objects)[currentScreen] = 0;
    }

    currentScreen = nextScreen;
    create_screen_by_id(screenId);
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    if (!screen) {
        return;
    }

    if (animate) {
        lv_scr_load_anim_t anim = (d_next >= d_prev)
            ? LV_SCR_LOAD_ANIM_MOVE_LEFT
            : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
        lv_screen_load_anim(screen, anim, 150u, 0u, true);
    } else {
        lv_screen_load(screen);
    }
#endif
}

void ui_init(void) {
    loadScreen(SCREEN_ID_HOME);
}

void ui_tick(void) {
    if (currentScreen >= 0) {
        tick_screen(currentScreen);
    }
}
