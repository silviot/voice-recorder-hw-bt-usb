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

print("=== 1. PSMP nv_set (0xD41CA0..0xD41E4C) ===")
print(disasm(fw[0x141CA0:0x141E4C], 0x00D41CA0))

print("\n=== 2. SDP Records in Flash (search for 0x111E Handsfree, 0x110A AudioSource, 0x110B AudioSink, 0x110C/0x110E AVRCP, 0x1101 SPP) ===")
for name, uuid_be in [
    ("SPP_0x1101", b"\x19\x11\x01"),
    ("AudioSrc_0x110A", b"\x19\x11\x0a"),
    ("AudioSnk_0x110B", b"\x19\x11\x0b"),
    ("AVRCP_TG_0x110C", b"\x19\x11\x0c"),
    ("AVRCP_0x110E", b"\x19\x11\x0e"),
    ("HFP_HF_0x111E", b"\x19\x11\x1e"),
    ("HFP_AG_0x111F", b"\x19\x11\x1f"),
    ("OBEX_0x1105", b"\x19\x11\x05"),
    ("PnP_0x1200", b"\x19\x12\x00"),
]:
    hits = [hex(m.start()) for m in re.finditer(re.escape(uuid_be), fw)]
    print(f"  {name:18s}: {hits}")

print("\n=== 3. SDIO read_mid_buf / HAL_sd_disk_read in FIRM (search 'sdio(e):read_mid_buf') ===")
m = fw.find(b"sdio(e):read_mid_buf", 0x00E000)
if m != -1:
    vma = 0x00C00000 + m
    refs = find_u32(fw[0x00E000:0x173E8C], 0x00C0E000, vma)
    print(f"  read_mid_buf string at 0x{vma:08X}, refs: {[hex(r) for r in refs]}")
m2 = fw.find(b"HAL_sd_disk_read\x00", 0x00E000)
if m2 != -1:
    vma2 = 0x00C00000 + m2
    refs2 = find_u32(fw[0x00E000:0x173E8C], 0x00C0E000, vma2)
    print(f"  HAL_sd_disk_read string at 0x{vma2:08X}, refs: {[hex(r) for r in refs2]}")
    if refs2:
        ro = refs2[0] - 0x00C00000
        print(disasm(fw[ro-0x140:ro+0x40], refs2[0]-0x140))
