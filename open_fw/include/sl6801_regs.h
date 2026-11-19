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
 * SL6801 SoC Peripheral Base Addresses
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
#define IPC_MBOX_BASE       0x40070000u /* CPU0<->CPU1 Hardware IPC Mailbox */
#define CRU_BASE            0x40080000u /* Clock, PLL & Reset Unit */
#define GPIO_BASE           0x40081000u /* GPIO & Pinmux (Ports A/B/C/D, +0x40 stride) */
#define US_TIMER_BASE       0x40082100u /* 64-bit Microsecond System Counter */
#define WDT_BASE            0x40083000u /* Watchdog Timer */
#define PWM_BASE            0x40084000u /* 6-Channel PWM (+0x20 + ch*0x20) */
#define AON_RTC_BASE        0x40085000u /* Always-On Power, Hardware RTC & Alarms */
#define RTWI_BASE           0x40085100u /* PMU RTWI Master (slave 0x30 on PC0/PC1) */
#define UART0_BASE          0x40090000u /* UART0 Debug Console (PD0 TX, 115200) */
#define UART1_BASE          0x40091000u /* UART1 (PD6/PD5) */
#define UART2_BASE          0x40092000u /* UART2 CLI Shell (PB22/PB23, 115200) */
#define LCDC_BASE           0x40095000u /* 8080 8-bit Parallel LCD Controller */
#define SARADC_BASE         0x40096000u /* 12-bit SARADC (kadc_ch0..5, key ladder on ch1) */
#define EFUSE_BASE          0x40099000u /* eFuse / OTP Controller (+0x20 + 4*idx) */
#define GPTIMER_BASE        0x4009B000u /* General-Purpose Timers */
#define SPINLOCK_BASE       0x400A0010u /* Hardware Spinlocks (16-byte stride) */
#define ASRC_BASE           0x400D0000u /* Audio Sample Rate Converter */
#define AUDIO_DSP_BASE      0x400D1000u /* Audio DSP / Hardware Filter */

/* ============================================================================
 * SYSCTL, US_TIMER, WDT, CRU
 * ============================================================================ */
#define SYSCTL_CPU_ID       REG32(SYSCTL_BASE + 0x0Cu) /* 0 = CPU0, !=0 = CPU1 */
#define SYSCTL_CPU1_READY   REG32(SYSCTL_BASE + 0x60u) /* write 0x8 from CPU1 */

#define US_TIMER_LO         REG32(US_TIMER_BASE + 0x04u)
#define US_TIMER_HI         REG32(US_TIMER_BASE + 0x08u)

#define WDT_FEED            REG32(WDT_BASE + 0x10u)
#define WDT_TIMEOUT         REG32(WDT_BASE + 0x14u)
#define WDT_CTRL0           REG32(WDT_BASE + 0x18u)
#define WDT_CTRL1           REG32(WDT_BASE + 0x1Cu)
#define WDT_FEED_KEY        0x28000001u

/* CRU IDs & SARADC clock/reset bank 2 (ID 0x54 = bit 20) */
#define CRU_CLK_SRC_HOSC24M 0x0Au
#define CRU_RST_PWM         0x44u
#define CRU_CLK_PWM         0x27u
#define CRU_RST_LCDC        0x5Cu
#define CRU_CLK_LCDC        0x3Fu
#define CRU_SARADC_CLK      REG32(CRU_BASE + 0x68u)
#define CRU_SARADC_RST      REG32(CRU_BASE + 0x78u)

/* ============================================================================
 * GPIO & Pinmux (0x40081000, Ports A..D @ +0x40 stride)
 * ============================================================================ */
#define GPIO_PORT_A         0u
#define GPIO_PORT_B         1u
#define GPIO_PORT_C         2u
#define GPIO_PORT_D         3u

#define GPIOA_BASE          (GPIO_BASE + 0x00u)
#define GPIOB_BASE          (GPIO_BASE + 0x40u)
#define GPIOC_BASE          (GPIO_BASE + 0x80u)
#define GPIOD_BASE          (GPIO_BASE + 0xC0u)
#define GPIO_LCD_PAD_EN     REG32(GPIO_BASE + 0x404u) /* |= 0x0000FFD0 for 11 LCDC pads */

static inline uint32_t gpio_base(uint32_t port)
{
    return GPIO_BASE + (port << 6);
}

static inline void gpio_set_mux(uint32_t port, uint32_t pin, uint32_t mux)
{
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(gpio_base(port) + ((pin >> 3) << 2));
    uint32_t shift = (pin & 7u) * 4u;
    *reg = (*reg & ~(0xFu << shift)) | ((mux & 0xFu) << shift);
}

static inline void gpio_write(uint32_t port, uint32_t pin, int val)
{
    REG32(gpio_base(port) + (val ? 0x34u : 0x38u)) = (1u << pin);
}

static inline int gpio_read(uint32_t port, uint32_t pin)
{
    return (int)((REG32(gpio_base(port) + 0x10u) >> pin) & 1u);
}

/* ============================================================================
 * LCDC (8080 8-bit Parallel @ 0x40095000) & PWM3 Backlight (@ 0x40084080)
 * ============================================================================ */
