# Voice Recorder (`HW_76` / Smartlink SL6801) — Hardware, Firmware & Integration Analysis

This repository documents the hardware profile, USB Mass Storage behavior, Linux kernel quirks, Bluetooth interfaces, and open-source firmware ecosystem of the **HW_76** voice recorder / MP3 player, along with scripts and instructions to reliably interact with it from Linux.

The complete instruction-level disassembly reference — dual-core memory map, `PSMP` NVRAM format, vendor `0xC0` SCSI command table, and the full USB/Bluetooth (HFP / A2DP / AVRCP) interaction protocol — is in **[firmware_analysis.md](firmware_analysis.md)**.

---

## 1. Hardware Overview & SoC Identification

From USB and Bluetooth enumeration, this voice recorder is built around the **Smartlink (Shenzhen Shenju Technology / Jointbees) SL6801** SoC (running an ARM Cortex-M4F core with FreeRTOS + an LVGL 8.3 UI, with stock firmware versions prefixed `yp3_` / 云P3).

| Property | Value |
| :--- | :--- |
| **Device Screen Name** | `HW_76` (derived from `HW_` + last byte of BT MAC `76`) |
| **SoC / Chip Vendor** | Smartlink Technology (Shenzhen Shenju Technology / Jointbees) — **SL6801** (QFN-48, dual-core ARM Cortex-M4F with VFP + 2 MB internal XIP SPI NOR flash) |
| **OS & GUI Stack** | FreeRTOS + POSIX-like device layer (`/dev/lcd`, `/dev/audio0`, `/dev/audio1`, `/dev/usbd`, etc.) + **LVGL v8.3** + **FatFs R0.14** (FAT32 & exFAT) |
| **USB Vendor:Product ID** | `301a:2801` (Card Reader Mode) / `301a:2800` (Bootloader / ROM Mode) |
| **USB Manufacturer / Product** | `SmartlinkTechnology` / `USB2.0 Device` |
| **USB Serial Number** | `20201111000001` (uniquely identifies **SL6801**; SL6806 uses `20221008000002` / `20220320000001`) |
| **SCSI Inquiry String** | `SMTLINK  CARDREADER       1.00` (Bootloader mode: `SMTLINK DEVICE 2.00`) |
| **Storage Capacity** | `122,101,760` 512-byte sectors (`62.5 GB` / `58.2 GiB` microSD/flash, partition `sda1`) |
| **Bluetooth MAC Address** | `XX:XX:XX:XX:XX:76` (Classic Bluetooth BR/EDR; last byte matches `HW_76`) |
| **Bluetooth Class of Device** | `0x280408` (Service: Audio + Capturing; Device: Audio/Video -> Hands-free) |

---

## 2. USB Interface & Why It Disconnects on Linux (Firmware Bug Analysis)

### Observed Behavior in `dmesg` (`journalctl -k`)
When plugged in via USB in normal operation, the device enumerates as a Full-Speed USB 2.0 Mass Storage device (`301a:2801`) exposing the 64 GB storage (`sda` / `sda1`):

```text
usb 3-2: new full-speed USB device number 37 using xhci_hcd
usb 3-2: New USB device found, idVendor=301a, idProduct=2801, bcdDevice= 1.00
usb 3-2: Product: USB2.0 Device
usb 3-2: Manufacturer: SmartlinkTechnology
usb 3-2: SerialNumber: 20201111000001
usb-storage 3-2:1.0: USB Mass Storage device detected
scsi 0:0:0:0: Direct-Access     SMTLINK  CARDREADER       1.00 PQ: 0 ANSI: 2
sd 0:0:0:0: [sda] 122101760 512-byte logical blocks: (62.5 GB/58.2 GiB)
 sda: sda1
```

However, **~15 seconds after connecting**, the device abruptly freezes and resets/disconnects in an endless loop:

