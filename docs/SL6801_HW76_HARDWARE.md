# Smartlink SL6801 (`HW_76` / `HW-A1`) Hardware & Bare-Metal Reference

This document is a clean-room hardware reference for the **Smartlink SL6801** voice recorder (`HW_76` board / `HW-A1` enclosure). It summarizes empirically verified memory maps, peripherals, pin assignments, boot/DFU workflows, and silicon quirks needed to write bare-metal firmware (`open_fw`) or host tooling for the device.

---

## 1. System Overview

| Subsystem | Specification |
| :--- | :--- |
| **SoC** | Smartlink **SL6801** |
| **CPU Core** | ARM **Cortex-M4F** (`armv7e-m`) with hardware single-precision FPU (**`fpv4-sp-d16`**), running at **96 / 120 / 192 MHz** (HOSC on cold boot) |
| **Internal Memory** | **384 KiB Mask ROM** (`0x00000000..0x0005FFFF`), **512 KiB SRAM** (`0x00800000..0x0087FFFF`) |
| **Flash Storage** | **2 MiB SPI NOR** memory-mapped via XIP at `0x00C00000..0x00DFFFFF` |
| **External Storage** | microSD slot on 1-bit SDIO (`PD4` / `PD5` / `PD6`), exFAT / FAT32 |
| **Audio Input** | **Dual differential analog microphones** (`AMIC1 + AMIC2`, `18 mm` spacing), 16-bit & 24/32-bit ADC up to 48 kHz |
| **Audio Output** | Stereo DAC (`DAC0 + DAC1`) feeding both the 3.5 mm headphone jack (`PB11` active-low detect) and built-in front speaker via external PA enabled on **`PB30`** (internal pull-up ON / Hi-Z OFF) |
| **Display** | **80 × 160** SPI/DBI TFT (**ST7735S**, landscape `MADCTL = 0xA8`, `X_OFF = 1`, `Y_OFF = 26`), PWM backlight |
| **USB** | Mentor **MUSB** USB 2.0 Full-Speed Device (`0x40040000`), Mask ROM DFU (`VID:PID 301a:2800`) |
| **Bluetooth** | **BR/EDR Classic only** (no BLE), integrated baseband + 2.4 GHz RF + Mask ROM `RW-BT` HCI controller |
| **Controls** | Active-low slide record switch (`PB6`), 6-button resistor ladder on `PD0` (KADC), PMU power button (`PMU reg 0x31`) |

---

## 2. Memory & Peripheral MMIO Map

### 2.1 Address Space & Flash Layout
- `0x00000000..0x0005FFFF` — Stage-0 Mask BootROM (384 KiB)
- `0x00800000..0x0087FFFF` — On-chip SRAM (512 KiB)
- `0x00C00000..0x00DFFFFF` — SPI NOR XIP window (2 MiB):
  - `0x000060..0x00C950` — Stage-1 `HLKJ` bootloader (copied to SRAM `0x0081FBC0`)
  - `0x00D000` — Partition table
  - `0x00E000..0x173E8C` — Main `FIRM` image
  - `0x1FC000..0x1FFFFF` — `PSMP` NVRAM key-value store
  - **Critical XIP Window Size Register (`0x30000024` = `NOR_XIP_SIZE_CFG`):** Bits `[6:4]` set the AHB XIP decode window size (`0x00` = 128 KiB, `0x10` = 256 KiB, `0x20` = 512 KiB, `0x30` = 1 MiB, **`0x40` = 2 MiB**, `0x50` = 4 MiB). Writing `0x00` truncates XIP at `0x00C20000` (all reads at `0x00C20000+` return `0x00` cached / `0xFF` uncached). Always write `0x30000024 = 0x40` for full 2 MiB XIP, and `0x30000804` (`XIP_CACHE_CTRL1`) = `0x0FF00001` (bits `[28:20] = 0x0FF` for 2048 KiB in 8 KiB units, bits `3` and `2` cleared). `0x30000204` bit 8 (`0x100`) is a self-clearing XIP bridge strobe (normally reads back `0`).

