/*
 * Host Unit & Multi-Threaded Dekker Stress Tests for CPU1 2-Slot UI IPC
 *
 * Exercises both rapid coalescing while CPU1 renders a slow (>126 ms) screen
 * under XIP+BT load, 31-bit sequence wrapping, genuine stall detection, and
 * TSan-verified producer/consumer mutual exclusion.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "openfw_cpu1_ipc.h"

#ifdef OPENFW_CPU1_IPC_HOST_ATOMIC
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

typedef struct {
    uint32_t seq;
    uint32_t payload;
    uint32_t guard;
    uint32_t words[8];
    uint32_t csum;
} test_slot_t;

static uint32_t slot_checksum(const test_slot_t *s)
{
    uint32_t h = s->seq ^ (s->payload * 0x9E3779B1u) ^ s->guard;
    for (size_t i = 0; i < 8u; i++) {
        h ^= (s->words[i] + (uint32_t)i * 0x85EBCA6Bu);
    }
    return h;
}

static void fill_slot(test_slot_t *s, uint32_t frame_id)
{
    /* Poison first so any reader race against an active write triggers TSan/checksum failure. */
    s->seq   = 0xFFFFFFFFu;
    s->guard = 0xBAD0BEEFu;
    s->seq     = frame_id;
    s->payload = frame_id * 2654435761u;
    s->guard   = 0x5A5A0000u ^ frame_id;
    for (uint32_t i = 0u; i < 8u; i++) {
        s->words[i] = frame_id ^ (0x10203040u * (i + 1u));
    }
    s->csum = slot_checksum(s);
}

static void verify_slot(const test_slot_t *s)
{
    assert(s->seq != 0u && s->seq != 0xFFFFFFFFu);
    assert(s->guard == (0x5A5A0000u ^ s->seq));
    for (uint32_t i = 0u; i < 8u; i++) {
        assert(s->words[i] == (s->seq ^ (0x10203040u * (i + 1u))));
    }
    assert(s->csum == slot_checksum(s));
}

static void test_slow_home_render_and_coalescing(void)
{
    cpu1_ipc_t  ipc;
    test_slot_t slots[2];
    memset(slots, 0, sizeof(slots));
    cpu1_ipc_init(&ipc);

    assert(!cpu1_ipc_is_busy(&ipc));
    assert(!cpu1_ipc_has_pending_req(&ipc));

    /* CPU0 submits initial frame (simulating SAVED -> HOME transition). */
    uint64_t t_us = 1000000ull;
    assert(!cpu1_ipc_check_stalled(&ipc, t_us));
    uint32_t s0 = cpu1_ipc_begin_write(&ipc);
    assert(s0 == 1u);
    fill_slot(&slots[s0], 1u);
    cpu1_ipc_publish_write(&ipc, s0);
    assert(cpu1_ipc_is_busy(&ipc));
    assert(cpu1_ipc_has_pending_req(&ipc));

    /* CPU1 claims slot 1 (HOME render starts; takes ~350 ms under XIP+BT load). */
    uint32_t c_slot = 0xFFu, c_seq = 0u;
    assert(cpu1_ipc_claim_read(&ipc, &c_slot, &c_seq) == 1);
    assert(c_slot == 1u && c_seq == 1u);
    verify_slot(&slots[c_slot]);

    /* Over the next 350 ms, CPU0 checks stall and updates UI 18 times every 20 ms.
     * Old cpu1_ui.c timed out at 100 ms ("slot busy") and 250 ms ("stall")! */
    for (uint32_t k = 2u; k <= 19u; k++) {
        t_us += 20000ull;
        assert(!cpu1_ipc_check_stalled(&ipc, t_us));
        uint32_t w_slot = cpu1_ipc_begin_write(&ipc);
        assert(w_slot == 0u); /* must NEVER clobber reading slot 1 */
        fill_slot(&slots[w_slot], k);
        cpu1_ipc_publish_write(&ipc, w_slot);
        assert(cpu1_ipc_is_busy(&ipc));
        assert(cpu1_ipc_has_pending_req(&ipc));
    }

    /* Slot 1 payload remained untouched while slot 0 absorbed frames 2..19. */
    assert(slots[c_slot].seq == 1u);
    verify_slot(&slots[c_slot]);
    cpu1_ipc_finish_read(&ipc, c_seq);

    /* Coalesced request on slot 0 is still pending until CPU1 renders it. */
    assert(cpu1_ipc_is_busy(&ipc));
    assert(cpu1_ipc_has_pending_req(&ipc));
    assert(cpu1_ipc_claim_read(&ipc, &c_slot, &c_seq) == 1);
    assert(c_slot == 0u);
    assert(slots[c_slot].seq == 19u);
    verify_slot(&slots[c_slot]);
    cpu1_ipc_finish_read(&ipc, c_seq);

    assert(!cpu1_ipc_is_busy(&ipc));
    assert(!cpu1_ipc_has_pending_req(&ipc));
}

