# BT=1 firmware integration (plan docs/BLUETOOTH_BLUEDROID_PLAN.md, step 1). Included by open_fw/Makefile.
# Builds picolibc (once, outside the tree), Bluedroid + port archives, and links them into open_fw.elf with
# .data/.bss placed in .bt_data/.bt_bss (RTOS_HEAP region) by sl6801_firm.ld.
#
# SPDX-License-Identifier: MIT
ifeq ($(BT),1)
PICOLIBC_WORK ?= /tmp/sl6801_picolibc
PICOLIBC      ?= $(PICOLIBC_WORK)/pl-install
BT_BUILD      := $(BUILD_DIR)/bluedroid
BT_FW_INPUTS  := $(BT_BUILD)/bt_link_roots.o $(BT_BUILD)/libbluedroid.a $(BT_BUILD)/libbt_port.a \
                 $(PICOLIBC)/lib/libc.a $(PICOLIBC)/lib/libclang_rt_min.a

$(PICOLIBC)/lib/libc.a $(PICOLIBC)/lib/libclang_rt_min.a: tools/build_picolibc.sh
	tools/build_picolibc.sh $(PICOLIBC_WORK)

# Bluedroid is rebuilt by its own makefile (incremental); FORCE so header/config changes are seen.
# Empty recipe: make re-stats the archives after the sub-make, so a rebuilt archive relinks open_fw.elf.
$(BT_BUILD)/bt_link_roots.o $(BT_BUILD)/libbluedroid.a $(BT_BUILD)/libbt_port.a: bt-fw-libs ; @:
.PHONY: bt-fw-libs check-bt-link-roots
bt-fw-libs: $(PICOLIBC)/lib/libc.a
	$(MAKE) -f bt/Makefile.bluedroid fw BUILD_DIR=$(BT_BUILD) PICOLIBC=$(PICOLIBC) HCI_PROBE=$(HCI_PROBE) BENCH=$(or $(BENCH),0) HP_GATES=$(or $(HP_GATES),0) \
		HCI_PROBE_WAIVE_LIVENESS=$(or $(HCI_PROBE_WAIVE_LIVENESS),0) HCI_PROBE_AUTORUN=$(or $(HCI_PROBE_AUTORUN),0)

check-bt-link-roots: $(BUILD_DIR)/open_fw.elf
	tools/check_bt_link_roots.sh $< bt/bt_link_roots.c
else
BT_FW_INPUTS :=
endif