### 2.2 Peripheral Base Addresses
| Base Address | Peripheral | Notes |
| :--- | :--- | :--- |
| `0x40000000` | **SYSCTL** | Includes `SYSCTL_USB_CTRL` (`+0x00`) and codec SRAM control |
| `0x40001000` | **DMAC** | 8 channels, NVIC IRQs `60..67` (CH1 used for Audio ADC RX) |
| `0x40003000` | **SDIO** | DesignWare-style SD/MMC controller (PIO + IDMAC), 1-bit bus on `PD4/PD5/PD6` |
| `0x40009000` | **Audio Codec** | Dual-mic ADC, stereo DAC, internal codec SRAM `0x40009400..0x400095FF` |
| `0x40010000` | **BT RF** | 2.4 GHz RF front-end registers |
| `0x40011000` | **BT Modem** | Digital GFSK/EDR modem |
| `0x40020000` | **BT Baseband (`RW-BT`)** | Classic BR/EDR link controller, NVIC IRQ `26` |
| `0x40033000` | **BT Exchange Memory** | Shared descriptor/TX/RX buffer RAM for `RW-BT` |
| `0x40040000` | **Mentor MUSB** | USB 2.0 controller (NVIC IRQs `41..43`) + USB PHY (`0x40040300..0x40040308`) |
| `0x40080000` | **CRU** | Clock & Reset Unit (PLLs, clock muxes, peripheral reset controls) |
| `0x40081000` | **GPIO** | Port muxes, direction, input/output registers |
| `0x40082000` | **HW Timers** | 4-channel timer block (stride `0x20`, IRQs `8..11`); **1 MHz 64-bit monotonic timer** at `0x40082104` / `0x40082108` |
| `0x40083000` | **Watchdog (WDT)** | Hardware watchdog (`WDT_FEED = 0x40083010`, `WDT_MODE = 0x40083014`, `WDT_CFG = 0x40083018`, `WDT_CTRL1 = 0x4008301C`) |
| `0x40084000` | **PWM** | 6 channels; channel 3 (`0x40084080`) drives LCD backlight |
| `0x40085000` | **AON / RTC / PMU RTWI** | Pure-binary RTC (`+0x04/+0x08`, Alarm 1 `+0x20/+0x24` IRQ `47`), 32 kHz RC freq (`+0x84/+0x88`), PMU RTWI bridge (`+0x100..+0x110`, slave `0x30`), 16× 32-bit AON backup regs (`0x40085180..0x400851BC`) |
| `0x40095000` | **LCDC** | DBI display controller (FIFO `0x40095030`) |
| `0x40096000` | **KADC** | 10-channel key/resistor-ladder ADC |

---

## 3. Controls, Pinout & User I/O

### 3.1 Slide Record Switch & Power Button
- **Slide Record Switch:** **`PB6`**, active-low (`0` = Record / Start, `1` = Stop & Save).
- **Power / ON-OFF Button:** Monitored through PMU IRQ Status 1 (**`PMU reg 0x31`**, W1C):
  - Bit 6 (`0x40`) = press, Bit 5 (`0x20`) = long-press, Bit 4 (`0x10`) = release.
- **Headphone Jack Detect:** **`PB11`**, active-low (`0` = headphones inserted).

### 3.2 Front/Side Buttons (`PD0` KADC Resistor Ladder)
All 6 tactile buttons share a single analog resistor ladder on **`PD0`**:

| Button | ADC Range (12-bit KADC) |
| :--- | :--- |
| **Prev** | `0 .. 335` |
| **Next** | `336 .. 847` |
| **VOL+** | `848 .. 1423` |
| **VOL-** | `1824 .. 2175` |
| **Back** | `2176 .. 2559` |
| **Play / OK / M** | `2560 .. 2959` |

---

## 4. Audio Subsystem

