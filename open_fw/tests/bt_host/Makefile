# Host build of Bluedroid + bt_port + bt_ctrl + FreeRTOS-Kernel V11.1.0 (POSIX port) for the end-to-end harness
# (plan M3 path (b)). Invoked from bt/host_tests.mk (make test-bt-host / test-bt-host-tsan); run from open_fw/:
#   make -f tests/bt_host/Makefile BUILD_DIR=/tmp/x SAN=asan|tsan [-j N]
#
# Include order matters: the POSIX port directory comes first (its portmacro.h, not the type-only
# tests/bt_host/portmacro.h used by test-bt-ctrl), then tests/bt_host (host FreeRTOSConfig.h and the host
# idf_freertos_compat.h, which replace src/rtos/ and bt_port/ versions), then bt_port.
# SPDX-License-Identifier: MIT

THIS_MK   := $(abspath $(lastword $(MAKEFILE_LIST)))
ROOT      := $(abspath $(dir $(THIS_MK))/../..)
CC        := $(or $(BT_HOST_CC),clang)
BUILD_DIR ?= /tmp/sl6801_bt_host
SAN       ?= asan
BD        := $(ROOT)/third_party/bluedroid
FR        := $(ROOT)/third_party/freertos
POSIX     := $(FR)/portable/ThirdParty/GCC/Posix
HT        := $(ROOT)/tests/bt_host
OUT       := $(BUILD_DIR)/$(SAN)

# -fno-sanitize=function: Bluedroid passes osi_free_func(void *) as typed free callbacks (fixed_queue_free,
# hci_cmd_free_cb, list free_cb ...) throughout; calls through such a cast are well-defined on every ABI we target.
SAN_asan  := -fsanitize=address,undefined -fno-sanitize=function -fno-sanitize-recover=undefined \
             -fno-omit-frame-pointer
SAN_tsan  := -fsanitize=thread -fno-omit-frame-pointer -DBTH_PREEMPTION=0
SANFLAGS  := $(SAN_$(SAN))
OPT       := -O1 -g -ffunction-sections -fdata-sections

include $(ROOT)/bt/bluedroid_srcs.mk
# bt_xrec_spp_mtu.c / bt_xrec_bond.c (phone mode) include Bluedroid internals: built like vendored code, as in bt/Makefile.bluedroid.
BD_SRCS   := $(addprefix $(ROOT)/,$(BLUEDROID_SRCS)) $(ROOT)/bt_port/bt_xrec_spp_mtu.c \
             $(ROOT)/bt_port/bt_xrec_bond.c $(ROOT)/bt_port/hp_sbc.c $(ROOT)/bt_port/hp_tx_bd.c
PORT_SRCS := $(addprefix $(ROOT)/bt_port/,bt_alarm.c bt_esp_timer.c bt_entropy.c bt_drbg.c bt_store.c bt_ctrl.c \
             bt_ctrl_cmds.c bt_ctrl_evt.c bt_xrec_bluedroid.c bt_hp_bluedroid.c hp_sink.c hp_tx.c \
             bt_task_sup.c)
HOST_SRCS := $(HT)/host_heap.c $(HT)/host_glue.c $(HT)/host_ctrl.c $(HT)/test_bt_host.c $(HT)/host_link_stubs.c \
             $(HT)/host_xrec.c $(HT)/host_oom.c $(HT)/test_phone.c $(HT)/test_hp_link.c $(HT)/test_headphones.c \
             $(ROOT)/tests/fake_rom.c
KERN_SRCS := $(addprefix $(FR)/,tasks.c queue.c list.c timers.c event_groups.c stream_buffer.c) $(HT)/host_main.c
PPORT_SRCS:= $(POSIX)/port.c $(POSIX)/utils/wait_for_event.c
# Firmware sources the phone scenario runs unmodified: XREC protocol and the exFAT layer (RAM SD card).
FW_SRCS   := $(ROOT)/src/crypto.c $(ROOT)/src/xrec_proto.c $(ROOT)/src/exfat.c

