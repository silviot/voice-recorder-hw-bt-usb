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
    uint32_t svc_sum_us;         /* all irq_service() calls (ROM cscnt / rx ISRs): the ROM part of the bottom half */
    uint32_t disp_sum_us;        /* all ke_event dispatches (ROM kernel handlers incl. TASK_HCI upcalls) */
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

/* ROM link state per link 0..6 (read-only, ROM RAM; static analysis, docs/BT_ROM_HCI_MAP.md §11): lc = TASK_LC state
 * (0 idle, 5 connected, 0x20/0x24..0x26 detach in progress), ld = LD link state (1 = counted as an ACL link by the
 * 0C1A rule), rep = LC env +0x57 (the link was reported to the host; the E458 sender skips the event when it is 0). */
#define BT_ROM_REAL_LINKS 7u
void bt_rom_real_links(uint8_t lc[BT_ROM_REAL_LINKS], uint8_t ld[BT_ROM_REAL_LINKS], uint8_t rep[BT_ROM_REAL_LINKS]);
/* Reset the *_max_us fields (window measurements). */
void bt_rom_real_diag_reset_max(void);
/* 1 while bt_rom_real owns or may have left RW-BT in an unknown state: from the first rf_calibrate()/start() until a
 * stop() whose soft reset completed. The legacy path must not start while it is 1 (it may stay 1 for the whole boot
 * after a hang, a hard stop, a failed start() or a stop() timeout). */
int  bt_rom_real_busy(void);
/* Plan M1 hard path, radio side: the register half of stop() (legacy bt_xrec_stop F:2151-2158: IRQ 26 masked,
 * INTCNTL = 0, INTACK all, RWBTEN cleared, soft reset requested and polled with the same bound) WITHOUT any ROM call
 * and without touching IPR[26], owner or claimed: RW-BT stays claimed, so legacy BT mode stays refused until reboot.
 * For a bt_ctrl that was hard-stopped (teardown deadline) or demoted (hang / CPU guard), whose last ROM call may still be
 * unfinished in its context; call from a task that bt_ctrl cannot preempt at that point (the probe task, priority 1,
 * after bt_stop is latched or bt_ctrl was demoted to idle). Best effort: a ROM read-modify-write interrupted before
 * the call can still write RWBTEN back when it resumes. Returns RWBT_CNTL read back (bit 8 RWBTEN, bit 29 soft reset
 * still pending); RWBTEN clear is what the legacy stop relies on to end inquiry / page scan. */
uint32_t bt_rom_real_silence(void);
/* Emergency PRIMASK unwind after bt_ctrl_rtos_hard_stop() + bt_rom_real_silence() when a ROM path returns with
 * lock_depth() != 0 (otherwise ulTaskNotifyTake() in bt_ctrl_task deadlocks with PRIMASK=1). */
void bt_rom_real_force_unlock(void);
/* hci_tl_env.evt_msk (0x00803D04) as the ROM holds it now (Exp 1 stage B read-back of 0C01 Set Event Mask). */
void bt_rom_real_evt_mask(uint8_t out[8]);
/* What the legacy hook 0x00803BBC[7] stores for a Set Event Mask parameter: mask | these mandatory bits (F:481-488). */
#define BT_ROM_REAL_EVT_MASK_MANDATORY { 0x00u, 0x60u, 0x04u, 0x00u, 0xF8u, 0x07u, 0x40u, 0xE2u }

/* ---- SSP key material (plan M2.1, docs/BT_ROM_INTERFACE.md §2.3) ----
 * The ROM's local P-192 key pair is a constant copied from ROM by the LM bring-up; start() replaces it with the pair
 * staged here (after checking the RAM slots hold exactly that constant) and stop() wipes the private key again.
 * Nothing staged (HCI probe image, or no credited entropy): the ROM pair stays and key_installed is 0, which the
 * phone-mode platform treats as "pairing refused". The RNG hook at 0x00803C24 serves security call sites from the
 * bt_rom_rng pool (bt_rom_rng.h); its counters are reported here. */
#include "bt_rom_rng.h"
typedef struct {
    uint8_t  key_staged;       /* a pair is staged for the next start() */
    uint8_t  key_installed;    /* the running controller uses the staged pair (0 after stop()) */
    uint32_t key_installs;     /* successful installs since boot */
    uint32_t key_unexpected;   /* start() found something other than the ROM pair in the slots: not installed */
    bt_rom_rng_stats_t rng;
} bt_rom_real_sec_t;
/* App task, while bt_ctrl is not inside start() (before the session's bring-up job / after it ended). NULLs clear and
 * wipe the staged pair. The pair must be valid (bt_p192_keygen); it is copied. */
void bt_rom_real_stage_p192(const uint8_t priv_le[24], const uint8_t pub_le[48]);
void bt_rom_real_sec(bt_rom_real_sec_t *out);

#endif /* BT_ROM_REAL_H */
