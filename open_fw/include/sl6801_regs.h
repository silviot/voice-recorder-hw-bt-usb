/*
 * Smartlink SL6801 Complete Hardware MMIO Register Definitions
 *
 * Clean-room C header derived from hardware & firmware mapping of the
 * Smartlink SL6801 dual-core ARM Cortex-M4F SoC.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL6801_REGS_H
#define SL6801_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#define REG8(addr)  (*(volatile uint8_t  *)(uintptr_t)(addr))

/* ============================================================================
 * ARM Cortex-M4F Core Peripherals (PPB)
 * ============================================================================ */
#define SYSTICK_CSR         REG32(0xE000E010u)
#define SYSTICK_RVR         REG32(0xE000E014u)
#define SYSTICK_CVR         REG32(0xE000E018u)
#define SYSTICK_CALIB       REG32(0xE000E01Cu)

#define SCB_VTOR            REG32(0xE000ED08u)
#define SCB_AIRCR           REG32(0xE000ED0Cu)
#define SCB_CPACR           REG32(0xE000ED88u)

#define SCB_AIRCR_VECTKEY   0x05FA0000u
#define SCB_AIRCR_SYSRESET  (1u << 2)
#define SCB_CPACR_FPU_FULL  (0xFu << 20)

/* ============================================================================
 * SL6801 SoC Peripheral Base Addresses (32 Verified 4 KB Blocks)
 * ============================================================================ */
#define SYSCTL_BASE         0x40000000u /* Core ID (+0x0C), CPU1 ready (+0x60) */
#define DMAC_BASE           0x40001000u /* Multi-channel DMA (0x40-byte stride) */
#define SDIO_BASE           0x40003000u /* SDIO / SDMMC Controller */
#define AUDIO_CODEC_BASE    0x40009000u /* Analog Audio Codec (AMIC1/2, DAC, PGA) */
#define ROM_DL_IF_BASE      0x4000A000u /* BootROM Debug/Download Slave I/F */
#define RF_BB_BASE          0x40010000u /* RF Baseband / Modem Ctrl */
#define RF_ANA_BASE         0x40011000u /* RF Analog / PHY Registers */
#define RWBT_BB_BASE        0x40020000u /* RivieraWaves RW-BT BR/EDR Baseband */
#define RWBT_CTRL_BASE      0x40027000u /* BT Controller / HCI / Sync Ctrl */
#define RWBT_EM_BASE        0x40030000u /* BT Exchange Memory (0x40030000..0x40038000) */
#define MUSB_BASE           0x40040000u /* Mentor MUSB USB 2.0 OTG Controller */
#define IPC_MBOX_BASE       0x40070000u /* CPU0<->CPU1 IPC Mailbox & PMU RTWI Bus */
#define CRU_BASE            0x40080000u /* Clock, PLL & Reset Unit */
#define GPIO_BASE           0x40081000u /* GPIO & Pinmux (Ports A/B/C/D, +0x40 stride) */
#define US_TIMER_BASE       0x40082100u /* 64-bit Microsecond System Counter */
#define WDT_BASE            0x40083000u /* Watchdog Timer */
#define PWM_BASE            0x40084000u /* 6-Channel PWM (+0x20 + ch*0x20) */
#define AON_RTC_BASE        0x40085000u /* Always-On Power & Hardware RTC/Alarms */
#define UART0_BASE          0x40090000u /* UART0 Debug Console (PD0 TX, 115200) */
#define UART1_BASE          0x40091000u /* UART1 (PD6/PD5) */
#define UART2_BASE          0x40092000u /* UART2 CLI Shell (PB22/PB23, 115200) */
#define SPI_LCD_BASE        0x40095000u /* SPI Controller driving ST7735S LCD */
#define QSPI_BASE           0x40096000u /* SPI / QSPI Flash Controller */
#define EFUSE_BASE          0x40099000u /* eFuse / OTP Controller (+0x20 + 4*idx) */
#define GPTIMER_BASE        0x4009B000u /* General-Purpose Timers */
#define SPINLOCK_BASE       0x400A0010u /* Hardware Spinlocks (16-byte stride) */
#define ASRC_BASE           0x400D0000u /* Audio Sample Rate Converter */
#define AUDIO_DSP_BASE      0x400D1000u /* Audio DSP / Hardware Filter */