obj = $(patsubst $(ROOT)/%.c,$(OUT)/%.o,$(1))
BD_OBJS   := $(call obj,$(BD_SRCS))
PORT_OBJS := $(call obj,$(PORT_SRCS))
HOST_OBJS := $(call obj,$(HOST_SRCS))
KERN_OBJS := $(call obj,$(KERN_SRCS) $(PPORT_SRCS))
FW_OBJS   := $(call obj,$(FW_SRCS))
# Headphones scenario (test_headphones.c): the fake headset decodes what it receives with the vendored SBC decoder
# (host only; the target build excludes it), as test-bt-hp does.
SBC_DEC_SRCS := $(wildcard $(BD)/host/bluedroid/external/sbc/decoder/srce/*.c)
SBC_DEC_OBJS := $(patsubst $(ROOT)/%.c,$(OUT)/sbcdec/%.o,$(SBC_DEC_SRCS))
ALL_OBJS  := $(BD_OBJS) $(PORT_OBJS) $(HOST_OBJS) $(KERN_OBJS) $(FW_OBJS) $(SBC_DEC_OBJS)

BD_INC_DIRS := $(shell find $(BD)/host/bluedroid $(BD)/common -type d \( -name include -o -path '*/api/include/api' \) \
               -not -path '*/test*' 2>/dev/null) $(BD)/host/bluedroid/bta/pan $(BD)/common/btc/profile/esp/blufi/include
INCS := -I$(POSIX) -I$(HT) $(addprefix -I,$(sort $(BD_INC_DIRS))) -I$(BD)/host/bluedroid/api/include/api \
        -I$(BD)/common/tinycrypt/port -I$(BD)/common/tinycrypt/include -I$(ROOT)/bt_port \
        -I$(BD)/deps/esp_ringbuf/include -I$(BD)/deps/ble_profiles_common/include -I$(FR)/include

# Same vendored-code warning policy as bt/Makefile.bluedroid (clang), same defines and prefix header.
BD_WARN := -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format \
           -Wno-address-of-packed-member -Wno-implicit-fallthrough -Wno-unused-const-variable \
           -Wno-pointer-sign -Wno-unterminated-string-initialization -Wno-missing-braces \
           -Wno-sometimes-uninitialized -Wno-unused-label -Wno-absolute-value -Wno-parentheses \
           -Wno-gnu-variable-sized-type-not-at-end -Wno-implicit-enum-enum-cast -Wno-constant-logical-operand
# TIMER_PARAM_TYPE (bt_target.h, #ifndef; UINT32 on target): pointer-wide on x86-64. L2CAP keeps its link control
# blocks in a heap list (l2cb.p_lcb_pool) and stores them in TIMER_LIST_ENT.param through TIMER_PARAM_TYPE; heap
# pointers are above 4 GiB on the host, so the first L2CAP link timer of a connected link crashed (phone scenario).
BD_DEFS := -DESP_PLATFORM=1 -D__ESP_ATTR_H__ -DOPENFW_BLUEDROID=1 -DBT_LOGS=0 -DOPENFW_BT_HOST_HARNESS=1 \
           -DTIMER_PARAM_TYPE=__UINTPTR_TYPE__ -include $(ROOT)/bt_port/bt_port_prefix.h
# x86-64 only: Bluedroid keeps pointers in 32-bit fields (TIMER_LIST_ENT.param = UINT32 in l2cap/rfcomm/avdt/sdp/btm
# timers, all pointing at static control blocks or code). The harness is linked -no-pie so .text/.data/.bss sit below
# 4 GiB and those round trips are exact; test_bt_host.c asserts the image end (_end) < 4 GiB at start-up.
BD_HOST_WARN := -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-int-to-void-pointer-cast \
                -Wno-void-pointer-to-int-cast
CFLAGS_BASE := $(OPT) -fno-pie -std=gnu11 $(SANFLAGS) $(BD_WARN) $(BD_DEFS) $(INCS)
# -fno-sanitize=alignment for vendored Bluedroid only: it lays out structs that hold pointers at 4-byte offsets (e.g.
# btc_transfer_context() copies the argument right after the 4-byte btc_msg_t), which is aligned for Cortex-M4 (4-byte
# pointers; LDRD/LDM need only word alignment) but not for x86-64's 8-byte pointers. Port and harness code keep it.
CFLAGS_BD   := $(CFLAGS_BASE) $(BD_HOST_WARN) $(if $(filter asan,$(SAN)),-fno-sanitize=alignment)
CFLAGS_PORT := $(CFLAGS_BASE) -Wextra -Werror -I$(ROOT)/include
CFLAGS_HOST := $(CFLAGS_PORT) -I$(ROOT)/tests -Wno-missing-field-initializers
# Kernel + POSIX port: pristine sources, no Bluedroid prefix, no bt_port (its empty pthread.h shim must not shadow
# the system one). The port's own allocations bypass the tracked allocator (__real_*), and its event mutexes are made
# robust by posix_port_shim.h (the V11.2.0 fix for a cancel-while-waiting deadlock; see the shim).
CFLAGS_KERN := $(OPT) -fno-pie -std=gnu11 $(SANFLAGS) -Wall -Wno-unused-parameter -I$(POSIX) -I$(HT) -I$(FR)/include
$(call obj,$(PPORT_SRCS)): CFLAGS_KERN += -D_GNU_SOURCE -Wno-macro-redefined -Wno-unused-variable -Dmalloc=__real_malloc -Dfree=__real_free \
                               -include $(HT)/posix_port_shim.h
# Vendored btc_spp.c: size_t* vs UBaseType_t* (same width on both targets; static-asserted in bt_port_prefix.h).
$(OUT)/third_party/bluedroid/host/bluedroid/btc/profile/std/spp/btc_spp.o: CFLAGS_BD += -Wno-incompatible-pointer-types
# fake_rom's own bookkeeping allocations are not part of the system under test (real_alloc.h).
$(OUT)/tests/fake_rom.o: CFLAGS_HOST += -include $(HT)/real_alloc.h
# Test hooks of the XREC session (bt_xrec_test_set_inflight_max: phone scenario step P5b).
$(OUT)/bt_port/bt_xrec_bluedroid.o: CFLAGS_PORT += -DBT_XREC_TEST_HOOKS=1
# The phone scenario drives the bench overrides (P17-P19): built as a bench image (make BT=1 BENCH=1) would be.
$(OUT)/bt_port/bt_xrec_bluedroid.o: CFLAGS_PORT += -DOPENFW_BT_BENCH_IMAGE=1
$(OUT)/tests/bt_host/host_xrec.o: CFLAGS_HOST += -DOPENFW_BT_BENCH_IMAGE=1
# bt_osal_time_t is uint32_t while the POSIX port's TickType_t is 64-bit: portMAX_DELAY truncates to 0xFFFFFFFF (an
# "infinite" osal wait becomes 49.7 days). Host-only difference (the firmware tick is 32-bit); bt_osal is not on the
# Classic data path.
$(OUT)/third_party/bluedroid/common/osal/src/bt_osal_freertos.o: CFLAGS_BD += -Wno-constant-conversion \
    -Wno-tautological-constant-out-of-range-compare

comma   := ,
# host_oom.c (patch 0044): btc_transfer_context fixed_queue_enqueue btc_storage_add_bonded_device
WRAPS   := malloc calloc realloc free esp_timer_create esp_timer_delete \
           btc_transfer_context fixed_queue_enqueue btc_storage_add_bonded_device
# --gc-sections as in the firmware link: unused BLE/GATT/SMP-OOB entry points reference code that is not built.
LDFLAGS := $(SANFLAGS) -no-pie -pthread -Wl,--gc-sections $(addprefix -Wl$(comma)--wrap=,$(WRAPS))

BIN := $(OUT)/test_bt_host
# test_bt_host_hp: the same harness with bt_ctrl's headphones rows forwarded (BT_CTRL_HP_GATE_LIST: 0401 0402 0405 0408),
# for the headphones smoke (`test_bt_host_hp hp`, tests/bt_host/test_hp_link.c). Only bt_ctrl_cmds differs.
BIN_HP := $(OUT)/test_bt_host_hp
CMDS_OBJ := $(OUT)/bt_port/bt_ctrl_cmds.o
CMDS_HP_OBJ := $(OUT)/bt_port/bt_ctrl_cmds_hpgates.o
all: $(BIN) $(BIN_HP)
$(BIN): $(ALL_OBJS)
	$(CC) $(LDFLAGS) $^ -lm -o $@
$(BIN_HP): $(filter-out $(CMDS_OBJ),$(ALL_OBJS)) $(CMDS_HP_OBJ)
	$(CC) $(LDFLAGS) $^ -lm -o $@
$(CMDS_HP_OBJ): $(ROOT)/bt_port/bt_ctrl_cmds.c $(THIS_MK)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_PORT) -DBT_CTRL_EXP1_PASSED_LIST=BT_CTRL_HP_GATE_LIST -MMD -MP -c $< -o $@

