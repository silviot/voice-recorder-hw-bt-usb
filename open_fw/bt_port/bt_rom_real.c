/* bt_rom_real: the real bt_rom_if_t over the SL6801 RW-BT mask-ROM controller (plan M1; interface map
 * docs/BT_ROM_INTERFACE.md §2-§10). Ported from the legacy glue that runs on silicon today; every block cites the
 * legacy lines it mirrors (F: = src/bt_hci.c, R: = src/bt_rf.c, M: = src/main.c, line numbers of this tree).
 *
 * Reused, not re-derived: RF calibration (R:632 bt_rf_init_calibrated, called with the legacy arguments of M:1275)
 * and the LD / LC / LM bring-up (F:1109 rwbt_init_ld_layer, F:1289 rwbt_init_lc_layer, F:1313 rwbt_init_lm_layer,
 * exported by wrappers at the end of bt_hci.c only in the probe build) run the exact legacy code. g_bt_heap is the
 * legacy 8 KiB heap (bt_hci.c), shared because the two paths never own the controller at the same time: main.c refuses
 * legacy BT mode while bt_rom_real_busy() (from the first rf_calibrate()/start() until a stop() that completed).
 *
 * Own (bt_ctrl needs different behaviour, BT_ROM_INTERFACE §10 notes):
 *   - kernel hooks (F:1048-1079): own nested PRIMASK lock with a readable depth (plan §3 rule (e)), malloc/free with
 *     used-bytes / high-water accounting and a fault upcall instead of a silent NULL (BT_ROM_INTERFACE §8.2), no ev5
 *     (bt_ctrl copies out of TASK_HCI directly, no H4 transport engine);
 *   - HCI layer (F:1081-1107): TASK_HCI's default handler calls bt_rom_upcalls_t.on_hci_msg;
 *   - IRQ 26 top half (F:241-251): .fast_ramtext, per-bit counters, coalescing and latency stamps, on_irq upcall;
 *   - IPR priority: set to configMAX_SYSCALL_INTERRUPT_PRIORITY and read back (plan §3 rule (d); legacy never sets it,
 *     so the value found at the first start() is put back by stop(): the legacy path only ever runs with it);
 *   - stop() reports a soft-reset timeout (legacy F:2158 polls without an error path).
 *
 * Context: every entry point runs in bt_ctrl's task (single ROM caller, BT_ROM_INTERFACE §8.3) except the IRQ
 * handler and the read-only diagnostics.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "bt_rom_real.h"
#include "bt_rom_rng.h"

/* ---- legacy symbols reused (bt_hci.c exports them, from its end, only with OPENFW_HCI_PROBE=1; bt_rf.c always) ---- */
extern uint32_t *const bt_hci_shared_heap;                    /* = g_bt_heap, F:239, BT_HEAP_WORDS (F:233) words */
void     bt_hci_shared_init_ld_layer(const uint8_t bd_addr[6]);   /* F:1109-1287 rwbt_init_ld_layer */
void     bt_hci_shared_init_lc_layer(void);                   /* F:1289-1311 rwbt_init_lc_layer */
void     bt_hci_shared_init_lm_layer(void);                   /* F:1313-1351 rwbt_init_lm_layer */
void     bt_hci_legacy_irq_top(void);                         /* F:241-251 (legacy RWBT_IRQHandler, renamed) */
#define  g_bt_heap  bt_hci_shared_heap
uint32_t bt_rf_init_calibrated(uint32_t freq_drift, uint32_t clk6_hz);   /* R:632 */
uint64_t us_timer_now(void);

#define BT_HEAP_WORDS       2048u                             /* F:233 */
#define BT_HEAP_BYTES       (BT_HEAP_WORDS * 4u)
_Static_assert(BT_HEAP_BYTES == BT_ROM_REAL_HEAP_BYTES, "bt_rom_real_msg_fits() uses the legacy heap size");

#define TASK_HCI            0x3Cu                             /* F:19 */
#define KE_MSG_DEFAULT_HANDLER 0xFFFFu                        /* F:20 */
#define KE_MSG_CONSUMED     0                                 /* F:22 */
#define KE_MSG_NO_FREE      1                                 /* F:23 */
#define EM_BASE_ADDR        0x40030000u                       /* F:29 */
#define EM_SIZE_BYTES       0x8000u                           /* F:30 */
#define KE_EVT_FIELD        0x008041E4u                       /* F:395 */
#define KE_EVT_CBS          0x008041E8u                       /* F:396 */
#define SOFT_RESET_POLLS    200000u                           /* F:1364, F:2158 */
#define SCB_SHPR3_ADDR      0xE000ED20u

/* IRQ 26 priority (plan §3: logical 4..7 = IPR 0x80..0xE0 with 3 bits, numerically >= MAX_SYSCALL). The most urgent
 * value FreeRTOS allows, so the top half preempts every task and the kernel's own exceptions. */
#define BT_ROM_IRQ_PRIO     ((uint8_t)configMAX_SYSCALL_INTERRUPT_PRIORITY)
_Static_assert(configMAX_SYSCALL_INTERRUPT_PRIORITY >= 0x20u && configMAX_SYSCALL_INTERRUPT_PRIORITY <= 0xE0u,
               "IRQ 26 must be a FreeRTOS-aware interrupt (FromISR calls)");

