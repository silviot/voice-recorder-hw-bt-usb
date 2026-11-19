#!/usr/bin/env python3
import struct
import pathlib
import subprocess
import re

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()
rom = pathlib.Path("dumps/bootrom_64K.bin").read_bytes()

print("=== 1. BootROM Strings (0x0000..0x10000) ===")
rom_strs = [(m.start(), m.group().decode("ascii")) for m in re.finditer(rb"[ -~]{4,}", rom)]
for off, s in rom_strs:
    print(f"  ROM 0x{off:04X}: {s!r}")

print("\n=== 2. Bootloader Strings (flash 0x000060..0x00C950, RAM 0x81FBC0..) ===")
bl_blob = fw[0x60:0xC950]
for m in re.finditer(rb"[ -~]{5,}", bl_blob):
    s = m.group().decode("ascii")
    if any(k in s.lower() for k in ["usb", "update", "firm", "crc", "boot", "sl68", "sd", "fat", "err", "load", "psmp", "key", "up"]):
        print(f"  BL 0x{0x81FBC0 + m.start():06X} (flash 0x{0x60+m.start():05X}): {s!r}")

print("\n=== 3. FIRM Strings: Devices (/dev/*), USB, Bluetooth, AT+, Record, Call, Time ===")
firm_strs = [(m.start(), m.group().decode("ascii")) for m in re.finditer(rb"[ -~]{4,}", fw[0x00E000:0x173E8C])]
categories = {
    "DEV": re.compile(r"/dev/[a-zA-Z0-9_]+"),
    "USB": re.compile(r"(usb|udisk|msc|scsi|smtlink|cardreader|mass|301a|2801|2800)", re.I),
    "BT_AT": re.compile(r"(^AT[\+\=]|^\+[A-Z]{3,}|bt_|bluetooth|hfp|a2dp|avrcp|rfcomm|sdp|l2cap|spp|ble|gatt|obex|sbc|msbc|cvsd|sco|call_|phone\.wav|HW_|btrlk|btlink)", re.I),
    "REC_RTC": re.compile(r"(record|\.wav|adpcm|audio1|rtc|time\.|update\.up|DBROOT|db_wave)", re.I),
}
for cat, rx in categories.items():
    print(f"\n--- Category: {cat} ---")
    shown = 0
    for foff, s in firm_strs:
        flash_off = 0xE000 + foff
        if rx.search(s):
            print(f"  flash 0x{flash_off:06X}: {s!r}")
            shown += 1
            if shown >= 120:
                print("  ... (truncated)")
                break
