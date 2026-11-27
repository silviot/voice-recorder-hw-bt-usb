# `open_fw` — Standalone Open-Source SL6801 Firmware

Bare-metal C99 + **LVGL v9.2.2** firmware for the **Smartlink SL6801** dual-core Cortex-M4F voice recorder (`HW_76` / `HW-A1`).

## Directory Structure

```text
open_fw/
├── Makefile                  # ARM build, LVGL auto-fetch, host/ASan tests, update.up, QEMU
├── sl6801_firm.ld            # Hybrid <= 4 KiB .ram_boot + Quad XIP (.text) + SRAM fast-path (.data)
├── cassini.eez-project       # Native EEZ Studio LVGL v9.2.2 visual UI project (16 screens @ 160x80)
├── include/                  # Public headers, board_config.h, lv_conf.h, SL6801 MMIO/ROM headers
├── src/
│   ├── startup.c             # .ram_boot trampoline (FPU, 96 MHz PLL0, Quad 48 MHz XIP), crash handler
│   ├── main.c                # Top-level app state machine, power/screen management, settings & sync loops
│   ├── cpu1_ui.c             # Dedicated CPU1 UI worker + IPC Mailbox WFE sleep
│   ├── lcd_st7735s.c         # ST7735S 8080 parallel LCD & PWM3 backlight driver
│   ├── ui_spectrogram.c      # Live 20 Hz 128-pt FFT Mel spectrogram renderer
│   ├── ui/                   # LVGL port, theme, generated 4-bit AA fonts, data binders & EEZ screens
│   ├── audio_dma.c           # Dual-mic AMIC1+AMIC2 DMAC CH1 ring capture
│   ├── audio_dac.c           # Stereo DAC0/DAC1 DMAC CH0 playback, PB30 speaker PA & 32-step log volume
│   ├── audio_dsp.c           # Clean-room Voice Clarity / ASR Raw / Bypass DSP + VOR gate
│   ├── recorder.c            # Checkpointed WAV recording state machine (+ Auto-PGA clip protection)
│   ├── player.c              # WAV (PCM/IMA-ADPCM) player with speed dial (-32x..32x) & CWX captions
│   ├── audio_out.c           # BT=1 output dispatch (DAC / BT A2DP headset / Both)
│   ├── wav.c / cwx.c         # WAV parser/writer & CWX1 timed-word transcript engine
│   ├── sdio.c / exfat.c      # 1-bit SDIO driver (CMD18/CMD25) & crash-safe exFAT filesystem
│   ├── apmem.c / persist.c   # Per-folder playback auto-resume (.autoplay.state) & /cassini/settings.bin
│   ├── usb_musb.c / usb_scsi.c # Mentor MUSB Full-Speed USB MSC (EP2 2 KiB FIFO) + vendor 0xE0 SCSI
│   ├── bt_rf.c / xrec_proto.c # Classic BT RF calibration + +XREC v5 protocol
│   ├── rtos/                 # FreeRTOS V11.1.0 glue & boot self-test (RTOS=1 / BT=1)
│   ├── keys.c / pmu.c / rtc.c / uart.c / wdt_timer.c # Platform peripherals & SRAM log ring
│   ├── crypto.c              # SHA-256, HMAC-SHA256, CRC-16/CCITT-FALSE, constant-time compare
│   └── exp_runner.c          # SRAM experiment execution bridge (0x00850000 arena, DEV_EXP=1)
├── bt_port/                  # BT=1 Bluedroid host port (phone XREC, A2DP/AVRCP headphones, RW-BT glue)
├── tests/                    # Native host MMIO/UI/DSP/FS suites + tests/bt_host/ Bluedroid harness
├── exp/                      # Standalone Thumb-2 SRAM hardware probes (built via tools/run_exp.py)
├── tools/                    # FIRM packer, LVGL font & EEZ generators, QEMU runner & patch
└── third_party/lvgl/         # Auto-fetched pinned LVGL v9.2.2 (gitignored)
```

## Build & Test Commands

