# Bluetooth host-side test targets (included from the main Makefile).
# Each module owns its own section; keep targets self-contained with their own BUILD_DIR.
#   test-bt-ctrl  : controller shim unit tests (fake ROM)       [owner: controller]
#   test-bt-host  : Bluedroid-on-host end-to-end harness        [owner: hosttest]
#   test-bt-port  : entropy/DRBG/store shim tests               [owner: entropy]
.PHONY: test-bt-ctrl test-bt-host test-bt-port test-bt-all
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
