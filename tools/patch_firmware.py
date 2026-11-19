#!/usr/bin/env python3
"""
Firmware verifier, binary patcher, XIP payload injector, and 0:\\update.up
packager for Smartlink SL6801 (HW_76 / pen133_2.0.65).

Examples:
  # Verify all four CRC-16/CCITT-FALSE checksums in a 2 MB flash dump:
  python3 tools/patch_firmware.py dumps/firmware_2M.bin --verify

  # Apply SDIO + WAV fixes + Bluetooth +XREC XIP payload and emit both raw bin & update.up:
  python3 tools/patch_firmware.py dumps/firmware_2M.bin \\
      --patch sdio-end-of-disk --patch wav-blockalign \\
      --xip-payload /tmp/bt_filesync.bin \\
      --out-bin dumps/firmware_2M_patched.bin \\
      --out-update-up dumps/update.up
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys
from dataclasses import dataclass


FLASH_SIZE = 0x200000  # 2 MiB
PART_TABLE_OFF = 0x00D000
FIRM_OFF = 0x00E000
PSMP_OFF = 0x1FC000

# XIP Bluetooth RFCOMM (+XREC) extension hook parameters (pen133_2.0.65)
XIP_HOOK_VMA = 0x00D625E2
XIP_HOOK_FLASH_OFF = XIP_HOOK_VMA - 0x00C00000  # 0x1625E2
XIP_ORIG_TARGET_VMA = 0x00D62180
XIP_PAYLOAD_VMA = 0x00D74000
XIP_PAYLOAD_OFF = XIP_PAYLOAD_VMA - 0x00C00000  # 0x174000
XIP_MAX_PAYLOAD_SIZE = 0x4000  # 16 KiB


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
    "bt-autoboot": BinaryPatch(
        name="bt-autoboot",
        description=(
            "Auto-enable Bluetooth stack on every boot strictly AFTER the 'welcome' splash screen "
            "is dismissed (watch_presenter_entry @ 0x00CFBD8A / flash 0x0FBD8A): redirect "
            "bl soft_timer_reset('pstr') (0x00D1BCD4) to XIP bt_autoboot_post_welcome (0x00D74010). "
            "Requires --xip-payload compiled from fw_mod/bt_filesync.c."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x0FBD8A,  # VMA 0x00CFBD8A: bl 0x00D1BCD4 -> bl 0x00D74010
                original=bytes.fromhex("1f f0 a3 ff"),
                replacement=bytes.fromhex("78 f0 41 f9"),
            ),
        ),
    ),
    "bt-force-phone": BinaryPatch(
        name="bt-force-phone",
        description=(
            "Force Bluetooth profile=0x400 branch in bt_subsystem_init @ 0x00CEA132 / flash 0x0EA132 "
            "(Note: on pen133_2.0.65, 0x400 selects Music/A2DP mode; leave disabled to preserve "
            "Phone/HFP mode saved in PSMP)."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x0EA132,  # VMA 0x00CEA132: cbnz r0, 0xcea180 -> b 0xcea180
                original=bytes.fromhex("28 bb"),
                replacement=bytes.fromhex("25 e0"),
            ),
        ),
    ),
    "bt-invisible-boot": BinaryPatch(
        name="bt-invisible-boot",
        description=(
            "Boot Bluetooth in connectable-only mode (Page Scan ON, Inquiry Scan OFF; "
            "bt_host_init @ 0x00D5D47A / flash 0x15D47A): already-paired devices reconnect "
            "instantly while nearby Bluetooth scans cannot discover HW_76. Combine with "
            "bt-pair-menu so opening the Bluetooth menu temporarily enables discoverability."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x15D47A,  # VMA 0x00D5D47A: movw r3, #0x0101 -> movw r3, #0x0001
                original=bytes.fromhex("40 f2 01 13"),
                replacement=bytes.fromhex("40 f2 01 03"),
            ),
        ),
    ),
    "bt-pair-menu": BinaryPatch(
        name="bt-pair-menu",
        description=(
            "Hook ViewTask page_bt_menu_create (@ 0x00CFFA56 / flash 0x0FFA56 -> 0x00D74020) "
            "so opening the recorder's Bluetooth menu opens a 60-second physical-presence "
            "pairing window, enables BT discoverability, and renders a 6-digit PIN banner "
            "on the LCD via LVGL. Requires --xip-payload compiled from fw_mod/bt_filesync.c v5+."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x0FFA56,  # VMA 0x00CFFA56: bl 0x00D02D64 -> bl 0x00D74020
                original=bytes.fromhex("03 f0 85 f9"),
                replacement=bytes.fromhex("74 f0 e3 fa"),
            ),
        ),
    ),
    "bt-rfcomm-turbo": BinaryPatch(
        name="bt-rfcomm-turbo",
        description=(
            "Raise RFCOMM negotiated DLCI MTU from 512 B to 660 B (0x294, safely inside "
            "672 B L2CAP MTU) and shorten RFCOMM RX credit-refill delay timer from 100 ms "
            "to 5 ms (threshold <=16, batch 32 credits) for faster +XREC downloads and OTA."
        ),
        hunks=(
            PatchHunk(
                flash_offset=0x1641B8,  # Outgoing DLCI MTU: mov.w r3, #0x200 -> movw r3, #0x294 (660)
                original=bytes.fromhex("4f f4 00 73"),
                replacement=bytes.fromhex("40 f2 94 23"),
            ),
            PatchHunk(
                flash_offset=0x16429A,  # Server DLCI MTU: mov.w r3, #0x200 -> movw r3, #0x294 (660)
                original=bytes.fromhex("4f f4 00 73"),
                replacement=bytes.fromhex("40 f2 94 23"),
            ),
            PatchHunk(
                flash_offset=0x163DDA,  # Credit refill delay timer: 100 ms -> 5 ms
                original=bytes.fromhex("64 20"),
                replacement=bytes.fromhex("05 20"),
            ),
            PatchHunk(
                flash_offset=0x163F72,  # Credit refill delay timer: 100 ms -> 5 ms
                original=bytes.fromhex("64 20"),
                replacement=bytes.fromhex("05 20"),
            ),
            PatchHunk(
                flash_offset=0x163DB2,  # Credit refill threshold: <= 7 -> <= 16
                original=bytes.fromhex("07 2b"),
                replacement=bytes.fromhex("10 2b"),
            ),
            PatchHunk(
                flash_offset=0x163F4A,  # Credit refill threshold: <= 7 -> <= 16
                original=bytes.fromhex("07 2b"),
                replacement=bytes.fromhex("10 2b"),
            ),
            PatchHunk(
                flash_offset=0x163DBA,  # Credit refill batch: 16 -> 32
                original=bytes.fromhex("10 22"),
                replacement=bytes.fromhex("20 22"),
            ),
            PatchHunk(
                flash_offset=0x163F52,  # Credit refill batch: 16 -> 32
                original=bytes.fromhex("10 22"),
                replacement=bytes.fromhex("20 22"),
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


def encode_thumb2_bl(pc: int, target: int) -> bytes:
    """Encode a 32-bit Thumb-2 BL instruction from `pc` to `target`."""
    offset = target - (pc + 4)
    if offset < -(1 << 24) or offset >= (1 << 24) or (offset & 1):
        raise ValueError(f"BL target 0x{target:08X} out of range from 0x{pc:08X}")
    s = (offset >> 24) & 1
    i1 = (offset >> 23) & 1
    i2 = (offset >> 22) & 1
    j1 = (~(i1 ^ s)) & 1
    j2 = (~(i2 ^ s)) & 1
    imm10 = (offset >> 12) & 0x3FF
    imm11 = (offset >> 1) & 0x7FF
    hw1 = 0xF000 | (s << 10) | imm10
    hw2 = 0xD000 | (j1 << 13) | (1 << 12) | (j2 << 11) | imm11
    return struct.pack("<HH", hw1, hw2)


def inject_xip_payload(buf: bytearray, payload: bytes) -> int:
    """Inject compiled XIP payload at 0x174000 and patch BL at 0x1625E2 -> 0x00D74000.

    Returns the end flash offset (XIP_PAYLOAD_OFF + len(payload)).
    """
    if not (1 <= len(payload) <= XIP_MAX_PAYLOAD_SIZE):
        raise ValueError(f"XIP payload size {len(payload)} out of range (1..{XIP_MAX_PAYLOAD_SIZE})")

    firm_off, firm_size, _ = read_firm_layout(buf)
    if not (firm_off <= XIP_HOOK_FLASH_OFF < firm_off + firm_size):
        raise ValueError("XIP hook offset falls outside FIRM partition")
    if XIP_PAYLOAD_OFF < firm_off + firm_size:
        raise ValueError("XIP payload overlaps FIRM partition")

    expected_orig_bl = encode_thumb2_bl(XIP_HOOK_VMA, XIP_ORIG_TARGET_VMA)
    new_bl = encode_thumb2_bl(XIP_HOOK_VMA, XIP_PAYLOAD_VMA)
    actual_bl = bytes(buf[XIP_HOOK_FLASH_OFF : XIP_HOOK_FLASH_OFF + 4])
    if actual_bl not in (expected_orig_bl, new_bl):
        raise ValueError(
            f"Unexpected instruction at 0x{XIP_HOOK_VMA:06X}: got {actual_bl.hex()}, "
            f"expected {expected_orig_bl.hex()} (is this pen133_2.0.65?)"
        )

    if actual_bl == expected_orig_bl:
        target_slice = buf[XIP_PAYLOAD_OFF : XIP_PAYLOAD_OFF + len(payload)]
        if any(b != 0xFF for b in target_slice):
            raise ValueError(f"Target XIP region 0x{XIP_PAYLOAD_OFF:06X} is not erased 0xFF!")

    buf[XIP_HOOK_FLASH_OFF : XIP_HOOK_FLASH_OFF + 4] = new_bl
    buf[XIP_PAYLOAD_OFF : XIP_PAYLOAD_OFF + len(payload)] = payload
    print(
        f"  [XIP] Hook @ 0x{XIP_HOOK_VMA:06X} (flash 0x{XIP_HOOK_FLASH_OFF:06X}): "
        f"{actual_bl.hex()} -> {new_bl.hex()} (bl 0x{XIP_PAYLOAD_VMA:06X})"
    )
    print(
        f"  [XIP] Payload @ 0x{XIP_PAYLOAD_VMA:06X} (flash 0x{XIP_PAYLOAD_OFF:06X}): "
        f"{len(payload)} bytes"
    )
    return XIP_PAYLOAD_OFF + len(payload)


def build_update_up(buf: bytes | bytearray, min_end_off: int = 0) -> bytes:
    """Wrap the flash image up to max(FIRM end, min_end_off) (4 KiB aligned, < PSMP) in 0:\\update.up format."""
    firm_off, firm_size, _ = read_firm_layout(buf)
    end_off = (max(firm_off + firm_size, min_end_off) + 0xFFF) & ~0xFFF
    if end_off > PSMP_OFF:
        raise ValueError(f"Image end 0x{end_off:X} overlaps PSMP at 0x{PSMP_OFF:X}")

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
    ap.add_argument("--xip-payload", type=pathlib.Path, help="Inject compiled XIP payload (e.g. bt_filesync.bin) at 0x174000 and hook 0x1625E2")
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

    orig_raw = args.input.read_bytes()
    raw = bytearray(orig_raw)
    if len(raw) != FLASH_SIZE:
        print(f"Error: expected {FLASH_SIZE} bytes, got {len(raw)}", file=sys.stderr)
        return 1

    print(f"Loaded {args.input} ({len(raw)} bytes)")
    if args.verify or not (args.patch or args.xip_payload or args.out_bin or args.out_update_up):
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

    xip_end_off = 0
    if args.xip_payload:
        xip_end_off = inject_xip_payload(raw, args.xip_payload.read_bytes())

    if args.bump_timestamp:
        firm_off, _, _ = read_firm_layout(raw)
        old_ts = struct.unpack_from("<I", raw, firm_off + 0x04)[0]
        struct.pack_into("<I", raw, firm_off + 0x04, (old_ts + 1) & 0xFFFFFFFF)
        print(f"  [BUMPED] FIRM timestamp: 0x{old_ts:08X} -> 0x{(old_ts + 1) & 0xFFFFFFFF:08X}")

    if args.patch or args.xip_payload or args.bump_timestamp:
        update_firm_checksums(raw)
        print("Recomputed FIRM checksums:")
        assert verify_checksums(raw)

    changed_sectors = [
        sec for sec in range(0, len(raw), 0x1000)
        if raw[sec : sec + 0x1000] != orig_raw[sec : sec + 0x1000]
    ]
    if changed_sectors:
        print(f"  Changed 4 KiB flash sectors ({len(changed_sectors)}): {[hex(s) for s in changed_sectors]}")

    if args.out_bin:
        args.out_bin.write_bytes(raw)
        print(f"Wrote patched 2 MB flash image: {args.out_bin} (SHA-256: {hashlib.sha256(raw).hexdigest()})")

    if args.out_update_up:
        pkg = build_update_up(raw, min_end_off=xip_end_off)
        args.out_update_up.write_bytes(pkg)
        print(f"Wrote SD update package: {args.out_update_up} ({len(pkg)} bytes, covers up to 0x{len(pkg) - 0x200:06X}, < PSMP 0x1FC000)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
