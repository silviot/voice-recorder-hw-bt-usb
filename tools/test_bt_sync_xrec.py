#!/usr/bin/env python3
"""
Offline tests for tools/bt_sync.py +XREC transfers against a scripted fake device (no Bluetooth):
  - put_file / upload_cwx: happy path, late OK after a timeout (never one reply behind), lost PUTEND reply
    (idempotent PUTEND on current firmware, LS check on older firmware), refused header + resync (current and old
    firmware), fatal SDIO, CRC retry, NOENT, local name checks
  - get_burst / download_file: EOF inside a burst (short frame, no N=0 frame), short first frame at EOF (timeout),
    MTU-clamped frames, file shorter than its LS size

Run: python3 tools/test_bt_sync_xrec.py   (also run by `make test-host` in open_fw/)
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import re
import struct
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import bt_sync  # noqa: E402

TIMEOUT = object()   # queue marker: the next recv() times out
DEVICE_CRC = bt_sync.crc16_ccitt_false   # the device side always checks with the real CRC


class FakeDevice:
    """Reactive stand-in for the RFCOMM socket: sendall() feeds a small model of the open_fw +XREC PUT/GETS
    handlers, recv() pops queued replies (TIMEOUT or an empty queue raises socket.timeout)."""

    def __init__(self, files=None, old_fw=False, putend_idempotent=True):
        self.files = dict(files or {})          # "/record/NAME" -> bytes (what the card holds)
        self.old_fw = old_fw                    # no REC target (ERR,ARG) and payload fed to the line parser
        self.putend_idempotent = putend_idempotent
        self.inbuf = bytearray()
        self.out = []                           # queued recv() items: bytes or TIMEOUT
        self.put_sends = []                     # (offset, len) of every PUT header received
        self.staged = None                      # [name, bytearray] of the staged REC upload
        self.last = None                        # (off, len, crc, name) of the last written chunk
        self.done_total = None
        self.delay_put_reply_at = set()         # offsets whose (first) reply arrives after one timeout
        self.lose_putend_reply = 0              # PUTEND replies to swallow (a timeout instead)
        self.fail_put_at = {}                   # offset -> error sent instead of OK (once)
        self.corrupt_put_at = set()             # offsets whose payload arrives corrupted once
        self.gets_frames = None                 # override: list of payload sizes the next GETS answers with
        self.dbg_calls = 0                      # +XREC:DBG (bench images)
        self.recording = self.rec_done = False
        self.pace_bps = 48000
        self.gets_bytes = 0
        self.dels = 0

    # --- socket API ---
    def settimeout(self, t):
        pass

    def close(self):
        pass

    def recv(self, n):
        if not self.out:
            raise bt_sync.socket.timeout()
        item = self.out.pop(0)
        if item is TIMEOUT:
            raise bt_sync.socket.timeout()
        return item

    def sendall(self, data):
        self.inbuf.extend(data)
        self._run()

    # --- device model ---
    def _reply(self, line):
        self.out.append(line.encode("ascii") + b"\r\n")

    def _run(self):
        while True:
            m = re.match(rb"\+XREC:PUT,(\d+),(\d+),(\d+),([^:\r]*):", bytes(self.inbuf))
            if m:
                off, ln, crc, tgt = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4).decode()
                if len(self.inbuf) < m.end() + ln:
                    return
                payload = bytes(self.inbuf[m.end() : m.end() + ln])
                del self.inbuf[: m.end() + ln]
                self._put(off, ln, crc, tgt, payload)
                continue
            cr = self.inbuf.find(b"\r")
            if cr < 0:
                return
            line = bytes(self.inbuf[:cr]).decode("ascii", errors="replace").lstrip("\n")
            del self.inbuf[: cr + 1]
            if len(line) >= 192:
                self._reply("+XREC:ERR,LINE")
            elif line:
                self._cmd(line)

    def _put(self, off, ln, crc, tgt, payload):
        self.put_sends.append((off, ln))
        if not tgt.startswith("REC,") or self.old_fw:
            self._reply("+XREC:ERR,ARG")
            if self.old_fw:   # old firmware: the payload goes to the line parser
                self.inbuf[0:0] = payload
            return
        name = tgt[4:]
        if not re.fullmatch(r"[\x20-\x7e]{1,26}\.[cC][wW][xX]", name) or name.startswith("."):
            self._reply("+XREC:ERR,NAME")
            return
        if off in self.corrupt_put_at:
            self.corrupt_put_at.discard(off)
            payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
        if DEVICE_CRC(payload) != crc:
            self._reply("+XREC:ERR,CRC")
            return
        if off in self.fail_put_at:
            err = self.fail_put_at.pop(off)
            if err == "+XREC:ERR,SDIO":
                self.staged = None
            self._reply(err)
            return
        self.done_total = None
        if self.staged and self.last == (off, ln, crc, name):
            self._put_ok(off, ln)
            return
        if off == 0:
            wav = "/record/" + name[:-4] + ".wav"
            if not any(k.lower() == wav.lower() for k in self.files):
                self._reply("+XREC:ERR,NOENT")
                return
            self.staged = [name, bytearray()]
        elif not self.staged or self.staged[0] != name or off != len(self.staged[1]):
            self._reply("+XREC:ERR,OFF")
            return
        self.staged[1].extend(payload)
        self.last = (off, ln, crc, name)
        self._put_ok(off, ln)

    def _put_ok(self, off, ln):
        if off in self.delay_put_reply_at:
            self.delay_put_reply_at.discard(off)
            self.out.append(TIMEOUT)
        self._reply(f"+XREC:OK,PUT,{off},{ln}")

    def _cmd(self, line):
        if line == "+XREC:PING":
            self._reply("+XREC:PONG,openfw_v5,v5,bound=1,auth=1,pair=0")
        elif line == "+XREC:PUTEND":
            if self.staged:
                name, data = self.staged
                self.files["/record/" + name] = bytes(data)
                self.staged, self.last = None, None
                self.done_total = len(data) if self.putend_idempotent else None
                total = len(data)
            elif self.done_total is not None:
                total = self.done_total
            else:
                self._reply("+XREC:ERR,NOPUT")
                return
            if self.lose_putend_reply:
                self.lose_putend_reply -= 1
                self.out.append(TIMEOUT)
                return
            self._reply(f"+XREC:OK,PUTEND,{total}")
        elif line == "+XREC:LS":
            for k, v in self.files.items():
                if k.startswith("/record/"):
                    self._reply(f"+XREC:F,{len(v)},{k[len('/record/'):]}")
            self._reply("+XREC:END")
        elif line == "+XREC:DBG,PACE" or line.startswith("+XREC:DBG,PACE,"):
            if line != "+XREC:DBG,PACE":
                self.pace_bps = int(line.split(",")[2])
            self._reply(f"+XREC:DBG,PACE,rate={self.pace_bps},budget_us=5000,ring_waits=0,budget_stops=0,cap_stops=0,"
                        "later=0,busy=0,rec_b=0")
        elif line == "+XREC:DBG" or line.startswith("+XREC:DBG,"):
            self.dbg_calls += 1
            if line == "+XREC:DBG":
                self._reply(f"+XREC:DBG,LIVE,seq=1,file=REC_0001.wav,rec={int(self.recording)},ov=0,gets_tot={self.gets_bytes}")
            elif line == "+XREC:DBG,REC,0" and self.rec_done:
                self._reply("+XREC:DBG,REC,0,seq=1,file=REC_0001.wav,rec=0,ov=0,acct=0")
            else:
                self._reply("+XREC:ERR,NOENT")
        elif line == "+XREC:STAT":
            # open_fw with sync while recording: fields 9 (rec_idx) and 10 (pace_ms); old_fw: the 8 fields
            self._reply("+XREC:STAT,80,3900,0,0,1,12,1000,30000" + ("" if self.old_fw else
                                                                    (",1,342" if self.recording else ",0,0")))
        elif line.startswith("+XREC:DEL,"):
            self.dels += 1
            self._reply("+XREC:ERR,DEL")
        elif line == "+XREC:RECSTART":
            self.recording = True
            self._reply("+XREC:OK,RECSTART,REC_0001.wav")
        elif line == "+XREC:RECSTOP":
            self.recording, self.rec_done = False, True
            self._reply("+XREC:OK,RECSTOP")
        elif line.startswith("+XREC:GETS,"):
            off, tot, chunk, name = line[len("+XREC:GETS,") :].split(",", 3)
            self._gets(int(off), int(tot), int(chunk), name)
        elif line.startswith("+XREC:"):
            self._reply("+XREC:ERR,CMD")

    def _frame(self, off, payload):
        crc = DEVICE_CRC(payload)
        self.out.append(struct.pack("<4sIHH", b"XREC", off, len(payload), crc) + payload)

    def _gets(self, off, tot, chunk, name):
        data = self.files.get("/record/" + name)
        if data is None:
            self._reply("+XREC:ERR,NOENT")
            return
        if self.gets_frames is not None:
            chunk = self.gets_frames      # MTU clamp emulation: frame size the device really uses
        rem = tot
        if off >= len(data):
            self._frame(off, b"")
            return
        # xrec_poll: frames of min(rem, chunk); a short read (EOF) ends the burst without an N=0 frame
        while rem > 0:
            want = min(rem, chunk)
            got = data[off : off + want]
            self.gets_bytes += len(got)
            self._frame(off, got)
            off += len(got)
            rem -= len(got)
            if len(got) < want:
                break


def client_for(dev: FakeDevice) -> bt_sync.XRecRFCOMMClient:
    c = bt_sync.XRecRFCOMMClient("02:CA:55:00:01:1D", transport="native")
    c.sock = dev  # type: ignore[assignment]
    return c


def expect_raises(fn, *needles):
    try:
        fn()
    except (RuntimeError, ValueError) as exc:
        for n in needles:
            assert n in str(exc), f"{n!r} not in {exc!r}"
        return str(exc)
    raise AssertionError(f"expected an exception containing {needles}")


def cwx_bytes(n: int) -> bytes:
    return b"CWX1" + bytes((i * 7) & 0xFF for i in range(n - 4))


def test_put_happy_and_late_replies(td: Path) -> None:
    data = cwx_bytes(1500)
    src = td / "REC_0007.cwx"
    src.write_bytes(data)

    dev = FakeDevice({"/record/REC_0007.wav": b"w" * 100})
    assert client_for(dev).upload_cwx(src, "REC_0007.cwx") == 1500
    assert dev.files["/record/REC_0007.cwx"] == data and dev.put_sends == [(0, 600), (600, 600), (1200, 300)]

    # late OK for chunk 600: client times out and resends, device acks the identical resend again. The stale
    # duplicate must be dropped, not taken as the reply to the next chunk or to PUTEND.
    dev = FakeDevice({"/record/REC_0007.wav": b"w"})
    dev.delay_put_reply_at = {600}
    assert client_for(dev).upload_cwx(src, "REC_0007.cwx") == 1500
    assert dev.files["/record/REC_0007.cwx"] == data
    assert dev.put_sends == [(0, 600), (600, 600), (600, 600), (1200, 300)], dev.put_sends
    assert not dev.out

    # same, on the last chunk (the stale OK sits in front of the PUTEND reply)
    dev = FakeDevice({"/record/REC_0007.wav": b"w"})
    dev.delay_put_reply_at = {1200}
    assert client_for(dev).upload_cwx(src, "REC_0007.cwx") == 1500

    # lost PUTEND reply: current firmware answers the repeated PUTEND with the same OK
    dev = FakeDevice({"/record/REC_0007.wav": b"w"})
    dev.lose_putend_reply = 1
    assert client_for(dev).upload_cwx(src, "REC_0007.cwx") == 1500
    # older firmware: repeated PUTEND -> ERR,NOPUT, the client confirms the commit with LS (name + size)
    dev = FakeDevice({"/record/REC_0007.wav": b"w"}, putend_idempotent=False)
    dev.lose_putend_reply = 1
    assert client_for(dev).upload_cwx(src, "REC_0007.cwx") == 1500

    # payload corrupted on the link once: ERR,CRC is retried with the same chunk
    dev = FakeDevice({"/record/REC_0007.wav": b"w"})
    dev.corrupt_put_at = {600}
    assert client_for(dev).put_file("REC,REC_0007.cwx", data) == 1500
    assert dev.files["/record/REC_0007.cwx"] == data and dev.put_sends.count((600, 600)) == 2
    print("[PASS] bt_sync put_file: chunks, late OK after timeout, lost PUTEND reply (both firmwares), CRC retry")


def test_put_errors(td: Path) -> None:
    data = cwx_bytes(1500)
    src = td / "x.cwx"
    src.write_bytes(data)

    # SDIO mid-upload: fatal at once, the real error is reported (not ERR,OFF after futile retries)
    dev = FakeDevice({"/record/REC_0001.wav": b"w"})
    dev.fail_put_at = {600: "+XREC:ERR,SDIO"}
    expect_raises(lambda: client_for(dev).upload_cwx(src, "REC_0001.cwx"), "ERR,SDIO", "offset 600")
    assert dev.put_sends == [(0, 600), (600, 600)]

    # no recording for that stem
    dev = FakeDevice({})
    expect_raises(lambda: client_for(dev).upload_cwx(src, "REC_0009.cwx"), "no matching .wav")

    # refused header (ERR,NAME from the device): resync with CR + PING, the session stays usable
    dev = FakeDevice({"/record/REC_0001.wav": b"w"})
    c = client_for(dev)
    expect_raises(lambda: c.put_file("REC,REC_0001.cwx.wav.cwx" + "x" * 10 + ".cwx", data), "refused", "ERR,NAME")
    assert c.ping().startswith("+XREC:PONG") and not dev.out

    # old firmware: ERR,ARG, the 600 B payload went to its line parser (extra ERR,LINE / ERR,CMD replies). The
    # client reports "too old" and drains everything up to its PONG.
    dev = FakeDevice({"/record/REC_0001.wav": b"w"}, old_fw=True)
    payload = b"CWX1" + b"y" * 300 + b"\r+XREC:FOO\r" + b"z" * 285
    src2 = td / "y.cwx"
    src2.write_bytes(payload)
    c = client_for(dev)
    expect_raises(lambda: c.upload_cwx(src2, "REC_0001.cwx"), "firmware too old")
    assert dev.put_sends == [(0, 600)]
    assert c.ping().startswith("+XREC:PONG") and not dev.out and not c.rx_buf

    # local name checks (never sent)
    dev = FakeDevice({})
    c = client_for(dev)
    for bad in ("REC_0001.wav", ".x.cwx", "a/b.cwx", "a:b.cwx", "x" * 27 + ".cwx", "é.cwx", ".cwx"):
        expect_raises(lambda b=bad: c.upload_cwx(src, b), "invalid sidecar name")
    (td / "notcwx.cwx").write_bytes(b"RIFF....")
    expect_raises(lambda: c.upload_cwx(td / "notcwx.cwx", "REC_0001.cwx"), "not a CWX1")
    assert dev.put_sends == []
    print("[PASS] bt_sync put_file: SDIO fatal, NOENT, refused header resync (current + old firmware), name checks")


def test_get_burst_eof(td: Path) -> None:
    data = bytes((i * 31) & 0xFF for i in range(1380))
    # EOF inside a burst: frames 640, 640, 100 and no N=0 frame (file shorter than requested)
    dev = FakeDevice({"/record/A.wav": data})
    c = client_for(dev)
    assert c.get_burst("A.wav", 0, 3840, 640) == data and not dev.out
    # short first frame that is EOF: ends on the timeout with the partial data
    dev = FakeDevice({"/record/A.wav": data})
    assert client_for(dev).get_burst("A.wav", 1300, 640, 640) == data[1300:]
    # MTU-clamped frames (device sends 500 B frames for chunk 640): a short first frame is not EOF
    dev = FakeDevice({"/record/A.wav": data})
    dev.gets_frames = 500
    assert client_for(dev).get_burst("A.wav", 0, 1380, 640) == data and not dev.out
    # at EOF: one N=0 frame
    dev = FakeDevice({"/record/A.wav": data})
    assert client_for(dev).get_burst("A.wav", 1380, 640, 640) == b""
    # download_file of a file that shrank after LS (LS said 5000): saves what the card holds, no retries/raise
    dev = FakeDevice({"/record/A.wav": data})
    dst = td / "A.wav"
    client_for(dev).download_file("A.wav", 5000, dst)
    assert dst.read_bytes() == data
    print("[PASS] bt_sync get_burst: EOF inside a burst, short EOF first frame, MTU-clamped frames, shrunk file")


def test_bench_dbg_and_load() -> None:
    dev = FakeDevice({"/record/OLD.wav": bytes(range(256)) * 20})
    c = client_for(dev)
    d = c.dbg()
    assert d["rec"] == "0" and d["file"] == "REC_0001.wav" and d["_line"].startswith("+XREC:DBG,LIVE,")
    expect_raises(lambda: c.dbg(0), "DBG failed", "NOENT")
    lines = []
    res = c.bench_load(0.4, "OLD.wav", 5120, burst_size=3840, chunk_size=640, kbps=0, dbg_every=0.1, rec=True,
                       log=lines.append)
    assert res["bytes"] > 5120 and res["errors"] == 0, res                 # wrapped around the file at least once
    assert dev.dbg_calls >= 4 and dev.rec_done, dev.dbg_calls               # polls + the final REC,0
    assert "RECSTART REC_0001.wav" in lines[0] and any("RECSTOP" in ln for ln in lines)
    assert any("+XREC:DBG,REC,0," in ln for ln in lines) and "LOAD bytes=" in lines[-1]
    # capped: about kbps * seconds (one burst of slack)
    dev = FakeDevice({"/record/OLD.wav": bytes(10000)})
    res = client_for(dev).bench_load(0.5, "OLD.wav", 10000, burst_size=1024, chunk_size=512, kbps=8, dbg_every=0,
                                     log=lambda s: None, storm_every=0.1)
    assert res["bytes"] <= 8 * 1024 * 0.5 + 1024 + 1, res
    assert res["storms"] >= 3 and dev.dels == res["storms"] and dev.files, res   # DEL of a missing name only
    # STAT: 10 fields from firmware with sync while recording, 8 from older firmware (keys left out)
    dev = FakeDevice({})
    c = client_for(dev)
    st = c.get_stat()
    assert st["total_mb"] == 30000 and st["rec_idx"] == 0 and st["pace_ms"] == 0, st
    dev.recording = True
    st = c.get_stat()
    assert st["rec_idx"] == 1 and st["pace_ms"] == 342, st
    st = client_for(FakeDevice({}, old_fw=True)).get_stat()
    assert st["total_mb"] == 30000 and "rec_idx" not in st and "pace_ms" not in st, st
    # DBG,PACE: read / set, and bench_load(pace_bps=...) sets the cap first and logs the counters last
    assert c.pace()["rate"] == "48000"
    assert c.pace(24000)["rate"] == "24000" and dev.pace_bps == 24000
    dev = FakeDevice({"/record/OLD.wav": bytes(4000)})
    lines = []
    client_for(dev).bench_load(0.2, "OLD.wav", 4000, burst_size=1024, chunk_size=512, dbg_every=0, log=lines.append,
                               pace_bps=64000)
    assert dev.pace_bps == 64000 and "+XREC:DBG,PACE,rate=64000" in lines[0], lines
    assert sum("+XREC:DBG,PACE," in ln for ln in lines) == 2, lines
    print("[PASS] bt_sync +XREC:DBG and --bench-load (RECSTART, GETS loop with wrap and cap, DBG polls, STAT/LS/DEL storm, RECSTOP, STAT 8/10 fields, DBG,PACE)")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="test_bt_sync_xrec_") as t:
        td = Path(t)
        test_put_happy_and_late_replies(td)
        test_put_errors(td)
        test_get_burst_eof(td)
    test_bench_dbg_and_load()
    print("bt_sync +XREC transfer tests: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
