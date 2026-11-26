/* bt_rom_bringup: the SL6801 RW-BT mask-ROM controller bring-up that bt_rom_real.c runs at every start(): the static
 * 8 KiB ROM heap g_bt_heap, the LD / LC / LM layer initialisation (hook tables, SRAM constants, EM, TX power / RSSI
 * tables, RWBTEN) and the hooks and RF callbacks those tables point at. Moved verbatim from the legacy host
 * src/bt_hci.c (deleted in plan step 9, docs/BLUETOOTH_BLUEDROID_PLAN.md): every function body below is that file's,
 * unchanged; only rwbt_init_{ld,lc,lm}_layer became the exported bt_rom_bringup_{ld,lc,lm}_layer and g_bt_heap lost
 * "static". Docs cite the original lines as F:<line>: src/bt_hci.c at 7c76b77, the last commit that has it.
 *
 *   F:120-183   bt_memzero / bt_memcpy (EM-safe word copies, no libc)
 *   F:427-479   hook_noop / hook_ret0, CS hooks 0x00802BB0, LD hook 0x00802904[0], LC hook 0x008021BC[0]
 *   F:490-540   rwip_rf callbacks (TX power inc/dec/EPC, RSSI conversion)
 *   F:997-1030  TX power / RSSI tables
 *   F:1109-1351 rwbt_init_ld_layer / rwbt_init_lc_layer / rwbt_init_lm_layer
 *   F:233, F:239 BT_HEAP_WORDS / g_bt_heap (bt_rom_bringup.h)
 *
 * Built into libbt_port.a (every BT=1 image); sl6801_firm.ld keeps this object's .bss (g_bt_heap) in main SRAM, where
 * bt_hci.c had it, instead of .bt_bss. Compiled with -ffreestanding -fno-builtin like the rest of the firmware
 * (bt/Makefile.bluedroid), so the copy loops stay loops.
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "bt_rom_bringup.h"

#define TASK_LM                 0x38u
#define TASK_LC                 0x39u
#define TASK_LD                 0x3Bu

#define EM_BASE_ADDR            0x40030000u
#define EM_SIZE_BYTES           0x8000u

uint32_t g_bt_heap[BT_HEAP_WORDS] __attribute__((aligned(8)));

/* ============================================================================
 * Bare-Metal Memory Helpers (no libc calls)
 * ============================================================================ */
typedef uint32_t __attribute__((may_alias)) bt_u32_t;

static void bt_memzero(void *dst, uint32_t n)
{
    uintptr_t addr = (uintptr_t)dst;
    if (addr >= EM_BASE_ADDR && addr < (EM_BASE_ADDR + EM_SIZE_BYTES)) {
        volatile uint8_t *d = (volatile uint8_t *)dst;
        while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
            *d++ = 0u;
            n--;
        }
        volatile uint32_t *dw = (volatile uint32_t *)d;
        while (n >= 4u) {
            *dw++ = 0u;
            n -= 4u;
        }
        d = (volatile uint8_t *)dw;
        while (n > 0u) {
            *d++ = 0u;
            n--;
        }
        return;
    }

    uint8_t *d = (uint8_t *)dst;
    while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
        *d++ = 0u;
        n--;
    }
    bt_u32_t *dw = (bt_u32_t *)d;
    while (n >= 4u) {
        *dw++ = 0u;
        n -= 4u;
    }
    d = (uint8_t *)dw;
    while (n > 0u) {
        *d++ = 0u;
        n--;
    }
}

static void bt_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
            *d++ = *s++;
            n--;
        }
        bt_u32_t *dw = (bt_u32_t *)d;
        const bt_u32_t *sw = (const bt_u32_t *)s;
        while (n >= 4u) {
            *dw++ = *sw++;
            n -= 4u;
        }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n > 0u) {
        *d++ = *s++;
        n--;
    }
}

/* ============================================================================
 * RW-BT Active-Link / RF Hook Implementations
 * ============================================================================ */
static void hook_noop(void) {}
static uint32_t hook_ret0(void) { return 0u; }

static void cs_set_common(uint8_t *cs, uint8_t a_val)
{
    cs[0x0A] = (uint8_t)((cs[0x0A] & 0x03u) | a_val);
    cs[0x14] = *(volatile uint8_t *)0x00802CF0u;
    volatile uint32_t *w30 = (volatile uint32_t *)(cs + 0x30);
    *w30 = (*w30 & ~0x78000000u) | 0x10000000u;
}

