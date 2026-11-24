/*
 * Provisional minimal libc subset for the FreeRTOS kernel. Weak so that a real libc
 * (picolibc, evaluated for Bluedroid) replaces it without edits.
 * Compiled with -fno-builtin (project default) so the loops are not turned back into libcalls.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

#define W __attribute__((weak))

W void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) { *d++ = *s++; }
    return dst;
}

W void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) { *d++ = *s++; }
    } else {
        d += n; s += n;
        while (n--) { *--d = *--s; }
    }
    return dst;
}

W void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) { *d++ = (uint8_t)c; }
    return dst;
}

W int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) { return (int)*x - (int)*y; }
    }
    return 0;
}

W size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) { n++; }
    return n;
}

W char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) { dst[i] = src[i]; }
    for (; i < n; i++) { dst[i] = 0; }
    return dst;
}
