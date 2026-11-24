/* bt_rom_if: the RW-BT mask-ROM controller as seen by bt_ctrl (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M1;
 * table from docs/BT_ROM_INTERFACE.md §10).
 *
 * Every entry point below is a "ROM call" for the purposes of plan M1's bt_stop rule (bt_ctrl checks bt_stop before
 * each one), except the two diagnostics lock_depth() and stats(), which only read open_fw RAM.
 * All entry points are called from ONE context (the bt_ctrl task), never from an ISR (BT_ROM_INTERFACE §8.3);
 * the only thing that crosses from the IRQ-26 top half is the on_irq upcall.
 *
 * Implementations:
 *   real: bt_rom_real.c (not written yet; plan step 6) = today's bt_hci.c bring-up/hooks/ISR, verbatim.
 *   fake: tests/fake_rom.c (host tests).
 *
 * Deviations from BT_ROM_INTERFACE.md §10 (all additive):
 *   - stop() returns a status (plan M1 teardown: "reset completion checked (status reported, not ignored)"); the
 *     legacy soft-reset poll (bt_hci.c bt_xrec_stop) has no error path today, the real impl must report a timeout.
 *   - bt_rom_acl_desc_t and the message / task ids bt_ctrl needs are named here.
 * SPDX-License-Identifier: MIT */
#ifndef BT_ROM_IF_H
#define BT_ROM_IF_H

#include <stddef.h>
#include <stdint.h>

/* ---- kernel task ids (BT_ROM_INTERFACE §3.2) ---- */
#define BT_ROM_TASK_LM          0x38u
#define BT_ROM_TASK_LC          0x39u   /* per-link instance: (link_id << 8) | 0x39 */
#define BT_ROM_TASK_LD          0x3Bu
#define BT_ROM_TASK_HCI         0x3Cu   /* open_fw's own task; every ROM reply is sent here */
#define BT_ROM_TASK_INVALID     0xFFu   /* conhdl_to_task() failure value (ROM 0x3076C returns 0xFF, not 0xFFFF) */
#define BT_ROM_MSG_DEST(id)     ((uint16_t)((id) >> 10))   /* 0xE0xx -> LM, 0xE4xx -> LC */

/* ---- message ids bt_ctrl handles specially ---- */
#define BT_ROM_MSG_ACL_TX       0xE426u /* HCI -> LC instance, param = bt_rom_acl_desc_t (bt_hci.c:971) */
#define BT_ROM_MSG_ACL_RX       0xE47Du /* LC -> HCI, param = bt_rom_acl_desc_t, buf_ptr in an EM RX buffer */
#define BT_ROM_MSG_NOCP         0xE462u /* LC -> HCI: u8 n, u8 pad, n x {u16 conhdl, u16 count} (bt_hci.c:817) */

/* ---- exchange memory (BT_ROM_INTERFACE §6, static reading of ROM 0x3052A; Exp 1 question 6) ---- */
#define BT_ROM_EM_TX_BUFS       9u
#define BT_ROM_EM_RX_BUFS       8u
#define BT_ROM_EM_BUF_PAYLOAD   1024u   /* 0x40C block - 12 B header/headroom */

/* ---- IRQ 26 status bits serviced by irq_service() (BT_ROM_INTERFACE §7) ---- */
#define BT_ROM_IRQ_CSCNT        (1u << 0)
#define BT_ROM_IRQ_RX           (1u << 1)
#define BT_ROM_IRQ_KE_TIMER     (1u << 10)
#define BT_ROM_IRQ_SERVICED     (BT_ROM_IRQ_CSCNT | BT_ROM_IRQ_RX | BT_ROM_IRQ_KE_TIMER)

/* struct hci_acl_data_pkt (bt_hci.c:62-69): naturally aligned ROM struct, not wire format. The handle carries no
 * PB/BC bits; pb_bc_flag holds them (PB in bits 0-1, BC in bits 2-3, i.e. wire header >> 12). */
typedef struct {
    uint16_t conhdl;
    uint8_t  pb_bc_flag;
    uint8_t  pad0;
    uint16_t length;
    uint16_t pad1;
    uint8_t *buf_ptr;
} bt_rom_acl_desc_t;
#if defined(__arm__)
_Static_assert(sizeof(bt_rom_acl_desc_t) == 12u && offsetof(bt_rom_acl_desc_t, buf_ptr) == 8u,
               "bt_rom_acl_desc_t must match the ROM's 12-byte hci_acl_data_pkt");
