# Firmware analysis scripts

One-off scripts used to produce the findings in [`firmware_analysis.md`](../../firmware_analysis.md)
for the Smartlink SL6801 dual-core (BootROM / bootloader / FIRM) images in `dumps/`.
They are kept as-is from the original analysis session — exploratory, no CLI
arguments, each prints a sectioned report to stdout.

## Requirements

- Python 3 (stdlib only: `struct`, `zlib`, `re`, `pathlib`)
- `llvm-objdump` at `/usr/lib/llvm-21/bin/llvm-objdump` (Thumb-2 disassembly;
  the `disasm_*` scripts wrap raw flash regions in a minimal ARM ELF with
  correct VMAs so `--triple=thumbv7em-none-eabi` resolves PC-relative
  literals correctly). Adjust the path if your LLVM lives elsewhere.
- The dumps from the device: `dumps/bootrom_64K.bin`, `dumps/firmware_2M.bin`
  (not committed — see `.gitignore`; re-read them from your own device).

Run from the **repo root**, e.g. `python3 tools/fw_analysis/analyze_fw.py`.

## Scripts

| Script | What it does |
|---|---|
| `analyze_fw.py` | Parses the `PSMP` NVRAM partition (`0x1FC000..0x200000`): all active records (`0x55AA` header, CRC32), key names/IDs/values; also scans WAV headers in the SD backup |
| `bt_strings.py` | Extracts every Bluetooth/HFP/A2DP/AVRCP/SPP/BLE/SDP/AT string from FIRM |
| `check_loader.py` | Disassembles the bootloader FIRM loader (`0x820380`) and the `.data` copy loop |
| `deep_scan.py` | BootROM reset handler + bootloader entry disassembly |
| `deep_scan2.py` | FIRM entropy/zero/data map and internal region layout |
| `disasm_all_five.py` | FIRM SCSI interceptor tail, HFP CIEV/AT table, HFP SLC open |
| `disasm_bt_psmp.py` | AVRCP Passthrough `op_id` switch, `PHONERECORD` AT trigger |
| `disasm_core.py` | BootROM SCSI dispatcher + `0xC0` vendor command handler, READ(10)/WRITE(10)/`usb_set_userfn` |
| `disasm_firm.py` | Finds FIRM callers of the ROM `usb_set_userfn` hook |
| `disasm_sdp_sdio_psmp.py` | `PSMP nv_set` routine, SDP service records (Handsfree `0x111E`, AudioSource `0x110A`, …) |
| `disasm_usb_bt.py` | Bootloader `boot--->firmware_header_len` loader path, BootROM USB SCSI + `0xC0` handler |
| `disasm_usb_ops_and_bt.py` | FIRM USB MSC ops callbacks in `.ramtext`, SD read/write registered callbacks |
| `final_checks.py` | `HAL_sd_disk_read`, raw SDP records + query patterns |
| `find_bt_xrefs.py` | Cross-references into the BT host stack |
| `inspect_details.py` | All strings in the BT host stack (`0x0D6700..0x0DD400`) and CLI strings |
| `map_and_disasm.py` | Derives the flash→SRAM mapping used by FIRM, then disassembles against it |
| `usb_bt_scan.py` | BootROM and bootloader string maps |

## Note on identifiers

These scripts read your **own** dumps and print device-specific values
(Bluetooth MAC, serial, pairing slots) verbatim. The public
`firmware_analysis.md` redacts those values; keep local output to yourself if
you care about that.
