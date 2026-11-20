#!/usr/bin/env python3
"""
Capture a multi-screen tour of real Cassini OpenFW UI screens from the
SL6801 QEMU emulator via QMP button injection (`kadc-raw-ch1`).

Generates 3x-scaled 160x80 landscape PNGs (480x240) directly from the emulated
ST7735S RGB565 framebuffer into docs/assets/:
  - openfw_qemu_home.png      (Home 4-tile launcher)
  - openfw_qemu_files.png     (Files browser with sample recordings)
  - openfw_qemu_settings.png  (9-row Settings menu with Mic gain + live bar)
  - openfw_qemu_about.png     (About screen with storage, battery, DFU hint)
  - openfw_qemu_bt.png        (Bluetooth sync screen)
  - openfw_qemu_usb.png       (USB disk mode with live TX/RX byte counters)

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_qemu  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[2]
ASSETS_DIR = REPO_ROOT / "docs" / "assets"

# ADC values matching open_fw/src/keys.c resistor ladder
KEY_LEFT = 100
KEY_RIGHT = 550
KEY_VOL_UP = 1050
KEY_VOL_DOWN = 1600
KEY_BACK = 2200
KEY_ENTER = 2850
KEY_NONE = 4095


class QmpClient:
    def __init__(self, sock_path: str, timeout: float = 5.0) -> None:
        deadline = time.time() + timeout
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(2.0)
        while True:
            try:
                self.sock.connect(sock_path)
                break
            except OSError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.05)
        self._buf = b""
        self._readline()  # greeting
        self.cmd("qmp_capabilities")

    def _readline(self) -> dict:
        while b"\n" not in self._buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("QMP closed")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def cmd(self, execute: str, arguments: dict | None = None) -> dict:
        req: dict = {"execute": execute}
        if arguments is not None:
            req["arguments"] = arguments
        self.sock.sendall((json.dumps(req) + "\n").encode("utf-8"))
        while True:
            msg = self._readline()
            if "event" in msg:
                continue
            return msg

    def set_key(self, adc_val: int) -> None:
        res = self.cmd(
            "qom-set",
            {"path": "/machine", "property": "kadc-raw-ch1", "value": int(adc_val)},
        )
        if "error" in res:
            raise RuntimeError(f"QMP qom-set failed: {res}")

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def build_sample_exfat_sd(sd_path: Path) -> None:
    """Create a 4 MiB exFAT image with sample recordings using test_core."""
    test_core = Path("/tmp/sl6801_openfw/test_core")
    if not test_core.exists():
        subprocess.run(["make", "-C", str(REPO_ROOT / "open_fw"), "test-host"], check=True)
    subprocess.run(
        ["dd", "if=/dev/zero", f"of={sd_path}", "bs=512", "count=8192", "status=none"],
        check=True,
    )
    mkfs = shutil.which("mkfs.exfat") or "/usr/sbin/mkfs.exfat"
    subprocess.run([mkfs, "-q", "-b", "4096", "-c", "4096", str(sd_path)], check=True)
    # Use test_core --exfat-img to create /record/REC_0001.wav + /cassini/settings.bin
    subprocess.run([str(test_core), "--exfat-img", str(sd_path)], check=True)


def start_qemu(
    qemu_bin: str,
    rom_path: Path,
    stub_path: Path,
    flash_path: Path,
    sd_path: Path,
    vbus_off: bool,
    qmp_sock: str,
    gdb_port: int,
) -> subprocess.Popen:
    machine = run_qemu.build_machine_arg(
        rom_path, stub_path, flash_path, sd_path, kadc_ch1=KEY_NONE, vbus_off=vbus_off
    )
    cmd = [
        qemu_bin,
        "-M", machine,
        "-S",
        "-nographic",
        "-monitor", "none",
        "-serial", "none",
        "-qmp", f"unix:{qmp_sock},server,nowait",
        "-gdb", f"tcp:127.0.0.1:{gdb_port}",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def capture_screen(gdb: run_qemu.GdbRspClient, out_png: Path, scale: int) -> None:
    fb_raw = gdb.read_mem(run_qemu.FB_ADDR, run_qemu.LCD_H * run_qemu.LCD_STRIDE * 2)
    w, h, rgb, nonblack, ncolors = run_qemu.decode_fb_to_rgb24(
        fb_raw, landscape=True, scale=scale
    )
    run_qemu.write_png_rgb(out_png, w, h, rgb)
    print(f"  Captured {out_png.name} ({w}x{h}, nonblack={nonblack}, colors={ncolors})")


def wait_for_boot(gdb: run_qemu.GdbRspClient, tick_addr: int, target_ms: int = 2600) -> int:
    for _ in range(40):
        gdb.continue_for(0.4)
        ms = gdb.read_u32(tick_addr)
        if ms >= target_ms:
            return ms
    return gdb.read_u32(tick_addr)


def tap_key(
    qmp: QmpClient,
    gdb: run_qemu.GdbRspClient,
    tick_addr: int,
    key_adc: int,
    hold_ms: int = 80,
    settle_ms: int = 120,
) -> None:
    t0 = gdb.read_u32(tick_addr)
    qmp.set_key(key_adc)
    for _ in range(15):
        gdb.continue_for(0.15)
        if gdb.read_u32(tick_addr) - t0 >= hold_ms:
            break
    t1 = gdb.read_u32(tick_addr)
    qmp.set_key(KEY_NONE)
    for _ in range(15):
        gdb.continue_for(0.15)
        if gdb.read_u32(tick_addr) - t1 >= settle_ms:
            break


def main() -> int:
    ap = argparse.ArgumentParser(description="Capture real open_fw QEMU UI tour screenshots")
    ap.add_argument("--qemu", default=run_qemu.DEFAULT_QEMU)
    ap.add_argument("--stub", type=Path, default=run_qemu.BUILD_DIR / "qemu_boot_stub.bin")
    ap.add_argument("--elf", type=Path, default=run_qemu.BUILD_DIR / "open_fw.elf")
    ap.add_argument("--flash", type=Path, default=REPO_ROOT / "dumps" / "firmware_2M_openfw.bin")
    ap.add_argument("--rom", type=Path, default=REPO_ROOT / "dumps" / "bootrom_384K.bin")
    ap.add_argument("--out-dir", type=Path, default=ASSETS_DIR)
    ap.add_argument("--scale", type=int, default=3)
    args = ap.parse_args()

    if not args.stub.exists():
        subprocess.run(["make", "-C", str(REPO_ROOT / "open_fw"), "qemu-stub"], check=True)

    syms = run_qemu.read_elf32_symbols(args.elf)
    tick_addr = syms["g_systick_ms"][0]
    scr_addr = syms["g_ui_screen"][0]
    usb_addr = syms["g_usb_active"][0]

    args.out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="openfw_tour_") as td:
        td_path = Path(td)
        padded_rom = td_path / "rom_368k.bin"
        rom_bytes = args.rom.read_bytes()
        padded_rom.write_bytes(rom_bytes[: run_qemu.SL6801_ROM_SIZE].ljust(run_qemu.SL6801_ROM_SIZE, b"\xff"))

        sd_img = td_path / "sd.img"
        build_sample_exfat_sd(sd_img)

        # Pass 1: VBUS OFF -> Home, Files, Settings, About, Bluetooth Sync
        qmp_sock1 = str(td_path / "qmp1.sock")
        gdb_port1 = run_qemu.free_tcp_port()
        proc1 = start_qemu(args.qemu, padded_rom, args.stub, args.flash, sd_img, True, qmp_sock1, gdb_port1)
        try:
            qmp = QmpClient(qmp_sock1)
            gdb = run_qemu.GdbRspClient("127.0.0.1", gdb_port1)
            print("Pass 1 (VBUS OFF): waiting for boot animation to finish...")
            ms = wait_for_boot(gdb, tick_addr, 2600)
            print(f"  Booted: g_systick_ms={ms}, g_ui_screen={gdb.read_u32(scr_addr)}")

            # 1. Home
            capture_screen(gdb, args.out_dir / "openfw_qemu_home.png", args.scale)

            # Reset Home focus to tile 0 (Rec), then step cleanly to each screen:
            for _ in range(3):
                tap_key(qmp, gdb, tick_addr, KEY_LEFT, hold_ms=50, settle_ms=70)

            # 2. Files (tile 1)
            tap_key(qmp, gdb, tick_addr, KEY_RIGHT)
            tap_key(qmp, gdb, tick_addr, KEY_ENTER, settle_ms=200)
            print(f"  Files: g_ui_screen={gdb.read_u32(scr_addr)}")
            capture_screen(gdb, args.out_dir / "openfw_qemu_files.png", args.scale)

            # 3. Settings (BACK to Home -> RIGHT to tile 2 -> ENTER)
            tap_key(qmp, gdb, tick_addr, KEY_BACK)
            tap_key(qmp, gdb, tick_addr, KEY_RIGHT)
            tap_key(qmp, gdb, tick_addr, KEY_ENTER, settle_ms=200)
            print(f"  Settings: g_ui_screen={gdb.read_u32(scr_addr)}")
            capture_screen(gdb, args.out_dir / "openfw_qemu_settings.png", args.scale)

            # 4. About (9x RIGHT to row 10/10 About -> ENTER)
            for _ in range(9):
                tap_key(qmp, gdb, tick_addr, KEY_RIGHT, hold_ms=50, settle_ms=70)
            tap_key(qmp, gdb, tick_addr, KEY_ENTER, settle_ms=200)
            print(f"  About: g_ui_screen={gdb.read_u32(scr_addr)}")
            capture_screen(gdb, args.out_dir / "openfw_qemu_about.png", args.scale)

            # 5. Bluetooth sync (BACK -> Settings, BACK -> Home, RIGHT -> tile 3 -> ENTER)
            tap_key(qmp, gdb, tick_addr, KEY_BACK)
            tap_key(qmp, gdb, tick_addr, KEY_BACK)
            tap_key(qmp, gdb, tick_addr, KEY_RIGHT)
            tap_key(qmp, gdb, tick_addr, KEY_ENTER, settle_ms=300)
            print(f"  Bluetooth: g_ui_screen={gdb.read_u32(scr_addr)}")
            capture_screen(gdb, args.out_dir / "openfw_qemu_bt.png", args.scale)

            qmp.close()
            gdb.close()
        finally:
            proc1.terminate()
            try:
                proc1.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc1.kill()

        # Pass 2: VBUS ON + ENTER -> USB Disk screen
        qmp_sock2 = str(td_path / "qmp2.sock")
        gdb_port2 = run_qemu.free_tcp_port()
        proc2 = start_qemu(args.qemu, padded_rom, args.stub, args.flash, sd_img, False, qmp_sock2, gdb_port2)
        try:
            qmp2 = QmpClient(qmp_sock2)
            gdb2 = run_qemu.GdbRspClient("127.0.0.1", gdb_port2)
            print("Pass 2 (VBUS ON): waiting for boot, then entering USB Disk...")
            ms2 = wait_for_boot(gdb2, tick_addr, 2600)
            if gdb2.read_u32(usb_addr) == 0:
                tap_key(qmp2, gdb2, tick_addr, KEY_ENTER, settle_ms=250)
            print(f"  USB: g_systick_ms={ms2}, g_usb_active={gdb2.read_u32(usb_addr)}, g_ui_screen={gdb2.read_u32(scr_addr)}")
            capture_screen(gdb2, args.out_dir / "openfw_qemu_usb.png", args.scale)
            qmp2.close()
            gdb2.close()
        finally:
            proc2.terminate()
            try:
                proc2.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc2.kill()

    print("Done! All 6 real QEMU screenshots saved to", args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
