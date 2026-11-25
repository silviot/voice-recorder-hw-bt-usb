# Build system for Cassini OpenFW (standalone clean-room SL6801 firmware)
#
# Usage:
#   make -C open_fw            # Compile & pack standalone open_fw FIRM partition (= `all`; BT=1: Bluedroid, needs picolibc)
#   make -C open_fw BT=0       # The same without Bluetooth (no picolibc / meson / ninja needed)
#   make -C open_fw test-host  # Compile & run host unit tests (crypto, wav, exfat, LVGL UI)
#   make -C open_fw update-up  # Also build 2 MB flash image + 0:\update.up (requires ../dumps/firmware_2M.bin)
#   make -C open_fw clean      # Remove build artifacts
#
# SPDX-License-Identifier: MIT

# `all` must be the default goal: the picolibc rule (bt/firmware.mk) and $(CONFIG_STAMP) appear before it, and make
# would otherwise pick whichever comes first (a goal-less `make` then only wrote .config or checked libc.a).
.DEFAULT_GOAL := all

CLANG    ?= clang
HOST_CC  ?= cc
LLD      ?= $(shell which ld.lld 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name ld.lld 2>/dev/null | head -n 1)
WASM_LD  ?= $(shell which wasm-ld 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name wasm-ld 2>/dev/null | head -n 1)
OBJCOPY  ?= $(shell which llvm-objcopy 2>/dev/null || find /usr/lib "$${ANDROID_HOME:-$$HOME/Android/Sdk}/ndk" -maxdepth 8 \( -type f -o -type l \) -executable -name llvm-objcopy 2>/dev/null | head -n 1)
AR       ?= $(shell which llvm-ar 2>/dev/null || which ar 2>/dev/null)

LVGL_DIR := third_party/lvgl
LVGL_TAG := v9.2.2

ifeq ($(DEV_EXP),1)
BUILD_DIR ?= /tmp/sl6801_openfw_devexp
else
BUILD_DIR ?= /tmp/sl6801_openfw
endif

# BT=1 (default since plan step 9, parity reached): Bluedroid host stack (docs/BLUETOOTH_BLUEDROID_PLAN.md); implies
# RTOS=1 and builds picolibc once (tools/build_picolibc.sh: meson + ninja). BT=0: the supported image without Bluetooth.
BT ?= 1
ifeq ($(BT),1)
override RTOS := 1
endif
# RTOS=1: run the app as a FreeRTOS task (prerequisite for Bluedroid; forced by BT=1). BT=0 images default to 0.
RTOS ?= 0
RTOS_SELFTEST ?= 0
FREERTOS_DIR := third_party/freertos
ifeq ($(RTOS),1)
RTOS_INC   := -Isrc/rtos -Isrc/rtos/shim -I$(FREERTOS_DIR)/include -I$(FREERTOS_DIR)/portable/GCC/ARM_CM4F
RTOS_DEFS  := -DOPENFW_RTOS=1 -DOPENFW_RTOS_SELFTEST=$(RTOS_SELFTEST) -DOPENFW_BT_BLUEDROID=$(BT)
else
RTOS_INC   :=
RTOS_DEFS  := -DOPENFW_RTOS=0
endif

LVGL_INC := -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1

CFLAGS   := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb \
            -Os -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector \
            -ffunction-sections -fdata-sections \
            -Iinclude -Isrc/ui -Isrc/ui/eez $(LVGL_INC) $(RTOS_INC) $(RTOS_DEFS) -I$(BUILD_DIR)
