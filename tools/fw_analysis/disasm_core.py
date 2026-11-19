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

def disasm(raw_bytes, vma):
    tmp = pathlib.Path("/tmp/chunk_dis.elf")
    tmp.write_bytes(make_elf32_arm([(vma, raw_bytes)]))
    cmd = ["/usr/lib/llvm-21/bin/llvm-objdump", "-d", "--triple=thumbv7em-none-eabi", str(tmp)]
    out = subprocess.check_output(cmd, text=True)
    lines = [l for l in out.splitlines() if re.match(r"^\s*[0-9a-fA-F]+:", l)]
    return "\n".join(lines)

print("=== 1. BootROM SCSI Dispatcher & 0xC0 Vendor Handler (0x6080..0x6378) ===")
print(disasm(rom[0x6080:0x6378], 0x6080))

print("\n=== 2. BootROM READ(10)=0x5cbc, WRITE(10)=0x5f74, usb_set_userfn=0x5914 ===")
print("--- 0x5900..0x5960 ---")
print(disasm(rom[0x5900:0x5960], 0x5900))
print("--- 0x5cbc..0x5d74 (READ(10)) ---")
print(disasm(rom[0x5cbc:0x5d74], 0x5cbc))
