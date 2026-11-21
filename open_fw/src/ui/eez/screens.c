#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

objects_t objects;

void create_screen_home(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.home = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Fri 20 Nov");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.home_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.home_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.home_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_tile_0 = obj;
            lv_obj_set_pos(obj, 3, 16);
            lv_obj_set_size(obj, 37, 37);
            add_style_st_tile(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_ic_0 = obj;
                    lv_obj_set_pos(obj, 0, 4);
                    lv_obj_set_size(obj, 37, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "\xE2\x96\xB6");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_lbl_0 = obj;
                    lv_obj_set_pos(obj, 0, 22);
                    lv_obj_set_size(obj, 37, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Last");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_tile_1 = obj;
            lv_obj_set_pos(obj, 42, 16);
            lv_obj_set_size(obj, 37, 37);
            add_style_st_tile(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_ic_1 = obj;
                    lv_obj_set_pos(obj, 0, 4);
                    lv_obj_set_size(obj, 37, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "=");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_lbl_1 = obj;
                    lv_obj_set_pos(obj, 0, 22);
                    lv_obj_set_size(obj, 37, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Files");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_tile_2 = obj;
            lv_obj_set_pos(obj, 81, 16);
            lv_obj_set_size(obj, 37, 37);
            add_style_st_tile(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_ic_2 = obj;
                    lv_obj_set_pos(obj, 0, 4);
                    lv_obj_set_size(obj, 37, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "*");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_lbl_2 = obj;
                    lv_obj_set_pos(obj, 0, 22);
                    lv_obj_set_size(obj, 37, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Setup");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_tile_3 = obj;
            lv_obj_set_pos(obj, 120, 16);
            lv_obj_set_size(obj, 37, 37);
            add_style_st_tile(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_ic_3 = obj;
                    lv_obj_set_pos(obj, 0, 4);
                    lv_obj_set_size(obj, 37, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "BT");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_tile_lbl_3 = obj;
                    lv_obj_set_pos(obj, 0, 22);
                    lv_obj_set_size(obj, 37, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Sync");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_focus_mark = obj;
            lv_obj_set_pos(obj, 51, 53);
            lv_obj_set_size(obj, 19, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.home_details = obj;
            lv_obj_set_pos(obj, 3, 55);
            lv_obj_set_size(obj, 154, 24);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_0_0 = obj;
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Last recording");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_0_1 = obj;
                    lv_obj_set_pos(obj, 0, 12);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Play from start");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_1_0 = obj;
                    lv_obj_set_pos(obj, 154, 0);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "4 recordings");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_1_1 = obj;
                    lv_obj_set_pos(obj, 154, 12);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "18h 20m of space left");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_2_0 = obj;
                    lv_obj_set_pos(obj, 308, 0);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Gain 20");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_2_1 = obj;
                    lv_obj_set_pos(obj, 308, 12);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Voice trigger off");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_3_0 = obj;
                    lv_obj_set_pos(obj, 462, 0);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Bluetooth file sync");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.home_detail_3_1 = obj;
                    lv_obj_set_pos(obj, 462, 12);
                    lv_obj_set_size(obj, 154, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Device name HW_76");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
    }
}

void delete_screen_home(void) {
    if (objects.home) {
        lv_obj_delete(objects.home);
        objects.home = 0;
        objects.home_bar = 0;
        objects.home_bar_ic = 0;
        objects.home_bar_l = 0;
        objects.home_bar_sub = 0;
        objects.home_bar_r = 0;
        objects.home_bar_hp = 0;
        objects.home_batt = 0;
        objects.home_batt_fill = 0;
        objects.home_batt_nub = 0;
        objects.home_tile_0 = 0;
        objects.home_tile_ic_0 = 0;
        objects.home_tile_lbl_0 = 0;
        objects.home_tile_1 = 0;
        objects.home_tile_ic_1 = 0;
        objects.home_tile_lbl_1 = 0;
        objects.home_tile_2 = 0;
        objects.home_tile_ic_2 = 0;
        objects.home_tile_lbl_2 = 0;
        objects.home_tile_3 = 0;
        objects.home_tile_ic_3 = 0;
        objects.home_tile_lbl_3 = 0;
        objects.home_focus_mark = 0;
        objects.home_details = 0;
        objects.home_detail_0_0 = 0;
        objects.home_detail_0_1 = 0;
        objects.home_detail_1_0 = 0;
        objects.home_detail_1_1 = 0;
        objects.home_detail_2_0 = 0;
        objects.home_detail_2_1 = 0;
        objects.home_detail_3_0 = 0;
        objects.home_detail_3_1 = 0;
    }
}

void tick_screen_home(void) {}

void create_screen_rec(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.rec = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0xFF3B3Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "REC");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "2ch \xC2\xB7 Raw");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.rec_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFF3B3Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.rec_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.rec_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.rec_timer = obj;
            lv_obj_set_pos(obj, 3, 14);
            lv_obj_set_size(obj, 90, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "00:00");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_gain_pill = obj;
            lv_obj_set_pos(obj, 107, 16);
            lv_obj_set_size(obj, 50, 13);
            add_style_st_pill(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_gain_lbl = obj;
                    lv_obj_set_pos(obj, 0, 1);
                    lv_obj_set_size(obj, 50, 11);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Gain 20");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.rec_free_lbl = obj;
            lv_obj_set_pos(obj, 86, 29);
            lv_obj_set_size(obj, 70, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "18h 20m left");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_image_create(parent_obj);
            objects.rec_spec_img = obj;
            lv_obj_set_pos(obj, 2, 42);
            lv_obj_set_size(obj, 144, 24);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x000000u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_0 = obj;
            lv_obj_set_pos(obj, 148, 63);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_1 = obj;
            lv_obj_set_pos(obj, 148, 60);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_2 = obj;
            lv_obj_set_pos(obj, 148, 57);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_3 = obj;
            lv_obj_set_pos(obj, 148, 54);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_4 = obj;
            lv_obj_set_pos(obj, 148, 51);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_5 = obj;
            lv_obj_set_pos(obj, 148, 48);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_6 = obj;
            lv_obj_set_pos(obj, 148, 45);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vml_7 = obj;
            lv_obj_set_pos(obj, 148, 42);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_0 = obj;
            lv_obj_set_pos(obj, 153, 63);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_1 = obj;
            lv_obj_set_pos(obj, 153, 60);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_2 = obj;
            lv_obj_set_pos(obj, 153, 57);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_3 = obj;
            lv_obj_set_pos(obj, 153, 54);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_4 = obj;
            lv_obj_set_pos(obj, 153, 51);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_5 = obj;
            lv_obj_set_pos(obj, 153, 48);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_6 = obj;
            lv_obj_set_pos(obj, 153, 45);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_vmr_7 = obj;
            lv_obj_set_pos(obj, 153, 42);
            lv_obj_set_size(obj, 4, 2);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_foot_box = obj;
            lv_obj_set_pos(obj, 2, 67);
            lv_obj_set_size(obj, 156, 13);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_foot_left = obj;
                    lv_obj_set_pos(obj, 2, 1);
                    lv_obj_set_size(obj, 108, 11);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "REC_0001.wav");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x5D6B82u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_foot_right = obj;
                    lv_obj_set_pos(obj, 104, 1);
                    lv_obj_set_size(obj, 50, 11);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "OK Mark");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_bar_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_bar_set_range(obj, 0, 31);
                    lv_bar_set_value(obj, 20, LV_ANIM_OFF);
                    objects.rec_foot_bar = obj;
                    lv_obj_set_pos(obj, 4, 4);
                    lv_obj_set_size(obj, 148, 5);
                    add_style_st_prog_cyan(obj);
                }
            }
        }
    }
}