- **Audio PLL (`0x40080010` / `0x40080014`):** Programmable to **24.576 MHz** (for 16 / 32 / 48 kHz families) or **22.5792 MHz** (for 44.1 kHz).
- **Dual-Mic Capture (`AMIC1 + AMIC2`, `0x40009200`) — Live-Verified on Silicon:**
  - `AMIC1` differential input is muxed on **`PB12` / `PB13`** (analog mux 15).
  - Both mics (`ADC_CH1` + `ADC_CH2`) can be captured simultaneously by configuring `CODEC_ADC_CH_MUX (0x40009020) = 0x000D0D00` (`0x00000D00` for mono `AMIC1` only).
  - **Critical channel control bit (`ADC_CHx_CTRL`):** Channel control registers have stride `0x30` (`CH0=0x40009220`, `CH1=0x40009250` for `AMIC1`, `CH2=0x40009280` for `AMIC2`, `CH3=0x400092B0`). Each active channel **must** be set to **`0x09`** (`bit 0 = ADC_CHx_EN | bit 3 = ADC_CHx_FIFO_EN`); without bit 3 no samples enter the RX FIFO and DMAC stalls after draining stale FIFO words.
  - **`ADC_DIG_CTRL (0x40009200)`:** Bit 0 (`0x01`) is a write-1 **self-clearing soft-reset pulse** (reads back `0`); bits `[11:8]` select the sample rate (`3` = 16 kHz, `7` = 48 kHz, `8` = 44.1 kHz); bit 1 (`0x02`, `ADC_DIG_EN`) is enabled **last**. `ADC_FIFO_CTRL (0x40009208)` sets bit 2 (`0x04` = 16-bit) and bit 7 (`0x80` = `DMA_EN`).
  - **DMAC RX (CH1, IRQ 61, RX FIFO `0x4000920C`):** Supports 16-bit and 24/32-bit PCM with `DMAC_CTL = 0x402A0281` and `XFER_CNT = 0xFF000000 | (len & 0x3FFFF)`. Live stereo `AMIC1 + AMIC2` 16 kHz capture is silicon-verified (`-29.0 / -29.1 dBFS`).
  - *Note on stock recordings:* Stock firmware enables an aggressive on-device voice DSP chain at `512 kbps` (350 Hz HPF + 2.2 kHz `+9 dB` high-shelf + `-15 dB` STFT noise suppressor + `2:1` AGC). For modern ASR models like **NVIDIA Parakeet** or **Cactus Whistle**, capturing clean raw audio with only a gentle ~75 Hz HPF and safety limiter gives significantly better accuracy; see [CLEAN_ROOM_AUDIO_PLAN.md](CLEAN_ROOM_AUDIO_PLAN.md).
- **Speaker & Headphone Playback (`DAC0 + DAC1`, `0x40009100`) — Live-Verified on Silicon:**
  - Stereo `DAC0 + DAC1` playback at 16 kHz (APLL `24.576 MHz`) is silicon-verified via **DMAC CH0 TX** (TX FIFO `0x4000910C`, `DMAC_CTL_DAC_TX_BASE = 0x002812A0`, `XFER_CNT = 0xFF000000 | (len & 0x3FFFF)`).
  - Apply factory eFuse DC-offset trims from **eFuse words 2 & 3** to `DAC0_DC (0x40009064)` (`{W3[3:0], W2[31:29]}`) and `DAC1_DC (0x40009068)` (`W3[10:4]`).
  - **Built-in speaker PA (`PB30`):** Shares the same `DAC0 + DAC1` output (`DAC2` is unused) through an external PA enabled by **`PB30`** (`GPIOB_BASE = 0x40081400`, pin 30) configured as `mux = 0` (input/Hi-Z): internal pull-up ON (`rom_gpio_config(0x0001F008u)` -> `GPIOB+0x28[29:28] = 2`) turns the PA **ON**; no-pull/Hi-Z (`rom_gpio_config(0x0001F000u)` -> `0`) turns the PA **OFF** via external pull-down. Keep `PB30` OFF when headphones are inserted (`PB11 == 0`) and during DAC analog depop ramps.

