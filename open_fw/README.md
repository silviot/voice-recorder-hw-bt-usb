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
│   ├── recorder.c            # Checkpointed WAV recording state machine
│   ├── player.c              # WAV (PCM/IMA-ADPCM) player with audible 2x-5x scrub & CWX captions
│   ├── wav.c / cwx.c         # WAV parser/writer & CWX1 timed-word transcript engine
│   ├── sdio.c / exfat.c      # 1-bit SDIO driver (CMD18/CMD25) & crash-safe exFAT filesystem
│   ├── persist.c             # Persistent settings (/cassini/settings.bin)
│   ├── usb_musb.c / usb_scsi.c # Mentor MUSB Full-Speed USB MSC (EP2 2 KiB FIFO) + vendor 0xE0 SCSI
│   ├── bt_rf.c / bt_hci.c / bt_spp.c / xrec_proto.c # Classic BT RF/HCI/SPP + +XREC v5 (WIP)
│   ├── keys.c / pmu.c / rtc.c / uart.c / wdt_timer.c # Platform peripherals & SRAM log ring
│   ├── crypto.c              # SHA-256, HMAC-SHA256, CRC-16/CCITT-FALSE, constant-time compare
│   └── exp_runner.c          # SRAM experiment execution bridge (0x00850000 arena, DEV_EXP=1)
├── tests/                    # 6 native host test suites (core, MMIO drivers, UI, DSP, player, audio/BT)
├── exp/                      # Standalone Thumb-2 SRAM hardware probes (built via tools/run_exp.py)
├── tools/                    # FIRM packer, LVGL font & EEZ generators, QEMU runner & patch
└── third_party/lvgl/         # Auto-fetched pinned LVGL v9.2.2 (gitignored)
```

## Build & Test Commands

```bash
# Build ARM Cortex-M4F FIRM binary (/tmp/sl6801_openfw/open_fw_firm.bin)
make -C open_fw all

# Run all 6 host unit, MMIO driver, UI render, DSP, and fsck.exfat tests
make -C open_fw test-host

# Run the same suite under ASan + UBSan
make -C open_fw test-host-san

# Pack full 2 MiB flash image + 0:\update.up (requires local ../dumps/firmware_2M.bin)
make -C open_fw all update-up

# Boot and verify in SL6801 QEMU emulator (see ../docs/EMULATOR_GUIDE.md)
make -C open_fw test-qemu

# Build with SRAM experiment runner enabled for hardware probing (see ../docs/DEVELOPMENT_WORKFLOW.md)
make -C open_fw DEV_EXP=1 all update-up
```

## Memory Layout (`sl6801_firm.ld`)

| Region | Address | Budget / Purpose |
| :--- | :--- | :--- |
| `.ram_boot` | `0x00804C00` | `<= 4 KiB` Stage-1 `HLKJ` load window (bootstrap + vector table) |
| `.text` / `.rodata` | `0x00C0F430` | Quad 48 MHz XIP NOR (~`272 KiB` used out of ~`1.9 MiB`) |
| `.data` + `.bss` | `0x0081A380..0x0084FFFF` | Zero-wait-state SRAM fast-path (hot LVGL/LCD code, 25.6 KiB FB, audio rings, TLSF heap) |
| `.exp_arena` / `.exp_out` | `0x00850000..0x0085BFFF` | `32 KiB` SRAM probe arena + `16 KiB` capture buffer |

For deeper technical details, see **[../docs/LVGL_ARCHITECTURE.md](../docs/LVGL_ARCHITECTURE.md)**, **[../docs/SL6801_HW76_HARDWARE.md](../docs/SL6801_HW76_HARDWARE.md)**, and **[../docs/DEVELOPMENT_WORKFLOW.md](../docs/DEVELOPMENT_WORKFLOW.md)**.
