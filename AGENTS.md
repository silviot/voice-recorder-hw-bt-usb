# `open_fw` Engineering Rules & Invariants

## Silicon, Flash & Boot Safety
- **Header-last flashing:** Any host flasher or updater touching `0x00D000` (partition table/boot header) MUST erase/invalidate `0x00D000` first, program `0x00E000..end` next, and write `0x00D000` **last** so interrupted flashes fall back to `301a:2800` BootROM even when `PD0` is on the SARADC button ladder.
- **Strict USB PID guard:** Never send BootROM commands to `301a:2801` (MSC card-reader mode); only talk BootROM protocol to `301a:2800`.
- **Keep CPU1 hot loops in SRAM:** The SL6801 has no XIP flash cache. Place CPU1 main loops, fault handlers, hot LVGL/LCD draw paths, and BT controller/SBC hot loops in SRAM (`.fast_ramtext`, `.ramtext`, `.bt_fast.text`) to prevent CPU1/CPU0 XIP fetch deadlocks.

## Concurrency, Interrupts & LVGL
- **No long `PRIMASK` locks:** Never use `OPENFW_IRQ_SAVE` / `__disable_irq()` around exFAT, recorder, or UI work. Use FreeRTOS `BASEPRI` (`taskENTER_CRITICAL`, threshold `0x80`) for shared state, and keep audio ADC/DAC DMAC IRQs (61/60) at priority `0x00` so they preempt BT (`0x80`) and are never masked.
- **Zero SD/file I/O on the LVGL/UI path:** Keep `LV_USE_FS_* = 0`. Never read exFAT/SD files or reopen assets inside LVGL render callbacks or CPU1 UI code; CPU0 owns all storage I/O and publishes lock-free `ui_model_t` snapshots.
- **Static LVGL memory only:** Keep LVGL heap and draw buffers statically sized in SRAM (`LV_MEM_POOL_EXPAND_SIZE = 0`) with compiled-in fonts.

## Storage, USB MSC & Recording Integrity
- **MSC/exFAT boundary:** Flush and unmount/remount exFAT cleanly around `run_usb_msc_mode()`, zero `cbw->signature` after handling a SCSI CBW, and verify local exFAT writes vs. USB MSC host reads with SHA-256 parity tests.
- **Battery & file-size guards:** Preserve the `< 3450 mV` low-battery recording cutoff (with 100 mV hysteresis), the 2 GiB WAV auto-split (`REC_MAX_PCM_BYTES`), and free-cluster remaining-time reporting.
- **Cleanroom licensing:** `open_fw` is MIT. Never vendor source from GPL/unlicensed reference repos (Rockbox, `smartlink_flash`, Betaflight, etc.); adopt only wire protocols, test invariants, and architectural patterns cleanly.

## Bluetooth, Timers & UX
- **Strict BT privacy:** Keep Classic BT SSP MITM-only (purge non-MITM bonds), auto-close discoverability after pairing/failure, and never re-open unauthenticated file access on disconnect.
- **Wrap-safe timers:** Use 64-bit `us_timer_now()` for microseconds and wrap-safe subtraction (`(int32_t)(millis() - deadline) >= 0`) for 32-bit `millis()`.
- **Auto-resume durability:** Commit playback position (`apmem`) periodically, on pause, and on every track/mode switch—not only on explicit Stop.