/* ---- ROM structures (F:32-111, verbatim layouts) ---- */
typedef void (*rwip_eif_cb_t)(uint8_t status);
struct rwip_eif_api {
    void (*read)(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb);
    void (*write)(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb);
    void (*flow_on)(void);
    uint8_t (*flow_off)(void);
};
struct co_list_hdr { struct co_list_hdr *next; };
struct co_list { struct co_list_hdr *first; struct co_list_hdr *last; };
struct ke_msg {
    struct co_list_hdr hdr;
    uint8_t  hci_type;
    int8_t   hci_hdr_offset;
    uint16_t hci_len;
    uint16_t id;
    uint16_t dest_id;
    uint16_t src_id;
    uint16_t param_len;
    uint8_t  param[];
};
struct hci_tl_env_tag {
    const struct rwip_eif_api *ext_if;
    uint8_t  tx_curr_type;
    uint8_t  rx_state;
    uint8_t  tx_state;
    uint8_t  nb_h2c_cmd_pkts;
    uint8_t  _pad0[0x68];
    struct co_list evt_queue;
    struct co_list acl_queue;
    uint8_t *acl_tx_frag_ptr;
    uint16_t acl_tx_rem_len;
    uint16_t acl_tx_hdr;
    uint16_t host_nb_acl_pkts;
    uint16_t host_unacked_pkts[7];
    uint16_t host_acl_data_pkt_len;
    uint8_t  _pad1[0x3D];
    uint8_t  host_flow_ctrl_en;
    uint32_t _pad2;
    uint8_t  evt_msk[8];
    uint32_t _pad3;
};
struct ke_msg_handler {
    uint16_t id;
    uint16_t pad;
    int (*func)(uint16_t msgid, void const *param, uint16_t dest_id, uint16_t src_id);
};
struct ke_state_handler {
    const struct ke_msg_handler *msg_table;
    uint16_t msg_cnt;
    uint16_t pad;
};
struct ke_task_desc {
    const struct ke_state_handler *state_handler;
    const struct ke_state_handler *default_handler;
    uint8_t *state;
    uint16_t state_max;
    uint16_t idx_max;
};
_Static_assert(offsetof(struct ke_msg, param) == 0x10u, "ke_msg header is 16 bytes (BT_ROM_INTERFACE §3.1)");
_Static_assert(offsetof(struct hci_tl_env_tag, evt_msk) == 0xDCu, "hci_tl_env.evt_msk at 0x00803D04 (F:484)");
#define HCI_TL_ENV   ((struct hci_tl_env_tag *)0x00803C28u)   /* F:113-115 */
#define HCI_EVT_Q    ((void *)(0x00803C28u + 0x70u))
#define HCI_ACL_Q    ((void *)(0x00803C28u + 0x78u))

typedef struct {
    uint32_t size_flags;   /* F:234-237: aligned block size incl. 8 B header; bit 0 = used */
    uint32_t tag;
} bt_blk_hdr_t;
_Static_assert(sizeof(bt_blk_hdr_t) == BT_ROM_REAL_BLK_HDR, "bt_rom_real_msg_fits() uses the legacy block header");

/* ---- state ---- */
static bt_rom_upcalls_t   s_up;            /* copied at start(): a later incarnation's ctx never reaches this one */
static volatile uint8_t   s_owner;
/* 1 from the first rf_calibrate()/start() until a stop() whose soft reset completed. A bt_ctrl stuck (and demoted)
 * inside a ROM call, a hard-stopped one, a failed start() and a stop() timeout all leave it set: RW-BT state is then
 * unknown and the legacy path must stay off for the rest of the boot (bt_rom_real_busy()). */
static volatile uint8_t   s_claimed;
static uint8_t            s_ipr_orig;      /* IPR[26] found at the first start(): the only value legacy has run with */
static uint8_t            s_ipr_saved;
static volatile uint32_t  s_irq_pending;
static volatile uint32_t  s_irq_t_latch;   /* US_TIMER_LO at the first latch since the last irq_take() */
static volatile uint32_t  s_irq_count[32];
static volatile uint32_t  s_irq_unhandled;
static volatile uint32_t  s_nest;          /* F:229 g_irq_nest */
static volatile uint32_t  s_saved_primask; /* F:230 */
static uint32_t           s_prng = 0x6D2B79F5u;   /* F:231 */
static bt_rom_real_diag_t s_d;

static inline uint32_t now32(void)
{
    return US_TIMER_LO;
}

/* ---- memory helpers (F:122-159 bt_memzero, F:185-207 bt_memcpy_to_em): volatile stores for EM ---- */
static void em_zero(uint32_t addr, uint32_t n)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)addr;
    for (uint32_t i = 0u; i < n / 4u; i++) {
        d[i] = 0u;
    }
}

static void zero_ram(void *dst, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    while (n-- > 0u) {
        *d++ = 0u;
    }
}

/* ---- nested PRIMASK lock (F:253-272), hooked at 0x00803C1C/20 ---- */
BT_FAST static void rr_lock(void)
{
    uint32_t primask;
    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    if (s_nest == 0u) {
        s_saved_primask = primask;
    }
    s_nest++;
}

BT_FAST static void rr_unlock(void)
{
    if (s_nest > 0u) {
        s_nest--;
        if (s_nest == 0u) {
            uint32_t pm = s_saved_primask;
            __asm__ volatile ("msr primask, %0" :: "r"(pm) : "memory");
        }
    }
}

void bt_rom_real_force_unlock(void)
{
    if (s_nest > 0u) {
        s_nest = 0u;
        uint32_t pm = s_saved_primask;
        __asm__ volatile ("msr primask, %0" :: "r"(pm) : "memory");
    }
}

static inline uint32_t raw_irq_save(void)
{
    uint32_t pm;
    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r"(pm) :: "memory");
    return pm;
}

static inline void raw_irq_restore(uint32_t pm)
{
    __asm__ volatile ("msr primask, %0" :: "r"(pm) : "memory");
}