void delete_screen_rec(void) {
    if (objects.rec) {
        lv_obj_delete(objects.rec);
        objects.rec = 0;
        objects.rec_bar = 0;
        objects.rec_bar_ic = 0;
        objects.rec_bar_l = 0;
        objects.rec_bar_sub = 0;
        objects.rec_bar_r = 0;
        objects.rec_bar_hp = 0;
        objects.rec_batt = 0;
        objects.rec_batt_fill = 0;
        objects.rec_batt_nub = 0;
        objects.rec_timer = 0;
        objects.rec_gain_pill = 0;
        objects.rec_gain_lbl = 0;
        objects.rec_free_lbl = 0;
        objects.rec_spec_img = 0;
        objects.rec_vml_0 = 0;
        objects.rec_vml_1 = 0;
        objects.rec_vml_2 = 0;
        objects.rec_vml_3 = 0;
        objects.rec_vml_4 = 0;
        objects.rec_vml_5 = 0;
        objects.rec_vml_6 = 0;
        objects.rec_vml_7 = 0;
        objects.rec_vmr_0 = 0;
        objects.rec_vmr_1 = 0;
        objects.rec_vmr_2 = 0;
        objects.rec_vmr_3 = 0;
        objects.rec_vmr_4 = 0;
        objects.rec_vmr_5 = 0;
        objects.rec_vmr_6 = 0;
        objects.rec_vmr_7 = 0;
        objects.rec_foot_box = 0;
        objects.rec_foot_left = 0;
        objects.rec_foot_right = 0;
        objects.rec_foot_bar = 0;
    }
}

void tick_screen_rec(void) {}

