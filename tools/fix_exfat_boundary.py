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
import argparse
import fcntl
import os
import stat
import struct
import sys
from pathlib import Path

BLKGETSIZE64 = 0x80081272


def get_disk_sectors(disk, dev_path: Path) -> int:
    st = os.fstat(disk.fileno())
    if stat.S_ISBLK(st.st_mode):
        buf = bytearray(8)
        fcntl.ioctl(disk.fileno(), BLKGETSIZE64, buf)
        return struct.unpack("<Q", buf)[0] // 512
    return st.st_size // 512


def find_exfat_bitmap_info(disk, heap_lba: int, root_cluster: int, sec_per_clus: int) -> tuple[int, int, bool]:
    """Scan the first cluster of the exFAT root directory for the 0x81 Allocation Bitmap."""
    lba = heap_lba + (root_cluster - 2) * sec_per_clus
    disk.seek(lba * 512)
    root_data = disk.read(sec_per_clus * 512)
    for off in range(0, len(root_data), 32):
        etype = root_data[off]
        if etype == 0x00:
            break
        if etype == 0x81:
            no_fat_chain = (root_data[off + 1] & 0x02) != 0
            bmap_cluster = struct.unpack_from("<I", root_data, off + 20)[0]
            bmap_len = struct.unpack_from("<Q", root_data, off + 24)[0]
            return bmap_cluster, bmap_len, no_fat_chain
    raise RuntimeError("exFAT 0x81 Allocation Bitmap entry not found in root directory")


def find_exfat_bitmap(disk, heap_lba: int, root_cluster: int, sec_per_clus: int) -> tuple[int, int]:
    bmap_cluster, bmap_len, _ = find_exfat_bitmap_info(disk, heap_lba, root_cluster, sec_per_clus)
    return bmap_cluster, bmap_len


def is_cluster_allocated(
    disk,
    fat_lba: int,
    heap_lba: int,
    sec_per_clus: int,
    bmap_cluster: int,
    bmap_len: int,
    cluster_idx_0based: int,
    no_fat_chain: bool = False,
) -> bool:
    byte_off = cluster_idx_0based // 8
    bit_mask = 1 << (cluster_idx_0based % 8)
    if byte_off >= bmap_len:
        raise RuntimeError(f"Cluster index {cluster_idx_0based} exceeds bitmap length {bmap_len}")
    cluster_bytes = sec_per_clus * 512
    step_clusters = byte_off // cluster_bytes
    in_clus_off = byte_off % cluster_bytes
    cur = bmap_cluster + step_clusters if no_fat_chain else bmap_cluster
    if not no_fat_chain:
        for _ in range(step_clusters):
            disk.seek(fat_lba * 512 + cur * 4)
            nxt = struct.unpack("<I", disk.read(4))[0]
            if nxt >= 0xFFFFFFF8 or nxt < 2:
                cur = cur + 1  # Contiguous bitmap fallback
            else:
                cur = nxt
    disk.seek((heap_lba + (cur - 2) * sec_per_clus) * 512 + in_clus_off)
    b = disk.read(1)[0]
    return (b & bit_mask) != 0


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

        vol_off, vol_len, fat_off, fat_len, clus_off, clus_cnt, root_clus = struct.unpack_from(
            "<QQIIIII", vbr_region, 0x40
        )
        sec_per_clus_shift = vbr_region[0x6D]
        sec_per_clus = 1 << sec_per_clus_shift
        total_sectors = get_disk_sectors(disk, dev_path)
        print(
            f"exFAT VBR:       vol_off={vol_off}, vol_len={vol_len}, "
            f"clus_cnt={clus_cnt}, sec_per_clus={sec_per_clus}, disk_sectors={total_sectors}"
        )

        if vol_off != p1_start or vol_len != p1_size:
            raise RuntimeError(
                f"MBR/VBR geometry mismatch (MBR={p1_start}+{p1_size}, VBR={vol_off}+{vol_len}); aborting."
            )

        if total_sectors > 0 and (p1_start + p1_size + sec_per_clus) <= total_sectors:
            print(
                f"Partition already leaves {total_sectors - (p1_start + p1_size)} tail sectors "
                f"(>= 1 cluster of {sec_per_clus} sectors); already fixed — nothing to do."
            )
            return

        if clus_cnt < 2:
            raise RuntimeError(f"Unexpected cluster count {clus_cnt}; aborting.")

        # Verify the tail cluster being removed is FREE in the Allocation Bitmap
        fat_lba = p1_start + fat_off
        heap_lba = p1_start + clus_off
        bmap_cluster, bmap_len, no_fat_chain = find_exfat_bitmap_info(disk, heap_lba, root_clus, sec_per_clus)
        if is_cluster_allocated(
            disk, fat_lba, heap_lba, sec_per_clus, bmap_cluster, bmap_len, clus_cnt - 1, no_fat_chain=no_fat_chain
        ):
            raise RuntimeError(
                f"Tail cluster #{clus_cnt - 1} is currently ALLOCATED in exFAT bitmap! "
                "Refusing to shrink volume and lose data."
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


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="In-place MBR + exFAT tail-cluster shrink for SL6801 Linux USB stability"
    )
    ap.add_argument("--dry-run", action="store_true", help="Inspect geometry without modifying the device")
    ap.add_argument("device", type=Path, help="Block device or disk image path (e.g. /dev/sdX)")
    return ap.parse_args(argv)


if __name__ == "__main__":
    args = parse_args()
    fix_device(args.device, dry_run=args.dry_run)
