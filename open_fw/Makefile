# Build system for Cassini OpenFW (standalone clean-room SL6801 firmware)
#
# Usage:
#   make -C open_fw            # Compile & pack standalone open_fw FIRM partition
#   make -C open_fw test-host  # Compile & run host unit tests (crypto, wav, exfat, LVGL UI)
#   make -C open_fw update-up  # Also build 2 MB flash image + 0:\update.up (requires ../dumps/firmware_2M.bin)
#   make -C open_fw clean      # Remove build artifacts
#
# SPDX-License-Identifier: MIT

CLANG    ?= clang
HOST_CC  ?= cc
LLD      ?= $(shell which ld.lld 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name ld.lld 2>/dev/null | head -n 1)
OBJCOPY  ?= $(shell which llvm-objcopy 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name llvm-objcopy 2>/dev/null | head -n 1)
AR       ?= $(shell which llvm-ar 2>/dev/null || which ar 2>/dev/null)

LVGL_DIR := third_party/lvgl
LVGL_TAG := v9.2.2

ifeq ($(DEV_EXP),1)
BUILD_DIR ?= /tmp/sl6801_openfw_devexp
else
BUILD_DIR ?= /tmp/sl6801_openfw
endif

LVGL_INC := -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1

CFLAGS   := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb \
            -Os -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector \
            -ffunction-sections -fdata-sections \
            -Iinclude -Isrc/ui -Isrc/ui/eez $(LVGL_INC) -I$(BUILD_DIR)
ifeq ($(DEV_EXP),1)
CFLAGS   += -DOPENFW_ENABLE_USB_EXP=1 -DOPENFW_UNATTENDED_DEV=1
else ifeq ($(UNATTENDED_DEV),1)
CFLAGS   += -DOPENFW_UNATTENDED_DEV=1
endif

LVGL_CFLAGS := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb \
               -Os -ffreestanding -fno-builtin -fno-stack-protector \
               -ffunction-sections -fdata-sections \
               -Wno-unused-parameter -Wno-sign-compare \
               -Iinclude $(LVGL_INC)

HOST_CFLAGS := -O2 -Wall -Wextra -Werror -Iinclude -Isrc/ui -Isrc/ui/eez $(LVGL_INC)
LVGL_HOST_CFLAGS := -O2 -Wno-unused-parameter -Wno-sign-compare -Iinclude $(LVGL_INC)

