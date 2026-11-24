# Cassini OpenFW — `160×80` Display System & UI/UX Guide

This document describes the display hardware, typography, 14-color dark-mode palette, physical control model, and design guidelines for **`open_fw`**'s **`0.96"` (`160×80`)** LVGL v9.2.2 + EEZ Studio UI.

![Cassini OpenFW 160x80 UI Showcase](assets/openfw_ui_showcase.png)

*(Rendered directly by the real compiled `open_fw` LVGL v9.2.2 + EEZ Studio + ST7735S driver code via `make -C open_fw test-host`.)*

---

## 1. Display Hardware & Rendering Capabilities

| Property | Specification |
| :--- | :--- |
| **Panel** | `0.96"` TFT (`ST7735S`), **`160 × 80` px** landscape (`MADCTL = 0xA8`, `X_OFF = 1`, `Y_OFF = 26`) |
| **Color depth** | **16-bit RGB565** (`65,536` colors) |
| **UI framework** | **LVGL v9.2.2** (`LV_STDLIB_BUILTIN`) authored visually in **EEZ Studio** ([`open_fw/cassini.eez-project`](../open_fw/cassini.eez-project)) |
| **Rendering core** | Dedicated **CPU1 (96 MHz)** with full-screen `25.6 KiB` RGB565 direct buffer, zero-wait-state SRAM draw loops, and `10.4 ms` full-frame parallel 8080 LCD blits |
| **Text rendering** | **4-bit (16-level) grayscale anti-aliased** Noto Sans fonts with tabular digits (`0..9`) and crisp Unicode UI symbols |
| **Backlight** | PWM channel 3 (`0–100%`), driven low when blanked |
| **Flash budget** | ~`272 KiB` total `.text` out of **2 MiB Quad 48 MHz XIP NOR** |

> **Why 4-bit grayscale AA (not ClearType subpixel AA)?**
> In landscape (`160×80`), the panel's R/G/B subpixel stripes are stacked vertically rather than horizontally, so horizontal RGB subpixel AA causes color fringing. 4-bit (16-level) grayscale AA gives crisp, fringe-free edges in both orientations.

---

## 2. Built-in Typography System (`open_fw/src/ui/ui_fonts.c`)

Three 4-bit anti-aliased LVGL v9 fonts (~`14.8 KiB` combined glyph bitmaps) are generated from system Noto Sans TTFs via [`open_fw/tools/gen_lvgl_fonts.py`](../open_fw/tools/gen_lvgl_fonts.py):

| LVGL Symbol | Font & Size | Approx. Chars / `160 px` | Best Used For | Bitmap Size |
| :--- | :--- | :---: | :--- | :---: |
| `ui_font_lg` | Noto Sans Bold **22 px** | ~10–12 chars | Hero timers (`13:21`, `02:00`), device name (`HW_76`), primary status | `8.7 KiB` |
| `ui_font_md` | Noto Sans Medium **14 px** | ~18–21 chars | File rows (`Today 14:32`), settings rows, transcript captions | `3.7 KiB` |
| `ui_font_sm` | Noto Sans Medium **11 px** | ~24–28 chars | Top status bar, durations, badges, bottom hint bar | `2.3 KiB` |

- **Tabular numerals:** Digits `0..9` share a uniform advance width so live recording and playback timers never jitter horizontally.
- **Included European scripts & UI symbols:** Full **Latin-1 Supplement** (`U+00A0..U+00FF`: German `ä ö ü Ä Ö Ü ß`, French, Spanish, Italian, Portuguese, Nordic, `« » ± · ×`), **Latin Extended-A** (`U+0100..U+017F` + Romanian `Ș ș Ț ț`: Polish, Czech, Slovak, Hungarian, Turkish, Croatian, Baltic), **Greek** (`U+0386..U+03CE`), **Cyrillic** (`U+0400..U+045F` + Ukrainian `Ґ ґ`), European punctuation (`– — ‘ ’ “ ” „ … €`), and UI symbols (`− ■ ▲ ▶ ▼ ◀ ● ✓`).
- **Automatic truncation:** LVGL `LV_LABEL_LONG_DOT` handles clean ellipsis truncation on long names.

