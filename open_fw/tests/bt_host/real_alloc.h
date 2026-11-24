/* Force-included into tests/fake_rom.c for the host harness only (tests/bt_host/Makefile). fake_rom models the ROM's
 * own g_bt_heap (with its own byte budget); its host allocations are bookkeeping, not part of the system under test,
 * so they bypass the tracked allocator and its failure injection (--wrap leaves __real_* = the sanitizer's malloc).
 * SPDX-License-Identifier: MIT */
#ifndef BT_HOST_REAL_ALLOC_H
#define BT_HOST_REAL_ALLOC_H
#include <stddef.h>
#include <stdlib.h>
void *__real_calloc(size_t n, size_t m);
void __real_free(void *p);
#define calloc(n, m) __real_calloc((n), (m))
#define free(p) __real_free(p)
#endif
