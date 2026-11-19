#!/usr/bin/env python3
"""
Clean-room USB host utility for Cassini OpenFW (VID=0x1209, PID=0x0001).

Communicates over USB Mass Storage Bulk-Only Transport (BOT) using OpenFW's
vendor SCSI 0xE0 subcommands:
  - 0xE0 0x01 GET_INFO : device telemetry & SRAM log ring metadata (32 B IN)
  - 0xE0 0x02 READ_LOG : read chunk (<= 64 B) from circular g_openfw_log (IN)
  - 0xE0 0x03 GET_RTC  : read AON hardware RTC datetime (8 B IN)
  - 0xE0 0x04 SET_RTC  : set AON hardware RTC datetime (8 B OUT)

Examples:
  python3 tools/openfw_usb.py --self-test
  python3 tools/openfw_usb.py --info
  python3 tools/openfw_usb.py --log
  python3 tools/openfw_usb.py --rtc-read
  python3 tools/openfw_usb.py --rtc-sync
  python3 tools/openfw_usb.py --rtc-set "2026-11-19 12:00:00"

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import datetime
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

INFO_MAGIC = b"OFW1"
INFO_STRUCT = struct.Struct("<4sIIBBBBIIII")  # 32 bytes
RTC_STRUCT = struct.Struct("<HBBBBBB")       # 8 bytes
READ_LOG_MAX_CHUNK = 64

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


def unpack_info(raw: bytes) -> OpenFwInfo:
    if len(raw) < INFO_STRUCT.size:
        raise ValueError(f"Expected {INFO_STRUCT.size} B for GET_INFO, got {len(raw)}")
    fields = INFO_STRUCT.unpack_from(raw, 0)
    info = OpenFwInfo(*fields)
    if info.magic != INFO_MAGIC:
        raise ValueError(f"Invalid GET_INFO magic {info.magic!r} (expected {INFO_MAGIC!r})")
    return info


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

    def exchange(self, cbw: bytes, data_out: bytes = b"", in_len: int = 0) -> tuple[bytes, bytes]:
        raise NotImplementedError

    def close(self) -> None:
        pass


class PyUsbBulkTransport(BulkOnlyTransport):
    """Live libusb/pyusb BOT transport targeting Cassini OpenFW (1209:0001)."""

    def __init__(self, vid: int = VID, pid: int = PID, timeout_ms: int = 2000) -> None:
        try:
            import usb.core
            import usb.util
        except ImportError as exc:
            raise SystemExit(
                "pyusb is required for live USB access: install with `pip install pyusb` or `apt install python3-usb`"
            ) from exc

        self._usb_util = usb.util
        self._timeout_ms = timeout_ms
        self.dev = usb.core.find(idVendor=vid, idProduct=pid)
        if self.dev is None:
            raise SystemExit(
                f"Cassini OpenFW ({vid:04x}:{pid:04x}) not found. "
                "Plug in via USB and press ENTER to enter USB MSC mode."
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
        if self.ep_out is None or self.ep_in is None:
            raise RuntimeError("Missing Bulk IN/OUT endpoints on OpenFW USB interface 0")

    def close(self) -> None:
        self._usb_util.release_interface(self.dev, 0)
        if self._reattach:
            try:
                self.dev.attach_kernel_driver(0)
            except Exception:
                pass

    def exchange(self, cbw: bytes, data_out: bytes = b"", in_len: int = 0) -> tuple[bytes, bytes]:
        self.ep_out.write(cbw, timeout=self._timeout_ms)
        data_in = b""
        if data_out:
            self.ep_out.write(data_out, timeout=self._timeout_ms)
        elif in_len > 0:
            data_in = bytes(self.ep_in.read(in_len, timeout=self._timeout_ms))
        csw = bytes(self.ep_in.read(13, timeout=self._timeout_ms))
        return data_in, csw


class OpenFwUsbClient:
    """High-level client for OpenFW vendor SCSI 0xE0 subcommands."""

    def __init__(self, transport: BulkOnlyTransport) -> None:
        self.transport = transport
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
    ) -> bytes:
        tag = self._tag
        self._tag += 1
        direction = 0x80 if in_len > 0 else 0x00
        xfer_len = in_len if in_len > 0 else len(data_out)
        cdb = struct.pack("<BBII6x", SCSI_OP_VENDOR_E0, subcmd, arg1 & 0xFFFFFFFF, arg2 & 0xFFFFFFFF)
        cbw = struct.pack("<IIIBBB", CBW_SIGNATURE, tag, xfer_len, direction, 0, 16) + cdb

        data_in, csw = self.transport.exchange(cbw, data_out=data_out, in_len=in_len)
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
        if not (1 <= req_len <= READ_LOG_MAX_CHUNK):
            raise ValueError(f"READ_LOG req_len={req_len} must be in 1..{READ_LOG_MAX_CHUNK}")
        if ring_offset < 0:
            raise ValueError(f"Negative ring_offset: {ring_offset}")
        return self._vendor_e0(SUBCMD_READ_LOG, arg1=req_len, arg2=ring_offset, in_len=req_len)

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
                req_len = min(READ_LOG_MAX_CHUNK, seg_len - pos)
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


class _MockOpenFwTransport(BulkOnlyTransport):
    """In-memory USB MSC BOT simulator for OpenFW 0xE0 vendor subcommands."""

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
        self.rtc_bytes = bytearray(pack_rtc(datetime.datetime(2026, 1, 1, 0, 0, 0)))

    def push_log(self, data: bytes) -> None:
        for b in data:
            self.log_buf[self.log_write_idx % self.log_size] = b
            self.log_write_idx += 1

    def exchange(self, cbw: bytes, data_out: bytes = b"", in_len: int = 0) -> tuple[bytes, bytes]:
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
            assert 1 <= arg1 <= READ_LOG_MAX_CHUNK
            assert arg2 < self.log_size and arg2 + arg1 <= self.log_size
            assert xfer_len == arg1 and in_len == arg1
            data_in = bytes(self.log_buf[arg2 : arg2 + arg1])
        elif subcmd == SUBCMD_GET_RTC:
            assert (flags & 0x80) != 0 and xfer_len == RTC_STRUCT.size and in_len == RTC_STRUCT.size
            data_in = bytes(self.rtc_bytes)
        elif subcmd == SUBCMD_SET_RTC:
            assert (flags & 0x80) == 0 and xfer_len == RTC_STRUCT.size and len(data_out) == RTC_STRUCT.size
            unpack_rtc(data_out)
            self.rtc_bytes = bytearray(data_out)
        else:
            status = 1
            residue = xfer_len

        csw = struct.pack("<IIIB", CSW_SIGNATURE, tag, residue, status)
        return data_in, csw


def run_self_test() -> int:
    """Test all 4 0xE0 subcommands + wrapped log ring reconstruction via mock BOT."""
    mock = _MockOpenFwTransport(log_size=4096)
    client = OpenFwUsbClient(mock)

    # 1. Pre-wrap log (185 bytes -> 3 chunks: 64 + 64 + 57)
    pre_wrap = b"=== Cassini OpenFW boot ===\n" + (b"0123456789ABCDEF" * 9) + b"TAIL!\n"
    mock.push_log(pre_wrap)
    info1 = client.get_info()
    assert info1.magic == INFO_MAGIC
    assert info1.uptime_ms == 123456
    assert info1.vbat_mv == 4075
    assert info1.chg_state == 1
    assert info1.sd_mounted == 1
    assert info1.rec_active == 0
    assert info1.mic_gain == 20
    assert info1.log_write_idx == len(pre_wrap)
    assert info1.log_dropped == 3
    assert info1.log_size == 4096
    assert client.read_log(info1) == pre_wrap

    # 2. Wrap circular ring across 4096 boundary with non-aligned start offset
    extra_lines = bytearray()
    for i in range(160):
        extra_lines.extend(f"[line {i:03d}] tick={i * 25} vbat=4075\n".encode("ascii"))
    mock.push_log(bytes(extra_lines))
    combined = pre_wrap + bytes(extra_lines)
    assert len(combined) > 4096
    expected_tail = combined[-4096:]

    info2 = client.get_info()
    assert info2.log_write_idx == len(combined)
    reconstructed = client.read_log(info2)
    assert len(reconstructed) == 4096
    assert reconstructed == expected_tail

    # 3. GET_RTC & SET_RTC round-trips
    dt0, wd0 = client.get_rtc()
    assert dt0 == datetime.datetime(2026, 1, 1, 0, 0, 0) and wd0 == 3

    for target in (
        datetime.datetime(2026, 11, 19, 21, 45, 30),
        datetime.datetime(2024, 2, 29, 23, 59, 59),
    ):
        client.set_rtc(target)
        rb, wd = client.get_rtc()
        assert rb == target and wd == target.weekday()

    print(
        "openfw_usb.py self-test PASSED "
        "(GET_INFO, READ_LOG pre-wrap & wrapped 4096B ring, GET_RTC, SET_RTC)"
    )
    return 0


def format_info(info: OpenFwInfo) -> str:
    chg = CHG_STATE_NAMES.get(info.chg_state, f"UNKNOWN({info.chg_state})")
    return "\n".join([
        f"Magic:      {info.magic.decode('ascii', 'replace')}",
        f"Uptime:     {info.uptime_ms / 1000.0:.3f} s ({info.uptime_ms} ms)",
        f"Battery:    {info.vbat_mv} mV ({chg})",
        f"SD mounted: {bool(info.sd_mounted)} ({info.sd_mounted})",
        f"Recording:  {bool(info.rec_active)} (mic_gain={info.mic_gain})",
        f"Log ring:   write_idx={info.log_write_idx} size={info.log_size} dropped={info.log_dropped}",
    ])


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Cassini OpenFW USB telemetry, SRAM log reader & RTC sync (1209:0001, SCSI 0xE0)"
    )
    ap.add_argument("--self-test", action="store_true", help="Run in-memory mock BOT test without USB")
    ap.add_argument("--info", action="store_true", help="Query device status via 0xE0 0x01 GET_INFO")
    ap.add_argument("--log", action="store_true", help="Dump circular SRAM g_openfw_log via 0xE0 0x02 READ_LOG")
    ap.add_argument("--rtc-read", action="store_true", help="Read hardware RTC via 0xE0 0x03 GET_RTC")
    ap.add_argument("--rtc-sync", action="store_true", help="Sync hardware RTC to host local clock via 0xE0 0x04")
    ap.add_argument("--rtc-set", metavar="YYYY-MM-DD HH:MM:SS", help="Set hardware RTC via 0xE0 0x04 SET_RTC")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if args.rtc_sync and args.rtc_set is not None:
        ap.error("--rtc-sync and --rtc-set are mutually exclusive")

    if not any((args.info, args.log, args.rtc_read, args.rtc_sync, args.rtc_set is not None)):
        ap.error("Specify one of --info, --log, --rtc-read, --rtc-sync, --rtc-set, or --self-test")

    client = OpenFwUsbClient(PyUsbBulkTransport(vid=VID, pid=PID))
    try:
        info: OpenFwInfo | None = None
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
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
