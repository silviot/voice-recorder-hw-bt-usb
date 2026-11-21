#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_HOME = 1,
    SCREEN_ID_REC = 2,
    SCREEN_ID_REC_ERR = 3,
    SCREEN_ID_SAVED = 4,
    SCREEN_ID_FILES = 5,
    SCREEN_ID_FILES_EMPTY = 6,
    SCREEN_ID_PLAYER = 7,
    SCREEN_ID_PLAYER_TR = 8,
    SCREEN_ID_DELETE_CONFIRM = 9,
    SCREEN_ID_SETTINGS = 10,
    SCREEN_ID_ABOUT = 11,
    SCREEN_ID_USB = 12,
    SCREEN_ID_BT = 13,
    SCREEN_ID_CHARGE = 14,
    SCREEN_ID_POWER = 15,
    SCREEN_ID_OFF = 16,
    _SCREEN_ID_LAST = 16,
};

typedef struct _objects_t {
    lv_obj_t *home;
    lv_obj_t *rec;
    lv_obj_t *rec_err;
    lv_obj_t *saved;
    lv_obj_t *files;
    lv_obj_t *files_empty;
    lv_obj_t *player;
    lv_obj_t *player_tr;
    lv_obj_t *delete_confirm;
    lv_obj_t *settings;
    lv_obj_t *about;
    lv_obj_t *usb;
    lv_obj_t *bt;
    lv_obj_t *charge;
    lv_obj_t *power;
    lv_obj_t *off;
    lv_obj_t *home_bar;
    lv_obj_t *home_bar_ic;
    lv_obj_t *home_bar_l;
    lv_obj_t *home_bar_sub;
    lv_obj_t *home_bar_r;
    lv_obj_t *home_bar_hp;
    lv_obj_t *home_batt;
    lv_obj_t *home_batt_fill;
    lv_obj_t *home_batt_nub;
    lv_obj_t *home_tile_0;
    lv_obj_t *home_tile_ic_0;
    lv_obj_t *home_tile_lbl_0;
    lv_obj_t *home_tile_1;
    lv_obj_t *home_tile_ic_1;
    lv_obj_t *home_tile_lbl_1;
    lv_obj_t *home_tile_2;
    lv_obj_t *home_tile_ic_2;
    lv_obj_t *home_tile_lbl_2;
    lv_obj_t *home_tile_3;
    lv_obj_t *home_tile_ic_3;
    lv_obj_t *home_tile_lbl_3;
    lv_obj_t *home_line1;
    lv_obj_t *home_line2;
    lv_obj_t *rec_bar;
    lv_obj_t *rec_bar_ic;
    lv_obj_t *rec_bar_l;
    lv_obj_t *rec_bar_sub;
    lv_obj_t *rec_bar_r;
    lv_obj_t *rec_bar_hp;
    lv_obj_t *rec_batt;
    lv_obj_t *rec_batt_fill;
    lv_obj_t *rec_batt_nub;
    lv_obj_t *rec_timer;
    lv_obj_t *rec_gain_pill;
    lv_obj_t *rec_gain_lbl;
    lv_obj_t *rec_free_lbl;
    lv_obj_t *rec_spec_img;
    lv_obj_t *rec_vml_0;
    lv_obj_t *rec_vml_1;
    lv_obj_t *rec_vml_2;
    lv_obj_t *rec_vml_3;
    lv_obj_t *rec_vml_4;
    lv_obj_t *rec_vml_5;
    lv_obj_t *rec_vml_6;
    lv_obj_t *rec_vml_7;
    lv_obj_t *rec_vmr_0;
    lv_obj_t *rec_vmr_1;
    lv_obj_t *rec_vmr_2;
    lv_obj_t *rec_vmr_3;
    lv_obj_t *rec_vmr_4;
    lv_obj_t *rec_vmr_5;
    lv_obj_t *rec_vmr_6;
    lv_obj_t *rec_vmr_7;
    lv_obj_t *rec_foot_box;
    lv_obj_t *rec_foot_left;
    lv_obj_t *rec_foot_right;
    lv_obj_t *rec_foot_bar;
    lv_obj_t *rec_err_bar;
    lv_obj_t *rec_err_bar_ic;
    lv_obj_t *rec_err_bar_l;
    lv_obj_t *rec_err_bar_sub;
    lv_obj_t *rec_err_bar_r;
    lv_obj_t *rec_err_bar_hp;
    lv_obj_t *rec_err_batt;
    lv_obj_t *rec_err_batt_fill;
    lv_obj_t *rec_err_batt_nub;
    lv_obj_t *rec_err_title;
    lv_obj_t *rec_err_l1;
    lv_obj_t *rec_err_l2;
    lv_obj_t *rec_err_l3;
    lv_obj_t *saved_bar;
    lv_obj_t *saved_bar_ic;
    lv_obj_t *saved_bar_l;
    lv_obj_t *saved_bar_sub;
    lv_obj_t *saved_bar_r;
    lv_obj_t *saved_bar_hp;
    lv_obj_t *saved_batt;
    lv_obj_t *saved_batt_fill;
    lv_obj_t *saved_batt_nub;
    lv_obj_t *saved_dur;
    lv_obj_t *saved_meta;
    lv_obj_t *saved_file;
    lv_obj_t *saved_footer;
    lv_obj_t *saved_prog;
    lv_obj_t *files_bar;
    lv_obj_t *files_bar_ic;
    lv_obj_t *files_bar_l;
    lv_obj_t *files_bar_sub;
    lv_obj_t *files_bar_r;
    lv_obj_t *files_bar_hp;
    lv_obj_t *files_batt;
    lv_obj_t *files_batt_fill;
    lv_obj_t *files_batt_nub;
    lv_obj_t *files_row_0;
    lv_obj_t *files_dot_0;
    lv_obj_t *files_name_0;
    lv_obj_t *files_badge_0;
    lv_obj_t *files_dur_0;
    lv_obj_t *files_row_1;
    lv_obj_t *files_dot_1;
    lv_obj_t *files_name_1;
    lv_obj_t *files_badge_1;
    lv_obj_t *files_dur_1;
    lv_obj_t *files_row_2;
    lv_obj_t *files_dot_2;
    lv_obj_t *files_name_2;
    lv_obj_t *files_badge_2;
    lv_obj_t *files_dur_2;
    lv_obj_t *files_scroll_track;
    lv_obj_t *files_scroll_thumb;
    lv_obj_t *files_footer;
    lv_obj_t *files_e_bar;
    lv_obj_t *files_e_bar_ic;
    lv_obj_t *files_e_bar_l;
    lv_obj_t *files_e_bar_sub;
    lv_obj_t *files_e_bar_r;
    lv_obj_t *files_e_bar_hp;
    lv_obj_t *files_e_batt;
    lv_obj_t *files_e_batt_fill;
    lv_obj_t *files_e_batt_nub;
    lv_obj_t *files_empty_ic;
    lv_obj_t *files_empty_title;
    lv_obj_t *files_empty_foot;
    lv_obj_t *play_bar;
    lv_obj_t *play_bar_ic;
    lv_obj_t *play_bar_l;
    lv_obj_t *play_bar_sub;
    lv_obj_t *play_bar_r;
    lv_obj_t *play_bar_hp;
    lv_obj_t *play_batt;
    lv_obj_t *play_batt_fill;
    lv_obj_t *play_batt_nub;
    lv_obj_t *player_date;
    lv_obj_t *player_vol_pill;
    lv_obj_t *player_vol_lbl;
    lv_obj_t *player_pos;
    lv_obj_t *player_tot;
    lv_obj_t *player_vu_l;
    lv_obj_t *player_vu_r;
    lv_obj_t *player_prog;
    lv_obj_t *player_footer;
    lv_obj_t *ptr_bar;
    lv_obj_t *ptr_bar_ic;
    lv_obj_t *ptr_bar_l;
    lv_obj_t *ptr_bar_sub;
    lv_obj_t *ptr_bar_r;
    lv_obj_t *ptr_bar_hp;
    lv_obj_t *ptr_batt;
    lv_obj_t *ptr_batt_fill;
    lv_obj_t *ptr_batt_nub;
    lv_obj_t *ptr_prog;
    lv_obj_t *ptr_bar_prev;
    lv_obj_t *ptr_prev;
    lv_obj_t *ptr_bar_curr;
    lv_obj_t *ptr_curr_spoken;
    lv_obj_t *ptr_bar_next;
    lv_obj_t *ptr_next;
    lv_obj_t *ptr_foot_box;
    lv_obj_t *ptr_footer;
    lv_obj_t *del_bar;
    lv_obj_t *del_bar_ic;
    lv_obj_t *del_bar_l;
    lv_obj_t *del_bar_sub;
    lv_obj_t *del_bar_r;
    lv_obj_t *del_bar_hp;
    lv_obj_t *del_batt;
    lv_obj_t *del_batt_fill;
    lv_obj_t *del_batt_nub;
    lv_obj_t *del_date;
    lv_obj_t *del_sub;
    lv_obj_t *del_hold_bar;
    lv_obj_t *del_footer;
    lv_obj_t *set_bar;
    lv_obj_t *set_bar_ic;
    lv_obj_t *set_bar_l;
    lv_obj_t *set_bar_sub;
    lv_obj_t *set_bar_r;
    lv_obj_t *set_bar_hp;
    lv_obj_t *set_batt;
    lv_obj_t *set_batt_fill;
    lv_obj_t *set_batt_nub;
    lv_obj_t *set_row_0;
    lv_obj_t *set_lbl_0;
    lv_obj_t *set_val_0;
    lv_obj_t *set_row_1;
    lv_obj_t *set_lbl_1;
    lv_obj_t *set_val_1;
    lv_obj_t *set_row_2;
    lv_obj_t *set_lbl_2;
    lv_obj_t *set_val_2;
    lv_obj_t *set_scroll_track;
    lv_obj_t *set_scroll_thumb;
    lv_obj_t *set_foot_desc;
    lv_obj_t *set_foot_vu_box;
    lv_obj_t *set_foot_vu_lbl;
    lv_obj_t *set_foot_vu_bar;
    lv_obj_t *about_bar;
    lv_obj_t *about_bar_ic;
    lv_obj_t *about_bar_l;
    lv_obj_t *about_bar_sub;
    lv_obj_t *about_bar_r;
    lv_obj_t *about_bar_hp;
    lv_obj_t *about_batt;
    lv_obj_t *about_batt_fill;
    lv_obj_t *about_batt_nub;
    lv_obj_t *about_title;
    lv_obj_t *about_storage;
    lv_obj_t *about_batt_lbl;
    lv_obj_t *about_footer;
    lv_obj_t *usb_bar;
    lv_obj_t *usb_bar_ic;
    lv_obj_t *usb_bar_l;
    lv_obj_t *usb_bar_sub;
    lv_obj_t *usb_bar_r;
    lv_obj_t *usb_bar_hp;
    lv_obj_t *usb_batt;
    lv_obj_t *usb_batt_fill;
    lv_obj_t *usb_batt_nub;
    lv_obj_t *usb_badge;
    lv_obj_t *usb_badge_lbl;
    lv_obj_t *usb_title;
    lv_obj_t *usb_tx_lbl;
    lv_obj_t *usb_rx_lbl;
    lv_obj_t *usb_batt_lbl;
    lv_obj_t *usb_eject_lbl;
    lv_obj_t *bt_bar;
    lv_obj_t *bt_bar_ic;
    lv_obj_t *bt_bar_l;
    lv_obj_t *bt_bar_sub;
    lv_obj_t *bt_bar_r;
    lv_obj_t *bt_bar_hp;
    lv_obj_t *bt_batt;
    lv_obj_t *bt_batt_fill;
    lv_obj_t *bt_batt_nub;
    lv_obj_t *bt_badge;
    lv_obj_t *bt_badge_lbl;
    lv_obj_t *bt_name_lbl;
    lv_obj_t *bt_l1_lbl;
    lv_obj_t *bt_l2_lbl;
    lv_obj_t *bt_prog;
    lv_obj_t *bt_footer;
    lv_obj_t *chg_bar;
    lv_obj_t *chg_bar_ic;
    lv_obj_t *chg_bar_l;
    lv_obj_t *chg_bar_sub;
    lv_obj_t *chg_bar_r;
    lv_obj_t *chg_bar_hp;
    lv_obj_t *chg_batt;
    lv_obj_t *chg_batt_fill;
    lv_obj_t *chg_batt_nub;
    lv_obj_t *chg_pct_lbl;
    lv_obj_t *chg_footer;
    lv_obj_t *pwr_title;
    lv_obj_t *pwr_hold_bar;
    lv_obj_t *pwr_sub;
} objects_t;

