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

print("=== 1. Find exact Flash -> SRAM mapping in FIRM ===")
# Search for 0x0081A380 (_sbss / _edata!) or 0x00C39470 in FIRM
for target in [0x0081A380, 0x00C39470, 0x00039470, 0x0080CC40, 0x00805800]:
    hits = [i for i in range(0, len(fw)-3, 2) if struct.unpack_from("<I", fw, i)[0] == target]
    print(f"  target 0x{target:08X} found at flash offsets: {[hex(h) for h in hits]}")

# Also check Bootloader around 'boot--->load_flash_addr= %x' (flash 0x0A026 -> BL 0x829B86)
bl_blob = fw[0x60:0xC950]
for i in range(0, len(bl_blob)-3, 2):
    v = struct.unpack_from("<I", bl_blob, i)[0]
    if 0x829B60 <= v <= 0x829BF0:
        print(f"  BL literal 0x{v:06X} at BL 0x{0x81FBC0+i:06X} (flash 0x{0x60+i:05X})")

# Let's check literal pool of 0x805200 (flash 0x00EA30..0x00EB60)
print("\nLiteral pool of 0x805200 (flash 0x00EB20..0x00EB60):")
for off in range(0x00EB20, 0x00EB60, 4):
    v = struct.unpack_from("<I", fw, off)[0]
    print(f"  flash 0x{off:05X} (RAM 0x{0x804800 + (off - 0x00E030):06X}): 0x{v:08X}")
