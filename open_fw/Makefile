# Build system for Cassini OpenFW (standalone clean-room SL6801 firmware)
#
# Usage:
#   make -C open_fw            # Compile & pack standalone open_fw FIRM partition
#   make -C open_fw test-host  # Compile & run host unit tests (crypto, wav, exfat)
#   make -C open_fw update-up  # Also build 2 MB flash image + 0:\update.up (requires ../dumps/firmware_2M.bin)
#   make -C open_fw clean      # Remove build artifacts
#
# SPDX-License-Identifier: MIT

CLANG    ?= clang
HOST_CC  ?= cc
LLD      ?= $(shell which ld.lld 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name ld.lld 2>/dev/null | head -n 1)
OBJCOPY  ?= $(shell which llvm-objcopy 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name llvm-objcopy 2>/dev/null | head -n 1)

BUILD_DIR ?= /tmp/sl6801_openfw

CFLAGS   := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft -mthumb \
            -Os -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector \
            -Iinclude -I$(BUILD_DIR)

HOST_CFLAGS := -O2 -Wall -Wextra -Werror -Iinclude

SRCS      := src/startup.c src/wdt_timer.c src/rtc.c src/uart.c src/pmu.c \
             src/keys.c src/lcd_st7735s.c src/sdio.c src/audio_dma.c \
             src/usb_scsi.c src/usb_musb.c src/bt_rf.c src/bt_spp.c \
             src/xrec_proto.c src/bt_hci.c src/recorder.c \
             src/crypto.c src/wav.c src/exfat.c src/main.c
OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

SPLASH_PNG  := ../assets/cassini_welcome_80x40.png
SPLASH_HDR  := $(BUILD_DIR)/cassini_splash.h
STOCK_DUMP  := ../dumps/firmware_2M.bin
OUT_BIN     := ../dumps/firmware_2M_openfw.bin
OUT_UP      := ../dumps/update_openfw.up

.PHONY: all test-host update-up clean

all: $(BUILD_DIR)/open_fw_firm.bin

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(SPLASH_HDR): $(SPLASH_PNG) tools/png_to_rgb565_h.py | $(BUILD_DIR)
	python3 tools/png_to_rgb565_h.py $(SPLASH_PNG) --out $@

$(BUILD_DIR)/main.o: src/main.c $(SPLASH_HDR) include/sl6801_regs.h include/openfw.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c include/sl6801_regs.h include/sl6801_rom.h include/openfw.h include/openfw_crypto.h include/openfw_wav.h include/openfw_fs.h include/openfw_usb_msc.h include/openfw_bt_spp.h include/openfw_xrec.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/open_fw.elf: $(OBJS) sl6801_firm.ld
	@test -n "$(LLD)" || (echo "Error: ld.lld not found (set LLD=/path/to/ld.lld)" >&2; exit 1)
	$(LLD) -T sl6801_firm.ld $(OBJS) -o $@

$(BUILD_DIR)/ram_boot.bin: $(BUILD_DIR)/open_fw.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary -j .ram_boot $< $@

$(BUILD_DIR)/xip.bin: $(BUILD_DIR)/open_fw.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary -j .text -j .data $< $@

$(BUILD_DIR)/open_fw_firm.bin: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	python3 tools/pack_firm.py --ram-boot $(BUILD_DIR)/ram_boot.bin --xip $(BUILD_DIR)/xip.bin --out-firm $@

test-host: tests/test_core.c src/crypto.c src/wav.c src/exfat.c src/usb_scsi.c src/bt_spp.c src/xrec_proto.c | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $^ -o $(BUILD_DIR)/test_core
	$(BUILD_DIR)/test_core
	dd if=/dev/zero of=$(BUILD_DIR)/test_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_exfat.img
	$(BUILD_DIR)/test_core --exfat-img $(BUILD_DIR)/test_exfat.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_exfat.img

update-up: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	@test -f "$(STOCK_DUMP)" || (echo "Error: $(STOCK_DUMP) not found" >&2; exit 1)
	python3 tools/pack_firm.py --ram-boot $(BUILD_DIR)/ram_boot.bin --xip $(BUILD_DIR)/xip.bin \
		--out-firm $(BUILD_DIR)/open_fw_firm.bin \
		--base-hlkj $(STOCK_DUMP) \
		--out-bin $(OUT_BIN) \
		--out-update-up $(OUT_UP)
	python3 ../tools/inspect_fw.py $(OUT_BIN)

clean:
	rm -rf $(BUILD_DIR)