---

## 5. Power Management (PMU), Watchdog & Reset Domains

The SL6801 communicates with its companion PMU over the internal **RTWI** bus (`0x40085100..0x40085110`, I2C slave `0x30`).

### 5.1 USB VBUS Detection & Always-On Operation
- **`PMU reg 0x2A`** is a live hardware VBUS comparator (read-only):
  - **Bit 2 (`0x04`) = `1`** while VBUS is connected / **CHARGING**.
  - **Bit 6 (`0x40`) = `1`** when battery is **FULL** (or intermediate VBUS) while VBUS remains connected.
  - Check **`(pmu_read(0x2A) & 0x44) != 0`** as the authoritative test for "USB 5V VBUS is present" (and `== 0` for unplugged on battery).
- **No hidden power-off timers:** The PMU has **no** idle auto-shutdown timer, **no** low-current cutoff, **no** charge-complete auto-poweroff, and **no** internal PMU watchdog. Power is only cut on explicit software power-off (`PMU[0x21] |= 0x03` / AON `0x400850D0 bit 0`), battery UVLO without VBUS, or holding ON/OFF for the long-press timer (`PMU reg 0x24[3:2]`, 6–12 s) while `PMU reg 0x00[7:6] != 0`.
- **`PMU reg 0x00` safety rule on `HW_76` (`pmu_cfg[0x0C] == 1`):** **NEVER** clear **bit 3 (`0x08`)** — it is a rail/LDO power-hold bit on this board. Only clear bits `[2:1]` (**`&= ~0x06`**).

### 5.2 Reset Domains & AON Backup Registers (`0x40085180..0x400851BC`)
The chip has **two distinct hardware reset paths**. Standard Cortex-M **`SCB->AIRCR = 0x05FA0004` (`SYSRESETREQ`)** has zero callers in BootROM, `HLKJ`, and stock `FIRM` — it only resets core logic without re-sequencing PMU rails, PLLs, USB PHY, or SPI NOR XIP, leaving peripherals/XIP hung.

1. **Universal Clean PMU Cold Reset (works out of BOTH `FIRM` and `301a:2800`, Stage-0 & `HLKJ`-entered):**
   - Drops and re-sequences SoC rails (cleanly resets PLLs, CRU, `0x40000004/08`, USB PHY, and SPI NOR XIP).
   - **Required sequence (from SRAM with IRQs disabled):**
     1. `PMU[0x22] = 0x00;`
     2. `PMU[0x20] = 0x2F;` *(wakeup/restart enable mask `bits[5:0]`)*
     3. Latch `PMU[0x1B]` bit 7: `PMU[0x1B] |= 0x80;` then `PMU[0x1B] &= ~0x80;`
     4. `PMU[0x20] = 0xAF;` *(`0x2F | 0x80`)* and spin in SRAM.
   - *(In `301a:2800` RAM stubs, RTWI is already initialized and BootROM exposes `rom_pmu_write = 0x00003C69` and `rom_pmu_read = 0x00003CA5`.)*
   - **Why `PMU[0x20] |= 0x80` alone powers off the board:** If `PMU[0x20][5:0]` was not initialized to `0x2F` (e.g., straight out of Stage-0 BootROM), asserting bit 7 (`0x80`) drops the rails with no restart/wakeup enable and powers the device off instead of rebooting.
   - **Persistence:** **Preserves** sticky PMU IC flags (`PMU[0x23]`, `PMU[0x00]`), but **clears** SoC `AON_BACKUP(0..15)`.
