/* bt_rom_real: bt_rom_if_t over the SL6801 RW-BT mask ROM (docs/BT_ROM_INTERFACE.md §9-§10, plan M1).
 * Built only into the HCI probe image for now (make BT=1 HCI_PROBE=1, plan M5 Exp 1 stage A).
 * SPDX-License-Identifier: MIT */
#ifndef BT_ROM_REAL_H
#define BT_ROM_REAL_H

#include <stdint.h>
#include "bt_rom_if.h"

extern const bt_rom_if_t bt_rom_real;

/* on_fault codes */
#define BT_ROM_REAL_FAULT_HEAP      1u   /* g_bt_heap could not satisfy a ROM ke_malloc (the ROM does not check NULL) */
#define BT_ROM_REAL_FAULT_BAD_FREE  2u   /* ke_free of a pointer outside g_bt_heap or of a block already free */

/* start() error codes (bt_ctrl treats any nonzero value as a reset failure) */
#define BT_ROM_REAL_ERR_RESET_TIMEOUT  (-1)   /* RWBT_CNTL bit 29 (soft reset) did not clear */
#define BT_ROM_REAL_ERR_IRQ_PRIO       (-2)   /* IRQ 26 priority read back below configMAX_SYSCALL_INTERRUPT_PRIORITY */

/* Exp 1 measurements that go beyond bt_rom_stats_t. Times are us_timer microseconds. */
typedef struct {
    /* NVIC (recorded by start(), while IRQ 26 is masked) */
    uint8_t  ipr_impl_mask;      /* IPR[26] read back after writing 0xFF: implemented priority bits */
    uint8_t  ipr_set;            /* priority written */
    uint8_t  ipr_readback;       /* priority read back */
    uint8_t  owner;              /* 1 while bt_rom_real owns RW-BT (between start() and stop()) */
    uint32_t shpr3;              /* SCB SHPR3 (PendSV / SysTick priority bytes) for reference */
    uint8_t  ipr_orig;           /* IPR[26] found at the first start(); stop() writes it back (legacy never sets one) */
    uint8_t  ipr_now;            /* IPR[26] when the snapshot was taken */
    uint8_t  claimed;            /* 1 from rf_calibrate()/start() until a stop() that completed (bt_rom_real_busy) */
    uint8_t  _pad0;
    /* IRQ 26 top half / bottom half */
    uint32_t irq_total;          /* top-half entries with a nonzero INTSTAT */
    uint32_t irq_spurious;       /* top-half entries that read INTSTAT == 0 */
    uint32_t irq_coalesced[32];  /* bit latched again before the bottom half took the previous one */
    uint32_t irq_lat_max_us;     /* first latch -> irq_take() */
    uint32_t irq_lat_sum_us, irq_lat_n;
    uint32_t svc_max_us;         /* one irq_service() call */
    /* kernel */
    uint32_t disp_max_us;        /* one ke_event dispatch (ROM handler incl. TASK_HCI upcall) */
    uint32_t disp_max_ev;        /* event bit of that dispatch */
    uint32_t disp_count;
    uint32_t run_max_us;         /* one run(max_steps) call */
    /* g_bt_heap (8 KiB, shared with the legacy path, never used by both at once) */
    uint32_t heap_bytes, heap_used, heap_hw, alloc_fail, bad_free, alloc_refused;
    /* lifecycle */
    uint32_t starts, stops, stop_timeouts;
    uint32_t rf_cal_us, start_us, stop_us;   /* last call */
    uint32_t eif_writes;         /* ROM calls of the hci_tl_env ext_if (not expected: the ROM has no HCI transport) */
} bt_rom_real_diag_t;

/* g_bt_heap geometry (legacy bt_hci.c F:233-237) and the msg_alloc admission rule. */
#define BT_ROM_REAL_HEAP_BYTES    8192u   /* BT_HEAP_WORDS (2048) x 4 */
#define BT_ROM_REAL_BLK_HDR       8u      /* bt_blk_hdr_t: size_flags + tag */
#define BT_ROM_REAL_HEAP_RESERVE  1024u   /* left for the ROM's own allocations (timers, LMP), which never check NULL */

/* msg_alloc admission (BT_ROM_INTERFACE §3.1: the ROM's ke_msg_alloc never returns NULL, so a message that would make
 * ke_malloc fail must be refused before the ROM is called). heap_used: bytes in use including block headers;
 * largest_payload: largest contiguous free run minus one block header. The first-fit allocator has no back-coalescing,
 * so the total alone is not enough: the message must fit in one run that still leaves the reserve contiguous. */
static inline int bt_rom_real_msg_fits(uint32_t heap_used, uint32_t largest_payload, uint16_t len)
{
    uint32_t payload = ((uint32_t)len + 0x10u + 7u) & ~7u;    /* struct ke_msg header (16 B) + param, 8-aligned */
    uint32_t need = payload + BT_ROM_REAL_BLK_HDR;
    return heap_used + need + BT_ROM_REAL_HEAP_RESERVE <= BT_ROM_REAL_HEAP_BYTES &&
           largest_payload >= payload + BT_ROM_REAL_HEAP_RESERVE;
}

void bt_rom_real_diag(bt_rom_real_diag_t *out);
/* Reset the *_max_us fields (window measurements). */
void bt_rom_real_diag_reset_max(void);
/* 1 while bt_rom_real owns or may have left RW-BT in an unknown state: from the first rf_calibrate()/start() until a
 * stop() whose soft reset completed. The legacy path must not start while it is 1 (it may stay 1 for the whole boot
 * after a hang, a hard stop, a failed start() or a stop() timeout). */
int  bt_rom_real_busy(void);

#endif /* BT_ROM_REAL_H */
