/* Shared pattern for Exp 0 (exp_sram_fill.c / exp_sram_check.c). SPDX-License-Identifier: MIT */
#ifndef EXP_SRAM_PATTERN_H
#define EXP_SRAM_PATTERN_H
#include <stdint.h>

typedef struct {
    uint32_t start;
    uint32_t end; /* exclusive */
} srampat_region_t;

#define SRAMPAT_NREGIONS 2u
static const srampat_region_t k_srampat_regions[SRAMPAT_NREGIONS] = {
    { 0x00805C00u, 0x0081A380u }, /* candidate second heap_5 region (~81.9 KiB) */
    { 0x0086B000u, 0x00880000u }, /* RTOS_HEAP (84 KiB) */
};

/* murmur3 finaliser of the address: every word distinct, no stored state needed to verify */
static inline uint32_t srampat_value(uint32_t addr)
{
    uint32_t h = addr ^ 0x5EED0B7Eu;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}
#endif
