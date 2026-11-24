#pragma once
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
#define __NOINIT_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define COREDUMP_DRAM_ATTR