`BT=1` (the default since plan step 9) links the Bluedroid host stack (`bt_port/`, `third_party/bluedroid/`) and
picolibc, built once by `tools/build_picolibc.sh` (needs `meson` + `ninja`); it forces `RTOS=1`. `BT=0` is the
supported image without Bluetooth (no picolibc). The WebAssembly build always uses the no-BT configuration.

```bash
# Build ARM Cortex-M4F FIRM binary (/tmp/sl6801_openfw/open_fw_firm.bin), Bluetooth included
make -C open_fw all

# The same without Bluetooth
make -C open_fw BT=0 all

# Bluetooth host tests (bt/host_tests.mk): port shims, controller shim, XREC session, headphones DSP, Bluedroid harness
make -C open_fw test-bt-port test-bt-ctrl test-bt-xrec test-bt-hp test-bt-host

# Boot checks + RTOS self-test in QEMU: without BT (RTOS=1) and with BT
make -C open_fw test-qemu-rtos test-qemu-bt

# Run all native host unit, MMIO driver, UI render, DSP, and fsck.exfat tests
make -C open_fw test-host

# Run the same suite under ASan + UBSan
make -C open_fw test-host-san

# Pack full 2 MiB flash image + 0:\update.up (requires local ../dumps/firmware_2M.bin)
make -C open_fw all update-up

# Boot and verify in SL6801 QEMU emulator (see ../docs/EMULATOR_GUIDE.md)
make -C open_fw test-qemu

# Build the interactive browser WebAssembly emulator (../web_emu/openfw.wasm)
make -C open_fw wasm
python3 web_emu/serve.py --open

# Build with SRAM experiment runner enabled for hardware probing (see ../docs/DEVELOPMENT_WORKFLOW.md)
make -C open_fw DEV_EXP=1 all update-up
```

## Memory Layout (`sl6801_firm.ld`)

| Region | Address | Budget / Purpose |
| :--- | :--- | :--- |
| `.ram_boot` | `0x00804C00` | `<= 4 KiB` Stage-1 `HLKJ` load window (`3,960 B` `BT=1`, `3,964 B` `BT=0`) |
| `.bt_fast` + `RTOS_HEAP_LO` | `0x00805C00..0x0081A37F` | `81.9 KiB` lower SRAM (`24.1 KiB` `.bt_fast` hot BT/SBC SRAM code + optional `.bt_lo_bss` in `BENCH`/`HCI_PROBE` + `~57.8 KiB` FreeRTOS heap) |
| `.text` / `.rodata` | `0x00C0F430` | Quad 48 MHz XIP NOR (`~395 KiB` `BT=0`, `~803 KiB` `BT=1` out of ~`1.9 MiB`) |
| `.data` + `.bss` | `0x0081A380..0x0084FFFF` | Zero-wait-state SRAM fast-path (hot LVGL/LCD code, 25.6 KiB FB, audio rings, TLSF heap; `BT=1` `_ebss = 0x0084F928`) |
| `.exp_arena` / `.exp_out` | `0x00850000..0x0085BFFF` | `32 KiB` SRAM probe arena + `16 KiB` capture buffer |
| `.bss_hi` (`OPENFW_SRAM_HI`) | `0x0085C000..0x00866FFF` | `44 KiB` upper SRAM for large non-hot buffers (96-entry file browser & CPU1 UI slots; `BT=1` `_ebss_hi = 0x00866B30`), below `_estack = 0x0086AC08` |
| `RTOS_HEAP` | `0x0086B000..0x0087FFFF` | `84 KiB` upper RTOS SRAM (`9.0 KiB` `.rtos_static`, `42.2 KiB` `.bt_data`/`.bt_bss` + `~32.8 KiB` FreeRTOS heap; total `~90.6 KiB` FreeRTOS heap across `RTOS_HEAP_LO` + `RTOS_HEAP`) |

## microSD Disk Layout (exFAT)

The recorder uses a single exFAT partition on the microSD card with the following directory and file conventions:

