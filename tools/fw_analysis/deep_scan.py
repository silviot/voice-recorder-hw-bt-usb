#!/usr/bin/env python3
import struct
import pathlib
import subprocess
import re

fw = pathlib.Path("dumps/firmware_2M.bin").read_bytes()
rom = pathlib.Path("dumps/bootrom_64K.bin").read_bytes()

def make_elf32_arm(segments):
    """segments: list of (vma, data_bytes)"""
    ehdr_sz = 52
    phdr_sz = 32
    phnum = len(segments)
    off = ehdr_sz + phdr_sz * phnum
    phdrs = bytearray()
    payload = bytearray()
    for vma, data in segments:
        phdrs += struct.pack(
            "<IIIIIIII",
            1,           # PT_LOAD
            off,         # p_offset
            vma,         # p_vaddr
            vma,         # p_paddr
            len(data),   # p_filesz
            len(data),   # p_memsz
            5,           # PF_R | PF_X
            4            # p_align
        )
        payload += data
        off += len(data)
    e_ident = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,               # ET_EXEC
        40,              # EM_ARM
        1,               # EV_CURRENT
        segments[0][0] | 1,  # e_entry (Thumb)
        ehdr_sz,         # e_phoff
        0,               # e_shoff
        0x05000000,      # EF_ARM_EABI_VER5
        ehdr_sz,         # e_ehsize
        phdr_sz,         # e_phentsize
        phnum,           # e_phnum
        0, 0, 0
    )
    return bytes(ehdr + phdrs + payload)

def disasm(raw_bytes, vma, count_insns=60):
    tmp = pathlib.Path("/tmp/chunk_dis.elf")
    tmp.write_bytes(make_elf32_arm([(vma, raw_bytes)]))
    cmd = [
        "/usr/lib/llvm-21/bin/llvm-objdump",
        "-d", "--triple=thumbv7em-none-eabi",
        str(tmp)
    ]
    out = subprocess.check_output(cmd, text=True)
    lines = [l for l in out.splitlines() if re.match(r"^\s*[0-9a-fA-F]+:", l)]
    return "\n".join(lines[:count_insns])

print("=== 1. BootROM Reset_Handler (0x00000040) ===")
print(disasm(rom[0x40:0x140], 0x40, 40))

print("\n=== 2. Bootloader Entry (0x00820000, file 0x0004A0) ===")
print(disasm(fw[0x4A0:0x5A0], 0x00820000, 45))

print("\n=== 3. FIRM Vector Table & Reset_Handler ===")
ram_blk = fw[0x00E030 : 0x00E030 + 0x1000]
in_nz = False
start = 0
for i, b in enumerate(ram_blk):
    if b != 0 and not in_nz:
        in_nz = True; start = i
    elif b == 0 and in_nz and ram_blk[i:i+16] == b"\x00"*16:
        in_nz = False
        print(f"  +0x{start:04X} .. +0x{i:04X}")
if in_nz:
    print(f"  +0x{start:04X} .. +0x1000")

print("\nDisassembling FIRM RAM block +0x0400 (base=0x804800 -> 0x804C00):")
print(disasm(ram_blk[0x400:0x580], 0x00804C00, 55))
