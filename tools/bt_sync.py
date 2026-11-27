#!/usr/bin/env python3
"""
Bluetooth RFCOMM File Sync Client (`+XREC` protocol) for Smartlink SL6801 (`HW_76`).

Primary target: open_fw (`Cassini_XX`, native SPP on RFCOMM channel 1, no HFP handshake; docs/XREC_PROTOCOL.md).
Also works with the legacy `fw_mod/bt_filesync.c` stock-firmware patch (`HW_XX`), completing its HFP Service Level
Connection (`SLC_OPEN`) first (--transport auto/hfp). It uses the custom `+XREC:` commands to:
  - Ping the device (`+XREC:PING`)
  - List all recordings in `0:\record` with exact sizes (`+XREC:LS`)
  - Download recordings as bit-exact binary chunks with CRC-16 verification (`+XREC:GET` / `+XREC:GETS`)
  - Skip files that already exist locally with the same filename and byte size (stateless, rsync-style).
  - Upload a CWX1 transcript sidecar next to a recording (`+XREC:PUT,...,REC,<stem>.cwx:` + `+XREC:PUTEND`, open_fw).

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import hmac
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple


DEFAULT_NAME_PREFIX = "HW_"
DEFAULT_NAME_PREFIXES = ("Cassini_", "HW_")
DEFAULT_KEY_FILE = Path.home() / ".config" / "hw76-sync" / "keys.json"
CHUNK_SIZE = 640  # 12 B header + 640 B = 652 B <= 660 B RFCOMM MTU (with bt-rfcomm-turbo; use 480 on stock MTU)
PUT_CHUNK_SIZE = 600  # +XREC:PUT payload (<= 640); header + payload stays one ~650 B write, one chunk in flight
DEFAULT_BURST_SIZE = 3840  # 6 x 640 B frames per +XREC:GETS burst (stock firmware's window; open_fw takes any size)


def load_device_key(key_file: Path, mac: str) -> Optional[bytes]:
    if not key_file.is_file():
        return None
    try:
        data = json.loads(key_file.read_text(encoding="utf-8"))
        hex_key = data.get(mac.upper())
        if isinstance(hex_key, str) and len(hex_key) == 64:
            return bytes.fromhex(hex_key)
    except Exception:
        return None
    return None


def save_device_key(key_file: Path, mac: str, key: bytes) -> None:
    key_file.parent.mkdir(parents=True, exist_ok=True)
    data: Dict[str, str] = {}
    if key_file.is_file():
        try:
            loaded = json.loads(key_file.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                data = loaded
        except Exception:
            pass
    data[mac.upper()] = key.hex()
    payload = (json.dumps(data, indent=2) + "\n").encode("utf-8")
    tmp_file = key_file.with_suffix(key_file.suffix + ".tmp")
    fd = os.open(tmp_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, payload)
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(tmp_file, key_file)


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        t = ((crc >> 8) ^ b) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


def find_remote_file(files: List[Tuple[str, int]], name: str) -> Optional[Tuple[str, int]]:
    """Case-insensitive lookup in 0:\\record (exFAT), returning the device's (on_card_name, size)."""
    want = name.lower()
    for fn, sz in files:
        if fn.lower() == want:
            return fn, sz
    return None


def discover_hw_mac(explicit_mac: Optional[str] = None) -> Optional[str]:
    if explicit_mac:
        return explicit_mac
    try:
        out = subprocess.check_output(["bluetoothctl", "devices"], text=True)
        for line in out.splitlines():
            m = re.search(r"Device ([0-9A-F:]{17}) (.*)", line)
            if m and m.group(2).strip().startswith(DEFAULT_NAME_PREFIXES):
                return m.group(1)
    except Exception:
        pass
    return None


NATIVE_SPP_PROBE_S = 1.5  # OpenFW (native SPP) sends nothing after RFCOMM open; the stock HFP unit sends AT+BRSF at once


# STAT pace_ms of a stereo recording (no transfers until it stops) and the wait of ERR,LATER
LATER_PACE_MS = 30000


class DeviceRefused(RuntimeError):
    """+XREC:ERR,BUSY (the file being recorded) or +XREC:ERR,LATER,<ms> (stereo recording): skip the file for now."""

    def __init__(self, msg: str, reply: str):
        super().__init__(msg)
        self.reply = reply


