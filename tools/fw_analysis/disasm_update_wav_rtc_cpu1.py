#!/usr/bin/env python3
"""Disassemble bootloader sdupdate (0:\\update.up), CRC-16 verification,
WAV header writer (blockAlign bug), /dev/rtc driver, and CPU1 boot trampoline."""

import pathlib
import re
import struct
import subprocess

FW = pathlib.Path("dumps/firmware_2M.bin").read_bytes()
BL_BASE = 0x81FB60  # Flash 0x000060 -> RAM 0x0081FBC0


def make_elf(vma: int, data: bytes) -> bytes:
    ehdr_sz, phdr_sz = 52, 32
    off = ehdr_sz + phdr_sz
    phdr = struct.pack("<IIIIIIII", 1, off, vma, vma, len(data), len(data), 5, 4)
    ehdr = (
        b"\x7fELF\x01\x01\x01"
        + b"\x00" * 9
        + struct.pack(
            "<HHIIIIIHHHHHH",
            2, 40, 1, vma | 1, ehdr_sz, 0, 0x05000000, ehdr_sz, phdr_sz, 1, 0, 0, 0,
        )
    )
    return ehdr + phdr + data


def disasm(data: bytes, vma: int) -> str:
    tmp = pathlib.Path("/tmp/dis_fw_ext.elf")
    tmp.write_bytes(make_elf(vma, data))
    out = subprocess.check_output(
        ["/usr/lib/llvm-21/bin/llvm-objdump", "-d", "--triple=thumbv7em-none-eabi", str(tmp)],
        text=True,
    )
    return "\n".join(l for l in out.splitlines() if re.match(r"^\s*[0-9a-fA-F]+:", l))


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def main() -> None:
    print("=== 1. CRC-16/CCITT-FALSE Verification ===")
    checks = [
        ("HLKJ header (0x0000..0x005C)", FW[0x00:0x5C], struct.unpack_from("<H", FW, 0x5C)[0]),
        ("Bootloader  (0x0060..0xC950)", FW[0x60:0x60 + 0xC8F0], struct.unpack_from("<H", FW, 0x14)[0]),
        ("FIRM ram    (0xE030..0xF030)", FW[0xE030:0xE030 + 0x1000], struct.unpack_from("<H", FW, 0xE01C)[0]),
        ("FIRM all    (0xE000..0x173E8C)", FW[0xE000:0xE000 + 0x165E8C], struct.unpack_from("<H", FW, 0xD01C)[0]),
    ]
    for label, buf, expected in checks:
        got = crc16_ccitt_false(buf)
        status = "MATCH" if got == expected else "MISMATCH"
        print(f"  {label}: got=0x{got:04X} stored=0x{expected:04X} [{status}]")

    print("\n=== 2. Bootloader FIRM Loader (0x820470..0x8204D8) ===")
    print(disasm(FW[0x820470 - BL_BASE : 0x8204D8 - BL_BASE], 0x820470))

    print("\n=== 3. PCM WAV Header Writer blockAlign Bug (0xCED454..0xCED484) ===")
    print(disasm(FW[0xED454:0xED484], 0xCED454))

    print("\n=== 4. /dev/rtc Write & Read (0xD3B40C..0xD3B4A4) ===")
    print(disasm(FW[0x13B40C:0x13B4A4], 0xD3B40C))

    print("\n=== 5. CPU1 Boot & Trampoline (0xCE7370..0xCE73CE) ===")
    print(disasm(FW[0xE7370:0xE73CE], 0xCE7370))


if __name__ == "__main__":
    main()
