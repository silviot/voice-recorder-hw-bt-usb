#!/usr/bin/env python3
import struct
import pathlib
import subprocess
import re

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()
rom = pathlib.Path("dumps/bootrom_64K.bin").read_bytes()

print("=== 1. ALL STRINGS IN BT HOST STACK (flash 0x0D6700..0x0DD400) ===")
for m in re.finditer(rb"[ -~]{2,}\x00", fw[0x0D6700:0x0DD400]):
    s = m.group()[:-1].decode("ascii")
    off = 0x0D6700 + m.start()
    if any(k in s for k in ["AT", "+", "OK", "RING", "ERROR", "spark_bt", "entry", "uuid", "psm", "op_id", "avrcp", "hfp", "hf ", "spp", "ble", "gatt", "sdp", "codec", "bcs", "cind", "ciev", "clip", "chup", "ata", "vgs", "vgm", "bvra", "xa", "iphone", "bat"]):
        print(f"  0x{off:06X}: {s!r}")

print("\n=== 2. CLI STRINGS AROUND 0x0BAC00..0x0BB400 ===")
for m in re.finditer(rb"[ -~]{3,}\x00", fw[0x0BAC00:0x0BB400]):
    print(f"  0x{0x0BAC00+m.start():06X}: {m.group()[:-1].decode('ascii')!r}")

print("\n=== 3. ENGLISH UI STRINGS IN pen133_2.0.65 (around 0x054E00..0x055B00) ===")
for idx, m in enumerate(re.finditer(rb"[^\x00]{1,}\x00", fw[0x054EB0:0x055AB0])):
    raw_s = m.group()[:-1]
    try:
        s = raw_s.decode("utf-8")
        if any(c.isprintable() for c in s):
            print(f"  0x{0x054EB0+m.start():06X}: {s!r}")
    except UnicodeDecodeError:
        pass

print("\n=== 4. BootROM USB Descriptors (ROM 0xDE80..0xE020) ===")
for off in range(0xDEE0, 0xDFC0, 16):
    hx = " ".join(f"{b:02x}" for b in rom[off:off+16])
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in rom[off:off+16])
    print(f"  ROM 0x{off:04X}: {hx}  {asc}")
