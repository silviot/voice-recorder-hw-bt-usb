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

firm = fw[0x00E000:0x173E8C]
FIRM_VMA = 0x00C0E000

# 1. Bluetooth HFP AT handler: string '_hfp_hf_slc_open_status' is at flash 0x0DADAD -> VMA 0x00CDADAD
#    and '+CIEV:' is at flash 0x0DB886 -> VMA 0x00CDB886
# 2. AVRCP passthrough handler: string '_avrcp_tg_command_parser' is at flash 0x0DA4BF -> VMA 0x00CDA4BF
#    and '-pass through op_id 0x%x, btn state %d, ' is at flash 0x0DA94D -> VMA 0x00CDA94D
# 3. PHONERECORD trigger: string '-status to PHONERECORD' is at flash 0x0AFA24 -> VMA 0x00CAFA24
#    and '20%02d%02d%02d-%02d%02d%02d-phone.wav' at flash 0x04E538 -> RAM 0x80CC40 + (0x4E538 - 0x39470)? Wait: 0x39470 + 0xD740 = 0x46BB0!
#    Aha! Flash 0x046BB0 is the end of .data! So flash >= 0x048030 is XIP rodata (VMA = 0x00C00000 + flash_off)!
#    Let's verify: 0x04E538 -> VMA 0x00C4E538!
# 4. PSMP NVRAM checksum: search for 0xAA55 or 0x55AA or string 'PSMP' (0x00CBAFD3)

targets = {
    "HFP_SLC_OPEN": 0x00CDADAD,
    "HFP_CIEV": 0x00CDB886,
    "AVRCP_PASSTHRU": 0x00CDA94D,
    "PHONERECORD": 0x00CAFA24,
    "PHONE_WAV_FMT": 0x00C4E538,
    "PSMP_STR": 0x00CBAFD3,
    "PC_UPGRADE_FLAG": 0x00C4EA95, # pstr_usb_start_event_analysis
    "SD_WR_CB": 0x00CB98C1,        # usbd_register_sd_wr_cb
}
for name, vma in targets.items():
    refs = find_u32(firm, FIRM_VMA, vma)
    print(f"  {name:16s} (0x{vma:08X}) literal refs: {[hex(r) for r in refs]}")
