# Cassini OpenFW — Development, Profiling & On-Silicon Iteration Playbook

Because the **Smartlink SL6801** has no public vendor datasheet and `DWT->CYCCNT` is unavailable, guessing from code or host benchmarks routinely misleads. This playbook captures the closed-loop testing, hardware-probing, and optimization techniques proven on real `HW_76` silicon.

---

## 1. Host-First Verification Gate (Always Run Before Flashing)

Never flash untested code. Every change must pass the full zero-warning ARM build, host ASan + UBSan test suite, `fsck.exfat` filesystem check, and Python tool self-tests:

```bash
make -C open_fw all test-host-san \
  && python3 tools/openfw_usb.py --self-test \
  && python3 tools/dev_cycle.py --self-test
```

*(If you also have your local `dumps/firmware_2M.bin` dump present, include `update-up` in the `make` command to pack the full 2 MiB flash image and `update_openfw.up` OTA bundle.)*

What this catches in seconds on x86-64:
- **Memory & UB bugs:** `-Wall -Wextra -Werror -fsanitize=undefined,address` across `test_core`, `test_drivers_mmio`, `test_ui_render`, `test_audio_dsp`, `test_player`, and `test_audio_bt_mmio`.
- **exFAT corruption:** Creates real formatted images, runs multi-cluster WAV writes, checkpoints, atomic replaces, and validates with `fsck.exfat -n`.
- **UI regressions & heap blowups:** `test_ui_render` drives the real firmware LVGL + EEZ code through all 14 screens, the hardware-scroll screen push (checked before and after every scroll step), scroll animations, and overlays, asserting exact pixel dirty-budgets, toast pill bounds (catching LVGL `LV_DPI_DEF` default-size traps), and peak TLSF heap (`<= 18 KiB` on 64-bit host).
- **Pixel-exact UI refactors:** When optimizing EEZ-generated screens or LVGL styles (e.g., deduplicating redundant default style calls in `open_fw/tools/gen_eez_project.py`), save a baseline contact sheet before editing and diff after:
  ```bash
  cp /tmp/sl6801_openfw_san/ui_all_screens.ppm /tmp/baseline_ui.ppm
  # ... apply optimization & re-run test-host-san ...
  cmp /tmp/baseline_ui.ppm /tmp/sl6801_openfw_san/ui_all_screens.ppm
  ```

### Memory Budget Checks
Inspect ELF section sizes after any significant change:
```bash
size -A /tmp/sl6801_openfw/open_fw.elf | grep -E "ram_boot|\.text|\.data|\.bss|Total"
nm -n /tmp/sl6801_openfw/open_fw.elf | grep -E "_ebss|_sdata|_edata"
```
Keep hard margins on:
- **`.ram_boot`:** `<= 4,096 B` (loaded into early SRAM by `HLKJ`).
- **`_ebss`:** Must remain strictly below **`0x00850000`** (start of the 32 KiB SRAM experiment arena `g_exp_arena`).

---

## 2. Closed-Loop On-Silicon Profiling (Zero-Wire Telemetry)

When optimizing latency, throughput, or power, always measure directly on the SL6801 instead of relying on estimates:

1. **Instrument with `us_timer_now()` + `openfw_log_*()`:**
   - Read the hardware 1 MHz 64-bit counter (`us_timer_now()`, `0x40082104`/`0x40082108`) around the exact phase under test.
   - Log compact one-line `KEY=VAL` metrics into the 4 KiB SRAM ring (`g_openfw_log`, safe even after `PD0` switches from UART0 to the SARADC key ladder).
   - Existing built-in telemetry lines to watch:
     - `BOOT_US LCDB=... SD=... MNT=... LCDF=... SCAN=...`
     - `CPU1=ON WFE=1`
     - `UI_FIRST_US=... LOAD=... DRAW=... BLIT=... HEAP=... WFE_W=...`
     - `USB_RST HS=...`
     - `SDW_*` / `REC_ERR` (one-line SDIO/recorder error diagnostics)
2. **Flash safely over USB (preserves `HLKJ` and `PSMP` NVRAM):**
   ```bash
   sudo python3 tools/dev_cycle.py flash
   ```
   `dev_cycle.py flash` automatically reboots OpenFW (`1209:0001`) into BootROM DFU (`301a:2800`), flashes only `0x00D000..end_off` (never touching `HLKJ` at `0x000000..0x00CFFF` or `PSMP` NVRAM at `0x1FC000..0x1FFFFF`), verifies both hardware CRC-16 and full readback, reboots into OpenFW, and prints the live SRAM log.
3. **Pull live telemetry anytime without re-flashing:**
   ```bash
   sudo python3 tools/dev_cycle.py status
   sudo python3 tools/dev_cycle.py log
   ```
4. **Act on the dominant term, then re-measure:** Always confirm the delta on silicon. Note that single-threaded LVGL software rasterization runs ~`208×` slower on 96 MHz Cortex-M4 than on x86-64 host tests, so host timings are valid for relative A/B deltas only once calibrated against silicon.

---

## 3. 2-Second Hardware Probing via SRAM Experiments (`DEV_EXP=1`)

When a silicon register, FIFO size, clock mux, IRQ line, or SD card write path is undocumented, **do not guess or burn flash cycles** — run a small C probe directly in the 32 KiB SRAM arena (`0x00850000`):

1. **Build & flash a `DEV_EXP=1` image once:**
   ```bash
   make -C open_fw DEV_EXP=1 all update-up
   sudo env OPENFW_FIRM_BIN=/tmp/sl6801_openfw_devexp/open_fw_firm.bin \
     python3 tools/dev_cycle.py flash
   ```
