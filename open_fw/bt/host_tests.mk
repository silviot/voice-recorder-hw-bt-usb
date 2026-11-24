# Bluetooth host-side test targets (included from the main Makefile).
# Each module owns its own section; keep targets self-contained with their own BUILD_DIR.
#   test-bt-ctrl  : controller shim unit tests (fake ROM)       [owner: controller]
#   test-bt-host  : Bluedroid-on-host end-to-end harness        [owner: hosttest]
#   test-bt-host-tsan : the same harness under ThreadSanitizer    [owner: hosttest]
#   test-bt-port  : entropy/DRBG/store shim tests               [owner: entropy]
#   test-bt-xrec  : phone-mode XREC session on mocked esp_spp/esp_bt_gap [owner: xrec]
.PHONY: test-bt-ctrl test-bt-host test-bt-host-tsan test-bt-port test-bt-xrec test-bt-all
test-bt-all: test-bt-ctrl test-bt-host test-bt-port test-bt-xrec

# ---- test-bt-port: entropy pool + HMAC_DRBG (plan M2, step 2). Host build, sanitizers on. ----
BT_PORT_TEST_DIR ?= /tmp/sl6801_bt_port_tests
BT_PORT_TEST_CFLAGS := -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
                       -Iinclude -Ibt_port -Itests
# ROM RNG / P-192 facts re-derived from the mask ROM when the gitignored dump is present (skipped otherwise).
BT_ROM_DUMP ?= $(wildcard ../dumps/bootrom_384K.bin)
test-bt-port:
	@mkdir -p $(BT_PORT_TEST_DIR)
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_entropy.c bt_port/bt_entropy.c bt_port/bt_drbg.c src/crypto.c \
		-lm -o $(BT_PORT_TEST_DIR)/test_bt_entropy
	$(BT_PORT_TEST_DIR)/test_bt_entropy
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_p192.c bt_port/bt_p192.c -o $(BT_PORT_TEST_DIR)/test_bt_p192
	$(BT_PORT_TEST_DIR)/test_bt_p192
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_rom_rng.c bt_port/bt_rom_rng.c bt_port/bt_p192.c \
		-o $(BT_PORT_TEST_DIR)/test_bt_rom_rng
	$(BT_PORT_TEST_DIR)/test_bt_rom_rng $(BT_ROM_DUMP)
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_alarm.c bt_port/bt_alarm.c -o $(BT_PORT_TEST_DIR)/test_bt_alarm
	$(BT_PORT_TEST_DIR)/test_bt_alarm
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_store.c bt_port/bt_store.c src/crypto.c -o $(BT_PORT_TEST_DIR)/test_bt_store
	$(BT_PORT_TEST_DIR)/test_bt_store
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_hci_probe.c bt_port/hci_probe_trigger.c src/exfat.c \
		-o $(BT_PORT_TEST_DIR)/test_hci_probe
	dd if=/dev/zero of=$(BT_PORT_TEST_DIR)/hcip_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BT_PORT_TEST_DIR)/hcip_exfat.img
	$(BT_PORT_TEST_DIR)/test_hci_probe $(BT_PORT_TEST_DIR)/hcip_exfat.img
	$(HOST_CC) $(BT_PORT_TEST_CFLAGS) tests/test_bt_bench.c bt_port/bt_bench.c src/exfat.c \
		-o $(BT_PORT_TEST_DIR)/test_bt_bench
	$(BT_PORT_TEST_DIR)/test_bt_bench $(BT_PORT_TEST_DIR)/hcip_exfat.img

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
	@# The same tests with the headphones rows forwarded (BT_CTRL_HP_GATE_LIST: 0401 0402 0405 0408; design H2 gate
	@# "green with and without the gate list"): the table validates, and the inquiry/page fake-ROM models run.
	$(HOST_CC) $(BT_CTRL_TEST_CFLAGS) -DBT_CTRL_EXP1_PASSED_LIST=BT_CTRL_HP_GATE_LIST tests/test_bt_ctrl.c \
		tests/fake_rom.c bt_port/bt_ctrl.c bt_port/bt_ctrl_cmds.c bt_port/bt_ctrl_evt.c \
		$(BT_CTRL_TEST_DIR)/hci_packet_parser.o $(BT_CTRL_TEST_DIR)/bt_ctrl_parser_shim.o \
		-o $(BT_CTRL_TEST_DIR)/test_bt_ctrl_hp
	$(BT_CTRL_TEST_DIR)/test_bt_ctrl_hp

