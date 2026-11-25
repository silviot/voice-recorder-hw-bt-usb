/* Host IRQ / RTOS critical-section hook prototypes for test_audio_bt_mmio_bench.
 * SPDX-License-Identifier: MIT */
#ifndef OPENFW_AUDIO_BT_MMIO_IRQ_HOOKS_H
#define OPENFW_AUDIO_BT_MMIO_IRQ_HOOKS_H
#include <stdint.h>
uint32_t openfw_host_irq_save(void);
void     openfw_host_irq_restore(uint32_t pm);
void     openfw_host_rtos_crit_enter(void);
void     openfw_host_rtos_crit_exit(void);
#endif
