#!/usr/bin/env python3
"""
Clean-room firmware image inspector for Smartlink SL6801 / SL6806 flash dumps.
Validates the HLKJ bootloader header, partition directory, and CRC-16/CCITT-FALSE
checksums without modifying or redistributing proprietary firmware binaries.

SPDX-License-Identifier: MIT
"""
import hashlib
import re
import struct
import sys
from pathlib import Path


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    """Compute CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF)."""
    crc = init
    for byte in data:
        t = ((crc >> 8) ^ byte) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


def inspect_image(path: Path) -> None:
    raw = path.read_bytes()
    print(f"File:       {path} ({len(raw):,} bytes)")
    print(f"SHA-256:    {hashlib.sha256(raw).hexdigest()}")

    if len(raw) < 0x60 or raw[:4] != b"HLKJ":
        print("Error: Missing 'HLKJ' bootloader header magic at offset 0x00.")
        sys.exit(1)

    load_addr, entry_addr, code_off, code_size, boot_crc = struct.unpack_from("<IIIII", raw, 0x04)
    ptab_off = struct.unpack_from("<I", raw, 0x20)[0]
    calc_boot_crc = crc16_ccitt_false(raw[code_off : code_off + code_size])

    print("\n=== Bootloader Header (HLKJ) ===")
    print(f"  Load Address:      0x{load_addr:08X}")
    print(f"  Entry Address:     0x{entry_addr:08X}")
    print(f"  Code Offset/Size:  0x{code_off:06X} + 0x{code_size:06X} ({code_size:,} bytes)")
    print(f"  CRC-16:            0x{boot_crc:04X} (calculated: 0x{calc_boot_crc:04X})")
    print(f"  Partition Table:   0x{ptab_off:06X}")

    count, _, version = struct.unpack_from("<III", raw, ptab_off)
    print(f"\n=== Partition Table (count={count}, version=0x{version:X}) ===")

    for idx in range(count):
        entry_off = ptab_off + 16 + idx * 16
        name = raw[entry_off : entry_off + 4].decode("ascii", errors="replace")
        p_off, p_size = struct.unpack_from("<II", raw, entry_off + 4)
        crc_field_off = entry_off + (14 if version >= 0x30 else 12)
        p_crc = struct.unpack_from("<H", raw, crc_field_off)[0]
        calc_crc = crc16_ccitt_false(raw[p_off : p_off + p_size])
        crc_status = "N/A (NVRAM)" if name == "PSMP" else f"calc=0x{calc_crc:04X} ({'OK' if p_crc == calc_crc else 'MISMATCH'})"
        print(f"  [{idx}] {name:4s}  offset=0x{p_off:06X}  size=0x{p_size:06X} ({p_size:9,d} B)  crc=0x{p_crc:04X} [{crc_status}]")

    # Search for firmware version strings (e.g., yp3_...)
    versions = sorted(set(m.decode("ascii") for m in re.findall(rb"yp3_[0-9A-Za-z._-]+", raw)))
    if versions:
        print(f"\n=== Detected Firmware Version Strings ===")
        for v in versions:
            print(f"  - {v}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <firmware_dump.bin>")
        sys.exit(1)
    inspect_image(Path(sys.argv[1]))
