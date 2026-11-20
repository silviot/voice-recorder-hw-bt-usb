#!/usr/bin/env bash
# Set up and build headless QEMU 8.2.2 with Smartlink SL6801 + OpenFW extensions
# (UART0 0x40090000, 4-byte LCDC write support, and qapi/visitor.h build fix).
#
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_FILE="${PATCH_FILE:-$SCRIPT_DIR/sl6801_qemu_openfw.patch}"
EMU_REPO_URL="${EMU_REPO_URL:-https://github.com/anton-veretenenko/smartlink-sl6801-emulator}"
EMU_COMMIT="${EMU_COMMIT:-a4136d678c6fc98c85e3459fc29f90173db5eeab}"
QEMU_VERSION="${QEMU_VERSION:-8.2.2}"

if [ ! -f "$PATCH_FILE" ]; then
    echo "ERROR: Patch file not found: $PATCH_FILE" >&2
    exit 1
fi

if [ -n "${EMU_DIR:-}" ]; then
    mkdir -p "$EMU_DIR"
elif mkdir -p /var/tmp/sl6801-emu 2>/dev/null && [ -w /var/tmp/sl6801-emu ]; then
    EMU_DIR="/var/tmp/sl6801-emu"
else
    EMU_DIR="/tmp/sl6801-emu"
    mkdir -p "$EMU_DIR"
fi

REPO_DIR="$EMU_DIR/repo"
WORK_DIR="$EMU_DIR/work"
mkdir -p "$WORK_DIR"

if [ ! -d "$REPO_DIR/.git" ]; then
    echo "Cloning $EMU_REPO_URL (commit $EMU_COMMIT) -> $REPO_DIR..."
    git clone "$EMU_REPO_URL" "$REPO_DIR"
    git -C "$REPO_DIR" checkout --detach "$EMU_COMMIT"
fi

if git -C "$REPO_DIR" apply --check "$PATCH_FILE" >/dev/null 2>&1; then
    echo "Applying $PATCH_FILE to $REPO_DIR..."
    git -C "$REPO_DIR" apply "$PATCH_FILE"
elif git -C "$REPO_DIR" apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1; then
    echo "Patch already applied in $REPO_DIR."
else
    echo "ERROR: Could not cleanly apply $PATCH_FILE to $REPO_DIR" >&2
    exit 1
fi

if [ -z "${QEMU_TARBALL:-}" ] && [ -f "$EMU_DIR/qemu-${QEMU_VERSION}.tar.xz" ]; then
    export QEMU_TARBALL="$EMU_DIR/qemu-${QEMU_VERSION}.tar.xz"
fi

echo "Building headless QEMU $QEMU_VERSION into $WORK_DIR..."
WORK="$WORK_DIR" QEMU_VERSION="$QEMU_VERSION" bash "$REPO_DIR/emulator/scripts/build_qemu_m1.sh"

ln -sfn "qemu-${QEMU_VERSION}/build" "$WORK_DIR/build"

if [ "$EMU_DIR" != "/tmp/sl6801-emu" ]; then
    if [ -L /tmp/sl6801-emu ] || [ ! -e /tmp/sl6801-emu ]; then
        ln -sfn "$EMU_DIR" /tmp/sl6801-emu
    else
        echo "WARNING: /tmp/sl6801-emu already exists as a non-symlink; leaving untouched." >&2
    fi
fi

QEMU_BIN="$WORK_DIR/build/qemu-system-arm"
if [ ! -x "$QEMU_BIN" ]; then
    echo "ERROR: Expected QEMU binary missing or not executable: $QEMU_BIN" >&2
    exit 1
fi

echo "QEMU ready: $QEMU_BIN"
