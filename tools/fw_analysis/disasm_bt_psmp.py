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

def find_u32(buf, base_vma, val):
    return [base_vma + i for i in range(0, len(buf) - 3, 2) if struct.unpack_from("<I", buf, i)[0] == val]

print("=== 1. AVRCP Passthrough op_id switch (0xD615C0..0xD618C0) ===")
print(disasm(fw[0x1615C0:0x1618C0], 0x00D615C0))

print("\n=== 2. PHONERECORD trigger (0xD1A3E0..0xD1A560 & 0xCF9640..0xCF9780) ===")
print(disasm(fw[0x11A3E0:0x11A560], 0x00D1A3E0))
print("--- 0xCF9640..0xCF9780 ---")
print(disasm(fw[0x0F9640:0x0F9780], 0x00CF9640))

print("\n=== 3. PSMP NVRAM Checksum & Read/Write (around 0xD40480..0xD40680) ===")
print(disasm(fw[0x140480:0x140680], 0x00D40480))