void create_screen_rec_err(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.rec_err = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.rec_err_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x5A1418u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_err_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_err_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Stopped");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_err_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_err_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.rec_err_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.rec_err_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x5A1418u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.rec_err_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.rec_err_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.rec_err_title = obj;
            lv_obj_set_pos(obj, 4, 15);
            lv_obj_set_size(obj, 152, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Storage error");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFF3B3Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.rec_err_l1 = obj;
            lv_obj_set_pos(obj, 4, 41);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Nothing is being recorded.");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.rec_err_l2 = obj;
            lv_obj_set_pos(obj, 4, 53);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Slide REC off, then connect");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.rec_err_l3 = obj;
            lv_obj_set_pos(obj, 4, 65);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "USB to check the storage.");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_rec_err(void) {
    if (objects.rec_err) {
        lv_obj_delete(objects.rec_err);
        objects.rec_err = 0;
        objects.rec_err_bar = 0;
        objects.rec_err_bar_ic = 0;
        objects.rec_err_bar_l = 0;
        objects.rec_err_bar_sub = 0;
        objects.rec_err_bar_r = 0;
        objects.rec_err_bar_hp = 0;
        objects.rec_err_batt = 0;
        objects.rec_err_batt_fill = 0;
        objects.rec_err_batt_nub = 0;
        objects.rec_err_title = 0;
        objects.rec_err_l1 = 0;
        objects.rec_err_l2 = 0;
        objects.rec_err_l3 = 0;
    }
}

void tick_screen_rec_err(void) {}

void create_screen_saved(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.saved = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.saved_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.saved_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x000000u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.saved_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Saved");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x000000u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.saved_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.saved_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.saved_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.saved_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.saved_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.saved_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.saved_dur = obj;
            lv_obj_set_pos(obj, 4, 14);
            lv_obj_set_size(obj, 152, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "12:34");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.saved_meta = obj;
            lv_obj_set_pos(obj, 4, 39);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Today 14:32 \xC2\xB7 2ch");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.saved_file = obj;
            lv_obj_set_pos(obj, 4, 50);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "REC_0043.wav");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x5D6B82u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.saved_footer = obj;
            lv_obj_set_pos(obj, 0, 64);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "OK Play it \xC2\xB7 BACK Done");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 65, LV_ANIM_OFF);
            objects.saved_prog = obj;
            lv_obj_set_pos(obj, 0, 77);
            lv_obj_set_size(obj, 160, 3);
            add_style_st_prog_green(obj);
        }
    }
}

void delete_screen_saved(void) {
    if (objects.saved) {
        lv_obj_delete(objects.saved);
        objects.saved = 0;
        objects.saved_bar = 0;
        objects.saved_bar_ic = 0;
        objects.saved_bar_l = 0;
        objects.saved_bar_sub = 0;
        objects.saved_bar_r = 0;
        objects.saved_bar_hp = 0;
        objects.saved_batt = 0;
        objects.saved_batt_fill = 0;
        objects.saved_batt_nub = 0;
        objects.saved_dur = 0;
        objects.saved_meta = 0;
        objects.saved_file = 0;
        objects.saved_footer = 0;
        objects.saved_prog = 0;
    }
}

void tick_screen_saved(void) {}

