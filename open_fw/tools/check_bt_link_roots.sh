#!/usr/bin/env bash
# check_bt_link_roots.sh ELF ROOTS_C : every ROOT(name) in ROOTS_C must be a defined function in ELF,
# and the BT sections must be non-empty (guards against --gc-sections producing a hollow image).
# SPDX-License-Identifier: MIT
set -euo pipefail
elf=$1; roots=$2
NM=${NM:-$(which llvm-nm 2>/dev/null || ls /usr/bin/llvm-nm* /usr/lib/llvm-*/bin/llvm-nm 2>/dev/null | head -n 1)}
defined=$("$NM" --defined-only "$elf" | awk '$2 ~ /^[Tt]$/ {print $3}' | sort -u)
missing=0; n=0
for f in $(grep -o 'ROOT(esp_[a-z_0-9]*)' "$roots" | sed 's/ROOT(\(.*\))/\1/'); do
  n=$((n+1))
  if ! grep -qx "$f" <<<"$defined"; then echo "missing BT entry point: $f" >&2; missing=1; fi
done
[ $n -gt 10 ] || { echo "check_bt_link_roots: parsed only $n roots" >&2; exit 1; }
for s in btu_task_start_up bta_dm_enable l2c_init AVDT_Register SBC_Encoder_Init btc_a2dp_source_startup; do
  grep -qx "$s" <<<"$defined" || { echo "missing Bluedroid internal: $s" >&2; missing=1; }
done
[ $missing -eq 0 ] && echo "BT link roots OK ($n entry points)"
exit $missing
