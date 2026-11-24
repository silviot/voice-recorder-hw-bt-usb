/* Tracked allocator for the host harness (make test-bt-host).
 *
 * The harness binary is linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free, so every allocation
 * made by Bluedroid (osi/allocator.c and direct callers), bt_port and the FreeRTOS kernel (pvPortMalloc below) lands
 * here, while libc-internal allocations and the POSIX port's own bookkeeping (compiled with -Dmalloc=__real_malloc)
 * do not. Each block carries a 16-byte header (size + magic) so live bytes/blocks are exact and a foreign or double
 * free aborts with a message. The real allocator underneath is the sanitizer's, so overflows past the end are still
 * caught by ASan.
 *
 * Thread safety: the POSIX port pre-empts tasks from a SIGALRM handler, which would deadlock if it switched away from
 * a task holding the libc/ASan allocator lock and the next task allocated. Every allocation therefore runs inside a
 * FreeRTOS critical section (signals blocked), which is also what makes the counters consistent.
 *
 * Failure injection (plan M1 "allocation-failure sweep"): bth_heap_fail_arm(n) makes the n-th allocation attempt
 * after arming return NULL (persistent: every attempt from the n-th on).
 * SPDX-License-Identifier: MIT */
#include <execinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "host_env.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

#define HDR 96u
#define BT_FRAMES 6
#define MAGIC_LIVE 0xB7A110C8u
#define MAGIC_DEAD 0xDEADF4EEu

/* Live blocks are linked (under the allocator lock) with their allocation sequence number and return address, so a
 * baseline failure can name what leaked (bth_heap_dump_since()). */
typedef struct hdr_s {
    size_t size;
    uint32_t magic;
    uint32_t seq;
    void *ra;
    struct hdr_s *prev, *next;
    uint64_t pad;
    void *bt[BT_FRAMES];         /* allocation call stack, only with BTH_HEAP_TRACE set (backtrace() is slow) */
} hdr_t;
_Static_assert(sizeof(hdr_t) == HDR, "header must keep 16-byte alignment");
static hdr_t *s_live;
static uint32_t s_seq;
static int s_trace = -1;

static bth_heap_stats_t s_st;
static uint64_t s_fail_at;
static bool s_fail_persistent;

static void lock(void) { vPortEnterCritical(); }
static void unlock(void) { vPortExitCritical(); }

/* Decide whether this attempt fails (caller holds the lock). */
static bool inject(void *ra)
{
    s_st.armed_seen++;
    if (s_fail_at != 0u && (s_st.armed_seen == s_fail_at || (s_fail_persistent && s_st.armed_seen > s_fail_at))) {
        s_st.injected++;
        s_st.last_injected_ra = ra;
        /* Print the call stack of the failed allocation at once (the stack may assert right after); the sweep runner
         * (run_sweep.py) symbolises it. */
        void *bt[12];
        int n = backtrace(bt, 12);
        fprintf(stderr, "INJECT attempt=%llu bt=", (unsigned long long)s_st.armed_seen);
        for (int i = 1; i < n; i++) {
            fprintf(stderr, "%s%p", i > 1 ? "," : "", bt[i]);
        }
        fprintf(stderr, "\n");
        return true;
    }
    return false;
}

static void *track(void *raw, size_t n, void *ra)
{
    hdr_t *h = (hdr_t *)raw;
    h->size = n;
    h->magic = MAGIC_LIVE;
    h->seq = ++s_seq;
    h->ra = ra;
    memset(h->bt, 0, sizeof(h->bt));
    if (s_trace < 0) {
        s_trace = getenv("BTH_HEAP_TRACE") != NULL;
    }
    if (s_trace) {
        void *f[BT_FRAMES + 3];
        int k = backtrace(f, BT_FRAMES + 3);
        for (int i = 3; i < k; i++) {
            h->bt[i - 3] = f[i];
        }
    }
    h->prev = NULL;
    h->next = s_live;
    if (s_live != NULL) {
        s_live->prev = h;
    }
    s_live = h;
    s_st.live_bytes += n;
    s_st.live_blocks++;
    s_st.allocs++;
    if (s_st.live_bytes > s_st.peak_bytes) {
        s_st.peak_bytes = s_st.live_bytes;
    }
    return (uint8_t *)raw + HDR;
}

static hdr_t *untrack(void *p)
{
    hdr_t *h = (hdr_t *)((uint8_t *)p - HDR);
    if (h->magic != MAGIC_LIVE) {
        fprintf(stderr, "bt_host heap: free of %p that is not a live tracked block (magic %08x)\n", p, h->magic);
        abort();
    }
    h->magic = MAGIC_DEAD;
    if (h->prev != NULL) {
        h->prev->next = h->next;
    } else {
        s_live = h->next;
    }
    if (h->next != NULL) {
        h->next->prev = h->prev;
    }
    s_st.live_bytes -= h->size;
    s_st.live_blocks--;
    return h;
}

