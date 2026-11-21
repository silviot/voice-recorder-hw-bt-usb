# Cassini OpenFW — LVGL v9 UI Architecture, Multi-Device Porting & EEZ Studio Guide

`open_fw` uses **LVGL v9.2.2** (freestanding, `LV_STDLIB_BUILTIN`) organized into clean, decoupled layers so the firmware is easy to evolve and straightforward to adapt to other similar voice-recorder hardware.

---

## 1. Layered Architecture

```
open_fw/
├── include/
│   ├── board_config.h       # Device profile (resolution, color depth, compact mode, RAM budget)
│   ├── lv_conf.h            # Lean LVGL v9 config (RGB565, built-in TLSF/string/snprintf)
│   ├── lv_inttypes_shim.h   # Freestanding <inttypes.h> shim for bare-metal Cortex-M4F
│   └── openfw_ui.h          # Hardware-agnostic UI state model (ui_model_t) & screen IDs
├── src/
│   ├── lcd_st7735s.c        # Pure hardware LCD + PWM backlight driver (no UI logic)
│   ├── keys.c               # 1 kHz IRQ SARADC debouncer + SPSC event queue (never drops taps)
│   ├── ui_spectrogram.c     # Live 128-pt FFT Mel spectrogram (RGB565 buffer)
│   └── ui/
│       ├── ui_port.c/.h     # LVGL glue: display flush_cb, partial buffer, tick, keypad indev
│       ├── ui_theme.c/.h    # 14-color palette, reusable styles, status bar, footer, VU meter, toast
│       ├── ui_fonts.c       # Generated 4-bit AA Noto Sans LVGL fonts (sm/md/lg, tabular digits)
│       └── ui_screens.c     # Declarative LVGL v9 screens + in-place ui_model_t binders
├── tools/
│   └── gen_lvgl_fonts.py    # Regenerate src/ui/ui_fonts.c from any TTF (Python + Pillow)
└── third_party/
    └── lvgl/                # Auto-fetched pinned LVGL v9.2.2 (gitignored)
```

### Memory Footprint (SL6801 Cortex-M4F)
- **Flash (`.text`):** ~233 KiB total firmware (out of 2 MiB XIP NOR)
- **LVGL SRAM:** 6.4 KiB partial draw buffer (`160 × 20 × 2 B`) + 36 KiB built-in TLSF heap
- **Single-active-screen lifecycle:** Only the active screen's widget tree exists in RAM at a time (~8–12 KiB peak heap), avoiding fragmentation and keeping >45 KiB SRAM free.

---

## 2. Adapting to Another Voice Recorder

To bring `open_fw` up on a different recorder with similar hardware:
1. **Display & layout geometry:** Edit `open_fw/include/board_config.h` (`BOARD_DISP_HOR_RES`, `BOARD_DISP_VER_RES`, `BOARD_DISP_BUF_LINES`, `BOARD_NAME`). Screens use LVGL flex/alignment and `BOARD_UI_COMPACT` automatically scales list row counts and chrome heights for larger panels.
2. **Display controller:** Replace or add a hardware driver providing `lcd_init()`, `lcd_set_rotation()`, `lcd_backlight_set()`, and `lcd_blit_rgb565(x, y, w, h, pixels)`. `ui_port.c` calls only `lcd_blit_rgb565()`.
3. **Buttons / switches:** Adapt `keys.c` to the new ADC ladder or GPIO pins while keeping the same `keys_pop_event()` / `keys_state()` API.
4. **Fonts:** Run `python3 open_fw/tools/gen_lvgl_fonts.py` with different sizes if targeting a higher-DPI screen.

---

## 3. Adding Animations Safely

`main.c` calls `ui_port_task_handler()` (`lv_timer_handler()`) every **~16 ms** (~60 Hz) whenever the screen is awake.
- **Keep animations lightweight:** Use translation (`x`/`y`), `width`/`height`, `lv_bar` value (`LV_ANIM_ON`), background/text color transitions, and simple opacity. Software rotation, zoom, and drop shadows are intentionally disabled in `lv_conf.h` for Cortex-M4F performance.
- **Short durations:** 120–200 ms eased transitions (`lv_anim_path_ease_out`) feel crisp on button-driven hardware without slowing down rapid key navigation.
- **Blocking I/O:** Animations briefly pause during synchronous SD/exFAT writes; keep filesystem calls short or yield between chunks if animating during I/O.

---

## 4. Editing Screens with EEZ Studio

The UI is authored as a native **EEZ Studio (LVGL 9.2.2)** project at **`open_fw/cassini.eez-project`**:

1. Open **`open_fw/cassini.eez-project`** in [EEZ Studio](https://www.envox.eu/studio/studio-introduction/).
2. Edit any of the 16 screens (`home`, `rec`, `rec_err`, `saved`, `files`, `files_empty`, `player`, `player_tr`, `delete_confirm`, `settings`, `about`, `usb`, `bt`, `charge`, `power`, `off`), adjust styles/colors in the Dark theme, or tweak widget positions visually at `160×80`.
3. Click **Build** in EEZ Studio — it writes directly to **`open_fw/src/ui/eez/`** (`ui.c`, `screens.c`, `styles.c`, `images.c`, and headers).
4. **Keep hardware & app logic decoupled:**
   - Leave `ui_port.c` (display flush, tick, keypad `indev`) and `openfw_ui.h` (`ui_model_t`) untouched.
   - `open_fw/src/ui/ui_screens.c` is a thin, layout-free data binder that maps `ui_model_t` onto EEZ's generated `objects.*` widget handles.
   - Rebuild and verify instantly on host with `make -C open_fw all test-host`.
