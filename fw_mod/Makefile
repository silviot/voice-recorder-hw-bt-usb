# Build system for Smartlink SL6801 (pen133_2.0.65) XIP C extensions & custom firmware
#
# Usage:
#   make -C fw_mod             # Compile bt_filesync.bin (Thumb-2 XIP payload)
#   make -C fw_mod firmware    # Build patched 2 MB image + 0:\update.up OTA package
#   make -C fw_mod clean       # Remove intermediate build artifacts
#
# SPDX-License-Identifier: MIT

CLANG    ?= clang
LLD      ?= $(shell which ld.lld 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name ld.lld 2>/dev/null | head -n 1)
OBJCOPY  ?= $(shell which llvm-objcopy 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name llvm-objcopy 2>/dev/null | head -n 1)

CFLAGS   := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft -mthumb \
            -Os -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector

BUILD_DIR ?= /tmp/sl6801_build
PAYLOAD   := $(BUILD_DIR)/bt_filesync.bin

STOCK_BIN ?= ../dumps/firmware_2M.bin
ART_PNG   ?= ../assets/cassini_welcome_80x40.png
OUT_BIN   ?= ../dumps/firmware_2M_custom.bin
OUT_UP    ?= ../dumps/update_custom.up

PATCHES   := --patch sdio-end-of-disk \
             --patch wav-blockalign \
             --patch bt-autoboot \
             --patch bt-invisible-boot \
             --patch bt-pair-menu \
             --patch bt-rfcomm-turbo \
             --patch bt-spp-uuid

.PHONY: all firmware check-sdk clean

all: $(PAYLOAD) check-sdk

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/bt_filesync.o: bt_filesync.c | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/bt_filesync.elf: $(BUILD_DIR)/bt_filesync.o xip.ld
	@test -n "$(LLD)" || (echo "Error: ld.lld not found (set LLD=/path/to/ld.lld)" >&2; exit 1)
	$(LLD) -T xip.ld $< -o $@

$(PAYLOAD): $(BUILD_DIR)/bt_filesync.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary $< $@
	@ls -lh $@

check-sdk: custom_ui.c sl6801_sdk.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c custom_ui.c -o $(BUILD_DIR)/custom_ui.o

firmware: $(PAYLOAD)
	@test -f "$(STOCK_BIN)" || (echo "Error: stock dump $(STOCK_BIN) not found" >&2; exit 1)
	python3 ../tools/assets.py replace "$(STOCK_BIN)" --desc-vma 0x00C55CB8 --png "$(ART_PNG)" --out "$(BUILD_DIR)/fw_art.bin"
	python3 ../tools/patch_firmware.py "$(BUILD_DIR)/fw_art.bin" \
		--out-bin "$(OUT_BIN)" --out-update-up "$(OUT_UP)" \
		$(PATCHES) --xip-payload "$(PAYLOAD)" --bump-timestamp
	python3 ../tools/patch_firmware.py "$(OUT_BIN)" --verify

clean:
	rm -rf $(BUILD_DIR)