/* ---- PRNG hook 0x00803C24 (BT_ROM_INTERFACE §2.3, bt_rom_rng.h) ----
 * rr_rand80 is the legacy generator (F:293-309 verbatim): it still serves the radio-timing call sites and any
 * untraced one, so they keep the exact 0..79 behaviour tested on silicon. rr_rand_hook is what the slot holds: it
 * routes the SSP nonce, LMP random and passkey sites to DRBG bytes the app pre-filled (bt_rom_rng_feed). */
BT_FAST static uint32_t rr_rand80(void)
{
    uint32_t x = s_prng ^ (uint32_t)us_timer_now();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0u) {
        x = 0x13579BDFu;
    }
    s_prng = x;
    return x % 80u;
}

BT_FAST __attribute__((noinline)) static uint32_t rr_rand_hook(void)
{
    /* Entered by `bx r3` from the thunk at ROM 0x3DE98, which every caller reaches with `bl`: LR is the ROM call
     * site + 4 (test_bt_rom_rng re-derives the 11 sites from the ROM image). */
    uint32_t lr = (uint32_t)(uintptr_t)__builtin_return_address(0);
    return bt_rom_rng_hook(lr, (volatile uint8_t *)(uintptr_t)BT_ROM_NONCE_BLOCK_ADDR, rr_rand80);
}

/* ---- local P-192 key pair (BT_ROM_INTERFACE §2.3): rom_lm_sec_p192_init (0x31DE4, run by the LM bring-up) copies
 * the ROM's FIXED pair into RAM; nothing in the ROM writes these slots again. The private key is read from RAM at
 * the start of every DHKey computation (0x32574), the public key whenever it is sent or hashed (0x319B8), both only
 * while debug mode (byte 0x00803348, zeroed by the same init; HCI 0x1804 is not forwarded by bt_ctrl) is off. ---- */
#define P192_PRIV_RAM   0x00803798u    /* 0x00803348 + 0x450, 24 B little-endian */
#define P192_PUB_RAM    0x0080334Cu    /* 0x00803348 + 0x4, X || Y, 48 B little-endian */
#define P192_PRIV_ROM   0x0000D7ECu    /* 0xD72C + 0xC0: the fixed private key */
#define P192_PUB_ROM    0x0000D84Cu    /* 0xD72C + 0x120: its public key */
static uint8_t           s_p192[72];   /* staged by the app: priv (24) || pub (48) */
static volatile uint8_t  s_p192_staged;
static bt_rom_real_sec_t s_sec;

static int mem_eq(uint32_t a, uint32_t b, uint32_t n)
{
    uint8_t diff = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        diff |= (uint8_t)(*(volatile const uint8_t *)(uintptr_t)(a + i) ^ *(volatile const uint8_t *)(uintptr_t)(b + i));
    }
    return diff == 0u;
}

static void mem_put(uint32_t dst, const uint8_t *src, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)dst;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = src[i];
    }
}

/* After the LM bring-up, before any ROM event runs (start(), bt_ctrl task). */
static void rr_install_p192(void)
{
    s_sec.key_installed = 0u;
    if (!s_p192_staged) {
        return;
    }
    /* The slots must hold exactly what the trace says the init copied; anything else means another ROM revision or a
     * misread, and the key is not touched (pairing stays refused: bt_rom_real_sec().key_installed == 0). */
    if (!mem_eq(P192_PRIV_RAM, P192_PRIV_ROM, 24u) || !mem_eq(P192_PUB_RAM, P192_PUB_ROM, 48u)) {
        s_sec.key_unexpected++;
        return;
    }
    mem_put(P192_PRIV_RAM, s_p192, 24u);
    mem_put(P192_PUB_RAM, &s_p192[24], 48u);
    if (mem_eq(P192_PRIV_RAM, (uint32_t)(uintptr_t)s_p192, 24u) &&
        mem_eq(P192_PUB_RAM, (uint32_t)(uintptr_t)&s_p192[24], 48u)) {
        s_sec.key_installed = 1u;
        s_sec.key_installs++;
    }
}

static void raise_fault_locked_ok(uint32_t code)
{
    if (s_up.on_fault != NULL) {
        s_up.on_fault(s_up.ctx, code);     /* flags only; may run with the ROM lock held */
    }
}

/* ---- g_bt_heap first-fit allocator (F:311-388 verbatim) + accounting ---- */
static void rr_heap_init(void)
{
    bt_blk_hdr_t *blk = (bt_blk_hdr_t *)g_bt_heap;           /* F:311-316 */
    blk->size_flags = BT_HEAP_BYTES;
    blk->tag = 0u;
    s_d.heap_bytes = BT_HEAP_BYTES;
    s_d.heap_used = 0u;
}

BT_FAST static void *rr_malloc(uint32_t size, uint8_t type)
{
    if (size > BT_HEAP_BYTES - (uint32_t)sizeof(bt_blk_hdr_t) - 7u) {
        s_d.alloc_fail++;
        raise_fault_locked_ok(BT_ROM_REAL_FAULT_HEAP);
        return 0;
    }
    uint32_t aligned = ((size + 7u) & ~7u) + (uint32_t)sizeof(bt_blk_hdr_t);
    if (aligned < 16u) {
        aligned = 16u;
    }
    rr_lock();
    uint8_t *base = (uint8_t *)g_bt_heap;
    uint8_t *end  = base + BT_HEAP_BYTES;
    uint8_t *cur  = base;
    while (cur + sizeof(bt_blk_hdr_t) <= end) {
        bt_blk_hdr_t *blk = (bt_blk_hdr_t *)cur;
        uint32_t bsz = blk->size_flags & ~7u;
        if (bsz < sizeof(bt_blk_hdr_t) || cur + bsz > end) {
            break;
        }
        if ((blk->size_flags & 1u) == 0u) {
            while (cur + bsz + sizeof(bt_blk_hdr_t) <= end) {  /* forward coalescing */
                bt_blk_hdr_t *nxt = (bt_blk_hdr_t *)(cur + bsz);
                uint32_t nsz = nxt->size_flags & ~7u;
                if ((nxt->size_flags & 1u) != 0u || nsz < sizeof(bt_blk_hdr_t)) {
                    break;
                }
                bsz += nsz;
                blk->size_flags = bsz;
            }
            if (bsz >= aligned) {
                uint32_t rem = bsz - aligned;
                uint32_t taken = bsz;
                if (rem >= 16u) {
                    bt_blk_hdr_t *split = (bt_blk_hdr_t *)(cur + aligned);
                    split->size_flags = rem;
                    split->tag = 0u;
                    taken = aligned;
                }
                blk->size_flags = taken | 1u;
                blk->tag = type;
                s_d.heap_used += taken;
                if (s_d.heap_used > s_d.heap_hw) {
                    s_d.heap_hw = s_d.heap_used;
                }
                rr_unlock();
                return cur + sizeof(bt_blk_hdr_t);
            }
        }
        cur += bsz;
    }
    s_d.alloc_fail++;
    raise_fault_locked_ok(BT_ROM_REAL_FAULT_HEAP);
    rr_unlock();
    return 0;
}

