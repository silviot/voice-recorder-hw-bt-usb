#!/usr/bin/env python3
"""
Clean-room firmware patcher for Smartlink SL6801 (`pen133_2.0.65`).

Applies the Bluetooth RFCOMM file-transfer XIP extension (`bt_filesync.bin`)
into the erased 0xFF SPI NOR flash space at offset 0x174000 (VMA 0x00D74000),
patches the single 4-byte `BL` call at VMA 0x00D625E2 (flash 0x1625E2), and
recomputes the `FIRM` partition CRC-16/CCITT-FALSE in the partition table.

Never touches:
  - Stage-1 HLKJ Bootloader (0x000000..0x00C950)
  - PSMP NVRAM / calibration / BT MAC / link keys (0x1FC000..0x200000)

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path


HOOK_VMA = 0x00D625E2
HOOK_FLASH_OFF = HOOK_VMA - 0x00C00000  # 0x1625E2
ORIG_TARGET_VMA = 0x00D62180
XIP_PAYLOAD_VMA = 0x00D74000
XIP_PAYLOAD_OFF = XIP_PAYLOAD_VMA - 0x00C00000  # 0x174000
MAX_PAYLOAD_SIZE = 0x4000  # 16 KiB


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        t = ((crc >> 8) ^ b) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


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


def patch_firmware(src_fw: Path, payload_bin: Path, dst_fw: Path) -> None:
    fw = bytearray(src_fw.read_bytes())
    payload = payload_bin.read_bytes()

    if len(fw) != 0x200000:
        raise RuntimeError(f"Expected 2 MB flash dump, got {len(fw)} bytes")
    if fw[:4] != b"HLKJ":
        raise RuntimeError("Missing HLKJ bootloader magic")
    if len(payload) == 0 or len(payload) > MAX_PAYLOAD_SIZE:
        raise RuntimeError(f"Payload size {len(payload)} out of range (1..{MAX_PAYLOAD_SIZE})")

    # 1. Verify expected original BL instruction at 0xD625E2 -> 0xD62180
    expected_orig_bl = encode_thumb2_bl(HOOK_VMA, ORIG_TARGET_VMA)
    actual_orig_bl = bytes(fw[HOOK_FLASH_OFF : HOOK_FLASH_OFF + 4])
    if actual_orig_bl != expected_orig_bl:
        raise RuntimeError(
            f"Unexpected instruction at 0x{HOOK_VMA:06X}: got {actual_orig_bl.hex()}, "
            f"expected {expected_orig_bl.hex()} (is this pen133_2.0.65?)"
        )

    # 2. Verify target XIP region at 0x174000 is completely erased (0xFF)
    target_slice = fw[XIP_PAYLOAD_OFF : XIP_PAYLOAD_OFF + len(payload)]
    if any(b != 0xFF for b in target_slice):
        raise RuntimeError(f"Target XIP region 0x{XIP_PAYLOAD_OFF:06X} is not erased 0xFF!")

    # 3. Verify FIRM partition header and original CRC before patching
    ptab_off = struct.unpack_from("<I", fw, 0x20)[0]
    count, _, ver = struct.unpack_from("<III", fw, ptab_off)
    firm_entry = None
    for i in range(count):
        e = ptab_off + 16 + i * 16
        name = fw[e : e + 4]
        if name == b"FIRM":
            p_off, p_size = struct.unpack_from("<II", fw, e + 4)
            crc_off = e + (14 if ver >= 0x30 else 12)
            orig_crc = struct.unpack_from("<H", fw, crc_off)[0]
            calc_crc = crc16_ccitt_false(bytes(fw[p_off : p_off + p_size]))
            if orig_crc != calc_crc:
                raise RuntimeError(
                    f"Original FIRM CRC mismatch: stored=0x{orig_crc:04X}, calc=0x{calc_crc:04X}"
                )
            firm_entry = (p_off, p_size, crc_off, orig_crc)
            break
    if not firm_entry:
        raise RuntimeError("FIRM partition not found in partition table")

    p_off, p_size, crc_off, orig_crc = firm_entry
    if not (p_off <= HOOK_FLASH_OFF < p_off + p_size):
        raise RuntimeError("Hook offset falls outside FIRM partition")
    if XIP_PAYLOAD_OFF < p_off + p_size:
        raise RuntimeError("XIP payload overlaps FIRM partition")

    # 4. Write new BL instruction and XIP payload
    new_bl = encode_thumb2_bl(HOOK_VMA, XIP_PAYLOAD_VMA)
    fw[HOOK_FLASH_OFF : HOOK_FLASH_OFF + 4] = new_bl
    fw[XIP_PAYLOAD_OFF : XIP_PAYLOAD_OFF + len(payload)] = payload

    # 5. Recompute FIRM CRC-16 in partition table
    new_crc = crc16_ccitt_false(bytes(fw[p_off : p_off + p_size]))
    struct.pack_into("<H", fw, crc_off, new_crc)

    dst_fw.write_bytes(fw)
    print(f"Patched firmware written to: {dst_fw}")
    print(f"  Hook @ 0x{HOOK_VMA:06X} (flash 0x{HOOK_FLASH_OFF:06X}): {actual_orig_bl.hex()} -> {new_bl.hex()} (bl 0x{XIP_PAYLOAD_VMA:06X})")
    print(f"  XIP payload @ 0x{XIP_PAYLOAD_VMA:06X} (flash 0x{XIP_PAYLOAD_OFF:06X}): {len(payload)} bytes")
    print(f"  FIRM CRC-16 @ 0x{crc_off:06X}: 0x{orig_crc:04X} -> 0x{new_crc:04X}")
    print(f"  Output SHA-256: {hashlib.sha256(fw).hexdigest()}")

    # Report exact 4 KiB flash sectors that changed
    orig_bytes = src_fw.read_bytes()
    changed_sectors = [
        sec for sec in range(0, len(fw), 0x1000)
        if fw[sec : sec + 0x1000] != orig_bytes[sec : sec + 0x1000]
    ]
    print(f"  Changed 4 KiB flash sectors ({len(changed_sectors)}): {[hex(s) for s in changed_sectors]}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Patch SL6801 pen133_2.0.65 firmware with BT RFCOMM file-sync XIP payload.")
    parser.add_argument("src_fw", type=Path, help="Original 2 MB firmware dump (e.g. dumps/firmware_2M.bin)")
    parser.add_argument("payload_bin", type=Path, help="Compiled XIP binary (e.g. /tmp/bt_filesync.bin)")
    parser.add_argument("dst_fw", type=Path, help="Output patched 2 MB firmware image (e.g. dumps/firmware_bt_sync.bin)")
    args = parser.parse_args()
    patch_firmware(args.src_fw, args.payload_bin, args.dst_fw)


if __name__ == "__main__":
    main()