2. **Write a minimal probe against `open_fw/exp/exp_sdk.h`** (see `open_fw/exp/exp_probe.c` for a template) and run it in ~2 seconds:
   ```bash
   sudo env LLD=/path/to/ld.lld OBJCOPY=/path/to/llvm-objcopy \
     python3 tools/dev_cycle.py exp /path/to/my_probe.c
   ```
   - `openfw_exp_api_t` exposes `us_timer_now`, `log_*`, `pmu_*`, `keys_*`, `lcd_*`, `sd_write_file`, `audio_*`, and `bt_*`.
   - If a probe triggers a fault or watchdog reset, OpenFW's crash handler stores `PC`/`LR`/`CFSR`/`BFAR`/`IN_EXP=1` in `AON_BACKUP` and auto-recovers back into USB `1209:0001`.
3. **Real discoveries made in seconds with this technique:**
   - Probing `MUSB+0x1F` (`FIFOSIZE`) per `INDEX` revealed EP1/EP3 only have `64 B` FIFOs (`0x66`) while **EP2 has `2048 B` TX + `2048 B` RX FIFOs (`0xBB`)** — avoiding a dead-end attempt at 512 B packets on EP1 and yielding an immediate `+15%` USB read boost (`696 → 810 KB/s`).
   - Running a 12-instruction CPU1 stub proved Cortex-M4 `SEV` from CPU0 does **not** wake CPU1 from `WFE`, leading directly to the Hardware IPC Mailbox (`0x40070000`, IRQ `55`) + `SEVONPEND` wake path.
   - Calling `api->sd_write_file()` with `16 KiB` verified live `CMD25` multi-sector exFAT writes on the real microSD card (`21.4 ms`, `0` errors) without needing physical button presses.

> **Always re-flash a normal production build** (`make -C open_fw all update-up && sudo python3 tools/dev_cycle.py flash`) when finished probing so `DEV_EXP=1` / `OPENFW_UNATTENDED_DEV` are not left enabled.

---

## 4. Fast Stock-Binary Verification (Before Writing Bare-Metal Drivers)

When local dumps (`dumps/firmware_2M.bin` or `dumps/bootrom_384K.bin`) are available, verify exact CRU clock/reset IDs, NVIC IRQs, and hardware sequences in one command using `clang` `.incbin` + `llvm-objdump`:

```bash
python3 - << 'EOF'
import subprocess
from pathlib import Path

off, sz = 0xe7360, 0xf0
Path("/tmp/dis.s").write_text(
    f'.syntax unified\n.thumb\n.section .text\n.incbin "dumps/firmware_2M.bin", {off}, {sz}\n'
)
subprocess.run(["clang", "--target=thumbv7em-none-eabi", "-mcpu=cortex-m4", "-mthumb",
                "-c", "/tmp/dis.s", "-o", "/tmp/dis.o"], check=True)
print(subprocess.run(["llvm-objdump", "-d", "--triple=thumbv7em-none-eabi", "/tmp/dis.o"],
                     text=True, capture_output=True).stdout)
EOF
```

Use literal-pool searches (`struct.pack("<I", mmio_base)`) to locate peripheral drivers immediately (e.g., `0x40070000` quickly yielded `CRU_RST_IPC_MBOX=0x28`, `CRU_CLK_IPC_MBOX=0x2E`, and NVIC IRQs `54`/`55`).

---

## 5. High-Impact Architectural Rules Learned on SL6801

1. **Always self-verify hardware features with automatic fallback:**
   - **Dual-core CPU1 (`cpu1_ui.c`):** Boot-probes CPU1 and falls back to synchronous CPU0 rendering on single-core QEMU or fault.
   - **CPU1 `WFE` sleep:** Self-tests IPC Mailbox IRQ `55` (`NVIC_ISPR1` bit 23) on boot and only enables `WFE` sleep when verified (falling back to NOP backoff otherwise).
   - **USB MSC speed:** Checks `MUSB_POWER.HS_Mode` (`0x10`) on every USB bus reset, switching between 512 B HS and 64 B FS automatically (stalling `DEVICE_QUALIFIER` in FS mode to keep Linux `dmesg` clean).
   - **SDIO writes:** Narrow `sdio_wait_card_ready()` to true write-fatal R1 bits (`0x04380000`) and fall back automatically from `CMD25` to single-block `CMD24` on error.
2. **Respect dual-core AHB/XIP bus contention:** Never tight-poll MMIO (`us_timer_now()`) on CPU0 while CPU1 is actively blitting to the LCD or rasterizing LVGL; yield with `OPENFW_WFI()` in `cpu1_ui_sync()` (`19.8 ms → 10.4 ms` blit time).
3. **Overlap mandatory hardware delays with useful work:** ST7735S requires a `120 ms` `SLPOUT` wait — running `sd_init()` + `exfat_mount()` + `reload_files_and_sd_stats()` (`~95 ms`) inside that window cut cold boot by `76 ms` (`365 ms → 289 ms`).
4. **Keep LVGL widgets explicit and cheap:** Always set explicit sizes (or `LV_SIZE_CONTENT`) on manually created LVGL objects (`lv_obj_create` defaults to `LV_DPI_DEF × LV_DPI_DEF`), enable `LV_OBJ_STYLE_CACHE`, cache high-frequency nav screens (`HOME`/`FILES`/`SETTINGS`), and deduplicate default style calls in EEZ generation (`gen_eez_project.py`).
5. **USB block throughput benchmarking:** Verify real USB MSC throughput with direct I/O (`sudo dd if=/dev/sdX of=/dev/null bs=64k count=64 skip=64 iflag=direct`) and check `sudo dmesg | tail` for clean enumeration with zero SCSI errors or speed warnings.