extern objects_t objects;

void create_screen_home(void);
void delete_screen_home(void);
void tick_screen_home(void);
void create_screen_rec(void);
void delete_screen_rec(void);
void tick_screen_rec(void);
void create_screen_rec_err(void);
void delete_screen_rec_err(void);
void tick_screen_rec_err(void);
void create_screen_saved(void);
void delete_screen_saved(void);
void tick_screen_saved(void);
void create_screen_files(void);
void delete_screen_files(void);
void tick_screen_files(void);
void create_screen_files_empty(void);
void delete_screen_files_empty(void);
void tick_screen_files_empty(void);
void create_screen_player(void);
void delete_screen_player(void);
void tick_screen_player(void);
void create_screen_player_tr(void);
void delete_screen_player_tr(void);
void tick_screen_player_tr(void);
void create_screen_delete_confirm(void);
void delete_screen_delete_confirm(void);
void tick_screen_delete_confirm(void);
void create_screen_settings(void);
void delete_screen_settings(void);
void tick_screen_settings(void);
void create_screen_about(void);
void delete_screen_about(void);
void tick_screen_about(void);
void create_screen_usb(void);
void delete_screen_usb(void);
void tick_screen_usb(void);
void create_screen_bt(void);
void delete_screen_bt(void);
void tick_screen_bt(void);
void create_screen_charge(void);
void delete_screen_charge(void);
void tick_screen_charge(void);
void create_screen_power(void);
void delete_screen_power(void);
void tick_screen_power(void);
void create_screen_off(void);
void delete_screen_off(void);
void tick_screen_off(void);

void create_screen_by_id(enum ScreensEnum screenId);
void delete_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);
void create_screens(void);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/
