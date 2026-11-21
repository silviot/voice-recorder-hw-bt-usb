#!/usr/bin/env python3
"""
Generate open_fw/cassini.eez-project (EEZ Studio LVGL 9.2.2 visual UI project)
and its matching initial EEZ Studio C code in open_fw/src/ui/eez/.

Once generated, you can open open_fw/cassini.eez-project directly in EEZ Studio
(https://www.envox.eu/studio/studio-introduction/), edit screens visually, and
click Build to regenerate open_fw/src/ui/eez/ — while open_fw/src/ui/ui_screens.c
binds firmware state (ui_model_t) cleanly to EEZ's generated `objects.*` handles.

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import json
import uuid
from pathlib import Path

NS = uuid.UUID("6b3f1a10-4c9e-4c6e-9c0b-51a4c9a10001")


def uid(key: str) -> str:
    return str(uuid.uuid5(NS, key))


# 14-color dark-mode palette
PALETTE = [
    ("bg",      "#070b14"),
    ("bar",     "#111827"),
    ("tile",    "#131b2b"),
    ("pill",    "#1f4e8c"),
    ("pill_hi", "#2a64b0"),
    ("off",     "#1a2232"),
    ("faint",   "#344055"),
    ("mute",    "#5d6b82"),
    ("dim",     "#9cb0ce"),
    ("text",    "#ffffff"),
    ("black",   "#000000"),
    ("cyan",    "#42c8ff"),
    ("red",     "#ff3b3b"),
    ("dred",    "#5a1418"),
    ("amber",   "#ffb020"),
    ("green",   "#3ddc84"),
    ("pink",    "#ff7ac8"),
]
COL = dict(PALETTE)

# Reusable named LVGL styles exported to EEZ Studio
STYLES = [
    ("st_screen", {
        "MAIN": {"DEFAULT": {
            "bg_color": COL["bg"], "bg_opa": 255, "border_width": 0,
            "pad_top": 0, "pad_bottom": 0, "pad_left": 0, "pad_right": 0,
            "text_color": COL["text"],
        }}
    }),
    ("st_bar", {
        "MAIN": {"DEFAULT": {
            "bg_color": COL["bar"], "bg_opa": 255, "border_width": 0, "radius": 0,
            "pad_top": 0, "pad_bottom": 0, "pad_left": 3, "pad_right": 3,
        }}
    }),
    ("st_tile", {
        "MAIN": {"DEFAULT": {
            "bg_color": COL["tile"], "bg_opa": 255, "border_width": 0, "radius": 6,
            "pad_top": 0, "pad_bottom": 0, "pad_left": 0, "pad_right": 0,
        }}
    }),
    ("st_row", {
        "MAIN": {"DEFAULT": {
            "bg_color": COL["pill"], "bg_opa": 0, "border_width": 0, "radius": 8,
            "pad_top": 0, "pad_bottom": 0, "pad_left": 6, "pad_right": 5,
        }}
    }),
    ("st_pill", {
        "MAIN": {"DEFAULT": {
            "bg_color": COL["tile"], "bg_opa": 255, "border_width": 0, "radius": 6,
            "pad_top": 0, "pad_bottom": 0, "pad_left": 5, "pad_right": 5,
        }}
    }),
    ("st_prog_cyan", {
        "MAIN": {"DEFAULT": {"bg_color": COL["off"], "bg_opa": 255, "radius": 9999}},
        "INDICATOR": {"DEFAULT": {"bg_color": COL["cyan"], "bg_opa": 255, "radius": 9999}},
    }),
    ("st_prog_red", {
        "MAIN": {"DEFAULT": {"bg_color": COL["off"], "bg_opa": 255, "radius": 9999}},
        "INDICATOR": {"DEFAULT": {"bg_color": COL["red"], "bg_opa": 255, "radius": 9999}},
    }),
    ("st_prog_green", {
        "MAIN": {"DEFAULT": {"bg_color": COL["off"], "bg_opa": 255, "radius": 9999}},
        "INDICATOR": {"DEFAULT": {"bg_color": COL["green"], "bg_opa": 255, "radius": 9999}},
    }),
    ("st_prog_white", {
        "MAIN": {"DEFAULT": {"bg_color": COL["off"], "bg_opa": 255, "radius": 9999}},
        "INDICATOR": {"DEFAULT": {"bg_color": COL["text"], "bg_opa": 255, "radius": 9999}},
    }),
]


def w_base(wtype: str, key: str, ident: str, x: int, y: int, w: int, h: int,
           style_name: str = "default", local_def: dict | None = None,
           children: list | None = None, extra: dict | None = None) -> dict:
    obj: dict = {
        "objID": uid(f"w:{key}"),
        "type": wtype,
        "left": x,
        "top": y,
        "width": w,
        "height": h,
        "leftUnit": "px",
        "topUnit": "px",
        "widthUnit": "px",
        "heightUnit": "px",
        "customInputs": [],
        "customOutputs": [],
        "style": {
            "objID": uid(f"ws:{key}"),
            "useStyle": style_name,
            "conditionalStyles": [],
            "childStyles": [],
        },
        "timeline": [],
        "eventHandlers": [],
        "children": children or [],
        "flags": "",
        "hiddenFlagType": "literal",
        "clickableFlagType": "literal",
        "scrollbarMode": "off",
        "scrollDirection": "none",
        "checkedStateType": "literal",
        "disabledStateType": "literal",
        "states": "",
        "localStyles": {"objID": uid(f"wls:{key}")},
    }
    if ident:
        obj["identifier"] = ident
    if local_def:
        obj["localStyles"]["definition"] = local_def
    if extra:
        obj.update(extra)
    return obj


def w_panel(key: str, ident: str, x: int, y: int, w: int, h: int,
            style_name: str = "default", bg: str | None = None, bg_opa: int | None = None,
            radius: int | None = None, border_w: int = 0, border_col: str | None = None,
            children: list | None = None) -> dict:
    d: dict = {"border_width": border_w, "pad_top": 0, "pad_bottom": 0, "pad_left": 0, "pad_right": 0}
    if bg is not None:
        d["bg_color"] = bg
    if bg_opa is not None:
        d["bg_opa"] = bg_opa
    if radius is not None:
        d["radius"] = radius
    if border_col is not None:
        d["border_color"] = border_col
    return w_base("LVGLPanelWidget", key, ident, x, y, w, h, style_name, {"MAIN": {"DEFAULT": d}}, children)


def w_label(key: str, ident: str, x: int, y: int, w: int, h: int, text: str,
            font: str = "ui_font_sm", color: str = COL["text"],
            align: str = "LEFT", long_mode: str = "CLIP",
            bg: str | None = None, radius: int | None = None) -> dict:
    d: dict = {"text_color": color, "text_align": align}
    if bg is not None:
        d["bg_color"] = bg
        d["bg_opa"] = 255
    if radius is not None:
        d["radius"] = radius
    return w_base(
        "LVGLLabelWidget", key, ident, x, y, w, h, "default",
        {"MAIN": {"DEFAULT": d}},
        extra={
            "text": text,
            "textType": "literal",
            "longMode": long_mode,
            "recolor": False,
            "_font": font,  # used by C codegen
        },
    )


def w_bar(key: str, ident: str, x: int, y: int, w: int, h: int,
          style_name: str = "st_prog_cyan", vmin: int = 0, vmax: int = 100, val: int = 50) -> dict:
    return w_base(
        "LVGLBarWidget", key, ident, x, y, w, h, style_name,
        extra={
            "min": vmin, "max": vmax, "mode": "NORMAL",
            "value": val, "valueType": "literal",
            "valueStart": 0, "valueStartType": "literal",
            "enableAnimation": False,
        },
    )


def w_image(key: str, ident: str, x: int, y: int, w: int, h: int) -> dict:
    return w_base(
        "LVGLImageWidget", key, ident, x, y, w, h, "default",
        {"MAIN": {"DEFAULT": {"bg_color": COL["black"], "bg_opa": 255}}},
    )


def make_status_bar(pfx: str, left_txt: str = "", right_txt: str = "14:32",
                    bar_bg: str = COL["bar"], fg: str = COL["text"]) -> dict:
    return w_panel(
        f"{pfx}_bar", f"{pfx}_bar", 0, 0, 160, 13, "st_bar",
        bg=bar_bg, bg_opa=255, radius=0,
        children=[
            w_label(f"{pfx}_bar_ic", f"{pfx}_bar_ic", 3, 0, 10, 13, "", "ui_font_sm", fg),
            w_label(f"{pfx}_bar_l", f"{pfx}_bar_l", 3, 0, 90, 13, left_txt, "ui_font_sm", fg),
            w_label(f"{pfx}_bar_sub", f"{pfx}_bar_sub", 52, 0, 36, 13, "", "ui_font_sm", COL["dim"]),
            w_label(f"{pfx}_bar_r", f"{pfx}_bar_r", 86, 0, 52, 13, right_txt, "ui_font_sm", COL["dim"], "RIGHT"),
            w_label(f"{pfx}_bar_hp", f"{pfx}_bar_hp", 124, 0, 16, 13, "HP", "ui_font_sm", COL["cyan"]),
            w_panel(
                f"{pfx}_batt", f"{pfx}_batt", 142, 3, 14, 7, "default",
                bg=bar_bg, bg_opa=0, radius=1, border_w=1, border_col=COL["dim"],
                children=[
                    w_panel(f"{pfx}_batt_fill", f"{pfx}_batt_fill", 1, 1, 10, 3, "default",
                            bg=COL["green"], bg_opa=255, radius=0),
                ],
            ),
            w_panel(f"{pfx}_batt_nub", f"{pfx}_batt_nub", 156, 5, 2, 3, "default",
                    bg=COL["dim"], bg_opa=255, radius=0),
        ],
    )


def make_page(name: str, bg: str, components: list[dict]) -> dict:
    return {
        "objID": uid(f"page:{name}"),
        "name": name,
        "left": 0,
        "top": 0,
        "width": 160,
        "height": 80,
        "components": components,
        "connectionLines": [],
        "localVariables": [],
        "lvglLocalStyles": {
            "objID": uid(f"pls:{name}"),
            "definition": {"MAIN": {"DEFAULT": {"bg_color": bg, "bg_opa": 255}}},
        },
    }


def build_pages() -> list[dict]:
    pages: list[dict] = []

    # 1. Home
    home_tiles = []
    icons = ["\u25b6", "=", "*", "BT"]
    labels = ["Last", "Files", "Setup", "Sync"]
    for i in range(4):
        tx = 3 + i * 39
        home_tiles.append(w_panel(
            f"home_t{i}", f"home_tile_{i}", tx, 16, 37, 37, "st_tile",
            bg=COL["pill"] if i == 1 else COL["tile"], bg_opa=255, radius=6,
            children=[
                w_label(f"home_t{i}_ic", f"home_tile_ic_{i}", 0, 4, 37, 16, icons[i],
                        "ui_font_md", COL["text"] if i == 1 else COL["dim"], "CENTER"),
                w_label(f"home_t{i}_lb", f"home_tile_lbl_{i}", 0, 22, 37, 13, labels[i],
                        "ui_font_sm", COL["text"] if i == 1 else COL["dim"], "CENTER"),
            ],
        ))
    pages.append(make_page("home", COL["bg"], [
        make_status_bar("home", "Fri 20 Nov", "14:32", COL["bar"], COL["dim"]),
        *home_tiles,
        w_label("home_l1", "home_line1", 3, 55, 154, 12, "4 recordings", "ui_font_sm", COL["text"], "CENTER", "DOT"),
        w_label("home_l2", "home_line2", 3, 67, 154, 12, "18h 20m of space left", "ui_font_sm", COL["dim"], "CENTER", "DOT"),
    ]))

    # 2. Recording (active / VOR wait)
    rec_vm_l = [w_panel(f"rec_vml_{i}", f"rec_vml_{i}", 148, 42 + (7 - i) * 3, 4, 2, "default",
                        bg=COL["off"], bg_opa=255, radius=1) for i in range(8)]
    rec_vm_r = [w_panel(f"rec_vmr_{i}", f"rec_vmr_{i}", 153, 42 + (7 - i) * 3, 4, 2, "default",
                        bg=COL["off"], bg_opa=255, radius=1) for i in range(8)]
    pages.append(make_page("rec", COL["bg"], [
        make_status_bar("rec", "REC", "2ch \u00b7 Raw", COL["red"], COL["text"]),
        w_label("rec_tm", "rec_timer", 3, 14, 90, 26, "00:00", "ui_font_lg", COL["text"]),
        w_panel("rec_gp", "rec_gain_pill", 107, 16, 50, 13, "st_pill", bg=COL["tile"], bg_opa=255, radius=6,
                children=[w_label("rec_gl", "rec_gain_lbl", 0, 1, 50, 11, "Gain 20", "ui_font_sm", COL["text"], "CENTER")]),
        w_label("rec_fr", "rec_free_lbl", 86, 29, 70, 12, "18h 20m left", "ui_font_sm", COL["dim"], "RIGHT"),
        w_image("rec_sp", "rec_spec_img", 2, 42, 144, 24),
        *rec_vm_l,
        *rec_vm_r,
        w_panel("rec_fb", "rec_foot_box", 2, 67, 156, 13, "default", bg=COL["bg"], bg_opa=0, radius=6, children=[
            w_label("rec_fl", "rec_foot_left", 2, 1, 108, 11, "REC_0001.wav", "ui_font_sm", COL["mute"]),
            w_label("rec_frt", "rec_foot_right", 104, 1, 50, 11, "OK Mark", "ui_font_sm", COL["dim"], "RIGHT"),
            w_bar("rec_fbar", "rec_foot_bar", 4, 4, 148, 5, "st_prog_cyan", 0, 31, 20),
        ]),
    ]))

    # 3. Recording Error / Stopped
    pages.append(make_page("rec_err", COL["bg"], [
        make_status_bar("rec_err", "Stopped", "", COL["dred"], COL["text"]),
        w_label("rec_et", "rec_err_title", 4, 15, 152, 26, "Storage error", "ui_font_lg", COL["red"]),
        w_label("rec_e1", "rec_err_l1", 4, 41, 152, 12, "Nothing is being recorded.", "ui_font_sm", COL["text"]),
        w_label("rec_e2", "rec_err_l2", 4, 53, 152, 12, "Slide REC off, then connect", "ui_font_sm", COL["dim"]),
        w_label("rec_e3", "rec_err_l3", 4, 65, 152, 12, "USB to check the storage.", "ui_font_sm", COL["dim"]),
    ]))

    # 4. Saved Card
    pages.append(make_page("saved", COL["bg"], [
        make_status_bar("saved", "Saved", "", COL["green"], COL["black"]),
        w_label("sav_dur", "saved_dur", 4, 14, 152, 26, "12:34", "ui_font_lg", COL["text"], "CENTER"),
        w_label("sav_meta", "saved_meta", 4, 39, 152, 12, "Today 14:32 \u00b7 2ch", "ui_font_sm", COL["text"], "CENTER"),
        w_label("sav_file", "saved_file", 4, 50, 152, 12, "REC_0043.wav", "ui_font_sm", COL["mute"], "CENTER"),
        w_label("sav_foot", "saved_footer", 0, 64, 160, 12, "OK Play it \u00b7 BACK Done", "ui_font_sm", COL["dim"], "CENTER"),
        w_bar("sav_bar", "saved_prog", 0, 77, 160, 3, "st_prog_green", 0, 100, 65),
    ]))

    # 5. Files List
    file_rows = []
    for r in range(3):
        ry = 15 + r * 17
        file_rows.append(w_panel(
            f"files_r{r}", f"files_row_{r}", 2, ry, 151, 16, "st_row",
            bg=COL["pill"], bg_opa=255 if r == 0 else 0, radius=8,
            children=[
                w_panel(f"files_d{r}", f"files_dot_{r}", 5, 6, 4, 4, "default", bg=COL["cyan"], bg_opa=255, radius=9999),
                w_label(f"files_n{r}", f"files_name_{r}", 12, 0, 92, 16, "Today 14:32", "ui_font_md", COL["text"], "LEFT", "DOT"),
                w_label(f"files_b{r}", f"files_badge_{r}", 106, 2, 11, 12, "T", "ui_font_sm", COL["pill"], "CENTER", "CLIP", COL["text"], 3),
                w_label(f"files_du{r}", f"files_dur_{r}", 118, 2, 29, 12, "03:12", "ui_font_sm", COL["dim"], "RIGHT"),
            ],
        ))
    pages.append(make_page("files", COL["bg"], [
        make_status_bar("files", "Files", "14:32"),
        *file_rows,
        w_panel("files_sb", "files_scroll_track", 155, 15, 2, 50, "default", bg=COL["off"], bg_opa=255, radius=1,
                children=[w_panel("files_st", "files_scroll_thumb", 0, 0, 2, 36, "default", bg=COL["dim"], bg_opa=255, radius=1)]),
        w_label("files_ft", "files_footer", 0, 67, 160, 12, "OK Play \u00b7 Hold OK Delete", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 6. Files Empty
    pages.append(make_page("files_empty", COL["bg"], [
        make_status_bar("files_e", "Files", "14:32"),
        w_label("fe_ic", "files_empty_ic", 0, 20, 160, 16, "MIC", "ui_font_md", COL["faint"], "CENTER"),
        w_label("fe_t", "files_empty_title", 0, 38, 160, 16, "No recordings yet", "ui_font_md", COL["text"], "CENTER"),
        w_label("fe_f", "files_empty_foot", 0, 67, 160, 12, "Slide REC to record", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 7. Player (Time-First)
    pages.append(make_page("player", COL["bg"], [
        make_status_bar("play", "Playing", "14:32", COL["bar"], COL["cyan"]),
        w_label("pl_dt", "player_date", 4, 16, 100, 12, "Today 14:32", "ui_font_sm", COL["text"], "LEFT", "DOT"),
        w_panel("pl_vp", "player_vol_pill", 112, 15, 44, 14, "st_pill", bg=COL["tile"], bg_opa=255, radius=6,
                children=[w_label("pl_vl", "player_vol_lbl", 0, 1, 44, 12, "HP 5.8", "ui_font_sm", COL["dim"], "CENTER")]),
        w_label("pl_pos", "player_pos", 3, 29, 64, 26, "02:00", "ui_font_lg", COL["text"]),
        w_label("pl_tot", "player_tot", 66, 39, 52, 12, "/ 03:12", "ui_font_sm", COL["dim"]),
        w_bar("pl_vlb", "player_vu_l", 122, 37, 34, 3, "st_prog_green", 0, 100, 55),
        w_bar("pl_vrb", "player_vu_r", 122, 43, 34, 3, "st_prog_green", 0, 100, 48),
        w_bar("pl_pb", "player_prog", 6, 56, 148, 5, "st_prog_cyan", 0, 100, 62),
        w_label("pl_ft", "player_footer", 0, 67, 160, 12, "\u25c0 5s \u00b7 OK Pause \u00b7 \u25b6 10s", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 8. Player (Transcript)
    pages.append(make_page("player_tr", COL["bg"], [
        make_status_bar("ptr", "05:49", "14:32"),
        w_bar("ptr_pb", "ptr_prog", 0, 13, 160, 2, "st_prog_cyan", 0, 100, 25),
        w_panel("ptr_b1", "ptr_bar_prev", 0, 18, 2, 14, "default", bg=COL["off"], bg_opa=255),
        w_label("ptr_p", "ptr_prev", 6, 17, 152, 16, "Previous line", "ui_font_md", COL["faint"], "LEFT", "DOT"),
        w_panel("ptr_b2", "ptr_bar_curr", 0, 34, 2, 14, "default", bg=COL["cyan"], bg_opa=255),
        w_label("ptr_cs", "ptr_curr_spoken", 6, 33, 152, 16, "Current spoken", "ui_font_md", COL["text"], "LEFT", "DOT"),
        w_panel("ptr_b3", "ptr_bar_next", 0, 50, 2, 14, "default", bg=COL["off"], bg_opa=255),
        w_label("ptr_n", "ptr_next", 6, 49, 152, 16, "Next line", "ui_font_md", COL["faint"], "LEFT", "DOT"),
        w_panel("ptr_fb", "ptr_foot_box", 0, 67, 160, 13, "st_bar", bg=COL["bar"], bg_opa=255,
                children=[w_label("ptr_ft", "ptr_footer", 0, 1, 160, 11, "\u25c0\u25b6 Line \u00b7 OK Pause \u00b7 \u00b1 Vol", "ui_font_sm", COL["dim"], "CENTER")]),
    ]))

    # 9. Delete Confirmation
    pages.append(make_page("delete_confirm", COL["bg"], [
        make_status_bar("del", "Delete?", "", COL["red"], COL["text"]),
        w_label("del_dt", "del_date", 4, 18, 152, 16, "Today 14:32", "ui_font_md", COL["text"], "CENTER"),
        w_label("del_sb", "del_sub", 4, 35, 152, 12, "03:12 \u00b7 REC_0043.wav", "ui_font_sm", COL["dim"], "CENTER"),
        w_bar("del_hb", "del_hold_bar", 10, 52, 140, 7, "st_prog_red", 0, 100, 55),
        w_label("del_ft", "del_footer", 0, 67, 160, 12, "Hold OK Delete \u00b7 BACK Keep", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 10. Settings
    set_rows = []
    for r in range(3):
        ry = 15 + r * 17
        set_rows.append(w_panel(
            f"set_r{r}", f"set_row_{r}", 2, ry, 151, 16, "st_row",
            bg=COL["pill"], bg_opa=255 if r == 0 else 0, radius=8,
            children=[
                w_label(f"set_l{r}", f"set_lbl_{r}", 6, 0, 86, 16, "Mic gain", "ui_font_md", COL["text"]),
                w_label(f"set_v{r}", f"set_val_{r}", 92, 2, 54, 12, "- 20 +", "ui_font_sm", COL["cyan"], "RIGHT"),
            ],
        ))
    pages.append(make_page("settings", COL["bg"], [
        make_status_bar("set", "Settings", "14:32"),
        *set_rows,
        w_panel("set_sb", "set_scroll_track", 155, 15, 2, 50, "default", bg=COL["off"], bg_opa=255, radius=1,
                children=[w_panel("set_st", "set_scroll_thumb", 0, 0, 2, 15, "default", bg=COL["dim"], bg_opa=255, radius=1)]),
        w_label("set_fd", "set_foot_desc", 3, 67, 154, 12, "", "ui_font_sm", COL["dim"], "CENTER"),
        w_panel("set_fvb", "set_foot_vu_box", 4, 67, 152, 12, "default", bg=COL["bg"], bg_opa=0, children=[
            w_label("set_fvl", "set_foot_vu_lbl", 0, 0, 30, 12, "Input", "ui_font_sm", COL["dim"]),
            w_bar("set_fvbar", "set_foot_vu_bar", 32, 3, 120, 5, "st_prog_green", 0, 100, 35),
        ]),
    ]))

    # 11. About
    pages.append(make_page("about", COL["bg"], [
        make_status_bar("about", "About", "14:32"),
        w_label("ab_t", "about_title", 4, 17, 152, 16, "Cassini OpenFW v0.7", "ui_font_md", COL["text"]),
        w_label("ab_st", "about_storage", 4, 34, 152, 12, "Storage 58.1 GB \u00b7 18h left", "ui_font_sm", COL["dim"]),
        w_label("ab_bt", "about_batt_lbl", 4, 46, 152, 12, "Battery 3.92 V \u00b7 72%", "ui_font_sm", COL["dim"]),
        w_label("ab_ft", "about_footer", 0, 67, 160, 12, "Hold OK Update mode", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 12. USB Disk
    pages.append(make_page("usb", COL["bg"], [
        make_status_bar("usb", "USB", "", COL["bar"], COL["cyan"]),
        w_panel("usb_bg", "usb_badge", 4, 18, 30, 26, "st_tile", bg=COL["tile"], bg_opa=255, radius=6,
                children=[w_label("usb_bl", "usb_badge_lbl", 0, 7, 30, 12, "USB", "ui_font_sm", COL["cyan"], "CENTER")]),
        w_label("usb_t", "usb_title", 40, 15, 116, 26, "USB disk", "ui_font_lg", COL["text"]),
        w_label("usb_tx", "usb_tx_lbl", 40, 43, 58, 12, "\u25b2 14.1 MB", "ui_font_sm", COL["cyan"]),
        w_label("usb_rx", "usb_rx_lbl", 100, 43, 58, 12, "\u25bc 256 KB", "ui_font_sm", COL["amber"]),
        w_label("usb_bp", "usb_batt_lbl", 4, 67, 44, 12, "+ 72%", "ui_font_sm", COL["green"]),
        w_label("usb_ej", "usb_eject_lbl", 52, 67, 104, 12, "Hold BACK Eject", "ui_font_sm", COL["dim"], "RIGHT"),
    ]))

    # 13. Bluetooth Sync
    pages.append(make_page("bt", COL["bg"], [
        make_status_bar("bt", "Bluetooth sync", "14:32"),
        w_panel("bt_bg", "bt_badge", 4, 18, 26, 26, "st_tile", bg=COL["tile"], bg_opa=255, radius=6,
                children=[w_label("bt_bl", "bt_badge_lbl", 0, 5, 26, 16, "BT", "ui_font_md", COL["cyan"], "CENTER")]),
        w_label("bt_nm", "bt_name_lbl", 36, 15, 120, 26, "HW_76", "ui_font_lg", COL["text"]),
        w_label("bt_l1", "bt_l1_lbl", 36, 41, 120, 12, "Waiting for phone\u2026", "ui_font_sm", COL["dim"]),
        w_label("bt_l2", "bt_l2_lbl", 36, 53, 120, 12, "Open the Cassini app", "ui_font_sm", COL["mute"]),
        w_bar("bt_pb", "bt_prog", 36, 56, 118, 4, "st_prog_cyan", 0, 100, 45),
        w_label("bt_ft", "bt_footer", 0, 67, 160, 12, "BACK Stop sync", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 14. Charge
    pages.append(make_page("charge", COL["bg"], [
        make_status_bar("chg", "Charging", "14:32", COL["bar"], COL["green"]),
        w_label("chg_p", "chg_pct_lbl", 4, 24, 152, 26, "+ 72%", "ui_font_lg", COL["green"], "CENTER"),
        w_label("chg_f", "chg_footer", 0, 67, 160, 12, "Any key Use the device", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 15. Power Off
    pages.append(make_page("power", COL["black"], [
        w_label("pwr_t", "pwr_title", 4, 12, 152, 26, "Power off", "ui_font_lg", COL["text"], "CENTER"),
        w_bar("pwr_b", "pwr_hold_bar", 24, 43, 112, 6, "st_prog_white", 0, 100, 60),
        w_label("pwr_s", "pwr_sub", 4, 65, 152, 12, "Keep holding POWER", "ui_font_sm", COL["dim"], "CENTER"),
    ]))

    # 16. Off (blank black screen)
    pages.append(make_page("off", COL["black"], []))

    return pages


UI_C_TEMPLATE = """#include "ui.h"
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
            ? LV_SCR_LOAD_ANIM_OVER_LEFT
            : LV_SCR_LOAD_ANIM_OVER_RIGHT;
        lv_screen_load_anim(screen, anim, 180u, 0u, true);
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
"""


def build_eez_project_json(pages: list[dict]) -> dict:
    lvgl_styles = []
    for name, definition in STYLES:
        lvgl_styles.append({
            "objID": uid(f"style:{name}"),
            "name": name,
            "definition": definition,
        })

    # Strip private _font helper key from EEZ JSON
    def clean(obj):
        if isinstance(obj, dict):
            return {k: clean(v) for k, v in obj.items() if not k.startswith("_")}
        if isinstance(obj, list):
            return [clean(x) for x in obj]
        return obj

    return {
        "objID": uid("project:cassini"),
        "lvglStyles": {
            "objID": uid("lvglStyles"),
            "styles": lvgl_styles,
            "defaultStyles": {},
        },
        "settings": {
            "objID": uid("settings"),
            "general": {
                "objID": uid("settings.general"),
                "projectVersion": "v3",
                "projectType": "lvgl",
                "lvglVersion": "9.2.2",
                "extensions": [],
                "imports": [],
                "flowSupport": False,
                "displayWidth": 160,
                "displayHeight": 80,
                "colorFormat": "RGB",
                "resourceFiles": [],
            },
            "build": {
                "objID": uid("settings.build"),
                "configurations": [{"objID": uid("cfg:Default"), "name": "Default"}],
                "files": [
                    {
                        "objID": uid("file:screens.h"),
                        "fileName": "screens.h",
                        "template": "#ifndef EEZ_LVGL_UI_SCREENS_H\n#define EEZ_LVGL_UI_SCREENS_H\n\n//${eez-studio LVGL_INCLUDE}\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n//${eez-studio LVGL_SCREENS_DECL}\n//${eez-studio LVGL_SCREENS_DECL_EXT}\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_SCREENS_H*/\n",
                    },
                    {
                        "objID": uid("file:screens.c"),
                        "fileName": "screens.c",
                        "template": "#include \"screens.h\"\n#include \"images.h\"\n#include \"fonts.h\"\n#include \"actions.h\"\n#include \"vars.h\"\n#include \"styles.h\"\n#include \"ui.h\"\n\n//${eez-studio LVGL_SCREENS_DEF}\n//${eez-studio LVGL_SCREENS_DEF_EXT}\n",
                    },
                    {
                        "objID": uid("file:actions.h"),
                        "fileName": "actions.h",
                        "template": "#ifndef EEZ_LVGL_UI_ACTIONS_H\n#define EEZ_LVGL_UI_ACTIONS_H\n\n//${eez-studio LVGL_INCLUDE}\n\n//${eez-studio LVGL_ACTIONS_DECL}\n\n#endif /*EEZ_LVGL_UI_ACTIONS_H*/\n",
                    },
                    {
                        "objID": uid("file:vars.h"),
                        "fileName": "vars.h",
                        "template": "#ifndef EEZ_LVGL_UI_VARS_H\n#define EEZ_LVGL_UI_VARS_H\n\n#include <stdint.h>\n\n//${eez-studio LVGL_VARS_DECL}\n\n#endif /*EEZ_LVGL_UI_VARS_H*/\n",
                    },
                    {
                        "objID": uid("file:images.h"),
                        "fileName": "images.h",
                        "template": "#ifndef EEZ_LVGL_UI_IMAGES_H\n#define EEZ_LVGL_UI_IMAGES_H\n\n//${eez-studio LVGL_INCLUDE}\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n//${eez-studio LVGL_IMAGES_DECL}\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_IMAGES_H*/\n",
                    },
                    {
                        "objID": uid("file:images.c"),
                        "fileName": "images.c",
                        "template": "#include \"images.h\"\n\n//${eez-studio LVGL_IMAGES_DEF}\n",
                    },
                    {
                        "objID": uid("file:fonts.h"),
                        "fileName": "fonts.h",
                        "template": "#ifndef EEZ_LVGL_UI_FONTS_H\n#define EEZ_LVGL_UI_FONTS_H\n\n//${eez-studio LVGL_INCLUDE}\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n//${eez-studio LVGL_FONTS_DECL}\nLV_FONT_DECLARE(ui_font_sm);\nLV_FONT_DECLARE(ui_font_md);\nLV_FONT_DECLARE(ui_font_lg);\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_FONTS_H*/\n",
                    },
                    {
                        "objID": uid("file:styles.h"),
                        "fileName": "styles.h",
                        "template": "#ifndef EEZ_LVGL_UI_STYLES_H\n#define EEZ_LVGL_UI_STYLES_H\n\n//${eez-studio LVGL_INCLUDE}\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n//${eez-studio LVGL_STYLES_DECL}\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_STYLES_H*/\n",
                    },
                    {
                        "objID": uid("file:styles.c"),
                        "fileName": "styles.c",
                        "template": "#include \"styles.h\"\n#include \"images.h\"\n#include \"fonts.h\"\n\n//${eez-studio LVGL_STYLES_DEF}\n",
                    },
                    {
                        "objID": uid("file:ui.h"),
                        "fileName": "ui.h",
                        "template": "#ifndef EEZ_LVGL_UI_GUI_H\n#define EEZ_LVGL_UI_GUI_H\n\n//${eez-studio LVGL_INCLUDE}\n#include \"screens.h\"\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\nvoid ui_init(void);\nvoid ui_tick(void);\nvoid loadScreen(enum ScreensEnum screenId);\n\n#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_GUI_H*/\n",
                    },
                    {
                        "objID": uid("file:ui.c"),
                        "fileName": "ui.c",
                        "template": UI_C_TEMPLATE,
                    },
                ],
                "destinationFolder": "src/ui/eez",
                "lvglInclude": "lvgl.h",
            },
        },
        "variables": {
            "objID": uid("variables"),
            "globalVariables": [],
            "structures": [],
            "enums": [],
        },
        "actions": [],
        "userPages": clean(pages),
        "userWidgets": [],
        "fonts": [],
        "bitmaps": [],
        "colors": [{"objID": uid(f"col:{name}"), "name": name} for name, _ in PALETTE],
        "themes": [{
            "objID": uid("theme:dark"),
            "name": "Dark",
            "colors": [hx for _, hx in PALETTE],
        }],
    }


def hex_to_u32(hx: str) -> str:
    return "0x" + hx.lstrip("#").upper() + "u"


def c_escape(s: str) -> str:
    out = []
    for b in s.encode("utf-8"):
        if b == ord("\\"):
            out.append("\\\\")
        elif b == ord('"'):
            out.append('\\"')
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            out.append(f"\\x{b:02X}")
    return "".join(out)


def emit_eez_c_files(pages: list[dict], out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    # Collect all identifiers in EEZ Studio order (screens first, then widgets per screen)
    screen_idents = [p["name"] for p in pages]
    widget_idents: list[str] = []

    def walk(nodes: list[dict]):
        for n in nodes:
            if n.get("identifier"):
                widget_idents.append(n["identifier"])
            walk(n.get("children", []))

    for p in pages:
        walk(p["components"])

    all_idents = screen_idents + widget_idents

    # 1. fonts.h / images.h / images.c / actions.h / vars.h / ui.h / ui.c
    (out_dir / "fonts.h").write_text(
        "#ifndef EEZ_LVGL_UI_FONTS_H\n#define EEZ_LVGL_UI_FONTS_H\n\n"
        "#include <lvgl.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        "LV_FONT_DECLARE(ui_font_sm);\nLV_FONT_DECLARE(ui_font_md);\nLV_FONT_DECLARE(ui_font_lg);\n\n"
        "#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_FONTS_H*/\n",
        encoding="utf-8",
    )
    (out_dir / "images.h").write_text(
        "#ifndef EEZ_LVGL_UI_IMAGES_H\n#define EEZ_LVGL_UI_IMAGES_H\n\n"
        "#include <lvgl.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        "typedef struct _ext_img_desc_t {\n    const char *name;\n    const lv_img_dsc_t *img_dsc;\n} ext_img_desc_t;\n\n"
        "extern const ext_img_desc_t images[1];\n\n"
        "#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_IMAGES_H*/\n",
        encoding="utf-8",
    )
    (out_dir / "images.c").write_text(
        "#include \"images.h\"\n\nconst ext_img_desc_t images[1] = { { 0, 0 } };\n",
        encoding="utf-8",
    )
    (out_dir / "actions.h").write_text(
        "#ifndef EEZ_LVGL_UI_ACTIONS_H\n#define EEZ_LVGL_UI_ACTIONS_H\n\n#include <lvgl.h>\n\n#endif /*EEZ_LVGL_UI_ACTIONS_H*/\n",
        encoding="utf-8",
    )
    (out_dir / "vars.h").write_text(
        "#ifndef EEZ_LVGL_UI_VARS_H\n#define EEZ_LVGL_UI_VARS_H\n\n#include <stdint.h>\n\n#endif /*EEZ_LVGL_UI_VARS_H*/\n",
        encoding="utf-8",
    )
    (out_dir / "ui.h").write_text(
        "#ifndef EEZ_LVGL_UI_GUI_H\n#define EEZ_LVGL_UI_GUI_H\n\n"
        "#include <lvgl.h>\n#include \"screens.h\"\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        "void ui_init(void);\nvoid ui_tick(void);\nvoid loadScreen(enum ScreensEnum screenId);\n\n"
        "#ifdef __cplusplus\n}\n#endif\n\n#endif /*EEZ_LVGL_UI_GUI_H*/\n",
        encoding="utf-8",
    )
    (out_dir / "ui.c").write_text(UI_C_TEMPLATE, encoding="utf-8")

    # 2. styles.h & styles.c
    sh_lines = [
        "#ifndef EEZ_LVGL_UI_STYLES_H", "#define EEZ_LVGL_UI_STYLES_H", "",
        "#include <lvgl.h>", "", "#ifdef __cplusplus", 'extern "C" {', "#endif", "",
    ]
    sc_lines = ['#include "styles.h"', '#include "images.h"', '#include "fonts.h"', ""]
    for name, definition in STYLES:
        sh_lines.append(f"void add_style_{name}(lv_obj_t *obj);")
        sc_lines.append(f"static lv_style_t *{name}_main_default(void) {{")
        sc_lines.append("    static lv_style_t style;")
        sc_lines.append("    static int inited = 0;")
        sc_lines.append("    if (!inited) {")
        sc_lines.append("        inited = 1;")
        sc_lines.append("        lv_style_init(&style);")
        for prop, val in definition.get("MAIN", {}).get("DEFAULT", {}).items():
            if prop == "bg_color":
                sc_lines.append(f"        lv_style_set_bg_color(&style, lv_color_hex({hex_to_u32(val)}));")
            elif prop == "text_color":
                sc_lines.append(f"        lv_style_set_text_color(&style, lv_color_hex({hex_to_u32(val)}));")
            elif prop == "radius":
                sc_lines.append(f"        lv_style_set_radius(&style, {'LV_RADIUS_CIRCLE' if val >= 9999 else val});")
            else:
                sc_lines.append(f"        lv_style_set_{prop}(&style, {val});")
        sc_lines.append("    }")
        sc_lines.append("    return &style;")
        sc_lines.append("}")
        if "INDICATOR" in definition:
            sc_lines.append(f"static lv_style_t *{name}_ind_default(void) {{")
            sc_lines.append("    static lv_style_t style;")
            sc_lines.append("    static int inited = 0;")
            sc_lines.append("    if (!inited) {")
            sc_lines.append("        inited = 1;")
            sc_lines.append("        lv_style_init(&style);")
            for prop, val in definition["INDICATOR"]["DEFAULT"].items():
                if prop == "bg_color":
                    sc_lines.append(f"        lv_style_set_bg_color(&style, lv_color_hex({hex_to_u32(val)}));")
                elif prop == "radius":
                    sc_lines.append(f"        lv_style_set_radius(&style, {'LV_RADIUS_CIRCLE' if val >= 9999 else val});")
                else:
                    sc_lines.append(f"        lv_style_set_{prop}(&style, {val});")
            sc_lines.append("    }")
            sc_lines.append("    return &style;")
            sc_lines.append("}")
        sc_lines.append(f"void add_style_{name}(lv_obj_t *obj) {{")
        sc_lines.append(f"    lv_obj_add_style(obj, {name}_main_default(), LV_PART_MAIN | LV_STATE_DEFAULT);")
        if "INDICATOR" in definition:
            sc_lines.append(f"    lv_obj_add_style(obj, {name}_ind_default(), LV_PART_INDICATOR | LV_STATE_DEFAULT);")
        sc_lines.append("}")
        sc_lines.append("")

    sh_lines += ["", "#ifdef __cplusplus", "}", "#endif", "", "#endif /*EEZ_LVGL_UI_STYLES_H*/", ""]
    (out_dir / "styles.h").write_text("\n".join(sh_lines), encoding="utf-8")
    (out_dir / "styles.c").write_text("\n".join(sc_lines), encoding="utf-8")

    # 3. screens.h
    scr_h = [
        "#ifndef EEZ_LVGL_UI_SCREENS_H", "#define EEZ_LVGL_UI_SCREENS_H", "",
        "#include <lvgl.h>", "", "#ifdef __cplusplus", 'extern "C" {', "#endif", "",
        "enum ScreensEnum {",
        "    _SCREEN_ID_FIRST = 1,",
    ]
    for i, p in enumerate(pages, 1):
        scr_h.append(f"    SCREEN_ID_{p['name'].upper()} = {i},")
    scr_h.append(f"    _SCREEN_ID_LAST = {len(pages)},")
    scr_h.append("};")
    scr_h.append("")
    scr_h.append("typedef struct _objects_t {")
    for ident in all_idents:
        scr_h.append(f"    lv_obj_t *{ident};")
    scr_h.append("} objects_t;")
    scr_h.append("")
    scr_h.append("extern objects_t objects;")
    scr_h.append("")
    for p in pages:
        n = p["name"]
        scr_h.append(f"void create_screen_{n}(void);")
        scr_h.append(f"void delete_screen_{n}(void);")
        scr_h.append(f"void tick_screen_{n}(void);")
    scr_h += [
        "",
        "void create_screen_by_id(enum ScreensEnum screenId);",
        "void delete_screen_by_id(enum ScreensEnum screenId);",
        "void tick_screen(int screen_index);",
        "void create_screens(void);",
        "", "#ifdef __cplusplus", "}", "#endif", "", "#endif /*EEZ_LVGL_UI_SCREENS_H*/", "",
    ]
    (out_dir / "screens.h").write_text("\n".join(scr_h), encoding="utf-8")

    # 4. screens.c
    scr_c = [
        '#include "screens.h"', '#include "images.h"', '#include "fonts.h"',
        '#include "actions.h"', '#include "vars.h"', '#include "styles.h"', '#include "ui.h"', "",
        "objects_t objects;", "",
    ]

    def emit_widget(w: dict, parent_var: str, lines: list[str], depth: int = 1) -> None:
        ind = "    " * depth
        wtype = w["type"]
        ident = w.get("identifier", "")
        lines.append(f"{ind}{{")
        ind2 = "    " * (depth + 1)
        if wtype == "LVGLPanelWidget":
            lines.append(f"{ind2}lv_obj_t *obj = lv_obj_create({parent_var});")
            lines.append(f"{ind2}lv_obj_remove_style_all(obj);")
            lines.append(f"{ind2}lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);")
        elif wtype == "LVGLLabelWidget":
            lines.append(f"{ind2}lv_obj_t *obj = lv_label_create({parent_var});")
        elif wtype == "LVGLBarWidget":
            lines.append(f"{ind2}lv_obj_t *obj = lv_bar_create({parent_var});")
            lines.append(f"{ind2}lv_obj_remove_style_all(obj);")
            lines.append(f"{ind2}lv_bar_set_range(obj, {w['min']}, {w['max']});")
            lines.append(f"{ind2}lv_bar_set_value(obj, {w['value']}, LV_ANIM_OFF);")
        elif wtype == "LVGLImageWidget":
            lines.append(f"{ind2}lv_obj_t *obj = lv_image_create({parent_var});")
        else:
            lines.append(f"{ind2}lv_obj_t *obj = lv_obj_create({parent_var});")

        if ident:
            lines.append(f"{ind2}objects.{ident} = obj;")

        lines.append(f"{ind2}lv_obj_set_pos(obj, {w['left']}, {w['top']});")
        lines.append(f"{ind2}lv_obj_set_size(obj, {w['width']}, {w['height']});")

        st_name = w["style"]["useStyle"]
        if st_name and st_name != "default":
            lines.append(f"{ind2}add_style_{st_name}(obj);")

        if wtype == "LVGLLabelWidget":
            font = w.get("_font", "ui_font_sm")
            lines.append(f"{ind2}lv_obj_set_style_text_font(obj, &{font}, LV_PART_MAIN | LV_STATE_DEFAULT);")
            if w.get("longMode") == "DOT":
                lines.append(f"{ind2}lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);")
            lines.append(f'{ind2}lv_label_set_text(obj, "{c_escape(w["text"])}");')

        ldef = w.get("localStyles", {}).get("definition", {}).get("MAIN", {}).get("DEFAULT", {})
        for prop, val in ldef.items():
            if prop == "bg_color":
                lines.append(f"{ind2}lv_obj_set_style_bg_color(obj, lv_color_hex({hex_to_u32(val)}), LV_PART_MAIN | LV_STATE_DEFAULT);")
            elif prop == "border_color":
                lines.append(f"{ind2}lv_obj_set_style_border_color(obj, lv_color_hex({hex_to_u32(val)}), LV_PART_MAIN | LV_STATE_DEFAULT);")
            elif prop == "text_color":
                lines.append(f"{ind2}lv_obj_set_style_text_color(obj, lv_color_hex({hex_to_u32(val)}), LV_PART_MAIN | LV_STATE_DEFAULT);")
            elif prop == "text_align":
                lines.append(f"{ind2}lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_{val}, LV_PART_MAIN | LV_STATE_DEFAULT);")
            elif prop == "radius":
                r = "LV_RADIUS_CIRCLE" if val >= 9999 else str(val)
                lines.append(f"{ind2}lv_obj_set_style_radius(obj, {r}, LV_PART_MAIN | LV_STATE_DEFAULT);")
            elif prop in ("bg_opa", "border_width", "pad_top", "pad_bottom", "pad_left", "pad_right"):
                lines.append(f"{ind2}lv_obj_set_style_{prop}(obj, {val}, LV_PART_MAIN | LV_STATE_DEFAULT);")

        if w.get("children"):
            lines.append(f"{ind2}{{")
            lines.append(f"{ind2}    lv_obj_t *parent_obj = obj;")
            for ch in w["children"]:
                emit_widget(ch, "parent_obj", lines, depth + 2)
            lines.append(f"{ind2}}}")
        lines.append(f"{ind}}}")

    for p in pages:
        n = p["name"]
        bg = p["lvglLocalStyles"]["definition"]["MAIN"]["DEFAULT"]["bg_color"]
        scr_c.append(f"void create_screen_{n}(void) {{")
        scr_c.append("    lv_obj_t *obj = lv_obj_create(0);")
        scr_c.append("    lv_obj_remove_style_all(obj);")
        scr_c.append("    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);")
        scr_c.append("    add_style_st_screen(obj);")
        scr_c.append(f"    lv_obj_set_style_bg_color(obj, lv_color_hex({hex_to_u32(bg)}), LV_PART_MAIN | LV_STATE_DEFAULT);")
        scr_c.append(f"    objects.{n} = obj;")
        scr_c.append("    lv_obj_set_pos(obj, 0, 0);")
        scr_c.append("    lv_obj_set_size(obj, 160, 80);")
        if p["components"]:
            scr_c.append("    {")
            scr_c.append("        lv_obj_t *parent_obj = obj;")
            for w in p["components"]:
                emit_widget(w, "parent_obj", scr_c, 2)
            scr_c.append("    }")
        scr_c.append("}")
        scr_c.append("")
        # delete_screen_<n>: clear object pointers & delete screen
        page_wids: list[str] = []
        def collect_ids(nodes: list[dict]):
            for nd in nodes:
                if nd.get("identifier"):
                    page_wids.append(nd["identifier"])
                collect_ids(nd.get("children", []))
        collect_ids(p["components"])
        scr_c.append(f"void delete_screen_{n}(void) {{")
        scr_c.append(f"    if (objects.{n}) {{")
        scr_c.append(f"        lv_obj_delete(objects.{n});")
        scr_c.append(f"        objects.{n} = 0;")
        for wid in page_wids:
            scr_c.append(f"        objects.{wid} = 0;")
        scr_c.append("    }")
        scr_c.append("}")
        scr_c.append("")
        scr_c.append(f"void tick_screen_{n}(void) {{}}")
        scr_c.append("")

    scr_c.append("void create_screen_by_id(enum ScreensEnum screenId) {")
    scr_c.append("    switch (screenId) {")
    for p in pages:
        n = p["name"]
        scr_c.append(f"    case SCREEN_ID_{n.upper()}: create_screen_{n}(); break;")
    scr_c.append("    default: break;")
    scr_c.append("    }")
    scr_c.append("}")
    scr_c.append("")
    scr_c.append("void delete_screen_by_id(enum ScreensEnum screenId) {")
    scr_c.append("    switch (screenId) {")
    for p in pages:
        n = p["name"]
        scr_c.append(f"    case SCREEN_ID_{n.upper()}: delete_screen_{n}(); break;")
    scr_c.append("    default: break;")
    scr_c.append("    }")
    scr_c.append("}")
    scr_c.append("")
    scr_c.append("void tick_screen(int screen_index) { (void)screen_index; }")
    scr_c.append("void create_screens(void) { create_screen_home(); }")
    scr_c.append("")

    (out_dir / "screens.c").write_text("\n".join(scr_c), encoding="utf-8")


def main() -> int:
    pages = build_pages()
    proj = build_eez_project_json(pages)
    proj_path = Path("open_fw/cassini.eez-project")
    proj_path.write_text(json.dumps(proj, indent=2) + "\n", encoding="utf-8")
    eez_dir = Path("open_fw/src/ui/eez")
    emit_eez_c_files(pages, eez_dir)
    print(f"Wrote {proj_path} ({proj_path.stat().st_size} B, {len(pages)} screens)")
    print(f"Wrote EEZ C sources to {eez_dir}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
