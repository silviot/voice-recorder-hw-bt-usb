/*
 * Wait-Free 2-Slot Dekker IPC State Machine for Dual-Core UI Offload
 *
 * Allows CPU0 (single producer) to publish the latest UI snapshot in O(1)
 * without spinning when CPU1 (single consumer) is busy rasterizing a heavy
 * LVGL screen over uncached SPI NOR XIP flash.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_CPU1_IPC_H
#define OPENFW_CPU1_IPC_H

#include <stdint.h>

#ifdef OPENFW_CPU1_IPC_HOST_ATOMIC
#include <stdatomic.h>
typedef _Atomic uint32_t cpu1_ipc_u32_t;
#define CPU1_IPC_ALWAYS_INLINE static inline __attribute__((always_inline))

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_barrier(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

CPU1_IPC_ALWAYS_INLINE uint32_t cpu1_ipc_load32(const cpu1_ipc_u32_t *p)
{
    return atomic_load_explicit((cpu1_ipc_u32_t *)p, memory_order_seq_cst);
}

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_store32(cpu1_ipc_u32_t *p, uint32_t v)
{
    atomic_store_explicit(p, v, memory_order_seq_cst);
}
#else
typedef volatile uint32_t cpu1_ipc_u32_t;
#define CPU1_IPC_ALWAYS_INLINE static inline __attribute__((always_inline))

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_barrier(void)
{
#ifdef OPENFW_DSB
    OPENFW_DSB();
#else
    __asm__ volatile ("" ::: "memory");
#endif
}

CPU1_IPC_ALWAYS_INLINE uint32_t cpu1_ipc_load32(const cpu1_ipc_u32_t *p)
{
    return *p;
}

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_store32(cpu1_ipc_u32_t *p, uint32_t v)
{
    *p = v;
}
#endif /* OPENFW_CPU1_IPC_HOST_ATOMIC */

#define CPU1_IPC_SLOT_IDLE        0xFFu
/* 3.0 s watchdog for genuine CPU1 hangs (>10x worst-case XIP+BT screen load) */
#define CPU1_IPC_STALL_TIMEOUT_US 3000000ull

typedef struct {
    cpu1_ipc_u32_t req_word;       /* (seq << 1) | slot_idx; seq == 0 disarmed */
    cpu1_ipc_u32_t ack_seq;        /* last seq completed by CPU1 */
    cpu1_ipc_u32_t reading_slot;   /* 0, 1, or CPU1_IPC_SLOT_IDLE */
    uint32_t       last_seq;       /* CPU0-only: last published seq */
    uint32_t       last_slot;      /* CPU0-only: last published slot index */
    uint32_t       stall_ack_seen; /* CPU0-only: ack_seq at start of stall window */
    uint8_t        stall_armed;    /* CPU0-only: 1 while monitoring in-flight ack */
    uint64_t       stall_since_us; /* CPU0-only: timestamp when unacked seq began */
} cpu1_ipc_t;

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_init(cpu1_ipc_t *ipc)
{
    ipc->last_seq       = 0u;
    ipc->last_slot      = 0u;
    ipc->stall_ack_seen = 0u;
    ipc->stall_armed    = 0u;
    ipc->stall_since_us = 0ull;
    cpu1_ipc_store32(&ipc->req_word, 0u);
    cpu1_ipc_store32(&ipc->ack_seq, 0u);
    cpu1_ipc_store32(&ipc->reading_slot, CPU1_IPC_SLOT_IDLE);
    cpu1_ipc_barrier();
}

CPU1_IPC_ALWAYS_INLINE int cpu1_ipc_is_busy(const cpu1_ipc_t *ipc)
{
    uint32_t ack     = cpu1_ipc_load32(&ipc->ack_seq);
    uint32_t reading = cpu1_ipc_load32(&ipc->reading_slot);
    return (ack != ipc->last_seq || reading != CPU1_IPC_SLOT_IDLE) ? 1 : 0;
}

