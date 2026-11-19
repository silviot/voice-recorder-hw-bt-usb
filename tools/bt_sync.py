#!/usr/bin/env python3
"""
Bluetooth RFCOMM File Sync Client (`+XREC` protocol) for Smartlink SL6801 (`HW_76`).

Works with the `fw_mod/bt_filesync.c` XIP extension installed at 0x00D74000.
Connects to `HW_76` on RFCOMM Channel 1, completes the SL6801 HFP Service Level
Connection (`SLC_OPEN`), and then uses the custom `+XREC:` commands to:
  - Ping the extension (`+XREC:PING`)
  - List all recordings in `0:\record` with exact sizes (`+XREC:LS`)
  - Download recordings as bit-exact binary chunks with CRC-16 verification (`+XREC:GET` / `+XREC:GETS`)
  - Skip files that already exist locally with the same filename and byte size (stateless, rsync-style).

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
DEFAULT_KEY_FILE = Path.home() / ".config" / "hw76-sync" / "keys.json"
CHUNK_SIZE = 480  # 12 B header + 480 B = 492 B <= 512 B negotiated RFCOMM MTU (0x200)
DEFAULT_BURST_SIZE = 1920  # 4 x 480 B frames per +XREC:GETS burst (safe within K=7 RFCOMM credit window)


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
    key_file.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.chmod(key_file, 0o600)


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
            assert self.sock is not None
            for line in lines:
                self.sock.sendall(f"\r\n{line}\r\n".encode("ascii"))
                time.sleep(0.05)

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

    def set_rtc(self, dt: datetime) -> None:
        cmd = f"+XREC:RTC,{dt.year},{dt.month},{dt.day},{dt.hour},{dt.minute},{dt.second},{dt.weekday()}"
        self._send_cmd(cmd)
        line = self._read_line()
        if line != "+XREC:OK,RTC":
            raise RuntimeError(f"Failed to set RTC: {line}")

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
                self.sock.sendall(header + chunk)
                resp = self._read_line()
                if resp == f"+XREC:OK,PUT,{offset},{len(chunk)}":
                    break
                if attempt == 2:
                    raise RuntimeError(f"OTA PUT failed at offset {offset}: {resp}")
                print(f"\n  Retry {attempt + 1} at offset {offset}: {resp}")
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

    def get_stat(self) -> Dict[str, int]:
        self._send_cmd("+XREC:STAT")
        line = self._read_line()
        if not line.startswith("+XREC:STAT,"):
            raise RuntimeError(f"Failed to query status: {line}")
        parts = [int(x) for x in line[len("+XREC:STAT,") :].split(",")]
        keys = ("battery_pct", "voltage_mv", "charge_status", "vbus", "media_status", "rec_sec", "free_mb", "total_mb")
        return dict(zip(keys, parts))

    def _read_xrec_frame(self, expected_offset: int, filename: str) -> bytes:
        # Wait for either 12-byte binary header "XREC" or text error "+XREC:ERR,"
        while len(self.rx_buf) < 4:
            self._recv_into_buf()

        if self.rx_buf.startswith(b"+XREC:ERR,"):
            err = self._read_line()
            raise RuntimeError(f"Device error reading {filename}@{expected_offset}: {err}")

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
        self._send_cmd(f"+XREC:GETS,{offset},{total_len},{chunk_size},{filename}")
        out = bytearray()
        cur = offset
        while len(out) < total_len:
            frame = self._read_xrec_frame(cur, filename)
            out.extend(frame)
            cur += len(frame)
            if len(frame) < min(chunk_size, total_len - (len(out) - len(frame))):
                break  # EOF
        return bytes(out)

    def download_file(self, filename: str, total_size: int, dst_path: Path, burst_size: int = DEFAULT_BURST_SIZE) -> str:
        tmp_path = dst_path.with_suffix(dst_path.suffix + ".part")
        h = hashlib.sha256()
        offset = 0
        t0 = time.monotonic()

        with open(tmp_path, "wb") as f:
            while offset < total_size:
                use_burst = burst_size > CHUNK_SIZE
                req = min(burst_size if use_burst else CHUNK_SIZE, total_size - offset)
                chunk = None
                for attempt in range(3):
                    try:
                        if use_burst:
                            chunk = self.get_burst(filename, offset, req, CHUNK_SIZE)
                        else:
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
    parser.add_argument("--delete", metavar="FILENAME", help="Delete a single recording from 0:\\record")
    parser.add_argument("--rtc-read", action="store_true", help="Read hardware RTC over Bluetooth")
    parser.add_argument("--rtc-sync", action="store_true", help="Sync device hardware RTC to host local clock over Bluetooth")
    parser.add_argument("--rtc-set", metavar="YYYY-MM-DD HH:MM:SS", help="Set device hardware RTC to explicit local timestamp")
    parser.add_argument("--stat", action="store_true", help="Query battery %%, voltage, charge state, recording state & SD free space")
    parser.add_argument("--rec-start", nargs="?", const="", metavar="FILENAME", help="Start a local stereo WAV recording (optional custom filename)")
    parser.add_argument("--rec-stop", action="store_true", help="Stop an active local stereo WAV recording")
    parser.add_argument("--reboot", action="store_true", help="Cleanly reboot the device over Bluetooth (+XREC:REBOOT)")
    parser.add_argument("--ota", type=Path, metavar="UPDATE_UP", help="Upload 0:\\update.up over Bluetooth (+XREC:PUT) and auto-flash (+XREC:OTA)")
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

        if args.rtc_read:
            print(f"Device RTC: {client.read_rtc()}")
            return
        if args.rtc_sync or args.rtc_set:
            target = datetime.now().replace(microsecond=0) if args.rtc_sync else datetime.strptime(args.rtc_set, "%Y-%m-%d %H:%M:%S")
            client.set_rtc(target)
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
            client.ota_upload_and_flash(args.ota)
            return

        files = client.list_files()
        print(f"Found {len(files)} file(s) in 0:\\record:")
        for name, sz in files:
            print(f"  - {name} ({sz:,} bytes)")
        if args.list:
            return

        args.dest.mkdir(parents=True, exist_ok=True)
        synced = 0
        for name, sz in files:
            dst = args.dest / name
            if dst.is_file() and dst.stat().st_size == sz:
                print(f"Skipping already synced: {name} ({sz:,} bytes)")
                continue
            digest = client.download_file(name, sz, dst, burst_size=args.burst_size)
            print(f"  -> Saved {dst} (SHA-256: {digest[:16]}...)")
            synced += 1
        print(f"Bluetooth sync complete ({synced} new file(s) downloaded to {args.dest}).")
    finally:
        client.close()


if __name__ == "__main__":
    main()
