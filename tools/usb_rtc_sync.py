#!/usr/bin/env python3
"""
USB RTC reader & clock synchronizer for Cassini OpenFW (1209:0001).

Uses OpenFW's native vendor SCSI 0xE0 RTC subcommands (via tools/openfw_usb.py).
Never sends BootROM 0xC0 commands to stock Card Reader mode (301a:2801), in
accordance with AGENTS.md's strict USB PID guard.

Examples:
  python3 tools/usb_rtc_sync.py --read
  python3 tools/usb_rtc_sync.py --sync
  python3 tools/usb_rtc_sync.py --set "2026-11-19 12:00:00"
  python3 tools/usb_rtc_sync.py --dry-run

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import datetime
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import openfw_usb  # noqa: E402


def pack_rtc_time(dt: datetime.datetime) -> bytes:
    """Pack datetime into OpenFW 8-byte wire format (delegates to openfw_usb.pack_rtc)."""
    return openfw_usb.pack_rtc(dt)


def unpack_rtc_time(raw: bytes) -> tuple[datetime.datetime, int]:
    """Unpack OpenFW 8-byte wire payload into (datetime, weekday)."""
    return openfw_usb.unpack_rtc(raw)


def _open_openfw_client() -> openfw_usb.OpenFwUsbClient:
    try:
        tr = openfw_usb.open_live_transport()
    except Exception as exc:
        try:
            import usb.core

            if usb.core.find(idVendor=0x301A, idProduct=0x2801) is not None:
                raise SystemExit(
                    "Stock SL6801 Card Reader (301a:2801) detected. BootROM 0xC0 commands are prohibited "
                    "on 301a:2801 per AGENTS.md; flash open_fw via BootROM DFU (301a:2800) first."
                ) from exc
        except ImportError:
            pass
        raise SystemExit(
            "Cassini OpenFW (1209:0001) not found. Plug in the recorder or run "
            "`python3 tools/openfw_usb.py --rtc-sync`."
        ) from exc
    return openfw_usb.OpenFwUsbClient(tr)


def main() -> int:
    ap = argparse.ArgumentParser(description="Read or sync Cassini OpenFW hardware RTC over USB (0xE0)")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--read", action="store_true", help="Read current hardware RTC from device")
    group.add_argument("--sync", action="store_true", help="Sync device RTC to host local clock")
    group.add_argument("--set", metavar="YYYY-MM-DD HH:MM:SS", help="Set device RTC to explicit timestamp")
    group.add_argument("--dry-run", action="store_true", help="Self-test OpenFW RTC packing without USB")
    args = ap.parse_args()

    if args.dry_run:
        now = datetime.datetime.now().replace(microsecond=0)
        packed = pack_rtc_time(now)
        dt2, wd = unpack_rtc_time(packed)
        assert dt2 == now and wd == now.weekday()
        print(f"Dry-run OK: {now.isoformat(' ')} -> {packed.hex(' ')} ({len(packed)}B)")
        return 0

    client = _open_openfw_client()
    try:
        if args.read:
            dt, wd = client.get_rtc()
            print(f"Device RTC: {dt.strftime('%Y-%m-%d %H:%M:%S')} (weekday={wd})")
        else:
            target = (
                datetime.datetime.now().replace(microsecond=0)
                if args.sync
                else datetime.datetime.strptime(args.set, "%Y-%m-%d %H:%M:%S")
            )
            client.set_rtc(target)
            rb, wd = client.get_rtc()
            print(f"RTC updated: {rb.strftime('%Y-%m-%d %H:%M:%S')} (weekday={wd})")
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
