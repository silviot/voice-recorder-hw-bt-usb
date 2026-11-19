#!/usr/bin/env python3
"""
Bluetooth RFCOMM File Sync Client (`+XREC` protocol) for Smartlink SL6801 (`HW_76`).

Works with the `fw_mod/bt_filesync.c` XIP extension installed at 0x00D74000.
Connects to `HW_76` on RFCOMM Channel 1, completes the SL6801 HFP Service Level
Connection (`SLC_OPEN`), and then uses the custom `+XREC:` commands to:
  - Ping the extension (`+XREC:PING`)
  - List all recordings in `0:\record` with exact sizes (`+XREC:LS`)
  - Download recordings as bit-exact binary chunks with CRC-16 verification (`+XREC:GET`)
  - Track synced files in `.sync_manifest.json` so already-synced files are skipped.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
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
CHUNK_SIZE = 240  # <= 256 bytes per RFCOMM packet for reliable flow control


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        t = ((crc >> 8) ^ b) & 0xFF
        t ^= t >> 4
        crc = ((crc << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return crc


def discover_hw_mac(explicit_mac: Optional[str] = None) -> Optional[str]:
    if explicit_mac:
        return explicit_mac
    try:
        out = subprocess.check_output(["bluetoothctl", "devices"], text=True)
        for line in out.splitlines():
            m = re.search(r"Device ([0-9A-F:]{17}) (.*)", line)
            if m and m.group(2).strip().startswith(DEFAULT_NAME_PREFIX):
                return m.group(1)
    except Exception:
        pass
    return None


class XRecRFCOMMClient:
    def __init__(self, mac: str, channel: int = 1, timeout: float = 8.0):
        self.mac = mac
        self.channel = channel
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self.rx_buf = bytearray()

    def connect_and_handshake(self) -> None:
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        self.sock.settimeout(self.timeout)
        print(f"Connecting RFCOMM {self.mac} ch {self.channel}...")
        self.sock.connect((self.mac, self.channel))

        def send_ag(lines: List[str]) -> None:
            payload = ("\r\n" + "\r\n".join(lines) + "\r\n").encode("ascii")
            assert self.sock is not None
            self.sock.sendall(payload)

        # Complete HFP Service Level Connection until SLC_OPEN (after AT+CMER)
        buf = ""
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
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

    def get_chunk(self, filename: str, offset: int, length: int = CHUNK_SIZE) -> bytes:
        self._send_cmd(f"+XREC:GET,{offset},{length},{filename}")

        # Wait for either 12-byte binary header "XREC" or text error "+XREC:ERR,"
        while len(self.rx_buf) < 4:
            self._recv_into_buf()

        if self.rx_buf.startswith(b"+XREC:ERR,"):
            err = self._read_line()
            raise RuntimeError(f"Device error reading {filename}@{offset}: {err}")

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

        if rx_off != offset:
            raise RuntimeError(f"Offset mismatch: expected {offset}, got {rx_off}")
        calc_crc = crc16_ccitt_false(payload)
        if calc_crc != rx_crc:
            raise RuntimeError(f"CRC-16 mismatch at offset {offset}: expected 0x{rx_crc:04X}, calc 0x{calc_crc:04X}")
        return payload

    def download_file(self, filename: str, total_size: int, dst_path: Path) -> str:
        tmp_path = dst_path.with_suffix(dst_path.suffix + ".part")
        h = hashlib.sha256()
        offset = 0
        t0 = time.monotonic()

        with open(tmp_path, "wb") as f:
            while offset < total_size:
                req = min(CHUNK_SIZE, total_size - offset)
                chunk = None
                for attempt in range(3):
                    try:
                        chunk = self.get_chunk(filename, offset, req)
                        break
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

        print()
        tmp_path.replace(dst_path)
        return h.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description="Download recordings from HW_76 over Bluetooth RFCOMM (+XREC).")
    parser.add_argument("--mac", type=str, default=None, help="Bluetooth MAC address of HW_76")
    parser.add_argument(
        "-d", "--dest",
        type=Path,
        default=Path.home() / "Recordings" / "HW_76",
        help="Destination directory for downloaded recordings",
    )
    parser.add_argument("--ping", action="store_true", help="Only ping the +XREC firmware extension")
    parser.add_argument("--list", action="store_true", help="Only list recordings on 0:\\record")
    args = parser.parse_args()

    mac = discover_hw_mac(args.mac)
    if not mac:
        print("Could not find HW_76 in BlueZ paired devices. Pass --mac XX:XX:XX:XX:XX:XX.")
        sys.exit(1)

    client = XRecRFCOMMClient(mac)
    try:
        client.connect_and_handshake()
        pong = client.ping()
        print(f"Ping response: {pong}")
        if args.ping:
            return

        files = client.list_files()
        print(f"Found {len(files)} file(s) in 0:\\record:")
        for name, sz in files:
            print(f"  - {name} ({sz:,} bytes)")
        if args.list:
            return

        args.dest.mkdir(parents=True, exist_ok=True)
        manifest_path = args.dest / ".sync_manifest.json"
        manifest: Dict[str, dict] = {}
        if manifest_path.is_file():
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            except Exception:
                manifest = {}

        synced = 0
        for name, sz in files:
            dst = args.dest / name
            cached = manifest.get(name)
            if cached and cached.get("size") == sz and dst.is_file():
                print(f"Skipping already synced: {name}")
                continue
            digest = client.download_file(name, sz, dst)
            manifest[name] = {
                "filename": name,
                "size": sz,
                "sha256": digest,
                "synced_at": datetime.now(tz=timezone.utc).isoformat(),
                "transport": "bluetooth_rfcomm_xrec",
            }
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            synced += 1
        print(f"Bluetooth sync complete ({synced} new file(s) downloaded to {args.dest}).")
    finally:
        client.close()


if __name__ == "__main__":
    main()
