#!/usr/bin/env python3
import struct
import pathlib
import zlib
import re

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()
rom = pathlib.Path("dumps/bootrom_64K.bin").read_bytes()
psmp = fw[0x1FC000:0x200000]

print("=== ALL ACTIVE PSMP KEYS & UNIQUE KEY TYPES ===")
off = 0
seen_keys = {}
active = []
while off < len(psmp) - 12 and psmp[off:off+2] == b"\x55\xaa":
    status = psmp[off+2]
    hdr32 = struct.unpack_from("<I", psmp, off+3)[0]
    val_len, key_id, key_len = struct.unpack_from("<HHB", psmp, off+7)
    raw_body = psmp[off+7:off+12+key_len+val_len]
    crc_calc = zlib.crc32(raw_body) & 0xFFFFFFFF
    key = psmp[off+12:off+12+key_len].decode("ascii", errors="replace")
    val = psmp[off+12+key_len:off+12+key_len+val_len]
    seen_keys[key] = (key_id, val_len)
    if status == 0xFF:
        active.append((off, key_id, key, val_len, hdr32, crc_calc, val))
    off += 12 + key_len + val_len

for off, kid, key, vlen, h32, c32, val in active:
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in val)
    hx = val[:32].hex(" ") + ("..." if len(val) > 32 else "")
    print(f"  0x{off:04X}: id={kid:2d} key={key:13s} len={vlen:3d} hdr32=0x{h32:08X} (crc32=0x{c32:08X}) hex={hx}")
    if len(val) > 32 or any(32 <= b < 127 for b in val):
        print(f"          full_hex={val.hex(' ')}")
        print(f"          asc={asc!r}")
print("All seen key types:", seen_keys)

print("\n=== WAV FILE HEADERS IN dumps/sdcard_backup/record ===")
for p in sorted(pathlib.Path("dumps/sdcard_backup/record").glob("*.wav")):
    b = p.read_bytes()[:128]
    riff, sz, wave_id = struct.unpack_from("<4sI4s", b, 0)
    fmt_id, fmt_sz, audio_fmt, chans, rate, byterate, blockalign, bps = struct.unpack_from("<4sIHHIIHH", b, 12)
    print(f"  {p.name}: size={p.stat().st_size:,} fmt=0x{audio_fmt:04X} ch={chans} rate={rate}Hz byterate={byterate}B/s align={blockalign} bps={bps} fmt_chunk_sz={fmt_sz}")

print("\n=== ROM CALL TARGETS FROM FIRM & BOOTLOADER ===")
def decode_bl(buf, base_vma):
    targets = {}
    for i in range(0, len(buf) - 3, 2):
        hw1, hw2 = struct.unpack_from("<HH", buf, i)
        if (hw1 & 0xF800) == 0xF000 and (hw2 & 0xD000) in (0xD000, 0x9000): # BL or B.W
            s = (hw1 >> 10) & 1
            imm10 = hw1 & 0x3FF
            j1 = (hw2 >> 13) & 1
            j2 = (hw2 >> 11) & 1
            imm11 = hw2 & 0x7FF
            i1 = ~(j1 ^ s) & 1
            i2 = ~(j2 ^ s) & 1
            off = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
            if s:
                off -= (1 << 25)
            pc = base_vma + i + 4
            tgt = (pc + off) & 0xFFFFFFFF
            if tgt < 0x00100000:
                targets[tgt] = targets.get(tgt, 0) + 1
    return targets

rom_calls = decode_bl(fw[0x00E000:0x173E8C], 0x00C0E000)
print(f"  Total distinct <1MB BL/B.W targets from FIRM: {len(rom_calls)}")
top_rom = sorted(rom_calls.items(), key=lambda x: (-x[1], x[0]))
print("  Top 25 most-called ROM addresses:", [(hex(k), v) for k, v in top_rom[:25]])
above_64k = [(hex(k), v) for k, v in sorted(rom_calls.items()) if k >= 0x10000]
print(f"  ROM targets >= 0x10000 (above 64KB!): count={len(above_64k)}, sample={above_64k[:15]} ... {above_64k[-15:] if above_64k else []}")