/* ============================================================================
 * Verified Block Registers
 * ============================================================================ */

/* SYSCTL (0x40000000) */
#define SYSCTL_CPU_ID       REG32(SYSCTL_BASE + 0x0Cu) /* 0 = CPU0, !=0 = CPU1 */
#define SYSCTL_CPU1_READY   REG32(SYSCTL_BASE + 0x60u) /* write 0x8 from CPU1 */

/* 64-bit System Microsecond Counter (0x40082100) */
#define US_TIMER_LO         REG32(US_TIMER_BASE + 0x04u)
#define US_TIMER_HI         REG32(US_TIMER_BASE + 0x08u)

/* Watchdog Timer (0x40083000) */
#define WDT_FEED            REG32(WDT_BASE + 0x10u)
#define WDT_TIMEOUT         REG32(WDT_BASE + 0x14u)
#define WDT_CTRL0           REG32(WDT_BASE + 0x18u)
#define WDT_CTRL1           REG32(WDT_BASE + 0x1Cu)
#define WDT_FEED_KEY        0x28000001u

/* AON / Hardware RTC (0x40085000) */
#define RTC_DATE            REG32(AON_RTC_BASE + 0x04u)
#define RTC_TIME            REG32(AON_RTC_BASE + 0x08u)
#define RTC_ALARM0_VAL      REG32(AON_RTC_BASE + 0x10u)
#define RTC_ALARM0_CTRL     REG32(AON_RTC_BASE + 0x18u)
#define RTC_ALARM1_TIME     REG32(AON_RTC_BASE + 0x20u)
#define RTC_ALARM1_CTRL     REG32(AON_RTC_BASE + 0x24u)

/* Audio Codec (0x40009000) */
#define CODEC_MICBIAS       REG32(AUDIO_CODEC_BASE + 0x00u) /* bits [25:24] */
#define CODEC_AMIC1_CTRL    REG32(AUDIO_CODEC_BASE + 0x08u) /* PGA gain bits [28:24] (0..31) */
#define CODEC_AMIC2_CTRL    REG32(AUDIO_CODEC_BASE + 0x0Cu) /* PGA gain bits [14:8]  (0..127) */
#define CODEC_ADC_MUX       REG32(AUDIO_CODEC_BASE + 0x20u) /* bits [11:8] & [19:16] */

/* IPC Mailbox (0x40070000) */
#define MBOX_CPU0_IRQ_STAT  REG32(IPC_MBOX_BASE + 0x24u)
#define MBOX_CPU1_IRQ_STAT  REG32(IPC_MBOX_BASE + 0x2Cu)
#define MBOX_CH_CNT(ch)     REG32(IPC_MBOX_BASE + 0x44u + (uint32_t)(ch) * 0x20u)
#define MBOX_CH_DATA(ch)    REG32(IPC_MBOX_BASE + 0x48u + (uint32_t)(ch) * 0x20u)

/* Hardware Spinlock (0x400A0010, 16-byte stride) */
#define SPINLOCK_REG(id)    REG32(SPINLOCK_BASE + (uint32_t)(id) * 0x10u)

/* GPIO Ports (0x40081000, 0x40-byte stride per port) */
#define GPIOA_BASE          (GPIO_BASE + 0x00u)
#define GPIOB_BASE          (GPIO_BASE + 0x40u)
#define GPIOC_BASE          (GPIO_BASE + 0x80u)
#define GPIOD_BASE          (GPIO_BASE + 0xC0u)

/* CPU1 Boot Mailbox in shared SRAM (0x30000600) */
#define CPU1_BOOT_TRAMP     REG32(0x30000600u)
#define CPU1_BOOT_SP        REG32(0x30000604u)
#define CPU1_BOOT_ENTRY     REG32(0x30000608u)

#endif /* SL6801_REGS_H */
