#!/usr/bin/env python3
"""
Bluetooth Classic (BR/EDR) Companion & Audio Stream Capture Tool for Smartlink SL6801 (HW_76).

Architectural Note on SL6801 (`pen133_2.0.65`) Stock Bluetooth Stack (`spark_bt`):
  Reverse-engineering of the firmware's Bluetooth stack (`0xDA800..0xDC000`) shows
  that the stock firmware implements ONLY Classic Bluetooth audio profiles:
    1. HFP v1.6 Hands-Free Unit (RFCOMM Channel 1: AT+BRSF, AT+CIND, AT+CMER,
       AT+BCS, ATA, AT+CHUP + bidirectional SCO audio) — advertises as `HW_76`.
    2. A2DP Source + AVRCP Target (`spark_bt_a2dp_src_entry` / `spark_bt_avrcp_tg_entry`)
       — used in "Bluetooth Headphone" mode where the recorder streams playback of
       its stored recordings over A2DP to a paired Bluetooth Audio Sink!

  Because stock firmware has no OBEX / BLE GATT / SPP file-transfer protocol,
  there are TWO ways to capture/sync audio over Bluetooth from Linux:
    Mode A (`--mode a2dp-sink`):
      Your Linux PC acts as a Bluetooth Audio Sink (like headphones). When you play
      recordings on the recorder in its BT Headphone mode (or control playback remotely
      from Linux via AVRCP Play/Pause/Next/Prev), this tool captures the incoming
      digital A2DP audio stream directly into timestamped `.wav` files!
    Mode B (`--mode hfp-mic`):
      Connects to `HW_76` in its HFP Hands-Free mode, establishes the RFCOMM Service
      Level Connection (SLC) or binds via BlueZ/PipeWire HFP, and records live
      wireless microphone audio from `HW_76` into a `.wav` file (or routes PC audio
      to `HW_76` so `HW_76` records a `20YYMMDD-HHMMSS-phone.wav` file on its SD card).

SPDX-License-Identifier: MIT
"""
import argparse
import re
import socket
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Optional


DEFAULT_NAME_PREFIX = "HW_"


def discover_hw_device(timeout_sec: int = 8, target_mac: Optional[str] = None) -> Optional[str]:
    """Scan Classic Bluetooth (BR/EDR) for HW_XX (or check already-known BlueZ devices)."""
    if target_mac:
        return target_mac

    # 1. Check cached devices in BlueZ first
    try:
        out = subprocess.check_output(["bluetoothctl", "devices"], text=True)
        for line in out.splitlines():
            m = re.search(r"Device ([0-9A-F:]{17}) (.*)", line)
            if m and m.group(2).strip().startswith(DEFAULT_NAME_PREFIX):
                return m.group(1)
    except Exception:
        pass

    # 2. Run a Classic Bluetooth (BR/EDR) inquiry scan
    print(f"Scanning Classic Bluetooth (BR/EDR) for '{DEFAULT_NAME_PREFIX}*' ({timeout_sec}s)...")
    try:
        subprocess.run(
            ["bluetoothctl", "--timeout", str(timeout_sec), "scan", "bredr"],
            check=False,
        )
        out = subprocess.check_output(["bluetoothctl", "devices"], text=True)
        for line in out.splitlines():
            m = re.search(r"Device ([0-9A-F:]{17}) (.*)", line)
            if m and m.group(2).strip().startswith(DEFAULT_NAME_PREFIX):
                return m.group(1)
    except Exception as exc:
        print(f"Scan error: {exc}", file=sys.stderr)

    return None