#define LCDC_MCU_TIMING     REG32(LCDC_BASE + 0x00u)
#define LCDC_IF_CTRL        REG32(LCDC_BASE + 0x08u) /* bit 0 = 0 (8080 parallel) */
#define LCDC_STATUS         REG32(LCDC_BASE + 0x14u) /* [30:29]=BUSY, [31]=DONE (W1C) */
#define LCDC_XFER_CTRL      REG32(LCDC_BASE + 0x20u)
#define LCDC_CMD_REG        REG32(LCDC_BASE + 0x24u)
#define LCDC_DATA_LEN       REG32(LCDC_BASE + 0x28u) /* byte count - 1 */
#define LCDC_TX_DATA        REG32(LCDC_BASE + 0x30u) /* LSB-first up to 4 bytes */
#define LCDC_XBAR0          REG32(LCDC_BASE + 0x40u)
#define LCDC_XBAR1          REG32(LCDC_BASE + 0x44u)

#define PWM3_CTRL           REG32(PWM_BASE + 0x80u)  /* bit28=BUSY, bits[7:0]=ctrl */
#define PWM3_PERIOD_DUTY    REG32(PWM_BASE + 0x84u)  /* (period << 16) | duty */

/* ============================================================================
 * UART0 (Debug Console @ 0x40090000, PD0 TX mux=12, 115200 bps @ 24 MHz)
 * ============================================================================ */
#define UART0_DATA          REG32(UART0_BASE + 0x00u)
#define UART0_CTRL          REG32(UART0_BASE + 0x04u)
#define UART0_STAT          REG32(UART0_BASE + 0x08u) /* [4:0] = TX FIFO free slots */
#define UART0_IE            REG32(UART0_BASE + 0x10u)
#define UART0_REG18         REG32(UART0_BASE + 0x18u)
#define UART0_REG1C         REG32(UART0_BASE + 0x1Cu)
#define UART0_REG20         REG32(UART0_BASE + 0x20u)

/* ============================================================================
 * AON Hardware RTC (0x40085000) & PMU RTWI Bus (0x40085100)
 * ============================================================================ */
#define RTC_DATE            REG32(AON_RTC_BASE + 0x04u)
#define RTC_TIME            REG32(AON_RTC_BASE + 0x08u)
#define RTC_ALARM0_VAL      REG32(AON_RTC_BASE + 0x10u)
#define RTC_ALARM0_CTRL     REG32(AON_RTC_BASE + 0x18u)
#define RTC_ALARM1_TIME     REG32(AON_RTC_BASE + 0x20u)
#define RTC_ALARM1_CTRL     REG32(AON_RTC_BASE + 0x24u)

#define RTWI_CTRL           REG32(RTWI_BASE + 0x00u)
#define RTWI_CMD            REG32(RTWI_BASE + 0x04u) /* bit31=BUSY, bit30=1, [15:8]=reg, [7:0]=0x60/0x61 */
#define RTWI_WDATA          REG32(RTWI_BASE + 0x08u)
#define RTWI_RDATA          REG32(RTWI_BASE + 0x0Cu)
#define RTWI_STATUS         REG32(RTWI_BASE + 0x10u) /* [15:12] != 0 => error */

/* ============================================================================
 * SARADC Key Ladder (0x40096000, ch1 on PD0 mux=15)
 * ============================================================================ */
#define SARADC_CTRL         REG32(SARADC_BASE + 0x00u)
#define SARADC_START        REG32(SARADC_BASE + 0x04u)
#define SARADC_IE           REG32(SARADC_BASE + 0x0Cu)
#define SARADC_REG10        REG32(SARADC_BASE + 0x10u)
#define SARADC_REG18        REG32(SARADC_BASE + 0x18u)
#define SARADC_CH1_THR      REG32(SARADC_BASE + 0x30u)
#define SARADC_CH1_DATA     REG32(SARADC_BASE + 0x34u) /* bits [11:0] continuous scan */

/* ============================================================================
 * Audio Codec, IPC Mailbox, Spinlocks & CPU1 Boot Mailbox
 * ============================================================================ */
#define CODEC_MICBIAS       REG32(AUDIO_CODEC_BASE + 0x00u) /* bits [25:24] */
#define CODEC_AMIC1_CTRL    REG32(AUDIO_CODEC_BASE + 0x08u) /* PGA gain bits [28:24] (0..31) */
#define CODEC_AMIC2_CTRL    REG32(AUDIO_CODEC_BASE + 0x0Cu) /* PGA gain bits [14:8]  (0..127) */
#define CODEC_ADC_MUX       REG32(AUDIO_CODEC_BASE + 0x20u) /* bits [11:8] & [19:16] */

#define MBOX_CPU0_IRQ_STAT  REG32(IPC_MBOX_BASE + 0x24u)
#define MBOX_CPU1_IRQ_STAT  REG32(IPC_MBOX_BASE + 0x2Cu)
#define MBOX_CH_CNT(ch)     REG32(IPC_MBOX_BASE + 0x44u + (uint32_t)(ch) * 0x20u)
#define MBOX_CH_DATA(ch)    REG32(IPC_MBOX_BASE + 0x48u + (uint32_t)(ch) * 0x20u)

#define SPINLOCK_REG(id)    REG32(SPINLOCK_BASE + (uint32_t)(id) * 0x10u)

#define CPU1_BOOT_TRAMP     REG32(0x30000600u)
#define CPU1_BOOT_SP        REG32(0x30000604u)
#define CPU1_BOOT_ENTRY     REG32(0x30000608u)

#endif /* SL6801_REGS_H */
