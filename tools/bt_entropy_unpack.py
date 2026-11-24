#!/usr/bin/env python3
"""Turn exp/exp_entropy_dump.c firmware-source dumps into NIST SP 800-90B tool input (plan M2, BT_HW_RESULTS.md
"Entropy source assessment").

  seq      source 5 dumps (4 symbols per byte, bits 1:0 first) -> one 2-bit symbol per byte, concatenated
  restart  source 6 dumps (16 rows x 1000 symbols, one per byte) -> restart matrix file (rows concatenated)

Prints a quick most-common-value estimate (SP 800-90B 6.3.1, 99 % upper bound) per file and overall, and the
ea_non_iid / ea_restart command lines. The quick estimate is a sanity check only; the credit in
open_fw/bt_port/bt_entropy_mic.h may only be raised from the NIST tool's result.

  python3 tools/bt_entropy_unpack.py seq out/mic_seq.bin dumps/ent5_*.bin
  python3 tools/bt_entropy_unpack.py restart out/mic_restart.bin dumps/ent6_*.bin
  python3 tools/bt_entropy_unpack.py --self-test

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import math
import sys
from collections import Counter
from pathlib import Path

ROW = 1000
BITS = 2


def unpack4(data: bytes) -> bytes:
    out = bytearray(len(data) * 4)
    for i, b in enumerate(data):
        out[4 * i] = b & 3
        out[4 * i + 1] = (b >> 2) & 3
        out[4 * i + 2] = (b >> 4) & 3
        out[4 * i + 3] = (b >> 6) & 3
    return bytes(out)


def mcv_min_entropy(sym: bytes) -> float:
    """SP 800-90B 6.3.1: p_u = min(1, p_hat + 2.576 sqrt(p_hat (1 - p_hat) / (L - 1))), H = -log2(p_u)."""
    n = len(sym)
    if n < 2:
        return 0.0
    p = Counter(sym).most_common(1)[0][1] / n
    pu = min(1.0, p + 2.576 * math.sqrt(p * (1.0 - p) / (n - 1)))
    return -math.log2(pu)


def describe(name: str, sym: bytes) -> str:
    c = Counter(sym)
    hist = " ".join(f"{k}:{c.get(k, 0)}" for k in range(1 << BITS))
    return f"{name}: {len(sym)} symbols ({hist}), MCV min-entropy {mcv_min_entropy(sym):.3f} bit/symbol"


def cmd_seq(out: Path, inputs: list[Path]) -> int:
    allsym = bytearray()
    for p in inputs:
        sym = unpack4(p.read_bytes())
        print(describe(p.name, sym))
        allsym += sym
    out.write_bytes(bytes(allsym))
    print(describe(f"total -> {out}", bytes(allsym)))
    if len(allsym) < 1_000_000:
        print(f"note: SP 800-90B wants >= 1,000,000 samples; have {len(allsym)} ({math.ceil((1_000_000 - len(allsym)) / 65536)} more source-5 runs)")
    print(f"next: ./ea_non_iid -i -a -v {out} {BITS}")
    return 0


def cmd_restart(out: Path, inputs: list[Path]) -> int:
    rows = bytearray()
    for p in inputs:
        d = p.read_bytes()
        if len(d) % ROW:
            print(f"{p}: {len(d)} bytes is not a whole number of {ROW}-symbol rows (short run?): skipped")
            continue
        rows += d
    nrows = len(rows) // ROW
    out.write_bytes(bytes(rows))
    print(describe(f"restart matrix -> {out} ({nrows} rows)", bytes(rows)))
    if nrows < 1000:
        print(f"note: ea_restart needs 1000 rows; have {nrows} ({math.ceil((1000 - nrows) / 16)} more source-6 runs)")
    print(f"next: ./ea_restart -n -v {out} {BITS} <H_I from ea_non_iid>   (-n: non-IID; -i would mean IID here)")
    return 0


def self_test() -> int:
    packed = bytes([0b11100100, 0b00011011])
    assert unpack4(packed) == bytes([0, 1, 2, 3, 3, 2, 1, 0]), unpack4(packed)
    const = bytes(5000)
    assert mcv_min_entropy(const) == 0.0
    uni = bytes(i & 3 for i in range(400000))
    h = mcv_min_entropy(uni)
    assert 1.98 < h < 2.0, h
    print("bt_entropy_unpack self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["seq", "restart"])
    ap.add_argument("out", type=Path)
    ap.add_argument("inputs", type=Path, nargs="+")
    a = ap.parse_args(argv[1:])
    return cmd_seq(a.out, a.inputs) if a.mode == "seq" else cmd_restart(a.out, a.inputs)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