static void hook_bb0_2_3(uint8_t *cs) { cs_set_common(cs, 0x40u); }
static void hook_bb0_4_6(uint8_t *cs) { cs_set_common(cs, 0x58u); }
static void hook_bb0_5(uint8_t *cs)   { cs[0x14] = *(volatile uint8_t *)0x00802CF0u; }

static void hook_2904_00(void *cs, uint8_t link_id)
{
    (void)cs;
    volatile uint8_t *env = (volatile uint8_t *)0x0080296Cu + link_id;
    if (env[0x110] == 0u) {
        env[0x14F] = 2u;
        rom_ld_acl_txpwr_init(link_id, 2u, 1u);
    }
}

static void hook_21bc_acl_con_cmp(uint16_t dest_id)
{
    uint32_t link_id = (uint32_t)dest_id >> 8;
    uint8_t *lc      = (uint8_t *)0x00801384u + 0x208u * link_id;

    if (lc[0x55] == 0u) {
        rom_lc_con_cmp_evt(dest_id, 0u);
        lc[0x55] = 1u;
        rom_ld_acl_con_setup1(lc[0x41]);
    }
    lc[0x52] = 1u;

    uint16_t pkt = *(uint16_t *)(lc + 0x22);
    uint16_t m1  = rom_lc_pkt_mask1(pkt, 0x7Fu, lc[0x6C]);
    uint16_t m2  = rom_lc_pkt_mask2(pkt, 0xFAu, lc[0x6F], 0xDBu, lc[0x70], 0xFDu, lc[0x71]);
    *(uint16_t *)(lc + 0x22) = (uint16_t)(m1 | m2);

    rom_ld_acl_con_setup3(lc[0x41]);
    rom_ke_msg_send_basic(0xE485u, dest_id, dest_id);

    uint32_t has_feat = rom_lc_feat_check(lc + 0x6C, 0x12u);
    rom_ld_acl_con_setup2(lc[0x41], 0u, has_feat ? 0u : 1u);

    lc[0x204] = 0u;
    rom_ke_msg_send_basic(0xE4FAu, dest_id, dest_id);
    rom_ke_state_set(dest_id, 5u); /* LC_CONNECTED */
    rom_ke_msg_send_basic(0xE4EEu, dest_id, dest_id);
}

static uint8_t rf_txpwr_inc(uint8_t link_id)
{
    volatile uint8_t *cs = (volatile uint8_t *)(0x400302B0u + (uint32_t)link_id * 0x5Cu);
    uint8_t max_pwr = *(volatile uint8_t *)0x00803C0Cu;
    uint8_t cur     = cs[0x14];
    if (cur < max_pwr) {
        uint8_t nxt = (uint8_t)(cur + 1u);
        if ((nxt & 1u) == 0u && nxt < max_pwr) {
            nxt = (uint8_t)(cur + 2u);
            if (nxt > max_pwr) {
                nxt = max_pwr;
            }
        }
        cs[0x14] = nxt;
        return 0u;
    }
    return 1u;
}

static uint8_t rf_txpwr_dec(uint8_t link_id)
{
    volatile uint8_t *cs = (volatile uint8_t *)(0x400302B0u + (uint32_t)link_id * 0x5Cu);
    uint8_t cur = cs[0x14];
    if (cur > 2u) {
        uint8_t nxt = (uint8_t)(cur - 1u);
        cs[0x14] = ((nxt & 1u) == 0u) ? (uint8_t)(cur - 2u) : nxt;
        return 0u;
    }
    return 1u;
}

static uint8_t rf_txpwr_epc(uint8_t link_id, uint8_t req)
{
    if (req == 0u) {
        return rf_txpwr_dec(link_id) ? 0x3Fu : 0x15u;
    }
    if (req == 1u) {
        return rf_txpwr_inc(link_id) ? 0x2Au : 0x15u;
    }
    if (req == 2u) {
        volatile uint8_t *cs = (volatile uint8_t *)(0x400302B0u + (uint32_t)link_id * 0x5Cu);
        cs[0x14] = *(volatile uint8_t *)0x00803C0Cu;
        return 0x2Au;
    }
    return 0u;
}