```text
usb 3-2: USB disconnect, device number 37
sd 0:0:0:0: [sda] tag#0 FAILED Result: hostbyte=DID_ERROR driverbyte=DRIVER_OK cmd_age=15s
sd 0:0:0:0: [sda] tag#0 CDB: Read(10) 28 00 07 47 1f fd 00 00 01 00
I/O error, dev sda, sector 122101757 op 0x0:(READ) flags 0x880700 phys_seg 1 prio class 2
device offline error, dev sda, sector 122101752 op 0x0:(READ) flags 0x800000 phys_seg 1 prio class 2
Buffer I/O error on dev sda, logical block 15262719, async page read
```

### Root Cause: Device Firmware Bug Triggered by Linux End-of-Disk Probing
Decoding the failing 10-byte SCSI Command Descriptor Block (`28 00 07 47 1f fd 00 00 01 00`):
* **Byte 0 (`0x28`):** SCSI `READ(10)` opcode
* **Bytes 2–5 (`0x07471ffd` = `122,101,757`):** Starting LBA (3 sectors before the reported end of the `122,101,760`-sector disk, corresponding to 4 KB logical block `15,262,719` on `sda` or `15,262,711` on `sda1` = sectors `122,101,752..122,101,759`)
* **Bytes 7–8 (`0x0001` = `1`):** Transfer length (`1` sector = 512 bytes)

Why does a valid 1-sector read 3 sectors before the end of the disk crash the device?
1. The factory MBR partition table allocates `sda1` starting at sector `64` with size `122,101,696` sectors (`64 + 122,101,696 = 122,101,760`), reaching the exact final reported LBA of the card.
2. When `/dev/sda` and `/dev/sda1` attach on Linux, **two system daemons** automatically invoke `libblkid` (`blkid_do_safeprobe()`) on the block devices:
   * **`systemd-udevd`** (via `60-persistent-storage.rules` `IMPORT{builtin}="blkid"`).
   * **`fwupd`** (`/usr/libexec/fwupd/fwupd`, caught via `fuser -v /dev/sda1`), which opens every newly attached partition node (`/dev/sda1`) and calls `libblkid` directly to check if it is an EFI System Partition or UF2/android-boot volume.
3. Because `libblkid` checks the final 4 KiB of the device/partition for backup GPT headers and MD-RAID superblocks, it issues a read at LBA `122,101,752..122,101,759`.
4. The SL6801's USB Mass Storage firmware (`301a:2801`) has a boundary bug in its SD card `READ(10)` handler: when asked to read those final sectors of the card, the device's USB task hangs instead of returning data or a SCSI check-condition sense code.
5. After 15 seconds (`cmd_age=15s`) with no response on the bulk-in endpoint, Linux's `usb-storage` error handler issues a USB port reset, causing the device screen to prompt *"Charge only or File transfer"* / reboot its USB stack in an endless 15-second loop.

### Solutions & Workarounds for Reliable USB Access on Linux
1. **Permanent In-Place Fix (Tested & Verified, Zero Data Loss) — [`tools/fix_exfat_boundary.py`](tools/fix_exfat_boundary.py) + [`50-smartlink-sl6801.rules`](50-smartlink-sl6801.rules):**
   * **Step 1 (`udev` rule):** Installing [`50-smartlink-sl6801.rules`](50-smartlink-sl6801.rules) to `/etc/udev/rules.d/50-smartlink-sl6801.rules` disables `systemd-udevd`'s whole-disk `blkid` probe on `/dev/sda` (`ENV{UDEV_DISABLE_PERSISTENT_STORAGE_BLKID_FLAG}="1"`), clamps `max_sectors_kb` to `32`, and grants `uaccess` (`0666`).
   * **Step 2 (In-place MBR + exFAT VBR `-1 cluster` shrink):** Because `fwupd` still probes the end of `/dev/sda1`, running `sudo python3 tools/fix_exfat_boundary.py /dev/sdX` shrinks the MBR partition entry (`0x1BE+12`) and the exFAT Main & Backup Volume Boot Records (`VolumeLength` at `+0x48` and `ClusterCount` at `+0x5C`, plus the repeating 32-bit VBR checksum sector at `+11*512`) by **1 cluster (256 sectors = 128 KiB)**:
     * `VolumeLength`: `122,101,696` -> `122,101,440` sectors (`sda1` now ends at LBA `122,101,504`, safely 256 sectors before the broken LBA `122,101,752..122,101,759` boundary!).
     * `ClusterCount`: `476,935` -> `476,934` clusters (which preserves the exact on-disk exFAT Allocation Bitmap byte size: `ceil(476934 / 8) == 59,617` bytes, so the Linux kernel `exFAT-fs` driver mounts it cleanly with zero warnings and zero data loss!).