static void test_seq_wrap_and_stall_threshold(void)
{
    cpu1_ipc_t ipc;
    cpu1_ipc_init(&ipc);

    /* Disarm state must report no pending request and ignore stale zero seq. */
    uint32_t wr = cpu1_ipc_begin_write(&ipc);
    assert(wr == 1u);
    assert(!cpu1_ipc_has_pending_req(&ipc));
    uint32_t c_slot = 0xFFu, c_seq = 0u;
    assert(cpu1_ipc_claim_read(&ipc, &c_slot, &c_seq) == 0);
    cpu1_ipc_publish_write(&ipc, wr);

    /* Test 31-bit sequence wrap (0x7FFFFFFEu -> 0x7FFFFFFFu -> 1u) skipping 0 and ack. */
    cpu1_ipc_init(&ipc);
    ipc.last_seq = 0x7FFFFFFEu;
    cpu1_ipc_store32(&ipc.ack_seq, 1u);

    uint32_t s = cpu1_ipc_begin_write(&ipc);
    cpu1_ipc_publish_write(&ipc, s);
    assert(ipc.last_seq == 0x7FFFFFFFu);
    assert(cpu1_ipc_claim_read(&ipc, &c_slot, &c_seq) == 1 && c_seq == 0x7FFFFFFFu);
    cpu1_ipc_finish_read(&ipc, c_seq);

    /* Next seq would wrap to 0 and collide with skip-0 -> 1u == ack? Here ack == 0x7FFFFFFFu. */
    s = cpu1_ipc_begin_write(&ipc);
    cpu1_ipc_publish_write(&ipc, s);
    assert(ipc.last_seq == 1u);

    /* Suppose ack somehow equaled 1u before wrap: must skip both 0 and ack -> 2u. */
    ipc.last_seq = 0x7FFFFFFFu;
    cpu1_ipc_store32(&ipc.ack_seq, 1u);
    s = cpu1_ipc_begin_write(&ipc);
    cpu1_ipc_publish_write(&ipc, s);
    assert(ipc.last_seq == 2u);

    /* Genuine stall window resets whenever ack_seq advances, even across 10s+ total runtime. */
    cpu1_ipc_init(&ipc);
    uint64_t now_us = 0ull; /* Also verify t=0 works cleanly without sentinel bugs */
    for (int f = 0; f < 5; f++) {
        uint32_t ws = cpu1_ipc_begin_write(&ipc);
        cpu1_ipc_publish_write(&ipc, ws);
        assert(!cpu1_ipc_check_stalled(&ipc, now_us));
        now_us += 2500000ull; /* 2.5 s slow render (< 3.0 s watchdog) */
        assert(!cpu1_ipc_check_stalled(&ipc, now_us));
        assert(cpu1_ipc_claim_read(&ipc, &c_slot, &c_seq) == 1);
        cpu1_ipc_finish_read(&ipc, c_seq);
    }
    /* Now stop CPU1 from acking beyond CPU1_IPC_STALL_TIMEOUT_US (3.0 s). */
    uint32_t ws = cpu1_ipc_begin_write(&ipc);
    cpu1_ipc_publish_write(&ipc, ws);
    assert(!cpu1_ipc_check_stalled(&ipc, now_us));
    assert(!cpu1_ipc_check_stalled(&ipc, now_us + CPU1_IPC_STALL_TIMEOUT_US));
    assert(cpu1_ipc_check_stalled(&ipc, now_us + CPU1_IPC_STALL_TIMEOUT_US + 1ull));
}