ifeq ($(DEV_EXP),1)
CFLAGS   += -DOPENFW_ENABLE_USB_EXP=1 -DOPENFW_UNATTENDED_DEV=1
else ifeq ($(UNATTENDED_DEV),1)
CFLAGS   += -DOPENFW_UNATTENDED_DEV=1
endif
# HCI_PROBE=1 (requires BT=1): Exp 1 stage A/B probe image (plan M5; bt_port/hci_probe.c, docs/BT_HW_RESULTS.md).
# Diagnostic only: no phone or headphones mode (the Bluetooth tile shows a toast).
HCI_PROBE ?= 0
ifeq ($(HCI_PROBE),1)
ifneq ($(BT),1)
$(error HCI_PROBE=1 requires BT=1)
endif
CFLAGS   += -DOPENFW_HCI_PROBE=1
endif
# BENCH=1 (BT=1 phone images only): the bench image for unattended silicon tests (docs/BT_HW_RESULTS.md "Phone mode
# (BT=1) on silicon"): the one-shot SD:/BTBENCH.RUN trigger and its overrides (SSP autoconfirm, fixed XREC PIN). A
# release BT=1 image has neither, so card or USB-disk access alone cannot arm an unattended pairing (review finding).
BENCH ?= 0
ifeq ($(BENCH),1)
ifneq ($(BT)$(HCI_PROBE),10)
$(error BENCH=1 requires BT=1 without HCI_PROBE=1)
endif
CFLAGS   += -DOPENFW_BT_BENCH_IMAGE=1
endif
# HP_GATES=1 (with BENCH=1): bench HCI trace in bt_ctrl. The headphones rows 0401 0402 0405 0408 are forwarded in
# every BT=1 image (bt/Makefile.bluedroid).
ifeq ($(HP_GATES),1)
ifneq ($(BENCH),1)
$(error HP_GATES=1 requires BENCH=1)
endif
endif
# KEYTEST_MISMATCH=1 (with BENCH=1; plan step 6b): stage the session private key with the PUBLIC key of a second,
# unrelated key pair. If the ROM really uses the staged pair, SSP pairing must fail (DHKey check); if it silently
# keeps its fixed key, pairing succeeds. Test image only: it can never pair.
KEYTEST_MISMATCH ?= 0
ifeq ($(KEYTEST_MISMATCH),1)
ifneq ($(BENCH),1)
$(error KEYTEST_MISMATCH=1 requires BENCH=1)
endif
CFLAGS   += -DOPENFW_BT_KEYTEST_MISMATCH=1
endif

