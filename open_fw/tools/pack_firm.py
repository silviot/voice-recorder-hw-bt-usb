#!/usr/bin/env python3
"""
Pack standalone SRAM-resident open_fw binary into a Smartlink SL6801 FIRM partition
image (and optional full 2 MB flash image + 0:\\update.up OTA package).

Matches Stage-1 Bootloader handoff (`0x00820470..0x008204E0`):
  - 48-byte FIRM header at flash 0x00E000
  - 1024-byte (0x400) zero pad at 0x00E030..0x00E42F
  - Flat SRAM image at 0x00E430+ copied by Stage-1 to 0x00804C00, entered at 0x00804C01

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import os
import struct
from pathlib import Path

FIRM_HEADER_SIZE = 0x30
FIRM_PAD_SIZE = 0x400
FIRM_RAM_LOAD = 0x00804C00
FIRM_BOOT_WINDOW = 0x1000
FIRM_MAX_RAM = 0x00028000
FIRM_FLASH_OFF = 0x00E000
PART_TABLE_OFF = 0x00D000
PSMP_OFF = 0x1FC000
FLASH_SIZE = 0x200000


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        t = ((crc >> 8) ^ b) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


def build_firm_partition(
    ram_img: bytes,
    entry_addr: int,
    timestamp: int,
    xip_img: bytes = b"",
) -> bytes:
    if xip_img:
        if len(ram_img) > FIRM_BOOT_WINDOW:
            raise ValueError(
                f"ram_boot ({len(ram_img)} B) exceeds 4 KiB Stage-1 window ({FIRM_BOOT_WINDOW} B)"
            )
        ram_padded = ram_img + b"\x00" * (FIRM_BOOT_WINDOW - len(ram_img))
        xip_aligned_len = (len(xip_img) + 3) & ~3
        xip_padded = xip_img + b"\x00" * (xip_aligned_len - len(xip_img))
        load_len = FIRM_BOOT_WINDOW
        body_after_hdr = (b"\x00" * FIRM_PAD_SIZE) + ram_padded + xip_padded
    else:
        if len(ram_img) > FIRM_MAX_RAM:
            raise ValueError(
                f"ram_img ({len(ram_img)} B) exceeds safe pre-stack limit ({FIRM_MAX_RAM} B)"
            )
        load_len = (len(ram_img) + 3) & ~3
        ram_padded = ram_img + b"\x00" * (load_len - len(ram_img))
        body_after_hdr = (b"\x00" * FIRM_PAD_SIZE) + ram_padded

    ram_crc = crc16_ccitt_false(body_after_hdr[:load_len])

    hdr = bytearray(FIRM_HEADER_SIZE)
    struct.pack_into("<I", hdr, 0x00, FIRM_HEADER_SIZE)
    struct.pack_into("<I", hdr, 0x04, timestamp & 0xFFFFFFFF)
    struct.pack_into("<I", hdr, 0x10, FIRM_RAM_LOAD)
    struct.pack_into("<I", hdr, 0x14, entry_addr | 1)
    struct.pack_into("<I", hdr, 0x18, load_len)
    struct.pack_into("<I", hdr, 0x1C, ram_crc)

    return bytes(hdr) + body_after_hdr


def build_full_image_and_up(
    base_raw: bytes, firm: bytes
) -> tuple[bytes, bytes]:
    if len(base_raw) != FLASH_SIZE or base_raw[:4] != b"HLKJ":
        raise ValueError("Expected valid 2 MB SL6801 HLKJ flash dump for --base-hlkj")
    if FIRM_FLASH_OFF + len(firm) > PSMP_OFF:
        raise ValueError("FIRM image overlaps PSMP")

    raw = bytearray(base_raw)
    # Erase flash between FIRM start and PSMP, then place open_fw FIRM
    raw[FIRM_FLASH_OFF:PSMP_OFF] = b"\xFF" * (PSMP_OFF - FIRM_FLASH_OFF)
    raw[FIRM_FLASH_OFF : FIRM_FLASH_OFF + len(firm)] = firm

    # Update FIRM partition entry at 0x00D010 (offset=0xE000, size=len(firm), crc16)
    if raw[PART_TABLE_OFF + 0x10 : PART_TABLE_OFF + 0x14] != b"FIRM":
        raise ValueError("Missing FIRM partition entry at 0x00D010")
    firm_crc = crc16_ccitt_false(firm)
    struct.pack_into("<II", raw, PART_TABLE_OFF + 0x14, FIRM_FLASH_OFF, len(firm))
    struct.pack_into("<H", raw, PART_TABLE_OFF + 0x1C, firm_crc)

    # Build 0:\update.up (4 KiB aligned end < PSMP)
    end_off = (FIRM_FLASH_OFF + len(firm) + 0xFFF) & ~0xFFF
    up_hdr = bytearray(0x200)
    up_hdr[0x00:0x06] = b"CONFIG"
    struct.pack_into("<I", up_hdr, 0x06, 0x200)
    up_hdr[0x16:0x1C] = b"SL6801"
    up_bytes = bytes(up_hdr) + bytes(raw[:end_off])
    return bytes(raw), up_bytes


def resolve_timestamp(cli_timestamp: int | None) -> int:
    """Resolve deterministic FIRM header timestamp (--timestamp > SOURCE_DATE_EPOCH > 0)."""
    if cli_timestamp is not None:
        return cli_timestamp & 0xFFFFFFFF
    env_ts = os.environ.get("SOURCE_DATE_EPOCH", "").strip()
    if env_ts:
        base = 0 if env_ts.lower().startswith(("0x", "0b", "0o")) else 10
        return int(env_ts, base) & 0xFFFFFFFF
    return 0


def main() -> None:
    p = argparse.ArgumentParser(description="Pack standalone SL6801 open_fw FIRM partition.")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--ram-boot", type=Path, help="<= 4 KiB SRAM bootstrap binary (0x00804C00)")
    src.add_argument("--ram-img", type=Path, help="Legacy flat SRAM-only binary (0x00804C00)")
    p.add_argument("--xip", type=Path, default=None, help="SPI NOR XIP payload mapped at 0x00C0F430")
    p.add_argument("--entry", type=lambda s: int(s, 0), default=0x00804C01, help="Thumb entry address")
    p.add_argument(
        "--timestamp",
        type=lambda s: int(s, 0),
        default=None,
        help="FIRM header timestamp (base-0 int; defaults to SOURCE_DATE_EPOCH or 0)",
    )
    p.add_argument("--out-firm", type=Path, required=True, help="Output standalone FIRM partition binary")
    p.add_argument("--base-hlkj", type=Path, default=None, help="Optional 2 MB base dump for HLKJ + PSMP")
    p.add_argument("--out-bin", type=Path, default=None, help="Output complete 2 MB flash image")
    p.add_argument("--out-update-up", type=Path, default=None, help="Output 0:\\update.up OTA package")
    args = p.parse_args()

    ts = resolve_timestamp(args.timestamp)
    if args.ram_boot is not None:
        if args.xip is None:
            p.error("--ram-boot requires --xip")
        ram_img = args.ram_boot.read_bytes()
        xip_img = args.xip.read_bytes()
    else:
        ram_img = args.ram_img.read_bytes()
        xip_img = b""

    firm = build_firm_partition(ram_img, args.entry, ts, xip_img=xip_img)
    firm_crc = crc16_ccitt_false(firm)
    args.out_firm.parent.mkdir(parents=True, exist_ok=True)
    args.out_firm.write_bytes(firm)
    if xip_img:
        print(
            f"Packed hybrid FIRM: {args.out_firm} ({len(firm):,} B: "
            f"48 B hdr + 1024 B pad + {len(ram_img):,} B/4096 B ram_boot + "
            f"{len(xip_img):,} B XIP @ 0x00C0F430, entry=0x{args.entry | 1:08X}, "
            f"FIRM CRC-16=0x{firm_crc:04X})"
        )
    else:
        print(
            f"Packed standalone FIRM: {args.out_firm} ({len(firm):,} B: "
            f"48 B hdr + 1024 B pad + {len(ram_img):,} B SRAM image, "
            f"entry=0x{args.entry | 1:08X}, FIRM CRC-16=0x{firm_crc:04X})"
        )

    if args.base_hlkj is not None:
        full_bin, up_bytes = build_full_image_and_up(args.base_hlkj.read_bytes(), firm)
        if args.out_bin is not None:
            args.out_bin.parent.mkdir(parents=True, exist_ok=True)
            args.out_bin.write_bytes(full_bin)
            print(f"Wrote 2 MB open_fw flash image: {args.out_bin} ({len(full_bin):,} B)")
        if args.out_update_up is not None:
            args.out_update_up.parent.mkdir(parents=True, exist_ok=True)
            args.out_update_up.write_bytes(up_bytes)
            print(f"Wrote 0:\\update.up open_fw package: {args.out_update_up} ({len(up_bytes):,} B)")


if __name__ == "__main__":
    main()
