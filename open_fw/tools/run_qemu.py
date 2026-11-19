#!/usr/bin/env python3
"""
Headless QEMU verification runner for Cassini OpenFW (SL6801).

Boots open_fw inside anton-veretenenko/smartlink-sl6801-emulator, lets Cortex-M4F
run past startup + LCD + SDIO/exFAT init into the main loop, connects over a
pure-Python GDB RSP client (no arm-none-eabi-gdb required), and verifies:
  1. CPU PC is executing inside open_fw (not stuck in Default_Handler/fault).
  2. g_systick_ms > 0 (SysTick 1 kHz IRQ firing on NVIC).
  3. g_openfw_log SRAM ring contains valid 'OFLG' magic & Cassini OpenFW banner.
  4. g_sd_card / g_sd_mounted status over SL6801 SDIO (0x40003000) + exFAT.
  5. ST7735S RGB565 framebuffer (0x50000000, x=26..105) is non-blank & saved to PNG.

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path


DEFAULT_QEMU = "/tmp/sl6801-emu/work/build/qemu-system-arm"
BUILD_DIR = Path("/tmp/sl6801_openfw")
SL6801_ROM_SIZE = 0x5C000
LCD_STRIDE = 128
LCD_X_OFF = 26
LCD_W = 80
LCD_H = 160
FB_ADDR = 0x50000000
LOG_MAGIC = 0x474C464F  # 'OFLG' LE


def read_elf32_symbols(elf_path: Path) -> dict[str, tuple[int, int]]:
    """Parse ELF32 symbol table (.symtab/.strtab) without external binaries."""
    data = elf_path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise RuntimeError(f"{elf_path} is not a 32-bit ELF file")
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, _, _, sh_offset, sh_size, sh_link, _, _, sh_entsize = (
            struct.unpack_from("<10I", data, off)
        )
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    syms: dict[str, tuple[int, int]] = {}
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type != 2 or sh_entsize == 0:  # SHT_SYMTAB
            continue
        _, str_off, str_size, _, _ = sections[sh_link]
        strtab = data[str_off : str_off + str_size]
        for pos in range(0, sh_size, sh_entsize):
            st_name, st_value, st_size, st_info, _, _ = struct.unpack_from(
                "<IIIBBH", data, sh_offset + pos
            )
            if st_name == 0:
                continue
            end = strtab.find(b"\x00", st_name)
            name = strtab[st_name : end if end >= 0 else None].decode("ascii", "replace")
            syms[name] = (st_value & ~1 if (st_info & 0xF) == 2 else st_value, st_size)
    return syms


def write_png_rgb(path: Path, width: int, height: int, rgb_bytes: bytes) -> None:
    """Write an uncompressed/zlib 24-bit RGB PNG using stdlib only."""
    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    raw_rows = bytearray()
    row_stride = width * 3
    for y in range(height):
        raw_rows.append(0)  # Filter type 0 (None)
        raw_rows.extend(rgb_bytes[y * row_stride : (y + 1) * row_stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw_rows), 6))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


class GdbRspClient:
    """Minimal pure-Python GDB Remote Serial Protocol client for QEMU ARM."""

    def __init__(self, host: str, port: int, timeout: float = 10.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = bytearray()

    def close(self) -> None:
        self.sock.close()

    def _recv_byte(self) -> int:
        if not self._buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("GDB RSP socket closed by QEMU")
            self._buf.extend(chunk)
        b = self._buf[0]
        del self._buf[0]
        return b

    def send_packet(self, payload: str) -> str:
        raw = payload.encode("ascii")
        csum = sum(raw) & 0xFF
        pkt = f"${payload}#{csum:02x}".encode("ascii")
        self.sock.sendall(pkt)
        return self.read_reply()

    def send_interrupt(self) -> str:
        self.sock.sendall(b"\x03")
        return self.read_reply()

    def read_reply(self) -> str:
        while True:
            b = self._recv_byte()
            if b in (ord("+"), ord("-")):
                continue
            if b == ord("$"):
                body = bytearray()
                while True:
                    c = self._recv_byte()
                    if c == ord("#"):
                        _ = self._recv_byte()
                        _ = self._recv_byte()
                        self.sock.sendall(b"+")
                        msg = body.decode("ascii", "replace")
                        if msg.startswith("O"):
                            continue
                        return msg
                    body.append(c)

    def read_regs(self) -> list[int]:
        rep = self.send_packet("g")
        if rep.startswith("E") or len(rep) < 16 * 8:
            raise RuntimeError(f"Failed reading ARM registers via RSP: {rep!r}")
        regs = []
        for i in range(min(17, len(rep) // 8)):
            word = bytes.fromhex(rep[i * 8 : (i + 1) * 8])
            regs.append(struct.unpack("<I", word)[0])
        return regs

    def read_mem(self, addr: int, size: int) -> bytes:
        out = bytearray()
        cur = 0
        while cur < size:
            chunk_len = min(256, size - cur)
            rep = self.send_packet(f"m{addr + cur:x},{chunk_len:x}")
            if rep.startswith("E") or not rep:
                raise RuntimeError(f"RSP read_mem failed at 0x{addr + cur:08x}: {rep!r}")
            out.extend(bytes.fromhex(rep))
            cur += chunk_len
        return bytes(out)

    def continue_for(self, seconds: float) -> str:
        csum = sum(b"c") & 0xFF
        self.sock.sendall(f"$c#{csum:02x}".encode("ascii"))
        # Consume initial '+' ack if it arrives promptly, then run for `seconds`.
        time.sleep(seconds)
        return self.send_interrupt()


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def prepare_sd_image(img_path: Path, size_mb: int = 8) -> None:
    mkfs = shutil.which("mkfs.exfat") or "/usr/sbin/mkfs.exfat"
    if not Path(mkfs).exists():
        raise FileNotFoundError(f"mkfs.exfat not found at {mkfs}")
    with img_path.open("wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    subprocess.run([mkfs, "-q", "-b", "4096", "-c", "4096", str(img_path)], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Boot & verify Cassini OpenFW in QEMU SL6801")
    ap.add_argument("--qemu", default=os.environ.get("QEMU_SL6801", DEFAULT_QEMU))
    ap.add_argument("--elf", type=Path, default=BUILD_DIR / "open_fw.elf")
    ap.add_argument("--stub", type=Path, default=BUILD_DIR / "qemu_boot_stub.bin")
    ap.add_argument("--rom", type=Path, default=Path("../dumps/bootrom_384K.bin"))
    ap.add_argument("--flash", type=Path, default=Path("../dumps/firmware_2M_openfw.bin"))
    ap.add_argument("--boot-seconds", type=float, default=2.0)
    ap.add_argument("--png-out", type=Path, default=BUILD_DIR / "qemu_lcd.png")
    args = ap.parse_args()

    qemu_bin = Path(args.qemu)
    if not qemu_bin.is_file():
        print(f"ERROR: QEMU binary not found at {qemu_bin}", file=sys.stderr)
        return 1
    for req in (args.elf, args.stub, args.rom, args.flash):
        if not req.is_file():
            print(f"ERROR: Required input file missing: {req}", file=sys.stderr)
            return 1

    syms = read_elf32_symbols(args.elf)
    for req_sym in ("g_systick_ms", "g_openfw_log", "g_sd_card", "g_sd_mounted", "Default_Handler"):
        if req_sym not in syms:
            print(f"ERROR: Symbol {req_sym} missing from {args.elf}", file=sys.stderr)
            return 1

    # QEMU's smartlink_sl6801.c limits mask ROM load_image_targphys() to 0x5C000 bytes.
    qemu_rom = BUILD_DIR / "qemu_rom_368k.bin"
    qemu_rom.write_bytes(args.rom.read_bytes()[:SL6801_ROM_SIZE])

    sd_img = BUILD_DIR / "qemu_sd.img"
    prepare_sd_image(sd_img, size_mb=8)

    port = free_tcp_port()
    machine = (
        f"smartlink-sl6801,"
        f"rom={qemu_rom},"
        f"rom-stub={args.stub},"
        f"flash={args.flash},"
        f"sd-image={sd_img},"
        f"viewtask-local=off,"
        f"high-rom-hle=off,"
        f"boot-decision=6,"
        f"kadc-raw-ch1=4095,"
        f"pmu-battery-code=90"
    )
    cmd = [
        str(qemu_bin),
        "-M", machine,
        "-display", "none",
        "-serial", "none",
        "-gdb", f"tcp:127.0.0.1:{port}",
        "-S",
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        # Wait briefly for GDB RSP server socket to start listening
        client = None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                err = proc.stderr.read().decode("utf-8", "replace")
                raise RuntimeError(f"QEMU exited early (rc={proc.returncode}):\n{err}")
            try:
                client = GdbRspClient("127.0.0.1", port, timeout=10.0)
                break
            except OSError:
                time.sleep(0.05)
        if client is None:
            raise TimeoutError("Timed out connecting to QEMU GDB stub")

        try:
            _ = client.continue_for(args.boot_seconds)
            regs = client.read_regs()
            sp, lr, pc = regs[13], regs[14], regs[15]
            xpsr = regs[16] if len(regs) > 16 else 0

            default_handler = syms["Default_Handler"][0]
            in_fault = abs(pc - default_handler) <= 8

            systick_addr = syms["g_systick_ms"][0]
            systick_ms = struct.unpack("<I", client.read_mem(systick_addr, 4))[0]

            log_addr = syms["g_openfw_log"][0]
            log_hdr = client.read_mem(log_addr, 16)
            log_magic, log_size, log_widx, log_drop = struct.unpack("<4I", log_hdr)
            log_text = ""
            if log_magic == LOG_MAGIC and log_size > 0:
                nbytes = min(log_widx, log_size)
                raw_buf = client.read_mem(log_addr + 16, nbytes)
                log_text = raw_buf.decode("ascii", "replace")

            sd_card_raw = client.read_mem(syms["g_sd_card"][0], 16)
            sd_init_ok, sd_sdhc, sd_rca, sd_sectors = struct.unpack("<iiII", sd_card_raw)
            sd_rca &= 0xFFFF
            sd_mounted = struct.unpack("<i", client.read_mem(syms["g_sd_mounted"][0], 4))[0]

            fb_raw = client.read_mem(FB_ADDR, LCD_STRIDE * LCD_H * 2)
        except (OSError, EOFError, RuntimeError) as exc:
            err = ""
            if proc.poll() is not None and proc.stderr:
                err = proc.stderr.read().decode("utf-8", "replace")
            else:
                proc.terminate()
                try:
                    _, eb = proc.communicate(timeout=2.0)
                    err = eb.decode("utf-8", "replace")
                except Exception:
                    pass
            print(f"ERROR: GDB RSP communication failed: {exc}\nQEMU stderr:\n{err}", file=sys.stderr)
            return 1
        finally:
            client.close()

        proc.terminate()
        try:
            _, stderr_bytes = proc.communicate(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, stderr_bytes = proc.communicate(timeout=3.0)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    uart_stderr = stderr_bytes.decode("utf-8", "replace")

    # Convert active 80x160 pane (x=26..105, y=0..159) to RGB24 PNG
    rgb = bytearray(LCD_W * LCD_H * 3)
    unique_colors: set[int] = set()
    nonblack = 0
    for y in range(LCD_H):
        for x in range(LCD_W):
            px, = struct.unpack_from("<H", fb_raw, (y * LCD_STRIDE + (LCD_X_OFF + x)) * 2)
            unique_colors.add(px)
            if px != 0:
                nonblack += 1
            r = ((px >> 11) & 0x1F) << 3
            g = ((px >> 5) & 0x3F) << 2
            b = (px & 0x1F) << 3
            idx = (y * LCD_W + x) * 3
            rgb[idx : idx + 3] = bytes((r, g, b))
    write_png_rgb(args.png_out, LCD_W, LCD_H, bytes(rgb))

    print(f"CPU state: PC=0x{pc:08x} SP=0x{sp:08x} LR=0x{lr:08x} xPSR=0x{xpsr:08x}")
    print(f"SysTick:   g_systick_ms={systick_ms}")
    print(
        f"Log ring:  magic=0x{log_magic:08x} size={log_size} "
        f"write_idx={log_widx} dropped={log_drop}"
    )
    if log_text:
        for line in log_text.strip().splitlines():
            print(f"  [SRAM] {line}")
    if "Cassini OpenFW" in uart_stderr:
        for line in uart_stderr.strip().splitlines():
            if "Cassini OpenFW" in line or "CPU=" in line or "SD:" in line or "Switching PD0" in line:
                print(f"  [UART0] {line}")
    print(
        f"SDIO/FS:   sd_init={sd_init_ok} sdhc={sd_sdhc} rca=0x{sd_rca:04x} "
        f"sectors={sd_sectors} mounted={sd_mounted}"
    )
    print(
        f"LCD FB:    {LCD_W}x{LCD_H} @ 0x{FB_ADDR:08x}: nonblack={nonblack}/{LCD_W * LCD_H} "
        f"unique_colors={len(unique_colors)} -> {args.png_out}"
    )

    failures = []
    if in_fault:
        failures.append(f"CPU trapped near Default_Handler (PC=0x{pc:08x})")
    if pc < 0x00804C00:
        failures.append(f"PC=0x{pc:08x} did not reach SRAM/XIP open_fw space")
    if systick_ms == 0:
        failures.append("g_systick_ms remained 0")
    if log_magic != LOG_MAGIC or "Cassini OpenFW" not in log_text:
        failures.append("g_openfw_log did not record Cassini OpenFW boot banner")
    if "Switching PD0" not in log_text:
        failures.append("open_fw did not reach main loop banner")
    if sd_init_ok != 1 or sd_mounted != 1:
        failures.append(f"SD/exFAT mount failed in QEMU (sd_init={sd_init_ok}, mounted={sd_mounted})")
    if nonblack < 400 or len(unique_colors) < 6:
        failures.append(
            f"LCD framebuffer appears blank or corrupted (nonblack={nonblack}, colors={len(unique_colors)})"
        )

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        return 1

    print("QEMU SL6801 BOOT VERIFICATION PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