# Minimal LVGL v9.2 source set (core, display, SW draw, flex, built-in stdlib, enabled widgets)
LVGL_SRCS := \
	$(LVGL_DIR)/src/lv_init.c \
	$(wildcard $(LVGL_DIR)/src/core/*.c) \
	$(wildcard $(LVGL_DIR)/src/display/*.c) \
	$(wildcard $(LVGL_DIR)/src/draw/*.c) \
	$(wildcard $(LVGL_DIR)/src/draw/sw/*.c) \
	$(wildcard $(LVGL_DIR)/src/draw/sw/blend/*.c) \
	$(LVGL_DIR)/src/font/lv_font.c \
	$(LVGL_DIR)/src/font/lv_font_fmt_txt.c \
	$(wildcard $(LVGL_DIR)/src/indev/*.c) \
	$(wildcard $(LVGL_DIR)/src/layouts/*.c) \
	$(wildcard $(LVGL_DIR)/src/layouts/flex/*.c) \
	$(wildcard $(LVGL_DIR)/src/libs/bin_decoder/*.c) \
	$(wildcard $(LVGL_DIR)/src/misc/*.c) \
	$(wildcard $(LVGL_DIR)/src/misc/cache/*.c) \
	$(wildcard $(LVGL_DIR)/src/osal/*.c) \
	$(wildcard $(LVGL_DIR)/src/stdlib/*.c) \
	$(wildcard $(LVGL_DIR)/src/stdlib/builtin/*.c) \
	$(wildcard $(LVGL_DIR)/src/tick/*.c) \
	$(wildcard $(LVGL_DIR)/src/themes/*.c) \
	$(wildcard $(LVGL_DIR)/src/widgets/bar/*.c) \
	$(wildcard $(LVGL_DIR)/src/widgets/button/*.c) \
	$(wildcard $(LVGL_DIR)/src/widgets/image/*.c) \
	$(wildcard $(LVGL_DIR)/src/widgets/label/*.c)

LVGL_OBJS      := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl_arm/%.o,$(LVGL_SRCS))
LVGL_HOST_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl_host/%.o,$(LVGL_SRCS))

EEZ_SRCS  := src/ui/eez/ui.c src/ui/eez/screens.c src/ui/eez/styles.c src/ui/eez/images.c

SRCS      := src/startup.c src/wdt_timer.c src/rtc.c src/uart.c src/pmu.c \
             src/keys.c src/lcd_st7735s.c src/ui_spectrogram.c \
             src/ui/ui_port.c src/ui/ui_theme.c src/ui/ui_fonts.c src/ui/ui_screens.c \
             $(EEZ_SRCS) \
             src/sdio.c src/audio_dma.c src/audio_dac.c src/audio_dsp.c \
             src/usb_scsi.c src/usb_musb.c src/bt_rf.c src/bt_spp.c \
             src/xrec_proto.c src/bt_hci.c src/recorder.c src/player.c \
             src/exp_runner.c src/crypto.c src/wav.c src/cwx.c src/exfat.c \
             src/persist.c src/main.c
OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

SPLASH_PNG  := ../assets/cassini_welcome_80x40.png
SPLASH_HDR  := $(BUILD_DIR)/cassini_splash.h
STOCK_DUMP  := ../dumps/firmware_2M.bin
OUT_BIN     := ../dumps/firmware_2M_openfw.bin
OUT_UP      := ../dumps/update_openfw.up

.PHONY: all test-host test-host-san test-qemu qemu-stub update-up clean force-config fetch-lvgl

CONFIG_STAMP := $(BUILD_DIR)/.config

$(CONFIG_STAMP): force-config | $(BUILD_DIR)
	@printf '%s\n' '$(CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv $@.tmp $@; fi

fetch-lvgl: $(LVGL_DIR)/lvgl.h

$(LVGL_DIR)/lvgl.h:
	@mkdir -p $(dir $(LVGL_DIR))
	git clone --depth 1 --branch $(LVGL_TAG) https://github.com/lvgl/lvgl.git $(LVGL_DIR)
	@rm -rf $(LVGL_DIR)/.git

all: $(LVGL_DIR)/lvgl.h $(BUILD_DIR)/open_fw_firm.bin

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(SPLASH_HDR): $(SPLASH_PNG) tools/png_to_rgb565_h.py | $(BUILD_DIR)
	python3 tools/png_to_rgb565_h.py $(SPLASH_PNG) --out $@

$(BUILD_DIR)/lvgl_arm/%.o: $(LVGL_DIR)/%.c include/lv_conf.h include/board_config.h
	@mkdir -p $(dir $@)
	$(CLANG) $(LVGL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/liblvgl.a: $(LVGL_OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/lvgl_host/%.o: $(LVGL_DIR)/%.c include/lv_conf.h include/board_config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(LVGL_HOST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/liblvgl_host.a: $(LVGL_HOST_OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/main.o: src/main.c $(SPLASH_HDR) $(CONFIG_STAMP) include/sl6801_regs.h include/openfw.h include/openfw_ui.h include/openfw_exp.h include/openfw_player.h include/board_config.h src/ui/ui_port.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c $(CONFIG_STAMP) include/sl6801_regs.h include/sl6801_rom.h include/openfw.h include/openfw_ui.h include/audio_dsp.h include/openfw_exp.h include/openfw_crypto.h include/openfw_wav.h include/openfw_cwx.h include/openfw_player.h include/openfw_fs.h include/openfw_usb_msc.h include/openfw_bt_spp.h include/openfw_xrec.h include/board_config.h include/lv_conf.h | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/open_fw.elf: $(OBJS) $(BUILD_DIR)/liblvgl.a sl6801_firm.ld
	@test -n "$(LLD)" || (echo "Error: ld.lld not found (set LLD=/path/to/ld.lld)" >&2; exit 1)
	$(LLD) --gc-sections -T sl6801_firm.ld $(OBJS) $(BUILD_DIR)/liblvgl.a -o $@

$(BUILD_DIR)/ram_boot.bin: $(BUILD_DIR)/open_fw.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary -j .ram_boot $< $@

$(BUILD_DIR)/xip.bin: $(BUILD_DIR)/open_fw.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary -j .text -j .data $< $@

$(BUILD_DIR)/open_fw_firm.bin: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	python3 tools/pack_firm.py --ram-boot $(BUILD_DIR)/ram_boot.bin --xip $(BUILD_DIR)/xip.bin --out-firm $@

test-host: $(LVGL_DIR)/lvgl.h $(BUILD_DIR)/liblvgl_host.a tests/test_core.c src/crypto.c src/wav.c src/cwx.c src/exfat.c src/persist.c src/usb_scsi.c src/bt_spp.c src/xrec_proto.c | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_core.c src/crypto.c src/wav.c src/cwx.c src/exfat.c src/persist.c src/usb_scsi.c src/bt_spp.c src/xrec_proto.c -o $(BUILD_DIR)/test_core
	$(BUILD_DIR)/test_core
	dd if=/dev/zero of=$(BUILD_DIR)/test_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_exfat.img
	$(BUILD_DIR)/test_core --exfat-img $(BUILD_DIR)/test_exfat.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_exfat.img
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO -DOPENFW_ENABLE_USB_EXP=1 -DOPENFW_UNATTENDED_DEV=1 tests/test_drivers_mmio.c \
		src/wdt_timer.c src/uart.c src/rtc.c src/pmu.c src/keys.c \
		src/sdio.c src/lcd_st7735s.c src/usb_musb.c src/usb_scsi.c src/exfat.c \
		src/exp_runner.c src/crypto.c \
		-o $(BUILD_DIR)/test_drivers_mmio
	$(BUILD_DIR)/test_drivers_mmio $(BUILD_DIR)/test_exfat.img
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_ui_render.c \
		src/lcd_st7735s.c src/ui_spectrogram.c \
		src/ui/ui_port.c src/ui/ui_theme.c src/ui/ui_fonts.c src/ui/ui_screens.c \
		$(EEZ_SRCS) \
		$(BUILD_DIR)/liblvgl_host.a \
		-lm -o $(BUILD_DIR)/test_ui_render
	$(BUILD_DIR)/test_ui_render $(BUILD_DIR)/ui_all_screens.ppm
	$(HOST_CC) $(HOST_CFLAGS) tests/test_audio_dsp.c src/audio_dsp.c -lm -o $(BUILD_DIR)/test_audio_dsp
	$(BUILD_DIR)/test_audio_dsp
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_player.c src/player.c src/wav.c src/cwx.c -o $(BUILD_DIR)/test_player
	$(BUILD_DIR)/test_player
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_audio_bt_mmio.c \
		src/audio_dma.c src/audio_dac.c src/audio_dsp.c src/recorder.c src/bt_rf.c \
		src/wav.c src/exfat.c src/rtc.c src/wdt_timer.c \
		-o $(BUILD_DIR)/test_audio_bt_mmio
	dd if=/dev/zero of=$(BUILD_DIR)/test_rec_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_rec_exfat.img
	$(BUILD_DIR)/test_audio_bt_mmio $(BUILD_DIR)/test_rec_exfat.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_rec_exfat.img

test-host-san:
	$(MAKE) test-host HOST_CFLAGS="$(HOST_CFLAGS) -fsanitize=undefined,address -fno-omit-frame-pointer" LVGL_HOST_CFLAGS="$(LVGL_HOST_CFLAGS) -fsanitize=undefined,address -fno-omit-frame-pointer" BUILD_DIR="$(BUILD_DIR)_san"

update-up: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	@test -f "$(STOCK_DUMP)" || (echo "Error: $(STOCK_DUMP) not found" >&2; exit 1)
	python3 tools/pack_firm.py --ram-boot $(BUILD_DIR)/ram_boot.bin --xip $(BUILD_DIR)/xip.bin \
		--out-firm $(BUILD_DIR)/open_fw_firm.bin \
		--base-hlkj $(STOCK_DUMP) \
		--out-bin $(OUT_BIN) \
		--out-update-up $(OUT_UP)
	python3 ../tools/inspect_fw.py $(OUT_BIN)

qemu-stub: $(BUILD_DIR)/qemu_boot_stub.bin

$(BUILD_DIR)/qemu_boot_stub.bin: tools/qemu_boot_stub.S | $(BUILD_DIR)
	@test -n "$(LLD)" || (echo "Error: ld.lld not found (set LLD=/path/to/ld.lld)" >&2; exit 1)
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(CLANG) --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -c $< -o $(BUILD_DIR)/qemu_boot_stub.o
	$(LLD) -Ttext=0x10000000 -e _qemu_stub_vectors $(BUILD_DIR)/qemu_boot_stub.o -o $(BUILD_DIR)/qemu_boot_stub.elf
	$(OBJCOPY) -O binary $(BUILD_DIR)/qemu_boot_stub.elf $@

test-qemu: all update-up $(BUILD_DIR)/qemu_boot_stub.bin
	python3 tools/run_qemu.py --elf $(BUILD_DIR)/open_fw.elf --stub $(BUILD_DIR)/qemu_boot_stub.bin --flash $(OUT_BIN)

clean:
	rm -rf $(BUILD_DIR)