2. **Immediate Workaround for Firmware Dumping (`smtlink_dump`):**
   * For firmware dumping (`smtlink_dump`), `libusb` automatically detaches `usb-storage`, which stops all block-layer reads immediately.

---

## 3. Bluetooth Interface (`HW_76`)

The device uses **Classic Bluetooth (BR/EDR)**, not BLE. It operates in two user-selectable modes on the device screen:

1. **Bluetooth Headphone Mode (A2DP Source / Host):**
   * The recorder scans for and pairs with Bluetooth headphones/speakers to play back recordings directly from the recorder.
2. **Bluetooth Hands-Free Mode (HFP v1.6 Unit / Peripheral):**
   * Advertises via Classic Bluetooth Inquiry (`hcitool inq` / `bluetoothctl scan bredr`) as **`HW_76`** (`XX:XX:XX:XX:XX:76`).
   * **SDP Record (`0x10000`):**
     ```text
     Service RecHandle: 0x10000
     Service Class ID List:
       "Handsfree" (0x111e)
       "Generic Audio" (0x1203)
     Protocol Descriptor List:
       "L2CAP" (0x0100)
       "RFCOMM" (0x0003)
         Channel: 1
     Profile Descriptor List:
       "Handsfree" (0x111e)
         Version: 0x0106
     ```
   * **Purpose & Capabilities:**
     * Connects to a phone or computer as a Bluetooth Hands-Free headset.
     * Exposes **RFCOMM Channel 1** (standard HFP AT command channel: `AT+BRSF`, `AT+CIND`, `AT+VGS`, etc.) and bidirectional **SCO audio** (microphone input + speaker output, supporting SBC/mSBC/CVSD).
     * Does **not** expose OBEX File Transfer or BLE GATT file access; stored recording files (`.WAV` / ADPCM in `0:\record`) must be accessed via USB.

---

## 4. Open-Source Ecosystem & Adjacent Community Work

There are three key open-source projects targeting the **Smartlink SL6801 / SL6806 (`yp3_`)** ecosystem:

### 1. [`ilyakurdyukov/smartlink_flash`](https://github.com/ilyakurdyukov/smartlink_flash) (by Ilya Kurdyukov)
* **What it is:** The foundational Linux/Windows C tool (`smtlink_dump` + `fwhelper`) that speaks the SL6801/SL6806 custom vendor SCSI commands (`CDB[0] == 0xC0`) over USB (`301a:2800` bootloader mode and `301a:2801` card reader mode).
* **Capabilities:**
  * Detaches the broken Linux `usb-storage` kernel driver via `libusb_detach_kernel_driver()` (which immediately stops the 15-second disconnect loop!).
  * Dumps the 2 MB internal SPI NOR flash (`read_flash 0 2M firmware_2M.bin`) even in normal Card Reader mode (`--id 301a:2801`) without needing to find the hardware boot key!
  * Reads arbitrary RAM/ROM addresses (`read_mem`, including dumping the internal Boot ROM at `0x00000000`) by injecting a tiny Thumb-2 payload into RAM (`0x800de8`).
  * Writes/flashes modified firmware images (`write_flash 0 0 0 out.bin`) in Bootloader mode (`301a:2800`).
  * `fwhelper` parses the `HLKJ` bootloader header, partition table (`FIRM`, `TONE`, `PSMP`), and verifies/fixes `CRC-16/CCITT-FALSE` checksums.