BT_FAST static void rr_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    uint8_t *p = (uint8_t *)ptr;
    uint8_t *base = (uint8_t *)g_bt_heap;
    uint8_t *end = base + BT_HEAP_BYTES;
    if (p < base + sizeof(bt_blk_hdr_t) || p >= end) {
        s_d.bad_free++;
        raise_fault_locked_ok(BT_ROM_REAL_FAULT_BAD_FREE);
        return;
    }
    rr_lock();
    bt_blk_hdr_t *blk = (bt_blk_hdr_t *)(p - sizeof(bt_blk_hdr_t));
    if ((blk->size_flags & 1u) == 0u) {
        s_d.bad_free++;                                       /* double free: legacy clears the bit silently */
        raise_fault_locked_ok(BT_ROM_REAL_FAULT_BAD_FREE);
    } else {
        uint32_t sz = blk->size_flags & ~7u;
        s_d.heap_used = (sz <= s_d.heap_used) ? s_d.heap_used - sz : 0u;
        blk->size_flags &= ~1u;                               /* F:386 */
    }
    rr_unlock();
}

BT_FAST static uint32_t heap_largest_free(void)
{
    uint32_t best = 0u;
    uint32_t pm = raw_irq_save();                             /* allocator state changes only under PRIMASK */
    uint8_t *base = (uint8_t *)g_bt_heap;
    uint8_t *end = base + BT_HEAP_BYTES;
    uint8_t *cur = base;
    uint32_t run = 0u;
    while (cur + sizeof(bt_blk_hdr_t) <= end) {
        const bt_blk_hdr_t *blk = (const bt_blk_hdr_t *)cur;
        uint32_t bsz = blk->size_flags & ~7u;
        if (bsz < sizeof(bt_blk_hdr_t) || cur + bsz > end) {
            break;
        }
        if ((blk->size_flags & 1u) == 0u) {
            run += bsz;                                       /* adjacent free blocks coalesce on the next malloc */
            if (run > best) {
                best = run;
            }
        } else {
            run = 0u;
        }
        cur += bsz;
    }
    raw_irq_restore(pm);
    return best > sizeof(bt_blk_hdr_t) ? best - (uint32_t)sizeof(bt_blk_hdr_t) : 0u;
}

/* ---- ke_event scheduler hooks (F:393-425) ---- */
BT_FAST static uint32_t rr_schedule(uint32_t max_steps)
{
    volatile uint32_t *ev_field = (volatile uint32_t *)KE_EVT_FIELD;
    void (*const *cbs)(void) = (void (*const *)(void))KE_EVT_CBS;
    for (uint32_t iter = 0u; iter < max_steps; iter++) {
        uint32_t mask = *ev_field & 0x3Fu;
        if (mask == 0u) {
            break;
        }
        for (int b = 5; b >= 0; b--) {                        /* highest event bit first (F:403) */
            if (mask & (1u << b)) {
                uint32_t t0 = now32();
                if (cbs[b]) {
                    cbs[b]();
                } else {
                    rr_lock();
                    *ev_field &= ~(1u << b);
                    rr_unlock();
                }
                uint32_t dt = now32() - t0;
                s_d.disp_count++;
                s_d.disp_sum_us += dt;
                if (dt > s_d.disp_max_us) {
                    s_d.disp_max_us = dt;
                    s_d.disp_max_ev = (uint32_t)b;
                }
                break;
            }
        }
    }
    return *ev_field & 0x3Fu;
}

/* Installed at 0x00804200 like the legacy scheduler (F:1073), in case a ROM path calls ke_event_schedule itself. */
BT_FAST static void rr_ke_event_schedule(void)
{
    (void)rr_schedule(64u);
}

BT_FAST static void rr_ke_event_set(uint8_t ev)                       /* F:418-425 */
{
    if (ev <= 5u) {
        rr_lock();
        *(volatile uint32_t *)KE_EVT_FIELD |= (1u << ev);
        rr_unlock();
    }
}

/* ---- HCI layer: TASK_HCI + the hci_tl_env the ROM expects at 0x00803C28 (F:1081-1107) ---- */
BT_FAST static uint32_t rr_hook_ret0(void) { return 0u; }             /* F:428 */

static void rr_hook_3bbc_7_set_evt_mask(const uint8_t *mask)  /* F:481-488 verbatim */
{
    static const uint8_t mandatory[8] = BT_ROM_REAL_EVT_MASK_MANDATORY;
    volatile uint8_t *dst = (volatile uint8_t *)0x00803D04u;
    for (int i = 0; i < 8; i++) {
        dst[i] = (uint8_t)(mask[i] | mandatory[i]);
    }
}