static int8_t rf_rssi_convert(uint16_t raw)
{
    return (int8_t)((((raw >> 3) << 1) - 0x40u) & 0xFEu);
}

/* ============================================================================
 * Clean-Room RW-BT Controller Initialization (100% BootROM + OpenFW Hooks)
 * ============================================================================ */
static const uint8_t g_pwr_t_802d80[0x15] = {
    0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802cf4[0x46] = {
    0x1Eu, 0x00u, 0x3Cu, 0x00u, 0x1Du, 0x00u, 0x3Cu, 0x00u, 0x00u, 0x01u,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802e75[0x10] = {
    0xFFu, 0xFFu, 0xFFu, 0x00u, 0x00u, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0x00u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u
};
static const uint8_t g_pwr_t_802d95[0x15] = {
    0x00u, 0x00u, 0x01u, 0x08u, 0x03u, 0x03u, 0x10u, 0x07u, 0x00u, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802d3a[0x46] = {
    0x1Eu, 0x00u, 0x3Cu, 0x00u, 0x05u, 0x00u, 0x3Cu, 0x00u, 0x00u, 0x01u,
    0x1Eu, 0x00u, 0x3Cu, 0x00u, 0x05u, 0x00u, 0x3Cu, 0x00u, 0x00u, 0x02u,
    0x14u, 0x00u, 0x3Cu, 0x00u, 0x05u, 0x00u, 0x3Cu, 0x00u, 0x01u, 0x02u,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
};
static const uint8_t g_pwr_t_802e85[0x10] = {
    0xFFu, 0xFFu, 0xFFu, 0x00u, 0x01u, 0xFFu, 0xFFu, 0xFFu,
    0x02u, 0xFFu, 0x01u, 0x02u, 0xFFu, 0xFFu, 0x01u, 0x02u
};

void bt_rom_bringup_ld_layer(const uint8_t bd_addr[6])
{
    uint32_t *bb0 = (uint32_t *)0x00802BB0u;
    bb0[0] = 0x00036485u;
    bb0[1] = 0x000364EDu;
    bb0[2] = (uint32_t)(uintptr_t)hook_bb0_2_3;
    bb0[3] = (uint32_t)(uintptr_t)hook_bb0_2_3;
    bb0[4] = (uint32_t)(uintptr_t)hook_bb0_4_6;
    bb0[5] = (uint32_t)(uintptr_t)hook_bb0_5;
    bb0[6] = (uint32_t)(uintptr_t)hook_bb0_4_6;
    bb0[7] = 0x00034B5Du;

    uint32_t *ldh = (uint32_t *)0x00802904u;
    ldh[0]  = (uint32_t)(uintptr_t)hook_2904_00; /* +0x00 */
    ldh[1]  = (uint32_t)(uintptr_t)hook_ret0;    /* +0x04 */
    ldh[2]  = (uint32_t)(uintptr_t)hook_ret0;    /* +0x08 */
    ldh[3]  = (uint32_t)(uintptr_t)hook_ret0;    /* +0x0C */
    ldh[4]  = 0x0002E649u;                       /* +0x10 */
    ldh[5]  = 0x00039E01u;                       /* +0x14 */
    ldh[6]  = 0x0003A7EDu;                       /* +0x18 */
    ldh[7]  = 0x0002DE05u;                       /* +0x1C */
    ldh[8]  = 0x0002E041u;                       /* +0x20 */
    ldh[9]  = 0x0002E2A5u;                       /* +0x24 */
    ldh[10] = 0x0002E4C5u;                       /* +0x28 */
    ldh[11] = 0x0003A8B9u;                       /* +0x2C */
    ldh[12] = 0x0003C779u;                       /* +0x30 */
    ldh[13] = 0x0003AE9Du;                       /* +0x34 */
    ldh[14] = 0x0003C119u;                       /* +0x38 */
    ldh[15] = 0x0003A439u;                       /* +0x3C */
    ldh[16] = 0x0003A781u;                       /* +0x40 */
    ldh[17] = (uint32_t)(uintptr_t)hook_ret0;    /* +0x44 */
    ldh[18] = 0x0003B379u;                       /* +0x48 */
    ldh[19] = (uint32_t)(uintptr_t)hook_ret0;    /* +0x4C */
    ldh[20] = (uint32_t)(uintptr_t)hook_ret0;    /* +0x50 */
    ldh[21] = 0x0003DAA9u;                       /* +0x54 */
    ldh[22] = 0x000366E9u;                       /* +0x58 */
    ldh[23] = 0x0003AB65u;                       /* +0x5C */

    uint32_t *sco = (uint32_t *)0x008030CCu;
    sco[0] = (uint32_t)(uintptr_t)hook_ret0;
    sco[1] = 0x0003E791u;
    sco[2] = 0x0003E181u;

    /* Install rwip_rf vtable @ 0x00803BDC */
    uint32_t *rf = (uint32_t *)0x00803BDCu;
    rf[0]  = (uint32_t)(uintptr_t)hook_noop;       /* +0x00 */
    rf[1]  = (uint32_t)(uintptr_t)hook_noop;       /* +0x04 */
    rf[2]  = (uint32_t)(uintptr_t)rf_txpwr_inc;    /* +0x08 */
    rf[3]  = (uint32_t)(uintptr_t)rf_txpwr_dec;    /* +0x0C */
    rf[4]  = (uint32_t)(uintptr_t)rf_txpwr_epc;    /* +0x10 */
    rf[5]  = (uint32_t)(uintptr_t)hook_ret0;       /* +0x14 */
    rf[6]  = (uint32_t)(uintptr_t)hook_ret0;       /* +0x18 */
    rf[7]  = (uint32_t)(uintptr_t)rf_rssi_convert; /* +0x1C */
    rf[8]  = (uint32_t)(uintptr_t)hook_noop;       /* +0x20 */
    rf[9]  = (uint32_t)(uintptr_t)hook_ret0;       /* +0x24 */
    rf[10] = (uint32_t)(uintptr_t)hook_noop;       /* +0x28 */
    rf[11] = (uint32_t)(uintptr_t)hook_noop;       /* +0x2C */
    REG8(0x00803BDCu + 0x30u) = 0x12u;             /* txpwr_max */

    bt_memzero((void *)0x0080296Cu, 0x1C4u);
    bt_memzero((void *)0x00802C04u, 0x28u);
    bt_memzero((void *)0x00802CF4u, 0x374u);
    bt_memzero((void *)0x008030D8u, 0x148u);
    bt_memzero((void *)0x00803220u, 0x6Cu);
    bt_memzero((void *)0x00802BD0u, 8u);
    REG8(0x00802BD1u) = 1u; /* Blocker #2 (0x00D5711C, read by BootROM) */

    bt_memzero((void *)0x00802B48u, 0x68u);
    bt_memzero((void *)0x00802208u, 0x614u);
    bt_memzero((void *)0x00802C2Cu, 0xC0u);
    bt_memzero((void *)0x0080281Cu, 0xE8u);
    bt_memzero((void *)0x00802BD8u, 0x12u);

    /* Blocker #1: 37 non-zero SRAM constants from stock ld_init prelude (0x00D56D00) */
    REG16(0x00802B46u) = 0x001Cu;
    REG16(0x00802B44u) = 0x0064u;
    REG16(0x00802964u) = 0x0032u;
    REG8(0x00803C14u)  = 0x00u;
    REG8(0x00803C15u)  = 0x0Fu;
    REG8(0x00803C16u)  = 0x27u;
    REG8(0x00803C17u)  = 0x06u;
    REG8(0x00802B42u)  = 0x1Eu;
    REG8(0x00802966u)  = 0x07u;
    REG8(0x00802967u)  = 0x0Fu;
    REG8(0x00802968u)  = 0x02u;
    REG8(0x00802969u)  = 0x03u;
    REG8(0x0080296Au)  = 0x1Cu;
    REG8(0x0080296Bu)  = 0x03u;
    static const uint8_t k_2b36[12] = {
        0x01u, 0x13u, 0x12u, 0x11u, 0x10u, 0x0Eu, 0x0Du, 0x0Cu, 0x0Bu, 0x0Au, 0x09u, 0x08u
    };
    for (uint32_t i = 0u; i < 12u; i++) {
        REG8(0x00802B36u + i) = k_2b36[i];
    }
    REG8(0x00804208u)  = 0x02u;
    REG8(0x00804209u)  = 0x0Au;
    REG8(0x0080420Au)  = 0x02u;
    REG8(0x0080420Bu)  = 0x0Eu;
    REG8(0x00804222u)  = 0x03u;
    REG8(0x00804223u)  = 0x16u;
    REG8(0x0080422Au)  = 0x03u;
    REG8(0x0080422Bu)  = 0x16u;
    REG8(0x0080422Cu)  = 0x02u;
    REG8(0x0080422Du)  = 0x12u;
    REG8(0x0080422Eu)  = 0x1Eu;

    REG8(0x00802CF0u)  = 0x12u;
    REG16(0x00803C12u) = 0x14u;
    REG8(0x00802B34u)  = 1u;

    rom_ke_task_create(TASK_LD, (const void *)0x00800608u);
    rom_ld_bd_addr_set(bd_addr);

    for (uint32_t a = EM_BASE_ADDR; a < EM_BASE_ADDR + 0x20u; a += 2u) {
        REG16(a) = 0x6000u;
    }
    for (uint32_t a = 0x00802AF3u; a <= 0x00802B02u; a++) {
        REG8(a) = 0xFFu;
    }

    rom_em_buf_init();
    rom_ld_sched_init();

    for (uint8_t cs = 0u; cs < 10u; cs++) {
        rom_ld_util_cs_init(cs, (const uint8_t *)0x00802C14u, (const uint8_t *)0x00802BEBu);
    }

    REG16(0x00802B48u + 0x40u) = 0x2000u;
    REG16(0x00802B48u + 0x4Eu) = 0x0038u;
    REG8(0x00802B48u  + 0x49u) = 0u;
    REG8(0x00802B48u  + 0x4Du) = 0u;
    REG8(0x00802B48u  + 0x65u) = 1u;
    REG8(0x00802B48u  + 0x66u) = 0u;
    REG8(0x00802B48u  + 0x67u) = 0u;

    REG8(0x0080296Cu  + 0x197u) = 0u;
    REG8(0x0080296Cu  + 0x19Du) = 0u;
    REG8(0x0080296Cu  + 0x19Eu) = 0u;
    REG32(0x0080296Cu + 0x1A4u) = 0x1Bu;
    REG32(0x0080296Cu + 0x1A8u) = 0x070004FFu;

    REG8(0x00802C04u + 0x19u) = 1u;
    REG8(0x00802C04u + 0x1Au) = 0u;
    REG8(0x00802C04u + 0x1Bu) = 0xFAu;
    REG8(0x00802C04u + 0x1Cu) = 0x0Au;
    REG8(0x00802C04u + 0x1Du) = 0u;
    REG32(0x00802C04u + 0x20u) = 0u;

    REG8(0x008030D8u + 0x113u) = 1u;

    bt_memzero((void *)0x400313A8u, 0xF0u);
    RWBT_REG(0x0C0u) = 0x014313A8u;

    /* Copy TX power & RSSI tables */
    bt_memcpy((void *)0x00802CF4u, g_pwr_t_802cf4, (uint32_t)sizeof(g_pwr_t_802cf4));
    bt_memcpy((void *)0x00802D3Au, g_pwr_t_802d3a, (uint32_t)sizeof(g_pwr_t_802d3a));
    bt_memcpy((void *)0x00802D80u, g_pwr_t_802d80, (uint32_t)sizeof(g_pwr_t_802d80));
    bt_memcpy((void *)0x00802D95u, g_pwr_t_802d95, (uint32_t)sizeof(g_pwr_t_802d95));
    bt_memcpy((void *)0x00802E75u, g_pwr_t_802e75, (uint32_t)sizeof(g_pwr_t_802e75));
    bt_memcpy((void *)0x00802E85u, g_pwr_t_802e85, (uint32_t)sizeof(g_pwr_t_802e85));
    REG8(0x00802CF4u + 0x1A1u) = 1u;
    REG8(0x00802CF4u + 0x1A2u) = 2u;

    /* Local LMP features (BR/EDR + SSP + eSCO). Role switch (b5), hold (b6) and pause encryption (b42) are cleared
     * to match BT_CTRL_OPCODE_TABLE.md §3.6: when we page a phone as master (bt_xrec_poke_bonded), advertising b5+b42
     * caused Android to send LMP_pause_encryption_req for an encrypted role switch, wedging the ROM in LC 0x35 until
     * the 30 s LMP response timeout (silicon 2026-11-26). */
    REG32(0x00802C04u + 0x00u) = 0xFACDFE1Fu;
    REG32(0x00802C04u + 0x04u) = 0x877BF9DBu;
    REG32(0x00802C0Cu) = 0u;
    REG32(0x00802C10u) = 0u;

    rom_ld_sco_audio_init();
    rom_ld_afh_init();

    RWBT_REG(0x000u) = (RWBT_REG(0x000u) & ~0x01407000u) | 0x00200007u;
    RWBT_REG(0x1E4u) = 0x00001590u;
    RWBT_REG(0x00Cu) = 0xFFFF0003u;
    RWBT_REG(0x1B0u) = 0x0000801Eu;
    REG8(0x40020118u) |= 0x08u;
    RWBT_REG(0x000u) |= 0x00000100u; /* RWBTEN */
}

void bt_rom_bringup_lc_layer(void)
{
    uint32_t *lc_hooks = (uint32_t *)0x008021BCu;
    lc_hooks[0]  = (uint32_t)(uintptr_t)hook_21bc_acl_con_cmp; /* 0x008021BC */
    lc_hooks[1]  = 0x000290C3u;
    lc_hooks[2]  = 0x0002154Du;
    lc_hooks[3]  = 0x0002D7C1u;
    lc_hooks[4]  = 0x0002AA6Du;
    lc_hooks[5]  = 0x0002B701u;
    lc_hooks[6]  = 0x00029891u;
    lc_hooks[7]  = 0x000336E9u;
    lc_hooks[12] = 0x0003381Du; /* 0x008021EC */
    lc_hooks[13] = 0x000338D9u; /* 0x008021F0 */

    rom_ke_task_create(TASK_LC, (const void *)0x008005F8u);
    for (uint32_t i = 0u; i < 7u; i++) {
        rom_ke_state_set((uint16_t)((i << 8) | TASK_LC), 0u);
        bt_memzero((void *)(0x00801384u + i * 0x208u), 0x208u);
    }

    rom_lc_pca_init(1u, 2u);
    rom_lc_sco_init();
}

void bt_rom_bringup_lm_layer(void)
{
    bt_memzero((void *)0x008038F4u, 0x220u);
    bt_memzero((void *)0x008032F0u, 0x54u);

    REG32(0x00803BB8u) = 0x00038BCDu;
    REG32(0x00803344u) = 0x00031979u;
    REG32(0x00803BACu) = 0x00037C05u;
    REG32(0x00803BA0u) = 0x000374DDu;
    REG32(0x00803BA4u) = 0x000375B9u;

    rom_rwbt_env_flags_clear();

    REG8(0x008038F4u  + 0x218u) = 1u;
    REG8(0x008038F4u  + 0x21Au) = 1u;
    REG16(0x008032F0u + 0x006u) = 0x1FA0u;
    REG16(0x008032F0u + 0x050u) = 5u;

    rom_lm_ssp_init((uint8_t *)0x008032FAu);
    REG8(0x008032F0u + 0x002u) = 7u;
    REG8(0x008032F0u + 0x003u) = 0x10u;

    rom_lm_nb_sync_init();
    rom_lm_acl_env_init();
    rom_lm_con_info_init();
    rom_lm_sec_p192_init();

    REG16(0x008038F4u + 0x00Au) = 0x0080u;
    REG16(0x008038F4u + 0x012u) = 0x0080u;
    REG16(0x008038F4u + 0x00Cu) = 0x0400u;
    REG16(0x008038F4u + 0x014u) = 0x0400u;
    REG32(0x008038F4u + 0x004u) = rom_lm_slot_to_time(0x0400u);
    REG32(0x008038F4u + 0x000u) = rom_lm_slot_to_time(0x0400u);
    REG16(0x008038F4u + 0x20Eu) = 0x8B33u; /* GIAC LAP */
    REG8(0x008038F4u  + 0x210u) = 0x9Eu;

    rom_ke_task_create(TASK_LM, (const void *)0x00800660u);
    rom_ke_state_set(TASK_LM, 0u);
}