### 2. [`dbrant/SL6801_player`](https://github.com/dbrant/SL6801_player) (by Dmitry Brant)
* **What it is:** A comprehensive, MIT-licensed reverse-engineering repository, Python asset unpacker/packer, Ghidra auto-naming suite, and C SDK for SL6801 (`yp3_2.0.43`) players.
* **What Dmitry Brant has already mapped out:**
  * **Complete Memory & Flash Layout:**
    * `0x00000000`: Boot ROM (`memcpy` at `0xB2A`, debug UART `printf` at `0x2B0`, `usb_set_userfn` at `0x5914`).
    * `0x00800000–0x0087FFFF`: 512 KB SRAM (LVGL 40 KB TLSF heap at `0x00876000`).
    * `0x00C00000`: 2 MB XIP SPI Flash (`0x000000` `HLKJ` header + bootloader, `0x00D000` partition table, `0x00E000` `FIRM` main app, `0x183000` `TONE`, **`0x185038–0x1FC000` ~475 KB of unused `0xFF` flash**, `0x1FC000` `PSMP` non-volatile settings).
  * **Internal Software Stack:**
    * FreeRTOS with POSIX-style device nodes: `/dev/lcd`, `/dev/audio0` (playback), `/dev/audio1` (recording), `/dev/fm`, `/dev/usbd`, `/dev/uart0..2`, `/dev/key_io`.
    * Voice recordings are saved as WAV/ADPCM into `0:\record` on the FatFs volume.
  * **Custom App Injection SDK (`sdk/`):**
    * Compiles standalone Thumb-2 C apps (using `arm-none-eabi-gcc` or `zig cc`) that execute in-place (XIP) from the 475 KB free flash region at `0x1D0000` (`0xDD0000` in memory) and hook into the native LVGL v8.3 UI (`APP` menu) and `/dev/audio0` driver!
    * Includes working custom apps (*Mp3tris*, *Flutter*, *Key test*, *Sound test*), asset unpacker/repacker (`tools/assets.py`) for UI strings, images, and LVGL fonts, and headless Ghidra scripts (`ghidra/`) that automatically name ~350 firmware functions via `__func__` assert strings.

### 3. [`zeroproj/OpenPod`](https://github.com/zeroproj/OpenPod) (by zeroproj)
* **What it is:** A pre-packaged custom firmware distribution (targeting Ligenai GN-438 / GN-408 with SL6801 `yp3_2.0.43`) that reskins the stock UI into an iPod-inspired theme and adds embedded JPEG album art support.
* **Note on Open-Source Contributions:** `OpenPod`'s repository currently only distributes binary releases (its analysis tools are not public), whereas **`dbrant/SL6801_player`** and **`ilyakurdyukov/smartlink_flash`** are 100% open-source C/Python repositories with clear scope and documented open questions.

---

## 5. Open-Source Research & Upstream Contribution Areas

1. **`dbrant/SL6801_player` Open Questions:**
   * **Boot ROM (`0x00000000`):** The main firmware calls into Boot ROM functions (`bl 0x2b0`, `b.w 0x2564`). Mapping the ROM entry points from a local `read_mem 0 64K` dump allows replacing the Ghidra `bx lr` ROM stubs with real definitions.
   * **`/dev/audio1` (Voice Recorder API) & Bluetooth HFP internals:** `SL6801_player`'s SDK currently wraps `/dev/audio0` (playback), LVGL, and keys, while `/dev/audio1` (recording) and the Bluetooth dispatcher (`0x81D14C` / `FUN_00D27158`) are not yet documented in `fw.h`.
   * **Multi-device / Firmware Version Compatibility:** Comparing `HW_76` (`pen133_2.0.65`) against `yp3_2.0.43` to document differences across SL6801 board variants.
2. **Linux USB Mass Storage Handling (`301a:2801`):**
   * Documenting the `udev` + exFAT `-1 cluster` boundary fix so Linux users can mount SL6801 players without hitting the 15-second USB reset loop.

---

## 6. Firmware & Boot ROM Analysis (`HW_76` / `pen133_2.0.65`)

Using `smtlink_dump --id 301a:2801` in Card Reader mode, the **2 MB SPI NOR flash** (`dumps/firmware_2M.bin`) and **64 KB internal Boot ROM** (`dumps/bootrom_64K.bin`) can be dumped locally and inspected with [`tools/inspect_fw.py`](tools/inspect_fw.py). The full Thumb-2 disassembly walkthrough and protocol reference derived from these dumps lives in **[firmware_analysis.md](firmware_analysis.md)**.