/* ext_if: the ROM is not expected to call it (no HCI transport in ROM, BT_ROM_INTERFACE §1); kept like F:1032-1046
 * so a stray call completes instead of jumping through NULL, and counted. */
static void rr_eif_read(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb) { (void)buf; (void)len; (void)cb; }
static void rr_eif_write(uint8_t *buf, uint32_t len, rwip_eif_cb_t cb)
{
    (void)buf;
    (void)len;
    s_d.eif_writes++;
    if (cb) {
        cb(0u);
    }
}
static void rr_eif_flow_on(void) {}
static uint8_t rr_eif_flow_off(void) { return 1u; }
static const struct rwip_eif_api k_eif_api = { rr_eif_read, rr_eif_write, rr_eif_flow_on, rr_eif_flow_off };

/* TASK_HCI default handler: every ROM reply/event (sent to src_id = TASK_HCI, BT_ROM_INTERFACE §3.2) goes to bt_ctrl.
 * Runs inside run() with the ROM lock released (§3.4). */
static int rr_hci_default_handler(uint16_t msgid, void const *param, uint16_t dest_id, uint16_t src_id)
{
    (void)dest_id;
    uint8_t *p = (uint8_t *)(uintptr_t)param;
    const struct ke_msg *m = (const struct ke_msg *)(p - 0x10);
    if (s_up.on_hci_msg == NULL) {
        return KE_MSG_CONSUMED;
    }
    bt_rom_msg_ret_t r = s_up.on_hci_msg(s_up.ctx, msgid, p, m->param_len, src_id);
    return (r == BT_ROM_MSG_KEEP) ? KE_MSG_NO_FREE : KE_MSG_CONSUMED;
}

static uint8_t s_hci_state[1];                                 /* F:865-874 */
static const struct ke_msg_handler k_hci_default_tbl[] = {
    { KE_MSG_DEFAULT_HANDLER, 0u, rr_hci_default_handler },
};
static const struct ke_state_handler k_hci_default_state = { k_hci_default_tbl, 1u, 0u };
static const struct ke_task_desc k_task_desc_hci = { 0, &k_hci_default_state, s_hci_state, 1u, 1u };

static void rr_install_kernel_hooks(void)                     /* F:1048-1079 */
{
    REG32(0x00803C1Cu) = (uint32_t)(uintptr_t)rr_lock;
    REG32(0x00803C20u) = (uint32_t)(uintptr_t)rr_unlock;
    REG32(0x00803C24u) = (uint32_t)(uintptr_t)rr_rand_hook;
    REG32(0x008041CCu) = (uint32_t)(uintptr_t)rr_malloc;
    REG32(0x008041D0u) = (uint32_t)(uintptr_t)rr_free;

    REG32(0x008041D4u) = 0x00030B59u; /* ke_timer_hw_set */
    REG32(0x008041D8u) = 0x00030BF9u; /* ke_timer_set */
    REG32(0x008041DCu) = 0x00030CA9u; /* ke_timer_clear */
    REG32(0x008041E0u) = 0x00030D19u; /* ke_timer_active */

    REG32(0x008041A8u) = 0x00030A01u; /* ke_msg_alloc */
    REG32(0x008041ACu) = 0x00030A3Du; /* ke_msg_send */
    REG32(0x008041B0u) = 0x00030A7Du; /* ke_msg_forward */

    REG32(0x008040D0u) = 0x000304BDu; /* EM buf pool alloc */
    REG32(0x008040D4u) = 0x000304E9u; /* EM buf pool free */
    REG32(0x00804124u) = 0x000305D9u; /* EM RX buf alloc */

    zero_ram((void *)0x008041B4u, 24u);                       /* message / saved / timer queues */
    zero_ram((void *)0x0080415Cu, 0x4Cu);                     /* task table */

    rom_ke_event_init();
    REG32(0x00804200u) = (uint32_t)(uintptr_t)rr_ke_event_schedule;
    REG32(0x00804204u) = (uint32_t)(uintptr_t)rr_ke_event_set;

    rom_ke_event_callback_set(0u, (void (*)(void))(uintptr_t)0x000307D5u); /* ke_task_schedule */
    rom_ke_event_callback_set(1u, (void (*)(void))(uintptr_t)0x00030B8Du); /* ke_timer_schedule */
    /* ev5 (legacy H4 TX completion, F:1078) is not used: bt_ctrl copies in the TASK_HCI handler. */
}

static void rr_init_hci_layer(void)                           /* F:1081-1107 */
{
    uint32_t *hci_hooks = (uint32_t *)0x00803BBCu;
    for (uint32_t i = 0u; i < 7u; i++) {
        hci_hooks[i] = (uint32_t)(uintptr_t)rr_hook_ret0;
    }
    hci_hooks[7] = (uint32_t)(uintptr_t)rr_hook_3bbc_7_set_evt_mask;

    zero_ram(HCI_TL_ENV, (uint32_t)sizeof(*HCI_TL_ENV));
    HCI_TL_ENV->ext_if                = &k_eif_api;
    HCI_TL_ENV->tx_state              = 1u;
    HCI_TL_ENV->nb_h2c_cmd_pkts       = 7u;
    HCI_TL_ENV->host_acl_data_pkt_len = 0xFFFFu;

    static const uint8_t def_msk[8] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    for (uint32_t i = 0u; i < 8u; i++) {
        HCI_TL_ENV->evt_msk[i] = def_msk[i];
    }
    rr_hook_3bbc_7_set_evt_mask(def_msk);

    rom_co_list_init(HCI_EVT_Q);
    rom_co_list_init(HCI_ACL_Q);
    rom_ke_task_create(TASK_HCI, &k_task_desc_hci);           /* before LD/LC/LM (BT_ROM_INTERFACE §2.1 step 7) */
    rom_ke_state_set(TASK_HCI, 0u);
}

