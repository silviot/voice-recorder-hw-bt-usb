#!/usr/bin/env bash
# Build picolibc (BSD-style licence) for Cortex-M4F with clang + lld, no multilib, no semihosting.
# Usage: tools/build_picolibc.sh <workdir>   -> installs to <workdir>/pl-install (use as PICOLIBC=...)
# Needs: git, meson, ninja, clang, ld.lld (same lookup as open_fw/Makefile), llvm-ar/nm/strip.
# SPDX-License-Identifier: MIT
set -euo pipefail
W=${1:?workdir}; mkdir -p "$W"; W=$(cd "$W" && pwd)
TAG=${PICOLIBC_REF:-7128f23}          # commit used when this was measured (1.8.12 dev)
LLD=${LLD:-$(which ld.lld 2>/dev/null || find /usr/lib "${ANDROID_HOME:-$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name ld.lld 2>/dev/null | head -n 1)}
BIN=$W/llvmbin; mkdir -p "$BIN"; ln -sf "$LLD" "$BIN/ld.lld"
for t in llvm-ar llvm-nm llvm-strip llvm-size; do
  p=$(which $t 2>/dev/null || ls /usr/lib/llvm-*/bin/$t 2>/dev/null | tail -n 1); ln -sf "$p" "$BIN/$t"
done
[ -d "$W/picolibc" ] || git clone https://github.com/picolibc/picolibc.git "$W/picolibc"
git -C "$W/picolibc" checkout -q "$TAG" 2>/dev/null || true
cat > "$W/m4f-clang.txt" <<X
[binaries]
c = ['clang', '--target=thumbv7em-none-eabi', '-mcpu=cortex-m4', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard', '-mthumb', '-nostdlib', '-fuse-ld=lld', '-B$BIN']
ar = '$BIN/llvm-ar'
nm = '$BIN/llvm-nm'
strip = '$BIN/llvm-strip'
[host_machine]
system = 'none'
cpu_family = 'arm'
cpu = 'cortex-m4'
endian = 'little'
[properties]
skip_sanity_check = true
X
rm -rf "$W/pl-build"
meson setup "$W/pl-build" "$W/picolibc" --cross-file "$W/m4f-clang.txt" --prefix="$W/pl-install" \
  -Dmultilib=false -Dtests=false -Dsemihost=false -Dspecsdir=none -Dio-float-exact=false \
  -Dthread-local-storage=false -Dc_args="-Os -ffunction-sections -fdata-sections" >/dev/null
ninja -C "$W/pl-build" >/dev/null && ninja -C "$W/pl-build" install >/dev/null
# Minimal compiler-rt builtins (soft double helpers pulled by picolibc strtod/dtoa). 64-bit div stays in src/startup.c.
if [ ! -d "$W/llvm/compiler-rt/lib/builtins" ]; then
  git clone --depth 1 --filter=blob:none --sparse https://github.com/llvm/llvm-project.git "$W/llvm"
  git -C "$W/llvm" sparse-checkout set compiler-rt/lib/builtins
fi
mkdir -p "$W/crt"; OBJS=""
for f in adddf3 muldf3 extendsfdf2 truncdfsf2 floatundidf fp_mode; do
  clang --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -Os -ffunction-sections \
        -c "$W/llvm/compiler-rt/lib/builtins/$f.c" -o "$W/crt/$f.o"; OBJS="$OBJS $W/crt/$f.o"
done
"$BIN/llvm-ar" rcs "$W/pl-install/lib/libclang_rt_min.a" $OBJS
echo "PICOLIBC=$W/pl-install"