### 1. Flash Image Structure & Checksums (`inspect_fw.py`)
```text
File:       dumps/firmware_2M.bin (2,097,152 bytes)
SHA-256:    9916d0ffb68889e0b8369a761513150387bf8518d7644dd713ffa0e8593f3dfd
Flash ID:   0x14851485

=== Bootloader Header (HLKJ) ===
  Load Address:      0x0081FBC0
  Entry Address:     0x00820001
  Code Offset/Size:  0x000060 + 0x00C8F0 (51,440 bytes)
  CRC-16:            0x3244 (calculated: 0x3244)
  Partition Table:   0x00D000

=== Partition Table (count=2, version=0x0) ===
  [0] FIRM  offset=0x00E000  size=0x165E8C (1,465,996 B)  crc=0xBDA3 [calc=0xBDA3 (OK)]
  [1] PSMP  offset=0x1FC000  size=0x004000 (   16,384 B)  crc=0x0000 [N/A (NVRAM)]
```

### 2. Key Differences vs. `yp3_2.0.43` Players
* **Firmware Version & Build Timestamp:**
  * Version string: **`pen133_2.0.65`** (built from `F:\pen133_yp3_mp\spark2\src\...`, compared to `yp3_2.0.43` in `dbrant/SL6801_player`).
  * `FIRM` build timestamp: **`2509261408`** (`2025-09-26 14:08`).
* **Partition Layout & Free XIP Flash Space:**
  * Unlike `yp3_2.0.43` (which had 3 partitions: `FIRM`, `TONE`, and `PSMP`), `pen133_2.0.65` has **only 2 partitions** (`FIRM` and `PSMP`, no `TONE` partition).
  * `FIRM` ends at `0x00E000 + 0x165E8C = 0x173E8C`, leaving **`0x173E8C` to `0x1FC000` (~544 KB / 557,428 bytes)** of unused `0xFF` flash space available for custom XIP apps or assets.
* **Hardware & Recording Insights from Firmware Strings:**
  * **Display Controller:** `st7789s_lcd_init` (`/dev/lcd`, RGB565).
  * **Voice & Phone Call Recording:**
    * Standard microphone recordings are written via `/dev/audio1` to **`0:\record`**.
    * **Bluetooth HFP Phone Call Recording:** When paired with a phone in Bluetooth Hands-Free (`HW_76`, `bt_nameHW_76`) mode, incoming/outgoing calls trigger `page_call_come_create` / `page_call_on_create` (`call_income`, `call_ring`, `call_online`, `call_end`), and the firmware automatically records the Bluetooth HFP call audio directly to the SD card as:
      ```text
      0:\record\20%02d%02d%02d-%02d%02d%02d-phone.wav
      ```
  * **SD Card Firmware Update Path:** The bootloader checks for **`0:\update.up`** on startup.

---

## 7. Dumping & Inspecting the Firmware Locally

