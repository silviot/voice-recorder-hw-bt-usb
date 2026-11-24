# Bluetooth host-side test targets (included from the main Makefile).
# Each module owns its own section; keep targets self-contained with their own BUILD_DIR.
#   test-bt-ctrl  : controller shim unit tests (fake ROM)       [owner: controller]
#   test-bt-host  : Bluedroid-on-host end-to-end harness        [owner: hosttest]
#   test-bt-host-tsan : the same harness under ThreadSanitizer    [owner: hosttest]
#   test-bt-port  : entropy/DRBG/store shim tests               [owner: entropy]
.PHONY: test-bt-ctrl test-bt-host test-bt-host-tsan test-bt-port test-bt-all
test-bt-all: test-bt-ctrl test-bt-host test-bt-port

# ---- test-bt-port: entropy pool + HMAC_DRBG (plan M2, step 2). Host build, sanitizers on. ----
BT_PORT_TEST_DIR ?= /tmp/sl6801_bt_port_tests
BT_PORT_TEST_CFLAGS := -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
                       -Iinclude -Ibt_port -Itests
test-bt-port:
	@mkdir -p $(BT_PORT_TEST_DIR)
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_entropy.c bt_port/bt_entropy.c bt_port/bt_drbg.c src/crypto.c \
		-lm -o $(BT_PORT_TEST_DIR)/test_bt_entropy
	$(BT_PORT_TEST_DIR)/test_bt_entropy
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_alarm.c bt_port/bt_alarm.c -o $(BT_PORT_TEST_DIR)/test_bt_alarm
	$(BT_PORT_TEST_DIR)/test_bt_alarm
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_store.c bt_port/bt_store.c src/crypto.c -o $(BT_PORT_TEST_DIR)/test_bt_store
	$(BT_PORT_TEST_DIR)/test_bt_store
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_hci_probe.c bt_port/hci_probe_trigger.c src/exfat.c \
		-o $(BT_PORT_TEST_DIR)/test_hci_probe
	dd if=/dev/zero of=$(BT_PORT_TEST_DIR)/hcip_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BT_PORT_TEST_DIR)/hcip_exfat.img
	$(BT_PORT_TEST_DIR)/test_hci_probe $(BT_PORT_TEST_DIR)/hcip_exfat.img

# ---- test-bt-ctrl: controller shim against the fake ROM (plan M1, step 3). Host build, sanitizers on. ----
# Bluedroid's hci_packet_parser.c is compiled in (unmodified) so the mandatory-success replies are proven against the
# parser controller.c uses; tests/bt_host/portmacro.h only lets its headers compile on the host.
BT_CTRL_TEST_DIR ?= /tmp/sl6801_bt_ctrl_tests
BT_CTRL_BD := third_party/bluedroid
BT_CTRL_BD_INCS = $(addprefix -I,$(sort $(shell find $(BT_CTRL_BD)/host/bluedroid $(BT_CTRL_BD)/common -type d \
                  \( -name include -o -path '*/api/include/api' \) -not -path '*/test*' 2>/dev/null))) \
                  -I$(BT_CTRL_BD)/host/bluedroid/api/include/api -I$(BT_CTRL_BD)/deps/esp_ringbuf/include \
                  -Ithird_party/freertos/include -Itests/bt_host -Isrc/rtos -Ibt_port -Itests
BT_CTRL_BD_DEFS := -DESP_PLATFORM=1 -D__ESP_ATTR_H__ -DOPENFW_BLUEDROID=1 -DBT_LOGS=0 \
                   -include bt_port/bt_port_prefix.h
BT_CTRL_TEST_CFLAGS := $(BT_PORT_TEST_CFLAGS) -std=gnu11
test-bt-ctrl:
	@mkdir -p $(BT_CTRL_TEST_DIR)
	$(HOST_CC) $(BT_CTRL_TEST_CFLAGS) $(BT_CTRL_BD_DEFS) $(BT_CTRL_BD_INCS) -c $(BT_CTRL_BD)/host/bluedroid/hci/hci_packet_parser.c \
		-o $(BT_CTRL_TEST_DIR)/hci_packet_parser.o
	$(HOST_CC) $(BT_CTRL_TEST_CFLAGS) $(BT_CTRL_BD_DEFS) $(BT_CTRL_BD_INCS) -c tests/bt_ctrl_parser_shim.c \
		-o $(BT_CTRL_TEST_DIR)/bt_ctrl_parser_shim.o
	$(HOST_CC) $(BT_CTRL_TEST_CFLAGS) tests/test_bt_ctrl.c tests/fake_rom.c bt_port/bt_ctrl.c bt_port/bt_ctrl_cmds.c \
		bt_port/bt_ctrl_evt.c $(BT_CTRL_TEST_DIR)/hci_packet_parser.o $(BT_CTRL_TEST_DIR)/bt_ctrl_parser_shim.o \
		-o $(BT_CTRL_TEST_DIR)/test_bt_ctrl
	$(BT_CTRL_TEST_DIR)/test_bt_ctrl

# ---- test-bt-host: Bluedroid end-to-end on the FreeRTOS POSIX port (plan M3 path (b), step 5). ----
# Real Bluedroid (patches 0001-0010) + bt_port + bt_ctrl + tests/fake_rom.c on FreeRTOS-Kernel V11.1.0's POSIX port,
# ASan/UBSan: patch 0009 fixed_queue slot restore, init/enable/GAP/disable/deinit, patch 0010 NOACK at zero command
# credits, a 200-cycle leak loop, then the allocation-failure sweep over esp_bluedroid_enable() (one process per failing
# attempt; known upstream sites are waived with exact point counts in tests/bt_host/sweep_waivers.txt, anything else
# fails). Details: tests/bt_host/README.md.
BT_HOST_TEST_DIR ?= /tmp/sl6801_bt_host_tests
BT_HOST_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
test-bt-host:
	$(MAKE) -f tests/bt_host/Makefile BUILD_DIR=$(BT_HOST_TEST_DIR) SAN=asan -j$(BT_HOST_JOBS)
	$(BT_HOST_TEST_DIR)/asan/test_bt_host all 200
	python3 tests/bt_host/run_sweep.py $(BT_HOST_TEST_DIR)/asan/test_bt_host --jobs $(BT_HOST_JOBS) --timeout 120 \
		--waivers tests/bt_host/sweep_waivers.txt

# TSan variant: cooperative scheduling (the POSIX port's in-signal-handler task switch deadlocks under TSan), two
# suppressions for races inside the port itself (tests/bt_host/tsan.supp).
test-bt-host-tsan:
	$(MAKE) -f tests/bt_host/Makefile BUILD_DIR=$(BT_HOST_TEST_DIR) SAN=tsan -j$(BT_HOST_JOBS)
	TSAN_OPTIONS="suppressions=$(CURDIR)/tests/bt_host/tsan.supp report_signal_unsafe=0 halt_on_error=1" \
		$(BT_HOST_TEST_DIR)/tsan/test_bt_host all 50
