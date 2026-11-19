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

def disasm(raw_bytes, vma, max_lines=120):
    tmp = pathlib.Path("/tmp/chunk_dis.elf")
    tmp.write_bytes(make_elf32_arm([(vma, raw_bytes)]))
    cmd = ["/usr/lib/llvm-21/bin/llvm-objdump", "-d", "--triple=thumbv7em-none-eabi", str(tmp)]
    out = subprocess.check_output(cmd, text=True)
    lines = [l for l in out.splitlines() if re.match(r"^\s*[0-9a-fA-F]+:", l)]
    return "\n".join(lines[:max_lines])

def find_u32_refs(blob, base_vma, target_val, mask=0xFFFFFFFF):
    hits = []
    for i in range(0, len(blob) - 3, 2):
        v = struct.unpack_from("<I", blob, i)[0]
        if (v & mask) == target_val:
            hits.append(base_vma + i)
    return hits

print("=== 1. Bootloader FIRM Loader (ref to 0x829BC5 'boot--->firmware_header_len...') ===")
bl_blob = fw[0x60:0xC950]
refs = find_u32_refs(bl_blob, 0x81FBC0, 0x829BC5)
print("Literal pool refs to 0x829BC5:", [hex(r) for r in refs])
if refs:
    # disassemble 0x180 bytes before the literal pool
    fn_off = refs[0] - 0x81FBC0 - 0x160
    print(disasm(bl_blob[fn_off:fn_off+0x200], 0x81FBC0 + fn_off, 110))

print("\n=== 2. BootROM USB SCSI & 0xC0 Handler (around USBC at 0x6450 and 0x7808) ===")
# Let's check literal pools near 0x6450 and 0x7808
for addr in (0x6450, 0x7808):
    print(f"--- ROM around 0x{addr:04X} ---")
    start = max(0, addr - 0x160)
    print(disasm(rom[start:addr+0x60], start, 100))
