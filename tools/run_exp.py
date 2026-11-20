#!/usr/bin/env python3
"""
Compile, upload, run, and collect logs/dumps for Cassini OpenFW SRAM experiments.

Builds any Thumb-2 experiment C source against open_fw/exp/exp_sdk.h into the
fixed 0x00850000..0x00857FFF SRAM arena, verifies zero undefined symbols and
entry-at-0x00850000, uploads over USB MSC 0xE0 0x05 EXP_WRITE, triggers
0xE0 0x06 EXP_RUN, reads g_openfw_log, and optionally dumps 0x00858000 out_buf
to raw binary or WAV.

Examples:
  python3 tools/run_exp.py --build-only open_fw/exp/exp_probe.c
  sudo python3 tools/run_exp.py open_fw/exp/exp_probe.c
  sudo python3 tools/run_exp.py open_fw/exp/exp_adc.c --arg 24 --dump-wav /tmp/exp_adc.wav

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave

REPO_ROOT = Path(__file__).resolve().parent.parent
OPENFW_DIR = REPO_ROOT / "open_fw"
EXP_LD = OPENFW_DIR / "exp" / "exp_sram.ld"
EXP_ARENA_ADDR = 0x00850000
EXP_ARENA_SIZE = 32768
EXP_OUT_ADDR = 0x00858000

sys.path.insert(0, str(REPO_ROOT / "tools"))
from openfw_usb import (  # noqa: E402
    PID,
    VID,
    OpenFwUsbClient,
    format_exp_result,
    open_live_transport,
)


def find_llvm_tool(name: str, env_var: str) -> str:
    val = os.environ.get(env_var, "").strip()
    if val:
        return val
    found = shutil.which(name)
    if found:
        return found
    search_roots = [Path("/usr/lib")]
    ndk_base = Path(os.environ.get("ANDROID_HOME") or (Path.home() / "Android" / "Sdk")) / "ndk"
    if ndk_base.is_dir():
        search_roots.append(ndk_base)
    for root in search_roots:
        if not root.is_dir():
            continue
        for match in sorted(root.rglob(name)):
            if os.access(match, os.X_OK):
                return str(match)
    raise FileNotFoundError(f"Could not locate required LLVM tool '{name}' (set {env_var})")


def build_experiment(src_path: Path, out_bin: Path | None = None) -> bytes:
    if not src_path.is_file():
        raise FileNotFoundError(f"Experiment source not found: {src_path}")
    if not EXP_LD.is_file():
        raise FileNotFoundError(f"Experiment linker script not found: {EXP_LD}")

    clang = find_llvm_tool("clang", "CLANG")
    lld = find_llvm_tool("ld.lld", "LLD")
    objcopy = find_llvm_tool("llvm-objcopy", "OBJCOPY")
    nm = find_llvm_tool("llvm-nm", "LLVM_NM")

    with tempfile.TemporaryDirectory(prefix="openfw_exp_") as tmp:
        tmp_dir = Path(tmp)
        obj_file = tmp_dir / "exp.o"
        elf_file = tmp_dir / "exp.elf"
        bin_file = tmp_dir / "exp.bin"

        cflags = [
            clang,
            "--target=thumbv7em-none-eabi",
            "-mcpu=cortex-m4",
            "-mfpu=fpv4-sp-d16",
            "-mfloat-abi=hard",
            "-mthumb",
            "-Os",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-stack-protector",
            f"-I{OPENFW_DIR / 'include'}",
            f"-I{OPENFW_DIR / 'exp'}",
            "-c",
            str(src_path),
            "-o",
            str(obj_file),
        ]
        subprocess.run(cflags, check=True)

        subprocess.run(
            [lld, "-T", str(EXP_LD), str(obj_file), "-o", str(elf_file)],
            check=True,
        )

        undef = subprocess.check_output([nm, "-u", str(elf_file)], text=True).strip()
        if undef:
            raise RuntimeError(f"Undefined symbols in {src_path.name}:\n{undef}")

        sym_out = subprocess.check_output([nm, str(elf_file)], text=True)
        entry_addr = None
        for line in sym_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "exp_main":
                entry_addr = int(parts[0], 16)
                break
        if entry_addr is None or (entry_addr & ~1) != EXP_ARENA_ADDR:
            raise RuntimeError(
                f"exp_main must reside at 0x{EXP_ARENA_ADDR:08X} (got {entry_addr!r}); "
                "did you annotate exp_main with EXP_ENTRY?"
            )

        subprocess.run([objcopy, "-O", "binary", str(elf_file), str(bin_file)], check=True)
        payload = bin_file.read_bytes()

    if not payload or len(payload) > EXP_ARENA_SIZE:
        raise RuntimeError(
            f"Experiment binary size {len(payload)} B out of 1..{EXP_ARENA_SIZE} B range"
        )
    if out_bin is not None:
        out_bin.parent.mkdir(parents=True, exist_ok=True)
        out_bin.write_bytes(payload)
    return payload


def main() -> int:
    ap = argparse.ArgumentParser(description="Build, upload, run, and collect logs from OpenFW experiments")
    ap.add_argument("source", type=Path, help="Experiment C file (e.g. open_fw/exp/exp_probe.c)")
    ap.add_argument("--arg", type=lambda s: int(s, 0), default=0, help="uint32 arg passed to exp_main")
    ap.add_argument("--entry", type=lambda s: int(s, 0), default=0, help="Entry offset inside arena (default 0)")
    ap.add_argument("--build-only", action="store_true", help="Compile & validate binary without USB")
    ap.add_argument("--no-reset-log", action="store_true", help="Preserve prior g_openfw_log contents")
    ap.add_argument("--save-bin", type=Path, help="Save compiled flat binary to PATH")
    ap.add_argument("--dump-out", type=Path, help="Read res.out_len bytes from 0x00858000 to PATH")
    ap.add_argument("--dump-wav", type=Path, help="Wrap 0x00858000 16kHz mono PCM output into WAV")
    ap.add_argument("--timeout-ms", type=int, default=20000, help="USB timeout in ms (default 20000)")
    args = ap.parse_args()

    payload = build_experiment(args.source, out_bin=args.save_bin)
    print(f"Built {args.source.name}: {len(payload)} / {EXP_ARENA_SIZE} B (entry=0x{EXP_ARENA_ADDR:08X}, 0 undef)")
    if args.build_only:
        return 0

    tmo = max(args.timeout_ms, (args.arg + 6) * 1000 if 0 < args.arg <= 120 else args.timeout_ms)
    client = OpenFwUsbClient(open_live_transport(vid=VID, pid=PID, timeout_ms=tmo), default_timeout_ms=tmo)
    try:
        if not args.no_reset_log:
            client.log_reset()
        client.exp_write(payload, base_offset=0)
        res = client.exp_run(entry_off=args.entry, arg=args.arg, timeout_ms=tmo)
        print(format_exp_result(res))

        raw_log = client.read_log()
        text = raw_log.rstrip(b"\x00").decode("utf-8", errors="replace")
        if text:
            print("=== EXP LOG ===")
            print(text, end="" if text.endswith("\n") else "\n")

        if res.out_len > 0 and (args.dump_out or args.dump_wav):
            out_addr = (res.arena_base + 0x8000) if res.arena_base else EXP_OUT_ADDR
            out_data = client.mem_read(out_addr, res.out_len)
            if args.dump_out:
                args.dump_out.parent.mkdir(parents=True, exist_ok=True)
                args.dump_out.write_bytes(out_data)
                print(f"Dumped {len(out_data)} B -> {args.dump_out}")
            if args.dump_wav:
                args.dump_wav.parent.mkdir(parents=True, exist_ok=True)
                with wave.open(str(args.dump_wav), "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(16000)
                    wf.writeframes(out_data)
                print(f"Saved WAV ({len(out_data) // 2} samples @ 16kHz mono) -> {args.dump_wav}")
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
