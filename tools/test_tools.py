#!/usr/bin/env python3
"""
Offline regression tests for host safety tools:
  - tools/bt_sync.py: remote filename guard, native-SPP (OpenFW) vs stock HFP start-up detection
  - tools/usb_sync.py: content-hashed collision handling & deduplication
  - tools/fix_exfat_boundary.py: tail-cluster allocation bitmap check,
    MBR/VBR geometry validation, shrink, and idempotency on re-run
  - tools/rf_audio_check.py: its synthetic self-test (clean vs spur / dropout / phase step / AM / noise, and the
    --band-split injections: rumble PASS / thumps REVIEW / BT-like artefacts next to rumble FAIL; needs numpy)

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import bt_sync  # noqa: E402
import fix_exfat_boundary  # noqa: E402
import usb_rtc_sync  # noqa: E402
import usb_sync  # noqa: E402


def test_bt_sync_path_traversal() -> None:
    with tempfile.TemporaryDirectory(prefix="test_bt_sync_") as td:
        root = Path(td)
        ok = bt_sync.safe_dest_path(root, "REC_0001.wav")
        assert ok == root.resolve() / "REC_0001.wav"
        for bad in ("", ".", "..", "../evil.wav", "sub/a.wav", "sub\\a.wav", "/etc/passwd", "REC\x00.wav", "REC\n.wav"):
            rejected = False
            try:
                bt_sync.safe_dest_path(root, bad)
            except ValueError:
                rejected = True
            assert rejected, f"Expected safe_dest_path to reject {bad!r}"
    print("[PASS] bt_sync.py remote filename traversal & control-char guard")


class _FakeRfcomm:
    """Scripted stand-in for the AF_BLUETOOTH RFCOMM socket: recv() pops the script (None = timeout)."""

    def __init__(self, script):
        self.script = list(script)
        self.sent = bytearray()
        self.timeouts = []

    def settimeout(self, t):
        self.timeouts.append(t)

    def connect(self, addr):
        self.addr = addr

    def recv(self, n):
        if not self.script:
            raise bt_sync.socket.timeout()
        item = self.script.pop(0)
        if item is None:
            raise bt_sync.socket.timeout()
        return item

    def sendall(self, data):
        self.sent.extend(data)

    def close(self):
        pass


def _run_handshake(script, transport="auto"):
    fake = _FakeRfcomm(script)
    real_socket, real_sleep = bt_sync.socket.socket, bt_sync.time.sleep
    bt_sync.socket.socket = lambda *a, **k: fake
    bt_sync.time.sleep = lambda s: None
    try:
        c = bt_sync.XRecRFCOMMClient("02:CA:55:00:01:1D", transport=transport)
        c.connect_and_handshake()
    finally:
        bt_sync.socket.socket, bt_sync.time.sleep = real_socket, real_sleep
    return c, fake


def test_bt_sync_transport_detection() -> None:
    # OpenFW: silent after RFCOMM open -> native SPP, nothing sent, probe timeout used once
    c, fake = _run_handshake([None])
    assert fake.sent == b"" and bt_sync.NATIVE_SPP_PROBE_S in fake.timeouts and c.rx_buf == b""
    # forced native: no probe at all
    c, fake = _run_handshake([], transport="native")
    assert fake.sent == b"" and bt_sync.NATIVE_SPP_PROBE_S not in fake.timeouts
    # unexpected non-AT bytes: native, bytes kept for the +XREC reader
    c, fake = _run_handshake([b"+XREC:PONG,x\r\n"])
    assert fake.sent == b"" and bytes(c.rx_buf) == b"+XREC:PONG,x\r\n"
    # stock HW_76 HFP unit: AT+BRSF in the probe read, rest of the SLC after it
    c, fake = _run_handshake([b"AT+BRSF=", b"20\rAT+CIND=?\r", b"AT+CIND?\rAT+CMER=3,0,0,1\r"])
    sent = bytes(fake.sent)
    assert b"+BRSF: 1007" in sent and b"+CIND: 1,0,0,0,5,0,5" in sent and sent.endswith(b"\r\nOK\r\n")
    print("[PASS] bt_sync.py native-SPP (OpenFW) vs stock HFP start-up detection")


def test_bt_sync_split_error_and_keyfile_perms() -> None:
    # 1. Split +XREC:ERR frame must raise RuntimeError immediately instead of misparsing "XREC:ERR,NOE" as binary
    c, _ = _run_handshake([], transport="native")
    assert c.sock is not None
    c.sock.script = [b"+XREC:", b"ERR,NOENT\r\n"]  # type: ignore[attr-defined]
    got_err = ""
    try:
        c.get_chunk("MISSING.WAV", 0, 64)
    except RuntimeError as exc:
        got_err = str(exc)
    assert "NOENT" in got_err, f"Expected NOENT RuntimeError on split error frame, got {got_err!r}"

    # 2. save_device_key must create keyfile atomically with 0600 permissions even under permissive umask / stale .tmp
    with tempfile.TemporaryDirectory(prefix="test_bt_key_") as td:
        kf = Path(td) / "keys.json"
        stale_tmp = kf.with_suffix(".json.tmp")
        old_umask = bt_sync.os.umask(0o000)
        try:
            stale_tmp.write_text("stale\n", encoding="utf-8")
            bt_sync.os.chmod(stale_tmp, 0o666)
            bt_sync.save_device_key(kf, "02:ca:55:00:01:1d", bytes(range(32)))
        finally:
            bt_sync.os.umask(old_umask)
        mode = kf.stat().st_mode & 0o777
        assert mode == 0o600, f"Expected keyfile mode 0o600, got {oct(mode)}"
        assert not stale_tmp.exists(), "Expected .tmp file to be atomically replaced"
        assert bt_sync.load_device_key(kf, "02:CA:55:00:01:1D") == bytes(range(32))
    print("[PASS] bt_sync.py split +XREC:ERR frame handling & 0600 atomic keyfile write")


def test_usb_sync_collisions() -> None:
    with tempfile.TemporaryDirectory(prefix="test_usb_sync_") as td:
        root = Path(td)
        mp = root / "mnt"
        (mp / "record").mkdir(parents=True)
        dest = root / "dest"

        # 1. Initial import + delete
        (mp / "record" / "REC_0001.wav").write_bytes(b"A" * 100)
        usb_sync.sync_recordings(mp, dest, delete_after_sync=True)
        assert (dest / "REC_0001.wav").read_bytes() == b"A" * 100
        assert not (mp / "record" / "REC_0001.wav").exists()

        # 2. Same filename, SAME byte length, DIFFERENT content -> must NOT skip or overwrite
        (mp / "record" / "REC_0001.wav").write_bytes(b"B" * 100)
        usb_sync.sync_recordings(mp, dest, delete_after_sync=True)
        assert (dest / "REC_0001.wav").read_bytes() == b"A" * 100
        assert (dest / "REC_0001_2.wav").read_bytes() == b"B" * 100

        # 3. Same filename, DIFFERENT length, DIFFERENT content -> must NOT overwrite
        (mp / "record" / "REC_0001.wav").write_bytes(b"C" * 200)
        usb_sync.sync_recordings(mp, dest, delete_after_sync=True)
        assert (dest / "REC_0001.wav").read_bytes() == b"A" * 100
        assert (dest / "REC_0001_2.wav").read_bytes() == b"B" * 100
        assert (dest / "REC_0001_3.wav").read_bytes() == b"C" * 200

        # 4. Identical content already archived -> deduplicate cleanly
        (mp / "record" / "REC_0001.wav").write_bytes(b"B" * 100)
        usb_sync.sync_recordings(mp, dest, delete_after_sync=True)
        assert not (dest / "REC_0001_4.wav").exists()
        assert not (mp / "record" / "REC_0001.wav").exists()

        # 5. Destination aliasing device record/ dir (direct or symlink) -> MUST refuse and preserve file
        only_copy = mp / "record" / "REC_ONLY.wav"
        only_copy.write_bytes(b"IRREPLACEABLE")
        for bad_dest in (mp / "record", root / "record_alias"):
            if bad_dest == root / "record_alias":
                bad_dest.symlink_to(mp / "record")
            refused = False
            try:
                usb_sync.sync_recordings(mp, bad_dest, delete_after_sync=True)
            except ValueError:
                refused = True
            assert refused, f"Expected ValueError when dest aliases record/: {bad_dest}"
            assert only_copy.exists() and only_copy.read_bytes() == b"IRREPLACEABLE"

        # 6. Companion .cwx sidecar syncing, collision suffixing, and delete-after-sync cleanup + /cassini mount marker
        (mp / "cassini").mkdir()
        (mp / "record" / "SIDE_0001.wav").write_bytes(b"W1" * 60)
        (mp / "record" / "SIDE_0001.cwx").write_bytes(b"CWX1sidecar_one")
        usb_sync.sync_recordings(mp, dest, delete_after_sync=True)
        assert (dest / "SIDE_0001.wav").read_bytes() == b"W1" * 60
        assert (dest / "SIDE_0001.cwx").read_bytes() == b"CWX1sidecar_one"
        assert not (mp / "record" / "SIDE_0001.wav").exists() and not (mp / "record" / "SIDE_0001.cwx").exists()
        (mp / "record" / "SIDE_0001.wav").write_bytes(b"W2" * 80)
        (mp / "record" / "SIDE_0001.cwx").write_bytes(b"CWX1sidecar_two")
        usb_sync.sync_recordings(mp, dest, delete_after_sync=True)
        assert (dest / "SIDE_0001_2.wav").read_bytes() == b"W2" * 80
        assert (dest / "SIDE_0001_2.cwx").read_bytes() == b"CWX1sidecar_two"
        assert not (mp / "record" / "SIDE_0001.wav").exists() and not (mp / "record" / "SIDE_0001.cwx").exists()
    print("[PASS] usb_sync.py collision, .cwx sidecar sync/delete, deduplication & alias-path safety")


def _make_mbr_exfat_image(td: Path, total_sec: int = 4096, p1_start: int = 64) -> tuple[Path, int]:
    p1_size = total_sec - p1_start
    part_img = td / "part.img"
    disk_img = td / "disk.img"
    subprocess.run(
        ["dd", "if=/dev/zero", f"of={part_img}", "bs=512", f"count={p1_size}", "status=none"],
        check=True,
    )
    mkfs = shutil.which("mkfs.exfat") or "/usr/sbin/mkfs.exfat"
    subprocess.run([mkfs, "-q", "-b", "4096", "-c", "4096", str(part_img)], check=True)
    pb = bytearray(part_img.read_bytes())
    for base in (0, 12):
        off = base * 512
        struct.pack_into("<Q", pb, off + 0x40, p1_start)
        cs = fix_exfat_boundary.exfat_vbr_checksum(bytes(pb[off : off + 11 * 512]))
        pb[off + 11 * 512 : off + 12 * 512] = struct.pack("<I", cs) * 128
    mbr = bytearray(512)
    mbr[0x1BE + 4] = 0x07
    struct.pack_into("<II", mbr, 0x1BE + 8, p1_start, p1_size)
    mbr[510:512] = b"\x55\xaa"
    with open(disk_img, "wb") as f:
        f.write(mbr)
        f.write(b"\x00" * ((p1_start - 1) * 512))
        f.write(pb)
    return disk_img, p1_size


def test_fix_exfat_boundary() -> None:
    with tempfile.TemporaryDirectory(prefix="test_exfat_fix_") as td:
        root = Path(td)
        disk_img, p1_size = _make_mbr_exfat_image(root)

        # 1. Mark tail cluster allocated in bitmap -> MUST refuse to shrink
        with open(disk_img, "r+b", buffering=0) as disk:
            disk.seek(64 * 512)
            vbr = disk.read(512)
            vol_off, vol_len, fat_off, fat_len, clus_off, clus_cnt, root_clus = struct.unpack_from(
                "<QQIIIII", vbr, 0x40
            )
            spc = 1 << vbr[0x6D]
            heap_lba = 64 + clus_off
            bmap_cls, bmap_len = fix_exfat_boundary.find_exfat_bitmap(disk, heap_lba, root_clus, spc)
            tail_idx = clus_cnt - 1
            bmap_byte_off = (heap_lba + (bmap_cls - 2) * spc) * 512 + (tail_idx // 8)
            disk.seek(bmap_byte_off)
            orig_b = disk.read(1)[0]
            disk.seek(bmap_byte_off)
            disk.write(bytes([orig_b | (1 << (tail_idx % 8))]))

        refused = False
        try:
            fix_exfat_boundary.fix_device(disk_img, dry_run=False)
        except RuntimeError as exc:
            refused = "ALLOCATED" in str(exc)
        assert refused, "Expected fix_device to refuse shrinking an allocated tail cluster"

        # Restore free tail cluster bit
        with open(disk_img, "r+b", buffering=0) as disk:
            disk.seek(bmap_byte_off)
            disk.write(bytes([orig_b]))

        # 2. dry_run=True must leave the disk image completely untouched
        before_dry = disk_img.read_bytes()
        assert fix_exfat_boundary.parse_args(["/dev/null", "--dry-run"]).dry_run is True
        fix_exfat_boundary.fix_device(disk_img, dry_run=True)
        assert disk_img.read_bytes() == before_dry

        # 3. NoFatChain (flags=0x03) on 0x81 bitmap entry still resolves cleanly
        with open(disk_img, "r+b", buffering=0) as disk:
            bmap_clus, bmap_len, no_fat = fix_exfat_boundary.find_exfat_bitmap_info(disk, heap_lba, root_clus, spc)
            assert bmap_clus >= 2 and bmap_len >= 63 and no_fat is False

        # 4. Clean shrink by 1 cluster
        fix_exfat_boundary.fix_device(disk_img, dry_run=False)
        _, sz1 = struct.unpack_from("<II", disk_img.read_bytes()[:512], 0x1BE + 8)
        assert sz1 == p1_size - 8

        # 5. Second run must be idempotent
        fix_exfat_boundary.fix_device(disk_img, dry_run=False)
        _, sz2 = struct.unpack_from("<II", disk_img.read_bytes()[:512], 0x1BE + 8)
        assert sz2 == sz1
    print("[PASS] fix_exfat_boundary.py allocated-tail refusal, dry-run, shrink, and idempotency")


def test_rf_audio_check() -> None:
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("[SKIP] rf_audio_check.py self-test (numpy not installed)")
        return
    import rf_audio_check  # noqa: E402

    assert rf_audio_check.self_test() == 0, "rf_audio_check self-test failed"
    print("[PASS] rf_audio_check.py synthetic self-test")


def test_bootrom_pid_guard_and_selftests() -> None:
    # AGENTS.md: never send BootROM 0xC0 commands to stock card-reader PID 0x2801; BootROM must stay 0x2800 only.
    for py_path in sorted(TOOLS_DIR.glob("*.py")):
        if py_path.name == "test_tools.py":
            continue
        src = py_path.read_text(encoding="utf-8", errors="replace")
        has_2801 = ("2801" in src)
        has_bootrom_c0 = ("ctrl_transfer(0xC0" in src or "ctrl_transfer(0x40" in src)
        assert not (has_2801 and has_bootrom_c0), f"{py_path.name} mixes 2801 with USB vendor control transfers"

    # usb_rtc_sync.py must use OpenFW SCSI 0xE0 (1209:0001) and explicitly refuse 301a:2801
    import datetime as _dt

    rtc_src = (TOOLS_DIR / "usb_rtc_sync.py").read_text(encoding="utf-8")
    assert "0x2801" in rtc_src and "0xC0" in rtc_src and "openfw_usb" in rtc_src
    sample_dt = _dt.datetime(2026, 11, 25, 14, 30, 45)
    packed = usb_rtc_sync.pack_rtc_time(sample_dt)
    unpacked_dt, wd = usb_rtc_sync.unpack_rtc_time(packed)
    assert len(packed) == 8 and unpacked_dt == sample_dt and wd == sample_dt.weekday()

    # Execute all offline tool self-tests
    for script, flag in (
        ("flash_ranges.py", "--self-test"),
        ("openfw_usb.py", "--self-test"),
        ("dev_cycle.py", "--self-test"),
        ("asr_prep.py", "--self-test"),
        ("cassini_to_device.py", "--self-test"),
        ("bt_probe_report.py", "--self-test"),
        ("bt_entropy_unpack.py", "--self-test"),
        ("usb_rtc_sync.py", "--dry-run"),
    ):
        subprocess.run([sys.executable, "-I", str(TOOLS_DIR / script), flag], check=True)
    print("[PASS] AGENTS.md BootROM PID guard & all tool --self-test runners")


def main() -> int:
    test_bt_sync_path_traversal()
    test_bt_sync_transport_detection()
    test_bt_sync_split_error_and_keyfile_perms()
    test_usb_sync_collisions()
    test_fix_exfat_boundary()
    test_rf_audio_check()
    test_bootrom_pid_guard_and_selftests()
    print("ALL HOST TOOL SAFETY TESTS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