void create_screen_files(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.files = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.files_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Files");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.files_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.files_row_0 = obj;
            lv_obj_set_pos(obj, 2, 15);
            lv_obj_set_size(obj, 151, 16);
            add_style_st_row(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_dot_0 = obj;
                    lv_obj_set_pos(obj, 5, 6);
                    lv_obj_set_size(obj, 4, 4);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_name_0 = obj;
                    lv_obj_set_pos(obj, 12, 0);
                    lv_obj_set_size(obj, 92, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Today 14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_badge_0 = obj;
                    lv_obj_set_pos(obj, 106, 2);
                    lv_obj_set_size(obj, 11, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "T");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_dur_0 = obj;
                    lv_obj_set_pos(obj, 118, 2);
                    lv_obj_set_size(obj, 29, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "03:12");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.files_row_1 = obj;
            lv_obj_set_pos(obj, 2, 32);
            lv_obj_set_size(obj, 151, 16);
            add_style_st_row(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_dot_1 = obj;
                    lv_obj_set_pos(obj, 5, 6);
                    lv_obj_set_size(obj, 4, 4);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_name_1 = obj;
                    lv_obj_set_pos(obj, 12, 0);
                    lv_obj_set_size(obj, 92, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Today 14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_badge_1 = obj;
                    lv_obj_set_pos(obj, 106, 2);
                    lv_obj_set_size(obj, 11, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "T");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_dur_1 = obj;
                    lv_obj_set_pos(obj, 118, 2);
                    lv_obj_set_size(obj, 29, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "03:12");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.files_row_2 = obj;
            lv_obj_set_pos(obj, 2, 49);
            lv_obj_set_size(obj, 151, 16);
            add_style_st_row(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_dot_2 = obj;
                    lv_obj_set_pos(obj, 5, 6);
                    lv_obj_set_size(obj, 4, 4);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_name_2 = obj;
                    lv_obj_set_pos(obj, 12, 0);
                    lv_obj_set_size(obj, 92, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
                    lv_label_set_text(obj, "Today 14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_badge_2 = obj;
                    lv_obj_set_pos(obj, 106, 2);
                    lv_obj_set_size(obj, 11, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "T");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_dur_2 = obj;
                    lv_obj_set_pos(obj, 118, 2);
                    lv_obj_set_size(obj, 29, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "03:12");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.files_scroll_track = obj;
            lv_obj_set_pos(obj, 155, 15);
            lv_obj_set_size(obj, 2, 50);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_scroll_thumb = obj;
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, 2, 36);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.files_footer = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "OK Play \xC2\xB7 Hold OK Delete");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_files(void) {
    if (objects.files) {
        lv_obj_delete(objects.files);
        objects.files = 0;
        objects.files_bar = 0;
        objects.files_bar_ic = 0;
        objects.files_bar_l = 0;
        objects.files_bar_sub = 0;
        objects.files_bar_r = 0;
        objects.files_bar_hp = 0;
        objects.files_batt = 0;
        objects.files_batt_fill = 0;
        objects.files_batt_nub = 0;
        objects.files_row_0 = 0;
        objects.files_dot_0 = 0;
        objects.files_name_0 = 0;
        objects.files_badge_0 = 0;
        objects.files_dur_0 = 0;
        objects.files_row_1 = 0;
        objects.files_dot_1 = 0;
        objects.files_name_1 = 0;
        objects.files_badge_1 = 0;
        objects.files_dur_1 = 0;
        objects.files_row_2 = 0;
        objects.files_dot_2 = 0;
        objects.files_name_2 = 0;
        objects.files_badge_2 = 0;
        objects.files_dur_2 = 0;
        objects.files_scroll_track = 0;
        objects.files_scroll_thumb = 0;
        objects.files_footer = 0;
    }
}

void tick_screen_files(void) {}

void create_screen_files_empty(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.files_empty = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.files_e_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_e_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_e_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Files");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_e_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_e_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.files_e_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_e_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.files_e_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.files_e_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.files_empty_ic = obj;
            lv_obj_set_pos(obj, 0, 20);
            lv_obj_set_size(obj, 160, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "MIC");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x344055u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.files_empty_title = obj;
            lv_obj_set_pos(obj, 0, 38);
            lv_obj_set_size(obj, 160, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "No recordings yet");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.files_empty_foot = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Slide REC to record");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_files_empty(void) {
    if (objects.files_empty) {
        lv_obj_delete(objects.files_empty);
        objects.files_empty = 0;
        objects.files_e_bar = 0;
        objects.files_e_bar_ic = 0;
        objects.files_e_bar_l = 0;
        objects.files_e_bar_sub = 0;
        objects.files_e_bar_r = 0;
        objects.files_e_bar_hp = 0;
        objects.files_e_batt = 0;
        objects.files_e_batt_fill = 0;
        objects.files_e_batt_nub = 0;
        objects.files_empty_ic = 0;
        objects.files_empty_title = 0;
        objects.files_empty_foot = 0;
    }
}

void tick_screen_files_empty(void) {}

void create_screen_player(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.player = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.play_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.play_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.play_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Playing");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.play_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.play_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.play_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.play_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.play_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.play_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.player_date = obj;
            lv_obj_set_pos(obj, 4, 16);
            lv_obj_set_size(obj, 100, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
            lv_label_set_text(obj, "Today 14:32");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.player_vol_pill = obj;
            lv_obj_set_pos(obj, 112, 15);
            lv_obj_set_size(obj, 44, 14);
            add_style_st_pill(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.player_vol_lbl = obj;
                    lv_obj_set_pos(obj, 0, 1);
                    lv_obj_set_size(obj, 44, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP 5.8");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.player_pos = obj;
            lv_obj_set_pos(obj, 3, 29);
            lv_obj_set_size(obj, 64, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "02:00");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.player_tot = obj;
            lv_obj_set_pos(obj, 66, 39);
            lv_obj_set_size(obj, 52, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "/ 03:12");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 55, LV_ANIM_OFF);
            objects.player_vu_l = obj;
            lv_obj_set_pos(obj, 122, 37);
            lv_obj_set_size(obj, 34, 3);
            add_style_st_prog_green(obj);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 48, LV_ANIM_OFF);
            objects.player_vu_r = obj;
            lv_obj_set_pos(obj, 122, 43);
            lv_obj_set_size(obj, 34, 3);
            add_style_st_prog_green(obj);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 62, LV_ANIM_OFF);
            objects.player_prog = obj;
            lv_obj_set_pos(obj, 6, 56);
            lv_obj_set_size(obj, 148, 5);
            add_style_st_prog_cyan(obj);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.player_footer = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "\xE2\x97\x80 5s \xC2\xB7 OK Pause \xC2\xB7 \xE2\x96\xB6 10s");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_player(void) {
    if (objects.player) {
        lv_obj_delete(objects.player);
        objects.player = 0;
        objects.play_bar = 0;
        objects.play_bar_ic = 0;
        objects.play_bar_l = 0;
        objects.play_bar_sub = 0;
        objects.play_bar_r = 0;
        objects.play_bar_hp = 0;
        objects.play_batt = 0;
        objects.play_batt_fill = 0;
        objects.play_batt_nub = 0;
        objects.player_date = 0;
        objects.player_vol_pill = 0;
        objects.player_vol_lbl = 0;
        objects.player_pos = 0;
        objects.player_tot = 0;
        objects.player_vu_l = 0;
        objects.player_vu_r = 0;
        objects.player_prog = 0;
        objects.player_footer = 0;
    }
}

void tick_screen_player(void) {}

void create_screen_player_tr(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.player_tr = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.ptr_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.ptr_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.ptr_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "05:49");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.ptr_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.ptr_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.ptr_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.ptr_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.ptr_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.ptr_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 25, LV_ANIM_OFF);
            objects.ptr_prog = obj;
            lv_obj_set_pos(obj, 0, 13);
            lv_obj_set_size(obj, 160, 2);
            add_style_st_prog_cyan(obj);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.ptr_bar_prev = obj;
            lv_obj_set_pos(obj, 0, 18);
            lv_obj_set_size(obj, 2, 14);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.ptr_prev = obj;
            lv_obj_set_pos(obj, 6, 17);
            lv_obj_set_size(obj, 152, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
            lv_label_set_text(obj, "Previous line");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x344055u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.ptr_bar_curr = obj;
            lv_obj_set_pos(obj, 0, 34);
            lv_obj_set_size(obj, 2, 14);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.ptr_curr_spoken = obj;
            lv_obj_set_pos(obj, 6, 33);
            lv_obj_set_size(obj, 152, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
            lv_label_set_text(obj, "Current spoken");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.ptr_bar_next = obj;
            lv_obj_set_pos(obj, 0, 50);
            lv_obj_set_size(obj, 2, 14);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.ptr_next = obj;
            lv_obj_set_pos(obj, 6, 49);
            lv_obj_set_size(obj, 152, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
            lv_label_set_text(obj, "Next line");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x344055u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.ptr_foot_box = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.ptr_footer = obj;
                    lv_obj_set_pos(obj, 0, 1);
                    lv_obj_set_size(obj, 160, 11);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "\xE2\x97\x80\xE2\x96\xB6 Line \xC2\xB7 OK Pause \xC2\xB7 \xC2\xB1 Vol");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
    }
}

void delete_screen_player_tr(void) {
    if (objects.player_tr) {
        lv_obj_delete(objects.player_tr);
        objects.player_tr = 0;
        objects.ptr_bar = 0;
        objects.ptr_bar_ic = 0;
        objects.ptr_bar_l = 0;
        objects.ptr_bar_sub = 0;
        objects.ptr_bar_r = 0;
        objects.ptr_bar_hp = 0;
        objects.ptr_batt = 0;
        objects.ptr_batt_fill = 0;
        objects.ptr_batt_nub = 0;
        objects.ptr_prog = 0;
        objects.ptr_bar_prev = 0;
        objects.ptr_prev = 0;
        objects.ptr_bar_curr = 0;
        objects.ptr_curr_spoken = 0;
        objects.ptr_bar_next = 0;
        objects.ptr_next = 0;
        objects.ptr_foot_box = 0;
        objects.ptr_footer = 0;
    }
}

void tick_screen_player_tr(void) {}

void create_screen_delete_confirm(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.delete_confirm = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.del_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0xFF3B3Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.del_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.del_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Delete?");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.del_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.del_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.del_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.del_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFF3B3Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.del_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.del_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.del_date = obj;
            lv_obj_set_pos(obj, 4, 18);
            lv_obj_set_size(obj, 152, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Today 14:32");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.del_sub = obj;
            lv_obj_set_pos(obj, 4, 35);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "03:12 \xC2\xB7 REC_0043.wav");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 55, LV_ANIM_OFF);
            objects.del_hold_bar = obj;
            lv_obj_set_pos(obj, 10, 52);
            lv_obj_set_size(obj, 140, 7);
            add_style_st_prog_red(obj);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.del_footer = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Hold OK Delete \xC2\xB7 BACK Keep");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_delete_confirm(void) {
    if (objects.delete_confirm) {
        lv_obj_delete(objects.delete_confirm);
        objects.delete_confirm = 0;
        objects.del_bar = 0;
        objects.del_bar_ic = 0;
        objects.del_bar_l = 0;
        objects.del_bar_sub = 0;
        objects.del_bar_r = 0;
        objects.del_bar_hp = 0;
        objects.del_batt = 0;
        objects.del_batt_fill = 0;
        objects.del_batt_nub = 0;
        objects.del_date = 0;
        objects.del_sub = 0;
        objects.del_hold_bar = 0;
        objects.del_footer = 0;
    }
}

void tick_screen_delete_confirm(void) {}

void create_screen_settings(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.settings = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.set_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Settings");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.set_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.set_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.set_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.set_row_0 = obj;
            lv_obj_set_pos(obj, 2, 15);
            lv_obj_set_size(obj, 151, 16);
            add_style_st_row(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_lbl_0 = obj;
                    lv_obj_set_pos(obj, 6, 0);
                    lv_obj_set_size(obj, 86, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Mic gain");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_val_0 = obj;
                    lv_obj_set_pos(obj, 92, 2);
                    lv_obj_set_size(obj, 54, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "- 20 +");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.set_row_1 = obj;
            lv_obj_set_pos(obj, 2, 32);
            lv_obj_set_size(obj, 151, 16);
            add_style_st_row(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_lbl_1 = obj;
                    lv_obj_set_pos(obj, 6, 0);
                    lv_obj_set_size(obj, 86, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Mic gain");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_val_1 = obj;
                    lv_obj_set_pos(obj, 92, 2);
                    lv_obj_set_size(obj, 54, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "- 20 +");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.set_row_2 = obj;
            lv_obj_set_pos(obj, 2, 49);
            lv_obj_set_size(obj, 151, 16);
            add_style_st_row(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1F4E8Cu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_lbl_2 = obj;
                    lv_obj_set_pos(obj, 6, 0);
                    lv_obj_set_size(obj, 86, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Mic gain");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_val_2 = obj;
                    lv_obj_set_pos(obj, 92, 2);
                    lv_obj_set_size(obj, 54, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "- 20 +");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.set_scroll_track = obj;
            lv_obj_set_pos(obj, 155, 15);
            lv_obj_set_size(obj, 2, 50);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x1A2232u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.set_scroll_thumb = obj;
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, 2, 15);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.set_foot_desc = obj;
            lv_obj_set_pos(obj, 3, 67);
            lv_obj_set_size(obj, 154, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.set_foot_vu_box = obj;
            lv_obj_set_pos(obj, 4, 67);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.set_foot_vu_lbl = obj;
                    lv_obj_set_pos(obj, 0, 0);
                    lv_obj_set_size(obj, 30, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Input");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_bar_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_bar_set_range(obj, 0, 100);
                    lv_bar_set_value(obj, 35, LV_ANIM_OFF);
                    objects.set_foot_vu_bar = obj;
                    lv_obj_set_pos(obj, 32, 3);
                    lv_obj_set_size(obj, 120, 5);
                    add_style_st_prog_green(obj);
                }
            }
        }
    }
}

void delete_screen_settings(void) {
    if (objects.settings) {
        lv_obj_delete(objects.settings);
        objects.settings = 0;
        objects.set_bar = 0;
        objects.set_bar_ic = 0;
        objects.set_bar_l = 0;
        objects.set_bar_sub = 0;
        objects.set_bar_r = 0;
        objects.set_bar_hp = 0;
        objects.set_batt = 0;
        objects.set_batt_fill = 0;
        objects.set_batt_nub = 0;
        objects.set_row_0 = 0;
        objects.set_lbl_0 = 0;
        objects.set_val_0 = 0;
        objects.set_row_1 = 0;
        objects.set_lbl_1 = 0;
        objects.set_val_1 = 0;
        objects.set_row_2 = 0;
        objects.set_lbl_2 = 0;
        objects.set_val_2 = 0;
        objects.set_scroll_track = 0;
        objects.set_scroll_thumb = 0;
        objects.set_foot_desc = 0;
        objects.set_foot_vu_box = 0;
        objects.set_foot_vu_lbl = 0;
        objects.set_foot_vu_bar = 0;
    }
}

void tick_screen_settings(void) {}

void create_screen_about(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.about = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.about_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.about_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.about_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "About");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.about_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.about_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.about_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.about_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.about_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.about_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.about_title = obj;
            lv_obj_set_pos(obj, 4, 17);
            lv_obj_set_size(obj, 152, 16);
            lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Cassini OpenFW v0.7");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.about_storage = obj;
            lv_obj_set_pos(obj, 4, 34);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Storage 58.1 GB \xC2\xB7 18h left");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.about_batt_lbl = obj;
            lv_obj_set_pos(obj, 4, 46);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Battery 3.92 V \xC2\xB7 72%");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.about_footer = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Hold OK Update mode");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_about(void) {
    if (objects.about) {
        lv_obj_delete(objects.about);
        objects.about = 0;
        objects.about_bar = 0;
        objects.about_bar_ic = 0;
        objects.about_bar_l = 0;
        objects.about_bar_sub = 0;
        objects.about_bar_r = 0;
        objects.about_bar_hp = 0;
        objects.about_batt = 0;
        objects.about_batt_fill = 0;
        objects.about_batt_nub = 0;
        objects.about_title = 0;
        objects.about_storage = 0;
        objects.about_batt_lbl = 0;
        objects.about_footer = 0;
    }
}

void tick_screen_about(void) {}

void create_screen_usb(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.usb = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.usb_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.usb_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.usb_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "USB");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.usb_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.usb_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.usb_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.usb_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.usb_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.usb_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.usb_badge = obj;
            lv_obj_set_pos(obj, 4, 18);
            lv_obj_set_size(obj, 30, 26);
            add_style_st_tile(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.usb_badge_lbl = obj;
                    lv_obj_set_pos(obj, 0, 7);
                    lv_obj_set_size(obj, 30, 12);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "USB");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.usb_title = obj;
            lv_obj_set_pos(obj, 40, 15);
            lv_obj_set_size(obj, 116, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "USB disk");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.usb_tx_lbl = obj;
            lv_obj_set_pos(obj, 40, 43);
            lv_obj_set_size(obj, 58, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "\xE2\x96\xB2 14.1 MB");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.usb_rx_lbl = obj;
            lv_obj_set_pos(obj, 100, 43);
            lv_obj_set_size(obj, 58, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "\xE2\x96\xBC 256 KB");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFB020u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.usb_batt_lbl = obj;
            lv_obj_set_pos(obj, 4, 67);
            lv_obj_set_size(obj, 44, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "+ 72%");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.usb_eject_lbl = obj;
            lv_obj_set_pos(obj, 52, 67);
            lv_obj_set_size(obj, 104, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Hold BACK Eject");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_usb(void) {
    if (objects.usb) {
        lv_obj_delete(objects.usb);
        objects.usb = 0;
        objects.usb_bar = 0;
        objects.usb_bar_ic = 0;
        objects.usb_bar_l = 0;
        objects.usb_bar_sub = 0;
        objects.usb_bar_r = 0;
        objects.usb_bar_hp = 0;
        objects.usb_batt = 0;
        objects.usb_batt_fill = 0;
        objects.usb_batt_nub = 0;
        objects.usb_badge = 0;
        objects.usb_badge_lbl = 0;
        objects.usb_title = 0;
        objects.usb_tx_lbl = 0;
        objects.usb_rx_lbl = 0;
        objects.usb_batt_lbl = 0;
        objects.usb_eject_lbl = 0;
    }
}

void tick_screen_usb(void) {}

void create_screen_bt(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.bt = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.bt_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.bt_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.bt_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Bluetooth sync");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.bt_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.bt_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.bt_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.bt_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.bt_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.bt_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.bt_badge = obj;
            lv_obj_set_pos(obj, 4, 18);
            lv_obj_set_size(obj, 26, 26);
            add_style_st_tile(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x131B2Bu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.bt_badge_lbl = obj;
                    lv_obj_set_pos(obj, 0, 5);
                    lv_obj_set_size(obj, 26, 16);
                    lv_obj_set_style_text_font(obj, &ui_font_md, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "BT");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.bt_name_lbl = obj;
            lv_obj_set_pos(obj, 36, 15);
            lv_obj_set_size(obj, 120, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "HW_76");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.bt_l1_lbl = obj;
            lv_obj_set_pos(obj, 36, 41);
            lv_obj_set_size(obj, 120, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Waiting for phone\xE2\x80\xA6");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.bt_l2_lbl = obj;
            lv_obj_set_pos(obj, 36, 53);
            lv_obj_set_size(obj, 120, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Open the Cassini app");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x5D6B82u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 45, LV_ANIM_OFF);
            objects.bt_prog = obj;
            lv_obj_set_pos(obj, 36, 56);
            lv_obj_set_size(obj, 118, 4);
            add_style_st_prog_cyan(obj);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.bt_footer = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "BACK Stop sync");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_bt(void) {
    if (objects.bt) {
        lv_obj_delete(objects.bt);
        objects.bt = 0;
        objects.bt_bar = 0;
        objects.bt_bar_ic = 0;
        objects.bt_bar_l = 0;
        objects.bt_bar_sub = 0;
        objects.bt_bar_r = 0;
        objects.bt_bar_hp = 0;
        objects.bt_batt = 0;
        objects.bt_batt_fill = 0;
        objects.bt_batt_nub = 0;
        objects.bt_badge = 0;
        objects.bt_badge_lbl = 0;
        objects.bt_name_lbl = 0;
        objects.bt_l1_lbl = 0;
        objects.bt_l2_lbl = 0;
        objects.bt_prog = 0;
        objects.bt_footer = 0;
    }
}

void tick_screen_bt(void) {}

void create_screen_charge(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x070B14u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.charge = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_obj_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            objects.chg_bar = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 160, 13);
            add_style_st_bar(obj);
            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            {
                lv_obj_t *parent_obj = obj;
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.chg_bar_ic = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 10, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.chg_bar_l = obj;
                    lv_obj_set_pos(obj, 3, 0);
                    lv_obj_set_size(obj, 90, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "Charging");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.chg_bar_sub = obj;
                    lv_obj_set_pos(obj, 52, 0);
                    lv_obj_set_size(obj, 36, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.chg_bar_r = obj;
                    lv_obj_set_pos(obj, 86, 0);
                    lv_obj_set_size(obj, 52, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "14:32");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_label_create(parent_obj);
                    objects.chg_bar_hp = obj;
                    lv_obj_set_pos(obj, 124, 0);
                    lv_obj_set_size(obj, 16, 13);
                    lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(obj, "HP");
                    lv_obj_set_style_text_color(obj, lv_color_hex(0x42C8FFu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.chg_batt = obj;
                    lv_obj_set_pos(obj, 142, 3);
                    lv_obj_set_size(obj, 14, 7);
                    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x111827u), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_border_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    {
                        lv_obj_t *parent_obj = obj;
                        {
                            lv_obj_t *obj = lv_obj_create(parent_obj);
                            lv_obj_remove_style_all(obj);
                            lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                            objects.chg_batt_fill = obj;
                            lv_obj_set_pos(obj, 1, 1);
                            lv_obj_set_size(obj, 10, 3);
                            lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                            lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                        }
                    }
                }
                {
                    lv_obj_t *obj = lv_obj_create(parent_obj);
                    lv_obj_remove_style_all(obj);
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
                    objects.chg_batt_nub = obj;
                    lv_obj_set_pos(obj, 156, 5);
                    lv_obj_set_size(obj, 2, 3);
                    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.chg_pct_lbl = obj;
            lv_obj_set_pos(obj, 4, 24);
            lv_obj_set_size(obj, 152, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "+ 72%");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x3DDC84u), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.chg_footer = obj;
            lv_obj_set_pos(obj, 0, 67);
            lv_obj_set_size(obj, 160, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Any key Use the device");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_charge(void) {
    if (objects.charge) {
        lv_obj_delete(objects.charge);
        objects.charge = 0;
        objects.chg_bar = 0;
        objects.chg_bar_ic = 0;
        objects.chg_bar_l = 0;
        objects.chg_bar_sub = 0;
        objects.chg_bar_r = 0;
        objects.chg_bar_hp = 0;
        objects.chg_batt = 0;
        objects.chg_batt_fill = 0;
        objects.chg_batt_nub = 0;
        objects.chg_pct_lbl = 0;
        objects.chg_footer = 0;
    }
}

void tick_screen_charge(void) {}

void create_screen_power(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x000000u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.power = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
    {
        lv_obj_t *parent_obj = obj;
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.pwr_title = obj;
            lv_obj_set_pos(obj, 4, 12);
            lv_obj_set_size(obj, 152, 26);
            lv_obj_set_style_text_font(obj, &ui_font_lg, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Power off");
            lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            lv_obj_t *obj = lv_bar_create(parent_obj);
            lv_obj_remove_style_all(obj);
            lv_bar_set_range(obj, 0, 100);
            lv_bar_set_value(obj, 60, LV_ANIM_OFF);
            objects.pwr_hold_bar = obj;
            lv_obj_set_pos(obj, 24, 43);
            lv_obj_set_size(obj, 112, 6);
            add_style_st_prog_white(obj);
        }
        {
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.pwr_sub = obj;
            lv_obj_set_pos(obj, 4, 65);
            lv_obj_set_size(obj, 152, 12);
            lv_obj_set_style_text_font(obj, &ui_font_sm, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(obj, "Keep holding POWER");
            lv_obj_set_style_text_color(obj, lv_color_hex(0x9CB0CEu), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void delete_screen_power(void) {
    if (objects.power) {
        lv_obj_delete(objects.power);
        objects.power = 0;
        objects.pwr_title = 0;
        objects.pwr_hold_bar = 0;
        objects.pwr_sub = 0;
    }
}

void tick_screen_power(void) {}

void create_screen_off(void) {
    lv_obj_t *obj = lv_obj_create(0);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    add_style_st_screen(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x000000u), LV_PART_MAIN | LV_STATE_DEFAULT);
    objects.off = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 160, 80);
}

void delete_screen_off(void) {
    if (objects.off) {
        lv_obj_delete(objects.off);
        objects.off = 0;
    }
}

void tick_screen_off(void) {}

void create_screen_by_id(enum ScreensEnum screenId) {
    switch (screenId) {
    case SCREEN_ID_HOME: create_screen_home(); break;
    case SCREEN_ID_REC: create_screen_rec(); break;
    case SCREEN_ID_REC_ERR: create_screen_rec_err(); break;
    case SCREEN_ID_SAVED: create_screen_saved(); break;
    case SCREEN_ID_FILES: create_screen_files(); break;
    case SCREEN_ID_FILES_EMPTY: create_screen_files_empty(); break;
    case SCREEN_ID_PLAYER: create_screen_player(); break;
    case SCREEN_ID_PLAYER_TR: create_screen_player_tr(); break;
    case SCREEN_ID_DELETE_CONFIRM: create_screen_delete_confirm(); break;
    case SCREEN_ID_SETTINGS: create_screen_settings(); break;
    case SCREEN_ID_ABOUT: create_screen_about(); break;
    case SCREEN_ID_USB: create_screen_usb(); break;
    case SCREEN_ID_BT: create_screen_bt(); break;
    case SCREEN_ID_CHARGE: create_screen_charge(); break;
    case SCREEN_ID_POWER: create_screen_power(); break;
    case SCREEN_ID_OFF: create_screen_off(); break;
    default: break;
    }
}

void delete_screen_by_id(enum ScreensEnum screenId) {
    switch (screenId) {
    case SCREEN_ID_HOME: delete_screen_home(); break;
    case SCREEN_ID_REC: delete_screen_rec(); break;
    case SCREEN_ID_REC_ERR: delete_screen_rec_err(); break;
    case SCREEN_ID_SAVED: delete_screen_saved(); break;
    case SCREEN_ID_FILES: delete_screen_files(); break;
    case SCREEN_ID_FILES_EMPTY: delete_screen_files_empty(); break;
    case SCREEN_ID_PLAYER: delete_screen_player(); break;
    case SCREEN_ID_PLAYER_TR: delete_screen_player_tr(); break;
    case SCREEN_ID_DELETE_CONFIRM: delete_screen_delete_confirm(); break;
    case SCREEN_ID_SETTINGS: delete_screen_settings(); break;
    case SCREEN_ID_ABOUT: delete_screen_about(); break;
    case SCREEN_ID_USB: delete_screen_usb(); break;
    case SCREEN_ID_BT: delete_screen_bt(); break;
    case SCREEN_ID_CHARGE: delete_screen_charge(); break;
    case SCREEN_ID_POWER: delete_screen_power(); break;
    case SCREEN_ID_OFF: delete_screen_off(); break;
    default: break;
    }
}

void tick_screen(int screen_index) { (void)screen_index; }
void create_screens(void) { create_screen_home(); }
