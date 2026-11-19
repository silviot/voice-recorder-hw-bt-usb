#!/usr/bin/env python3
"""
Zero-flash USB RTC reader & clock synchronizer for Smartlink SL6801 (HW_76, pen133_2.0.65).

Uses Boot ROM vendor SCSI 0xC0 subcommands WRITERAM (0x09) + RUNRAM (0x0A) in normal
Card Reader mode (301a:2801) to invoke FIRM's lock-free HAL_rtc_get (0x0080D4E4) and
HAL_rtc_set (0x00CE88D4) without modifying SPI NOR flash.

Requirements:
  pip install pyusb   (or system python3-usb)
  Run with sudo (or with 50-smartlink-sl6801.rules installed).

Examples:
  python3 tools/usb_rtc_sync.py --read
  python3 tools/usb_rtc_sync.py --sync
  python3 tools/usb_rtc_sync.py --set "2026-11-19 12:00:00"
"""

from __future__ import annotations

import argparse
import datetime
import struct
import sys

VID = 0x301A
PID_CARDREADER = 0x2801

# Boot ROM USB MSC buffer layout (0x00800DE8..0x00800FEF):
#   0x00800DEE: uint16_t tx_len (Bulk-IN response length)
#   0x00800DF0: uint8_t  data_buf[512] (first 16 B used for IN/OUT transfer)
#   0x00800E80: unused upper half of data_buf — safe scratch SRAM for tiny stubs
SCRATCH_ADDR = 0x00800E80
RTC_BUF_ADDR = 0x00800EA0
MSC_TX_LEN_ADDR = 0x00800DEE
MSC_DATA_BUF_ADDR = 0x00800DF0

HAL_RTC_GET_THUMB = 0x0080D4E5  # 0x0080D4E4 | 1
HAL_RTC_SET_THUMB = 0x00CE88D5  # 0x00CE88D4 | 1

# Verified Thumb-2 stub @ 0x00800E80:
#   push {r4, lr}; ldr r0, =0x800DF0; ldr r3, =0x80D4E5; blx r3;
#   ldr r1, =0x800DEE; movs r0, #10; strh r0, [r1]; pop {r4, pc}
STUB_RTC_GET = struct.pack(
    "<HHHHHHHHIII",
    0xB510, 0x4803, 0x4B03, 0x4798, 0x4903, 0x200A, 0x8008, 0xBD10,
    MSC_DATA_BUF_ADDR, HAL_RTC_GET_THUMB, MSC_TX_LEN_ADDR,
)

# Verified Thumb-2 stub @ 0x00800E80:
#   push {r4, lr}; ldr r0, =0x800EA0; ldr r3, =0xCE88D5; blx r3; pop {r4, pc}
STUB_RTC_SET = struct.pack(
    "<HHHHHHII",
    0xB510, 0x4802, 0x4B02, 0x4798, 0xBD10, 0xBF00,
    RTC_BUF_ADDR, HAL_RTC_SET_THUMB,
)


def pack_rtc_time(dt: datetime.datetime) -> bytes:
    """Pack datetime into SL6801 10-byte rtc_time_t (weekday: 0=Mon..6=Sun or Zeller 0..6)."""
    if not (2000 <= dt.year <= 2099):
        raise ValueError(f"Year {dt.year} out of SL6801 RTC range (2000..2099)")
    # Firmware Zeller's congruence at 0xD3B424 yields 0=Mon..6=Sun (matches dt.weekday()).
    return struct.pack(
        "<HBBHBBBB",
        dt.year - 2000,
        dt.month,
        dt.day,
        dt.weekday(),
        dt.hour,
        dt.minute,
        dt.second,
        0,
    )


def unpack_rtc_time(raw: bytes) -> tuple[datetime.datetime, int]:
    """Unpack 10-byte SL6801 rtc_time_t into (datetime, weekday)."""
    if len(raw) < 10:
        raise ValueError(f"Expected >= 10 bytes, got {len(raw)}")
    year_off, month, day, weekday, hour, minute, second, _ = struct.unpack_from("<HBBHBBBB", raw, 0)
    return datetime.datetime(2000 + year_off, month, day, hour, minute, second), weekday


