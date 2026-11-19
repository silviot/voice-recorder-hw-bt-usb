# Build system for Cassini OpenFW (standalone clean-room SL6801 firmware)
#
# Usage:
#   make -C open_fw        # Compile & pack standalone open_fw FIRM partition
#   make -C open_fw clean  # Remove build artifacts
#
# SPDX-License-Identifier: MIT

CLANG    ?= clang
LLD      ?= $(shell which ld.lld 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name ld.lld 2>/dev/null | head -n 1)
OBJCOPY  ?= $(shell which llvm-objcopy 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name llvm-objcopy 2>/dev/null | head -n 1)

CFLAGS   := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft -mthumb \
            -Os -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector \
            -Iinclude

BUILD_DIR ?= /tmp/sl6801_openfw
SRCS      := src/startup.c src/wdt_timer.c src/rtc.c src/main.c
OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(BUILD_DIR)/open_fw_firm.bin

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c include/sl6801_regs.h include/sl6801_rom.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/open_fw.elf: $(OBJS) sl6801_firm.ld
	@test -n "$(LLD)" || (echo "Error: ld.lld not found (set LLD=/path/to/ld.lld)" >&2; exit 1)
	$(LLD) -T sl6801_firm.ld $(OBJS) -o $@

$(BUILD_DIR)/ram_boot.bin: $(BUILD_DIR)/open_fw.elf
	$(OBJCOPY) -O binary -j .ram_boot $< $@

$(BUILD_DIR)/xip.bin: $(BUILD_DIR)/open_fw.elf
	$(OBJCOPY) -O binary -j .text -j .data $< $@

$(BUILD_DIR)/open_fw_firm.bin: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	python3 tools/pack_firm.py --ram-boot $(BUILD_DIR)/ram_boot.bin --xip $(BUILD_DIR)/xip.bin --out-firm $@

clean:
	rm -rf $(BUILD_DIR)
