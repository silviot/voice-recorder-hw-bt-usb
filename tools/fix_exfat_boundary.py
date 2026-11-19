#!/usr/bin/env python3
"""
In-place MBR + exFAT Volume Boot Record (VBR) tail-sector boundary fix for
Smartlink SL6801 / SL6806 (301a:2801) USB Mass Storage devices on Linux.

Problem:
  Factory-formatted SD cards on SL6801 devices allocate partition 1 (`sda1`)
  all the way to the final reported sector of the card (e.g., start=64,
  size=122,101,696 -> end=122,101,760).
  When attached to Linux, daemons like `fwupd` and `libblkid` probe the final
  4 KiB of `sda1` (LBA 122,101,752..122,101,759) looking for backup GPT / RAID
  superblocks. The SL6801 USB firmware has an LBA boundary bug that hangs for
  15 seconds and resets the USB bus whenever those final sectors are read.

Solution:
  Shrink both the MBR partition entry and the exFAT Main/Backup VBRs by
  exactly 1 cluster (256 sectors = 128 KiB), recomputing the exFAT VBR
  repeating 32-bit checksum sector.
  Shrinking by 1 cluster (from 476,935 to 476,934 clusters) keeps the exFAT
  Allocation Bitmap byte length unchanged (`ceil(476934 / 8) == 59,617` bytes)
  while keeping all partition reads 128 KiB away from the broken tail sectors.

SPDX-License-Identifier: MIT
"""
import os
import struct
import sys
from pathlib import Path


def exfat_vbr_checksum(sectors_11: bytes) -> int:
    """Compute the standard 32-bit exFAT Boot Sector checksum over sectors 0..10."""
    if len(sectors_11) != 11 * 512:
        raise ValueError("exFAT VBR checksum requires exactly 11 sectors (5632 bytes)")
    csum = 0
    for idx, byte in enumerate(sectors_11):
        # Skip VolumeFlags (0x6A, 0x6B) and PercentInUse (0x70 = 112)
        if idx in (106, 107, 112):
            continue
        csum = (((csum << 31) | (csum >> 1)) + byte) & 0xFFFFFFFF
    return csum


def fix_device(dev_path: Path, dry_run: bool = False) -> None:
    with open(dev_path, "r+b" if not dry_run else "rb", buffering=0) as disk:
        mbr = bytearray(disk.read(512))
        if mbr[510:512] != b"\x55\xaa":
            raise RuntimeError("Invalid MBR signature (expected 0x55AA)")

        p1_start, p1_size = struct.unpack_from("<II", mbr, 0x1BE + 8)
        print(f"MBR Partition 1: start={p1_start}, size={p1_size} sectors")

        disk.seek(p1_start * 512)
        vbr_region = bytearray(disk.read(24 * 512))
        if vbr_region[3:11] != b"EXFAT   ":
            raise RuntimeError(f"Partition at sector {p1_start} is not exFAT")

        vol_off, vol_len, fat_off, fat_len, clus_off, clus_cnt = struct.unpack_from(
            "<QQIIII", vbr_region, 0x40
        )
        sec_per_clus_shift = vbr_region[0x6D]
        sec_per_clus = 1 << sec_per_clus_shift
        print(
            f"exFAT VBR:       vol_off={vol_off}, vol_len={vol_len}, "
            f"clus_cnt={clus_cnt}, sec_per_clus={sec_per_clus}"
        )

        # Shrink by 1 cluster (256 sectors = 128 KiB)
        new_size = p1_size - sec_per_clus
        new_clus = clus_cnt - 1
        old_bmap_bytes = (clus_cnt + 7) // 8
        new_bmap_bytes = (new_clus + 7) // 8
        print(
            f"Target geometry: new_size={new_size} (-{sec_per_clus} sectors), "
            f"new_clus={new_clus} (bitmap bytes: {old_bmap_bytes} -> {new_bmap_bytes})"
        )

        if old_bmap_bytes != new_bmap_bytes:
            raise RuntimeError(
                f"Bitmap byte size would change ({old_bmap_bytes} -> {new_bmap_bytes}); aborting."
            )

        # Patch MBR partition 1 size
        struct.pack_into("<I", mbr, 0x1BE + 12, new_size)

        # Patch Main VBR (offset 0) and Backup VBR (offset 12 sectors)
        for vbr_idx, base_sec in (("Main VBR", 0), ("Backup VBR", 12)):
            off = base_sec * 512
            vbr_region[off + 0x6A] = 0x00  # Clear VolumeDirty bit
            struct.pack_into("<Q", vbr_region, off + 0x48, new_size)
            struct.pack_into("<I", vbr_region, off + 0x5C, new_clus)
            csum = exfat_vbr_checksum(bytes(vbr_region[off : off + 11 * 512]))
            vbr_region[off + 11 * 512 : off + 12 * 512] = struct.pack("<I", csum) * 128
            print(f"  {vbr_idx:10s}: updated CRC32=0x{csum:08X}")

        if dry_run:
            print("Dry run complete; no changes written.")
            return

        disk.seek(0)
        disk.write(bytes(mbr))
        disk.seek(p1_start * 512)
        disk.write(bytes(vbr_region))
        os.fsync(disk.fileno())
        print(f"Successfully patched and synced {dev_path}.")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(f"Usage: sudo {sys.argv[0]} [--dry-run] /dev/sdX")
        sys.exit(1)
    dry = "--dry-run" in sys.argv
    target = Path(sys.argv[-1])
    fix_device(target, dry_run=dry)