#endif

typedef enum { BT_ROM_MSG_CONSUMED = 0, BT_ROM_MSG_KEEP = 1 } bt_rom_msg_ret_t;

typedef struct {
    void *ctx;
    /* TASK_HCI default handler. Runs only inside run(), in bt_ctrl context, ROM lock NOT held.
     * CONSUMED: the ROM frees the ke_msg after return; `param` is dead (for BT_ROM_MSG_ACL_RX the callee must
     *           already have called em_rx_free(buf_ptr)).
     * KEEP:     bt_ctrl owns the message and must later call msg_free(param) (and em_rx_free for ACL_RX).
     * May call msg_alloc/msg_send/em_* (not run()). Must not block or call FreeRTOS. */
    bt_rom_msg_ret_t (*on_hci_msg)(void *ctx, uint16_t id, uint8_t *param, uint16_t param_len, uint16_t src_id);
    /* IRQ-26 top half, after the status was latched and acked (handler mode): real impl -> vTaskNotifyGiveFromISR. */
    void (*on_irq)(void *ctx);
    /* Fatal ROM-environment fault (g_bt_heap exhausted, unknown-task send, bad free). May be called with the ROM lock
     * held: set flags only. */
    void (*on_fault)(void *ctx, uint32_t code);
} bt_rom_upcalls_t;

typedef struct {
    uint32_t heap_free, heap_min_free, heap_largest, alloc_fail;
    uint32_t irq_count[32];          /* per-status-bit occurrences, counted in the top half */
    uint32_t irq_unhandled;          /* OR of bits acked but not serviced */
} bt_rom_stats_t;

typedef struct bt_rom_if {
    /* ---- lifecycle (bt_ctrl context only) ---- */
    uint32_t (*rf_calibrate)(void);                                   /* bt_rf_init_calibrated(0x53, 32 MHz); before start() */
    int      (*start)(const uint8_t bd_addr[6], const bt_rom_upcalls_t *up); /* §2 steps 2-11; 0 = ok; IRQ stays masked */
    int      (*stop)(void);       /* HW stop + soft reset; 0 = reset completed. Every param/EM pointer becomes invalid */

    /* ---- IRQ 26 ---- */
    void     (*irq_enable)(int on);           /* NVIC enable/disable (priority 0x80..0xE0 set by the real impl) */
    uint32_t (*irq_take)(void);               /* atomic fetch-and-clear of the latched (already acked) status bits */
    uint32_t (*irq_service)(uint32_t bits);   /* bit0 cscnt_isr, bit1 rx_isr, bit10 ke_event_set(1); returns the bits
                                                 it did not handle */
    /* ---- kernel ---- */
    uint32_t (*events)(void);                 /* ke_event field (0x008041E4 & 0x3F); 0 = idle */
    uint32_t (*run)(uint32_t max_steps);      /* <= max_steps ke_event dispatches; returns events() */

    /* ---- messages (all pointers are PARAM pointers) ---- */
    uint8_t *(*msg_alloc)(uint16_t id, uint16_t dst, uint16_t src, uint16_t len); /* zeroed; NULL only if the
                                                 implementation's own budget check refuses (the ROM never returns NULL) */
    void     (*msg_send)(uint8_t *param);     /* ownership -> ROM */
    void     (*msg_free)(uint8_t *param);     /* for KEEP'd or never-sent messages; impl passes param-0x10 to ROM */
    uint16_t (*conhdl_to_task)(uint16_t conhdl);  /* (link<<8)|0x39 or BT_ROM_TASK_INVALID */
    uint8_t  (*conhdl_type)(uint16_t conhdl);     /* 1 = ACL (0x80..0x86) */

    /* ---- exchange memory ---- */
    uint8_t *(*em_tx_alloc)(void);            /* NULL when the TX pool is empty */
    void     (*em_write)(uint8_t *em, const uint8_t *src, uint16_t len); /* volatile copy */
    void     (*em_read)(uint8_t *dst, const uint8_t *em, uint16_t len);
    void     (*em_rx_free)(uint8_t *em);      /* the buf_ptr of an ACL_RX (or an unused TX buffer) */

    /* ---- diagnostics (not ROM calls: no bt_stop check) ---- */
    uint32_t (*lock_depth)(void);             /* g_irq_nest, for assert(lock_depth()==0) before RTOS calls */
    void     (*stats)(bt_rom_stats_t *out);
} bt_rom_if_t;

#endif /* BT_ROM_IF_H */
