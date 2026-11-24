#!/usr/bin/env python3
"""Aggregate an ld.lld map (-Map) into per-component flash/RAM numbers.
Usage: bt_size_report.py lt_full.map
SPDX-License-Identifier: MIT"""
import re, sys, collections
rows = collections.defaultdict(lambda: collections.Counter())
cur_out = None
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\.\S+)\s*$", line)
    if m and ":(" not in line:  # output section header
        cur_out = m.group(2); continue
    m = re.match(r"\s*[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S.*)$", line)
    if not m or cur_out is None: continue
    size, ref = int(m.group(1), 16), m.group(2)
    if ":(" not in ref: continue
    path = ref.split(":(")[0]
    obj = re.search(r"\(([^)]+\.o)\)", path)
    path = path if not obj else path.split("(")[0] + "/" + obj.group(1)
    sec = cur_out if cur_out in (".text", ".rodata", ".data", ".bss") else None
    if sec is None: continue
    if "/bluedroid/" in ref or "bluedroid.a(" in ref:
        pass
    key = "other"
    for pat, name in (("freertos/", "freertos kernel"), ("pl-install/lib/libc", "picolibc"), ("libc.a", "picolibc"),
                      ("lt_", "test main"), ("<internal>", "merged strings/consts")):
        if pat in ref: key = name; break
    if key == "other":
        o = obj.group(1) if obj else ref
        key = "bluedroid:" + o
    rows[key][sec] += size
# map object -> directory using vendored tree
import os
tree = {}
root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "bluedroid"))
for d, _, fs in os.walk(root):
    for f in fs:
        if f.endswith(".c"): tree[f[:-2] + ".o"] = os.path.relpath(d, root)
agg = collections.defaultdict(lambda: collections.Counter())
for k, c in rows.items():
    if k.startswith("bluedroid:"):
        d = tree.get(k.split(":", 1)[1], "unknown")
        parts = d.split("/")
        d = "/".join(parts[:4] if parts[0] == "host" else parts[:2])
        k = "bluedroid " + d
    agg[k].update(c)
tot = collections.Counter()
print("%-52s %9s %9s %8s %8s" % ("component", ".text", ".rodata", ".data", ".bss"))
for k, c in sorted(agg.items(), key=lambda kv: -(kv[1][".text"] + kv[1][".rodata"])):
    print("%-52s %9d %9d %8d %8d" % (k, c[".text"], c[".rodata"], c[".data"], c[".bss"])); tot.update(c)
print("%-52s %9d %9d %8d %8d" % ("TOTAL", tot[".text"], tot[".rodata"], tot[".data"], tot[".bss"]))
print("flash (text+rodata+data) = %d B, static RAM (data+bss) = %d B" % (tot[".text"] + tot[".rodata"] + tot[".data"], tot[".data"] + tot[".bss"]))
