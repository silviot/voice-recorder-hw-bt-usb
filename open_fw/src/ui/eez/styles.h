#ifndef EEZ_LVGL_UI_STYLES_H
#define EEZ_LVGL_UI_STYLES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void add_style_st_screen(lv_obj_t *obj);
void add_style_st_bar(lv_obj_t *obj);
void add_style_st_tile(lv_obj_t *obj);
void add_style_st_row(lv_obj_t *obj);
void add_style_st_pill(lv_obj_t *obj);
void add_style_st_prog_cyan(lv_obj_t *obj);
void add_style_st_prog_red(lv_obj_t *obj);
void add_style_st_prog_green(lv_obj_t *obj);
void add_style_st_prog_white(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/
