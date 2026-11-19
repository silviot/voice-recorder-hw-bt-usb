/*
 * Cassini OpenFW — Standalone Open-Source Firmware Entry Point for SL6801 (HW_76)
 *
 * Layer 1 bring-up:
 *   - Runs directly from Stage-1 HLKJ Bootloader (0x00804C00 RAM + 0x00C0F030 XIP)
 *   - Zero stock FIRM code linked
 *   - Initializes SysTick, feeds WDT, reads SYSCTL CPU_ID and AON RTC,
 *     and emits telemetry over UART0 (PD0, 115200 bps) via BootROM printf.
 *
 * Next layers (plugged in as register bitfields are verified):
 *   - Layer 1b: GPIO, PWM backlight (pwm_ch3), ST7735S 80x160 LCD, SARADC buttons
 *   - Layer 2 : PMU RTWI (battery/poweroff) + SDIO + open-source FatFs R0.14
 *   - Layer 3 : AMIC1/AMIC2 audio codec + DMA -> mono 16 kHz PCM WAV recording
 *   - Layer 4 : Mentor MUSB USB MSC (TinyUSB)
 *   - Layer 5 : BTstack over Mask ROM RW-BT HCI -> native +XREC v5
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"

extern void wdt_feed(void);
extern void mdelay(uint32_t ms);
extern void systick_init(uint32_t cpu_hz);
extern uint32_t millis(void);
extern void rtc_read_raw(uint32_t *date_reg, uint32_t *time_reg);

int openfw_main(void)
{
    wdt_feed();
    systick_init(96000000u);

    uint32_t cpu_id = SYSCTL_CPU_ID;
    uint32_t rtc_d = 0, rtc_t = 0;
    rtc_read_raw(&rtc_d, &rtc_t);

    rom_printf("\r\n=== Cassini OpenFW (SL6801 Bare-Metal Layer 1) ===\r\n");
    rom_printf("CPU_ID=%u  RTC_DATE=0x%08x  RTC_TIME=0x%08x\r\n", cpu_id, rtc_d, rtc_t);

    uint32_t beat = 0;
    while (1) {
        wdt_feed();
        if ((beat % 10u) == 0u) {
            rtc_read_raw(&rtc_d, &rtc_t);
            rom_printf("[OpenFW] up=%ums  RTC=0x%08x/0x%08x\r\n", millis(), rtc_d, rtc_t);
        }
        mdelay(100u);
        beat++;
    }

    return 0;
}
