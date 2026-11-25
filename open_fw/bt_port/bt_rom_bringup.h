/* bt_rom_bringup: the RW-BT ROM controller bring-up bt_rom_real.c reuses (moved verbatim from the legacy src/bt_hci.c,
 * plan step 9; see bt_rom_bringup.c). Every BT=1 image; callers: bt_rom_real.c start() only, in this order, after the
 * EM wipe, the soft reset, the heap init, the kernel hooks and TASK_HCI (docs/BT_ROM_INTERFACE.md §2.1).
 * SPDX-License-Identifier: MIT */
#ifndef BT_ROM_BRINGUP_H
#define BT_ROM_BRINGUP_H
#include <stdint.h>

/* The ROM kernel's heap (ke_malloc / ke_free hooks at 0x008041CC / 0x008041D0; allocator in bt_rom_real.c). */
#define BT_HEAP_WORDS 2048u
extern uint32_t g_bt_heap[BT_HEAP_WORDS];

/* F:1109-1287: LD layer (CS / LD / SCO / rwip_rf hook tables, ld_init SRAM constants, EM, TX power and RSSI tables,
 * local features, RWBTEN). */
void bt_rom_bringup_ld_layer(const uint8_t bd_addr[6]);
/* F:1289-1311: LC layer (hook table 0x008021BC, TASK_LC and its 7 instances). */
void bt_rom_bringup_lc_layer(void);
/* F:1313-1351: LM layer (LM env, SSP / sync / ACL / con-info / P-192 init, TASK_LM). */
void bt_rom_bringup_lm_layer(void);

#endif
