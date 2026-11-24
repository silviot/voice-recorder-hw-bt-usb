/* Headphones audio core bit-exactness check (hp_selftest.c). SPDX-License-Identifier: MIT */
#ifndef HP_SELFTEST_H
#define HP_SELFTEST_H
#include <stdint.h>
#include "hp_sink.h"

/* CRC32 of the SBC + resampler outputs for the fixed input; must equal HP_SELFTEST_GOLDEN on host and target.
 * Regenerate the golden only for an intended change of the encoder or the resampler (test_hp_tx prints it). */
#define HP_SELFTEST_GOLDEN 0x3F0E9D61u
/* Scratch (about 3.6 KB): from the heap on target, so the check costs no static RAM in the BT region. */
typedef struct {
    hp_sink_t sink;
    int16_t   ring[1024];
    int16_t   in[2 * 64];
    int16_t   out[2 * 128];
    uint8_t   frame[128];
} hp_selftest_work_t;
uint32_t hp_selftest_crc(hp_selftest_work_t *w);
#endif
