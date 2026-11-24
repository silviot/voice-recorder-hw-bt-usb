/* BT heap: libc malloc family + heap_caps_* mapped onto the FreeRTOS heap_5 region (.rtos_heap).
 *
 * Bluedroid's osi/allocator.c calls plain malloc/calloc/free (heap_caps_* only with external RAM), so we
 * override the libc symbols; this also keeps picolibc's own malloc/sbrk (and __heap_start/__heap_end) out
 * of the image. Every block carries an 8-byte header holding its size (heap_5 has no size query), which
 * also makes realloc and heap_caps_get_allocated_size() possible.
 * pvPortMalloc is not interrupt safe: never allocate from ISR context.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <FreeRTOS.h>
#include "esp_heap_caps.h"

#define HDR 8u
static inline void *blk_user(void *b) { return (uint8_t *)b + HDR; }

void *malloc(size_t n)
{
    if (n == 0 || n > 0x7FFFFFF0u) return NULL;
    uint8_t *b = pvPortMalloc(n + HDR);
    if (!b) return NULL;
    *(uint32_t *)b = (uint32_t)n;
    return blk_user(b);
}
void free(void *p)
{
    if (p) vPortFree((uint8_t *)p - HDR);
}
void *calloc(size_t n, size_t s)
{
    if (s && n > 0x7FFFFFF0u / s) return NULL;
    void *p = malloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
}
void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    uint32_t old = *(uint32_t *)((uint8_t *)p - HDR);
    if (n <= old) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    free(p);
    return q;
}

void *heap_caps_malloc(size_t s, uint32_t c) { (void)c; return malloc(s); }
void *heap_caps_calloc(size_t n, size_t s, uint32_t c) { (void)c; return calloc(n, s); }
void *heap_caps_realloc(void *p, size_t s, uint32_t c) { (void)c; return realloc(p, s); }
void heap_caps_free(void *p) { free(p); }
size_t heap_caps_get_free_size(uint32_t c) { (void)c; return xPortGetFreeHeapSize(); }
size_t heap_caps_get_largest_free_block(uint32_t c) { (void)c; return xPortGetFreeHeapSize(); } /* approximation */
size_t heap_caps_get_allocated_size(void *p) { return p ? *(uint32_t *)((uint8_t *)p - HDR) : 0; }
