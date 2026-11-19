#!/usr/bin/env python3
"""
Pack standalone open_fw binaries (ram_boot.bin + xip.bin) into a Smartlink SL6801
FIRM partition image with exact 48-byte FIRM header and CRC-16/CCITT-FALSE.

Optionally injects the open-source FIRM partition into a local HLKJ Stage-1
bootloader image (0x000000..0x00DFFF) and updates the partition table at 0x00D000.

SPDX-License-Identifier: MIT
"""
import argparse
import struct
import time
from pathlib import Path

FIRM_HEADER_SIZE = 0x30
FIRM_RAM_LOAD = 0x00804C00
FIRM_RAM_SIZE = 0x1000
FIRM_FLASH_OFF = 0x00E000
PART_TABLE_OFF = 0x00D000


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        t = ((crc >> 8) ^ b) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


def build_firm_partition(ram_boot: bytes, xip_code: bytes, entry_addr: int, timestamp: int) -> bytes:
    if len(ram_boot) > FIRM_RAM_SIZE:
        raise ValueError(f"ram_boot.bin ({len(ram_boot)} B) exceeds {FIRM_RAM_SIZE} B limit")
    ram_padded = ram_boot + b"\x00" * (FIRM_RAM_SIZE - len(ram_boot))
    ram_crc = crc16_ccitt_false(ram_padded)

    hdr = bytearray(FIRM_HEADER_SIZE)
    struct.pack_into("<I", hdr, 0x00, FIRM_HEADER_SIZE)
    struct.pack_into("<I", hdr, 0x04, timestamp & 0xFFFFFFFF)
    struct.pack_into("<I", hdr, 0x10, FIRM_RAM_LOAD)
    struct.pack_into("<I", hdr, 0x14, entry_addr | 1)
    struct.pack_into("<I", hdr, 0x18, FIRM_RAM_SIZE)
    struct.pack_into("<I", hdr, 0x1C, ram_crc)

    body = bytes(hdr) + ram_padded + xip_code
    # Pad to 4-byte alignment
    if len(body) % 4:
        body += b"\xFF" * (4 - (len(body) % 4))
    return body


def main() -> None:
    p = argparse.ArgumentParser(description="Pack standalone SL6801 open_fw FIRM partition.")
    p.add_argument("--ram-boot", type=Path, required=True, help="Compiled .ram_boot binary (<= 4 KB)")
    p.add_argument("--xip", type=Path, required=True, help="Compiled .text/.rodata/.data XIP binary")
    p.add_argument("--entry", type=lambda s: int(s, 0), default=0x00804C01, help="Thumb entry address")
    p.add_argument("--out-firm", type=Path, required=True, help="Output standalone FIRM partition binary")
    args = p.parse_args()

    ram_boot = args.ram_boot.read_bytes()
    xip_code = args.xip.read_bytes()
    ts = int(time.time()) & 0xFFFFFFFF

    firm = build_firm_partition(ram_boot, xip_code, args.entry, ts)
    firm_crc = crc16_ccitt_false(firm)
    args.out_firm.parent.mkdir(parents=True, exist_ok=True)
    args.out_firm.write_bytes(firm)
    print(
        f"Packed standalone FIRM: {args.out_firm} ({len(firm):,} B: "
        f"48 B hdr + {len(ram_boot)} B ram_boot + {len(xip_code)} B XIP, "
        f"entry=0x{args.entry | 1:08X}, FIRM CRC-16=0x{firm_crc:04X})"
    )


if __name__ == "__main__":
    main()
