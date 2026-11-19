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

print("=== 1. FIRM USB MSC ops callbacks in .ramtext (RAM 0x812A00..0x812BF0, flash 0x03F230..0x03F420) ===")
# ops stored in usbd_msc_cardreader_init (0xD3A5D2..0xD3A5F4):
# +0x00: 0x812A01
# +0x04: 0x812A05
# +0x08: 0x812A49 (media_present)
# +0x0C: 0x812A79
# +0x10: 0x812A7D (sd_read)
# +0x14: 0x812AD5 (sd_write)
# +0x18: 0x812A81 (custom 0xC0 subcommand handler!)
# +0x1C: 0x81A03C (INQUIRY string buffer? Let's check!)
# +0x20: 0x812AD9 (custom top-level SCSI CDB interceptor!)
print("Bytes at RAM 0x81A03C (flash 0x04686C):", fw[0x04686C:0x046898])
print(disasm(fw[0x03F230:0x03F430], 0x00812A00))

print("\n=== 2. FIRM SD Read/Write Registered Callbacks (around 0xD3A650..0xD3A850, flash 0x13A650..0x13A850) ===")
print(disasm(fw[0x13A650:0x13A850], 0x00D3A650))