class XRecRFCOMMClient:
    def __init__(self, mac: str, channel: int = 1, timeout: float = 8.0, transport: str = "auto"):
        self.mac = mac
        self.channel = channel
        self.timeout = timeout
        self.transport = transport  # "auto" | "native" (OpenFW SPP, no HFP) | "hfp" (stock HW_76 + bt_filesync)
        self.sock: Optional[socket.socket] = None
        self.rx_buf = bytearray()

    def connect_and_handshake(self) -> None:
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        self.sock.settimeout(self.timeout)
        print(f"Connecting RFCOMM {self.mac} ch {self.channel}...")
        self.sock.connect((self.mac, self.channel))
        first = b""
        if self.transport == "native":
            print("Native SPP (OpenFW): no HFP handshake.")
            return
        if self.transport == "auto":
            # OpenFW (the BT=1 Bluedroid SPP server; earlier the legacy bt_spp.c) speaks +XREC directly on channel 1 and stays
            # silent until a command arrives; the stock firmware's HFP unit starts the SLC with AT+BRSF immediately.
            self.sock.settimeout(NATIVE_SPP_PROBE_S)
            try:
                first = self.sock.recv(256)
            except (TimeoutError, socket.timeout):
                first = b""
            finally:
                self.sock.settimeout(self.timeout)
            if not first:
                print("Native SPP (OpenFW): device silent after connect, skipping the HFP handshake.")
                return
            if not first.lstrip().startswith(b"AT"):
                # Not HFP either: keep the bytes for the +XREC reader.
                self.rx_buf.extend(first)
                print("Native SPP (OpenFW): no AT command after connect, skipping the HFP handshake.")
                return

        def send_ag(lines: List[str]) -> None:
            assert self.sock is not None
            for line in lines:
                self.sock.sendall(f"\r\n{line}\r\n".encode("ascii"))
                time.sleep(0.05)

        # Complete HFP Service Level Connection until SLC_OPEN (after AT+CMER)
        buf = first.decode("ascii", errors="replace")
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if "\r" in buf:
                chunk = ""
            else:
                chunk = self.sock.recv(256).decode("ascii", errors="replace")
                if not chunk:
                    raise ConnectionError("RFCOMM closed during HFP handshake")
            buf += chunk
            while "\r" in buf:
                line, buf = buf.split("\r", 1)
                line = line.strip()
                if not line:
                    continue
                if line.startswith("AT+BRSF="):
                    send_ag(["+BRSF: 1007", "OK"])
                elif line.startswith("AT+BAC="):
                    send_ag(["OK"])
                elif line == "AT+CIND=?":
                    send_ag([
                        '+CIND: ("service",(0,1)),("call",(0,1)),("callsetup",(0,3)),("callheld",(0,2)),("signal",(0,5)),("roam",(0,1)),("battchg",(0,5))',
                        "OK",
                    ])
                elif line == "AT+CIND?":
                    send_ag(["+CIND: 1,0,0,0,5,0,5", "OK"])
                elif line.startswith("AT+CMER="):
                    send_ag(["OK"])
                    print("HFP SLC_OPEN established!")
                    time.sleep(0.15)
                    return
                else:
                    send_ag(["OK"])
        raise TimeoutError("Timed out completing HFP SLC_OPEN handshake")

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def _recv_into_buf(self) -> None:
        assert self.sock is not None
        data = self.sock.recv(1024)
        if not data:
            raise ConnectionError("RFCOMM peer disconnected")
        self.rx_buf.extend(data)

    def _read_line(self) -> str:
        while True:
            nl = self.rx_buf.find(b"\n")
            if nl != -1:
                line = bytes(self.rx_buf[: nl + 1]).decode("ascii", errors="replace").strip()
                del self.rx_buf[: nl + 1]
                if line:
                    return line
                continue
            self._recv_into_buf()

    def _send_cmd(self, cmd: str) -> None:
        assert self.sock is not None
        self.sock.sendall((cmd + "\r").encode("ascii"))

    def ping(self) -> str:
        self._send_cmd("+XREC:PING")
        return self._read_line()

    def pair(self, pin: str, key: bytes) -> None:
        if len(pin) != 6 or not pin.isdigit():
            raise ValueError("PIN must be exactly 6 digits")
        if len(key) != 32:
            raise ValueError("Key must be 32 bytes")
        self._send_cmd(f"+XREC:PAIR,{pin},{key.hex()}")
        resp = self._read_line()
        if resp != "+XREC:OK,PAIR":
            raise RuntimeError(f"Pairing failed: {resp}")

    def authenticate(self, key: bytes) -> None:
        self._send_cmd("+XREC:CHAL")
        line = self._read_line()
        if not line.startswith("+XREC:CHAL,"):
            raise RuntimeError(f"Challenge failed: {line}")
        nonce = bytes.fromhex(line[len("+XREC:CHAL,") :].strip())
        mac_hex = hmac.new(key, nonce, hashlib.sha256).hexdigest()
        self._send_cmd(f"+XREC:AUTH,{mac_hex}")
        resp = self._read_line()
        if resp != "+XREC:OK,AUTH":
            raise RuntimeError(f"Authentication failed: {resp}")

    def list_files(self) -> List[Tuple[str, int]]:
        self._send_cmd("+XREC:LS")
        files: List[Tuple[str, int]] = []
        while True:
            line = self._read_line()
            if line == "+XREC:END":
                break
            if line.startswith("+XREC:ERR,"):
                raise RuntimeError(f"Device error listing files: {line}")
            if line.startswith("+XREC:F,"):
                parts = line.split(",", 2)
                if len(parts) == 3:
                    files.append((parts[2], int(parts[1])))
        return files

    def list_files_with_time(self) -> List[Tuple[str, int, str]]:
        """List recordings with ISO-8601 local timestamps (+XREC:LS,T), falling back to +XREC:LS on older firmware."""
        self._send_cmd("+XREC:LS,T")
        files: List[Tuple[str, int, str]] = []
        while True:
            line = self._read_line()
            if line == "+XREC:END":
                break
            if line == "+XREC:ERR,ARGS":
                return [(name, sz, "-") for name, sz in self.list_files()]
            if line.startswith("+XREC:ERR,"):
                raise RuntimeError(f"Device error listing files: {line}")
            if line.startswith("+XREC:FT,"):
                parts = line.split(",", 3)
                if len(parts) == 4:
                    files.append((parts[3], int(parts[1]), parts[2]))
        return files

    def delete_file(self, filename: str) -> None:
        self._send_cmd(f"+XREC:DEL,{filename}")
        line = self._read_line()
        if line != "+XREC:OK,DEL":
            raise RuntimeError(f"Failed to delete {filename}: {line}")

    def read_rtc(self) -> str:
        self._send_cmd("+XREC:RTC")
        line = self._read_line()
        if not line.startswith("+XREC:RTC,"):
            raise RuntimeError(f"Failed to read RTC: {line}")
        return line[len("+XREC:RTC,") :]

    def set_rtc(self, dt: datetime) -> bool:
        """Sets the clock. False: the device records and did not change it (+XREC:OK,RTC,LATER; set it again after
        the recording, docs/XREC_PROTOCOL.md 5.5)."""
        cmd = f"+XREC:RTC,{dt.year},{dt.month},{dt.day},{dt.hour},{dt.minute},{dt.second},{dt.weekday()}"
        self._send_cmd(cmd)
        line = self._read_line()
        if line == "+XREC:OK,RTC,LATER":
            return False
        if line != "+XREC:OK,RTC":
            raise RuntimeError(f"Failed to set RTC: {line}")
        return True

    def rec_start(self, filename: Optional[str] = None) -> str:
        cmd = f"+XREC:RECSTART,{filename}" if filename else "+XREC:RECSTART"
        self._send_cmd(cmd)
        line = self._read_line()
        if not line.startswith("+XREC:OK,RECSTART,"):
            raise RuntimeError(f"Failed to start recording: {line}")
        return line[len("+XREC:OK,RECSTART,") :]

    def rec_stop(self) -> None:
        self._send_cmd("+XREC:RECSTOP")
        line = self._read_line()
        if line != "+XREC:OK,RECSTOP":
            raise RuntimeError(f"Failed to stop recording: {line}")

    def reboot(self) -> None:
        self._send_cmd("+XREC:REBOOT")
        try:
            line = self._read_line()
            if line != "+XREC:OK,REBOOT":
                raise RuntimeError(f"Failed to reboot: {line}")
        except (TimeoutError, ConnectionError, OSError):
            # Device may reset the PMU/BT link immediately before the ACK flushes.
            pass

    def ota_upload_and_flash(self, update_up_path: Path, chunk_size: int = 440) -> None:
        """Upload 0:\\update.up over Bluetooth (+XREC:PUT) and trigger zero-button OTA (+XREC:OTA)."""
        data = update_up_path.read_bytes()
        total = len(data)
        t0 = time.monotonic()
        offset = 0
        assert self.sock is not None

        while offset < total:
            chunk = data[offset : offset + chunk_size]
            crc = crc16_ccitt_false(chunk)
            header = f"+XREC:PUT,{offset},{len(chunk)},{crc},OTA:".encode("ascii")
            for attempt in range(3):
                try:
                    self.sock.sendall(header + chunk)
                    resp = self._read_line()
                    if resp == f"+XREC:OK,PUT,{offset},{len(chunk)}":
                        break
                    err_msg = resp
                except TimeoutError as exc:
                    err_msg = str(exc)
                    self.rx_buf.clear()
                if attempt == 2:
                    raise RuntimeError(f"OTA PUT failed at offset {offset}: {err_msg}")
                print(f"\n  Retry {attempt + 1} at offset {offset}: {err_msg}")
            offset += len(chunk)
            elapsed = max(time.monotonic() - t0, 0.001)
            rate = (offset / 1024.0) / elapsed
            pct = offset * 100.0 / total
            print(
                f"\r  OTA upload: {offset:,}/{total:,} B ({pct:5.1f}%) @ {rate:5.1f} KB/s",
                end="",
                flush=True,
            )
        print("\nUpload complete! Triggering bootloader SD-OTA (+XREC:OTA)...")
        self._send_cmd("+XREC:OTA")
        resp = self._read_line()
        if resp != "+XREC:OK,OTA":
            raise RuntimeError(f"OTA trigger failed: {resp}")
        print("Device acknowledged +XREC:OTA — rebooting to flash 0:\\update.up and auto-boot new firmware!")

    _PUT_OK_RE = re.compile(r"\+XREC:OK,PUT,(\d+),(\d+)")

    def _read_put_reply(self, offset: int) -> str:
        """Next reply line for the PUT chunk at `offset` (or for PUTEND when offset is the total). A late
        +XREC:OK,PUT for an earlier chunk (its first reply arrived after we timed out and resent it; the device
        answers the identical resend again) is dropped, so the client never falls one reply behind."""
        while True:
            line = self._read_line()
            m = self._PUT_OK_RE.fullmatch(line)
            if m and int(m.group(1)) < offset:
                continue
            return line

    def _resync_after_refused_header(self) -> None:
        """After a refused PUT header. Current firmware discards the payload of a refused header whose length it
        parsed; older firmware fed it to the line parser, which may answer with extra ERR,LINE (or other) lines.
        A bare CR ends any partial line; then drain every reply up to our own PONG."""
        assert self.sock is not None
        self.sock.sendall(b"\r+XREC:PING\r")
        try:
            for _ in range(64):
                if self._read_line().startswith("+XREC:PONG"):
                    return
        except (TimeoutError, socket.timeout):
            self.rx_buf.clear()

    # PUT chunk errors worth a resend of the same chunk; every other error is final
    _PUT_RETRY_ERRS = ("+XREC:ERR,CRC",)
    _PUT_HEADER_ERRS = ("+XREC:ERR,ARG", "+XREC:ERR,UNSUP", "+XREC:ERR,NAME")

    def put_file(self, target: str, data: bytes, chunk_size: int = PUT_CHUNK_SIZE) -> int:
        """Staged upload: stop-and-wait +XREC:PUT chunks (one in flight: the device RX ring is 2 KiB) then
        +XREC:PUTEND, which commits atomically. target is "REC,<stem>.cwx" (sidecar next to /record/<stem>.wav) or
        "CAS,<name>" (/cassini/<name>). Returns the committed size."""
        assert self.sock is not None
        if not data:
            raise ValueError("nothing to upload (empty file)")
        if not 1 <= chunk_size <= PUT_CHUNK_SIZE:
            raise ValueError(f"PUT chunk size must be 1..{PUT_CHUNK_SIZE}")
        total = len(data)
        offset = 0
        while offset < total:
            chunk = data[offset : offset + chunk_size]
            header = f"+XREC:PUT,{offset},{len(chunk)},{crc16_ccitt_false(chunk)},{target}:".encode("ascii")
            want = f"+XREC:OK,PUT,{offset},{len(chunk)}"
            err_msg = ""
            for attempt in range(3):
                try:
                    self.sock.sendall(header + chunk)
                    resp = self._read_put_reply(offset)
                    if resp == want:
                        break
                    err_msg = resp
                    if resp in self._PUT_HEADER_ERRS:
                        self._resync_after_refused_header()
                        if offset == 0 and resp != "+XREC:ERR,NAME" and target.startswith("REC,"):
                            raise RuntimeError(f"recorder firmware too old for transcript upload ({resp})")
                        raise RuntimeError(f"PUT {target} refused: {resp}")
                    if resp == "+XREC:ERR,NOENT" and target.startswith("REC,"):
                        raise RuntimeError(f"PUT {target} refused: no matching .wav recording in 0:\\record")
                    if resp not in self._PUT_RETRY_ERRS:
                        # SDIO / NOSD / LOCKED / OFF ...: the device dropped the staged upload, a resend cannot help
                        raise RuntimeError(f"PUT {target} failed at offset {offset}: {resp}")
                except (TimeoutError, socket.timeout) as exc:
                    # keep rx_buf: a partial late reply completes later and is then matched or dropped as stale
                    err_msg = f"timeout ({exc})" if str(exc) else "timeout"
                if attempt == 2:
                    raise RuntimeError(f"PUT {target} failed at offset {offset}: {err_msg}")
                # resending the same chunk (same offset/len/crc/name) is acknowledged without a second write
                print(f"\n  Retry {attempt + 1} at offset {offset}: {err_msg}")
            offset += len(chunk)
            print(f"\r  PUT {target}: {offset:,}/{total:,} B", end="", flush=True)
        print()
        want_end = f"+XREC:OK,PUTEND,{total}"
        resp = ""
        for attempt in range(3):
            self._send_cmd("+XREC:PUTEND")
            try:
                resp = self._read_put_reply(total)
            except (TimeoutError, socket.timeout):
                resp = "timeout"
                continue   # PUTEND is idempotent on current firmware: a repeat answers the same OK
            if resp == want_end:
                return total
            if resp == "+XREC:ERR,NOPUT" and attempt > 0 and target.startswith("REC,"):
                # older firmware after a lost PUTEND reply: the commit happened iff LS shows the name with this size
                name = target[len("REC,") :]
                if any(n.lower() == name.lower() and sz == total for n, sz in self.list_files()):
                    return total
            break
        raise RuntimeError(f"PUTEND failed for {target}: {resp}")

    def upload_cwx(self, cwx_path: Path, remote_name: str, chunk_size: int = PUT_CHUNK_SIZE) -> int:
        """Upload a CWX1 transcript sidecar as /record/<remote_name> (must end in .cwx) via the REC target."""
        data = cwx_path.read_bytes()
        if data[:4] != b"CWX1":
            raise ValueError(f"{cwx_path} is not a CWX1 file")
        if not re.fullmatch(r"[\x20-\x7e]{1,26}\.[cC][wW][xX]", remote_name) or remote_name.startswith(".") or \
                any(ch in remote_name for ch in '/\\:*?"<>|'):
            raise ValueError(f"invalid sidecar name {remote_name!r} (printable ASCII, <= 30 chars, ends in .cwx)")
        return self.put_file(f"REC,{remote_name}", data, chunk_size)

    def get_stat(self) -> Dict[str, int]:
        self._send_cmd("+XREC:STAT")
        line = self._read_line()
        if not line.startswith("+XREC:STAT,"):
            raise RuntimeError(f"Failed to query status: {line}")
        parts = [int(x) for x in line[len("+XREC:STAT,") :].split(",")]
        # fields 9 / 10 (open_fw with sync while recording, docs/XREC_PROTOCOL.md): the active REC_NNNN number (0 = not
        # recording) and the shortest interval between GETS window starts (0 = none; the device paces itself, no pause
        # after a window); older firmware sends 8 fields: keys left out
        keys = ("battery_pct", "voltage_mv", "charge_status", "vbus", "media_status", "rec_sec", "free_mb", "total_mb",
                "rec_idx", "pace_ms", "rec_bytes")
        return dict(zip(keys, parts))

    def pace(self, rate_bps: int = 0, budget_us: int = 0) -> Dict[str, str]:
        """+XREC:DBG,PACE[,<bytes/s>[,<budget us>]] (bench images only, after AUTH): read / set the firmware's GETS cap
        while recording (design M3 runs at 24 / 48 / 64 / 96 KB/s) and its pacing counters; "_line" = raw reply."""
        cmd = "+XREC:DBG,PACE"
        if rate_bps > 0:
            cmd += f",{int(rate_bps)}"
            if budget_us > 0:
                cmd += f",{int(budget_us)}"
        self._send_cmd(cmd)
        line = self._read_line()
        if not line.startswith("+XREC:DBG,PACE,"):
            raise RuntimeError(f"DBG,PACE failed: {line} (bench image with sync-while-recording pacing only)")
        kv = dict(t.split("=", 1) for t in line.split(",") if "=" in t)
        kv["_line"] = line
        return kv

    def dbg(self, rec: Optional[int] = None) -> Dict[str, str]:
        """+XREC:DBG (bench images only, after AUTH): the recorder's measurement counters as a dict, "_line" = the raw
        reply. rec=None: the running (else the last) recording plus CPU / GETS totals; rec=i: the i-th latest
        finished recording. Field list: open_fw/include/openfw_bench_meas.h."""
        self._send_cmd("+XREC:DBG" if rec is None else f"+XREC:DBG,REC,{int(rec)}")
        line = self._read_line()
        if not line.startswith("+XREC:DBG,"):
            raise RuntimeError(f"DBG failed: {line} (bench image only: make -C open_fw BT=1 BENCH=1)")
        kv = dict(t.split("=", 1) for t in line.split(",") if "=" in t)
        kv["_line"] = line
        return kv

    def bench_load(
        self,
        seconds: float,
        load_file: Optional[str] = None,
        load_size: int = 0,
        burst_size: int = DEFAULT_BURST_SIZE,
        chunk_size: int = CHUNK_SIZE,
        kbps: float = 0.0,
        dbg_every: float = 5.0,
        rec: bool = False,
        log=print,
        storm_every: float = 0.0,
        pace_bps: int = 0,
    ) -> Dict[str, float]:
        """Measurement session (docs/BT_HW_RESULTS.md, "Sync during recording"): hold this link for `seconds`,
        optionally RECSTART first / RECSTOP last, pull GETS bursts of load_file in a loop (from 0 again at its end;
        kbps > 0 caps the average rate in KiB/s, 0 = as fast as the link goes) and log a +XREC:DBG line every
        dbg_every s (0 = only at the end). storm_every > 0: every storm_every s also STAT, LS and a DEL of a name that
        does not exist (ERR,DEL: the directory scan without deleting anything; while recording, firmware with sync
        while recording answers ERR,LATER without touching the card), design M3. pace_bps > 0 first sets the
        firmware's own GETS cap while recording (+XREC:DBG,PACE), and the pacing counters are logged at the end.
        Returns the bytes / seconds / KiB/s actually moved."""
        if pace_bps > 0:
            log(f"{0.0:8.1f} {self.pace(pace_bps)['_line']}")
        t0 = time.monotonic()
        nstorm = t0 + storm_every if storm_every > 0 else float("inf")
        storms = 0
        if rec:
            log(f"{0.0:8.1f} RECSTART {self.rec_start()}")
        nxt = t0 + dbg_every if dbg_every > 0 else float("inf")
        off = moved = errors = 0
        while True:
            now = time.monotonic()
            if now - t0 >= seconds:
                break
            if now >= nxt:
                log(f"{now - t0:8.1f} {self.dbg()['_line']}")
                nxt += dbg_every
                continue
            if now >= nstorm:
                self.get_stat()
                self.list_files()
                try:
                    self.delete_file("STORM_NO_SUCH_FILE.WAV")
                except RuntimeError:
                    pass
                storms += 1
                nstorm += storm_every
                continue
            if not load_file or load_size <= 0:
                time.sleep(min(0.05, max(0.0, nxt - now)))
                continue
            if kbps > 0 and moved > kbps * 1024.0 * (now - t0):
                time.sleep(0.005)
                continue
            req = min(burst_size, load_size - off)
            try:
                data = self.get_burst(load_file, off, req, chunk_size)
            except (RuntimeError, TimeoutError, OSError) as exc:
                errors += 1
                log(f"{now - t0:8.1f} GETS error at {off}: {exc}")
                self.rx_buf.clear()
                time.sleep(0.2)
                continue
            moved += len(data)
            off += len(data)
            if not data or off >= load_size:
                off = 0
        el = time.monotonic() - t0
        if rec:
            self.rec_stop()
            log(f"{el:8.1f} RECSTOP")
            log(f"{el:8.1f} {self.dbg(0)['_line']}")
        else:
            log(f"{el:8.1f} {self.dbg()['_line']}")
        if pace_bps > 0:
            log(f"{el:8.1f} {self.pace()['_line']}")
        res = {"bytes": float(moved), "seconds": el, "kib_s": moved / 1024.0 / max(el, 1e-6), "errors": float(errors),
               "storms": float(storms)}
        log(f"{el:8.1f} LOAD bytes={moved} kib_s={res['kib_s']:.1f} errors={errors} storms={storms}")
        return res

    def _read_xrec_frame(self, expected_offset: int, filename: str) -> bytes:
        # Wait for either 12-byte binary header "XREC" or text error "+XREC:ERR," (even if split across recv chunks)
        while True:
            while self.rx_buf and self.rx_buf[0] in (0x0D, 0x0A):
                del self.rx_buf[0]
            if self.rx_buf.startswith(b"+"):
                line = self._read_line()
                if line == "+XREC:ERR,BUSY" or line.startswith("+XREC:ERR,LATER"):
                    raise DeviceRefused(f"Device refused {filename}@{expected_offset}: {line}", line)
                if line.startswith("+XREC:"):
                    raise RuntimeError(f"Device error reading {filename}@{expected_offset}: {line}")
                continue
            if len(self.rx_buf) >= 4:
                break
            self._recv_into_buf()

        # Sync to "XREC" header (ignoring any stray HFP CR/LF)
        while True:
            idx = self.rx_buf.find(b"XREC")
            if idx != -1:
                if idx > 0:
                    del self.rx_buf[:idx]
                break
            self._recv_into_buf()

        while len(self.rx_buf) < 12:
            self._recv_into_buf()

        magic, rx_off, rx_len, rx_crc = struct.unpack_from("<4sIHH", self.rx_buf, 0)
        if magic != b"XREC":
            raise RuntimeError(f"Bad chunk magic: {magic!r}")

        while len(self.rx_buf) < 12 + rx_len:
            self._recv_into_buf()

        payload = bytes(self.rx_buf[12 : 12 + rx_len])
        del self.rx_buf[: 12 + rx_len]

        if rx_off != expected_offset:
            raise RuntimeError(f"Offset mismatch: expected {expected_offset}, got {rx_off}")
        calc_crc = crc16_ccitt_false(payload)
        if calc_crc != rx_crc:
            raise RuntimeError(f"CRC-16 mismatch at offset {expected_offset}: expected 0x{rx_crc:04X}, calc 0x{calc_crc:04X}")
        return payload

    def get_chunk(self, filename: str, offset: int, length: int = CHUNK_SIZE) -> bytes:
        self._send_cmd(f"+XREC:GET,{offset},{length},{filename}")
        return self._read_xrec_frame(offset, filename)

    def get_burst(self, filename: str, offset: int, total_len: int, chunk_size: int = CHUNK_SIZE) -> bytes:
        """One +XREC:GETS burst. total_len must not run past the file's LS size. The device may lower the frame size
        to its RFCOMM MTU - 12, so a short FIRST frame is not EOF; but every frame of a burst except the last has
        the same size, so a frame shorter than the first one ends the burst (EOF inside the burst: the device sends
        no N=0 frame after it). The burst also ends when total_len bytes arrived, on an empty frame (read at/after
        EOF), or on a timeout after at least one frame (a short first frame that was EOF)."""
        self._send_cmd(f"+XREC:GETS,{offset},{total_len},{chunk_size},{filename}")
        out = bytearray()
        cur = offset
        first_len = 0
        while len(out) < total_len:
            try:
                frame = self._read_xrec_frame(cur, filename)
            except (TimeoutError, socket.timeout):
                if not out:
                    raise
                self.rx_buf.clear()   # the device is done; drop any partial bytes
                break
            if not frame:
                break  # N=0: at/after EOF (the file is shorter than its LS size)
            out.extend(frame)
            cur += len(frame)
            if not first_len:
                first_len = len(frame)
            elif len(frame) < first_len:
                break  # EOF inside the burst
        return bytes(out)

    def download_file(
        self,
        filename: str,
        total_size: int,
        dst_path: Path,
        burst_size: int = DEFAULT_BURST_SIZE,
        chunk_size: int = CHUNK_SIZE,
    ) -> str:
        tmp_path = dst_path.with_suffix(dst_path.suffix + ".part")
        h = hashlib.sha256()
        offset = 0
        t0 = time.monotonic()

        try:
            with open(tmp_path, "wb") as f:
                while offset < total_size:
                    use_burst = burst_size > chunk_size
                    req = min(burst_size if use_burst else chunk_size, total_size - offset)
                    chunk = None
                    for attempt in range(3):
                        try:
                            if use_burst:
                                chunk = self.get_burst(filename, offset, req, chunk_size)
                            else:
                                chunk = self.get_chunk(filename, offset, req)
                            break
                        except DeviceRefused:
                            raise                    # BUSY / LATER: not a transfer error, no retry
                        except Exception as exc:
                            if attempt == 2:
                                raise
                            print(f"\n  Retry {attempt + 1} at offset {offset}: {exc}")
                            self.rx_buf.clear()
                    assert chunk is not None
                    if not chunk:
                        break
                    f.write(chunk)
                    h.update(chunk)
                    offset += len(chunk)
                    elapsed = max(time.monotonic() - t0, 0.001)
                    rate_kbs = (offset / 1024.0) / elapsed
                    pct = (offset * 100.0 / total_size) if total_size else 100.0
                    print(
                        f"\r  {filename}: {offset:,}/{total_size:,} B ({pct:5.1f}%) @ {rate_kbs:5.1f} KB/s",
                        end="",
                        flush=True,
                    )
                f.flush()
                os.fsync(f.fileno())
        except BaseException:
            tmp_path.unlink(missing_ok=True)
            raise

        print()
        tmp_path.replace(dst_path)
        return h.hexdigest()

    def sync_files(
        self,
        files: List[Tuple[str, int]],
        dest_root: Path,
        burst_size: int = DEFAULT_BURST_SIZE,
        chunk_size: int = CHUNK_SIZE,
    ) -> Tuple[int, List[str]]:
        """Downloads the listed files not yet in dest_root. While the device records (docs/XREC_PROTOCOL.md 5.5) the
        file being recorded (STAT rec_idx) is skipped, a stereo recording (pace_ms >= 30000) skips every download,
        and a file the device answers BUSY / LATER for is skipped for this run. Returns (downloaded, skipped)."""
        rec_name = ""
        skip_all = False
        try:
            st = self.get_stat()
        except (RuntimeError, ValueError, OSError):
            st = {}
        if st.get("rec_idx", 0):
            rec_name = f"REC_{st['rec_idx']:04d}.wav".lower()
        if st.get("pace_ms", 0) >= LATER_PACE_MS:
            skip_all = True
            print("The recorder records in stereo: no downloads until it stops.")
        synced = 0
        skipped: List[str] = []
        for name, sz in files:
            base_dst = safe_dest_path(dest_root, name)
            dst = base_dst
            if dst.is_file() and dst.stat().st_size == sz:
                print(f"Skipping already synced: {name} ({sz:,} bytes)")
                continue
            if dst.exists():
                suffix_idx = 2
                found_match = False
                while True:
                    cand = safe_dest_path(dest_root, f"{base_dst.stem}_{suffix_idx}{base_dst.suffix}")
                    if cand.is_file() and cand.stat().st_size == sz:
                        print(f"Skipping already synced: {name} -> {cand.name} ({sz:,} bytes)")
                        found_match = True
                        break
                    if not cand.exists():
                        dst = cand
                        break
                    suffix_idx += 1
                if found_match:
                    continue
            if skip_all or name.lower() == rec_name:
                print(f"Skipping {name}: " + ("stereo recording" if skip_all else "being recorded"))
                skipped.append(name)
                continue
            try:
                digest = self.download_file(name, sz, dst, burst_size=burst_size, chunk_size=chunk_size)
            except DeviceRefused as exc:
                print(f"\n  Skipping {name} for now ({exc.reply})")
                self.rx_buf.clear()
                skipped.append(name)
                continue
            print(f"  -> Saved {dst} (SHA-256: {digest[:16]}...)")
            synced += 1
        return synced, skipped


