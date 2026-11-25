#!/usr/bin/env python3
"""
Clean-room chunked partial flasher & CRC-16 verifier for Smartlink SL6801 BootROM (301a:2800).

Inspired by anton-veretenenko/smartlink_flash's range diffing, sector alignment,
and USB retry/verify workflow, built on this project's documented BootROM 0xC0
vendor SCSI protocol (see docs/FIRMWARE_ANALYSIS.md Section 3.3).

Safety defaults:
  - Targets BootROM USB DFU PID 0x2800 ONLY (never XIP Card Reader 0x2801).
  - Defaults to --dry-run; hardware writes require passing --execute explicitly.

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import ctypes
import struct
import sys
import time
from pathlib import Path


VID = 0x301A
PID_BOOTLOADER = 0x2800

SECTOR_SIZE = 4096
MAX_CHUNK = 4096

SUBCMD_READID = 0x00
SUBCMD_WRITEPAGE = 0x02
SUBCMD_ERASE4K = 0x03
SUBCMD_READ = 0x07
SUBCMD_WRITERAM = 0x09
SUBCMD_RUNRAM = 0x0A
SUBCMD_INIT = 0x81
SUBCMD_CRC = 0x82
SUBCMD_PING = 0x86

REBOOT_STUB_ADDR = 0x00800E80
# Thumb-2 PIC @ 0x00800E80 (198 B): cpsid i; PMU[0x23] &= ~0xC0 (via RTWI); SoC WDT reset (AON_BC=0, AON_D8&=~2, WDT 0x2001/1); delay; SCB->AIRCR fallback
REBOOT_STUB_BYTES = bytes.fromhex(
    "72 b6 43 f6 a5 45 23 20 a8 47 00 f0 3f 01 43 f6 "
    "69 44 23 20 a0 47 00 20 a8 47 00 f0 fe 01 00 20 "
    "a0 47 22 20 00 21 a0 47 20 20 2f 21 a0 47 1b 20 "
    "a8 47 40 f0 80 01 1b 20 a0 47 1b 20 a8 47 00 f0 "
    "7f 01 1b 20 a0 47 20 20 af 21 a0 47 00 bf fd e7"
)


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    """Compute CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF)."""
    crc = init
    for byte in data:
        t = ((crc >> 8) ^ byte) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


def align_and_merge_ranges(
    ranges: list[tuple[int, int]], sector_size: int = SECTOR_SIZE
) -> list[tuple[int, int]]:
    """Expand each (offset, length) outward to sector boundaries and merge overlaps."""
    if not ranges:
        return []
    aligned: list[tuple[int, int]] = []
    for off, length in ranges:
        if off < 0 or length < 0:
            raise ValueError(f"Negative range not allowed: ({off}, {length})")
        if length == 0:
            continue
        start = (off // sector_size) * sector_size
        end = ((off + length + sector_size - 1) // sector_size) * sector_size
        aligned.append((start, end))
    aligned.sort()
    merged: list[tuple[int, int]] = []
    for start, end in aligned:
        if merged and start <= merged[-1][1]:
            prev_s, prev_e = merged[-1]
            merged[-1] = (prev_s, max(prev_e, end))
        else:
            merged.append((start, end))
    return [(s, e - s) for s, e in merged]


def diff_sector_ranges(
    old_bytes: bytes, new_bytes: bytes, sector_size: int = SECTOR_SIZE
) -> list[tuple[int, int]]:
    """Return coalesced 4 KiB-aligned (offset, length) ranges where new_bytes differs."""
    total_len = len(new_bytes)
    dirty_sectors: list[tuple[int, int]] = []
    for off in range(0, total_len, sector_size):
        new_sec = new_bytes[off : off + sector_size]
        if len(new_sec) < sector_size:
            new_sec = new_sec + b"\xff" * (sector_size - len(new_sec))
        old_sec = old_bytes[off : off + sector_size]
        if len(old_sec) < sector_size:
            old_sec = old_sec + b"\xff" * (sector_size - len(old_sec))
        if old_sec != new_sec:
            dirty_sectors.append((off, sector_size))
    return align_and_merge_ranges(dirty_sectors, sector_size=sector_size)


def parse_range_spec(spec: str) -> tuple[int, int]:
    """Parse `<offset>:<length>` accepting decimal or 0x hex literals."""
    if ":" not in spec:
        raise ValueError(f"Range '{spec}' must be formatted as OFFSET:LENGTH")
    off_s, len_s = spec.split(":", 1)
    return int(off_s, 0), int(len_s, 0)


def expected_sector_data(image: bytes, off: int, length: int) -> bytes:
    """Slice image[off:off+length], right-padding missing tail with erased 0xFF bytes."""
    chunk = image[off : off + length]
    if len(chunk) < length:
        chunk += b"\xff" * (length - len(chunk))
    return chunk


class BootromFlashClient:
    """Abstract client implementing SL6801 BootROM 0xC0 SPI NOR subcommands."""

    def close(self) -> None:
        pass

    def init_nor(self) -> None:
        raise NotImplementedError

    def ping(self) -> None:
        raise NotImplementedError

    def read_id(self) -> int:
        raise NotImplementedError

    def erase_4k(self, offset: int) -> None:
        raise NotImplementedError

    def write_page(self, offset: int, data: bytes) -> None:
        raise NotImplementedError

    def read_flash(self, offset: int, length: int) -> bytes:
        raise NotImplementedError

    def crc_flash(self, offset: int, length: int) -> int:
        raise NotImplementedError

    def write_ram(self, addr: int, data: bytes) -> None:
        raise NotImplementedError

    def run_ram(self, addr: int, expect_disconnect: bool = False) -> None:
        raise NotImplementedError

    def reboot_chip(self) -> None:
        """Write a PMU[0x20]|=0x80 (+ AIRCR fallback) stub to scratch SRAM and execute it."""
        self.write_ram(REBOOT_STUB_ADDR, REBOOT_STUB_BYTES)
        self.run_ram(REBOOT_STUB_ADDR, expect_disconnect=True)


class PyUsbBootromClient(BootromFlashClient):
    """Live libusb/pyusb transport for SL6801 BootROM mode (301a:2800 ONLY)."""

    def __init__(self, vid: int = VID, pid: int = PID_BOOTLOADER, timeout_ms: int = 4000) -> None:
        if pid != PID_BOOTLOADER:
            raise ValueError(
                f"Refusing non-bootloader PID 0x{pid:04x}; partial flash requires 0x{PID_BOOTLOADER:04x}"
            )
        try:
            import usb.core
            import usb.util
        except ImportError as exc:
            raise SystemExit("pyusb is required for --execute: pip install pyusb") from exc

        self._usb_util = usb.util
        self._timeout_ms = timeout_ms
        self.dev = usb.core.find(idVendor=vid, idProduct=pid)
        if self.dev is None:
            raise SystemExit(
                f"SL6801 BootROM ({vid:04x}:{pid:04x}) not found. Enter PC Upgrade / mask ROM DFU mode first."
            )
        self._reattach = False
        if self.dev.is_kernel_driver_active(0):
            self.dev.detach_kernel_driver(0)
            self._reattach = True
        self._usb_util.claim_interface(self.dev, 0)
        cfg = self.dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.ep_out = self._usb_util.find_descriptor(
            intf,
            custom_match=lambda e: self._usb_util.endpoint_direction(e.bEndpointAddress)
            == self._usb_util.ENDPOINT_OUT,
        )
        self.ep_in = self._usb_util.find_descriptor(
            intf,
            custom_match=lambda e: self._usb_util.endpoint_direction(e.bEndpointAddress)
            == self._usb_util.ENDPOINT_IN,
        )
        self._tag = 1
        self._disconnected = False

    def close(self) -> None:
        if not self._disconnected:
            try:
                self._usb_util.release_interface(self.dev, 0)
            except Exception:
                pass
            if self._reattach:
                try:
                    self.dev.attach_kernel_driver(0)
                except Exception:
                    pass

    def _vendor_c0(
        self,
        subcmd: int,
        arg1: int,
        arg2: int,
        data_out: bytes = b"",
        in_len: int = 0,
        expect_disconnect: bool = False,
    ) -> bytes:
        tag = self._tag
        self._tag += 1
        direction = 0x80 if in_len > 0 else 0x00
        xfer_len = in_len if in_len > 0 else len(data_out)
        cdb = struct.pack("<BBII6x", 0xC0, subcmd, arg1, arg2)
        cbw = struct.pack("<IIIBBB", 0x43425355, tag, xfer_len, direction, 0, 16) + cdb
        self.ep_out.write(cbw, timeout=self._timeout_ms)

        result = b""
        try:
            if data_out:
                self.ep_out.write(data_out, timeout=self._timeout_ms)
            elif in_len > 0:
                result = bytes(self.ep_in.read(in_len, timeout=self._timeout_ms))

            csw = bytes(self.ep_in.read(13, timeout=self._timeout_ms))
        except (IOError, OSError) as exc:
            if expect_disconnect:
                self._disconnected = True
                return b""
            raise exc
        except Exception as exc:
            if expect_disconnect:
                self._disconnected = True
                return b""
            raise exc

        sig, rtag, residue, status = struct.unpack("<IIIB", csw)
        if sig != 0x53425355 or rtag != tag or status != 0:
            raise IOError(
                f"CSW failure on subcmd 0x{subcmd:02x}: sig=0x{sig:08x} tag={rtag} "
                f"residue={residue} status={status}"
            )
        return result

    def init_nor(self) -> None:
        self._vendor_c0(SUBCMD_INIT, 2, 0)

    def ping(self) -> None:
        self.read_id()

    def read_id(self) -> int:
        raw = self._vendor_c0(SUBCMD_READID, 4, 0, in_len=4)
        return struct.unpack("<I", raw)[0]

    def erase_4k(self, offset: int) -> None:
        self._vendor_c0(SUBCMD_ERASE4K, SECTOR_SIZE, offset)

    def write_page(self, offset: int, data: bytes) -> None:
        if len(data) == 0 or len(data) > MAX_CHUNK:
            raise ValueError(f"write_page length {len(data)} out of 1..{MAX_CHUNK}")
        self._vendor_c0(SUBCMD_WRITEPAGE, len(data), offset, data_out=data)

    def read_flash(self, offset: int, length: int) -> bytes:
        if length == 0 or length > MAX_CHUNK:
            raise ValueError(f"read_flash length {length} out of 1..{MAX_CHUNK}")
        return self._vendor_c0(SUBCMD_READ, length, offset, in_len=length)

    def crc_flash(self, offset: int, length: int) -> int:
        raw = self._vendor_c0(SUBCMD_CRC, length, offset, in_len=2)
        return struct.unpack("<H", raw)[0]

    def write_ram(self, addr: int, data: bytes) -> None:
        if len(data) == 0 or len(data) > 512:
            raise ValueError(f"write_ram length {len(data)} out of 1..512")
        self._vendor_c0(SUBCMD_WRITERAM, len(data), addr, data_out=data)

    def run_ram(self, addr: int, expect_disconnect: bool = False) -> None:
        self._vendor_c0(
            SUBCMD_RUNRAM, 0, addr | 1, expect_disconnect=expect_disconnect
        )


class LibUsbCtypesBootromClient(BootromFlashClient):
    """Zero-dependency libusb-1.0 ctypes client for SL6801 BootROM (301a:2800 ONLY)."""

    def __init__(self, vid: int = VID, pid: int = PID_BOOTLOADER, timeout_ms: int = 4000) -> None:
        if pid != PID_BOOTLOADER:
            raise ValueError(
                f"Refusing non-bootloader PID 0x{pid:04x}; partial flash requires 0x{PID_BOOTLOADER:04x}"
            )
        self._timeout_ms = timeout_ms
        self._tag = 1
        self._disconnected = False
        try:
            self.lib = ctypes.CDLL("libusb-1.0.so.0")
        except OSError as exc:
            raise RuntimeError(f"Unable to load libusb-1.0.so.0: {exc}") from exc

        lib = self.lib
        lib.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        lib.libusb_init.restype = ctypes.c_int
        lib.libusb_exit.argtypes = [ctypes.c_void_p]
        lib.libusb_exit.restype = None
        lib.libusb_open_device_with_vid_pid.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint16,
            ctypes.c_uint16,
        ]
        lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        lib.libusb_kernel_driver_active.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_kernel_driver_active.restype = ctypes.c_int
        lib.libusb_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_detach_kernel_driver.restype = ctypes.c_int
        lib.libusb_attach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_attach_kernel_driver.restype = ctypes.c_int
        lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_claim_interface.restype = ctypes.c_int
        lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_release_interface.restype = ctypes.c_int
        lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p,
            ctypes.c_ubyte,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
            ctypes.c_uint,
        ]
        lib.libusb_bulk_transfer.restype = ctypes.c_int
        lib.libusb_close.argtypes = [ctypes.c_void_p]
        lib.libusb_close.restype = None

        self.ctx = ctypes.c_void_p()
        if lib.libusb_init(ctypes.byref(self.ctx)) != 0:
            raise RuntimeError("libusb_init failed")

        self.handle = lib.libusb_open_device_with_vid_pid(self.ctx, vid, pid)
        if not self.handle:
            lib.libusb_exit(self.ctx)
            self.ctx = ctypes.c_void_p()
            raise SystemExit(
                f"SL6801 BootROM ({vid:04x}:{pid:04x}) not found. Enter PC Upgrade / mask ROM DFU mode first."
            )

        self._reattach = False
        if lib.libusb_kernel_driver_active(self.handle, 0) == 1:
            if lib.libusb_detach_kernel_driver(self.handle, 0) == 0:
                self._reattach = True

        rc = lib.libusb_claim_interface(self.handle, 0)
        if rc != 0:
            if self._reattach:
                lib.libusb_attach_kernel_driver(self.handle, 0)
            lib.libusb_close(self.handle)
            lib.libusb_exit(self.ctx)
            raise PermissionError(f"libusb_claim_interface failed ({rc}); try sudo")

    def close(self) -> None:
        if getattr(self, "handle", None):
            if not getattr(self, "_disconnected", False):
                try:
                    self.lib.libusb_release_interface(self.handle, 0)
                    if self._reattach:
                        self.lib.libusb_attach_kernel_driver(self.handle, 0)
                except Exception:
                    pass
            self.lib.libusb_close(self.handle)
            self.handle = None
        if getattr(self, "ctx", None):
            self.lib.libusb_exit(self.ctx)
            self.ctx = None

    def _bulk_write(self, data: bytes) -> None:
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        actual = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(
            self.handle, 0x01, ctypes.cast(buf, ctypes.c_void_p), len(data),
            ctypes.byref(actual), self._timeout_ms,
        )
        if rc != 0 or actual.value != len(data):
            raise IOError(f"libusb bulk OUT failed: rc={rc} sent={actual.value}/{len(data)}")

    def _bulk_read(self, n: int) -> bytes:
        buf = (ctypes.c_ubyte * n)()
        actual = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(
            self.handle, 0x81, ctypes.cast(buf, ctypes.c_void_p), n,
            ctypes.byref(actual), self._timeout_ms,
        )
        if rc != 0:
            raise IOError(f"libusb bulk IN failed: rc={rc} got={actual.value}/{n}")
        return bytes(buf[: actual.value])

    def _vendor_c0(
        self,
        subcmd: int,
        arg1: int,
        arg2: int,
        data_out: bytes = b"",
        in_len: int = 0,
        expect_disconnect: bool = False,
    ) -> bytes:
        tag = self._tag
        self._tag += 1
        direction = 0x80 if in_len > 0 else 0x00
        xfer_len = in_len if in_len > 0 else len(data_out)
        cdb = struct.pack("<BBII6x", 0xC0, subcmd, arg1, arg2)
        cbw = struct.pack("<IIIBBB", 0x43425355, tag, xfer_len, direction, 0, 16) + cdb
        self._bulk_write(cbw)

        result = b""
        try:
            if data_out:
                self._bulk_write(data_out)
            elif in_len > 0:
                result = self._bulk_read(in_len)

            csw = self._bulk_read(13)
        except (IOError, OSError):
            if expect_disconnect:
                self._disconnected = True
                return b""
            raise

        sig, rtag, residue, status = struct.unpack("<IIIB", csw)
        if sig != 0x53425355 or rtag != tag or status != 0:
            raise IOError(
                f"CSW failure on subcmd 0x{subcmd:02x}: sig=0x{sig:08x} tag={rtag} "
                f"residue={residue} status={status}"
            )
        return result

    def init_nor(self) -> None:
        self._vendor_c0(SUBCMD_INIT, 2, 0)

    def ping(self) -> None:
        self.read_id()

    def read_id(self) -> int:
        raw = self._vendor_c0(SUBCMD_READID, 4, 0, in_len=4)
        return struct.unpack("<I", raw)[0]

    def erase_4k(self, offset: int) -> None:
        self._vendor_c0(SUBCMD_ERASE4K, SECTOR_SIZE, offset)

    def write_page(self, offset: int, data: bytes) -> None:
        if len(data) == 0 or len(data) > MAX_CHUNK:
            raise ValueError(f"write_page length {len(data)} out of 1..{MAX_CHUNK}")
        self._vendor_c0(SUBCMD_WRITEPAGE, len(data), offset, data_out=data)

    def read_flash(self, offset: int, length: int) -> bytes:
        if length == 0 or length > MAX_CHUNK:
            raise ValueError(f"read_flash length {length} out of 1..{MAX_CHUNK}")
        return self._vendor_c0(SUBCMD_READ, length, offset, in_len=length)

    def crc_flash(self, offset: int, length: int) -> int:
        raw = self._vendor_c0(SUBCMD_CRC, length, offset, in_len=2)
        return struct.unpack("<H", raw)[0]

    def write_ram(self, addr: int, data: bytes) -> None:
        if len(data) == 0 or len(data) > 512:
            raise ValueError(f"write_ram length {len(data)} out of 1..512")
        self._vendor_c0(SUBCMD_WRITERAM, len(data), addr, data_out=data)

    def run_ram(self, addr: int, expect_disconnect: bool = False) -> None:
        self._vendor_c0(
            SUBCMD_RUNRAM, 0, addr | 1, expect_disconnect=expect_disconnect
        )


def open_bootrom_client(vid: int = VID, pid: int = PID_BOOTLOADER, timeout_ms: int = 4000) -> BootromFlashClient:
    """Open BootROM flash client using pyusb if installed, otherwise zero-dep libusb ctypes."""
    try:
        import usb.core  # noqa: F401
        return PyUsbBootromClient(vid=vid, pid=pid, timeout_ms=timeout_ms)
    except ImportError:
        return LibUsbCtypesBootromClient(vid=vid, pid=pid, timeout_ms=timeout_ms)


def _with_retries(client: BootromFlashClient, op_name: str, fn, retries: int):
    last_exc: Exception | None = None
    for attempt in range(retries):
        try:
            return fn()
        except (OSError, IOError, RuntimeError) as exc:
            last_exc = exc
            if attempt + 1 >= retries:
                break
            try:
                client.ping()
                client.init_nor()
            except Exception:
                pass
    raise IOError(f"{op_name} failed after {retries} attempt(s): {last_exc}")


def execute_flash_plan(
    client: BootromFlashClient | None,
    image: bytes,
    ranges: list[tuple[int, int]],
    chunk: int = MAX_CHUNK,
    retries: int = 3,
    verify: bool = True,
    dry_run: bool = True,
    out=sys.stdout,
) -> dict[str, int]:
    """Execute (or print under dry_run) sector-aligned erase/write/verify across ranges."""
    if chunk <= 0 or chunk > MAX_CHUNK or (SECTOR_SIZE % chunk) != 0:
        raise ValueError(f"chunk={chunk} must divide {SECTOR_SIZE} and be <= {MAX_CHUNK}")

    total_bytes = sum(length for _, length in ranges)
    total_sectors = total_bytes // SECTOR_SIZE
    print(
        f"Plan: {len(ranges)} range(s), {total_sectors} sector(s) ({total_bytes:,} bytes), "
        f"chunk={chunk} B, retries={retries}, verify={verify}, dry_run={dry_run}",
        file=out,
    )
    for idx, (start, length) in enumerate(ranges):
        sec_data = expected_sector_data(image, start, length)
        sw_crc = crc16_ccitt_false(sec_data)
        print(
            f"  [{idx}] 0x{start:06X}..0x{start + length:06X} "
            f"({length:,} B, {length // SECTOR_SIZE} sectors, CRC-16=0x{sw_crc:04X})",
            file=out,
        )

    stats = {
        "ranges": len(ranges),
        "sectors_erased": 0,
        "chunks_written": 0,
        "bytes_written": 0,
        "bytes_verified": 0,
    }
    if dry_run or not ranges:
        return stats

    if client is None:
        raise ValueError("Live USB transport missing when dry_run=False")

    t0 = time.monotonic()
    _with_retries(client, "init_nor", client.init_nor, retries)
    flash_id = _with_retries(client, "read_id", client.read_id, retries)
    print(f"BootROM SPI NOR ID: 0x{flash_id:08X}", file=out)

    BOOT_HEADER_OFFSET = 0x00D000

    # 1. Erase every 4 KiB sector across all ranges, invalidating the boot
    #    header sector (0x00D000) first so any interrupted flash falls back
    #    to 301a:2800 BootROM mode even when PD0 sits on an ADC button ladder.
    erase_sectors: list[int] = []
    for start, length in ranges:
        for sec_off in range(start, start + length, SECTOR_SIZE):
            erase_sectors.append(sec_off)
    if BOOT_HEADER_OFFSET in erase_sectors:
        erase_sectors.remove(BOOT_HEADER_OFFSET)
        erase_sectors.insert(0, BOOT_HEADER_OFFSET)

    for sec_off in erase_sectors:
        _with_retries(
            client,
            f"erase_4k@0x{sec_off:06X}",
            lambda o=sec_off: client.erase_4k(o),
            retries,
        )
        stats["sectors_erased"] += 1

    # 2. Program all payload chunks first, deferring the boot header sector
    #    (0x00D000..0x00DFFF) to the very end ("header-last" power-safe order).
    deferred_header_chunks: list[int] = []
    for start, length in ranges:
        for off in range(start, start + length, chunk):
            if BOOT_HEADER_OFFSET <= off < BOOT_HEADER_OFFSET + SECTOR_SIZE:
                deferred_header_chunks.append(off)
                continue
            data = expected_sector_data(image, off, chunk)
            _with_retries(
                client,
                f"write_page@0x{off:06X}",
                lambda o=off, d=data: client.write_page(o, d),
                retries,
            )
            stats["chunks_written"] += 1
            stats["bytes_written"] += len(data)

    for off in deferred_header_chunks:
        data = expected_sector_data(image, off, chunk)
        _with_retries(
            client,
            f"write_page@0x{off:06X}",
            lambda o=off, d=data: client.write_page(o, d),
            retries,
        )
        stats["chunks_written"] += 1
        stats["bytes_written"] += len(data)

    for start, length in ranges:
        # 3. Verify both hardware CRC-16 and exact byte readback
        if verify:
            exp = expected_sector_data(image, start, length)
            exp_crc = crc16_ccitt_false(exp)
            hw_crc = _with_retries(
                client,
                f"crc@0x{start:06X}",
                lambda s=start, l=length: client.crc_flash(s, l),
                retries,
            )
            if hw_crc != exp_crc:
                raise RuntimeError(
                    f"Hardware CRC-16 mismatch at 0x{start:06X}+0x{length:X}: "
                    f"got 0x{hw_crc:04X}, expected 0x{exp_crc:04X}"
                )
            for off in range(start, start + length, chunk):
                got = _with_retries(
                    client,
                    f"read@0x{off:06X}",
                    lambda o=off: client.read_flash(o, chunk),
                    retries,
                )
                want = exp[off - start : off - start + chunk]
                if got != want:
                    for bi, (gb, wb) in enumerate(zip(got, want)):
                        if gb != wb:
                            raise RuntimeError(
                                f"Readback mismatch at 0x{off + bi:06X}: "
                                f"got 0x{gb:02X}, expected 0x{wb:02X}"
                            )
                    raise RuntimeError(f"Readback length mismatch at 0x{off:06X}")
                stats["bytes_verified"] += len(got)

    elapsed = max(time.monotonic() - t0, 1e-6)
    print(
        f"Flashed {stats['bytes_written']:,} B across {stats['sectors_erased']} sectors "
        f"in {elapsed:.2f}s (verified {stats['bytes_verified']:,} B)",
        file=out,
    )
    return stats


class _MockBootromClient(BootromFlashClient):
    """Deterministic in-memory SL6801 BootROM SPI NOR mock for --self-test."""

    def __init__(self, initial: bytes, fail_every: int = 0) -> None:
        self.mem = bytearray(initial)
        self.fail_every = fail_every
        self.ops = 0
        self.inited = False
        self.pings = 0
        self.erases = 0
        self.writes = 0
        self.ram: dict[int, bytes] = {}
        self.run_ram_calls: list[int] = []
        self.rebooted = 0

    def _maybe_fail(self) -> None:
        self.ops += 1
        if self.fail_every > 0 and (self.ops % self.fail_every) == 0:
            raise IOError("Injected transient USB bulk stall")

    def init_nor(self) -> None:
        self._maybe_fail()
        self.inited = True

    def ping(self) -> None:
        self.pings += 1

    def read_id(self) -> int:
        self._maybe_fail()
        return 0x14851485

    def erase_4k(self, offset: int) -> None:
        assert (offset % SECTOR_SIZE) == 0
        self._maybe_fail()
        self.mem[offset : offset + SECTOR_SIZE] = b"\xff" * SECTOR_SIZE
        self.erases += 1

    def write_page(self, offset: int, data: bytes) -> None:
        assert 0 < len(data) <= MAX_CHUNK
        self._maybe_fail()
        # Real SPI NOR can only clear 1 bits to 0 without erasing!
        for i, b in enumerate(data):
            self.mem[offset + i] &= b
        self.writes += 1

    def read_flash(self, offset: int, length: int) -> bytes:
        self._maybe_fail()
        return bytes(self.mem[offset : offset + length])

    def crc_flash(self, offset: int, length: int) -> int:
        self._maybe_fail()
        return crc16_ccitt_false(bytes(self.mem[offset : offset + length]))

    def write_ram(self, addr: int, data: bytes) -> None:
        self._maybe_fail()
        self.ram[addr] = bytes(data)

    def run_ram(self, addr: int, expect_disconnect: bool = False) -> None:
        del expect_disconnect
        self._maybe_fail()
        self.run_ram_calls.append(addr | 1)
        if (addr & ~1) == REBOOT_STUB_ADDR:
            self.rebooted += 1


def run_self_test() -> int:
    """Verify diffing, 4 KiB alignment, coalescing, retries, dry-run, and CRC verify."""
    # 1. Alignment & range coalescing
    merged = align_and_merge_ranges([(0x00010, 0x100), (0x01800, 0x1200), (0x10004, 1)])
    assert merged == [(0x00000, 0x3000), (0x10000, 0x1000)], f"Unexpected merge: {merged}"

    # 2. Sector diff on synthetic 64 KiB NOR images
    old_img = bytearray([0xFF] * 0x10000)
    old_img[0x0000:0x0200] = b"\xAA" * 0x0200
    old_img[0x4000:0x8000] = b"\x55" * 0x4000
    new_img = bytearray(old_img)
    new_img[0x0100:0x0120] = b"\xCC" * 0x20  # flips bits 0->1 (needs erase!) -> sector 0
    new_img[0x5010] = 0x12                   # sector 0x5000
    new_img[0x6020] = 0x34                   # contiguous with 0x5000 -> [0x5000, 0x2000]
    ranges = diff_sector_ranges(bytes(old_img), bytes(new_img))
    assert ranges == [(0x0000, 0x1000), (0x5000, 0x2000)], f"Unexpected diff ranges: {ranges}"

    # 3. Default dry-run performs ZERO flash erases/writes
    mock = _MockBootromClient(bytes(old_img), fail_every=0)
    stats_dry = execute_flash_plan(
        mock, bytes(new_img), ranges, chunk=2048, retries=3, verify=True, dry_run=True
    )
    assert stats_dry["sectors_erased"] == 0 and stats_dry["bytes_written"] == 0
    assert mock.erases == 0 and mock.writes == 0 and bytes(mock.mem) == bytes(old_img)

    # 4. Live execution with transient USB failures every 4th op -> recovers via retries
    flaky = _MockBootromClient(bytes(old_img), fail_every=4)
    stats_ok = execute_flash_plan(
        flaky, bytes(new_img), ranges, chunk=2048, retries=3, verify=True, dry_run=False
    )
    assert stats_ok["sectors_erased"] == 3
    assert stats_ok["bytes_written"] == 3 * SECTOR_SIZE
    assert stats_ok["bytes_verified"] == 3 * SECTOR_SIZE
    assert bytes(flaky.mem) == bytes(new_img)
    assert flaky.pings > 0

    # 5. Corrupting a byte after programming causes verify to raise RuntimeError
    bad = _MockBootromClient(bytes(old_img), fail_every=0)
    orig_read = bad.read_flash
    def _corrupt_once(offset: int, length: int) -> bytes:
        res = bytearray(orig_read(offset, length))
        res[0] ^= 0x01
        return bytes(res)
    bad.read_flash = _corrupt_once  # type: ignore[method-assign]
    saw_err = False
    try:
        execute_flash_plan(
            bad, bytes(new_img), [(0x5000, 0x1000)], chunk=4096, retries=2, verify=True, dry_run=False
        )
    except RuntimeError:
        saw_err = True
    assert saw_err, "Expected verify to reject bit-flipped readback"

    # 6. BootROM WRITERAM + RUNRAM reboot stub
    reb = _MockBootromClient(bytes(old_img), fail_every=0)
    reb.reboot_chip()
    assert reb.ram.get(REBOOT_STUB_ADDR) == REBOOT_STUB_BYTES
    assert reb.run_ram_calls == [REBOOT_STUB_ADDR | 1]
    assert reb.rebooted == 1

    print(
        "flash_ranges.py self-test PASSED "
        "(alignment, diff, retries, dry-run, CRC+readback verify, reboot stub)"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Chunked partial SPI NOR flasher & verifier for SL6801 BootROM (301a:2800)"
    )
    ap.add_argument("--self-test", action="store_true", help="Run synthetic mock transport test")
    ap.add_argument(
        "--diff",
        nargs=2,
        metavar=("OLD_BIN", "NEW_BIN"),
        type=Path,
        help="Compute changed 4 KiB sectors from OLD_BIN -> NEW_BIN and flash NEW_BIN",
    )
    ap.add_argument("--image", type=Path, help="Image file to program (with --range)")
    ap.add_argument(
        "--range",
        action="append",
        default=[],
        metavar="OFF:LEN",
        help="Byte range to align outward to 4 KiB sectors and flash (repeatable)",
    )
    ap.add_argument("--chunk", type=int, default=MAX_CHUNK, help="Write/read chunk size (default 4096)")
    ap.add_argument("--retries", type=int, default=3, help="Max USB retry attempts per op (default 3)")
    ap.add_argument(
        "--verify",
        dest="verify",
        action="store_true",
        default=True,
        help="Verify written ranges with hardware CRC-16 + byte readback (default: enabled)",
    )
    ap.add_argument("--no-verify", dest="verify", action="store_false", help="Skip readback verification")
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Print plan without touching USB (DEFAULT unless --execute is passed)",
    )
    ap.add_argument(
        "--execute",
        action="store_true",
        help="Actually talk to BootROM 301a:2800 and erase/program flash",
    )
    ap.add_argument(
        "--reboot",
        action="store_true",
        help="After flashing with --execute, reboot out of 301a:2800 into FIRM via RUNRAM",
    )
    ap.add_argument(
        "--reboot-only",
        action="store_true",
        help="Connect to BootROM 301a:2800 and reboot into FIRM without writing flash",
    )
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if args.reboot_only:
        client = open_bootrom_client(vid=VID, pid=PID_BOOTLOADER)
        try:
            client.reboot_chip()
            print("Triggered PMU full-chip reset from BootROM 301a:2800 -> FIRM.")
        finally:
            client.close()
        return 0

    if args.diff:
        old_path, new_path = args.diff
        if not old_path.is_file() or not new_path.is_file():
            ap.error(f"Missing input binary in --diff: {old_path}, {new_path}")
        old_data = old_path.read_bytes()
        new_data = new_path.read_bytes()
        ranges = diff_sector_ranges(old_data, new_data)
    elif args.image and args.range:
        if not args.image.is_file():
            ap.error(f"Image not found: {args.image}")
        new_data = args.image.read_bytes()
        raw_ranges = [parse_range_spec(r) for r in args.range]
        ranges = align_and_merge_ranges(raw_ranges)
    else:
        ap.error("Specify --self-test, --reboot-only, --diff OLD NEW, or --image FILE --range OFF:LEN")

    dry_run = args.dry_run or not args.execute
    client: BootromFlashClient | None = None
    try:
        if not dry_run:
            client = open_bootrom_client(vid=VID, pid=PID_BOOTLOADER)
        execute_flash_plan(
            client,
            new_data,
            ranges,
            chunk=args.chunk,
            retries=args.retries,
            verify=args.verify,
            dry_run=dry_run,
        )
        if not dry_run and args.reboot and client is not None:
            client.reboot_chip()
            print("Triggered PMU full-chip reset from BootROM 301a:2800 -> FIRM.")
    finally:
        if client is not None:
            client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
