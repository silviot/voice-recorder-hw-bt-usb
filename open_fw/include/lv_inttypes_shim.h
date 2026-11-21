/*
 * Freestanding <inttypes.h> shim for bare-metal ARM Cortex-M4F LVGL builds.
 * On hosted builds (Linux tests), forwards directly to <inttypes.h>.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_LV_INTTYPES_SHIM_H
#define OPENFW_LV_INTTYPES_SHIM_H

#include <stdint.h>

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1
#include <inttypes.h>
#else
#define PRId8   "d"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"
#define PRId16  "d"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"
#define PRId32  "d"
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"
#define PRId64  "lld"
#define PRIu64  "llu"
#define PRIx64  "llx"
#define PRIX64  "llX"
#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"
#endif

#endif /* OPENFW_LV_INTTYPES_SHIM_H */
