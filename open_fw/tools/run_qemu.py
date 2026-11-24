#!/usr/bin/env python3
"""
Headless QEMU verification runner for Cassini OpenFW (SL6801).

Boots open_fw inside anton-veretenenko/smartlink-sl6801-emulator, lets Cortex-M4F
run past startup + LCD + SDIO/exFAT init into the main loop, connects over a
pure-Python GDB RSP client (no arm-none-eabi-gdb required), and verifies:
  1. CPU PC is executing inside open_fw (not stuck in Default_Handler/fault).
  2. g_systick_ms > 0 and continues advancing across a 0.4s resume window.
  3. g_openfw_log SRAM ring contains valid 'OFLG' magic & Cassini OpenFW banner.
  4. g_sd_card / g_sd_mounted status over SL6801 SDIO (0x40003000) + exFAT.
  5. ST7735S RGB565 framebuffer (0x50000000, x=26..105) is non-blank & saved to PNG.
  6. Unless --skip-keys is set, boots a second short QEMU pass with kadc-raw-ch1=2850
     (KEY_ENTER) + sd-image and verifies transition into USB MSC mode (g_usb_active == 1).

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
import tempfile
import time
import zlib
from pathlib import Path
from typing import Callable, TypeVar


DEFAULT_QEMU = "/tmp/sl6801-emu/work/build/qemu-system-arm"
REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = Path("/tmp/sl6801_openfw")
SL6801_ROM_SIZE = 0x5C000
LCD_STRIDE = 128
LCD_X_OFF = 26
LCD_W = 80
LCD_H = 160
FB_ADDR = 0x50000000
LOG_MAGIC = 0x474C464F  # 'OFLG' LE

T = TypeVar("T")


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
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def decode_fb_to_rgb24(
    fb_raw: bytes, landscape: bool = True, scale: int = 1
) -> tuple[int, int, bytes, int, int]:
    """Decode active ST7735S GRAM pane (px=26..105, py=0..159) to scaled RGB24.

    When MADCTL MV=1 (landscape 160x80) is active in QEMU, pixels are stored in
    portrait GRAM at px = dcs_cy (26..105), py = 159 - (dcs_cx - 1) (0..159),
    so landscape (lx, ly) maps back from (px = 26 + ly, py = 159 - lx).
    """
    if scale < 1:
        raise ValueError(f"scale must be >= 1, got {scale}")

    base_w, base_h = (LCD_H, LCD_W) if landscape else (LCD_W, LCD_H)
    unique_colors: set[int] = set()
    nonblack = 0
    out_w = base_w * scale
    out_h = base_h * scale
    rgb = bytearray()

    for y in range(base_h):
        row = bytearray()
        for x in range(base_w):
            if landscape:
                px_x = LCD_X_OFF + y
                px_y = (LCD_H - 1) - x
            else:
                px_x = LCD_X_OFF + x
                px_y = y
            px, = struct.unpack_from("<H", fb_raw, (px_y * LCD_STRIDE + px_x) * 2)
            unique_colors.add(px)
            if px != 0:
                nonblack += 1
            r5 = (px >> 11) & 0x1F
            g6 = (px >> 5) & 0x3F
            b5 = px & 0x1F
            pix = bytes((
                (r5 << 3) | (r5 >> 2),
                (g6 << 2) | (g6 >> 4),
                (b5 << 3) | (b5 >> 2),
            ))
            if scale == 1:
                row.extend(pix)
            else:
                row.extend(pix * scale)
        for _ in range(scale):
            rgb.extend(row)

    return out_w, out_h, bytes(rgb), nonblack, len(unique_colors)


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

    def read_u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read_mem(addr, 4))[0]

    def read_i32(self, addr: int) -> int:
        return struct.unpack("<i", self.read_mem(addr, 4))[0]

    def continue_for(self, seconds: float) -> str:
        csum = sum(b"c") & 0xFF
        self.sock.sendall(f"$c#{csum:02x}".encode("ascii"))
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


def build_machine_arg(
    qemu_rom: Path, stub: Path, flash: Path, sd_img: Path, kadc_ch1: int, vbus_off: bool = False
) -> str:
    arg = (
        f"smartlink-sl6801,"
        f"rom={qemu_rom},"
        f"rom-stub={stub},"
        f"flash={flash},"
        f"sd-image={sd_img},"
        f"viewtask-local=off,"
        f"high-rom-hle=off,"
        f"boot-decision=6,"
        f"kadc-raw-ch1={kadc_ch1},"
        f"pmu-battery-code=90"
    )
    if vbus_off:
        arg += ",pmu-vbus=off"
    return arg


def run_qemu_session(
    qemu_bin: Path, machine: str, fn: Callable[[GdbRspClient], T]
) -> tuple[T, str]:
    """Launch QEMU headless with GDB stub, run `fn(client)`, and return `(result, stderr)`."""
    port = free_tcp_port()
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
        client: GdbRspClient | None = None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
                raise RuntimeError(f"QEMU exited early (rc={proc.returncode}):\n{err}")
            try:
                client = GdbRspClient("127.0.0.1", port, timeout=10.0)
                break
            except OSError:
                time.sleep(0.05)
        if client is None:
            raise TimeoutError("Timed out connecting to QEMU GDB stub")

        try:
            result = fn(client)
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
            raise RuntimeError(f"GDB RSP communication failed: {exc}\nQEMU stderr:\n{err}") from exc
        finally:
            client.close()

        proc.terminate()
        try:
            _, stderr_bytes = proc.communicate(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, stderr_bytes = proc.communicate(timeout=3.0)
        return result, stderr_bytes.decode("utf-8", "replace")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def is_in_fault(pc: int, default_handler: int) -> bool:
    return abs(pc - default_handler) <= 8


def main() -> int:
    ap = argparse.ArgumentParser(description="Boot & verify Cassini OpenFW in QEMU SL6801")
    ap.add_argument("--qemu", default=os.environ.get("QEMU_SL6801", DEFAULT_QEMU))
    ap.add_argument("--elf", type=Path, default=BUILD_DIR / "open_fw.elf")
    ap.add_argument("--stub", type=Path, default=BUILD_DIR / "qemu_boot_stub.bin")
    ap.add_argument("--rom", type=Path, default=REPO_ROOT / "dumps/bootrom_384K.bin")
    ap.add_argument("--flash", type=Path, default=REPO_ROOT / "dumps/firmware_2M_openfw.bin")
    ap.add_argument("--boot-seconds", type=float, default=2.0)
    ap.add_argument("--keys-seconds", type=float, default=1.2)
    ap.add_argument("--skip-keys", action="store_true", help="Skip KEY_ENTER -> USB MSC check")
    ap.add_argument("--vbus-off", action="store_true", help="Boot unplugged on battery (pmu-vbus=off)")
    ap.add_argument("--png-out", type=Path, default=BUILD_DIR / "qemu_lcd.png")
    orient = ap.add_mutually_exclusive_group()
    orient.add_argument(
        "--landscape",
        dest="landscape",
        action="store_true",
        default=True,
        help="Capture 160x80 landscape image from MADCTL MV=1 GRAM mapping (default)",
    )
    orient.add_argument(
        "--portrait",
        dest="landscape",
        action="store_false",
        help="Capture raw 80x160 portrait GRAM pane without landscape un-rotation",
    )
    ap.add_argument(
        "--scale",
        type=int,
        default=3,
        help="Integer nearest-neighbor PNG scale factor (default: 3 -> 480x240 landscape)",
    )
    args = ap.parse_args()

    if args.scale < 1:
        print(f"ERROR: --scale must be >= 1 (got {args.scale})", file=sys.stderr)
        return 1

    qemu_bin = Path(args.qemu)
    if not qemu_bin.is_file():
        print(f"ERROR: QEMU binary not found at {qemu_bin}", file=sys.stderr)
        return 1
    for req in (args.elf, args.stub, args.rom, args.flash):
        if not req.is_file():
            print(f"ERROR: Required input file missing: {req}", file=sys.stderr)
            return 1

    syms = read_elf32_symbols(args.elf)
    req_syms = ["g_systick_ms", "g_openfw_log", "g_sd_card", "g_sd_mounted", "Default_Handler"]
    if not args.skip_keys:
        req_syms.append("g_usb_active")
    for req_sym in req_syms:
        if req_sym not in syms:
            print(f"ERROR: Symbol {req_sym} missing from {args.elf}", file=sys.stderr)
            return 1

    default_handler = syms["Default_Handler"][0]
    systick_addr = syms["g_systick_ms"][0]
    log_addr = syms["g_openfw_log"][0]

    with tempfile.TemporaryDirectory(prefix="sl6801_qemu_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        # QEMU's smartlink_sl6801.c limits mask ROM load_image_targphys() to 0x5C000 bytes.
        qemu_rom = tmp_path / "qemu_rom_368k.bin"
        qemu_rom.write_bytes(args.rom.read_bytes()[:SL6801_ROM_SIZE])

        sd_img = tmp_path / "qemu_sd.img"
        prepare_sd_image(sd_img, size_mb=8)

        selftest_result: dict = {}
        selftest_names = ["delay", "queue", "mutex_pi", "timer", "notify", "eventgroup",
                          "heap", "preempt", "from_isr", "stack", "reap", "btdata", "mallocfail"]

        def read_selftest(client: GdbRspClient):
            """FreeRTOS self-test (make RTOS=1 RTOS_SELFTEST=1): poll g_rtos_selftest until done."""
            addr = syms["g_rtos_selftest"][0]
            for _ in range(40):
                nwords = 6 + len(selftest_names)
                raw = client.read_mem(addr, nwords * 4)
                words = struct.unpack(f"<{nwords}I", raw)
                if words[1] == 1:
                    break
                _ = client.continue_for(0.5)
            aon = struct.unpack("<3I", client.read_mem(syms["g_rtos_fatal"][0] if "g_rtos_fatal" in syms else 0x40085180, 12))
            st_regs = client.read_regs()
            if words[1] != 1:
                print(f"RTOS selftest stalled: pc=0x{st_regs[15]:08x} lr=0x{st_regs[14]:08x} "
                      f"fatal[0..2]=0x{aon[0]:08x} 0x{aon[1]:08x} 0x{aon[2]:08x}")
                sp = st_regs[13]
                stk = struct.unpack("<96I", client.read_mem(sp, 96 * 4))
                code = [f"0x{w:08x}" for w in stk if 0x00800000 <= (w & ~1) < 0x00880000 and (w & 1)]
                print(f"  sp=0x{sp:08x} stack: {' '.join(f'{w:08x}' for w in stk[:48])}")
            selftest_result.update(
                magic=words[0], done=words[1], pass_mask=words[2], fail_mask=words[3],
                heap_free_start=words[4], heap_min_end=words[5], detail=list(words[6:nwords]),
            )

        def inspect_boot(client: GdbRspClient):
            _ = client.continue_for(args.boot_seconds)
            if "g_rtos_selftest" in syms:
                read_selftest(client)
            systick_1 = client.read_u32(systick_addr)
            _ = client.continue_for(0.4)
            systick_2 = client.read_u32(systick_addr)

            regs = client.read_regs()
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
            sd_mounted = client.read_i32(syms["g_sd_mounted"][0])
            fb_raw = client.read_mem(FB_ADDR, LCD_STRIDE * LCD_H * 2)
            return (
                regs, systick_1, systick_2,
                log_magic, log_size, log_widx, log_drop, log_text,
                sd_init_ok, sd_sdhc, sd_rca, sd_sectors, sd_mounted, fb_raw,
            )

        try:
            boot_machine = build_machine_arg(
                qemu_rom, args.stub, args.flash, sd_img, kadc_ch1=4095, vbus_off=args.vbus_off
            )
            boot_res, uart_stderr = run_qemu_session(qemu_bin, boot_machine, inspect_boot)
        except (RuntimeError, TimeoutError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1

        (
            regs, systick_1, systick_2,
            log_magic, log_size, log_widx, log_drop, log_text,
            sd_init_ok, sd_sdhc, sd_rca, sd_sectors, sd_mounted, fb_raw,
        ) = boot_res
        sp, lr, pc = regs[13], regs[14], regs[15]
        xpsr = regs[16] if len(regs) > 16 else 0

        # Convert active GRAM pane (px=26..105, py=0..159) to landscape (160x80) or portrait (80x160) PNG
        out_w, out_h, rgb, nonblack, n_colors = decode_fb_to_rgb24(
            fb_raw, landscape=args.landscape, scale=args.scale
        )
        write_png_rgb(args.png_out, out_w, out_h, rgb)

        print(f"CPU state: PC=0x{pc:08x} SP=0x{sp:08x} LR=0x{lr:08x} xPSR=0x{xpsr:08x}")
        print(
            f"SysTick:   g_systick_ms={systick_1} -> {systick_2} "
            f"(+{systick_2 - systick_1} ms over 0.4s)"
        )
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
        mode_str = "landscape 160x80" if args.landscape else "portrait 80x160"
        print(
            f"LCD FB:    {mode_str} ({out_w}x{out_h} @{args.scale}x) @ 0x{FB_ADDR:08x}: "
            f"nonblack={nonblack}/{LCD_W * LCD_H} unique_colors={n_colors} -> {args.png_out}"
        )

        failures = []
        if is_in_fault(pc, default_handler):
            failures.append(f"CPU trapped near Default_Handler (PC=0x{pc:08x})")
        if pc < 0x00804C00:
            failures.append(f"PC=0x{pc:08x} did not reach SRAM/XIP open_fw space")
        if systick_1 == 0:
            failures.append("g_systick_ms remained 0")
        if systick_2 <= systick_1:
            failures.append(
                f"g_systick_ms did not advance across 0.4s resume ({systick_1} -> {systick_2})"
            )
        if log_magic != LOG_MAGIC or "Cassini OpenFW" not in log_text:
            failures.append("g_openfw_log did not record Cassini OpenFW boot banner")
        if "Switching PD0" not in log_text:
            failures.append("open_fw did not reach main loop banner")
        if sd_init_ok != 1 or sd_mounted != 1:
            failures.append(f"SD/exFAT mount failed in QEMU (sd_init={sd_init_ok}, mounted={sd_mounted})")
        if nonblack < 400 or n_colors < 6:
            failures.append(
                f"LCD framebuffer appears blank or corrupted (nonblack={nonblack}, colors={n_colors})"
            )

        if selftest_result:
            n = len(selftest_names)
            print(
                f"RTOS selftest: done={selftest_result['done']} pass=0x{selftest_result['pass_mask']:03x} "
                f"fail=0x{selftest_result['fail_mask']:03x} heap_free_start={selftest_result['heap_free_start']} "
                f"heap_min_end={selftest_result['heap_min_end']}"
            )
            for i, name in enumerate(selftest_names):
                ok = (selftest_result["pass_mask"] >> i) & 1
                bad = (selftest_result["fail_mask"] >> i) & 1
                print(f"  [{'PASS' if ok and not bad else 'FAIL'}] {name} (detail={selftest_result['detail'][i]})")
            if selftest_result["magic"] != 0x52544F53 or selftest_result["done"] != 1:
                failures.append("RTOS selftest did not complete")
            if selftest_result["pass_mask"] != (1 << n) - 1 or selftest_result["fail_mask"] != 0:
                failures.append("RTOS selftest reported failures")

        if not args.skip_keys:
            usb_active_addr = syms["g_usb_active"][0]
            ui_screen_addr = syms["g_ui_screen"][0]

            def inspect_key_enter(client: GdbRspClient):
                _ = client.continue_for(args.keys_seconds)
                for _ in range(6):
                    k_regs = client.read_regs()
                    k_pc = k_regs[15]
                    k_systick = client.read_u32(systick_addr)
                    usb_active = client.read_i32(usb_active_addr)
                    ui_screen = client.read_u32(ui_screen_addr)
                    if is_in_fault(k_pc, default_handler) or usb_active == 1 or ui_screen == 3:
                        return k_pc, k_systick, usb_active, ui_screen
                    _ = client.continue_for(0.4)
                k_regs = client.read_regs()
                return (
                    k_regs[15],
                    client.read_u32(systick_addr),
                    client.read_i32(usb_active_addr),
                    client.read_u32(ui_screen_addr),
                )

            try:
                keys_machine = build_machine_arg(qemu_rom, args.stub, args.flash, sd_img, kadc_ch1=2850)
                (k_pc, k_systick, usb_active, ui_screen), _ = run_qemu_session(
                    qemu_bin, keys_machine, inspect_key_enter
                )
            except (RuntimeError, TimeoutError) as exc:
                failures.append(f"KEY_ENTER QEMU check failed: {exc}")
            else:
                print(
                    f"Key check: PC=0x{k_pc:08x} g_systick_ms={k_systick} "
                    f"g_ui_screen={ui_screen} g_usb_active={usb_active}"
                )
                if is_in_fault(k_pc, default_handler):
                    failures.append(f"KEY_ENTER check trapped near Default_Handler (PC=0x{k_pc:08x})")
                if k_pc < 0x00804C00:
                    failures.append(f"KEY_ENTER check PC=0x{k_pc:08x} did not reach open_fw space")
                if k_systick == 0:
                    failures.append("KEY_ENTER check g_systick_ms remained 0")
                if ui_screen != 3 and usb_active != 1:
                    failures.append(
                        f"KEY_ENTER did not open Files or USB MSC (g_ui_screen={ui_screen}, g_usb_active={usb_active})"
                    )
                elif ui_screen == 3:
                    print("[KEY_ENTER -> UI_SCR_FILES ACTIVE]")
                else:
                    print("[KEY_ENTER -> USB MSC ACTIVE]")

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        return 1

    print("QEMU SL6801 BOOT VERIFICATION PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
