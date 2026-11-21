#include "styles.h"
#include "images.h"
#include "fonts.h"

static lv_style_t *st_screen_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x070B14u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_border_width(&style, 0);
        lv_style_set_pad_top(&style, 0);
        lv_style_set_pad_bottom(&style, 0);
        lv_style_set_pad_left(&style, 0);
        lv_style_set_pad_right(&style, 0);
        lv_style_set_text_color(&style, lv_color_hex(0xFFFFFFu));
    }
    return &style;
}
void add_style_st_screen(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_screen_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_style_t *st_bar_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x111827u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_border_width(&style, 0);
        lv_style_set_radius(&style, 0);
        lv_style_set_pad_top(&style, 0);
        lv_style_set_pad_bottom(&style, 0);
        lv_style_set_pad_left(&style, 3);
        lv_style_set_pad_right(&style, 3);
    }
    return &style;
}
void add_style_st_bar(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_bar_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_style_t *st_tile_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x131B2Bu));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_border_width(&style, 0);
        lv_style_set_radius(&style, 6);
        lv_style_set_pad_top(&style, 0);
        lv_style_set_pad_bottom(&style, 0);
        lv_style_set_pad_left(&style, 0);
        lv_style_set_pad_right(&style, 0);
    }
    return &style;
}
void add_style_st_tile(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_tile_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_style_t *st_row_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x1F4E8Cu));
        lv_style_set_bg_opa(&style, 0);
        lv_style_set_border_width(&style, 0);
        lv_style_set_radius(&style, 8);
        lv_style_set_pad_top(&style, 0);
        lv_style_set_pad_bottom(&style, 0);
        lv_style_set_pad_left(&style, 6);
        lv_style_set_pad_right(&style, 5);
    }
    return &style;
}
void add_style_st_row(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_row_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_style_t *st_pill_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x131B2Bu));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_border_width(&style, 0);
        lv_style_set_radius(&style, 6);
        lv_style_set_pad_top(&style, 0);
        lv_style_set_pad_bottom(&style, 0);
        lv_style_set_pad_left(&style, 5);
        lv_style_set_pad_right(&style, 5);
    }
    return &style;
}
void add_style_st_pill(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_pill_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_style_t *st_prog_cyan_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x1A2232u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
static lv_style_t *st_prog_cyan_ind_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x42C8FFu));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
void add_style_st_prog_cyan(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_prog_cyan_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, st_prog_cyan_ind_default(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

static lv_style_t *st_prog_red_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x1A2232u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
static lv_style_t *st_prog_red_ind_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0xFF3B3Bu));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
void add_style_st_prog_red(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_prog_red_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, st_prog_red_ind_default(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

static lv_style_t *st_prog_green_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x1A2232u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
static lv_style_t *st_prog_green_ind_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x3DDC84u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
void add_style_st_prog_green(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_prog_green_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, st_prog_green_ind_default(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

static lv_style_t *st_prog_white_main_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0x1A2232u));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
static lv_style_t *st_prog_white_ind_default(void) {
    static lv_style_t style;
    static int inited = 0;
    if (!inited) {
        inited = 1;
        lv_style_init(&style);
        lv_style_set_bg_color(&style, lv_color_hex(0xFFFFFFu));
        lv_style_set_bg_opa(&style, 255);
        lv_style_set_radius(&style, LV_RADIUS_CIRCLE);
    }
    return &style;
}
void add_style_st_prog_white(lv_obj_t *obj) {
    lv_obj_add_style(obj, st_prog_white_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, st_prog_white_ind_default(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
}