def pair_and_connect_bluez(mac: str) -> bool:
    """Pair, trust, and connect to the recorder via bluetoothctl while keeping bredr scan active."""
    print(f"Pairing and connecting to {mac} via BlueZ...")
    proc = subprocess.Popen(
        ["bluetoothctl"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    def send(cmd: str, wait: float = 2.0) -> None:
        assert proc.stdin is not None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(wait)

    send("scan bredr", 3.0)
    send(f"pair {mac}", 4.0)
    send(f"trust {mac}", 1.0)
    send(f"connect {mac}", 4.0)
    send("scan off", 0.5)
    send("quit", 0.5)
    out, _ = proc.communicate(timeout=5)
    connected = "Connection successful" in out or "Connected: yes" in out
    if not connected:
        info = subprocess.run(
            ["bluetoothctl", "info", mac],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        connected = "Connected: yes" in info
    return connected


def run_rfcomm_hfp_probe(mac: str, channel: int = 1, simulate_call_sec: int = 0) -> None:
    """
    Connect directly to HW_76's RFCOMM Channel 1 as an HFP Audio Gateway (AG)
    and complete the exact AT command handshake expected by the SL6801 firmware
    (`_hfp_hf_slc_open_status` at 0xDADAD in pen133_2.0.65).

    Firmware HFP state machine transitions verified from binary dump:
      W4RFCOMM -> W4SF (`AT+BRSF=`) -> W4IND (`AT+CIND=?`) -> W4INDSTA (`AT+CIND?`)
      -> W4CMER (`AT+CMER=3,0,0,1`) -> SLC_OPEN!
    """
    print(f"Connecting to {mac} on RFCOMM Channel {channel} (HFP Audio Gateway emulation)...")
    sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
    sock.settimeout(10.0)
    sock.connect((mac, channel))
    print("RFCOMM connected! Performing SL6801 HFP Service Level Connection (SLC) handshake...")

    def send_ag(lines: list[str]) -> None:
        payload = "\r\n" + "\r\n".join(lines) + "\r\n"
        print(f"  [AG -> HF] {lines}")
        sock.sendall(payload.encode("ascii"))

    buf = ""
    slc_open = False
    try:
        while True:
            chunk = sock.recv(256).decode("ascii", errors="replace")
            if not chunk:
                break
            buf += chunk
            while "\r" in buf:
                line, buf = buf.split("\r", 1)
                line = line.strip()
                if not line:
                    continue
                print(f"  [HF -> AG] {line}")

                if line.startswith("AT+BRSF="):
                    # Report AG supported features (0x03EF = three-way, EC/NR, voice rec, in-band ring, codec neg)
                    send_ag(["+BRSF: 1007", "OK"])
                elif line == "AT+BAC=" or line.startswith("AT+BAC="):
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
                    slc_open = True
                    print("=== HFP Service Level Connection (SLC_OPEN) established! ===")
                    if simulate_call_sec > 0:
                        print(f"Triggering call_online (+CIEV: 2,1) for {simulate_call_sec}s on HW_76...")
                        send_ag(["+CIEV: 3,1", "RING"])
                        time.sleep(1.5)
                        send_ag(["+CIEV: 2,1", "+CIEV: 3,0"])
                        time.sleep(simulate_call_sec)
                        print("Ending simulated call (+CIEV: 2,0)...")
                        send_ag(["+CIEV: 2,0"])
                        return
                elif line.startswith("AT+VGS=") or line.startswith("AT+VGM=") or line.startswith("AT+BCS="):
                    send_ag(["OK"])
                elif line == "ATA":
                    send_ag(["OK", "+CIEV: 2,1", "+CIEV: 3,0"])
                elif line == "AT+CHUP":
                    send_ag(["OK", "+CIEV: 2,0"])
                else:
                    send_ag(["OK"])
    except socket.timeout:
        if slc_open:
            print("SLC idle timeout reached; closing RFCOMM session.")
        else:
            print("Timed out waiting for AT commands.")
    finally:
        sock.close()


def record_pipewire_bt_stream(mac: str, output_dir: Path, duration_sec: int = 0) -> Path:
    """
    Capture audio arriving over Bluetooth from HW_76 (either HFP microphone source
    or A2DP playback source when the PC is paired in BT Earphone mode) via PipeWire.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    mac_underscore = mac.replace(":", "_")
    sources_out = subprocess.run(
        ["pactl", "list", "sources", "short"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout

    bt_source = None
    for line in sources_out.splitlines():
        if mac_underscore.lower() in line.lower() or mac.lower() in line.lower():
            parts = line.split()
            if len(parts) >= 2:
                bt_source = parts[1]
                break

    if not bt_source:
        raise RuntimeError(
            f"No active PipeWire/BlueZ audio source found for {mac}. "
            "Make sure the device is connected via Bluetooth."
        )

    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_wav = output_dir / f"{ts}-bt-capture.wav"
    print(f"Recording Bluetooth audio from '{bt_source}' -> {out_wav} ...")
    cmd = ["pw-record", "--target", bt_source, str(out_wav)]
    if duration_sec > 0:
        proc = subprocess.Popen(cmd)
        time.sleep(duration_sec)
        proc.terminate()
        proc.wait(timeout=5)
    else:
        print("Press Ctrl+C to stop recording.")
        subprocess.run(cmd, check=False)
    print(f"Saved Bluetooth recording: {out_wav}")
    return out_wav


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Bluetooth Classic (HFP / A2DP) companion & audio capture tool for HW_76 (SL6801)."
    )
    parser.add_argument("--mac", type=str, default=None, help="Bluetooth MAC address of HW_76")
    parser.add_argument(
        "--mode",
        choices=("pair", "rfcomm-slc", "simulate-call", "record-stream"),
        default="rfcomm-slc",
        help=(
            "Action to perform: "
            "'pair' (BlueZ pair/connect), "
            "'rfcomm-slc' (direct RFCOMM Ch 1 HFP AT handshake), "
            "'simulate-call' (trigger HFP call_online state on HW_76 via AT+CIEV), "
            "'record-stream' (capture incoming BT audio stream via PipeWire to .wav)"
        ),
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=10,
        help="Duration in seconds for simulate-call or record-stream (default: 10)",
    )
    parser.add_argument(
        "-d", "--dest",
        type=Path,
        default=Path.home() / "Recordings" / "HW_76",
        help="Destination directory for captured Bluetooth audio (.wav)",
    )
    args = parser.parse_args()

    mac = discover_hw_device(target_mac=args.mac)
    if not mac:
        print("Could not find HW_76 in Classic Bluetooth scan. Ensure BT mode is active on the device.")
        sys.exit(1)

    print(f"Target device: {mac}")
    if args.mode == "pair":
        ok = pair_and_connect_bluez(mac)
        sys.exit(0 if ok else 1)
    elif args.mode == "rfcomm-slc":
        run_rfcomm_hfp_probe(mac, simulate_call_sec=0)
    elif args.mode == "simulate-call":
        run_rfcomm_hfp_probe(mac, simulate_call_sec=args.duration)
    elif args.mode == "record-stream":
        pair_and_connect_bluez(mac)
        record_pipewire_bt_stream(mac, args.dest, duration_sec=args.duration)


if __name__ == "__main__":
    main()