```text
/
├── record/                     # On-device voice recordings saved by recorder.c
│   ├── REC_0001.wav            # 16 kHz mono/stereo PCM WAV (newest-first in Files root)
│   ├── REC_0001.cwx            # Optional synced word-timed transcript sidecar (CWX1)
│   └── <subfolder>/            # Optional nested folders (also surfaced in Files root)
├── audiobooks/                 # Recommended location for multi-chapter audiobooks / audio folders
│   └── <Book_Name>/            # Surfaced as a folder row ("Book Name", "<N> ▶") in Files root
│       ├── .autoplay           # Optional marker (any content, may be empty): continue to next chapter
│       ├── .autoplay.state     # Firmware-written 16 KiB memory (per-file positions + last focus); safe to delete
│       ├── 01_Cap1.wav         # Chapters listed ascending (A–Z) inside the folder
│       └── 01_Cap1.cwx         # Optional synced UTF-8 transcript sidecar per chapter
├── cassini/                    # Firmware state & metadata
│   ├── settings.bin            # Persisted UI/audio settings + last played file & resume offset
│   └── <subfolder>/            # Optional nested audio folders (also surfaced in Files root)
├── cassini.key                 # +XREC v5 paired host key ("CSK5" + 32 B)
├── bt_bonds.bin                # BT=1: phone-mode Bluedroid bonds (bt_port/bt_xrec_fw.c)
├── bt_phone_cfg.bin            # BT=1: phone-mode auto-sync & auto-off preferences
├── bt_hp_bonds.bin             # BT=1: headphones-mode bonds (headsets)
├── bt_hp_cfg.bin               # BT=1: headphones settings (output mode, volume, last headset)
├── update.up                   # Optional SD OTA firmware update package (Stage-1 HLKJ)
└── <Custom_Folder>/            # Any other non-empty root folder (excluding dotfiles, System Volume Information, $RECYCLE.BIN)
```

A `bt_keys.bin` left by the legacy Bluetooth host (removed in plan step 9) is ignored: bonds are not migrated
(docs/BLUETOOTH_BLUEDROID_PLAN.md D8), so phones and headsets pair again once.