CPU1_IPC_ALWAYS_INLINE int cpu1_ipc_has_pending_req(const cpu1_ipc_t *ipc)
{
    uint32_t w   = cpu1_ipc_load32(&ipc->req_word);
    uint32_t seq = w >> 1;
    uint32_t ack = cpu1_ipc_load32(&ipc->ack_seq);
    return (seq != 0u && seq != ack) ? 1 : 0;
}

CPU1_IPC_ALWAYS_INLINE int cpu1_ipc_check_stalled(cpu1_ipc_t *ipc, uint64_t now_us)
{
    uint32_t ack = cpu1_ipc_load32(&ipc->ack_seq);
    if (ack == ipc->last_seq) {
        ipc->stall_armed    = 0u;
        ipc->stall_since_us = 0ull;
        return 0;
    }
    if (!ipc->stall_armed || ack != ipc->stall_ack_seen) {
        ipc->stall_ack_seen = ack;
        ipc->stall_since_us = now_us;
        ipc->stall_armed    = 1u;
        return 0;
    }
    return ((now_us - ipc->stall_since_us) > CPU1_IPC_STALL_TIMEOUT_US) ? 1 : 0;
}

/* Wait-free producer entry: disarms new claims and returns the non-reading slot. */
CPU1_IPC_ALWAYS_INLINE uint32_t cpu1_ipc_begin_write(cpu1_ipc_t *ipc)
{
    cpu1_ipc_store32(&ipc->req_word, 0u);
    cpu1_ipc_barrier();
    uint32_t busy = cpu1_ipc_load32(&ipc->reading_slot);
    return (busy <= 1u) ? (busy ^ 1u) : (ipc->last_slot ^ 1u);
}

/* Wait-free producer commit: publishes the completed slot with a fresh nonzero seq. */
CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_publish_write(cpu1_ipc_t *ipc, uint32_t slot)
{
    uint32_t ack = cpu1_ipc_load32(&ipc->ack_seq);
    uint32_t seq = (ipc->last_seq + 1u) & 0x7FFFFFFFu;
    if (seq == 0u) {
        seq = 1u;
    }
    if (seq == ack) {
        seq = (seq + 1u) & 0x7FFFFFFFu;
        if (seq == 0u) {
            seq = 1u;
        }
    }
    ipc->last_seq  = seq;
    ipc->last_slot = slot & 1u;
    cpu1_ipc_barrier();
    cpu1_ipc_store32(&ipc->req_word, (seq << 1) | (slot & 1u));
    cpu1_ipc_barrier();
}

/* Consumer Dekker claim: returns 1 with (*out_slot, *out_seq) pinned against overwrites. */
CPU1_IPC_ALWAYS_INLINE int cpu1_ipc_claim_read(cpu1_ipc_t *ipc,
                                               uint32_t *out_slot,
                                               uint32_t *out_seq)
{
    uint32_t w   = cpu1_ipc_load32(&ipc->req_word);
    uint32_t seq = w >> 1;
    uint32_t ack = cpu1_ipc_load32(&ipc->ack_seq);
    if (seq == 0u || seq == ack) {
        return 0;
    }
    uint32_t idx = w & 1u;
    cpu1_ipc_store32(&ipc->reading_slot, idx);
    cpu1_ipc_barrier();
    if (cpu1_ipc_load32(&ipc->req_word) != w) {
        cpu1_ipc_store32(&ipc->reading_slot, CPU1_IPC_SLOT_IDLE);
        cpu1_ipc_barrier();
        return 0;
    }
    *out_slot = idx;
    *out_seq  = seq;
    return 1;
}

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_finish_read(cpu1_ipc_t *ipc, uint32_t seq)
{
    cpu1_ipc_barrier();
    cpu1_ipc_store32(&ipc->ack_seq, seq);
    cpu1_ipc_store32(&ipc->reading_slot, CPU1_IPC_SLOT_IDLE);
    cpu1_ipc_barrier();
}

CPU1_IPC_ALWAYS_INLINE void cpu1_ipc_abort_read(cpu1_ipc_t *ipc)
{
    cpu1_ipc_store32(&ipc->reading_slot, CPU1_IPC_SLOT_IDLE);
    cpu1_ipc_barrier();
}

#endif /* OPENFW_CPU1_IPC_H */