/* ---- IRQ 26 top half (F:241-251 + counters). The single RWBT vector of the probe image. ---- */
__attribute__((section(".fast_ramtext"), used))
void RWBT_IRQHandler(void)
{
    if (!s_owner) {
        bt_hci_legacy_irq_top();              /* legacy path owns RW-BT: unchanged behaviour */
        return;
    }
    uint32_t stat = RWBT_INTSTAT;
    if (stat == 0u) {
        s_d.irq_spurious++;
        return;
    }
    RWBT_INTACK = stat;                       /* ack before service, as legacy (BT_ROM_INTERFACE §7) */
    uint32_t prev = s_irq_pending;
    if (prev == 0u) {
        s_irq_t_latch = now32();
    }
    s_irq_pending = prev | stat;
    s_d.irq_total++;
    uint32_t again = stat & prev;
    while (again != 0u) {
        uint32_t b = (uint32_t)__builtin_ctz(again);
        s_d.irq_coalesced[b]++;
        again &= again - 1u;
    }
    while (stat != 0u) {
        uint32_t b = (uint32_t)__builtin_ctz(stat);
        s_irq_count[b]++;
        stat &= stat - 1u;
    }
    if (s_up.on_irq != NULL) {
        s_up.on_irq(s_up.ctx);                /* bt_ctrl -> vTaskNotifyGiveFromISR + portYIELD_FROM_ISR */
    }
}

/* ---- bt_rom_if_t entry points ---- */
static uint32_t rr_rf_calibrate(void)
{
    s_claimed = 1u;                                           /* RF / modem state is about to change */
    uint32_t t0 = now32();
    uint32_t cal = bt_rf_init_calibrated(0x53u, 32000000u);  /* M:1275 legacy arguments; before the EM wipe */
    s_d.rf_cal_us = now32() - t0;
    return cal;
}

static int rr_start(const uint8_t bd_addr[6], const bt_rom_upcalls_t *up)   /* F:1353-1379 rwbt_controller_init */
{
    uint32_t t0 = now32();
    s_d.starts++;
    s_claimed = 1u;
    nvic_disable_irq(IRQ_BT);                                 /* F:1355-1359 */
    nvic_clear_pending(IRQ_BT);
    s_owner = 0u;
    if (!s_ipr_saved) {
        s_ipr_orig = nvic_get_priority(IRQ_BT);
        s_ipr_saved = 1u;
    }
    s_d.ipr_orig = s_ipr_orig;
    s_up = *up;
    s_irq_pending = 0u;
    s_nest = 0u;

    /* Implemented priority bits (Exp 1): IRQ 26 is masked, so a temporary 0xFF cannot change any behaviour. */
    nvic_set_priority(IRQ_BT, 0xFFu);
    s_d.ipr_impl_mask = nvic_get_priority(IRQ_BT);
    nvic_set_priority(IRQ_BT, BT_ROM_IRQ_PRIO);
    s_d.ipr_set = BT_ROM_IRQ_PRIO;
    s_d.ipr_readback = nvic_get_priority(IRQ_BT);
    s_d.shpr3 = REG32(SCB_SHPR3_ADDR);
    if (s_d.ipr_readback < (uint8_t)configMAX_SYSCALL_INTERRUPT_PRIORITY) {
        nvic_set_priority(IRQ_BT, s_ipr_orig);
        s_d.start_us = now32() - t0;
        return BT_ROM_REAL_ERR_IRQ_PRIO;                      /* FromISR calls at this priority would corrupt the kernel */
    }

    em_zero(EM_BASE_ADDR, EM_SIZE_BYTES);                     /* F:1361 */
    RWBT_CNTL |= (1u << 29);                                  /* F:1363-1365 soft reset */
    uint32_t t;
    for (t = 0u; t < SOFT_RESET_POLLS && (RWBT_CNTL & (1u << 29)); t++) {
    }
    if (RWBT_CNTL & (1u << 29)) {
        s_d.start_us = now32() - t0;
        return BT_ROM_REAL_ERR_RESET_TIMEOUT;
    }

    rr_heap_init();                                           /* F:1367 */
    rr_install_kernel_hooks();                                /* F:1368 (own hooks) */
    rr_init_hci_layer();                                      /* F:1369 (own TASK_HCI) */
    bt_hci_shared_init_ld_layer(bd_addr);                     /* F:1370 legacy: hooks, constants, EM, RWBTEN */
    bt_hci_shared_init_lc_layer();                            /* F:1371 legacy */
    bt_hci_shared_init_lm_layer();                            /* F:1372 legacy */
    rr_install_p192();                                        /* BT_ROM_INTERFACE §2.3: replace the fixed pair */

    RWBT_TIMING      = (RWBT_TIMING & 0x8000F000u) | 0x1B460846u;   /* F:1374-1375 */
    RWBT_REG(0x02Cu) = 0x000001FFu;
    nvic_clear_pending(IRQ_BT);                               /* F:1377; enabling is irq_enable()'s job */
    s_owner = 1u;
    s_d.start_us = now32() - t0;
    return 0;
}