LVGL_CFLAGS := --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb \
               -O2 -ffreestanding -fno-builtin -fno-stack-protector \
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
	$(wildcard $(LVGL_DIR)/src/widgets/image/*.c) \
	$(wildcard $(LVGL_DIR)/src/widgets/label/*.c)

LVGL_OBJS      := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl_arm/%.o,$(LVGL_SRCS))
LVGL_HOST_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl_host/%.o,$(LVGL_SRCS))

EEZ_SRCS  := src/ui/eez/ui.c src/ui/eez/screens.c src/ui/eez/styles.c src/ui/eez/images.c

# CORE_SRCS: every image (and, minus startup.c, the WASM build, which is pinned to the no-BT configuration). The
# Bluetooth phone/headphones modes are bt_port/ (bt/firmware.mk, BT=1); xrec_proto.c is their XREC protocol.
CORE_SRCS := src/startup.c src/wdt_timer.c src/rtc.c src/uart.c src/pmu.c \
             src/keys.c src/lcd_st7735s.c src/cpu1_ui.c src/ui_spectrogram.c \
             src/ui/ui_port.c src/ui/ui_theme.c src/ui/ui_fonts.c src/ui/ui_i18n.c src/ui/ui_screens.c \
             $(EEZ_SRCS) \
             src/sdio.c src/audio_dma.c src/audio_dac.c src/audio_dsp.c \
             src/usb_scsi.c src/usb_musb.c src/bt_rf.c \
             src/xrec_proto.c src/recorder.c src/player.c \
             src/exp_runner.c src/crypto.c src/wav.c src/cwx.c src/exfat.c \
             src/persist.c src/apmem.c src/hsprobe_log.c src/hsprobe.c src/main.c
SRCS      := $(CORE_SRCS)
ifeq ($(BT),1)
SRCS      += src/audio_out.c   # output dispatch DAC / headset / both (include/audio_out.h; BT=0 aliases the DAC)
SRCS      += src/ui/ui_i18n_bt.c   # BT UI strings, 18 languages (include/openfw_i18n_bt.h; not in the BT=0 image)
endif
OBJS      := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
$(BUILD_DIR)/audio_out.o: CFLAGS += -Ibt_port

# FreeRTOS kernel + glue (ARM link only; not part of WASM/host builds)
RTOS_SRCS := $(FREERTOS_DIR)/tasks.c $(FREERTOS_DIR)/queue.c $(FREERTOS_DIR)/list.c \
             $(FREERTOS_DIR)/timers.c $(FREERTOS_DIR)/event_groups.c $(FREERTOS_DIR)/stream_buffer.c \
             $(FREERTOS_DIR)/portable/GCC/ARM_CM4F/port.c $(FREERTOS_DIR)/portable/MemMang/heap_5.c \
             src/rtos/rtos_port.c
ifneq ($(BT),1)
RTOS_SRCS += src/rtos/libc_min.c   # BT=1 links picolibc instead
endif
RTOS_OBJS := $(patsubst %.c,$(BUILD_DIR)/rtos/%.o,$(RTOS_SRCS))
ifeq ($(RTOS_SELFTEST),1)
RTOS_OBJS += $(BUILD_DIR)/rtos/src/rtos/rtos_selftest.o
endif
ifeq ($(RTOS),1)
ARM_OBJS  := $(OBJS) $(RTOS_OBJS)
else
ARM_OBJS  := $(OBJS)
endif

include bt/firmware.mk

SPLASH_PNG  := ../assets/cassini_welcome_80x40.png
SPLASH_HDR  := $(BUILD_DIR)/cassini_splash.h
STOCK_DUMP  := ../dumps/firmware_2M.bin
OUT_BIN     := ../dumps/firmware_2M_openfw.bin
OUT_UP      := ../dumps/update_openfw.up
# QEMU-only probe images (HCI_PROBE_AUTORUN runs the probe on every boot; HCI_PROBE_WAIVE_LIVENESS waives its checks)
# never go to ../dumps, whose 2 MB image is what tools/dev_cycle.py flash writes (docs/BT_HW_RESULTS.md).
HCI_PROBE_QEMU_ONLY := $(filter-out 0,$(HCI_PROBE_AUTORUN) $(HCI_PROBE_WAIVE_LIVENESS))

.PHONY: all test-host test-host-san test-qemu test-qemu-rtos test-qemu-bt test-qemu-hciprobe test-qemu-hciprobe-full qemu-stub update-up wasm clean force-config fetch-lvgl

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

$(BUILD_DIR)/main.o: src/main.c $(SPLASH_HDR) $(CONFIG_STAMP) include/sl6801_regs.h include/openfw.h include/openfw_ui.h include/openfw_i18n.h include/openfw_i18n_bt.h include/openfw_exp.h include/openfw_player.h include/openfw_apmem.h include/openfw_hsprobe.h include/board_config.h src/ui/ui_port.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c $(CONFIG_STAMP) include/sl6801_regs.h include/sl6801_rom.h include/openfw.h include/openfw_ui.h include/openfw_i18n.h include/openfw_i18n_bt.h include/audio_dsp.h include/openfw_exp.h include/openfw_crypto.h include/openfw_wav.h include/openfw_cwx.h include/openfw_player.h include/openfw_apmem.h include/openfw_fs.h include/openfw_usb_msc.h include/openfw_xrec.h include/openfw_hsprobe.h include/board_config.h include/lv_conf.h | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rtos/%.o: %.c $(CONFIG_STAMP) src/rtos/FreeRTOSConfig.h include/openfw_rtos.h | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(filter-out -Wextra,$(CFLAGS)) -Wno-unused-parameter -c $< -o $@

$(BUILD_DIR)/open_fw.elf: $(ARM_OBJS) $(BUILD_DIR)/liblvgl.a $(BT_FW_INPUTS) sl6801_firm.ld
	@test -n "$(LLD)" || (echo "Error: ld.lld not found (set LLD=/path/to/ld.lld)" >&2; exit 1)
	$(LLD) --gc-sections -T sl6801_firm.ld $(ARM_OBJS) $(BUILD_DIR)/liblvgl.a $(BT_FW_INPUTS) -o $@
ifeq ($(BT),1)
	tools/check_bt_link_roots.sh $@ bt/bt_link_roots.c
endif

$(BUILD_DIR)/ram_boot.bin: $(BUILD_DIR)/open_fw.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary -j .ram_boot $< $@

$(BUILD_DIR)/xip.bin: $(BUILD_DIR)/open_fw.elf
	@test -n "$(OBJCOPY)" || (echo "Error: llvm-objcopy not found (set OBJCOPY=/path/to/llvm-objcopy)" >&2; exit 1)
	$(OBJCOPY) -O binary -j .text -j .data -j .bt_data -j .bt_fast $< $@

$(BUILD_DIR)/open_fw_firm.bin: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	python3 tools/pack_firm.py --ram-boot $(BUILD_DIR)/ram_boot.bin --xip $(BUILD_DIR)/xip.bin --out-firm $@

test-host: $(LVGL_DIR)/lvgl.h $(BUILD_DIR)/liblvgl_host.a tests/test_core.c tests/test_xrec_rec.c src/crypto.c src/wav.c src/cwx.c src/exfat.c src/persist.c src/apmem.c src/usb_scsi.c src/xrec_proto.c | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_core.c src/crypto.c src/wav.c src/cwx.c src/exfat.c src/persist.c src/apmem.c src/usb_scsi.c src/xrec_proto.c -o $(BUILD_DIR)/test_core
	$(BUILD_DIR)/test_core
	dd if=/dev/zero of=$(BUILD_DIR)/test_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_exfat.img
	$(BUILD_DIR)/test_core --exfat-img $(BUILD_DIR)/test_exfat.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_exfat.img
	@# +XREC PUT REC,<stem>.cwx (transcript sidecar into /record) on its own fresh image
	$(HOST_CC) $(HOST_CFLAGS) tests/test_xrec_rec.c src/xrec_proto.c src/crypto.c src/exfat.c -o $(BUILD_DIR)/test_xrec_rec
	dd if=/dev/zero of=$(BUILD_DIR)/test_xrec_rec.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_xrec_rec.img
	$(BUILD_DIR)/test_xrec_rec $(BUILD_DIR)/test_xrec_rec.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_xrec_rec.img
	python3 -I ../tools/test_bt_sync_xrec.py > $(BUILD_DIR)/test_bt_sync_xrec.log
	@tail -n 1 $(BUILD_DIR)/test_bt_sync_xrec.log
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO -DOPENFW_ENABLE_USB_EXP=1 -DOPENFW_UNATTENDED_DEV=1 tests/test_drivers_mmio.c \
		src/wdt_timer.c src/uart.c src/rtc.c src/pmu.c src/keys.c \
		src/sdio.c src/lcd_st7735s.c src/usb_musb.c src/usb_scsi.c src/exfat.c \
		src/exp_runner.c src/crypto.c \
		-o $(BUILD_DIR)/test_drivers_mmio
	$(BUILD_DIR)/test_drivers_mmio $(BUILD_DIR)/test_exfat.img
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_ui_render.c \
		src/lcd_st7735s.c src/cpu1_ui.c src/ui_spectrogram.c src/cwx.c \
		src/ui/ui_port.c src/ui/ui_theme.c src/ui/ui_fonts.c src/ui/ui_i18n.c src/ui/ui_screens.c \
		$(EEZ_SRCS) \
		$(BUILD_DIR)/liblvgl_host.a \
		-lm -o $(BUILD_DIR)/test_ui_render
	$(BUILD_DIR)/test_ui_render $(BUILD_DIR)/ui_all_screens.ppm
	@# BT=1 phone-mode UI (prompt, peer line, toasts): the same renderer with OPENFW_BT_BLUEDROID
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO -DOPENFW_BT_BLUEDROID=1 tests/test_ui_render.c \
		src/lcd_st7735s.c src/cpu1_ui.c src/ui_spectrogram.c src/cwx.c \
		src/ui/ui_port.c src/ui/ui_theme.c src/ui/ui_fonts.c src/ui/ui_i18n.c src/ui/ui_i18n_bt.c src/ui/ui_screens.c \
		$(EEZ_SRCS) \
		$(BUILD_DIR)/liblvgl_host.a \
		-lm -o $(BUILD_DIR)/test_ui_render_bt
	$(BUILD_DIR)/test_ui_render_bt $(BUILD_DIR)/ui_all_screens_bt.ppm
	$(HOST_CC) $(HOST_CFLAGS) tests/test_audio_dsp.c src/audio_dsp.c -lm -o $(BUILD_DIR)/test_audio_dsp
	$(BUILD_DIR)/test_audio_dsp
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_player.c src/player.c src/ui/ui_i18n.c src/wav.c src/cwx.c -o $(BUILD_DIR)/test_player
	$(BUILD_DIR)/test_player
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_audio_bt_mmio.c \
		src/audio_dma.c src/audio_dac.c src/audio_dsp.c src/recorder.c src/bt_rf.c \
		src/wav.c src/exfat.c src/rtc.c src/wdt_timer.c \
		-o $(BUILD_DIR)/test_audio_bt_mmio
	dd if=/dev/zero of=$(BUILD_DIR)/test_rec_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_rec_exfat.img
	$(BUILD_DIR)/test_audio_bt_mmio $(BUILD_DIR)/test_rec_exfat.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_rec_exfat.img
	$(HOST_CC) $(HOST_CFLAGS) -DOPENFW_HOST_MMIO tests/test_hsprobe.c src/hsprobe_log.c src/hsprobe.c \
		src/keys.c src/exfat.c -o $(BUILD_DIR)/test_hsprobe
	dd if=/dev/zero of=$(BUILD_DIR)/test_hsp_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BUILD_DIR)/test_hsp_exfat.img
	$(BUILD_DIR)/test_hsprobe $(BUILD_DIR)/test_hsp_exfat.img
	/usr/sbin/fsck.exfat -n $(BUILD_DIR)/test_hsp_exfat.img

test-host-san:
	$(MAKE) test-host HOST_CFLAGS="$(HOST_CFLAGS) -fsanitize=undefined,address -fno-omit-frame-pointer" LVGL_HOST_CFLAGS="$(LVGL_HOST_CFLAGS) -fsanitize=undefined,address -fno-omit-frame-pointer" BUILD_DIR="$(BUILD_DIR)_san"

update-up: $(BUILD_DIR)/ram_boot.bin $(BUILD_DIR)/xip.bin tools/pack_firm.py
	@test -f "$(STOCK_DUMP)" || (echo "Error: $(STOCK_DUMP) not found" >&2; exit 1)
	@test -z "$(HCI_PROBE_QEMU_ONLY)" -o -z "$(filter $(abspath ../dumps),$(abspath $(dir $(OUT_BIN)) $(dir $(OUT_UP))))" \
		|| (echo "Error: QEMU-only probe image (HCI_PROBE_AUTORUN / HCI_PROBE_WAIVE_LIVENESS): set OUT_BIN and OUT_UP outside ../dumps" >&2; exit 1)
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

-include bt/host_tests.mk

# FreeRTOS scheduler/primitives self-test + normal boot checks on the SL6801 QEMU model, RTOS without Bluetooth (BT=0;
# the BT=1 image has its own gate, test-qemu-bt).
# Usage: make -C open_fw test-qemu-rtos   (needs tools/setup_qemu.sh + ../dumps/*)
RTOS_QEMU_DIR ?= /tmp/sl6801_openfw_rtos_selftest
test-qemu-rtos:
	$(MAKE) update-up qemu-stub BT=0 RTOS=1 RTOS_SELFTEST=1 BUILD_DIR=$(RTOS_QEMU_DIR)
	python3 tools/run_qemu.py --elf $(RTOS_QEMU_DIR)/open_fw.elf --stub $(RTOS_QEMU_DIR)/qemu_boot_stub.bin \
		--flash $(OUT_BIN) --skip-keys --png-out $(RTOS_QEMU_DIR)/qemu_lcd.png

# Same gate for the BT=1 image (Bluedroid linked; BT is not started at boot).
BT_QEMU_DIR ?= /tmp/sl6801_openfw_bt_selftest
test-qemu-bt:
	$(MAKE) update-up qemu-stub BT=1 RTOS_SELFTEST=1 BUILD_DIR=$(BT_QEMU_DIR)
	python3 tools/run_qemu.py --elf $(BT_QEMU_DIR)/open_fw.elf --stub $(BT_QEMU_DIR)/qemu_boot_stub.bin \
		--flash $(OUT_BIN) --skip-keys --png-out $(BT_QEMU_DIR)/qemu_lcd.png

# Exp 1 probe image (BT=1 HCI_PROBE=1). QEMU has no RW-BT controller: the probe must report the IRQ-26
# liveness timeout, recover once, abort cleanly, put IRQ 26's priority back and release RW-BT (claimed=0),
# then write its log copy to the SD card (HCIPROBE.LOG), while the boot checks and the RTOS self-test still pass.
# HCI_PROBE_AUTORUN=1 (stage A; 2 = stage A + B): the blank QEMU SD card has no HCIPROBE.RUN request (the trigger
# itself is host-tested in test-bt-port). These images are written to their BUILD_DIR, never to ../dumps (update-up
# refuses that), and refuse to run on a part with programmed eFuses (HCIP OFF trig=qemu_only_on_silicon).
HCIPROBE_QEMU_DIR ?= /tmp/sl6801_openfw_hciprobe
test-qemu-hciprobe:
	$(MAKE) update-up qemu-stub BT=1 HCI_PROBE=1 HCI_PROBE_AUTORUN=1 RTOS_SELFTEST=1 BUILD_DIR=$(HCIPROBE_QEMU_DIR) \
		OUT_BIN=$(HCIPROBE_QEMU_DIR)/firmware_2M_qemu_only.bin OUT_UP=$(HCIPROBE_QEMU_DIR)/update_qemu_only.up
	python3 tools/run_qemu.py --elf $(HCIPROBE_QEMU_DIR)/open_fw.elf --stub $(HCIPROBE_QEMU_DIR)/qemu_boot_stub.bin \
		--flash $(HCIPROBE_QEMU_DIR)/firmware_2M_qemu_only.bin --skip-keys --png-out $(HCIPROBE_QEMU_DIR)/qemu_lcd.png \
		--expect-log "HCIP BEGIN" --expect-log "trig=autorun" --expect-log "HCIP TIMEOUT what=irq26" \
		--expect-log "claimed=0 owner=0 lockout=0" --expect-log "HCIP END st=ABORT" \
		--expect-log "HCIP LOGFILE file=HCIPROBE.LOG st=OK"

# Dev check of the probe's full sequence (stage A reads, idle window, 5 restarts; stage B writes, read-backs, scan
# windows; log volume) under QEMU: the IRQ-26 liveness check and stage B's ROM replies (QEMU's ROM never answers a
# forwarded command) are waived (logged waived=1), the scan window is 3 s. Separate BUILD_DIR, which also holds its
# 2 MB image (never ../dumps).
HCIPROBE_FULL_QEMU_DIR ?= /tmp/sl6801_openfw_hciprobe_full
test-qemu-hciprobe-full:
	$(MAKE) update-up qemu-stub BT=1 HCI_PROBE=1 HCI_PROBE_WAIVE_LIVENESS=1 HCI_PROBE_AUTORUN=2 RTOS_SELFTEST=1 \
		BUILD_DIR=$(HCIPROBE_FULL_QEMU_DIR) \
		OUT_BIN=$(HCIPROBE_FULL_QEMU_DIR)/firmware_2M_qemu_only.bin OUT_UP=$(HCIPROBE_FULL_QEMU_DIR)/update_qemu_only.up
	python3 tools/run_qemu.py --elf $(HCIPROBE_FULL_QEMU_DIR)/open_fw.elf --stub $(HCIPROBE_FULL_QEMU_DIR)/qemu_boot_stub.bin \
		--flash $(HCIPROBE_FULL_QEMU_DIR)/firmware_2M_qemu_only.bin --skip-keys --png-out $(HCIPROBE_FULL_QEMU_DIR)/qemu_lcd.png --expect-seconds 150 \
		--expect-log "HCIP RESTART i=5" --expect-log "trig=autorun cpu_pm=700 cpu_n=10" --expect-log "HCIP WIN w=scan" \
		--expect-log "HCIP STAGE s=B st=DONE" --expect-log "HCIP STACK ctrl_size=4096" \
		--expect-log "claimed=0 owner=0 lockout=0" --expect-log "HCIP END st=DONE" \
		--expect-log "HCIP LOGFILE file=HCIPROBE.LOG st=OK"

WASM_BUILD_DIR := $(BUILD_DIR)_wasm
WASM_OUT       := ../web_emu/openfw.wasm
WASM_CFLAGS    := --target=wasm32 -Os -Wall -Wextra -ffreestanding -fno-builtin \
                  -fno-stack-protector -DOPENFW_HOST_MMIO -DOPENFW_WASM=1 \
                  -Iinclude -Isrc/ui -Isrc/ui/eez $(LVGL_INC) -I$(BUILD_DIR)
WASM_LVGL_CFLAGS := --target=wasm32 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
                    -DOPENFW_WASM=1 -Wno-unused-parameter -Wno-sign-compare -Iinclude $(LVGL_INC)
# No-BT configuration whatever BT is (audio_out.c / ui_i18n_bt.c are BT=1 only; WASM_CFLAGS has no OPENFW_BT_BLUEDROID).
WASM_SRCS      := $(filter-out src/startup.c,$(CORE_SRCS)) wasm/wasm_platform.c
WASM_OBJS      := $(patsubst %.c,$(WASM_BUILD_DIR)/%.o,$(WASM_SRCS))
WASM_LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(WASM_BUILD_DIR)/lvgl/%.o,$(LVGL_SRCS))

$(WASM_BUILD_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c include/lv_conf.h include/board_config.h
	@mkdir -p $(dir $@)
	$(CLANG) $(WASM_LVGL_CFLAGS) -c $< -o $@

$(WASM_BUILD_DIR)/src/main.o: src/main.c $(SPLASH_HDR) include/sl6801_regs.h include/openfw.h include/openfw_ui.h include/openfw_i18n.h include/openfw_exp.h include/openfw_player.h include/openfw_apmem.h include/openfw_hsprobe.h include/board_config.h src/ui/ui_port.h
	@mkdir -p $(dir $@)
	$(CLANG) $(WASM_CFLAGS) -c $< -o $@

$(WASM_BUILD_DIR)/%.o: %.c include/sl6801_regs.h include/sl6801_rom.h include/openfw.h include/openfw_ui.h include/openfw_i18n.h include/audio_dsp.h include/openfw_exp.h include/openfw_crypto.h include/openfw_wav.h include/openfw_cwx.h include/openfw_player.h include/openfw_apmem.h include/openfw_fs.h include/openfw_usb_msc.h include/openfw_xrec.h include/openfw_hsprobe.h include/board_config.h include/lv_conf.h
	@mkdir -p $(dir $@)
	$(CLANG) $(WASM_CFLAGS) -c $< -o $@

$(WASM_OUT): $(LVGL_DIR)/lvgl.h $(WASM_OBJS) $(WASM_LVGL_OBJS)
	@test -n "$(WASM_LD)" || (echo "Error: wasm-ld not found (set WASM_LD=/path/to/wasm-ld)" >&2; exit 1)
	@mkdir -p $(dir $@)
	$(WASM_LD) --no-entry --gc-sections --allow-undefined --stack-first -z stack-size=65536 \
		--export-dynamic $(WASM_OBJS) $(WASM_LVGL_OBJS) -o $@
	@ls -lh $@

wasm: $(WASM_OUT)

clean:
	rm -rf $(BUILD_DIR) $(WASM_BUILD_DIR)