### File Browser Rules (`src/main.c`, `src/ui/ui_screens.c`)
- **Root `Files` view:** Shows all non-empty subfolders discovered under `/audiobooks/`, `/cassini/`, `/record/`, and `/` (sorted A–Z with an amber dot and `<N> ▶` chapter count), followed by loose `.wav` files from `/record/`, `/cassini/`, and `/` sorted **newest-first** (up to `96` entries per view).
- **Inside a folder (`OK` to enter, `BACK` to return):** Shows any nested subfolders first, followed by `.wav` files sorted **ascending (A–Z)** so audiobook chapters play in order. With the *Skip: Track* setting, prev/next in the player navigates only within audio files and skips folder rows.
- **Display titles:** Generic recorder names (`REC_*` or `YYYY*` timestamps) render as friendly dates (`Today 14:32`); custom names (folders and audiobook chapters) strip `.wav` and replace `_` with spaces (e.g., `Jekyll_Hyde` → `Jekyll Hyde`, `JH_01a_Cap1.wav` → `JH 01a Cap1`).
- **Transcripts (`.cwx`):** Any `<stem>.cwx` sitting in the same directory as `<stem>.wav` is automatically matched and displayed as live karaoke captions during playback.
- **Hidden files:** Names starting with `.` (the `.autoplay` marker, the `.autoplay.state` memory file, macOS `._*` AppleDouble twins) are never listed as tracks, counted as chapters, or returned by `+XREC:LS`; dot-directories are never listed as folders or counted as subfolders either.
- **Autoplay (`.autoplay`):** If a folder contains a file named `.autoplay` (matched case-insensitively, any content), a track that plays to its natural end continues immediately with the next track **of the same folder** in Files-list order (next chapter starts at its remembered position, else 0:00 — see below). The last track stops on the End screen as usual (no wrap). Fast-forwarding into the end with the speed dial counts as reaching it (the dial hands over to the normal end-of-file path): it advances, and the track is stored as finished; without the marker the player stays on End even while RIGHT is held. The marker is looked up once per folder via `player_dir_has_autoplay()` and cached in `main.c` until the file list is reloaded (USB/BT exit, delete, folder change). Resuming such a track from Home opens its folder in Files so playback can continue through it. Without the marker nothing changes.
- **Autoplay memory (`.autoplay.state`, `src/apmem.c`):** In a folder with the marker the recorder remembers the playhead **per file** and the folder's **last focused file**. Entering the folder in Files puts the cursor on the file you last had focused or played (fallback: first row); opening a file — by hand, from Home → continue, or by an automatic advance — resumes at its saved position. Rules: a saved position inside the last 10 s of a file, past its end, or of a file with unknown length starts at 0:00; a track that plays to its natural end is stored as finished (slot cleared) so it restarts at 0:00 next time. A file renamed (other name) or replaced (other size) does not match its old entry; stale entries are simply aged out. Without the marker no state is read or written; removing the marker over USB disables the feature (the file stays and is honoured again if the marker returns). Deleting `.autoplay.state` resets the memory.
  - **Storage:** one hidden fixed-size 16 KiB file per folder, created once on the first real save (one-time cost: directory entry + FAT/bitmap + data, like any small file). It holds 16 logical 512 B sectors (header with the focused name + 15 sectors × 31 slots = 465 position slots, `{name hash, size tag, seconds, stamp}`; slot sector = FNV-1a(lower-case name) mod 15, least-recently-written slot evicted when a sector is full). Each logical sector exists twice (A/B); every save reads the pair and overwrites the **older** copy with seq+1 and a CRC-16 via `exfat_overwrite_sector()` — **exactly one 512 B data-sector write, no FAT, bitmap, directory-entry or size change**. A torn write can only damage the copy being written, so the previous value survives (at most the latest position is lost) and the filesystem is never at risk. Unchanged values are not written at all.
  - **Why not `.autoplay.config/` with one small file per track, or editing the `.autoplay` marker:** each per-file update would create/replace a file (directory entries in the folder, FAT and allocation-bitmap sectors — the metadata a power cut can corrupt — plus a whole cluster, 32–128 KiB, per track), i.e. 5–8 metadata writes per save instead of one data sector. The marker is a user file, usually empty (no cluster to overwrite) and may be rewritten by the host over USB.
  - **When it writes:** pause (immediately; skips while paused coalesced to one write per 5 s), leaving a track (Back, next/prev, automatic advance, REC slide), power-off (also from the Files list: focus), Files Back out of the folder (focus), track start (focus, only when it changed), and during uninterrupted playback at most every 60 s (never while the speed dial is away from 1×: the first tick back at 1× writes the heard position if the period has elapsed) (every 15 s below 3.5 V, ahead of a brown-out). An hour of continuous listening therefore costs ~60 single-sector writes (+2 per chapter change), spread over two alternating sectors. A position whose write is triggered by the REC slide is kept in RAM and written only after the recording is closed; no position write ever runs while the recorder is saving. A write error (read-only/full card) silences the feature until the next file-list reload; playback never blocks. RAM cost: one 1 KiB static sector-pair buffer plus ~100 B of state.
- **Safety & universal resume:** The track being played is recorded as "last played" before it starts, and playback position is committed via `apmem` + `settings.bin` periodically (every 60 s at 1×, or 15 s under low battery), immediately on pause, on every track/mode switch, and on clean power-off for both `.autoplay` folders and loose tracks. Folder rows are protected from long-press delete; `settings.bin` resume lookup searches both loose files and discovered folders.

For deeper technical details, see **[../docs/LVGL_ARCHITECTURE.md](../docs/LVGL_ARCHITECTURE.md)**, **[../docs/SL6801_HW76_HARDWARE.md](../docs/SL6801_HW76_HARDWARE.md)**, and **[../docs/DEVELOPMENT_WORKFLOW.md](../docs/DEVELOPMENT_WORKFLOW.md)**.