static int rr_stop(void)                                      /* F:2151-2162 bt_xrec_stop (hardware part) */
{
    uint32_t t0 = now32();
    s_d.stops++;
    nvic_disable_irq(IRQ_BT);
    RWBT_INTCNTL = 0u;
    RWBT_INTACK  = 0xFFFFFFFFu;
    RWBT_CNTL   &= ~(1u << 8);                                /* RWBTEN off */
    RWBT_CNTL   |= (1u << 29);                                /* soft reset */
    for (uint32_t t = 0u; t < SOFT_RESET_POLLS && (RWBT_CNTL & (1u << 29)); t++) {
    }
    int rc = (RWBT_CNTL & (1u << 29)) ? -1 : 0;               /* reported (legacy has no error path) */
    if (s_sec.key_installed) {                                /* the session's private key does not outlive it */
        static const uint8_t z[24] = { 0u };
        mem_put(P192_PRIV_RAM, z, 24u);
        s_sec.key_installed = 0u;
    }
    nvic_clear_pending(IRQ_BT);
    if (s_ipr_saved) {
        nvic_set_priority(IRQ_BT, s_ipr_orig);                /* IRQ 26 is masked: legacy priority back */
    }
    uint32_t pm = raw_irq_save();
    s_irq_pending = 0u;
    raw_irq_restore(pm);
    s_owner = 0u;
    if (rc != 0) {
        s_d.stop_timeouts++;
    } else {
        s_claimed = 0u;                                       /* soft reset completed: RW-BT is back to reset state */
    }
    s_d.stop_us = now32() - t0;
    return rc;
}

static void rr_irq_enable(int on)
{
    if (on) {
        nvic_set_priority(IRQ_BT, BT_ROM_IRQ_PRIO);           /* idempotent; start() verified the read-back */
        nvic_clear_pending(IRQ_BT);                           /* F:1377-1378 */
        nvic_enable_irq(IRQ_BT);
    } else {
        nvic_disable_irq(IRQ_BT);
    }
}

BT_FAST static uint32_t rr_irq_take(void)                             /* F:274-280 fetch-and-clear under PRIMASK */
{
    uint32_t pm = raw_irq_save();
    uint32_t stat = s_irq_pending;
    uint32_t t_latch = s_irq_t_latch;
    s_irq_pending = 0u;
    raw_irq_restore(pm);
    if (stat != 0u) {
        uint32_t lat = now32() - t_latch;
        if (lat > s_d.irq_lat_max_us) {
            s_d.irq_lat_max_us = lat;
        }
        s_d.irq_lat_sum_us += lat;
        s_d.irq_lat_n++;
    }
    return stat;
}

BT_FAST static uint32_t rr_irq_service(uint32_t bits)                 /* F:282-290 */
{
    uint32_t t0 = now32();
    if (bits & (1u << 0)) {
        rom_rwbt_cscnt_isr();
    }
    if (bits & (1u << 1)) {
        rom_rwbt_rx_isr();
    }
    if (bits & (1u << 10)) {
        rom_ke_event_set(1u);
    }
    uint32_t dt = now32() - t0;
    s_d.svc_sum_us += dt;
    if (dt > s_d.svc_max_us) {
        s_d.svc_max_us = dt;
    }
    uint32_t un = bits & ~BT_ROM_IRQ_SERVICED;               /* acked and dropped, as legacy; reported */
    s_irq_unhandled |= un;
    return un;
}

BT_FAST static uint32_t rr_events(void)
{
    return REG32(KE_EVT_FIELD) & 0x3Fu;
}

BT_FAST static uint32_t rr_run(uint32_t max_steps)
{
    uint32_t t0 = now32();
    uint32_t ev = rr_schedule(max_steps);
    uint32_t dt = now32() - t0;
    if (dt > s_d.run_max_us) {
        s_d.run_max_us = dt;
    }
    return ev;
}

BT_FAST static uint8_t *rr_msg_alloc(uint16_t id, uint16_t dst, uint16_t src, uint16_t len)
{
    /* The ROM's ke_msg_alloc never returns NULL (BT_ROM_INTERFACE §3.1): refuse here, before it can run dry. Both the
     * total and the largest contiguous free run count (first fit, no back-coalescing: a fragmented heap can be under
     * budget and still fail). All allocations run in bt_ctrl's task, so nothing allocates between check and alloc. */
    if (!bt_rom_real_msg_fits(s_d.heap_used, heap_largest_free(), len)) {
        s_d.alloc_refused++;
        return NULL;
    }
    return (uint8_t *)rom_ke_msg_alloc(id, dst, src, len);    /* zero-filled param pointer */
}

BT_FAST static void rr_msg_send(uint8_t *param)
{
    rom_ke_msg_send(param);                                   /* takes the PARAM pointer (F:952) */
}

BT_FAST static void rr_msg_free(uint8_t *param)
{
    rom_ke_msg_free(param - 0x10);                            /* takes the HEADER pointer (F:944) */
}

BT_FAST static uint16_t rr_conhdl_to_task(uint16_t conhdl) { return rom_hci_conhdl_to_task(conhdl); }
BT_FAST static uint8_t  rr_conhdl_type(uint16_t conhdl)    { return rom_hci_conhdl_type(conhdl); }
BT_FAST static uint8_t *rr_em_tx_alloc(void)               { return rom_em_buf_tx_alloc(); }
BT_FAST static void     rr_em_rx_free(uint8_t *em)         { rom_em_buf_rx_free(em); }

BT_FAST static void rr_em_write(uint8_t *em, const uint8_t *src, uint16_t len)   /* F:185-207 */
{
    volatile uint8_t *d = (volatile uint8_t *)em;
    const uint8_t *s = src;
    uint32_t n = len;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
            *d++ = *s++;
            n--;
        }
        volatile uint32_t *dw = (volatile uint32_t *)d;
        while (n >= 4u) {
            uint32_t w;
            __builtin_memcpy(&w, s, 4u);
            *dw++ = w;
            s += 4;
            n -= 4u;
        }
        d = (volatile uint8_t *)dw;
    }
    while (n > 0u) {
        *d++ = *s++;
        n--;
    }
}

BT_FAST static void rr_em_read(uint8_t *dst, const uint8_t *em, uint16_t len)
{
    const volatile uint8_t *s = (const volatile uint8_t *)em;
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = s[i];
    }
}

