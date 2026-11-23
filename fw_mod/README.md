# `fw_mod/` — Optional XIP Patches for Stock Firmware (`pen133_2.0.65`)

> **Note:** For new development and daily use, see the standalone open-source firmware in **[`open_fw/`](../open_fw/)**. This directory preserves our earlier optional XIP C extension and patch set for the original closed-source `pen133_2.0.65` factory firmware.

## What's Included

- **`bt_filesync.c`** — Thumb-2 XIP extension injected into unused flash (`0x00D74000`) that adds encrypted `+XREC v5` file sync, pairing PIN, RTC sync, and OTA over Classic Bluetooth RFCOMM Channel 1 (see [`docs/XREC_PROTOCOL.md`](../docs/XREC_PROTOCOL.md)).
- **`sl6801_sdk.h` / `custom_ui.c`** — Minimal C SDK header and example demonstrating LVGL v8.3 hooks inside the stock firmware's `ViewTask`.
- **`xip.ld` / `Makefile`** — Linker script and build rules that compile `bt_filesync.bin` and invoke [`tools/patch_firmware.py`](../tools/patch_firmware.py) with stock bug fixes (`sdio-end-of-disk`, `wav-blockalign`, `bt-*`).

## Building

```bash
# Compile the Thumb-2 XIP payload and check the SDK example (no stock dump required)
make -C fw_mod all

# Produce a patched 2 MiB image + 0:\update.up (requires your own ../dumps/firmware_2M.bin)
make -C fw_mod firmware
```
