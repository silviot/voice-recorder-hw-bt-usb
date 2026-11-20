#!/usr/bin/env python3
"""
Zero-dependency USB host utility for Cassini OpenFW (VID=0x1209, PID=0x0001).

Communicates over USB Mass Storage Bulk-Only Transport (BOT) using OpenFW's
vendor SCSI 0xE0 subcommands:
  - 0xE0 0x01 GET_INFO   : device telemetry & SRAM log ring metadata (32 B IN)
  - 0xE0 0x02 READ_LOG   : read chunk (<= 64 B) from circular g_openfw_log (IN)
  - 0xE0 0x03 GET_RTC    : read AON hardware RTC datetime (8 B IN)
  - 0xE0 0x04 SET_RTC    : set AON hardware RTC datetime (8 B OUT)
  - 0xE0 0x05 EXP_WRITE  : write <= 64 B into 32 KiB SRAM experiment arena (OUT)
  - 0xE0 0x06 EXP_RUN    : execute Thumb-2 payload at entry_off with arg (32 B IN)
  - 0xE0 0x07 MEM_READ   : read 1..64 B from target memory / MMIO address (IN)
  - 0xE0 0x08 REBOOT     : deferred reboot (0=normal, 1=dfu, 2=ota, 3=poweroff)
  - 0xE0 0x09 EXP_STATUS : read last experiment result (32 B IN)
  - 0xE0 0x0A LOG_RESET  : reset g_openfw_log ring buffer

Transport selection order (zero external dependencies):
  1. Linux `/dev/sg*` SG_IO (keeps mounted `/dev/sda1` exFAT intact)
  2. `ctypes` `libusb-1.0.so.0`
  3. Optional `pyusb` fallback

Examples:
  python3 tools/openfw_usb.py --self-test
  python3 tools/openfw_usb.py --info --log
  python3 tools/openfw_usb.py --rtc-sync
  python3 tools/openfw_usb.py --exp-upload /tmp/exp.bin --exp-run --log
  python3 tools/openfw_usb.py --mem-read 0x00858000 256 --mem-out /tmp/out.bin
  python3 tools/openfw_usb.py --reboot dfu

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import fcntl
import os
from pathlib import Path
import struct
import sys
from dataclasses import dataclass

VID = 0x1209
PID = 0x0001

CBW_SIGNATURE = 0x43425355  # "USBC" LE
CSW_SIGNATURE = 0x53425355  # "USBS" LE

SCSI_OP_VENDOR_E0 = 0xE0
SUBCMD_GET_INFO = 0x01
SUBCMD_READ_LOG = 0x02
SUBCMD_GET_RTC = 0x03
SUBCMD_SET_RTC = 0x04
SUBCMD_EXP_WRITE = 0x05
SUBCMD_EXP_RUN = 0x06
SUBCMD_MEM_READ = 0x07
SUBCMD_REBOOT = 0x08
SUBCMD_EXP_STATUS = 0x09
SUBCMD_LOG_RESET = 0x0A
SUBCMD_AUDIO_CFG = 0x0B

REBOOT_MODES = {
    "normal": 0,
    "dfu": 1,
    "ota": 2,
    "poweroff": 3,
}

INFO_MAGIC = b"OFW1"
INFO_STRUCT = struct.Struct("<4sIIBBBBIIII")  # 32 bytes
EXP_RES_MAGIC = b"EXR1"
EXP_RES_STRUCT = struct.Struct("<4s7I")      # 32 bytes
RTC_STRUCT = struct.Struct("<HBBBBBB")       # 8 bytes
AUDIO_CFG_STRUCT = struct.Struct("<BBBBhH")  # 8 bytes
MAX_VENDOR_CHUNK = 64

DSP_PROFILE_NAMES = {
    0: "ASR_RAW",
    1: "VOICE_CLARITY",
    2: "BYPASS",
}

CHG_STATE_NAMES = {
    0: "DISCHARGING",
    1: "CHARGING",
    2: "FULL",
}


@dataclass(frozen=True)
class OpenFwInfo:
    magic: bytes
    uptime_ms: int
    vbat_mv: int
    chg_state: int
    sd_mounted: int
    rec_active: int
    mic_gain: int
    log_write_idx: int
    log_dropped: int
    log_size: int
    reserved: int


@dataclass(frozen=True)
class ExpResult:
    magic: bytes
    rc: int
    elapsed_ms: int
    out_len: int
    log_widx: int
    run_count: int
    arena_crc_and_size: int
    arena_base: int

    @property
    def arena_crc(self) -> int:
        return (self.arena_crc_and_size >> 16) & 0xFFFF

    @property
    def arena_loaded(self) -> int:
        return self.arena_crc_and_size & 0xFFFF


@dataclass(frozen=True)
class AudioConfig:
    dsp_profile: int
    mic_gain: int
    channels: int
    vor_enabled: int
    rms_dbfs_x10: int
    clip_events: int

    @property
    def rms_dbfs(self) -> float:
        return self.rms_dbfs_x10 / 10.0


def unpack_audio_cfg(raw: bytes) -> AudioConfig:
    if len(raw) < AUDIO_CFG_STRUCT.size:
        raise ValueError(f"Expected {AUDIO_CFG_STRUCT.size} B for AUDIO_CFG, got {len(raw)}")
    return AudioConfig(*AUDIO_CFG_STRUCT.unpack_from(raw, 0))


def unpack_info(raw: bytes) -> OpenFwInfo:
    if len(raw) < INFO_STRUCT.size:
        raise ValueError(f"Expected {INFO_STRUCT.size} B for GET_INFO, got {len(raw)}")
    fields = INFO_STRUCT.unpack_from(raw, 0)
    info = OpenFwInfo(*fields)
    if info.magic != INFO_MAGIC:
        raise ValueError(f"Invalid GET_INFO magic {info.magic!r} (expected {INFO_MAGIC!r})")
    return info


def unpack_exp_result(raw: bytes) -> ExpResult:
    if len(raw) < EXP_RES_STRUCT.size:
        raise ValueError(f"Expected {EXP_RES_STRUCT.size} B for EXP_RESULT, got {len(raw)}")
    fields = EXP_RES_STRUCT.unpack_from(raw, 0)
    res = ExpResult(*fields)
    if res.magic != EXP_RES_MAGIC:
        raise ValueError(f"Invalid EXP_RESULT magic {res.magic!r} (expected {EXP_RES_MAGIC!r})")
    return res


def pack_rtc(dt: datetime.datetime) -> bytes:
    """Pack datetime into OpenFW 8-byte <HBBBBBB wire format."""
    if not (2000 <= dt.year <= 2127):
        raise ValueError(f"Year {dt.year} out of OpenFW AON RTC range (2000..2127)")
    return RTC_STRUCT.pack(
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        dt.weekday(),
    )


def unpack_rtc(raw: bytes) -> tuple[datetime.datetime, int]:
    """Unpack OpenFW 8-byte <HBBBBBB wire payload into (datetime, weekday)."""
    if len(raw) < RTC_STRUCT.size:
        raise ValueError(f"Expected {RTC_STRUCT.size} B for GET_RTC, got {len(raw)}")
    year, month, day, hour, minute, second, weekday = RTC_STRUCT.unpack_from(raw, 0)
    return datetime.datetime(year, month, day, hour, minute, second), weekday


class BulkOnlyTransport:
    """Abstract USB Mass Storage Bulk-Only Transport exchange layer."""

    def exchange(
        self, cbw: bytes, data_out: bytes = b"", in_len: int = 0, timeout_ms: int | None = None
    ) -> tuple[bytes, bytes]:
        raise NotImplementedError

    def close(self) -> None:
        pass


class _SgIoHdr(ctypes.Structure):
    _fields_ = [
        ("interface_id", ctypes.c_int),
        ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte),
        ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_ushort),
        ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p),
        ("cmdp", ctypes.c_void_p),
        ("sbp", ctypes.c_void_p),
        ("timeout", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p),
        ("status", ctypes.c_ubyte),
        ("masked_status", ctypes.c_ubyte),
        ("msg_status", ctypes.c_ubyte),
        ("sb_len_wr", ctypes.c_ubyte),
        ("host_status", ctypes.c_ushort),
        ("driver_status", ctypes.c_ushort),
        ("resid", ctypes.c_int),
        ("duration", ctypes.c_uint),
        ("info", ctypes.c_uint),
    ]


def find_sg_device(vid: int = VID, pid: int = PID) -> Path | None:
    """Locate `/dev/sg*` matching USB VID/PID via sysfs without unmounting storage."""
    sys_sg = Path("/sys/class/scsi_generic")
    if not sys_sg.is_dir():
        return None
    want_vid = f"{vid:04x}"
    want_pid = f"{pid:04x}"
    for sg_dir in sorted(sys_sg.glob("sg*")):
        dev_link = sg_dir / "device"
        if not dev_link.exists():
            continue
        cur = dev_link.resolve()
        while cur != cur.parent:
            vf = cur / "idVendor"
            pf = cur / "idProduct"
            if vf.is_file() and pf.is_file():
                try:
                    got_v = vf.read_text(encoding="ascii").strip().lower()
                    got_p = pf.read_text(encoding="ascii").strip().lower()
                    if got_v == want_vid and got_p == want_pid:
                        dev_node = Path("/dev") / sg_dir.name
                        if dev_node.exists():
                            return dev_node
                except OSError:
                    pass
                break
            cur = cur.parent
    return None


class SgIoBulkTransport(BulkOnlyTransport):
    """Zero-dependency Linux `/dev/sg*` SG_IO BOT transport (keeps disk mounted)."""

    SG_IO = 0x2285
    SG_DXFER_NONE = -1
    SG_DXFER_TO_DEV = -2
    SG_DXFER_FROM_DEV = -3

    def __init__(self, vid: int = VID, pid: int = PID, timeout_ms: int = 5000) -> None:
        self._timeout_ms = timeout_ms
        dev_node = find_sg_device(vid=vid, pid=pid)
        if dev_node is None:
            raise FileNotFoundError(f"No /dev/sg* node for USB {vid:04x}:{pid:04x}")
        self.dev_path = dev_node
        self.fd = os.open(str(dev_node), os.O_RDWR)

    def close(self) -> None:
        if getattr(self, "fd", -1) >= 0:
            os.close(self.fd)
            self.fd = -1

    def exchange(
        self, cbw: bytes, data_out: bytes = b"", in_len: int = 0, timeout_ms: int | None = None
    ) -> tuple[bytes, bytes]:
        _, tag, xfer_len, _, _, cb_len = struct.unpack_from("<IIIBBB", cbw, 0)
        cdb = cbw[15 : 15 + cb_len]
        cbuf = (ctypes.c_ubyte * cb_len)(*cdb)
        sbuf = (ctypes.c_ubyte * 32)()

        if data_out:
            direction = self.SG_DXFER_TO_DEV
            dbuf = (ctypes.c_ubyte * len(data_out)).from_buffer_copy(data_out)
            dxfer_len = len(data_out)
            dxferp = ctypes.addressof(dbuf)
        elif in_len > 0:
            direction = self.SG_DXFER_FROM_DEV
            dbuf = (ctypes.c_ubyte * in_len)()
            dxfer_len = in_len
            dxferp = ctypes.addressof(dbuf)
        else:
            direction = self.SG_DXFER_NONE
            dbuf = None
            dxfer_len = 0
            dxferp = 0

        hdr = _SgIoHdr(
            interface_id=ord("S"),
            dxfer_direction=direction,
            cmd_len=cb_len,
            mx_sb_len=32,
            dxfer_len=dxfer_len,
            dxferp=dxferp,
            cmdp=ctypes.addressof(cbuf),
            sbp=ctypes.addressof(sbuf),
            timeout=timeout_ms if timeout_ms is not None else self._timeout_ms,
        )
        fcntl.ioctl(self.fd, self.SG_IO, hdr)

        ok = hdr.status == 0 and hdr.host_status == 0 and hdr.driver_status == 0
        resid = max(0, int(hdr.resid)) if xfer_len > 0 else 0
        data_in = b""
        if in_len > 0 and dbuf is not None:
            got_len = max(0, min(in_len, in_len - resid))
            data_in = bytes(dbuf[:got_len])

        csw = struct.pack("<IIIB", CSW_SIGNATURE, tag, resid, 0 if ok else 1)
        return data_in, csw


class LibUsbCtypesBulkTransport(BulkOnlyTransport):
    """Zero-dependency libusb-1.0 ctypes BOT transport (fallback if sg unavailable)."""

    def __init__(self, vid: int = VID, pid: int = PID, timeout_ms: int = 5000) -> None:
        self._timeout_ms = timeout_ms
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
            raise FileNotFoundError(
                f"Cassini OpenFW ({vid:04x}:{pid:04x}) not found or permission denied."
            )

        self._reattach = False
        if lib.libusb_kernel_driver_active(self.handle, 0) == 1:
            rc = lib.libusb_detach_kernel_driver(self.handle, 0)
            if rc != 0:
                lib.libusb_close(self.handle)
                lib.libusb_exit(self.ctx)
                raise PermissionError(f"libusb_detach_kernel_driver failed ({rc}); try sudo")
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
            self.lib.libusb_release_interface(self.handle, 0)
            if self._reattach:
                self.lib.libusb_attach_kernel_driver(self.handle, 0)
            self.lib.libusb_close(self.handle)
            self.handle = None
        if getattr(self, "ctx", None):
            self.lib.libusb_exit(self.ctx)
            self.ctx = None

    def _bulk_write(self, data: bytes, timeout_ms: int) -> None:
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        actual = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(
            self.handle, 0x01, ctypes.cast(buf, ctypes.c_void_p), len(data), ctypes.byref(actual), timeout_ms
        )
        if rc != 0 or actual.value != len(data):
            raise IOError(f"libusb bulk OUT failed: rc={rc} sent={actual.value}/{len(data)}")

    def _bulk_read(self, n: int, timeout_ms: int) -> bytes:
        buf = (ctypes.c_ubyte * n)()
        actual = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(
            self.handle, 0x81, ctypes.cast(buf, ctypes.c_void_p), n, ctypes.byref(actual), timeout_ms
        )
        if rc != 0:
            raise IOError(f"libusb bulk IN failed: rc={rc} got={actual.value}/{n}")
        return bytes(buf[: actual.value])

    def exchange(
        self, cbw: bytes, data_out: bytes = b"", in_len: int = 0, timeout_ms: int | None = None
    ) -> tuple[bytes, bytes]:
        tmo = timeout_ms if timeout_ms is not None else self._timeout_ms
        self._bulk_write(cbw, tmo)
        data_in = b""
        if data_out:
            self._bulk_write(data_out, tmo)
        elif in_len > 0:
            data_in = self._bulk_read(in_len, tmo)
        csw = self._bulk_read(13, tmo)
        return data_in, csw


class PyUsbBulkTransport(BulkOnlyTransport):
    """Optional pyusb BOT transport targeting Cassini OpenFW (1209:0001)."""

    def __init__(self, vid: int = VID, pid: int = PID, timeout_ms: int = 5000) -> None:
        try:
            import usb.core
            import usb.util
        except ImportError as exc:
            raise RuntimeError("pyusb not installed") from exc

        self._usb_util = usb.util
        self._timeout_ms = timeout_ms
        self.dev = usb.core.find(idVendor=vid, idProduct=pid)
        if self.dev is None:
            raise FileNotFoundError(f"Cassini OpenFW ({vid:04x}:{pid:04x}) not found.")

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
        if self.ep_out is None or self.ep_in is None:
            raise RuntimeError("Missing Bulk IN/OUT endpoints on OpenFW USB interface 0")

    def close(self) -> None:
        self._usb_util.release_interface(self.dev, 0)
        if self._reattach:
            try:
                self.dev.attach_kernel_driver(0)
            except Exception:
                pass

    def exchange(
        self, cbw: bytes, data_out: bytes = b"", in_len: int = 0, timeout_ms: int | None = None
    ) -> tuple[bytes, bytes]:
        tmo = timeout_ms if timeout_ms is not None else self._timeout_ms
        self.ep_out.write(cbw, timeout=tmo)
        data_in = b""
        if data_out:
            self.ep_out.write(data_out, timeout=tmo)
        elif in_len > 0:
            data_in = bytes(self.ep_in.read(in_len, timeout=tmo))
        csw = bytes(self.ep_in.read(13, timeout=tmo))
        return data_in, csw


def open_live_transport(vid: int = VID, pid: int = PID, timeout_ms: int = 5000) -> BulkOnlyTransport:
    """Open live OpenFW transport: prefer SG_IO (non-destructive), then libusb, then pyusb."""
    try:
        return SgIoBulkTransport(vid=vid, pid=pid, timeout_ms=timeout_ms)
    except PermissionError as exc:
        raise SystemExit(
            f"Permission denied opening OpenFW SCSI generic device ({exc}). Re-run with sudo."
        ) from exc
    except FileNotFoundError:
        pass
    except Exception:
        pass

    for cls in (LibUsbCtypesBulkTransport, PyUsbBulkTransport):
        try:
            return cls(vid=vid, pid=pid, timeout_ms=timeout_ms)
        except PermissionError as exc:
            raise SystemExit(
                f"Permission denied accessing USB device {vid:04x}:{pid:04x} ({exc}). Re-run with sudo."
            ) from exc
        except (FileNotFoundError, RuntimeError):
            continue

    raise SystemExit(
        f"Cassini OpenFW ({vid:04x}:{pid:04x}) not found. "
        "Plug in via USB and press ENTER on the recorder to enter USB MSC mode."
    )


class OpenFwUsbClient:
    """High-level client for OpenFW vendor SCSI 0xE0 subcommands."""

    def __init__(self, transport: BulkOnlyTransport, default_timeout_ms: int = 5000) -> None:
        self.transport = transport
        self._default_timeout_ms = default_timeout_ms
        self._tag = 1

    def close(self) -> None:
        self.transport.close()

    def _vendor_e0(
        self,
        subcmd: int,
        arg1: int = 0,
        arg2: int = 0,
        data_out: bytes = b"",
        in_len: int = 0,
        timeout_ms: int | None = None,
    ) -> bytes:
        tag = self._tag
        self._tag += 1
        direction = 0x80 if in_len > 0 else 0x00
        xfer_len = in_len if in_len > 0 else len(data_out)
        cdb = struct.pack("<BBII6x", SCSI_OP_VENDOR_E0, subcmd, arg1 & 0xFFFFFFFF, arg2 & 0xFFFFFFFF)
        cbw = struct.pack("<IIIBBB", CBW_SIGNATURE, tag, xfer_len, direction, 0, 16) + cdb

        tmo = timeout_ms if timeout_ms is not None else self._default_timeout_ms
        data_in, csw = self.transport.exchange(cbw, data_out=data_out, in_len=in_len, timeout_ms=tmo)
        if len(csw) != 13:
            raise RuntimeError(f"Expected 13-byte CSW, got {len(csw)} bytes")
        sig, rtag, residue, status = struct.unpack("<IIIB", csw)
        if sig != CSW_SIGNATURE or rtag != tag or status != 0:
            raise RuntimeError(
                f"CSW error on subcmd 0x{subcmd:02X}: sig=0x{sig:08X} tag={rtag} "
                f"residue={residue} status={status}"
            )
        return data_in

    def get_info(self) -> OpenFwInfo:
        raw = self._vendor_e0(SUBCMD_GET_INFO, in_len=INFO_STRUCT.size)
        return unpack_info(raw)

    def read_log_chunk(self, req_len: int, ring_offset: int) -> bytes:
        if not (1 <= req_len <= MAX_VENDOR_CHUNK):
            raise ValueError(f"READ_LOG req_len={req_len} must be in 1..{MAX_VENDOR_CHUNK}")
        if ring_offset < 0:
            raise ValueError(f"Negative ring_offset: {ring_offset}")
        return self._vendor_e0(SUBCMD_READ_LOG, arg1=ring_offset, arg2=req_len, in_len=req_len)

    def read_log(self, info: OpenFwInfo | None = None) -> bytes:
        """Reconstruct chronological g_openfw_log payload across circular wraparound."""
        if info is None:
            info = self.get_info()
        if info.log_size == 0 or info.log_write_idx == 0:
            return b""

        if info.log_write_idx <= info.log_size:
            segments = [(0, info.log_write_idx)]
        else:
            start = info.log_write_idx % info.log_size
            segments = [(start, info.log_size - start)]
            if start > 0:
                segments.append((0, start))

        out = bytearray()
        for seg_off, seg_len in segments:
            pos = 0
            while pos < seg_len:
                req_len = min(MAX_VENDOR_CHUNK, seg_len - pos)
                chunk = self.read_log_chunk(req_len, seg_off + pos)
                if not chunk:
                    raise RuntimeError(
                        f"Empty READ_LOG response at offset {seg_off + pos} (requested {req_len} B)"
                    )
                out.extend(chunk)
                pos += len(chunk)
        return bytes(out)

    def get_rtc(self) -> tuple[datetime.datetime, int]:
        raw = self._vendor_e0(SUBCMD_GET_RTC, in_len=RTC_STRUCT.size)
        return unpack_rtc(raw)

    def set_rtc(self, dt: datetime.datetime) -> None:
        self._vendor_e0(SUBCMD_SET_RTC, data_out=pack_rtc(dt))

    def exp_write(self, data: bytes, base_offset: int = 0) -> None:
        if not data:
            raise ValueError("Cannot upload empty experiment binary")
        if base_offset < 0:
            raise ValueError(f"Negative base_offset: {base_offset}")
        pos = 0
        while pos < len(data):
            chunk = data[pos : pos + MAX_VENDOR_CHUNK]
            self._vendor_e0(
                SUBCMD_EXP_WRITE,
                arg1=base_offset + pos,
                arg2=len(chunk),
                data_out=chunk,
            )
            pos += len(chunk)

    def exp_run(
        self, entry_off: int = 0, arg: int = 0, timeout_ms: int = 20000
    ) -> ExpResult:
        raw = self._vendor_e0(
            SUBCMD_EXP_RUN,
            arg1=entry_off,
            arg2=arg,
            in_len=EXP_RES_STRUCT.size,
            timeout_ms=timeout_ms,
        )
        return unpack_exp_result(raw)

    def mem_read(self, addr: int, length: int) -> bytes:
        if length <= 0:
            return b""
        out = bytearray()
        pos = 0
        while pos < length:
            req = min(MAX_VENDOR_CHUNK, length - pos)
            chunk = self._vendor_e0(SUBCMD_MEM_READ, arg1=addr + pos, arg2=req, in_len=req)
            if len(chunk) != req:
                raise RuntimeError(
                    f"MEM_READ short read at 0x{addr + pos:08X}: got {len(chunk)}/{req} B"
                )
            out.extend(chunk)
            pos += req
        return bytes(out)

    def reboot(self, mode: int = 0) -> None:
        if not (0 <= mode <= 3):
            raise ValueError(f"Invalid reboot mode {mode} (expected 0..3)")
        self._vendor_e0(SUBCMD_REBOOT, arg1=mode)

    def exp_status(self) -> ExpResult:
        raw = self._vendor_e0(SUBCMD_EXP_STATUS, in_len=EXP_RES_STRUCT.size)
        return unpack_exp_result(raw)

    def log_reset(self) -> None:
        self._vendor_e0(SUBCMD_LOG_RESET)

    def get_audio_cfg(self) -> AudioConfig:
        raw = self._vendor_e0(SUBCMD_AUDIO_CFG, in_len=AUDIO_CFG_STRUCT.size)
        return unpack_audio_cfg(raw)

    def set_audio_cfg(
        self,
        *,
        dsp_profile: int | None = None,
        mic_gain: int | None = None,
        channels: int | None = None,
        vor_enabled: int | None = None,
    ) -> AudioConfig:
        if dsp_profile is not None and not (0 <= dsp_profile <= 2):
            raise ValueError(f"Invalid dsp_profile {dsp_profile} (expected 0..2)")
        if mic_gain is not None and not (0 <= mic_gain <= 31):
            raise ValueError(f"Invalid mic_gain {mic_gain} (expected 0..31)")
        if channels is not None and channels not in (1, 2):
            raise ValueError(f"Invalid channels {channels} (expected 1 or 2)")
        if vor_enabled is not None and vor_enabled not in (0, 1):
            raise ValueError(f"Invalid vor_enabled {vor_enabled} (expected 0 or 1)")
        payload = bytes((
            0xFF if dsp_profile is None else dsp_profile,
            0xFF if mic_gain is None else mic_gain,
            0xFF if channels is None else channels,
            0xFF if vor_enabled is None else vor_enabled,
        ))
        self._vendor_e0(SUBCMD_AUDIO_CFG, data_out=payload)
        return self.get_audio_cfg()


class _MockOpenFwTransport(BulkOnlyTransport):
    """In-memory USB MSC BOT simulator for all OpenFW 0xE0 vendor subcommands."""

    def __init__(self, log_size: int = 4096) -> None:
        self.uptime_ms = 123456
        self.vbat_mv = 4075
        self.chg_state = 1
        self.sd_mounted = 1
        self.rec_active = 0
        self.mic_gain = 20
        self.log_size = log_size
        self.log_buf = bytearray(log_size)
        self.log_write_idx = 0
        self.log_dropped = 3
        self.reserved = 0
        self.dsp_profile = 0
        self.channels = 1
        self.vor_enabled = 0
        self.rms_dbfs_x10 = -245
        self.clip_events = 2
        self.rtc_bytes = bytearray(pack_rtc(datetime.datetime(2026, 1, 1, 0, 0, 0)))
        self.arena = bytearray(32768)
        self.out_buf = bytearray(16384)
        self.last_res = EXP_RES_STRUCT.pack(EXP_RES_MAGIC, 0, 0, 0, 0, 0, 0, 0x00850000)
        self.loaded_size = 0
        self.run_count = 0
        self.last_reboot_mode: int | None = None

    def push_log(self, data: bytes) -> None:
        for b in data:
            self.log_buf[self.log_write_idx % self.log_size] = b
            self.log_write_idx += 1

    def exchange(
        self, cbw: bytes, data_out: bytes = b"", in_len: int = 0, timeout_ms: int | None = None
    ) -> tuple[bytes, bytes]:
        (void_tmo := timeout_ms)
        del void_tmo
        if len(cbw) != 31:
            raise AssertionError(f"Expected 31-byte CBW, got {len(cbw)}")
        sig, tag, xfer_len, flags, lun, cb_len = struct.unpack_from("<IIIBBB", cbw, 0)
        assert sig == CBW_SIGNATURE
        assert lun == 0 and 1 <= cb_len <= 16
        cb = cbw[15:31]
        assert cb[0] == SCSI_OP_VENDOR_E0
        subcmd = cb[1]
        arg1, arg2 = struct.unpack_from("<II", cb, 2)

        data_in = b""
        residue = 0
        status = 0

        if subcmd == SUBCMD_GET_INFO:
            assert (flags & 0x80) != 0 and xfer_len == INFO_STRUCT.size and in_len == INFO_STRUCT.size
            data_in = INFO_STRUCT.pack(
                INFO_MAGIC,
                self.uptime_ms,
                self.vbat_mv,
                self.chg_state,
                self.sd_mounted,
                self.rec_active,
                self.mic_gain,
                self.log_write_idx,
                self.log_dropped,
                self.log_size,
                self.reserved,
            )
        elif subcmd == SUBCMD_READ_LOG:
            assert (flags & 0x80) != 0
            offset, req = arg1, arg2
            assert 1 <= req <= MAX_VENDOR_CHUNK
            assert xfer_len == req and in_len == req
            data_in = bytes(self.log_buf[(offset + i) % self.log_size] for i in range(req))
        elif subcmd == SUBCMD_GET_RTC:
            assert (flags & 0x80) != 0 and xfer_len == RTC_STRUCT.size and in_len == RTC_STRUCT.size
            data_in = bytes(self.rtc_bytes)
        elif subcmd == SUBCMD_SET_RTC:
            assert (flags & 0x80) == 0 and xfer_len == RTC_STRUCT.size and len(data_out) == RTC_STRUCT.size
            unpack_rtc(data_out)
            self.rtc_bytes = bytearray(data_out)
        elif subcmd == SUBCMD_EXP_WRITE:
            assert (flags & 0x80) == 0 and xfer_len == arg2 and len(data_out) == arg2
            if arg1 + arg2 <= len(self.arena):
                if arg1 == 0:
                    self.arena[:] = b"\x00" * len(self.arena)
                    self.loaded_size = 0
                self.arena[arg1 : arg1 + arg2] = data_out
                self.loaded_size = max(self.loaded_size, arg1 + arg2)
            else:
                status = 1
        elif subcmd == SUBCMD_EXP_RUN:
            assert (flags & 0x80) != 0 and xfer_len == EXP_RES_STRUCT.size and in_len == EXP_RES_STRUCT.size
            self.run_count += 1
            msg = f"MOCK RUN entry={arg1} arg={arg2}\n".encode("ascii")
            self.push_log(msg)
            self.out_buf[0:12] = struct.pack("<III", arg1, arg2, 0xCAFEBABE)
            self.last_res = EXP_RES_STRUCT.pack(
                EXP_RES_MAGIC,
                arg2 ^ 0x42,
                17,
                12,
                self.log_write_idx,
                self.run_count,
                (0x1234 << 16) | (self.loaded_size & 0xFFFF),
                0x00850000,
            )
            data_in = self.last_res
        elif subcmd == SUBCMD_MEM_READ:
            assert (flags & 0x80) != 0 and 1 <= arg2 <= MAX_VENDOR_CHUNK
            assert xfer_len == arg2 and in_len == arg2
            if 0x00850000 <= arg1 and arg1 + arg2 <= 0x00850000 + len(self.arena):
                off = arg1 - 0x00850000
                data_in = bytes(self.arena[off : off + arg2])
            elif 0x00858000 <= arg1 and arg1 + arg2 <= 0x00858000 + len(self.out_buf):
                off = arg1 - 0x00858000
                data_in = bytes(self.out_buf[off : off + arg2])
            else:
                data_in = b"\x00" * arg2
        elif subcmd == SUBCMD_REBOOT:
            assert (flags & 0x80) == 0 and xfer_len == 0 and 0 <= arg1 <= 3
            self.last_reboot_mode = arg1
        elif subcmd == SUBCMD_EXP_STATUS:
            assert (flags & 0x80) != 0 and xfer_len == EXP_RES_STRUCT.size and in_len == EXP_RES_STRUCT.size
            data_in = self.last_res
        elif subcmd == SUBCMD_LOG_RESET:
            assert (flags & 0x80) == 0 and xfer_len == 0
            self.log_buf[:] = b"\x00" * self.log_size
            self.log_write_idx = 0
            self.log_dropped = 0
        elif subcmd == SUBCMD_AUDIO_CFG:
            if (flags & 0x80) == 0:
                assert xfer_len == 4 and len(data_out) == 4
                p, g, ch, vor = data_out
                valid = (
                    (p == 0xFF or p <= 2)
                    and (g == 0xFF or g <= 31)
                    and (ch == 0xFF or ch in (1, 2))
                    and (vor == 0xFF or vor <= 1)
                )
                if valid:
                    if p != 0xFF:
                        self.dsp_profile = p
                    if g != 0xFF:
                        self.mic_gain = g
                    if ch != 0xFF:
                        self.channels = ch
                    if vor != 0xFF:
                        self.vor_enabled = vor
                else:
                    status = 1
            else:
                assert xfer_len == AUDIO_CFG_STRUCT.size and in_len == AUDIO_CFG_STRUCT.size
                data_in = AUDIO_CFG_STRUCT.pack(
                    self.dsp_profile,
                    self.mic_gain,
                    self.channels,
                    self.vor_enabled,
                    self.rms_dbfs_x10,
                    self.clip_events,
                )
        else:
            status = 1
            residue = xfer_len

        csw = struct.pack("<IIIB", CSW_SIGNATURE, tag, residue, status)
        return data_in, csw


def run_self_test() -> int:
    """Test all 10 0xE0 subcommands + wrapped log ring reconstruction via mock BOT."""
    mock = _MockOpenFwTransport(log_size=4096)
    client = OpenFwUsbClient(mock)

    # 1. Pre-wrap log (185 bytes -> 3 chunks: 64 + 64 + 57)
    pre_wrap = b"=== Cassini OpenFW boot ===\n" + (b"0123456789ABCDEF" * 9) + b"TAIL!\n"
    mock.push_log(pre_wrap)
    info1 = client.get_info()
    assert info1.magic == INFO_MAGIC
    assert info1.uptime_ms == 123456
    assert info1.vbat_mv == 4075
    assert info1.log_write_idx == len(pre_wrap)
    assert client.read_log(info1) == pre_wrap

    # 2. Wrap circular ring across 4096 boundary with non-aligned start offset
    extra_lines = bytearray()
    for i in range(160):
        extra_lines.extend(f"[line {i:03d}] tick={i * 25} vbat=4075\n".encode("ascii"))
    mock.push_log(bytes(extra_lines))
    combined = pre_wrap + bytes(extra_lines)
    assert len(combined) > 4096
    info2 = client.get_info()
    assert client.read_log(info2) == combined[-4096:]

    # 3. GET_RTC & SET_RTC round-trips
    dt0, wd0 = client.get_rtc()
    assert dt0 == datetime.datetime(2026, 1, 1, 0, 0, 0) and wd0 == 3
    target = datetime.datetime(2026, 11, 19, 21, 45, 30)
    client.set_rtc(target)
    rb, wd = client.get_rtc()
    assert rb == target and wd == target.weekday()

    # 4. LOG_RESET, EXP_WRITE (140 B -> 3 chunks), EXP_RUN, EXP_STATUS, MEM_READ, REBOOT
    client.log_reset()
    assert client.get_info().log_write_idx == 0

    payload = bytes((i * 13) & 0xFF for i in range(140))
    client.exp_write(payload, base_offset=0)
    assert client.mem_read(0x00850000, len(payload)) == payload

    res = client.exp_run(entry_off=0, arg=0x100)
    assert res.magic == EXP_RES_MAGIC
    assert res.rc == (0x100 ^ 0x42)
    assert res.out_len == 12
    assert res.arena_loaded == 140
    assert res.run_count == 1
    assert client.exp_status() == res
    out_bytes = client.mem_read(0x00858000, res.out_len)
    assert struct.unpack("<III", out_bytes) == (0, 0x100, 0xCAFEBABE)
    assert b"MOCK RUN entry=0 arg=256\n" in client.read_log()

    client.reboot(REBOOT_MODES["dfu"])
    assert mock.last_reboot_mode == 1

    # 5. AUDIO_CFG GET/SET (0x0B)
    cfg0 = client.get_audio_cfg()
    assert cfg0.dsp_profile == 0 and cfg0.channels == 1 and cfg0.vor_enabled == 0
    cfg1 = client.set_audio_cfg(dsp_profile=1, mic_gain=26, channels=2, vor_enabled=1)
    assert (cfg1.dsp_profile, cfg1.mic_gain, cfg1.channels, cfg1.vor_enabled) == (1, 26, 2, 1)
    assert cfg1.rms_dbfs == -24.5 and cfg1.clip_events == 2

    print(
        "openfw_usb.py self-test PASSED "
        "(0x01..0x0B: GET_INFO, READ_LOG pre/wrapped, RTC, EXP_WRITE/RUN/STATUS, MEM_READ, REBOOT, LOG_RESET, AUDIO_CFG)"
    )
    return 0


def format_info(info: OpenFwInfo) -> str:
    chg = CHG_STATE_NAMES.get(info.chg_state, f"UNKNOWN({info.chg_state})")
    lines = [
        f"Magic:      {info.magic.decode('ascii', 'replace')}",
        f"Uptime:     {info.uptime_ms / 1000.0:.3f} s ({info.uptime_ms} ms)",
        f"Battery:    {info.vbat_mv} mV ({chg})",
        f"SD mounted: {bool(info.sd_mounted)} ({info.sd_mounted})",
        f"Recording:  {bool(info.rec_active)} (mic_gain={info.mic_gain})",
        f"Log ring:   write_idx={info.log_write_idx} size={info.log_size} dropped={info.log_dropped}",
    ]
    if info.reserved != 0:
        lines.append(f"Prev fault: 0x{info.reserved:08X}")
    return "\n".join(lines)


def format_exp_result(res: ExpResult) -> str:
    return (
        f"ExpResult:  rc={res.rc} (0x{res.rc:08X}) elapsed={res.elapsed_ms} ms "
        f"out_len={res.out_len} log_widx={res.log_widx} runs={res.run_count} "
        f"arena={res.arena_loaded}B crc=0x{res.arena_crc:04X} base=0x{res.arena_base:08X}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Cassini OpenFW USB telemetry, SRAM log, memory reader & experiment loader (1209:0001)"
    )
    ap.add_argument("--self-test", action="store_true", help="Run in-memory mock BOT test without USB")
    ap.add_argument("--info", action="store_true", help="Query device status via 0xE0 0x01 GET_INFO")
    ap.add_argument("--log", action="store_true", help="Dump circular SRAM g_openfw_log via 0xE0 0x02 READ_LOG")
    ap.add_argument("--log-reset", action="store_true", help="Clear circular SRAM log via 0xE0 0x0A LOG_RESET")
    ap.add_argument("--rtc-read", action="store_true", help="Read hardware RTC via 0xE0 0x03 GET_RTC")
    ap.add_argument("--rtc-sync", action="store_true", help="Sync hardware RTC to host local clock via 0xE0 0x04")
    ap.add_argument("--rtc-set", metavar="YYYY-MM-DD HH:MM:SS", help="Set hardware RTC via 0xE0 0x04 SET_RTC")
    ap.add_argument("--exp-upload", metavar="BIN_PATH", help="Upload Thumb-2 experiment .bin to SRAM arena (0xE0 0x05)")
    ap.add_argument("--exp-run", action="store_true", help="Execute SRAM experiment payload (0xE0 0x06)")
    ap.add_argument("--exp-arg", type=lambda s: int(s, 0), default=0, help="uint32 arg passed in r1 to exp_main")
    ap.add_argument("--exp-entry", type=lambda s: int(s, 0), default=0, help="Entry offset inside SRAM arena")
    ap.add_argument("--exp-status", action="store_true", help="Query last experiment result (0xE0 0x09)")
    ap.add_argument("--mem-read", nargs=2, metavar=("ADDR", "LEN"), help="Read LEN bytes starting at ADDR (hex/dec)")
    ap.add_argument("--mem-out", metavar="FILE", help="Write --mem-read bytes to FILE instead of hex dump")
    ap.add_argument("--audio-cfg", action="store_true", help="Query recorder DSP/gain/channels/VOR (0xE0 0x0B)")
    ap.add_argument("--set-dsp", type=int, choices=[0, 1, 2], help="Set DSP profile (0=ASR_RAW, 1=VOICE_CLARITY, 2=BYPASS)")
    ap.add_argument("--set-gain", type=int, help="Set analog mic PGA gain (0..31)")
    ap.add_argument("--set-ch", type=int, choices=[1, 2], help="Set capture channels (1=mono, 2=stereo)")
    ap.add_argument("--set-vor", type=int, choices=[0, 1], help="Enable/disable clean-room VOR (0/1)")
    ap.add_argument("--reboot", choices=sorted(REBOOT_MODES.keys()), help="Deferred target reboot via 0xE0 0x08")
    ap.add_argument("--timeout-ms", type=int, default=20000, help="USB transfer timeout in ms (default 20000)")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if args.rtc_sync and args.rtc_set is not None:
        ap.error("--rtc-sync and --rtc-set are mutually exclusive")

    did_action = any((
        args.info,
        args.log,
        args.log_reset,
        args.rtc_read,
        args.rtc_sync,
        args.rtc_set is not None,
        args.exp_upload is not None,
        args.exp_run,
        args.exp_status,
        args.mem_read is not None,
        args.audio_cfg,
        args.set_dsp is not None,
        args.set_gain is not None,
        args.set_ch is not None,
        args.set_vor is not None,
        args.reboot is not None,
    ))
    if not did_action:
        ap.error("Specify an action (--info, --log, --rtc-*, --exp-*, --mem-read, --reboot, or --self-test)")

    client = OpenFwUsbClient(
        open_live_transport(vid=VID, pid=PID, timeout_ms=args.timeout_ms),
        default_timeout_ms=args.timeout_ms,
    )
    try:
        info: OpenFwInfo | None = None
        if args.log_reset:
            client.log_reset()
            print("Log ring reset.")
        if args.exp_upload is not None:
            data = Path(args.exp_upload).read_bytes()
            client.exp_write(data, base_offset=0)
            print(f"Uploaded {len(data):,} B to SRAM arena (0x00850000).")
        if args.exp_run:
            res = client.exp_run(entry_off=args.exp_entry, arg=args.exp_arg, timeout_ms=args.timeout_ms)
            print(format_exp_result(res))
        if args.exp_status:
            print(format_exp_result(client.exp_status()))
        if args.mem_read is not None:
            addr = int(args.mem_read[0], 0)
            length = int(args.mem_read[1], 0)
            mem = client.mem_read(addr, length)
            if args.mem_out:
                Path(args.mem_out).write_bytes(mem)
                print(f"Saved {len(mem)} B from 0x{addr:08X} -> {args.mem_out}")
            else:
                for off in range(0, len(mem), 16):
                    row = mem[off : off + 16]
                    hex_s = " ".join(f"{b:02X}" for b in row)
                    asc_s = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
                    print(f"0x{addr + off:08X}: {hex_s:<47s}  {asc_s}")
        if args.info:
            info = client.get_info()
            print(format_info(info))
        if args.log:
            raw_log = client.read_log(info)
            text = raw_log.rstrip(b"\x00").decode("utf-8", errors="replace")
            print(text, end="" if text.endswith("\n") or not text else "\n")
        if args.rtc_read:
            dt, wd = client.get_rtc()
            print(f"Device RTC: {dt.strftime('%Y-%m-%d %H:%M:%S')} (weekday={wd})")
        if args.rtc_sync or args.rtc_set is not None:
            target = (
                datetime.datetime.now().replace(microsecond=0)
                if args.rtc_sync
                else datetime.datetime.strptime(args.rtc_set, "%Y-%m-%d %H:%M:%S")
            )
            client.set_rtc(target)
            rb, wd = client.get_rtc()
            print(f"RTC updated: {rb.strftime('%Y-%m-%d %H:%M:%S')} (weekday={wd})")
        if any(x is not None for x in (args.set_dsp, args.set_gain, args.set_ch, args.set_vor)):
            acfg = client.set_audio_cfg(
                dsp_profile=args.set_dsp,
                mic_gain=args.set_gain,
                channels=args.set_ch,
                vor_enabled=args.set_vor,
            )
            pname = DSP_PROFILE_NAMES.get(acfg.dsp_profile, str(acfg.dsp_profile))
            print(f"AudioCfg:   dsp={pname} gain={acfg.mic_gain}/31 ch={acfg.channels} vor={acfg.vor_enabled} rms={acfg.rms_dbfs:.1f}dBFS clips={acfg.clip_events}")
        elif args.audio_cfg:
            acfg = client.get_audio_cfg()
            pname = DSP_PROFILE_NAMES.get(acfg.dsp_profile, str(acfg.dsp_profile))
            print(f"AudioCfg:   dsp={pname} gain={acfg.mic_gain}/31 ch={acfg.channels} vor={acfg.vor_enabled} rms={acfg.rms_dbfs:.1f}dBFS clips={acfg.clip_events}")
        if args.reboot is not None:
            client.reboot(REBOOT_MODES[args.reboot])
            print(f"Scheduled reboot: {args.reboot} (mode={REBOOT_MODES[args.reboot]})")
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