BT_FAST static uint32_t rr_lock_depth(void)
{
    return s_nest;
}

static void rr_stats(bt_rom_stats_t *out)
{
    out->heap_free = BT_HEAP_BYTES - s_d.heap_used;
    out->heap_min_free = BT_HEAP_BYTES - s_d.heap_hw;
    out->heap_largest = heap_largest_free();
    out->alloc_fail = s_d.alloc_fail;
    for (uint32_t b = 0u; b < 32u; b++) {
        out->irq_count[b] = s_irq_count[b];
    }
    out->irq_unhandled = s_irq_unhandled;
}

const bt_rom_if_t bt_rom_real = {
    rr_rf_calibrate, rr_start, rr_stop,
    rr_irq_enable, rr_irq_take, rr_irq_service,
    rr_events, rr_run,
    rr_msg_alloc, rr_msg_send, rr_msg_free, rr_conhdl_to_task, rr_conhdl_type,
    rr_em_tx_alloc, rr_em_write, rr_em_read, rr_em_rx_free,
    rr_lock_depth, rr_stats,
};

/* docs/BT_ROM_HCI_MAP.md §11: TASK_LC state array (descriptor 0x008005F8 -> state 0x0080137C, ke_state_get ROM
 * 0x309B0), LD link state bytes (ld_env 0x0080296C + 0x110, counted by ROM 0x368FC), LC env 0x00801384 stride 0x208
 * (+0x57 read by the E458 sender ROM 0x2A31A). Byte reads only; safe while the ROM runs (a torn view is just stale). */
#define ROM_LC_STATE   0x0080137Cu
#define ROM_LD_LINK_ST 0x00802A7Cu
#define ROM_LC_ENV     0x00801384u
#define ROM_LC_STRIDE  0x208u
void bt_rom_real_links(uint8_t lc[BT_ROM_REAL_LINKS], uint8_t ld[BT_ROM_REAL_LINKS], uint8_t rep[BT_ROM_REAL_LINKS])
{
    for (uint32_t i = 0u; i < BT_ROM_REAL_LINKS; i++) {
        lc[i] = *(volatile const uint8_t *)(uintptr_t)(ROM_LC_STATE + i);
        ld[i] = *(volatile const uint8_t *)(uintptr_t)(ROM_LD_LINK_ST + i);
        rep[i] = *(volatile const uint8_t *)(uintptr_t)(ROM_LC_ENV + ROM_LC_STRIDE * i + 0x57u);
    }
}

void bt_rom_real_diag(bt_rom_real_diag_t *out)
{
    uint32_t pm = raw_irq_save();
    *out = s_d;
    out->owner = s_owner;
    out->claimed = s_claimed;
    raw_irq_restore(pm);
    out->ipr_now = nvic_get_priority(IRQ_BT);
    out->heap_bytes = BT_HEAP_BYTES;
}

void bt_rom_real_diag_reset_max(void)
{
    uint32_t pm = raw_irq_save();
    s_d.irq_lat_max_us = 0u;
    s_d.irq_lat_sum_us = 0u;
    s_d.irq_lat_n = 0u;
    s_d.svc_max_us = 0u;
    s_d.disp_max_us = 0u;
    s_d.disp_max_ev = 0u;
    s_d.run_max_us = 0u;
    raw_irq_restore(pm);
}

uint32_t bt_rom_real_silence(void)                          /* see bt_rom_real.h; F:2151-2158 minus the ROM state */
{
    nvic_disable_irq(IRQ_BT);
    RWBT_INTCNTL = 0u;
    RWBT_INTACK  = 0xFFFFFFFFu;
    RWBT_CNTL   &= ~(1u << 8);                                /* RWBTEN off */
    RWBT_CNTL   |= (1u << 29);                                /* soft reset */
    for (uint32_t t = 0u; t < SOFT_RESET_POLLS && (RWBT_CNTL & (1u << 29)); t++) {
    }
    nvic_clear_pending(IRQ_BT);
    s_claimed = 1u;                                           /* whatever the soft reset did: state unknown */
    return RWBT_CNTL;
}

int bt_rom_real_busy(void)
{
    return s_owner != 0u || s_claimed != 0u;
}

/* Exp 1 stage B read-back of 0C01 (BT_ROM_INTERFACE §11 Q8): hci_tl_env.evt_msk, written by the ROM's Set Event Mask
 * handler through hook 0x00803BBC[7] (rr_hook_3bbc_7_set_evt_mask: mask | mandatory). Plain RAM read, no ROM call. */
void bt_rom_real_evt_mask(uint8_t out[8])
{
    for (uint32_t i = 0u; i < 8u; i++) {
        out[i] = ((volatile uint8_t *)HCI_TL_ENV->evt_msk)[i];
    }
}

void bt_rom_real_stage_p192(const uint8_t priv_le[24], const uint8_t pub_le[48])
{
    s_p192_staged = 0u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint32_t i = 0u; i < sizeof(s_p192); i++) {
        ((volatile uint8_t *)s_p192)[i] = 0u;
    }
    if (priv_le != NULL && pub_le != NULL) {
        for (uint32_t i = 0u; i < 24u; i++) {
            s_p192[i] = priv_le[i];
        }
        for (uint32_t i = 0u; i < 48u; i++) {
            s_p192[24u + i] = pub_le[i];
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        s_p192_staged = 1u;
    }
}

void bt_rom_real_sec(bt_rom_real_sec_t *out)
{
    if (out == NULL) {
        return;
    }
    out->key_staged = s_p192_staged;
    out->key_installed = s_sec.key_installed;
    out->key_installs = s_sec.key_installs;
    out->key_unexpected = s_sec.key_unexpected;
    bt_rom_rng_stats(&out->rng);
}
