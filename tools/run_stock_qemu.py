#!/usr/bin/env python3
"""
Boot the stock HW_76 (pen133_2.0.65) firmware in the SL6801 QEMU emulator
with ported ViewTask local-task patches and capture a real LCD screenshot.

Ported from anton-veretenenko/smartlink-sl6801-emulator's yp3_2.0.43 ViewTask
patch set to pen133_2.0.65 addresses:
  1. Static XIP patches + 20-byte hybrid code cave (at unused XIP 0x00DD0000)
     are applied to a temporary copy of dumps/firmware_2M.bin (never modifies
     the original dump).
  2. Runtime SRAM mailbox + ViewTask stack-descriptor patches are applied over
     GDB RSP when the CPU hits the ViewTask creation call at 0x00CE9BA8.

Usage:
    python3 tools/run_stock_qemu.py --out docs/assets/stock_firmware_screen.png

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "open_fw" / "tools"))
import run_qemu  # noqa: E402


def encode_thumb2_b_w(pc: int, target: int) -> tuple[int, int]:
    """Encode Thumb-2 unconditional 32-bit branch B.W (T4 encoding)."""
    delta = target - (pc + 4)
    if delta & 1 or not (-(1 << 24) <= delta < (1 << 24)):
        raise ValueError(f"b.w out of range: 0x{pc:08X} -> 0x{target:08X}")
    s = (delta >> 24) & 1
    i1 = (delta >> 23) & 1
    i2 = (delta >> 22) & 1
    imm10 = (delta >> 12) & 0x3FF
    imm11 = (delta >> 1) & 0x7FF
    j1 = (~(i1 ^ s)) & 1
    j2 = (~(i2 ^ s)) & 1
    hw1 = 0xF000 | (s << 10) | imm10
    hw2 = 0x9000 | (j1 << 13) | (j2 << 11) | imm11
    return hw1, hw2


def encode_thumb2_bl(pc: int, target: int) -> tuple[int, int]:
    """Encode Thumb-2 32-bit BL (T1 encoding)."""
    delta = target - (pc + 4)
    if delta & 1 or not (-(1 << 24) <= delta < (1 << 24)):
        raise ValueError(f"bl out of range: 0x{pc:08X} -> 0x{target:08X}")
    s = (delta >> 24) & 1
    i1 = (delta >> 23) & 1
    i2 = (delta >> 22) & 1
    imm10 = (delta >> 12) & 0x3FF
    imm11 = (delta >> 1) & 0x7FF
    j1 = (~(i1 ^ s)) & 1
    j2 = (~(i2 ^ s)) & 1
    hw1 = 0xF000 | (s << 10) | imm10
    hw2 = 0xD000 | (j1 << 13) | (j2 << 11) | imm11
    return hw1, hw2


# pen133_2.0.65 ViewTask local-task addresses (ported from yp3_2.0.43)
XIP_BASE = 0x00C00000
VIEWTASK_BP_PC = 0x00CE9BA8       # bl task_create (0x00D403A4) in ViewTask creator
TASK_CREATE_LOCAL = 0x00D403FE    # local branch after rtos_task_create()
TASK_CREATE_TAIL = 0x00D40402     # common tail (str r5, [r4, #0x50])
TASK_CREATE_REMOTE = 0x00D4042C   # remote mailbox body start
TASK_CREATE_RTAIL = 0x00D40456    # remote body tail
MBOX_NOTIFY_NOP = 0x00CE8F90      # 4 halfwords of context-2 notify
MBOX_NOTIFY_B = 0x00CE8FB0        # b.n 0x00CE8F0C (0xE7AC)
TRANS_SEND_CMP = 0x00D16484       # cmp r3, #1 -> cmp r3, #0
EVENT_SEND_FN = 0x00CE91AE        # rtos_queue_send(*event)
CAVE_ADDR = 0x00DD0000            # unused 0xFF XIP flash (544 KB free)


def build_patched_flash(src_flash: Path, dst_flash: Path) -> None:
    buf = bytearray(src_flash.read_bytes())

    def w16(vma: int, val: int) -> None:
        off = vma - XIP_BASE
        struct.pack_into("<H", buf, off, val)

    b_remote = encode_thumb2_b_w(CAVE_ADDR + 0x08, TASK_CREATE_REMOTE)
    b_tail = encode_thumb2_b_w(CAVE_ADDR + 0x10, TASK_CREATE_TAIL)
    b_cave = encode_thumb2_b_w(TASK_CREATE_LOCAL, CAVE_ADDR)

    patches = [
        # Hybrid code cave in unused XIP flash (0x00DD0000)
        (CAVE_ADDR + 0x00, 0x6D63),  # ldr  r3, [r4, #0x54]
        (CAVE_ADDR + 0x02, 0x2B03),  # cmp  r3, #3
        (CAVE_ADDR + 0x04, 0xD102),  # bne  +8
        (CAVE_ADDR + 0x06, 0x6060),  # str  r0, [r4, #4]
        (CAVE_ADDR + 0x08, b_remote[0]),
        (CAVE_ADDR + 0x0A, b_remote[1]),
        (CAVE_ADDR + 0x0C, 0x6127),  # str  r7, [r4, #16]
        (CAVE_ADDR + 0x0E, 0x6060),  # str  r0, [r4, #4]
        (CAVE_ADDR + 0x10, b_tail[0]),
        (CAVE_ADDR + 0x12, b_tail[1]),
        # task_create local branch -> cave
        (TASK_CREATE_LOCAL + 0x00, b_cave[0]),
        (TASK_CREATE_LOCAL + 0x02, b_cave[1]),
        # remote body tail: entry stays in TCB[+0x0C], handle -> TCB[+8]
        (TASK_CREATE_RTAIL + 0x00, 0x6923),  # ldr r3, [r4, #16]
        (TASK_CREATE_RTAIL + 0x02, 0x6123),  # str r3, [r4, #12]
        (TASK_CREATE_RTAIL + 0x04, 0x60A0),  # str r0, [r4, #8]
        # skip context-2 mailbox notify/ack wait
        (MBOX_NOTIFY_NOP + 0x00, 0xBF00),
        (MBOX_NOTIFY_NOP + 0x02, 0xBF00),
        (MBOX_NOTIFY_NOP + 0x04, 0xBF00),
        (MBOX_NOTIFY_NOP + 0x06, 0xBF00),
        (MBOX_NOTIFY_B, 0xE7AC),             # b.n 0x00CE8F0C
        # trans_send_to_view: deliver for LOCAL (type 0) target too
        (TRANS_SEND_CMP, 0x2B00),            # cmp r3, #0
    ]
    for addr, val in patches:
        w16(addr, val)

    # Note: HLKJ checks FIRM CRC-16 unless skipped; in QEMU rom_stub jumps
    # directly into HLKJ which verifies FIRM CRC. Update partition[0] CRC-16!
    firm_off, firm_sz = struct.unpack_from("<II", buf, 0x00D010)
    crc = 0xFFFF
    for b in buf[firm_off : firm_off + firm_sz]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    struct.pack_into("<H", buf, 0x00D018, crc)
    dst_flash.write_bytes(buf)


def gdb_send_ok(client: run_qemu.GdbRspClient, payload: str) -> str:
    """Send a GDB RSP packet without swallowing 'OK' (run_qemu.read_reply skips 'O*')."""
    raw = payload.encode("ascii")
    csum = sum(raw) & 0xFF
    client.sock.sendall(f"${payload}#{csum:02x}".encode("ascii"))
    while True:
        b = client._recv_byte()
        if b in (ord("+"), ord("-")):
            continue
        if b == ord("$"):
            body = bytearray()
            while True:
                c = client._recv_byte()
                if c == ord("#"):
                    _ = client._recv_byte()
                    _ = client._recv_byte()
                    client.sock.sendall(b"+")
                    msg = body.decode("ascii", "replace")
                    if msg.startswith("O") and msg != "OK":
                        continue
                    return msg
                body.append(c)


def gdb_write_mem(client: run_qemu.GdbRspClient, addr: int, data: bytes) -> None:
    resp = gdb_send_ok(client, f"M{addr:x},{len(data):x}:{data.hex()}")
    if resp != "OK":
        raise RuntimeError(f"GDB write @ 0x{addr:08X} failed: {resp!r}")


def gdb_write16(client: run_qemu.GdbRspClient, addr: int, val: int) -> None:
    gdb_write_mem(client, addr, struct.pack("<H", val))


def apply_runtime_patches(client: run_qemu.GdbRspClient) -> None:
    regs = client.read_regs()
    sp = regs[13]
    entry_le = client.read_mem(sp + 0x10, 4)
    gdb_write_mem(client, sp + 0x00, b"\x00")
    gdb_write_mem(client, sp + 0x0C, entry_le)
    gdb_write_mem(client, sp + 0x20, b"\x00")

    # SRAM mailbox helpers (identical addresses to yp3_2.0.43; only bl target changes)
    bl_enq = encode_thumb2_bl(0x0080D8AA, EVENT_SEND_FN)
    bl_drn = encode_thumb2_bl(0x0080D990, EVENT_SEND_FN)
    ram_patches = [
        (0x0080DA12, 0xBF00),  # drop cpu-id reject
        (0x0080D968, 0xBF00),  # drop consumer-id reject
        (0x0080D840, 0xBF00),  # no notify-ring retry
        (0x0080D864, 0xBF00),  # no notify-ring retry
        (0x0080D928, 0xBF00),  # nop cbz after core-id
        (0x0080D87C, 0x2600),  # movs r6, #0 (force xCoreID=0)
        (0x0080D87E, 0xBF00),  # nop (2nd half of ldrb.w)
        (0x0080D8A8, 0x4608),  # mov r0, r1
        (0x0080D8AA, bl_enq[0]),
        (0x0080D8AC, bl_enq[1]),
        (0x0080D98E, 0x4608),  # mov r0, r1
        (0x0080D990, bl_drn[0]),
        (0x0080D992, bl_drn[1]),
        # LCD batching driver busy-wait loop in pen133_2.0.65:
        # 0x00812790 polls *(uint8_t*)(0x0081CC20 + 0x16) waiting for DMA IRQ
        # (QEMU only auto-clears yp3_2.0.43's SL6801_LCD_DRV_BUSY at 0x0081D242).
        # NOP the bne.n 0x00812790 (0xD1FA) so ViewTask completes LCD uploads.
        (0x00812798, 0xBF00),
    ]
    for addr, val in ram_patches:
        gdb_write16(client, addr, val)
    print(f"Applied ViewTask runtime patches at SP=0x{sp:08X}, entry=0x{struct.unpack('<I', entry_le)[0]:08X}")


# HW_76 SARADC kadc_ch1 resistor-ladder values (see open_fw/src/keys.c)
KADC_IDLE = 0x0FFF
KADC_LEFT = 150
KADC_RIGHT = 600
KADC_VOL_UP = 1100
KADC_VOL_DOWN = 2000
KADC_BACK = 2360
KADC_ENTER = 2760


class QmpClient:
    """Minimal QEMU Machine Protocol client for live QOM property updates."""

    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        last_err: Exception | None = None
        for _ in range(60):
            try:
                self.sock = socket.create_connection((host, port), timeout=timeout)
                self.sock.settimeout(timeout)
                self.fp = self.sock.makefile("rwb", buffering=0)
                self.fp.readline()  # greeting
                self.fp.write(b'{"execute":"qmp_capabilities"}\n')
                self.fp.readline()
                return
            except OSError as exc:
                last_err = exc
                time.sleep(0.05)
        raise RuntimeError(f"QMP connect failed: {last_err}")

    def close(self) -> None:
        self.fp.close()
        self.sock.close()

    def qom_set(self, prop: str, value: object) -> None:
        req = json.dumps(
            {"execute": "qom-set", "arguments": {"path": "/machine", "property": prop, "value": value}}
        ) + "\n"
        self.fp.write(req.encode("ascii"))
        while True:
            line = self.fp.readline().decode("utf-8", "replace")
            if not line:
                raise EOFError("QMP closed")
            obj = json.loads(line)
            if "error" in obj:
                raise RuntimeError(f"QMP qom-set {prop}={value!r} failed: {obj}")
            if "return" in obj:
                return


def fb_to_image(fb: bytes, scale: int) -> tuple[Image.Image, Image.Image]:
    """Convert raw 128x160 RGB565 framebuffer to (raw_128x160, cropped_80x160_scaled)."""
    raw = Image.new("RGB", (128, 160))
    rpx = raw.load()
    for y in range(160):
        for x in range(128):
            off = (y * 128 + x) * 2
            val = fb[off] | (fb[off + 1] << 8)
            rpx[x, y] = (
                ((val >> 11) & 0x1F) * 255 // 31,
                ((val >> 5) & 0x3F) * 255 // 63,
                (val & 0x1F) * 255 // 31,
            )
    crop = raw.crop((26, 0, 106, 160))
    return raw, crop.resize((80 * scale, 160 * scale), Image.NEAREST)


def run_tour(emu_dir: Path, patched_flash: Path, scale: int, vbus: str, out_path: Path) -> int:
    """Boot stock pen133_2.0.65, navigate via QMP KADC keys, and save a 4-panel UI strip."""
    gdb_port = run_qemu.free_tcp_port()
    qmp_port = run_qemu.free_tcp_port()
    machine = (
        f"smartlink-sl6801,rom={emu_dir}/rom.bin,rom-stub={emu_dir}/rom_stub.bin,"
        f"flash={patched_flash},sd-image={emu_dir}/sd.img,"
        f"viewtask-local=off,high-rom-hle=on,boot-decision=0,refclk-hz=1000000,"
        f"pmu-battery-code=90,pmu-vbus={vbus},kadc-raw-ch1={KADC_IDLE}"
    )
    cmd = [
        str(run_qemu.DEFAULT_QEMU),
        "-M", machine,
        "-display", "none",
        "-serial", "none",
        "-monitor", "none",
        "-gdb", f"tcp:127.0.0.1:{gdb_port}",
        "-S",
        "-qmp", f"tcp:127.0.0.1:{qmp_port},server,nowait",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    frames: list[Image.Image] = []
    try:
        qmp = QmpClient("127.0.0.1", qmp_port)
        gdb: run_qemu.GdbRspClient | None = None
        for _ in range(60):
            try:
                gdb = run_qemu.GdbRspClient("127.0.0.1", gdb_port, timeout=10.0)
                break
            except OSError:
                time.sleep(0.05)
        if gdb is None:
            raise RuntimeError("GDB connect failed")

        resp = gdb_send_ok(gdb, f"Z1,{VIEWTASK_BP_PC:x},2")
        if resp != "OK":
            raise RuntimeError(f"Failed to set ViewTask BP: {resp!r}")
        gdb.sock.settimeout(12.0)
        gdb_send_ok(gdb, "c")
        gdb_send_ok(gdb, f"z1,{VIEWTASK_BP_PC:x},2")
        apply_runtime_patches(gdb)

        def press(adc: int, hold: float = 0.18, wait: float = 0.7) -> None:
            qmp.qom_set("kadc-raw-ch1", adc)
            gdb.continue_for(hold)
            qmp.qom_set("kadc-raw-ch1", KADC_IDLE)
            gdb.continue_for(wait)

        def snap(label: str) -> None:
            fb = gdb.read_mem(run_qemu.FB_ADDR, 128 * 160 * 2)
            _, img = fb_to_image(fb, scale)
            frames.append(img)
            print(f"Captured tour frame: {label}")

        # 1. Boot to Main Menu -> Music
        gdb.continue_for(2.0)
        snap("Music (main menu)")

        # 2. Navigate RIGHT x2 -> Recorder
        press(KADC_RIGHT)
        press(KADC_RIGHT)
        snap("Recorder (main menu)")

        # 3. Navigate RIGHT -> Bluetooth -> ENTER
        press(KADC_RIGHT)
        press(KADC_ENTER, wait=1.0)
        snap("Bluetooth (sub-screen)")

        # 4. BACK -> RIGHT -> Settings -> ENTER
        press(KADC_BACK, wait=0.8)
        press(KADC_RIGHT)
        press(KADC_ENTER, wait=1.0)
        snap("Settings (sub-screen)")

        gdb.close()
        qmp.close()
    finally:
        proc.terminate()
        _, err_bytes = proc.communicate(timeout=5)
        stderr = err_bytes.decode("utf-8", "replace")
        if stderr:
            print("QEMU stderr tail:", stderr[-300:])

    gap = 8
    w, h = frames[0].size
    strip = Image.new("RGB", (w * len(frames) + gap * (len(frames) - 1), h), (20, 20, 24))
    for i, im in enumerate(frames):
        strip.paste(im, (i * (w + gap), 0))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    strip.save(out_path)
    print(f"Saved {len(frames)}-panel tour strip to {out_path} ({strip.width}x{strip.height})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Boot stock pen133_2.0.65 in QEMU and capture LCD")
    ap.add_argument("--out", type=Path, default=Path("/tmp/sl6801_stock_emu/stock_ui.png"))
    ap.add_argument("--wait-sec", type=float, default=2.5, help="Seconds to run after ViewTask starts")
    ap.add_argument("--scale", type=int, default=None, help="Integer zoom factor (default: 2 for --tour, 3 otherwise)")
    ap.add_argument("--vbus", choices=("off", "on"), default="off", help="PMU VBUS state (default: off)")
    ap.add_argument("--tour", action="store_true", help="Navigate menus via QMP and compose a 4-screen horizontal strip")
    args = ap.parse_args()

    emu_dir = Path("/tmp/sl6801_stock_emu")
    patched_flash = emu_dir / "firmware_2M_viewtask.bin"
    build_patched_flash((REPO_ROOT / "dumps/firmware_2M.bin").resolve(), patched_flash)

    scale = args.scale if args.scale is not None else (2 if args.tour else 3)
    if args.tour:
        return run_tour(emu_dir, patched_flash, scale, args.vbus, args.out)

    machine = (
        f"smartlink-sl6801,rom={emu_dir}/rom.bin,rom-stub={emu_dir}/rom_stub.bin,"
        f"flash={patched_flash},sd-image={emu_dir}/sd.img,"
        f"viewtask-local=off,high-rom-hle=on,boot-decision=0,refclk-hz=1000000,"
        f"pmu-battery-code=90,pmu-vbus={args.vbus}"
    )

    def session(client: run_qemu.GdbRspClient):
        resp = gdb_send_ok(client, f"Z1,{VIEWTASK_BP_PC:x},2")
        if resp != "OK":
            raise RuntimeError(f"Failed to set ViewTask BP: {resp!r}")

        client.sock.settimeout(12.0)
        reply = gdb_send_ok(client, "c")
        pc = client.read_regs()[15]
        print(f"Stopped at PC=0x{pc:08X} (reply={reply[:20]})")
        gdb_send_ok(client, f"z1,{VIEWTASK_BP_PC:x},2")
        if abs(pc - VIEWTASK_BP_PC) <= 4:
            apply_runtime_patches(client)
        else:
            print("WARNING: Did not stop at ViewTask BP!")

        client.continue_for(args.wait_sec)
        fb = client.read_mem(run_qemu.FB_ADDR, 128 * 160 * 2)
        pc2 = client.read_regs()[15]
        return fb, pc2

    (fb, pc2), stderr = run_qemu.run_qemu_session(Path(run_qemu.DEFAULT_QEMU), machine, session)
    print(f"Captured LCD at PC=0x{pc2:08X}")
    if stderr:
        print("QEMU stderr tail:", stderr[-300:])

    raw, scaled = fb_to_image(fb, scale)
    raw.save("/tmp/sl6801_stock_emu/stock_ui_raw.png")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    scaled.save(args.out)
    print(f"Saved {args.out} ({80*scale}x{160*scale})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
