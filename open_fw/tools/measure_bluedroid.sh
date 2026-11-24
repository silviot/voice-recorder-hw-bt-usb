#!/usr/bin/env bash
# Phase-1 measurement: build Bluedroid (Classic), link-test variants, report sizes + unresolved symbols.
# Usage: tools/measure_bluedroid.sh <picolibc-install-prefix> [build_dir] [extra make vars, e.g. BT_LOGS=1]
# SPDX-License-Identifier: MIT
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
PL=${1:?picolibc prefix (see tools/build_picolibc.sh)}; B=${2:-/tmp/bluedroid_build}; shift 2 || true
make -f "$HERE/bt/Makefile.bluedroid" link PICOLIBC="$PL" BUILD_DIR="$B" -j"$(nproc)" "$@" 2>&1 | grep -E " error |Error " || true
SIZE=$(which llvm-size 2>/dev/null || ls /usr/lib/llvm-*/bin/llvm-size | tail -n1)
for v in core core_spp core_a2dp full; do printf '%-10s ' $v; "$SIZE" "$B/lt_$v.elf" | tail -1 | awk '{printf "text+rodata=%d data=%d bss=%d\n",$1,$2,$3}'; done
echo "--- unresolved symbols (full link) ---"
grep -oE "undefined symbol: [A-Za-z0-9_]+" "$B/lt_full.undef.txt" | sed 's/undefined symbol: //' | sort -u | tr '\n' ' '; echo
echo "--- per-component (full link) ---"
python3 -I "$HERE/tools/bt_size_report.py" "$B/lt_full.map"
