# Bluetooth host-side test targets (included from the main Makefile).
# Each module owns its own section; keep targets self-contained with their own BUILD_DIR.
#   test-bt-ctrl  : controller shim unit tests (fake ROM)       [owner: controller]
#   test-bt-host  : Bluedroid-on-host end-to-end harness        [owner: hosttest]
#   test-bt-port  : entropy/DRBG/store shim tests               [owner: entropy]
.PHONY: test-bt-ctrl test-bt-host test-bt-port test-bt-all
test-bt-all: test-bt-ctrl test-bt-host test-bt-port
