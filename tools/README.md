# `tools/` — Host Utilities & Development Scripts

Python host utilities for building, flashing, debugging, syncing, and testing the SL6801 voice recorder.

## Active `open_fw` Tools

| Script | Purpose | Self-Test / Example |
| :--- | :--- | :--- |
| **[`dev_cycle.py`](dev_cycle.py)** | Unattended dev loop: `status`, `dfu`, `boot`, safe `flash` (`0x00D000..end`, preserves `HLKJ` & `PSMP`), `log`, and SRAM `exp` runner | `python3 tools/dev_cycle.py --self-test` |
| **[`openfw_usb.py`](openfw_usb.py)** | Zero-dependency USB SCSI `0xE0` client for `open_fw` (`1209:0001`): device info, SRAM log, RTC get/set, memory read, SRAM experiment upload/run, reboot | `python3 tools/openfw_usb.py --self-test` |
| **[`flash_ranges.py`](flash_ranges.py)** | Clean-room chunked SPI NOR partial flasher & hardware CRC-16 + readback verifier targeting BootROM DFU (`301a:2800`) only (defaults to `--dry-run`) | `python3 tools/flash_ranges.py --self-test` |
| **[`usb_sync.py`](usb_sync.py)** | Auto-detect & rsync-style USB recording importer with SHA-256 verification and content-hashed collision safety (`--watch` supported) | `python3 tools/usb_sync.py --dest ~/Recordings` |
| **[`run_exp.py`](run_exp.py)** | Compile, upload, run, and dump output/WAV from standalone C probes in `open_fw/exp/` (`DEV_EXP=1` builds) | `python3 tools/run_exp.py --build-only open_fw/exp/exp_probe.c` |
| **[`asr_prep.py`](asr_prep.py)** | Downstream ASR preprocessing for NVIDIA Parakeet (`--target parakeet`) and Cactus Whistle (`--target whistle`) | `python3 tools/asr_prep.py --self-test` |
| **[`cassini_to_device.py`](cassini_to_device.py)** | Convert Cassini portable-meeting Ogg Opus files into device-ready IMA-ADPCM/PCM `.wav` + `.cwx` synced transcript files | `python3 tools/cassini_to_device.py --self-test` |
| **[`inspect_fw.py`](inspect_fw.py)** | Validate `HLKJ` bootloader header, partition directory, and CRC-16/CCITT-FALSE checksums on 2 MiB SL6801 images | `python3 tools/inspect_fw.py dumps/firmware_2M_openfw.bin` |
| **[`bt_probe_report.py`](bt_probe_report.py)** | Markdown/JSON report of the Bluetooth Exp 1 stage A HCI probe (`HCIP` lines from `dev_cycle.py log`; image `make -C open_fw BT=1 HCI_PROBE=1`, see `docs/BT_HW_RESULTS.md`) | `python3 tools/bt_probe_report.py --self-test` |
| **[`rf_audio_check.py`](rf_audio_check.py)** | "Does the radio change the recording?": WAV captures vs a radio-off baseline (noise floor, 1 Hz Welch spurs at the BT rhythms, envelope modulation, tone SNR / THD+N, continuity, device counters from `BENCH rec` / `+XREC:DBG`), Markdown + JSON pass/fail (`docs/BT_HW_RESULTS.md` "Sync during recording: M2/M3 measurement recipe"; numpy) | `python3 tools/rf_audio_check.py --self-test` |
| **[`test_tools.py`](test_tools.py)** | Offline regression tests for `usb_sync.py`, `bt_sync.py` path-traversal guards, and `fix_exfat_boundary.py` | `python3 tools/test_tools.py` |

## Bluetooth Sync Tools

| Script | Purpose |
| :--- | :--- |
| **[`bt_sync.py`](bt_sync.py)** | Classic BT RFCOMM `+XREC v5` pairing, file sync, RTC sync, and OTA client (works with `fw_mod` and targeted for `open_fw`'s upcoming BT connectivity) |
| **[`bt_companion.py`](bt_companion.py)** | Classic Bluetooth companion & HFP/A2DP audio capture helper for the stock firmware |

## Stock Firmware & Reverse-Engineering Tools (Optional / Legacy)

| Script / Folder | Purpose |
| :--- | :--- |
| **[`fix_exfat_boundary.py`](fix_exfat_boundary.py)** | Safe in-place MBR + exFAT VBR `-1 cluster` shrink to fix the factory firmware's 15-second Linux USB disconnect loop (always run `--dry-run` first) |
| **[`usb_rtc_sync.py`](usb_rtc_sync.py)** | Zero-flash USB RTC reader/setter for stock Card Reader mode (`301a:2801`), also redirects to `openfw_usb.py` when `1209:0001` is attached |
| **[`patch_firmware.py`](patch_firmware.py)** | Stock `pen133_2.0.65` verifier, binary patcher (`sdio-end-of-disk`, `wav-blockalign`, BT patches), XIP payload injector, and `0:\update.up` packager |
| **[`assets.py`](assets.py)** | LVGL v8.3 artwork extractor & patcher for stock `pen133_2.0.65` firmware dumps |
| **[`run_stock_qemu.py`](run_stock_qemu.py)** | Boot stock `pen133_2.0.65` firmware in SL6801 QEMU with ViewTask patches and capture a screenshot |
| **[`fw_analysis/`](fw_analysis/)** | Exploratory disassembly & NVRAM analysis scripts behind [`docs/FIRMWARE_ANALYSIS.md`](../docs/FIRMWARE_ANALYSIS.md) |