def safe_dest_path(dest_root: Path, remote_name: str) -> Path:
    """Validate a device-supplied filename against path traversal and control characters."""
    if not remote_name or remote_name in (".", ".."):
        raise ValueError(f"Unsafe remote filename: {remote_name!r}")
    if "/" in remote_name or "\\" in remote_name or Path(remote_name).name != remote_name:
        raise ValueError(f"Unsafe remote filename (path separator): {remote_name!r}")
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in remote_name):
        raise ValueError(f"Unsafe remote filename (control char): {remote_name!r}")
    root = dest_root.resolve()
    dst = (root / remote_name).resolve()
    if dst.parent != root:
        raise ValueError(f"Remote filename escapes destination directory: {remote_name!r}")
    return dst


def main() -> None:
    parser = argparse.ArgumentParser(description="Download recordings from HW_76 over Bluetooth RFCOMM (+XREC).")
    parser.add_argument("--mac", type=str, default=None, help="Bluetooth MAC address of HW_76")
    parser.add_argument(
        "--transport",
        choices=("auto", "native", "hfp"),
        default="auto",
        help="RFCOMM start-up: native = OpenFW SPP (no HFP), hfp = stock firmware HFP SLC handshake, "
        f"auto = native unless the device sends AT commands within {NATIVE_SPP_PROBE_S} s (default)",
    )
    parser.add_argument(
        "-d", "--dest",
        type=Path,
        default=Path.home() / "Recordings" / "HW_76",
        help="Destination directory for downloaded recordings",
    )
    parser.add_argument("--ping", action="store_true", help="Only ping the +XREC firmware extension")
    parser.add_argument("--list", action="store_true", help="Only list recordings on 0:\\record")
    parser.add_argument("--delete", metavar="FILENAME", help="Delete a single recording from 0:\\record")
    parser.add_argument("--rtc-read", action="store_true", help="Read hardware RTC over Bluetooth")
    parser.add_argument("--rtc-sync", action="store_true", help="Sync device hardware RTC to host local clock over Bluetooth")
    parser.add_argument("--rtc-set", metavar="YYYY-MM-DD HH:MM:SS", help="Set device hardware RTC to explicit local timestamp")
    parser.add_argument("--stat", action="store_true", help="Query battery %%, voltage, charge state, recording state & SD free space")
    parser.add_argument("--rec-start", nargs="?", const="", metavar="FILENAME", help="Start a local stereo WAV recording (optional custom filename)")
    parser.add_argument("--rec-stop", action="store_true", help="Stop an active local stereo WAV recording")
    parser.add_argument("--reboot", action="store_true", help="Cleanly reboot the device over Bluetooth (+XREC:REBOOT)")
    parser.add_argument("--ota", type=Path, metavar="UPDATE_UP", help="Upload 0:\\update.up over Bluetooth (+XREC:PUT) and auto-flash (+XREC:OTA)")
    parser.add_argument(
        "--upload-cwx",
        type=Path,
        metavar="CWX_FILE",
        help="Upload a CWX1 transcript sidecar into 0:\\record via +XREC:PUT REC,<stem>.cwx (open_fw). The remote "
        "name is <stem of --for-recording>.cwx, else the local file name",
    )
    parser.add_argument(
        "--for-recording",
        metavar="REC_NAME",
        help="Recording the --upload-cwx sidecar belongs to (e.g. REC_0001.wav -> REC_0001.cwx); checked against LS",
    )
    parser.add_argument("--dbg", action="store_true", help="Bench images: print +XREC:DBG (recording / CPU counters)")
    parser.add_argument("--dbg-rec", type=int, metavar="I", help="Bench images: +XREC:DBG,REC,I (I-th latest recording)")
    parser.add_argument(
        "--bench-load",
        type=float,
        metavar="SECONDS",
        help="Bench images: hold the link SECONDS, polling +XREC:DBG every --dbg-every s; with --load-file pull GETS "
        "bursts of that file in a loop (--load-kbps cap), with --load-rec RECSTART first and RECSTOP last",
    )
    parser.add_argument("--load-file", metavar="NAME", help="--bench-load: file in 0:\\record to pull repeatedly")
    parser.add_argument("--load-kbps", type=float, default=0.0, help="--bench-load: average GETS cap in KiB/s (0 = full)")
    parser.add_argument("--load-rec", action="store_true", help="--bench-load: RECSTART at the start, RECSTOP at the end")
    parser.add_argument("--load-storm", type=float, default=0.0,
                        help="--bench-load: every N s also STAT + LS + DEL of a missing name (0 = off)")
    parser.add_argument("--load-pace", type=int, default=0, metavar="BYTES_S",
                        help="--bench-load: set the firmware's GETS cap while recording first (+XREC:DBG,PACE; M3)")
    parser.add_argument("--dbg-every", type=float, default=5.0, help="--bench-load: DBG poll period in s (0 = end only)")
    parser.add_argument("--dbg-log", type=Path, help="--bench-load / --dbg: also append the lines to this file")
    parser.add_argument("--pair", metavar="PIN", help="Bind to recorder using the 6-digit PIN shown on its Bluetooth menu LCD (v5)")
    parser.add_argument(
        "--key-file",
        type=Path,
        default=DEFAULT_KEY_FILE,
        help=f"Local JSON keystore for v5 device secrets (default: {DEFAULT_KEY_FILE})",
    )
    parser.add_argument(
        "--burst-size",
        type=int,
        default=DEFAULT_BURST_SIZE,
        help=f"Multi-packet +XREC:GETS burst size in bytes (0 to use single-chunk +XREC:GET; default {DEFAULT_BURST_SIZE})",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=CHUNK_SIZE,
        help=f"Per-frame +XREC:GET/GETS payload size in bytes (default {CHUNK_SIZE}; up to 640 with bt-rfcomm-turbo)",
    )
    parser.add_argument(
        "--ota-chunk-size",
        type=int,
        default=560,
        help="Per-packet +XREC:PUT payload size in bytes (default 560 with bt-rfcomm-turbo; use 440 on stock MTU)",
    )
    args = parser.parse_args()

    mac = discover_hw_mac(args.mac)
    if not mac:
        print("Could not find Cassini_* or HW_* in BlueZ paired devices. Pass --mac XX:XX:XX:XX:XX:XX.")
        sys.exit(1)

    client = XRecRFCOMMClient(mac, transport=args.transport)
    try:
        client.connect_and_handshake()
        pong = client.ping()
        print(f"Ping response: {pong}")
        if args.ping:
            return

        if args.pair:
            new_key = os.urandom(32)
            client.pair(args.pair.strip(), new_key)
            save_device_key(args.key_file, mac, new_key)
            print(f"Paired & bound {mac} successfully! Secret saved to {args.key_file} (0600).")
            return

        if ",v5," in pong:
            key = load_device_key(args.key_file, mac)
            if not key:
                print(
                    f"Recorder is locked (v5) and no key is stored for {mac} in {args.key_file}.\n"
                    "Open the Bluetooth menu on the recorder to display the 6-digit PIN, then run:\n"
                    f"  python3 tools/bt_sync.py --mac {mac} --pair <6-digit-PIN>"
                )
                sys.exit(2)
            client.authenticate(key)
            print("Authenticated (HMAC-SHA256 challenge-response OK).")

        def out(line: str) -> None:
            print(line, flush=True)
            if args.dbg_log:
                with open(args.dbg_log, "a", encoding="ascii", errors="replace") as fh:
                    fh.write(line + "\n")

        if args.dbg or args.dbg_rec is not None:
            out(client.dbg(args.dbg_rec)["_line"])
            return
        if args.bench_load is not None:
            size = 0
            load_file = args.load_file
            if load_file:
                hit = find_remote_file(client.list_files(), load_file)
                if hit is None or hit[1] <= 0:
                    print(f"{load_file} is not in 0:\\record (LS); no GETS load.")
                    sys.exit(1)
                load_file, size = hit
            client.bench_load(args.bench_load, load_file, size, burst_size=max(args.burst_size, args.chunk_size),
                              chunk_size=args.chunk_size, kbps=args.load_kbps, dbg_every=args.dbg_every,
                              rec=args.load_rec, log=out, storm_every=args.load_storm, pace_bps=args.load_pace)
            return
        if args.rtc_read:
            print(f"Device RTC: {client.read_rtc()}")
            return
        if args.rtc_sync or args.rtc_set:
            target = datetime.now().replace(microsecond=0) if args.rtc_sync else datetime.strptime(args.rtc_set, "%Y-%m-%d %H:%M:%S")
            if not client.set_rtc(target):
                print(f"The recorder records: the clock was not changed (set it again after the recording). "
                      f"Device RTC: {client.read_rtc()}")
                return
            print(f"Device RTC updated: {client.read_rtc()}")
            return
        if args.delete:
            client.delete_file(args.delete)
            print(f"Deleted 0:\\record\\{args.delete}")
            return
        if args.stat:
            st = client.get_stat()
            chg_names = {0: "discharging", 1: "charging", 2: "full"}
            chg = chg_names.get(st.get("charge_status", -1), str(st.get("charge_status")))
            print(
                f"Battery: {st['battery_pct']}% ({st['voltage_mv']} mV, {chg}, vbus={st['vbus']}) | "
                f"Media: 0x{st['media_status']:02X} (rec={st['rec_sec']}s) | "
                f"SD: {st['free_mb']:,} / {st['total_mb']:,} MB free"
            )
            return
        if args.rec_start is not None:
            fn = client.rec_start(args.rec_start or None)
            print(f"Started local stereo recording: 0:\\record\\{fn}")
            return
        if args.rec_stop:
            client.rec_stop()
            print("Stopped local stereo recording.")
            return
        if args.reboot:
            client.reboot()
            print("Device rebooting...")
            return
        if args.ota:
            client.ota_upload_and_flash(args.ota, chunk_size=args.ota_chunk_size)
            return
        if args.upload_cwx:
            if args.for_recording:
                hit = find_remote_file(client.list_files(), args.for_recording)
                if hit is None:
                    print(f"{args.for_recording} is not in 0:\\record (LS); not uploading.")
                    sys.exit(1)
                remote = Path(hit[0]).stem + ".cwx"
            else:
                remote = args.upload_cwx.name
            n = client.upload_cwx(args.upload_cwx, remote)
            print(f"Uploaded transcript 0:\\record\\{remote} ({n:,} bytes)")
            return

        files_t = client.list_files_with_time()
        print(f"Found {len(files_t)} file(s) in 0:\\record:")
        for name, sz, iso in files_t:
            ts_note = f", {iso}" if iso != "-" else ""
            print(f"  - {name} ({sz:,} bytes{ts_note})")
        if args.list:
            return
        files = [(name, sz) for name, sz, _ in files_t]

        args.dest.mkdir(parents=True, exist_ok=True)
        dest_root = args.dest.resolve()
        synced, skipped = client.sync_files(files, dest_root, burst_size=args.burst_size, chunk_size=args.chunk_size)
        note = f", {len(skipped)} skipped while recording" if skipped else ""
        print(f"Bluetooth sync complete ({synced} new file(s) downloaded to {args.dest}{note}).")
    finally:
        client.close()


if __name__ == "__main__":
    main()