2. **SoC Digital WDT Reset (`0x40083000`, `WDT_MODE = 0x2001`):**
   - Resets digital CPU/bus/peripherals **without** cutting PMU rails, so **`AON_BACKUP(0..15)` (`0x40085180..0x400851BC`) are preserved** (ideal for crash dumps and boot-loop counters; all 16 slots are untouched by BootROM and Stage-1 `HLKJ`, and stock `FIRM` only uses slot 6 `0x40085198 = 0x5A`).
   - **Sequence:** Always write `WDT_CFG (0x40083018) = 0` **first**, then `WDT_EN |= 1`, `WDT_MODE = 0x2001`, `WDT_CFG = (timeout_idx << 4) | 1`, `WDT_CTRL1` (1-second tick count from `0x40085084`/`0x40085088`, reset default `0x007FFF00`). Feed with `WDT_FEED (0x40083010) = 0x28000001`.
   - **Clock/PLL teardown requirement:** Because `WDT_MODE = 0x2001` is digital-only and Stage-0 BootROM assumes HOSC + 1-bit SPI NOR, triggering it while PLLs/XIP are active hangs on reboot unless you first tear down CPU/AHB/QSPI clocks back to HOSC and disable PLLs **from SRAM** (matching `HLKJ 0x008284D6` or BootROM `0x00003BF1`, plus `*(0x400850D8) &= ~0x02`).

---

## 6. USB 2.0 Device Mode & DFU Bootloader (`301a:2800`)

### 6.1 Entering & Exiting USB DFU (`301a:2800`)
- **Hardware key combo (Stage-0 Mask BootROM):** Hold **Prev** (or **Next** depending on board revision) while plugging in USB / powering on.
- **Software reboot into DFU (via Stage-1 `HLKJ`):**
  1. Set `PMU[0x23] |= 0xC0` and `PMU[0x00] |= 0x01`.
  2. From SRAM with IRQs disabled, execute the universal PMU reset (`PMU[0x22] = 0x00`, `PMU[0x20] = 0x2F`, latch `PMU[0x1B]` bit 7, `PMU[0x20] = 0xAF`) and spin. On the next boot, Stage-1 `HLKJ` unconditionally clears the flag and enters `301a:2800`.
- **Clean exit from `301a:2800` (both Stage-0 & `HLKJ`-entered):** Upload and `RUNRAM` a tiny stub that clears `PMU[0x23] &= ~0xC0` / `PMU[0x00] &= ~0x01` and runs the universal PMU reset above via BootROM helpers `0x00003C69` / `0x00003CA5`.
- **Unsupported BootROM vendor subcommands (`0xC0`):**
  - **`CMD_SL_RESET (0x83)`:** Unimplemented in BootROM `301a:2800` — falls through a NULL table pointer to vector `[0x18]` (UsageFault `0x0004C2CA: b .`) and **hangs the chip**.
  - **`SUBCMD_PING (0x86)`:** Stalls EP1.

### 6.2 Critical USB PHY Wrapper Quirks (`0x40000000` & `0x40040300`)
1. **`SYSCTL_USB_CTRL (0x40000000)` bit 5 (`0x20`):** Hardware POR strap bit (`1`). Always **read-modify-write** `*(0x40000000) |= 0x41` (yielding `0x00000061`); never blind-write `0x41`.
2. **`USB_PHY_CTRL (0x40040300)` bits `[7:4]` (analog bias control) & bit 12 (`0x1000`):** Bits `[7:4]` reset to `0b0001` (`0x10`, bit 4 = `1`), which is required by the USB PHY analog bias block. **NEVER** blind-write `0x00001000`; always RMW:
   ```c
   *(volatile uint32_t *)0x40040300u =
       (*(volatile uint32_t *)0x40040300u & ~0x1F3u) | 0x1010u;
   ```
   Follow with a **50 ms settle delay** before configuring MUSB FIFOs and asserting `POWER.SOFTCONN`.

---

## 7. Timers, RTC & FPU Quirks

