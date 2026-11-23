#!/usr/bin/env python3
"""
Rapid unattended development, flashing & experiment turnaround tool for SL6801 OpenFW.

Automates the hands-free workflow across OpenFW (1209:0001) and BootROM DFU (301a:2800):
  - status        : inspect current USB mode, device info/log or NOR ID
  - dfu           : switch 1209:0001 -> 301a:2800 DFU via 0xE0 REBOOT
  - boot          : reboot 301a:2800 DFU -> 1209:0001 OpenFW via BootROM RUNRAM
  - flash         : (optional build) switch to DFU, flash 0x00D000..end_off, reboot & verify
  - exp <src.c>   : compile, upload, run SRAM experiment, print result + log (auto-recovers on crash)
  - log [--reset] : read or reset g_openfw_log
  - --self-test   : offline verification of safe flash range bounds & USB sysfs detection

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_ROOT / "tools"
OPENFW_DIR = REPO_ROOT / "open_fw"
FIRM_BIN = Path(os.environ.get("OPENFW_FIRM_BIN", "/tmp/sl6801_openfw/open_fw_firm.bin"))
IMAGE_2M = REPO_ROOT / "dumps" / "firmware_2M_openfw.bin"

PARTITION_TABLE_OFF = 0x00D000
FIRM_PART_OFF = 0x00E000
PSMP_NVRAM_OFF = 0x1FC000
SECTOR_SIZE = 4096

sys.path.insert(0, str(TOOLS_DIR))
import flash_ranges  # noqa: E402
import openfw_usb  # noqa: E402
import run_exp  # noqa: E402


USB_MODE_MAP: dict[tuple[int, int], str] = {
    (openfw_usb.VID, openfw_usb.PID): "OPENFW",
    (flash_ranges.VID, flash_ranges.PID_BOOTLOADER): "DFU",
    (flash_ranges.VID, 0x2801): "STOCK",
}


def detect_usb_device(sys_usb_root: Path = Path("/sys/bus/usb/devices")) -> str | None:
    """Scan Linux sysfs USB devices directory and return 'DFU', 'OPENFW', 'STOCK', or None."""
    if not sys_usb_root.is_dir():
        return None
    found: set[str] = set()
    for dev_dir in sys_usb_root.iterdir():
        vf = dev_dir / "idVendor"
        pf = dev_dir / "idProduct"
        if not (vf.is_file() and pf.is_file()):
            continue
        try:
            vid = int(vf.read_text(encoding="ascii").strip(), 16)
            pid = int(pf.read_text(encoding="ascii").strip(), 16)
        except (OSError, ValueError):
            continue
        mode = USB_MODE_MAP.get((vid, pid))
        if mode is not None:
            found.add(mode)
    for pref in ("DFU", "OPENFW", "STOCK"):
        if pref in found:
            return pref
    return None


def wait_for_usb_mode(target_mode: str, timeout_s: float = 15.0) -> str:
    """Poll sysfs until target_mode appears."""
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = detect_usb_device()
        if last == target_mode:
            return last
        time.sleep(0.15)
    raise TimeoutError(f"Timed out waiting for USB mode '{target_mode}' (last seen: {last})")


def open_openfw_with_retry(timeout_s: float = 8.0, io_timeout_ms: int = 20000) -> openfw_usb.OpenFwUsbClient:
    """Open 1209:0001 transport, waiting briefly for /dev/sg* or libusb settle after enumeration."""
    deadline = time.monotonic() + timeout_s
    last_exc: Exception | None = None
    while time.monotonic() < deadline:
        try:
            tr = openfw_usb.open_live_transport(
                vid=openfw_usb.VID, pid=openfw_usb.PID, timeout_ms=io_timeout_ms
            )
            return openfw_usb.OpenFwUsbClient(tr, default_timeout_ms=io_timeout_ms)
        except Exception as exc:
            last_exc = exc
            time.sleep(0.25)
    raise RuntimeError(f"Failed opening OpenFW 1209:0001 within {timeout_s:.1f}s: {last_exc}")


def compute_openfw_flash_range(firm_len: int) -> tuple[int, int]:
    """Compute minimal 4 KiB-aligned (offset, length) covering 0x00D000 + FIRM payload."""
    if firm_len <= 0:
        raise ValueError(f"Invalid FIRM payload length: {firm_len}")
    end_off = ((FIRM_PART_OFF + firm_len + SECTOR_SIZE - 1) // SECTOR_SIZE) * SECTOR_SIZE
    if end_off > PSMP_NVRAM_OFF:
        raise ValueError(
            f"FIRM payload ({firm_len} B, end=0x{end_off:06X}) would overlap PSMP NVRAM at 0x{PSMP_NVRAM_OFF:06X}"
        )
    start_off = PARTITION_TABLE_OFF
    length = end_off - start_off
    if start_off < PARTITION_TABLE_OFF or (start_off + length) > PSMP_NVRAM_OFF:
        raise AssertionError(f"Unsafe flash range 0x{start_off:06X}+0x{length:X}")
    return start_off, length


def print_openfw_snapshot(client: openfw_usb.OpenFwUsbClient, include_exp: bool = True) -> openfw_usb.OpenFwInfo:
    info = client.get_info()
    print(openfw_usb.format_info(info))
    if include_exp:
        try:
            res = client.exp_status()
            if res.run_count > 0 or res.out_len > 0 or res.arena_loaded > 0:
                print(openfw_usb.format_exp_result(res))
        except Exception:
            pass
    raw_log = client.read_log(info)
    text = raw_log.rstrip(b"\x00").decode("utf-8", errors="replace")
    if text:
        print("=== SRAM LOG ===")
        print(text, end="" if text.endswith("\n") else "\n")
    return info


def cmd_status(_args: argparse.Namespace) -> int:
    mode = detect_usb_device()
    print(f"USB mode: {mode or 'DISCONNECTED'}")
    if mode == "OPENFW":
        client = open_openfw_with_retry(timeout_s=4.0)
        try:
            print_openfw_snapshot(client, include_exp=True)
        finally:
            client.close()
    elif mode == "DFU":
        client = flash_ranges.open_bootrom_client(vid=flash_ranges.VID, pid=flash_ranges.PID_BOOTLOADER)
        try:
            client.init_nor()
            flash_id = client.read_id()
            print(f"BootROM SPI NOR ID: 0x{flash_id:08X}")
        finally:
            client.close()
    elif mode == "STOCK":
        print("Stock SL6801 Card Reader (301a:2801) detected.")
    return 0


def cmd_dfu(args: argparse.Namespace) -> int:
    mode = detect_usb_device()
    if mode == "DFU":
        print("Already in BootROM DFU (301a:2800).")
        return 0
    if mode != "OPENFW":
        raise SystemExit(f"Cannot enter DFU automatically from USB mode '{mode or 'NONE'}'")
    client = open_openfw_with_retry(timeout_s=5.0)
    try:
        client.reboot(openfw_usb.REBOOT_MODES["dfu"])
    finally:
        client.close()
    print("Sent REBOOT->DFU to 1209:0001; waiting for 301a:2800...")
    wait_for_usb_mode("DFU", timeout_s=args.timeout)
    time.sleep(0.3)
    print("BootROM DFU (301a:2800) ready.")
    return 0


def cmd_boot(args: argparse.Namespace) -> int:
    mode = detect_usb_device()
    if mode == "OPENFW":
        print("Already in OpenFW (1209:0001).")
    elif mode == "DFU":
        client = flash_ranges.open_bootrom_client(vid=flash_ranges.VID, pid=flash_ranges.PID_BOOTLOADER)
        try:
            client.reboot_chip()
        finally:
            client.close()
        print("Triggered BootROM reboot -> OpenFW; waiting for 1209:0001...")
        wait_for_usb_mode("OPENFW", timeout_s=args.timeout)
        time.sleep(0.4)
    else:
        raise SystemExit(f"Cannot boot OpenFW from USB mode '{mode or 'NONE'}'")

    client = open_openfw_with_retry(timeout_s=args.timeout)
    try:
        info = print_openfw_snapshot(client, include_exp=False)
        return 1 if info.reserved != 0 else 0
    finally:
        client.close()


def cmd_flash(args: argparse.Namespace) -> int:
    if args.build or not FIRM_BIN.is_file() or not IMAGE_2M.is_file():
        print("Building open_fw + 2M image...")
        subprocess.run(["make", "-C", str(OPENFW_DIR), "all", "update-up"], check=True)

    firm_len = FIRM_BIN.stat().st_size
    start_off, length = compute_openfw_flash_range(firm_len)
    image_bytes = IMAGE_2M.read_bytes()
    print(
        f"Safe partial flash range: 0x{start_off:06X}..0x{start_off + length:06X} "
        f"({length:,} B, preserving HLKJ & PSMP)"
    )

    mode = detect_usb_device()
    if mode == "OPENFW":
        cmd_dfu(argparse.Namespace(timeout=12.0))
    elif mode != "DFU":
        wait_for_usb_mode("DFU", timeout_s=5.0)
        time.sleep(0.3)

    br = flash_ranges.open_bootrom_client(vid=flash_ranges.VID, pid=flash_ranges.PID_BOOTLOADER)
    try:
        flash_ranges.execute_flash_plan(
            br,
            image_bytes,
            [(start_off, length)],
            chunk=flash_ranges.MAX_CHUNK,
            retries=3,
            verify=not args.no_verify,
            dry_run=False,
        )
        if not args.no_reboot:
            br.reboot_chip()
    finally:
        br.close()

    if args.no_reboot:
        return 0

    print("Waiting for new OpenFW (1209:0001)...")
    wait_for_usb_mode("OPENFW", timeout_s=15.0)
    time.sleep(0.4)
    client = open_openfw_with_retry(timeout_s=10.0)
    try:
        info = print_openfw_snapshot(client, include_exp=False)
        if info.reserved != 0:
            print(f"ERROR: Target reported previous fault 0x{info.reserved:08X}!", file=sys.stderr)
            return 1
        return 0
    finally:
        client.close()


def cmd_exp(args: argparse.Namespace) -> int:
    payload = run_exp.build_experiment(args.source)
    print(
        f"Built {args.source.name}: {len(payload)} / {run_exp.EXP_ARENA_SIZE} B "
        f"(entry=0x{run_exp.EXP_ARENA_ADDR:08X}, 0 undef)"
    )

    if detect_usb_device() == "DFU":
        cmd_boot(argparse.Namespace(timeout=15.0))
    elif detect_usb_device() != "OPENFW":
        wait_for_usb_mode("OPENFW", timeout_s=10.0)

    tmo = max(args.timeout_ms, (args.arg + 6) * 1000 if 0 < args.arg <= 120 else args.timeout_ms)
    client = open_openfw_with_retry(timeout_s=8.0, io_timeout_ms=tmo)
    try:
        if not args.keep_log:
            client.log_reset()
        client.exp_write(payload, base_offset=0)
        try:
            res = client.exp_run(entry_off=args.entry, arg=args.arg, timeout_ms=tmo)
        except (IOError, OSError, RuntimeError) as exc:
            print(f"WARN: Experiment connection dropped ({exc}); waiting for auto-recovery...", file=sys.stderr)
            client.close()
            wait_for_usb_mode("OPENFW", timeout_s=20.0)
            time.sleep(0.4)
            rec = open_openfw_with_retry(timeout_s=10.0)
            try:
                print("=== AUTO-RECOVERY SNAPSHOT ===")
                print_openfw_snapshot(rec, include_exp=False)
            finally:
                rec.close()
            return 1

        print(openfw_usb.format_exp_result(res))
        raw_log = client.read_log()
        text = raw_log.rstrip(b"\x00").decode("utf-8", errors="replace")
        if text:
            print("=== EXP LOG ===")
            print(text, end="" if text.endswith("\n") else "\n")

        if res.out_len > 0 and (args.dump_out or args.dump_wav):
            out_addr = (res.arena_base + 0x8000) if res.arena_base else run_exp.EXP_OUT_ADDR
            out_data = client.mem_read(out_addr, res.out_len)
            if args.dump_out:
                args.dump_out.parent.mkdir(parents=True, exist_ok=True)
                args.dump_out.write_bytes(out_data)
                print(f"Dumped {len(out_data)} B -> {args.dump_out}")
            if args.dump_wav:
                import wave

                args.dump_wav.parent.mkdir(parents=True, exist_ok=True)
                with wave.open(str(args.dump_wav), "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(16000)
                    wf.writeframes(out_data)
                print(f"Saved WAV ({len(out_data) // 2} samples @ 16kHz mono) -> {args.dump_wav}")
        return 0
    finally:
        client.close()


def cmd_log(args: argparse.Namespace) -> int:
    client = open_openfw_with_retry(timeout_s=6.0)
    try:
        if args.reset:
            client.log_reset()
            print("SRAM log cleared.")
            return 0
        raw_log = client.read_log()
        text = raw_log.rstrip(b"\x00").decode("utf-8", errors="replace")
        if text:
            print(text, end="" if text.endswith("\n") else "\n")
        return 0
    finally:
        client.close()


def run_self_test() -> int:
    """Offline test for safe flash range math, bounds checks, and USB sysfs scanning."""
    start, length = compute_openfw_flash_range(82000)
    assert start == 0x00D000
    assert (start + length) % SECTOR_SIZE == 0
    assert 0x00D000 + 82000 < start + length <= PSMP_NVRAM_OFF
    assert start + length == 0x023000

    overflow_err = False
    try:
        compute_openfw_flash_range(PSMP_NVRAM_OFF - FIRM_PART_OFF + 1)
    except ValueError:
        overflow_err = True
    assert overflow_err, "Expected PSMP overlap check to fail"

    with tempfile.TemporaryDirectory(prefix="dev_cycle_test_") as tmp:
        root = Path(tmp)
        assert detect_usb_device(root) is None
        d1 = root / "1-1"
        d1.mkdir()
        (d1 / "idVendor").write_text("1209\n", encoding="ascii")
        (d1 / "idProduct").write_text("0001\n", encoding="ascii")
        assert detect_usb_device(root) == "OPENFW"

        d2 = root / "1-2"
        d2.mkdir()
        (d2 / "idVendor").write_text("301a\n", encoding="ascii")
        (d2 / "idProduct").write_text("2800\n", encoding="ascii")
        assert detect_usb_device(root) == "DFU"

    print("dev_cycle.py self-test PASSED (safe range math, PSMP guard, USB sysfs detection)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Rapid OpenFW dev cycle tool")
    ap.add_argument("--self-test", action="store_true", help="Run offline unit self-test")
    sub = ap.add_subparsers(dest="cmd")

    sub.add_parser("status", help="Show USB mode & device status/log")

    p_dfu = sub.add_parser("dfu", help="Switch 1209:0001 -> 301a:2800 DFU")
    p_dfu.add_argument("--timeout", type=float, default=12.0)

    p_boot = sub.add_parser("boot", help="Reboot 301a:2800 DFU -> 1209:0001 OpenFW")
    p_boot.add_argument("--timeout", type=float, default=15.0)

    p_flash = sub.add_parser("flash", help="Flash OpenFW safely (0x00D000..end) & reboot")
    p_flash.add_argument("--build", action="store_true", help="Rebuild before flashing")
    p_flash.add_argument("--no-verify", action="store_true", help="Skip CRC/readback verify")
    p_flash.add_argument("--no-reboot", action="store_true", help="Stay in 301a:2800 after flashing")

    p_exp = sub.add_parser("exp", help="Build, upload, run SRAM experiment")
    p_exp.add_argument("source", type=Path)
    p_exp.add_argument("--arg", type=lambda s: int(s, 0), default=0)
    p_exp.add_argument("--entry", type=lambda s: int(s, 0), default=0)
    p_exp.add_argument("--dump-out", type=Path)
    p_exp.add_argument("--dump-wav", type=Path)
    p_exp.add_argument("--keep-log", action="store_true")
    p_exp.add_argument("--timeout-ms", type=int, default=20000)

    p_log = sub.add_parser("log", help="Read or clear g_openfw_log")
    p_log.add_argument("--reset", action="store_true")

    args = ap.parse_args()
    if args.self_test:
        return run_self_test()

    dispatch = {
        "status": cmd_status,
        "dfu": cmd_dfu,
        "boot": cmd_boot,
        "flash": cmd_flash,
        "exp": cmd_exp,
        "log": cmd_log,
    }
    fn = dispatch.get(args.cmd)
    if fn is None:
        ap.print_help()
        return 2
    return fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
