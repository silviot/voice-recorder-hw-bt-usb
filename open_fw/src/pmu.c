/*
 * Smartlink SL6801 Bare-Metal PMU RTWI Driver (0x40085100, Slave 0x30)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"

int pmu_write_reg(uint8_t reg, uint8_t val)
{
    RTWI_WDATA = (uint32_t)val;
    RTWI_CMD   = 0x40000060u | ((uint32_t)reg << 8);
    RTWI_CMD  |= (1u << 31);
    while ((int32_t)RTWI_CMD < 0) {
    }
    return (RTWI_STATUS & 0xF000u) ? 1 : 0;
}

uint8_t pmu_read_reg(uint8_t reg)
{
    RTWI_CMD  = 0x40000061u | ((uint32_t)reg << 8);
    RTWI_CMD |= (1u << 31);
    while ((int32_t)RTWI_CMD < 0) {
    }
    return (uint8_t)(RTWI_RDATA & 0xFFu);
}

uint32_t pmu_read_vbat_mv(void)
{
    uint8_t r47 = pmu_read_reg(0x47u);
    pmu_write_reg(0x47u, (uint8_t)(r47 | 0x10u));
    uint32_t raw = (uint32_t)(pmu_read_reg(0x2Eu) & 0x7Fu);
    pmu_write_reg(0x47u, (uint8_t)(r47 & ~0x10u));
    return (raw * 14173u + 2800000u) / 1000u;
}

pmu_chg_state_t pmu_get_charge_state(void)
{
    uint8_t r2a = pmu_read_reg(0x2Au);
    if ((r2a & 0x04u) != 0u) {
        return PMU_CHG_CHARGING;
    }
    if ((r2a & 0x40u) != 0u) {
        return PMU_CHG_FULL;
    }
    return PMU_CHG_DISCHARGING;
}

int pmu_is_vbus_present(void)
{
    return (pmu_read_reg(0x2Au) & 0x04u) ? 1 : 0;
}

void pmu_reboot_to_usb_bootloader(void)
{
    uint8_t r23 = pmu_read_reg(0x23u);
    uint8_t r00 = pmu_read_reg(0x00u);
    pmu_write_reg(0x23u, (uint8_t)(r23 | 0xC0u));
    pmu_write_reg(0x00u, (uint8_t)(r00 | 0x01u));
    system_reboot();
    while (1) {
        __asm__ volatile ("wfi");
    }
}

void pmu_reboot_to_sd_ota(void)
{
    uint8_t r23 = pmu_read_reg(0x23u);
    uint8_t r00 = pmu_read_reg(0x00u);
    pmu_write_reg(0x23u, (uint8_t)(r23 | 0xC0u));
    pmu_write_reg(0x00u, (uint8_t)(r00 & ~0x01u));
    system_reboot();
    while (1) {
        __asm__ volatile ("wfi");
    }
}