static void *bth_alloc(size_t n, bool zero, void *ra)
{
    if (n > ((size_t)1 << 30)) {
        return NULL;
    }
    lock();
    void *r = NULL;
    if (!inject(ra)) {
        void *raw = zero ? __real_calloc(1, HDR + n) : __real_malloc(HDR + n);
        if (raw != NULL) {
            r = track(raw, n, ra);
        }
    }
    unlock();
    return r;
}

static void bth_release(void *p)
{
    if (p == NULL) {
        return;
    }
    lock();
    hdr_t *h = untrack(p);
    __real_free(h);
    unlock();
}

void *__wrap_malloc(size_t n) { return bth_alloc(n, false, __builtin_return_address(0)); }
void __wrap_free(void *p) { bth_release(p); }

void *__wrap_calloc(size_t n, size_t m)
{
    if (m != 0u && n > ((size_t)1 << 30) / m) {
        return NULL;
    }
    return bth_alloc(n * m, true, __builtin_return_address(0));
}

void *__wrap_realloc(void *p, size_t n)
{
    if (p == NULL) {
        return bth_alloc(n, false, __builtin_return_address(0));
    }
    if (n == 0u) {
        bth_release(p);
        return NULL;
    }
    void *q = bth_alloc(n, false, __builtin_return_address(0));
    if (q == NULL) {
        return NULL;                      /* C semantics: the old block stays valid */
    }
    lock();
    size_t old = ((hdr_t *)((uint8_t *)p - HDR))->size;
    unlock();
    memcpy(q, p, old < n ? old : n);
    bth_release(p);
    return q;
}

/* ---- FreeRTOS heap (replaces heap_5 on the host) ---- */
void *pvPortMalloc(size_t n)
{
    void *p = bth_alloc(n, false, __builtin_return_address(0));
#if (configUSE_MALLOC_FAILED_HOOK == 1)
    if (p == NULL) {
        extern void vApplicationMallocFailedHook(void);
        vApplicationMallocFailedHook();
    }
#endif
    return p;
}
void vPortFree(void *p) { bth_release(p); }
void *pvPortCalloc(size_t n, size_t m) { return __wrap_calloc(n, m); }
size_t xPortGetFreeHeapSize(void) { return (size_t)1 << 30; }
size_t xPortGetMinimumEverFreeHeapSize(void) { return (size_t)1 << 30; }
void vPortInitialiseBlocks(void) {}
void xPortResetHeapMinimumEverFreeHeapSize(void) {}

/* Same policy as the BT=1 firmware (src/rtos/README.md): count, never reboot, so malloc() can return NULL. */
void vApplicationMallocFailedHook(void)
{
    s_st.rtos_failed_hook++;
}

/* ---- ESP-IDF heap_caps_* (bt_port/esp_heap_caps.h; firmware: bt_port/bt_port_heap.c) ---- */
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return bth_alloc(size, false, __builtin_return_address(0)); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return __wrap_calloc(n, size); }
void *heap_caps_realloc(void *p, size_t size, uint32_t caps) { (void)caps; return __wrap_realloc(p, size); }
void heap_caps_free(void *p) { bth_release(p); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return (size_t)1 << 30; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return (size_t)1 << 30; }
size_t heap_caps_get_allocated_size(void *p)
{
    return p ? ((hdr_t *)((uint8_t *)p - HDR))->size : 0u;
}

/* ---- control ---- */
void bth_heap_stats(bth_heap_stats_t *out)
{
    lock();
    *out = s_st;
    unlock();
}

void bth_heap_fail_arm(uint64_t n, bool persistent)
{
    void *warm[2];
    (void)backtrace(warm, 2);   /* first call loads libgcc_s (allocates): never inside an injection */
    lock();
    s_st.armed_seen = 0u;
    s_fail_at = n;
    s_fail_persistent = persistent;
    unlock();
}

void bth_heap_fail_disarm(void)
{
    lock();
    s_fail_at = 0u;
    s_fail_persistent = false;
    unlock();
}

void bth_heap_count_start(void)
{
    bth_heap_fail_arm(0u, false);
}

uint64_t bth_heap_counted(void)
{
    lock();
    uint64_t n = s_st.armed_seen;
    unlock();
    return n;
}

void bth_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lock();
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    unlock();
    va_end(ap);
}

uint32_t bth_heap_seq(void)
{
    lock();
    uint32_t v = s_seq;
    unlock();
    return v;
}

/* Print the live blocks allocated after sequence number `seq` (leak report; BTH_HEAP_TRACE=1 adds call stacks,
 * symbolise with addr2line -f -e test_bt_host). Not under the allocator lock while printing: call when quiescent. */
void bth_heap_dump_since(uint32_t seq)
{
    int n = 0;
    for (hdr_t *h = s_live; h != NULL && n < 64; h = h->next) {
        if (h->seq > seq) {
            fprintf(stderr, "  live block seq %u size %zu ra %p bt", h->seq, h->size, h->ra);
            for (int i = 0; i < BT_FRAMES && h->bt[i] != NULL; i++) {
                fprintf(stderr, " %p", h->bt[i]);
            }
            fprintf(stderr, "\n");
            n++;
        }
    }
}