#ifdef OPENFW_CPU1_IPC_HOST_ATOMIC
typedef struct {
    cpu1_ipc_t  ipc;
    test_slot_t slots[2];
    _Atomic int done;
    uint32_t    total_frames;
    uint64_t    frames_read;
    uint32_t    last_frame_seen;
} stress_ctx_t;

static void *consumer_thread(void *arg)
{
    stress_ctx_t *ctx = (stress_ctx_t *)arg;
    uint32_t prev_frame = 0u;
    for (;;) {
        uint32_t slot = 0xFFu, seq = 0u;
        if (cpu1_ipc_claim_read(&ctx->ipc, &slot, &seq)) {
            assert(slot <= 1u);
            test_slot_t snap1 = ctx->slots[slot];
            verify_slot(&snap1);
            if ((ctx->frames_read & 63u) == 0u) {
                /* Hold slot briefly while producer overwrites the OTHER slot. */
                sched_yield();
            }
            test_slot_t snap2 = ctx->slots[slot];
            assert(memcmp(&snap1, &snap2, sizeof(snap1)) == 0);
            assert(snap1.seq > prev_frame);
            prev_frame = snap1.seq;
            cpu1_ipc_finish_read(&ctx->ipc, seq);
            ctx->frames_read++;
            ctx->last_frame_seen = prev_frame;
        } else if (atomic_load_explicit(&ctx->done, memory_order_seq_cst)) {
            if (!cpu1_ipc_has_pending_req(&ctx->ipc)) {
                break;
            }
        } else if ((ctx->frames_read & 15u) == 0u) {
            sched_yield();
        }
    }
    return NULL;
}

static void test_multithreaded_dekker_stress(void)
{
    stress_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    cpu1_ipc_init(&ctx.ipc);
    atomic_init(&ctx.done, 0);
    ctx.total_frames = 50000u;

    pthread_t th;
    int rc = pthread_create(&th, NULL, consumer_thread, &ctx);
    assert(rc == 0);

    for (uint32_t f = 1u; f <= ctx.total_frames; f++) {
        uint32_t slot = cpu1_ipc_begin_write(&ctx.ipc);
        assert(slot <= 1u);
        fill_slot(&ctx.slots[slot], f);
        cpu1_ipc_publish_write(&ctx.ipc, slot);
        if ((f % 250u) == 0u) {
            sched_yield();
        }
    }
    atomic_store_explicit(&ctx.done, 1, memory_order_seq_cst);
    rc = pthread_join(th, NULL);
    assert(rc == 0);

    assert(ctx.frames_read >= 1u);
    assert(ctx.last_frame_seen == ctx.total_frames);
    assert(!cpu1_ipc_is_busy(&ctx.ipc));
    printf("cpu1_ipc Dekker TSan stress OK: %u published, %llu rendered/verified\n",
           ctx.total_frames, (unsigned long long)ctx.frames_read);
}
#endif

int main(void)
{
    test_slow_home_render_and_coalescing();
    test_seq_wrap_and_stall_threshold();
#ifdef OPENFW_CPU1_IPC_HOST_ATOMIC
    test_multithreaded_dekker_stress();
#endif
    printf("test_cpu1_ipc PASS\n");
    return 0;
}
