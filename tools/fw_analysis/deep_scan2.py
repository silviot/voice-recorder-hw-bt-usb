#!/usr/bin/env python3
import struct
import pathlib
import subprocess
import re

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()
rom = pathlib.Path("dumps/bootrom_64K.bin").read_bytes()

def make_elf32_arm(segments):
    ehdr_sz = 52
    phdr_sz = 32
    phnum = len(segments)
    off = ehdr_sz + phdr_sz * phnum
    phdrs = bytearray()
    payload = bytearray()
    for vma, data in segments:
        phdrs += struct.pack("<IIIIIIII", 1, off, vma, vma, len(data), len(data), 5, 4)
        payload += data
        off += len(data)
    e_ident = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    ehdr = e_ident + struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, segments[0][0] | 1, ehdr_sz, 0, 0x05000000, ehdr_sz, phdr_sz, phnum, 0, 0, 0)
    return bytes(ehdr + phdrs + payload)

def disasm(raw_bytes, vma, count_insns=80):
    tmp = pathlib.Path("/tmp/chunk_dis.elf")
    tmp.write_bytes(make_elf32_arm([(vma, raw_bytes)]))
    cmd = ["/usr/lib/llvm-21/bin/llvm-objdump", "-d", "--triple=thumbv7em-none-eabi", str(tmp)]
    out = subprocess.check_output(cmd, text=True)
    lines = [l for l in out.splitlines() if re.match(r"^\s*[0-9a-fA-F]+:", l)]
    return "\n".join(lines[:count_insns])

print("=== FIRM 0x805200 (file 0x00E030 + 0xA00 = 0x00EA30) ===")
print(disasm(fw[0x00EA30:0x00EB60], 0x00805200, 60))

print("\n=== FIRM Internal Entropy / Zero / Data Map (0x00E000..0x173E8C) ===")
# Find large zero runs (>512 bytes) or 0xFF runs inside FIRM
firm = fw[0x00E000:0x173E8C]
for m in re.finditer(rb"\x00{512,}", firm):
    s, e = m.start(), m.end()
    print(f"  Zero run: FIRM+0x{s:06X}..+0x{e:06X} (flash 0x{0xE000+s:06X}..0x{0xE000+e:06X}, len=0x{e-s:X} = {e-s:,} B)")
for m in re.finditer(rb"\xff{512,}", firm):
    s, e = m.start(), m.end()
    print(f"  0xFF run: FIRM+0x{s:06X}..+0x{e:06X} (flash 0x{0xE000+s:06X}..0x{0xE000+e:06X}, len=0x{e-s:X} = {e-s:,} B)")

print("\n=== BootROM Literal Pool at 0xA0..0x100 ===")
for off in range(0xA0, 0xE0, 4):
    val = struct.unpack_from("<I", rom, off)[0]
    print(f"  0x{off:04X}: 0x{val:08X}")

print("\n=== Search USBC / USB / SCSI / VID:PID in BootROM & Firmware ===")
for label, blob, base in [("BootROM", rom, 0), ("Flash", fw, 0)]:
    for pat_name, pat in [
        ("USBC_LE", b"USBC"),
        ("USBC_BE", b"CBSU"),
        ("VID_301A", b"\x1a\x30\x00\x28"),
        ("VID_301A_2801", b"\x1a\x30\x01\x28"),
        ("VID_301A_any", b"\x1a\x30"),
        ("SMTLINK", b"SMTLINK"),
        ("20201111", b"20201111"),
        ("/dev/usbd", b"/dev/usbd"),
    ]:
        hits = [hex(m.start() + base) for m in re.finditer(re.escape(pat), blob)]
        if hits:
            print(f"  {label:8s} {pat_name:14s}: {hits[:15]}")
