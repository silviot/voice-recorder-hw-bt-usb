# Using the SL6801 Emulator for Firmware & UI Development

You can build, test, and visually inspect **Cassini OpenFW (`open_fw`)** — and even run the **original stock firmware** — on your PC without touching real hardware or wearing out Flash memory.

---

## 1. Three Ways to Preview & Test

| Workflow | Speed | What It Runs | Best For |
| :--- | :--- | :--- | :--- |
| **Host Visual UI Renderer (`test_ui_render`)** | `< 0.5 s` | Real C UI code (`lcd_st7735s.c`, `cpu1_ui.c`, `ui_spectrogram.c`, `src/ui/*`, `src/ui/eez/*` + LVGL v9.2.2) compiled natively against a software ST7735S framebuffer | Rapid pixel-level UI layout & typography iteration (renders all 14 screens/sub-states to a contact sheet in one shot) |
| **Headless ARM QEMU (`open_fw/tools/run_qemu.py`)** | `~4 s` | Real ARM Cortex-M4F Thumb-2 `open_fw` binary (`firmware_2M_openfw.bin`) on emulated SL6801 CPU, LCDC, SDIO/exFAT, SARADC keys, and PMU | Full end-to-end firmware verification, boot animation & `160×80` landscape screenshots, automated CI checks |
| **Interactive Browser Emulator (`smartlink-sl6801-emulator` `./gui`)** | Real-time | Real ARM Cortex-M4F firmware with clickable on-screen buttons, keyboard shortcuts, and live `160×80` display | Interacting with the firmware as a user (navigating tiles, menus, settings, and testing button flows) |

---

## 2. One-Time QEMU Emulator Setup

The emulator is based on [`anton-veretenenko/smartlink-sl6801-emulator`](https://github.com/anton-veretenenko/smartlink-sl6801-emulator) with our patch ([`open_fw/tools/sl6801_qemu_openfw.patch`](../open_fw/tools/sl6801_qemu_openfw.patch)) that adds `UART0` logging, 32-bit LCDC bursts, and ST7735S `MADCTL (0x36)` `MV=1` `160×80` landscape support.

```bash
# Clones QEMU 8.2.2 + smartlink-sl6801-emulator, applies our OpenFW patch, and builds qemu-system-arm
bash open_fw/tools/setup_qemu.sh
```

The built emulator binary is placed at `/tmp/sl6801-emu/work/build/qemu-system-arm` (override with `QEMU_SL6801=/path/to/qemu-system-arm`).

> **Note:** Running QEMU requires a local copy of the 384 KB Boot ROM (`dumps/bootrom_384K.bin`) and 2 MB flash base (`dumps/firmware_2M.bin`) dumped from your own device.

---

## 3. Fast UI Iteration: Host Visual Renderer (`< 0.5 s`)

Every time you edit fonts, styles, or screen layouts in `open_fw/cassini.eez-project` / `open_fw/src/ui/`, run:

```bash
make -C open_fw test-host
```

This compiles and executes all 6 host test suites (`test_core`, `test_drivers_mmio`, `test_ui_render`, `test_audio_dsp`, `test_player`, and `test_audio_bt_mmio`), including `test_ui_render`, which renders **all 14 UI screens and sub-states** into a 2× zoomed contact sheet at:

```text
/tmp/sl6801_openfw/ui_all_screens.ppm
```

Convert to PNG if you want to share it:

```bash
python3 -c 'from PIL import Image; Image.open("/tmp/sl6801_openfw/ui_all_screens.ppm").save("/tmp/sl6801_openfw/ui_all_screens.png")'
```

---

## 4. Running `open_fw` in ARM QEMU (Headless & Screenshots)

Build the firmware image and run the automated QEMU verification harness:

```bash
# 1. Build open_fw and pack the 2 MB flash image
make -C open_fw all update-up

# 2. Boot in QEMU, verify SysTick, SRAM log, SDIO/exFAT mount, and save a 3x landscape PNG
python3 open_fw/tools/run_qemu.py --boot-seconds 3.5 --png-out /tmp/sl6801_openfw/qemu_home.png
```

Useful flags for `open_fw/tools/run_qemu.py`:
- `--boot-seconds 3.5` — wait past the 2.0 s Cassini boot animation to capture the Home screen (use `1.0` to capture mid-animation).
- `--landscape` (default) / `--portrait` — choose `160×80` landscape or raw `80×160` portrait output.
- `--scale 3` — integer nearest-neighbor zoom factor (default `3` → `480×240`).
- `--skip-keys` — skip the second pass that injects `KEY_ENTER` (`kadc-raw-ch1=2850`) to verify USB/key handling.

---

## 5. Interacting With `open_fw` as a User (Live GUI)

You can launch the emulator's interactive web GUI with `open_fw` loaded so you can click the recorder's physical buttons and watch the `160×80` screen respond live in your browser:

```bash
# Point the emulator GUI at your open_fw flash image and open the browser UI
QEMU=/tmp/sl6801-emu/work/build/qemu-system-arm \
SL6801_FLASH="$PWD/dumps/firmware_2M_openfw.bin" \
SL6801_ROM="$PWD/dumps/bootrom_384K.bin" \
SL6801_MACHINE_EXTRA="rom-stub=/tmp/sl6801_openfw/qemu_boot_stub.bin,viewtask-local=off,high-rom-hle=off" \
/tmp/sl6801-emu/repo/gui
```

From the browser UI you can press **PREV**, **NEXT**, **VOL+**, **VOL-**, **OK / PLAY**, **MENU / BACK**, and **POWER** (via mouse clicks or keyboard shortcuts) to test navigation, file browsing, settings, and USB/BT screens before flashing real hardware.

---

## 6. Running the Original Stock Firmware (`pen133_2.0.65`) in QEMU

To inspect or screenshot the **original factory firmware** (`dumps/firmware_2M.bin`), use [`tools/run_stock_qemu.py`](../tools/run_stock_qemu.py), which applies our ported `pen133_2.0.65` ViewTask local-task patches over GDB RSP:

```bash
python3 tools/run_stock_qemu.py --wait-sec 2.5 --out /tmp/stock_ui.png
```

---

## 7. Zero-Flash Live Hardware Preview (SRAM Experiments)

Once a UI screen or driver tweak looks good in the emulator, you can also test small code snippets directly in **device SRAM (`0x00850000`)** over USB without writing to NOR flash:

```bash
python3 tools/run_exp.py --build-only --save-bin /tmp/exp.bin open_fw/exp/exp_lcd_land.c
sudo python3 tools/openfw_usb.py --exp-upload /tmp/exp.bin --exp-run --log
```
