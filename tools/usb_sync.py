#!/usr/bin/env python3
"""
USB Auto-Sync & Recording Importer for Smartlink SL6801 (HW_76) Voice Recorders.

Automatically detects when the voice recorder's exFAT volume is mounted (or
watches for udev block/mount events with `--watch`), copies any new `.wav` /
`.mp3` recordings from `record/` into a local destination directory, verifies
SHA-256 integrity, and maintains a local `.sync_manifest.json` so already-synced
recordings are not re-copied.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional


USB_VENDOR_ID = "301a"
USB_PRODUCT_ID = "2801"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def find_recorder_mountpoint(explicit_mount: Optional[Path] = None) -> Optional[Path]:
    """Locate the mounted exFAT partition belonging to the 301a:2801 voice recorder."""
    if explicit_mount:
        return explicit_mount if (explicit_mount / "record").is_dir() else None

    try:
        out = subprocess.check_output(
            ["lsblk", "-J", "-o", "NAME,VENDOR,MODEL,SERIAL,FSTYPE,MOUNTPOINT"],
            text=True,
        )
        data = json.loads(out)
    except Exception:
        data = {}

    for dev in data.get("blockdevices", []):
        vendor = (dev.get("vendor") or "").strip()
        model = (dev.get("model") or "").strip()
        serial = (dev.get("serial") or "").strip()
        if "SMTLINK" in vendor or "CARDREADER" in model or serial == "20201111000001":
            for child in dev.get("children", [dev]):
                mp = child.get("mountpoint")
                if mp and Path(mp).is_dir():
                    return Path(mp)
                # Try auto-mounting via udisksctl if unmounted partition exists
                part_name = child.get("name")
                if part_name and child.get("fstype") == "exfat" and not mp:
                    try:
                        subprocess.run(
                            ["udisksctl", "mount", "-b", f"/dev/{part_name}"],
                            check=False,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    except Exception:
                        pass

    # Fallback: scan common user media directories for a volume with DBROOT + record
    for base in (Path("/run/media"), Path("/media")):
        if not base.is_dir():
            continue
        for candidate in base.glob("*/*"):
            if (candidate / "record").is_dir() and (candidate / "DBROOT").is_dir():
                return candidate
    return None


def sync_recordings(
    mount_point: Path,
    dest_dir: Path,
    delete_after_sync: bool = False,
) -> List[Path]:
    record_dir = mount_point / "record"
    if not record_dir.is_dir():
        print(f"No 'record/' directory found at {mount_point}")
        return []

    dest_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = dest_dir / ".sync_manifest.json"
    manifest: Dict[str, dict] = {}
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except Exception:
            manifest = {}

    synced_files: List[Path] = []
    audio_files = sorted(
        p for p in record_dir.iterdir()
        if p.is_file() and p.suffix.lower() in (".wav", ".mp3", ".flac")
    )

    for src in audio_files:
        stat = src.stat()
        key = src.name
        cached = manifest.get(key)
        dst = dest_dir / src.name

        if cached and cached.get("size") == stat.st_size and dst.is_file():
            continue

        print(f"Syncing {src.name} ({stat.st_size:,} bytes)...")
        tmp_dst = dest_dir / f".{src.name}.part"
        shutil.copy2(src, tmp_dst)
        src_hash = sha256_file(src)
        dst_hash = sha256_file(tmp_dst)
        if src_hash != dst_hash:
            tmp_dst.unlink(missing_ok=True)
            raise RuntimeError(f"Checksum mismatch while copying {src.name}")

        tmp_dst.replace(dst)
        manifest[key] = {
            "filename": src.name,
            "size": stat.st_size,
            "sha256": dst_hash,
            "mtime": datetime.fromtimestamp(stat.st_mtime, tz=timezone.utc).isoformat(),
            "synced_at": datetime.now(tz=timezone.utc).isoformat(),
            "source_type": "phone_call_bt" if "-phone" in src.name.lower() else "mic_recorder",
        }
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        synced_files.append(dst)
        print(f"  -> Saved to {dst} (SHA-256: {dst_hash[:16]}...)")

        if delete_after_sync:
            src.unlink()
            print(f"  -> Removed {src.name} from device")

    return synced_files


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Auto-sync voice recordings from Smartlink SL6801 (HW_76) over USB."
    )
    parser.add_argument(
        "-d", "--dest",
        type=Path,
        default=Path.home() / "Recordings" / "HW_76",
        help="Destination folder for synced recordings (default: ~/Recordings/HW_76)",
    )
    parser.add_argument(
        "-m", "--mount",
        type=Path,
        default=None,
        help="Explicit mountpoint of the recorder (auto-detected if omitted)",
    )
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Continuously watch for the voice recorder being plugged in over USB",
    )
    parser.add_argument(
        "--delete-after-sync",
        action="store_true",
        help="Delete recordings from the device SD card after verified SHA-256 copy",
    )
    args = parser.parse_args()

    if not args.watch:
        mp = find_recorder_mountpoint(args.mount)
        if not mp:
            print("HW_76 (301a:2801) USB volume not currently mounted.")
            sys.exit(1)
        print(f"Found HW_76 volume at: {mp}")
        newly_synced = sync_recordings(mp, args.dest, args.delete_after_sync)
        print(f"Sync complete ({len(newly_synced)} new recording(s) imported to {args.dest}).")
        return

    print(f"Watching for HW_76 USB connection -> syncing to {args.dest} ...")
    last_seen = False
    while True:
        mp = find_recorder_mountpoint(args.mount)
        if mp:
            if not last_seen:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Detected HW_76 at {mp}")
                newly_synced = sync_recordings(mp, args.dest, args.delete_after_sync)
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Imported {len(newly_synced)} new file(s).")
                last_seen = True
        else:
            last_seen = False
        time.sleep(2.0)


if __name__ == "__main__":
    main()