# The vendored SBC codec shifts negative values and lets fixed-point sums wrap (its idiom; 2s complement on both
# targets): the encoder now runs in the headphones scenario.
SBC_ENC_OBJS := $(filter $(OUT)/third_party/bluedroid/host/bluedroid/external/sbc/%,$(BD_OBJS))
$(SBC_ENC_OBJS): CFLAGS_BD += -fno-sanitize=shift,signed-integer-overflow
$(OUT)/bt_port/hp_sbc.o $(OUT)/bt_port/hp_tx_bd.o: CFLAGS_BD += -Wno-unused-parameter
$(SBC_DEC_OBJS): $(OUT)/sbcdec/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BD) -DSBC_DEC_INCLUDED=1 -w -fno-sanitize=shift,signed-integer-overflow -MMD -MP -c $< -o $@
$(OUT)/tests/bt_host/test_headphones.o: CFLAGS_HOST += -DSBC_DEC_INCLUDED=1 -Wno-unused-parameter \
    -I$(BD)/host/bluedroid/external/sbc/decoder/include -I$(BD)/host/bluedroid/external/sbc/encoder/include
$(BD_OBJS): $(OUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BD) -MMD -MP -c $< -o $@
$(PORT_OBJS): $(OUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_PORT) -MMD -MP -c $< -o $@
$(HOST_OBJS): $(OUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_HOST) -MMD -MP -c $< -o $@
$(KERN_OBJS): $(OUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KERN) -MMD -MP -c $< -o $@
$(FW_OBJS): $(OUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -fno-pie $(SANFLAGS) -Wall -Wextra -Werror -I$(ROOT)/include -MMD -MP -c $< -o $@
$(ALL_OBJS): $(THIS_MK)

-include $(ALL_OBJS:.o=.d) $(CMDS_HP_OBJ:.o=.d)
.PHONY: all
