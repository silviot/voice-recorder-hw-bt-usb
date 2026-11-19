#!/usr/bin/env python3
"""
Firmware verifier, binary patcher, and 0:\\update.up packager for
Smartlink SL6801 (HW_76 / pen133_2.0.65).

Examples:
  # Verify all four CRC-16/CCITT-FALSE checksums in a 2 MB flash dump:
  python3 tools/patch_firmware.py dumps/firmware_2M.bin --verify

  # Apply the WAV nBlockAlign=4 fix and write a patched 2 MB image + update.up:
  python3 tools/patch_firmware.py dumps/firmware_2M.bin \\
      --patch wav-blockalign \\
      --out-bin /tmp/firmware_2M_patched.bin \\
      --out-update-up /tmp/update.up
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
from dataclasses import dataclass


FLASH_SIZE = 0x200000  # 2 MiB
PART_TABLE_OFF = 0x00D000
FIRM_OFF = 0x00E000
PSMP_OFF = 0x1FC000


def crc16_ccitt_false(data: bytes | bytearray | memoryview) -> int:
    """CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF, xorout=0x0000)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class PatchHunk:
    flash_offset: int
    original: bytes
    replacement: bytes


@dataclass(frozen=True)
class BinaryPatch:
    name: str
    description: str
    hunks: tuple[PatchHunk, ...]


PATCHES: dict[str, BinaryPatch] = {
    "wav-blockalign": BinaryPatch(
        name="wav-blockalign",
        description=(
            "Fix PCM WAV header nBlockAlign at CloseWav (VMA 0xCED464 / flash 0x0ED464): "
            "store bytesPerSample * nChannels (4) instead of bytesPerSample (2)."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x0ED464,
                original=bytes.fromhex(
                    "d2 7b 50 43 92 b2 43 43 ad f8 2c 20 d2 00 0a 93 ad f8 2e 20"
                ),
                replacement=bytes.fromhex(
                    "d2 7b 53 43 58 43 ad f8 2c 30 d2 00 0a 90 ad f8 2e 20 00 bf"
                ),
            ),
        ),
    ),
    "sdio-end-of-disk": BinaryPatch(
        name="sdio-end-of-disk",
        description=(
            "Fix 15-second Linux USB disconnect on end-of-card reads (LBA >= total_sectors - 3) "
            "in sd_read_blocks (0x815C22) + HAL_sd_disk_read (0x8161F0): send CMD12 before "
            "switching from open-ended to finite AUTO_CMD12 reads and fix post-increment skip_cmd12 flag."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x042452,  # SRAM 0x815C22 (sd_read_blocks)
                original=bytes.fromhex("33 f0 02 03 04 d1"),
                replacement=bytes.fromhex("01 2b 05 d0 11 e0"),
            ),
            PatchHunk(
                flash_offset=0x042A20,  # SRAM 0x8161F0 (HAL_sd_disk_read exit)
                original=bytes.fromhex(
                    "98 f8 00 30 1b 01 5b f8 03 30 03 3b ab 42 65 d8 d4 f8 cc 30 01 2b 61 d1 c4 e9 0e 33"
                ),
                replacement=bytes.fromhex(
                    "a3 6b 02 2b 6a d1 d4 f8 cc 30 01 2b 66 d1 c4 e9 0e 33 d4 f8 98 20 aa 42 60 d3 00 bf"
                ),
            ),
        ),
    ),
}


def read_firm_layout(buf: bytes | bytearray) -> tuple[int, int, int]:
    """Return (firm_off, firm_size, firm_ram_size) from partition table & FIRM header."""
    if buf[0:4] != b"HLKJ":
        raise ValueError(f"Expected 'HLKJ' magic at 0x0, got {buf[0:4]!r}")
    if buf[PART_TABLE_OFF + 0x10 : PART_TABLE_OFF + 0x14] != b"FIRM":
        raise ValueError("Expected 'FIRM' partition entry at 0xD010")
    firm_off, firm_size = struct.unpack_from("<II", buf, PART_TABLE_OFF + 0x14)
    firm_ram_size = struct.unpack_from("<I", buf, firm_off + 0x18)[0]
    return firm_off, firm_size, firm_ram_size


def verify_checksums(buf: bytes | bytearray) -> bool:
    """Verify all four CRC-16/CCITT-FALSE checksums in the flash image."""
    bl_size = struct.unpack_from("<I", buf, 0x10)[0]
    firm_off, firm_size, firm_ram_size = read_firm_layout(buf)

    checks = [
        ("HLKJ header (0x000000..0x00005B)", buf[0x00:0x5C], struct.unpack_from("<H", buf, 0x5C)[0]),
        (f"Bootloader  (0x000060..0x{0x60 + bl_size - 1:06X})", buf[0x60 : 0x60 + bl_size], struct.unpack_from("<H", buf, 0x14)[0]),
        (f"FIRM RAM    (0x{firm_off + 0x30:06X}..0x{firm_off + 0x30 + firm_ram_size - 1:06X})", buf[firm_off + 0x30 : firm_off + 0x30 + firm_ram_size], struct.unpack_from("<H", buf, firm_off + 0x1C)[0]),
        (f"FIRM part   (0x{firm_off:06X}..0x{firm_off + firm_size - 1:06X})", buf[firm_off : firm_off + firm_size], struct.unpack_from("<H", buf, PART_TABLE_OFF + 0x1C)[0]),
    ]

    all_ok = True
    for label, region, stored in checks:
        calc = crc16_ccitt_false(region)
        ok = calc == stored
        all_ok &= ok
        print(f"  [{'OK' if ok else 'FAIL'}] {label}: calc=0x{calc:04X} stored=0x{stored:04X}")
    return all_ok


