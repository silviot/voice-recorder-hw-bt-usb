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

def find_bl_callers(buf, base_vma, target_addr):
    hits = []
    for i in range(0, len(buf) - 3, 2):
        hw1, hw2 = struct.unpack_from("<HH", buf, i)
        if (hw1 & 0xF800) == 0xF000 and (hw2 & 0xD000) in (0xD000, 0x9000):
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
            if ((pc + off) & 0xFFFFFFFF) == target_addr:
                hits.append(base_vma + i)
    return hits

def find_u32(buf, base_vma, val):
    return [base_vma + i for i in range(0, len(buf) - 3, 2) if struct.unpack_from("<I", buf, i)[0] == val]

firm = fw[0x00E000:0x173E8C]
FIRM_VMA = 0x00C0E000

print("=== 1. Callers of ROM usb_set_userfn (0x5914) in FIRM ===")
callers_5914 = find_bl_callers(firm, FIRM_VMA, 0x5914)
print("  BL/B.W 0x5914 callers:", [hex(c) for c in callers_5914])
# Also search literal pool refs to 0x5915 or 0x801068
print("  u32 refs to 0x5915:", [hex(c) for c in find_u32(firm, FIRM_VMA, 0x5915)])
print("  u32 refs to 0x801068:", [hex(c) for c in find_u32(firm, FIRM_VMA, 0x00801068)])
# Also search refs to 'usbd_msc_cardreader_init ++++++' (flash 0x0B9958 -> VMA 0x00CB9958)
refs_msc = find_u32(firm, FIRM_VMA, 0x00CB9958)
print("  u32 refs to 'usbd_msc_cardreader_init' (0xCB9958):", [hex(c) for c in refs_msc])
refs_sdwr = find_u32(firm, FIRM_VMA, 0x00CB98C1)
print("  u32 refs to 'usbd_register_sd_wr_cb' (0xCB98C1):", [hex(c) for c in refs_sdwr])

for c in callers_5914[:3]:
    off = c - FIRM_VMA
    print(f"\n--- Disassembly around 0x{c:06X} ---")
    print(disasm(firm[max(0, off-0x60):off+0x60], max(FIRM_VMA, c-0x60)))

for r in refs_msc[:2]:
    off = r - FIRM_VMA
    print(f"\n--- Disassembly before literal pool 0x{r:06X} ---")
    print(disasm(firm[max(0, off-0x100):off+0x30], max(FIRM_VMA, r-0x100)))
    print("  Literal pool words around", hex(r), ":", [hex(struct.unpack_from("<I", firm, off-16+k)[0]) for k in range(0, 48, 4)])
