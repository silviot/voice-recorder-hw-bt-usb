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

print("=== 1. HAL_sd_disk_read (0xC42900..0xC42A80, flash 0x042900?) ===")
# Wait! Earlier we passed fw[ro-0x140:ro+0x40] where ro = refs2[0] - 0xC00000!
# Wait: refs2[0] was 0xC42B08? Let's check: in disasm_sdp_sdio_psmp.py:
# find_u32(fw[0x00E000:0x173E8C], 0x00C0E000, vma2) -> 0xC42B08? NO! Wait: 0xC0E000 + offset = 0xD42B08 if offset was 0x134B08!
# Wait: 0xC42B08 - 0xC0E000 = 0x34B08 -> flash offset 0x00E000 + 0x34B08 = 0x042B08!
# WAIT: flash 0x042B08 is inside .data/.ramtext (0x039470..0x046BB0 -> RAM 0x80CC40 + (0x42B08 - 0x39470) = 0x8162D8)!!
# AHA! That's why bl targets in 0xC429C8 looked shifted — because flash 0x042900 runs in SRAM at 0x8160D0!
ram_off = 0x042900 - 0x039470 + 0x0080CC40
print(f"Disassembling flash 0x0428D0..0x042A80 at its TRUE SRAM VMA 0x{ram_off - 0x30:06X}:")
print(disasm(fw[0x0428D0:0x042A80], ram_off - 0x30))

print("\n=== 2. Raw SDP Service Records & Query Patterns ===")
for label, foff, ln in [
    ("A2DP_SRC (0x0D9510)", 0x0D9510, 96),
    ("AVRCP_TG (0x0DA5B8)", 0x0DA5B8, 96),
    ("HFP_HF   (0x0DAE08)", 0x0DAE08, 80),
]:
    print(f"  {label}: {fw[foff:foff+ln].hex(' ')}")

print("\n=== 3. PSMP Record Builder & CRC32 (0xD41280..0xD41360) ===")
print(disasm(fw[0x141280:0x141360], 0x00D41280))

print("\n=== 4. Phone Call Auto-Recording Logic (refs to 'phone record start' 0x00C4E55E & 'SCO_OPEN' 0x00C4D9A1) ===")
firm = fw[0x00E000:0x173E8C]
for nm, addr in [("phone_rec_start", 0x00C4E55E), ("SCO_OPEN", 0x00C4D9A1), ("call_online", 0x00C4D947)]:
    r = find_u32(firm, 0x00C0E000, addr)
    print(f"  {nm:16s}: {[hex(x) for x in r]}")
    if r:
        fo = r[0] - 0x00C00000
        print(disasm(fw[fo-0x120:fo+0x20], r[0]-0x120))