---

## 3. Dark-Mode Color Palette (`open_fw/include/openfw_ui.h`)

| Token | Hex | RGB565 | Role |
| :--- | :---: | :---: | :--- |
| `UI_COL_BG` | `#070B14` | `0x0042` | Global dark canvas |
| `UI_COL_BAR` | `#111827` | `0x10C4` | Top status bar, transcript footer |
| `UI_COL_TILE` | `#131B2B` | `0x10C5` | Unfocused tiles & subtle pills |
| `UI_COL_PILL` | `#1F4E8C` | `0x1A71` | Focused tile / active selection pill |
| `UI_COL_TEXT` | `#FFFFFF` | `0xFFFF` | Primary text & active labels |
| `UI_COL_DIM` | `#9CB0CE` | `0x9D99` | Secondary labels & footer hints |
| `UI_COL_CYAN` | `#42C8FF` | `0x465F` | Playback accent, values, Speaker 1 |
| `UI_COL_RED` | `#FF3B3B` | `0xF9C7` | Active recording, delete guard, overload |
| `UI_COL_AMBER` | `#FFB020` | `0xFD84` | Paused, VOR standby, warnings, Speaker 2 |
| `UI_COL_GREEN` | `#3DDC84` | `0x3EF0` | Saved, charging, healthy VU level, Speaker 3 |
| `UI_COL_PINK` | `#FF7AC8` | `0xFBD9` | Speaker 4 in synced CWX transcripts |

---

## 4. Physical Controls (No Touchscreen)

All interactions use physical buttons + 1 hardware slide switch (`1 kHz` IRQ SARADC debouncer in `open_fw/src/keys.c`):

| Control | Physical Input | UX Role in `open_fw` |
| :--- | :--- | :--- |
| **REC Slide Switch** | Side latch (`PB6`) | **Slide UP = Instant Record** (even from idle/playback); **Slide DOWN = Save & Stop** |
| **OK / PLAY** | Front (`PD0` ladder) | Confirm / Play / Pause / Drop spectrogram marker while recording / Hold to delete in Files |
| **BACK** | Front (`PD0` ladder) | Return one level / Stop sync / Hold to eject USB disk |
| **PREV / NEXT** | Side/front (`PD0` ladder) | Navigate tiles & lists; during playback: tap to skip, double-tap for prev/next file, hold for audible pitch-preserving `2×–5×` scrub |
| **VOL+ / VOL-** | Side (`PD0` ladder) | Adjust headphone/speaker volume (32-step log dB curve); adjust values in Settings |
| **POWER (ON/OFF)** | Side (`PMU 0x31`) | Short press: wake / blank screen; hold 2 s: power off (or charging standby while on USB) |

---

## 5. Practical Design Guidelines for `0.96"` (`160×80`)

1. **Dark-mode first:** Deep navy-black (`#070B14`) with crisp white/cyan/amber accents maximizes legibility on a tiny backlit TFT and blends with the bezel.
2. **Smartwatch glance density:** Keep 2–3 information zones per screen — a compact `13 px` top status bar, one hero focus zone (`22 px` timer, spectrogram, or 3-row list), and a subtle `13 px` footer hint.
3. **Filled pills over wireframe boxes:** Use rounded solid pills (`UI_COL_PILL` / `UI_COL_TILE`) for focus states rather than dim `1 px` borders.
4. **Edit visually in EEZ Studio, verify in `< 0.5 s` on host:** Open [`open_fw/cassini.eez-project`](../open_fw/cassini.eez-project), rebuild into `open_fw/src/ui/eez/`, and run `make -C open_fw test-host` to render all 14 screens to `/tmp/sl6801_openfw/ui_all_screens.ppm` and verify heap & pixel budgets. See **[LVGL_ARCHITECTURE.md](LVGL_ARCHITECTURE.md)** and **[EMULATOR_GUIDE.md](EMULATOR_GUIDE.md)** for full workflows.