- **Microsecond timing:** Use the **1 MHz 64-bit hardware timer** at `0x40082104` (low) / `0x40082108` (high). Do **not** rely on ARM `DWT->CYCCNT` (unused in stock BootROM/firmware and reported frozen on sibling SL6806 silicon).
- **RTC:** Pure-binary counter in `0x40085004`/`0x40085008`, with Alarm 1 at `0x40085020`/`0x40085024` (NVIC IRQ `47`).
- **Cortex-M4F Hardware FPU (`fpv4-sp-d16`):** Confirmed present and **live-verified on silicon** (single-precision VFPv4-SP hard-float works live; BootROM contains **5,544** VFP instructions, stock `FIRM` contains **5,788**, and Stage-1 `HLKJ` contains 18). Bare-metal firmware built with `-mfpu=fpv4-sp-d16 -mfloat-abi=hard` must enable `SCB->CPACR (0xE000ED88) |= 0x00F00000; __DSB(); __ISB();` in `Reset_Handler` before executing any VFP instructions.

---

## 8. Related Open-Source Projects & Online Documentation

There is no public vendor datasheet for the SL6801, but an active open-source community has built tools, emulators, and documentation around the SL6801 and sibling SL6806:

- **[ilyakurdyukov/smartlink_flash](https://github.com/ilyakurdyukov/smartlink_flash)** — Original Linux/MSYS2 USB DFU (`301a:2800`) flash dumper, writer, and RAM payload runner.
- **[anton-veretenenko/smartlink_flash](https://github.com/anton-veretenenko/smartlink_flash)** — Fork with USB timeout/retry fixes and chunked dump/flash/verify scripts (`flash_ranges.py`, `verify_flash.py`).
- **[anton-veretenenko/smartlink-sl6801-emulator](https://github.com/anton-veretenenko/smartlink-sl6801-emulator)** (and companion **[QEMU `sl6801-machine` fork](https://github.com/anton-veretenenko/qemu)**) — QEMU 8.2.2 machine model for SL6801 (CPU, SRAM, XIP, SDIO PIO/IDMAC, DMAC, LCDC, Timers, PWM, PMU RTWI, RTC, KADC) with GDB scripts and GUI. *(Note: its docs swap `0x40080000`/`0x40081000` labels — on silicon `0x40080000` is CRU and `0x40081000` is GPIO, and `0x40033000` is BT Exchange Memory.)*
- **[dbrant/SL6801_player](https://github.com/dbrant/SL6801_player)** — XIP C application SDK, LVGL asset tools, and headless Ghidra auto-setup/auto-naming scripts for SL6801 players.
- **[take-cheeze/sl6806](https://github.com/take-cheeze/sl6806)** — Bare-metal framework for the sibling SL6806 SoC with host-side MMIO-mocked unit tests and a wire-free USB-SCSI debug console. *(Do not copy SL6806 MMIO addresses blindly onto SL6801.)*
- **[victormrreis/yp3-nova-os](https://github.com/victormrreis/yp3-nova-os)** — Experimental LVGL replacement and documentation covering `update.up` packaging, display pipeline, and recovery.
- **[fluctuat/sl6801-kids-player](https://github.com/fluctuat/sl6801-kids-player)** — Clean `clang` Thumb-2 XIP patcher and `DBROOT` card builder.
- **[zeroproj/OpenPod](https://github.com/zeroproj/OpenPod)** — Custom open-source firmware for SL6801 (`yp3_`) MP3 players (overhauled UI, SD updates, embedded album art).
- **[re-3v0lv3d/pemetres](https://github.com/re-3v0lv3d/pemetres)** — Spanish-language documentation and scripts for YP3 / SMTLINK (`SL680x`) players.
- **[tytydraco/mp3c](https://github.com/tytydraco/mp3c)** & **[tytydraco/x264-yp3-patch](https://github.com/tytydraco/x264-yp3-patch)** — Specialized media converters and x264 patches for Smartlink `SL6806` / `yp3` hardware decoders.
- **[coderkei/DBROOT-Manager](https://github.com/coderkei/DBROOT-Manager)** — Viewer/editor for SL6801/SL6806 `DBROOT` database files.