# ---- test-bt-host: Bluedroid end-to-end on the FreeRTOS POSIX port (plan M3 path (b), step 5). ----
# Real Bluedroid (patches 0001-0027) + bt_port + bt_ctrl + tests/fake_rom.c on FreeRTOS-Kernel V11.1.0's POSIX port,
# ASan/UBSan: patch 0009 fixed_queue slot restore, init/enable/GAP/disable/deinit, patch 0010 NOACK at zero command
# credits, a 200-cycle leak loop, the phone-mode end-to-end scenario (BT=1 XREC backend + fake ROM laptop: SSP, bond
# store, SDP, RFCOMM, XREC PAIR/CHAL/GETS with congestion (session window and BTC ring), reconnect, stop with traffic
# outstanding, stop with a detach that never completes, restart, policy rejections incl. stale Just Works bonds; tests/bt_host/test_phone.c, on a 4096-sector exFAT image), then the allocation-failure sweeps over
# esp_bluedroid_init/enable/disable/deinit (one process per failing attempt; waivers, if any, carry exact point counts
# in tests/bt_host/sweep_waivers.txt, anything else fails; none since patches 0011-0023). Details:
# tests/bt_host/README.md.
BT_HOST_TEST_DIR ?= /tmp/sl6801_bt_host_tests
BT_HOST_JOBS ?= $(shell nproc 2>/dev/null || echo 4)
test-bt-host:
	$(MAKE) -f tests/bt_host/Makefile BUILD_DIR=$(BT_HOST_TEST_DIR) SAN=asan -j$(BT_HOST_JOBS)
	$(BT_HOST_TEST_DIR)/asan/test_bt_host all 200
	dd if=/dev/zero of=$(BT_HOST_TEST_DIR)/phone_exfat.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BT_HOST_TEST_DIR)/phone_exfat.img
	$(BT_HOST_TEST_DIR)/asan/test_bt_host phone $(BT_HOST_TEST_DIR)/phone_exfat.img
	set -e; for m in init enable disable deinit; do \
		python3 tests/bt_host/run_sweep.py $(BT_HOST_TEST_DIR)/asan/test_bt_host --mode $$m --jobs $(BT_HOST_JOBS) \
			--timeout 120 --waivers tests/bt_host/sweep_waivers.txt; done

# TSan variant: cooperative scheduling (the POSIX port's in-signal-handler task switch deadlocks under TSan), two
# suppressions for races inside the port itself (tests/bt_host/tsan.supp).
test-bt-host-tsan:
	$(MAKE) -f tests/bt_host/Makefile BUILD_DIR=$(BT_HOST_TEST_DIR) SAN=tsan -j$(BT_HOST_JOBS)
	TSAN_OPTIONS="suppressions=$(CURDIR)/tests/bt_host/tsan.supp report_signal_unsafe=0 halt_on_error=1" \
		$(BT_HOST_TEST_DIR)/tsan/test_bt_host all 50
	dd if=/dev/zero of=$(BT_HOST_TEST_DIR)/phone_exfat_tsan.img bs=512 count=4096 status=none
	/usr/sbin/mkfs.exfat -q -b 4096 -c 4096 $(BT_HOST_TEST_DIR)/phone_exfat_tsan.img
	TSAN_OPTIONS="suppressions=$(CURDIR)/tests/bt_host/tsan.supp report_signal_unsafe=0 halt_on_error=1" \
		$(BT_HOST_TEST_DIR)/tsan/test_bt_host phone $(BT_HOST_TEST_DIR)/phone_exfat_tsan.img

# ---- test-bt-xrec: XREC session over Bluedroid SPP (plan M4): bt_port/bt_xrec_bluedroid.c + the real xrec_proto
# against mocked esp_spp / esp_bt_gap APIs (real Bluedroid API headers) and a mocked platform. Host build, ASan/UBSan.
BT_XREC_TEST_DIR ?= /tmp/sl6801_bt_xrec_tests
test-bt-xrec:
	@mkdir -p $(BT_XREC_TEST_DIR)
	$(HOST_CC) $(BT_CTRL_TEST_CFLAGS) $(BT_CTRL_BD_DEFS) $(BT_CTRL_BD_INCS) -DBT_XREC_TEST_HOOKS=1 \
		-DOPENFW_BT_BENCH_IMAGE=1 \
		tests/test_bt_xrec.c bt_port/bt_xrec_bluedroid.c src/xrec_proto.c src/crypto.c src/exfat.c \
		-o $(BT_XREC_TEST_DIR)/test_bt_xrec
	$(BT_XREC_TEST_DIR)/test_bt_xrec
	@# A release BT=1 session (no BENCH=1) must not contain the SD bench overrides (autoconfirm, fixed XREC PIN).
	$(HOST_CC) $(BT_CTRL_TEST_CFLAGS) $(BT_CTRL_BD_DEFS) $(BT_CTRL_BD_INCS) -c bt_port/bt_xrec_bluedroid.c \
		-o $(BT_XREC_TEST_DIR)/bt_xrec_release.o
	@if nm $(BT_XREC_TEST_DIR)/bt_xrec_release.o | grep -i bench; then \
		echo "FAIL: release bt_xrec_bluedroid.o has bench override symbols (above)" >&2; exit 1; \
	else echo "release session object: no bench overrides"; fi