class SL6801Usb:
    def __init__(self, vid: int = VID, pid: int = PID_CARDREADER) -> None:
        try:
            import usb.core
            import usb.util
        except ImportError as exc:
            raise SystemExit("pyusb is required: install with `pip install pyusb` or `apt install python3-usb`") from exc

        self._usb_util = usb.util
        self.dev = usb.core.find(idVendor=vid, idProduct=pid)
        if self.dev is None:
            raise SystemExit(f"Smartlink SL6801 ({vid:04x}:{pid:04x}) not found. Plug in and select 'Charge & Transfer'.")

        self._reattach = False
        if self.dev.is_kernel_driver_active(0):
            self.dev.detach_kernel_driver(0)
            self._reattach = True
        self._usb_util.claim_interface(self.dev, 0)

        cfg = self.dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.ep_out = self._usb_util.find_descriptor(
            intf, custom_match=lambda e: self._usb_util.endpoint_direction(e.bEndpointAddress) == self._usb_util.ENDPOINT_OUT
        )
        self.ep_in = self._usb_util.find_descriptor(
            intf, custom_match=lambda e: self._usb_util.endpoint_direction(e.bEndpointAddress) == self._usb_util.ENDPOINT_IN
        )
        self._tag = 1

    def close(self) -> None:
        self._usb_util.release_interface(self.dev, 0)
        if self._reattach:
            try:
                self.dev.attach_kernel_driver(0)
            except Exception:
                pass

    def _vendor_c0(self, subcmd: int, arg1: int, arg2: int, data_out: bytes = b"", in_len: int = 0) -> bytes:
        tag = self._tag
        self._tag += 1
        direction = 0x80 if in_len > 0 else 0x00
        xfer_len = in_len if in_len > 0 else len(data_out)
        cdb = struct.pack("<BBII6x", 0xC0, subcmd, arg1, arg2)
        cbw = struct.pack("<IIIBBB", 0x43425355, tag, xfer_len, direction, 0, 16) + cdb
        self.ep_out.write(cbw, timeout=2000)

        result = b""
        if data_out:
            self.ep_out.write(data_out, timeout=2000)
        elif in_len > 0:
            result = bytes(self.ep_in.read(in_len, timeout=2000))

        csw = bytes(self.ep_in.read(13, timeout=2000))
        sig, rtag, residue, status = struct.unpack("<IIIB", csw)
        if sig != 0x53425355 or rtag != tag or status != 0:
            raise RuntimeError(f"CSW error: sig=0x{sig:08X} tag={rtag} residue={residue} status={status}")
        return result

    def write_ram(self, addr: int, data: bytes) -> None:
        self._vendor_c0(0x09, len(data), addr, data_out=data)

    def run_ram(self, func_thumb_addr: int, in_len: int = 0) -> bytes:
        return self._vendor_c0(0x0A, 0, func_thumb_addr | 1, in_len=in_len)

    def read_rtc(self) -> tuple[datetime.datetime, int]:
        self.write_ram(SCRATCH_ADDR, STUB_RTC_GET)
        raw = self.run_ram(SCRATCH_ADDR, in_len=10)
        return unpack_rtc_time(raw)

    def set_rtc(self, dt: datetime.datetime) -> None:
        payload = STUB_RTC_SET.ljust(RTC_BUF_ADDR - SCRATCH_ADDR, b"\x00") + pack_rtc_time(dt)
        self.write_ram(SCRATCH_ADDR, payload)
        self.run_ram(SCRATCH_ADDR, in_len=0)


def main() -> int:
    ap = argparse.ArgumentParser(description="Read or sync Smartlink SL6801 hardware RTC over USB (0xC0)")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--read", action="store_true", help="Read current hardware RTC from device")
    group.add_argument("--sync", action="store_true", help="Sync device RTC to host local clock")
    group.add_argument("--set", metavar="YYYY-MM-DD HH:MM:SS", help="Set device RTC to explicit timestamp")
    group.add_argument("--dry-run", action="store_true", help="Self-test rtc_time_t packing and stub sizes without USB")
    args = ap.parse_args()

    if args.dry_run:
        now = datetime.datetime.now().replace(microsecond=0)
        packed = pack_rtc_time(now)
        dt2, wd = unpack_rtc_time(packed)
        assert dt2 == now and wd == now.weekday()
        print(f"Dry-run OK: {now.isoformat(' ')} -> {packed.hex(' ')} (get_stub={len(STUB_RTC_GET)}B, set_stub={len(STUB_RTC_SET)}B)")
        return 0

    dev = SL6801Usb()
    try:
        if args.read:
            dt, wd = dev.read_rtc()
            print(f"Device RTC: {dt.strftime('%Y-%m-%d %H:%M:%S')} (weekday={wd})")
        else:
            target = datetime.datetime.now().replace(microsecond=0) if args.sync else datetime.datetime.strptime(args.set, "%Y-%m-%d %H:%M:%S")
            dev.set_rtc(target)
            rb, wd = dev.read_rtc()
            print(f"RTC updated: {rb.strftime('%Y-%m-%d %H:%M:%S')} (weekday={wd})")
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