def update_firm_checksums(buf: bytearray) -> None:
    """Recompute FIRM RAM block CRC-16 (0xE01C) and FIRM partition CRC-16 (0xD01C)."""
    firm_off, firm_size, firm_ram_size = read_firm_layout(buf)
    ram_crc = crc16_ccitt_false(buf[firm_off + 0x30 : firm_off + 0x30 + firm_ram_size])
    struct.pack_into("<I", buf, firm_off + 0x1C, ram_crc)
    part_crc = crc16_ccitt_false(buf[firm_off : firm_off + firm_size])
    struct.pack_into("<I", buf, PART_TABLE_OFF + 0x1C, part_crc)


def build_update_up(buf: bytes | bytearray) -> bytes:
    """Wrap the flash image up to the end of FIRM (aligned to 4 KiB, < PSMP) in 0:\\update.up format."""
    firm_off, firm_size, _ = read_firm_layout(buf)
    end_off = (firm_off + firm_size + 0xFFF) & ~0xFFF
    if end_off > PSMP_OFF:
        raise ValueError(f"FIRM end 0x{end_off:X} overlaps PSMP at 0x{PSMP_OFF:X}")

    header = bytearray(0x200)
    header[0x00:0x06] = b"CONFIG"
    struct.pack_into("<I", header, 0x06, 0x200)  # codeOffsetInByte
    header[0x16:0x1C] = b"SL6801"
    return bytes(header) + bytes(buf[:end_off])


def main() -> int:
    ap = argparse.ArgumentParser(description="Smartlink SL6801 firmware verifier, patcher & update.up packager")
    ap.add_argument("input", type=pathlib.Path, help="Input 2 MB flash dump (e.g. dumps/firmware_2M.bin)")
    ap.add_argument("--verify", action="store_true", help="Verify all CRC-16/CCITT-FALSE checksums")
    ap.add_argument("--list-patches", action="store_true", help="List available binary patches")
    ap.add_argument("--patch", action="append", default=[], choices=sorted(PATCHES), help="Apply a named binary patch")
    ap.add_argument("--bump-timestamp", action="store_true", help="Increment FIRM+0x04 build timestamp by 1")
    ap.add_argument("--out-bin", type=pathlib.Path, help="Write patched 2 MB raw flash image")
    ap.add_argument("--out-update-up", type=pathlib.Path, help="Write SD-card 0:\\update.up package")
    args = ap.parse_args()

    if args.list_patches:
        for p in PATCHES.values():
            total_b = sum(len(h.original) for h in p.hunks)
            offs = ", ".join(f"0x{h.flash_offset:06X}" for h in p.hunks)
            print(f"  {p.name:18s} @ {offs} ({total_b} B): {p.description}")
        return 0

    raw = bytearray(args.input.read_bytes())
    if len(raw) != FLASH_SIZE:
        print(f"Error: expected {FLASH_SIZE} bytes, got {len(raw)}", file=sys.stderr)
        return 1

    print(f"Loaded {args.input} ({len(raw)} bytes)")
    if args.verify or not (args.patch or args.out_bin or args.out_update_up):
        ok = verify_checksums(raw)
        if not ok:
            return 1

    for name in args.patch:
        p = PATCHES[name]
        # Pre-validate all hunks before modifying anything
        already = all(
            bytes(raw[h.flash_offset : h.flash_offset + len(h.original)]) == h.replacement
            for h in p.hunks
        )
        if already:
            print(f"  [SKIP] Patch '{name}' already applied")
            continue
        for h in p.hunks:
            cur = bytes(raw[h.flash_offset : h.flash_offset + len(h.original)])
            if cur != h.original:
                print(
                    f"Error: bytes at 0x{h.flash_offset:06X} do not match expected original for '{name}'",
                    file=sys.stderr,
                )
                return 1
        for h in p.hunks:
            raw[h.flash_offset : h.flash_offset + len(h.replacement)] = h.replacement
        total_b = sum(len(h.replacement) for h in p.hunks)
        print(f"  [APPLIED] {name} ({len(p.hunks)} hunk(s), {total_b} bytes)")

    if args.bump_timestamp:
        firm_off, _, _ = read_firm_layout(raw)
        old_ts = struct.unpack_from("<I", raw, firm_off + 0x04)[0]
        struct.pack_into("<I", raw, firm_off + 0x04, (old_ts + 1) & 0xFFFFFFFF)
        print(f"  [BUMPED] FIRM timestamp: 0x{old_ts:08X} -> 0x{(old_ts + 1) & 0xFFFFFFFF:08X}")

    if args.patch or args.bump_timestamp:
        update_firm_checksums(raw)
        print("Recomputed FIRM checksums:")
        assert verify_checksums(raw)

    if args.out_bin:
        args.out_bin.write_bytes(raw)
        print(f"Wrote patched 2 MB flash image: {args.out_bin}")

    if args.out_update_up:
        pkg = build_update_up(raw)
        args.out_update_up.write_bytes(pkg)
        print(f"Wrote SD update package: {args.out_update_up} ({len(pkg)} bytes, stops before PSMP 0x1FC000)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