### Building `smtlink_dump`
Clone and build [`ilyakurdyukov/smartlink_flash`](https://github.com/ilyakurdyukov/smartlink_flash):

```bash
git clone https://github.com/ilyakurdyukov/smartlink_flash.git /tmp/smartlink_flash
make -C /tmp/smartlink_flash
```

### Dumping in Card Reader Mode (No Boot Key Required)
Because `smtlink_dump` detaches the Linux `usb-storage` driver via `libusb` before the 15-second timeout occurs, the 2 MB firmware and 64 KB Boot ROM can be dumped directly in normal Card Reader mode (`301a:2801`):

```bash
sudo /tmp/smartlink_flash/smtlink_dump --id 301a:2801 flash_id read_flash 0 2M dumps/firmware_2M.bin
sudo /tmp/smartlink_flash/smtlink_dump --id 301a:2801 read_mem 0 64K dumps/bootrom_64K.bin
python3 tools/inspect_fw.py dumps/firmware_2M.bin
```

---

## 8. Integration Tools: USB Auto-Sync & Bluetooth Companion (`tools/`)

### 1. USB Auto-Sync (`tools/usb_sync.py`)
Automatically detects when the `HW_76` (`301a:2801`) exFAT volume is connected (and auto-mounts it via `udisksctl` if not yet mounted), imports new `.wav` / `.mp3` recordings from `record/`, verifies SHA-256 checksums, classifies whether each recording is a microphone recording (`20YYMMDD-HHMMSS-120min.wav`) or a Bluetooth phone call recording (`20YYMMDD-HHMMSS-phone.wav`), and tracks synced files in `.sync_manifest.json`.

```bash
# One-shot sync of all new recordings to ~/Recordings/HW_76
python3 tools/usb_sync.py --dest ~/Recordings/HW_76

# Continuous daemon mode: watches for USB plug-in and auto-imports new recordings
python3 tools/usb_sync.py --watch --dest ~/Recordings/HW_76
```

### 2. Bluetooth Companion & Audio Stream Capture (`tools/bt_companion.py`)
Reverse-engineering of the firmware's internal `spark_bt` stack (`0xDA800..0xDC000` in `pen133_2.0.65`) revealed the exact profiles and state machines compiled into the device:
* **`spark_bt_rfcomm_entry` / `_hfp_hf_slc_open_status` (HFP v1.6 Hands-Free Unit on RFCOMM Channel 1):**
  * Expects the exact AT command Service Level Connection (SLC) sequence: `W4RFCOMM` -> `W4SF` (`AT+BRSF=`) -> `W4IND` (`AT+CIND=?`) -> `W4INDSTA` (`AT+CIND?`) -> `W4CMER` (`AT+CMER=3,0,0,1`) -> `SLC_OPEN`.
  * Supports `+BCS` codec negotiation (`CVSD` / `mSBC`), `ATA` (answer), `AT+CHUP` (hang up), `+VGS` / `+VGM`, and `+CIEV` call indicators (`call_income`, `call_ring`, `call_online`, `call_end`).
* **`spark_bt_a2dp_src_entry` / `spark_bt_avrcp_tg_entry` (A2DP Source + AVRCP Target):**
  * Used in "Bluetooth Headphone" mode to stream playback of recordings stored on the device to a paired Bluetooth Audio Sink, accepting remote AVRCP pass-through commands (`Play`, `Pause`, `Stop`, `Forward`, `Backward`, `Fast-forward`, `Rewind`, `Vol up`, `Vol down`).
* **No OBEX / SPP / BLE File Transfer in Stock Firmware:**
  * Because stock firmware only implements Bluetooth audio profiles (HFP + A2DP/AVRCP), `tools/bt_companion.py` provides four Bluetooth integration modes:
    1. `--mode pair`: Automated Classic Bluetooth (`bredr`) discovery, pairing, trusting, and connection via BlueZ.
    2. `--mode rfcomm-slc`: Connects directly to RFCOMM Channel 1 as an HFP Audio Gateway (AG) and completes the SL6801 AT command handshake (`SLC_OPEN`).
    3. `--mode simulate-call`: Triggers the `call_online` state on `HW_76` over RFCOMM (`+CIEV: 2,1`), causing `HW_76` to switch to its call-recording UI (`page_call_on_create`) and record incoming SCO audio to `0:\record\20YYMMDD-HHMMSS-phone.wav`.
    4. `--mode record-stream`: Captures live audio arriving from `HW_76` over Bluetooth (either wireless HFP microphone audio or A2DP playback of stored recordings when the PC is paired as an A2DP Audio Sink) via PipeWire (`pw-record`) into timestamped `.wav` files.

```bash
# Pair and connect to HW_76 over Classic Bluetooth
python3 tools/bt_companion.py --mode pair

# Complete the direct RFCOMM Channel 1 HFP Service Level Connection handshake
python3 tools/bt_companion.py --mode rfcomm-slc

# Capture Bluetooth audio stream (HFP mic or A2DP playback) via PipeWire to .wav
python3 tools/bt_companion.py --mode record-stream --duration 30 --dest ~/Recordings/HW_76
```

---

## License

Code and documentation in this repository are released under the [MIT License](LICENSE). No proprietary firmware or Boot ROM binaries are distributed in this repository (users must extract any dumps locally from their own hardware).
