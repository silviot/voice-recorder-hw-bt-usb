#!/usr/bin/env python3
import struct
import pathlib
import subprocess
import re

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()

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

def disasm(raw_bytes, vma):
    tmp = pathlib.Path("/tmp/chunk_dis.elf")
    tmp.write_bytes(make_elf32_arm([(vma, raw_bytes)]))
    cmd = ["/usr/lib/llvm-21/bin/llvm-objdump", "-d", "--triple=thumbv7em-none-eabi", str(tmp)]
    out = subprocess.check_output(cmd, text=True)
    lines = [l for l in out.splitlines() if re.match(r"^\s*[0-9a-fA-F]+:", l)]
    return "\n".join(lines)

print("=== 1. Rest of FIRM SCSI Interceptor 0x812BD0..0x812E60 (flash 0x03F400..0x03F690) ===")
print(disasm(fw[0x03F400:0x03F690], 0x00812BD0))

print("\n=== 2. HFP CIEV / AT Table at 0xCDAEA0 (flash 0x0DAEA0..0x0DAF20) & HFP SLC Open at 0xD62800..0xD62AC0 ===")
for off in range(0x0DAE60, 0x0DAF00, 4):
    v = struct.unpack_from("<I", fw, off)[0]
    s = ""
    if 0x00C0E000 <= v < 0x00D73E8C:
        fo = v - 0x00C00000
        end = fw.find(b"\x00", fo, fo+64)
        if end != -1 and all(32 <= b < 127 for b in fw[fo:end]) and end > fo:
            s = fw[fo:end].decode()
    print(f"  0x{0xC00000+off:06X}: 0x{v:08X}  {s!r}")
print(disasm(fw[0x1627C0:0x162A60], 0x00D627C0))

print("\n=== 3. AVRCP Passthrough Command Parser at 0xD618C0..0xD61A80 ===")
print(disasm(fw[0x1618C0:0x161A80], 0x00D618C0))
