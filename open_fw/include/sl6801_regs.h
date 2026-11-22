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

#ifdef OPENFW_HOST_MMIO
uint32_t openfw_mmio_rd32(uint32_t addr);
void     openfw_mmio_wr32(uint32_t addr, uint32_t val);
uint16_t openfw_mmio_rd16(uint32_t addr);
void     openfw_mmio_wr16(uint32_t addr, uint16_t val);
uint8_t  openfw_mmio_rd8(uint32_t addr);
void     openfw_mmio_wr8(uint32_t addr, uint8_t val);
void     openfw_mmio_nop(void);
void     openfw_mmio_dsb(void);
void     openfw_mmio_wfi(void);
void     openfw_host_dmac_register_ptr(uint32_t ch, void *buf, uint32_t len);

#define OPENFW_NOP() openfw_mmio_nop()
#define OPENFW_DSB() openfw_mmio_dsb()
#define OPENFW_ISB() __asm__ volatile ("" ::: "memory")
#define OPENFW_WFI() openfw_mmio_wfi()
#define OPENFW_IRQ_SAVE(pm)                do { (pm) = 0u; } while (0)
#define OPENFW_IRQ_RESTORE(pm)             do { (void)(pm); } while (0)
#define OPENFW_HOST_DMAC_BUF(ch, ptr, len) do { openfw_host_dmac_register_ptr((ch), (ptr), (len)); } while (0)
#else
#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#define REG8(addr)  (*(volatile uint8_t  *)(uintptr_t)(addr))

static inline uint32_t openfw_mmio_rd32(uint32_t addr)              { return REG32(addr); }
static inline void     openfw_mmio_wr32(uint32_t addr, uint32_t v)  { REG32(addr) = v; }
static inline uint16_t openfw_mmio_rd16(uint32_t addr)              { return REG16(addr); }
static inline void     openfw_mmio_wr16(uint32_t addr, uint16_t v)  { REG16(addr) = v; }
static inline uint8_t  openfw_mmio_rd8(uint32_t addr)               { return REG8(addr); }
static inline void     openfw_mmio_wr8(uint32_t addr, uint8_t v)    { REG8(addr) = v; }

#define OPENFW_NOP() __asm__ volatile ("nop")
#define OPENFW_DSB() __asm__ volatile ("dsb" ::: "memory")
#define OPENFW_ISB() __asm__ volatile ("isb" ::: "memory")
#define OPENFW_WFI() __asm__ volatile ("wfi")
#define OPENFW_IRQ_SAVE(pm) do { \
    __asm__ volatile ("mrs %0, primask" : "=r"(pm)); \
    __asm__ volatile ("cpsid i" ::: "memory"); \
} while (0)
#define OPENFW_IRQ_RESTORE(pm) do { \
    if (((pm) & 1u) == 0u) { \
        __asm__ volatile ("cpsie i" ::: "memory"); \
    } \
} while (0)
#define OPENFW_HOST_DMAC_BUF(ch, ptr, len) do { } while (0)
#endif

static inline void openfw_mmio_set32(uint32_t addr, uint32_t bits)
{
    openfw_mmio_wr32(addr, openfw_mmio_rd32(addr) | bits);
}

static inline void openfw_mmio_clr32(uint32_t addr, uint32_t bits)
{
    openfw_mmio_wr32(addr, openfw_mmio_rd32(addr) & ~bits);
}

static inline void openfw_mmio_rmw32(uint32_t addr, uint32_t clear_mask, uint32_t set_bits)
{
    openfw_mmio_wr32(addr, (openfw_mmio_rd32(addr) & ~clear_mask) | set_bits);
}

/* ============================================================================
 * ARM Cortex-M4F Core Peripherals (PPB)
 * ============================================================================ */
#define SYSTICK_CSR_ADDR    0xE000E010u
#define SYSTICK_RVR_ADDR    0xE000E014u
#define SYSTICK_CVR_ADDR    0xE000E018u
#define SYSTICK_CALIB_ADDR  0xE000E01Cu

#define SCB_VTOR_ADDR       0xE000ED08u
#define SCB_AIRCR_ADDR      0xE000ED0Cu
#define SCB_SHCSR_ADDR      0xE000ED24u
#define SCB_CFSR_ADDR       0xE000ED28u
#define SCB_HFSR_ADDR       0xE000ED2Cu
#define SCB_MMFAR_ADDR      0xE000ED34u
#define SCB_BFAR_ADDR       0xE000ED38u
#define SCB_CPACR_ADDR      0xE000ED88u

#define SCB_AIRCR_VECTKEY   0x05FA0000u
#define SCB_AIRCR_SYSRESET  (1u << 2)
#define SCB_SHCSR_FAULTS_EN 0x00070000u /* MemManage | BusFault | UsageFault */
#define SCB_CPACR_FPU_FULL  (0xFu << 20)

#define NVIC_ISER_ADDR(n)   (0xE000E100u + 4u * (uint32_t)(n))
#define NVIC_ICER_ADDR(n)   (0xE000E180u + 4u * (uint32_t)(n))
#define NVIC_ICPR_ADDR(n)   (0xE000E280u + 4u * (uint32_t)(n))

#ifndef OPENFW_HOST_MMIO
#define SYSTICK_CSR         REG32(SYSTICK_CSR_ADDR)
#define SYSTICK_RVR         REG32(SYSTICK_RVR_ADDR)
#define SYSTICK_CVR         REG32(SYSTICK_CVR_ADDR)
#define SYSTICK_CALIB       REG32(SYSTICK_CALIB_ADDR)

#define SCB_VTOR            REG32(SCB_VTOR_ADDR)
#define SCB_AIRCR           REG32(SCB_AIRCR_ADDR)
#define SCB_SHCSR           REG32(SCB_SHCSR_ADDR)
#define SCB_CFSR            REG32(SCB_CFSR_ADDR)
#define SCB_HFSR            REG32(SCB_HFSR_ADDR)
#define SCB_MMFAR           REG32(SCB_MMFAR_ADDR)
#define SCB_BFAR            REG32(SCB_BFAR_ADDR)
#define SCB_CPACR           REG32(SCB_CPACR_ADDR)

#define NVIC_ISER(n)        REG32(NVIC_ISER_ADDR(n))
#define NVIC_ICER(n)        REG32(NVIC_ICER_ADDR(n))
#define NVIC_ICPR(n)        REG32(NVIC_ICPR_ADDR(n))
#endif

static inline void nvic_enable_irq(uint32_t irq)
{
    openfw_mmio_wr32(NVIC_ISER_ADDR(irq >> 5), (1u << (irq & 31u)));
}

static inline void nvic_disable_irq(uint32_t irq)
{
    openfw_mmio_wr32(NVIC_ICER_ADDR(irq >> 5), (1u << (irq & 31u)));
}

static inline void nvic_clear_pending(uint32_t irq)
{
    openfw_mmio_wr32(NVIC_ICPR_ADDR(irq >> 5), (1u << (irq & 31u)));
}

#define IRQ_BT              26u
#define IRQ_USB_MC          41u
#define IRQ_USB_DMA         42u
#define IRQ_USB_PHY         43u
#define IRQ_SDIO            44u
#define IRQ_RTC             47u
#define DMAC_IRQ_BASE       60u /* CH0..CH7 = NVIC IRQ 60..67 */

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
 * SYSCTL, SPI NOR XIP (0x30000000), US_TIMER, WDT, CRU
 * ============================================================================ */
#define SYSCTL_USB_CTRL_ADDR (SYSCTL_BASE + 0x00u) /* bit 0 = PHY PWR, bit 6 = USB EN */
#define SYSCTL_SRAM_WS0_ADDR (SYSCTL_BASE + 0x04u)
#define SYSCTL_SRAM_WS1_ADDR (SYSCTL_BASE + 0x08u)
#define SYSCTL_CPU_ID_ADDR   (SYSCTL_BASE + 0x0Cu) /* 0 = CPU0, !=0 = CPU1 */
#define SYSCTL_CODEC_SRAM_ADDR (SYSCTL_BASE + 0x20u) /* bit 2 = CPU access to codec SRAM */
#define SYSCTL_CPU1_READY_ADDR (SYSCTL_BASE + 0x60u) /* write 0x8 from CPU1 */

#ifndef OPENFW_HOST_MMIO
#define SYSCTL_USB_CTRL     REG32(SYSCTL_USB_CTRL_ADDR)
#define SYSCTL_SRAM_WS0     REG32(SYSCTL_SRAM_WS0_ADDR)
#define SYSCTL_SRAM_WS1     REG32(SYSCTL_SRAM_WS1_ADDR)
#define SYSCTL_CPU_ID       REG32(SYSCTL_CPU_ID_ADDR)
#define SYSCTL_CODEC_SRAM   REG32(SYSCTL_CODEC_SRAM_ADDR)
#define SYSCTL_CPU1_READY   REG32(SYSCTL_CPU1_READY_ADDR)
#endif

#define NOR_BASE            0x30000000u
#define NOR_XIP_WINDOW      0x00C00000u
#ifndef OPENFW_HOST_MMIO
#define NOR_CTRL            REG32(NOR_BASE + 0x00u)  /* 0x1 = enable polling controller */
#define NOR_XIP_CFG         REG32(NOR_BASE + 0x20u)  /* bit 25 = XIP enable */
#define NOR_XIP_OFFSET      REG32(NOR_BASE + 0x24u)  /* flash offset mapped to 0x00C00000 */
#define XIP_BRIDGE_CTRL     REG32(NOR_BASE + 0x204u) /* bit 8 = map AHB XIP window */
#define XIP_CACHE_CTRL0     REG32(NOR_BASE + 0x800u)
#define XIP_CACHE_CTRL1     REG32(NOR_BASE + 0x804u)
#endif
#define NOR_XIP_CFG_1WIRE   0x02A09503u              /* 1-wire 0x03 read */
#define NOR_XIP_CFG_DUAL    0x02A8993Bu              /* Fast Read Dual Output 0x3B */
#define NOR_XIP_CFG_QUAD    0x02A6BDEBu              /* Fast Read Quad I/O 0xEB (4-wire) */

#define US_TIMER_LO_ADDR    (US_TIMER_BASE + 0x04u)
#define US_TIMER_HI_ADDR    (US_TIMER_BASE + 0x08u)

#define WDT_EN_ADDR         (WDT_BASE + 0x00u)
#define WDT_IRQ_CLR_ADDR    (WDT_BASE + 0x04u)
#define WDT_FEED_ADDR       (WDT_BASE + 0x10u)
#define WDT_TIMEOUT_ADDR    (WDT_BASE + 0x14u)
#define WDT_MODE_ADDR       WDT_TIMEOUT_ADDR   /* 0x2001 = HW reset, 0x2004 = IRQ */
#define WDT_CTRL0_ADDR      (WDT_BASE + 0x18u)
#define WDT_CFG_ADDR        WDT_CTRL0_ADDR     /* [7:4] = timeout idx, [0] = enable */
#define WDT_CTRL1_ADDR      (WDT_BASE + 0x1Cu)
#define WDT_FEED_KEY        0x28000001u
#define WDT_MODE_HW_RESET   0x2001u

/* CRU raw registers & clock/reset IDs */
#define CRU_PLL0_CTRL_ADDR     (CRU_BASE + 0x00u)
#define CRU_PLL1_CTRL_ADDR     (CRU_BASE + 0x08u)
#define CRU_APLL_CTRL_ADDR     (CRU_BASE + 0x10u)
#define CRU_APLL_FRAC_ADDR     (CRU_BASE + 0x14u)
#define CRU_PLL0_POSTDIV_ADDR  (CRU_BASE + 0x30u)
#define CRU_CLK_SRC_GATE_ADDR  (CRU_BASE + 0x30u)  /* bit 4 = BT/RF clk, bit 6 = USB 48M tap */
#define CRU_RST_STAT1_ADDR     (CRU_BASE + 0x64u)  /* bank 1 reset status/release */
#define CRU_SARADC_CLK_ADDR    (CRU_BASE + 0x68u)
#define CRU_RST_SET1_ADDR      (CRU_BASE + 0x74u)  /* bank 1 reset control */
#define CRU_SARADC_RST_ADDR    (CRU_BASE + 0x78u)
#define CRU_CLK_EN_USB_ADDR    (CRU_BASE + 0x84u)  /* bit 0 = USB controller clk 0x12 */
#define CRU_CLK_EN_AUDIO_ADDR  (CRU_BASE + 0x88u)  /* bit 0 = audio codec clk 0x13 */
#define CRU_BT_RF_WRAP_ADDR    (CRU_BASE + 0xE8u)  /* bit 0 = BT RF wrapper release */

#ifndef OPENFW_HOST_MMIO
#define US_TIMER_LO         REG32(US_TIMER_LO_ADDR)
#define US_TIMER_HI         REG32(US_TIMER_HI_ADDR)
#define WDT_FEED            REG32(WDT_FEED_ADDR)
#define WDT_TIMEOUT         REG32(WDT_TIMEOUT_ADDR)
#define WDT_CTRL0           REG32(WDT_CTRL0_ADDR)
#define WDT_CTRL1           REG32(WDT_CTRL1_ADDR)
#define CRU_PLL0_CTRL       REG32(CRU_PLL0_CTRL_ADDR)
#define CRU_PLL1_CTRL       REG32(CRU_PLL1_CTRL_ADDR)
#define CRU_APLL_CTRL       REG32(CRU_APLL_CTRL_ADDR)
#define CRU_APLL_FRAC       REG32(CRU_APLL_FRAC_ADDR)
#define CRU_PLL0_POSTDIV    REG32(CRU_PLL0_POSTDIV_ADDR)
#define CRU_CLK_SRC_GATE    REG32(CRU_CLK_SRC_GATE_ADDR)
#define CRU_RST_STAT1       REG32(CRU_RST_STAT1_ADDR)
#define CRU_RST_SET1        REG32(CRU_RST_SET1_ADDR)
#define CRU_CLK_EN_USB      REG32(CRU_CLK_EN_USB_ADDR)
#define CRU_CLK_EN_AUDIO    REG32(CRU_CLK_EN_AUDIO_ADDR)
#define CRU_BT_RF_WRAP      REG32(CRU_BT_RF_WRAP_ADDR)
#define CRU_SARADC_CLK      REG32(CRU_SARADC_CLK_ADDR)
#define CRU_SARADC_RST      REG32(CRU_SARADC_RST_ADDR)
#endif

#define CRU_CLK_SRC_PLL0    0x02u
#define CRU_CLK_SRC_HOSC24M 0x0Au
#define CRU_CLK_APLL        0x00u
#define CRU_CLK_PLL0        0x02u
#define CRU_CLK_CPU         0x03u
#define CRU_CLK_HCLK        0x04u
#define CRU_CLK_PCLK        0x05u
#define CRU_CLK_BUS6        0x06u
#define CRU_CLK_USB_48M     0x09u
#define CRU_CLK_SDIO        0x11u
#define CRU_CLK_USB         0x12u
#define CRU_CLK_AUDIO       0x13u
#define CRU_CLK_PWM         0x27u
#define CRU_CLK_NOR         0x2Bu
#define CRU_CLK_LCDC        0x3Fu

#define CRU_RST_CPU1        0x00u
#define CRU_RST_NOR         0x01u
#define CRU_RST_CODEC_SRAM  0x20u
#define CRU_RST_DMAC        0x21u
#define CRU_RST_BT          0x22u
#define CRU_RST_USB         0x23u
#define CRU_RST_SDIO        0x24u
#define CRU_RST_AUDIO       0x25u
#define CRU_RST_PWM         0x44u
#define CRU_RST_XIP_BUS     0x5Au
#define CRU_RST_LCDC        0x5Cu

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
#define GPIO_LCD_PAD_EN_ADDR (GPIO_BASE + 0x404u) /* |= 0x0000FFD0 for 11 LCDC pads */
#ifndef OPENFW_HOST_MMIO
#define GPIO_LCD_PAD_EN     REG32(GPIO_LCD_PAD_EN_ADDR)
#endif

static inline uint32_t gpio_base(uint32_t port)
{
    return GPIO_BASE + (port << 6);
}

static inline void gpio_set_mux(uint32_t port, uint32_t pin, uint32_t mux)
{
    uint32_t addr = gpio_base(port) + ((pin >> 3) << 2);
    uint32_t shift = (pin & 7u) * 4u;
    openfw_mmio_rmw32(addr, 0xFu << shift, (mux & 0xFu) << shift);
}

static inline void gpio_write(uint32_t port, uint32_t pin, int val)
{
    openfw_mmio_wr32(gpio_base(port) + (val ? 0x34u : 0x38u), (1u << pin));
}

static inline int gpio_read(uint32_t port, uint32_t pin)
{
    return (int)((openfw_mmio_rd32(gpio_base(port) + 0x10u) >> pin) & 1u);
}

/* ============================================================================
 * LCDC (8080 8-bit Parallel @ 0x40095000) & PWM3 Backlight (@ 0x40084080)
 * ============================================================================ */
#define LCDC_MCU_TIMING_ADDR (LCDC_BASE + 0x00u)
#define LCDC_IF_CTRL_ADDR    (LCDC_BASE + 0x08u) /* bit 0 = 0 (8080 parallel) */
#define LCDC_STATUS_ADDR     (LCDC_BASE + 0x14u) /* [30:29]=BUSY, [31]=DONE (W1C) */
#define LCDC_XFER_CTRL_ADDR  (LCDC_BASE + 0x20u)
#define LCDC_CMD_REG_ADDR    (LCDC_BASE + 0x24u)
#define LCDC_DATA_LEN_ADDR   (LCDC_BASE + 0x28u) /* byte count - 1 */
#define LCDC_TX_DATA_ADDR    (LCDC_BASE + 0x30u) /* LSB-first up to 4 bytes */
#define LCDC_XBAR0_ADDR      (LCDC_BASE + 0x40u)
#define LCDC_XBAR1_ADDR      (LCDC_BASE + 0x44u)

#define PWM3_CTRL_ADDR        (PWM_BASE + 0x80u)  /* bit28=BUSY, bits[7:0]=ctrl */
#define PWM3_PERIOD_DUTY_ADDR (PWM_BASE + 0x84u)  /* (period << 16) | duty */

/* ============================================================================
 * UART0 (Debug Console @ 0x40090000, PD0 TX mux=12, 115200 bps @ 24 MHz)
 * ============================================================================ */
#define UART0_DATA_ADDR     (UART0_BASE + 0x00u)
#define UART0_CTRL_ADDR     (UART0_BASE + 0x04u)
#define UART0_STAT_ADDR     (UART0_BASE + 0x08u) /* [4:0] = TX FIFO free slots */
#define UART0_IE_ADDR       (UART0_BASE + 0x10u)
#define UART0_REG18_ADDR    (UART0_BASE + 0x18u)
#define UART0_REG1C_ADDR    (UART0_BASE + 0x1Cu)
#define UART0_REG20_ADDR    (UART0_BASE + 0x20u)

/* ============================================================================
 * AON Hardware RTC (0x40085000) & PMU RTWI Bus (0x40085100)
 * ============================================================================ */
#define AON_RTC_CTRL_ADDR    (AON_RTC_BASE + 0x00u) /* bit 4=mode, bit 8=HALT */
#define RTC_DATE_ADDR        (AON_RTC_BASE + 0x04u) /* binary: [24]leap [22:16]yr-2000 [11:8]mon [4:0]day */
#define RTC_TIME_ADDR        (AON_RTC_BASE + 0x08u) /* binary: [26:24]wday [20:16]hr [13:8]min [5:0]sec */
#define RTC_ALARM0_VAL_ADDR  (AON_RTC_BASE + 0x10u)
#define RTC_ALARM0_CTRL_ADDR (AON_RTC_BASE + 0x18u)
#define RTC_ALARM1_TIME_ADDR (AON_RTC_BASE + 0x20u)
#define RTC_ALARM1_CTRL_ADDR (AON_RTC_BASE + 0x24u)
#define AON_PWR_CTRL0_ADDR     (AON_RTC_BASE + 0x60u)
#define AON_WDT_TRIM0_ADDR     (AON_RTC_BASE + 0x84u)
#define AON_WDT_TRIM1_ADDR     (AON_RTC_BASE + 0x88u)
#define AON_CLK_CFG_ADDR       (AON_RTC_BASE + 0x90u) /* [11:4]=XTAL cap, [16:12]=RTC clk, [31]=busy */
#define AON_CLK_STAT_ADDR      (AON_RTC_BASE + 0x94u)
#define AON_PWR_CTL0_ADDR      (AON_RTC_BASE + 0xA0u) /* bit 28 = RTC pwr gate (0=on) */
#define AON_CLK_EN_ADDR        (AON_RTC_BASE + 0xA8u) /* bit 2 = RTC clk enable */
#define AON_WDT_PMU_RST_CMD_ADDR (AON_RTC_BASE + 0xBCu) /* WDT->PMU reset auto-write */
#define AON_PLL_CTRL_ADDR      (AON_RTC_BASE + 0xD8u) /* bit 2 = PLL0 LDO/power */
#define AON_WDT_RST_CTL_ADDR   AON_PLL_CTRL_ADDR
#define AON_BACKUP_ADDR(n)     (AON_RTC_BASE + 0x180u + 4u * (uint32_t)(n))

#define RTWI_CTRL_ADDR      (RTWI_BASE + 0x00u)
#define RTWI_CMD_ADDR       (RTWI_BASE + 0x04u) /* bit31=BUSY, bit30=1, [15:8]=reg, [7:0]=0x60/0x61 */
#define RTWI_WDATA_ADDR     (RTWI_BASE + 0x08u)
#define RTWI_RDATA_ADDR     (RTWI_BASE + 0x0Cu)
#define RTWI_STATUS_ADDR    (RTWI_BASE + 0x10u) /* [15:12] != 0 => error */

/* ============================================================================
 * SARADC Key Ladder (0x40096000, ch1 on PD0 mux=15)
 * ============================================================================ */
#define SARADC_CTRL_ADDR     (SARADC_BASE + 0x00u)
#define SARADC_START_ADDR    (SARADC_BASE + 0x04u)
#define SARADC_IE_ADDR       (SARADC_BASE + 0x0Cu)
#define SARADC_REG10_ADDR    (SARADC_BASE + 0x10u)
#define SARADC_REG18_ADDR    (SARADC_BASE + 0x18u)
#define SARADC_CH1_THR_ADDR  (SARADC_BASE + 0x30u)
#define SARADC_CH1_DATA_ADDR (SARADC_BASE + 0x34u) /* bits [11:0] continuous scan */

/* ============================================================================
 * 1-Bit SDIO / SDMMC Controller (0x40003000, PD5=CLK, PD4=CMD, PD6=D0)
 * ============================================================================ */
#define SDIO_CTRL_ADDR      (SDIO_BASE + 0x000u)
#define SDIO_CMD_ADDR       (SDIO_BASE + 0x004u)
#define SDIO_ARG_ADDR       (SDIO_BASE + 0x008u)
#define SDIO_BLKSIZ_ADDR    (SDIO_BASE + 0x00Cu)
#define SDIO_BYTCNT_ADDR    (SDIO_BASE + 0x010u)
#define SDIO_INTMASK_ADDR   (SDIO_BASE + 0x014u)
#define SDIO_RINTSTS_ADDR   (SDIO_BASE + 0x034u)
#define SDIO_RESP0_ADDR     (SDIO_BASE + 0x038u)
#define SDIO_RESP1_ADDR     (SDIO_BASE + 0x03Cu)
#define SDIO_RESP2_ADDR     (SDIO_BASE + 0x040u)
#define SDIO_RESP3_ADDR     (SDIO_BASE + 0x044u)
#define SDIO_STATUS_ADDR    (SDIO_BASE + 0x048u)
#define SDIO_FIFOTH_ADDR    (SDIO_BASE + 0x04Cu)
#define SDIO_TMOUT_ADDR      (SDIO_BASE + 0x064u)
#define SDIO_IDMAC_BMOD_ADDR (SDIO_BASE + 0x080u)
#define SDIO_PLDMND_ADDR     (SDIO_BASE + 0x084u)
#define SDIO_DBADDR_ADDR     (SDIO_BASE + 0x088u)
#define SDIO_IDSTS_ADDR      (SDIO_BASE + 0x08Cu)
#define SDIO_IDINTEN_ADDR    (SDIO_BASE + 0x090u)
#define SDIO_DSCADDR_ADDR    (SDIO_BASE + 0x094u)
#define SDIO_BUFADDR_ADDR    (SDIO_BASE + 0x098u)
#define SDIO_BMOD_ADDR       (SDIO_BASE + 0x100u)
#define SDIO_FIFO_ADDR       (SDIO_BASE + 0x200u)

#ifndef OPENFW_HOST_MMIO
#define LCDC_MCU_TIMING     REG32(LCDC_MCU_TIMING_ADDR)
#define LCDC_IF_CTRL        REG32(LCDC_IF_CTRL_ADDR)
#define LCDC_STATUS         REG32(LCDC_STATUS_ADDR)
#define LCDC_XFER_CTRL      REG32(LCDC_XFER_CTRL_ADDR)
#define LCDC_CMD_REG        REG32(LCDC_CMD_REG_ADDR)
#define LCDC_DATA_LEN       REG32(LCDC_DATA_LEN_ADDR)
#define LCDC_TX_DATA        REG32(LCDC_TX_DATA_ADDR)
#define LCDC_XBAR0          REG32(LCDC_XBAR0_ADDR)
#define LCDC_XBAR1          REG32(LCDC_XBAR1_ADDR)
#define PWM3_CTRL           REG32(PWM3_CTRL_ADDR)
#define PWM3_PERIOD_DUTY    REG32(PWM3_PERIOD_DUTY_ADDR)
#define UART0_DATA          REG32(UART0_DATA_ADDR)
#define UART0_CTRL          REG32(UART0_CTRL_ADDR)
#define UART0_STAT          REG32(UART0_STAT_ADDR)
#define UART0_IE            REG32(UART0_IE_ADDR)
#define UART0_REG18         REG32(UART0_REG18_ADDR)
#define UART0_REG1C         REG32(UART0_REG1C_ADDR)
#define UART0_REG20         REG32(UART0_REG20_ADDR)
#define AON_RTC_CTRL        REG32(AON_RTC_CTRL_ADDR)
#define RTC_DATE            REG32(RTC_DATE_ADDR)
#define RTC_TIME            REG32(RTC_TIME_ADDR)
#define RTC_ALARM0_VAL      REG32(RTC_ALARM0_VAL_ADDR)
#define RTC_ALARM0_CTRL     REG32(RTC_ALARM0_CTRL_ADDR)
#define RTC_ALARM1_TIME     REG32(RTC_ALARM1_TIME_ADDR)
#define RTC_ALARM1_CTRL     REG32(RTC_ALARM1_CTRL_ADDR)
#define AON_PWR_CTRL0       REG32(AON_PWR_CTRL0_ADDR)
#define AON_CLK_CFG         REG32(AON_CLK_CFG_ADDR)
#define AON_CLK_STAT        REG32(AON_CLK_STAT_ADDR)
#define AON_PWR_CTL0        REG32(AON_PWR_CTL0_ADDR)
#define AON_CLK_EN          REG32(AON_CLK_EN_ADDR)
#define AON_PLL_CTRL        REG32(AON_PLL_CTRL_ADDR)
#define AON_BACKUP(n)       REG32(AON_BACKUP_ADDR(n))
#define RTWI_CTRL           REG32(RTWI_CTRL_ADDR)
#define RTWI_CMD            REG32(RTWI_CMD_ADDR)
#define RTWI_WDATA          REG32(RTWI_WDATA_ADDR)
#define RTWI_RDATA          REG32(RTWI_RDATA_ADDR)
#define RTWI_STATUS         REG32(RTWI_STATUS_ADDR)
#define SARADC_CTRL         REG32(SARADC_CTRL_ADDR)
#define SARADC_START        REG32(SARADC_START_ADDR)
#define SARADC_IE           REG32(SARADC_IE_ADDR)
#define SARADC_REG10        REG32(SARADC_REG10_ADDR)
#define SARADC_REG18        REG32(SARADC_REG18_ADDR)
#define SARADC_CH1_THR      REG32(SARADC_CH1_THR_ADDR)
#define SARADC_CH1_DATA     REG32(SARADC_CH1_DATA_ADDR)
#define SDIO_CTRL           REG32(SDIO_CTRL_ADDR)
#define SDIO_CMD            REG32(SDIO_CMD_ADDR)
#define SDIO_ARG            REG32(SDIO_ARG_ADDR)
#define SDIO_BLKSIZ         REG32(SDIO_BLKSIZ_ADDR)
#define SDIO_BYTCNT         REG32(SDIO_BYTCNT_ADDR)
#define SDIO_INTMASK        REG32(SDIO_INTMASK_ADDR)
#define SDIO_RINTSTS        REG32(SDIO_RINTSTS_ADDR)
#define SDIO_RESP0          REG32(SDIO_RESP0_ADDR)
#define SDIO_RESP1          REG32(SDIO_RESP1_ADDR)
#define SDIO_RESP2          REG32(SDIO_RESP2_ADDR)
#define SDIO_RESP3          REG32(SDIO_RESP3_ADDR)
#define SDIO_STATUS         REG32(SDIO_STATUS_ADDR)
#define SDIO_FIFOTH         REG32(SDIO_FIFOTH_ADDR)
#define SDIO_TMOUT          REG32(SDIO_TMOUT_ADDR)
#define SDIO_BMOD           REG32(SDIO_BMOD_ADDR)
#define SDIO_FIFO           REG32(SDIO_FIFO_ADDR)
#endif

#define SDIO_INT_CMD_DONE   (1u << 2)
#define SDIO_INT_DTO        (1u << 3)
#define SDIO_INT_TXDR       (1u << 4)
#define SDIO_INT_RXDR       (1u << 5)
#define SDIO_INT_RCRC       (1u << 6)
#define SDIO_INT_DCRC       (1u << 7)
#define SDIO_INT_RTO        (1u << 8)
#define SDIO_INT_DRTO       (1u << 9)
#define SDIO_INT_SBE        (1u << 13)
#define SDIO_INT_EBE        (1u << 15)
#define SDIO_INT_DATA_ERRS  (SDIO_INT_RCRC | SDIO_INT_DCRC | SDIO_INT_RTO | SDIO_INT_DRTO | SDIO_INT_SBE | SDIO_INT_EBE)
#define SDIO_STAT_FIFO_EMP  (1u << 2)
#define SDIO_STAT_FIFO_FULL (1u << 3)
#define SDIO_CTRL_USE_IDMAC (1u << 25)
#define SDIO_IDMAC_BMOD_SWR (1u << 0)
#define SDIO_IDMAC_BMOD_FB  (1u << 1)
#define SDIO_IDMAC_BMOD_DE  (1u << 7)
#define SDIO_IDSTS_TI       (1u << 0)
#define SDIO_IDSTS_RI       (1u << 1)
#define SDIO_IDSTS_FBE      (1u << 2)
#define SDIO_IDSTS_DU       (1u << 4)
#define SDIO_IDSTS_CES      (1u << 5)
#define SDIO_IDSTS_NIS      (1u << 8)
#define SDIO_IDSTS_AIS      (1u << 9)

/* ============================================================================
 * Audio Codec (0x40009000) & DMAC (0x40001000)
 * ============================================================================ */
#define CODEC_ANA_CTRL0_ADDR     (AUDIO_CODEC_BASE + 0x000u)
#define CODEC_MICBIAS_ADDR       CODEC_ANA_CTRL0_ADDR /* bits [25:24] */
#define CODEC_AMIC01_PGA_ADDR    (AUDIO_CODEC_BASE + 0x008u) /* PGA gain bits [28:24] (0..31) */
#define CODEC_AMIC1_CTRL_ADDR    CODEC_AMIC01_PGA_ADDR
#define CODEC_AMIC2_CTRL_ADDR    (AUDIO_CODEC_BASE + 0x00Cu) /* PGA gain bits [14:8] (0..127) */
#define CODEC_ADC_CH_MUX_ADDR    (AUDIO_CODEC_BASE + 0x020u) /* bits [11:8] & [19:16] */
#define CODEC_ADC_MUX_ADDR       CODEC_ADC_CH_MUX_ADDR
#define CODEC_DAC2_ANA_VOL_ADDR  (AUDIO_CODEC_BASE + 0x02Cu)
#define CODEC_DAC01_ANA_VOL_ADDR (AUDIO_CODEC_BASE + 0x030u) /* [20:16]=DAC0 L, [28:24]=DAC1 R */
#define CODEC_DAC_ANA_CFG0_ADDR  (AUDIO_CODEC_BASE + 0x040u)
#define CODEC_DAC_ANA_CFG1_ADDR  (AUDIO_CODEC_BASE + 0x04Cu)
#define CODEC_DAC0_PWR_ADDR      (AUDIO_CODEC_BASE + 0x050u)
#define CODEC_DAC1_PWR_ADDR      (AUDIO_CODEC_BASE + 0x054u)
#define CODEC_DAC2_PWR_ADDR      (AUDIO_CODEC_BASE + 0x058u)
#define CODEC_DAC0_DC_ADDR       (AUDIO_CODEC_BASE + 0x064u)
#define CODEC_DAC1_DC_ADDR       (AUDIO_CODEC_BASE + 0x068u)
#define CODEC_ANA_MISC_ADDR      (AUDIO_CODEC_BASE + 0x0FCu)
#define CODEC_GLB_ADDR           (AUDIO_CODEC_BASE + 0x100u)
#define CODEC_DAC_FIFO_CTRL_ADDR (AUDIO_CODEC_BASE + 0x108u)
#define CODEC_DAC_TX_FIFO_ADDR   (AUDIO_CODEC_BASE + 0x10Cu)
#define CODEC_DAC_DIG_CFG_ADDR   (AUDIO_CODEC_BASE + 0x110u)
#define CODEC_DAC_FIFO_RST_ADDR  (AUDIO_CODEC_BASE + 0x114u)
#define CODEC_DAC0_DIG_EN_ADDR   (AUDIO_CODEC_BASE + 0x130u)
#define CODEC_DAC0_MUTE_ADDR     (AUDIO_CODEC_BASE + 0x134u)
#define CODEC_DAC0_VOL_ADDR      (AUDIO_CODEC_BASE + 0x138u)
#define CODEC_DAC1_DIG_EN_ADDR   (AUDIO_CODEC_BASE + 0x150u)
#define CODEC_DAC1_MUTE_ADDR     (AUDIO_CODEC_BASE + 0x154u)
#define CODEC_DAC1_VOL_ADDR      (AUDIO_CODEC_BASE + 0x158u)
#define CODEC_DAC2_DIG_EN_ADDR   (AUDIO_CODEC_BASE + 0x170u)
#define CODEC_DAC2_MUTE_ADDR     (AUDIO_CODEC_BASE + 0x174u)
#define CODEC_DAC2_VOL_ADDR      (AUDIO_CODEC_BASE + 0x178u)
#define CODEC_ADC_DIG_CTRL_ADDR  (AUDIO_CODEC_BASE + 0x200u)
#define CODEC_ADC_FIFO_CTRL_ADDR (AUDIO_CODEC_BASE + 0x208u)
#define CODEC_ADC_RX_FIFO_ADDR   (AUDIO_CODEC_BASE + 0x20Cu)
#define CODEC_ADC_CH_CTRL_ADDR(ch) (AUDIO_CODEC_BASE + 0x220u + (uint32_t)(ch) * 0x30u)
#define CODEC_ADC_CH_GAIN_ADDR(ch) (AUDIO_CODEC_BASE + 0x228u + (uint32_t)(ch) * 0x30u)
#define CODEC_SRAM_START         (AUDIO_CODEC_BASE + 0x400u)
#define CODEC_SRAM_END           (AUDIO_CODEC_BASE + 0x600u)

#define DMAC_CH_BASE(ch)          (DMAC_BASE + ((uint32_t)(ch) * 0x40u))
#define DMAC_CH_CTL_ADDR(ch)      (DMAC_CH_BASE(ch) + 0x00u)
#define DMAC_CH_SRC_ADDR(ch)      (DMAC_CH_BASE(ch) + 0x04u)
#define DMAC_CH_DST_ADDR(ch)      (DMAC_CH_BASE(ch) + 0x08u)
#define DMAC_CH_XFER_CNT_ADDR(ch) (DMAC_CH_BASE(ch) + 0x0Cu)
#define DMAC_CH_IRQ_EN_ADDR(ch)   (DMAC_CH_BASE(ch) + 0x20u)
#define DMAC_CH_IRQ_STAT_ADDR(ch) (DMAC_CH_BASE(ch) + 0x24u)

#ifndef OPENFW_HOST_MMIO
#define CODEC_MICBIAS         REG32(CODEC_MICBIAS_ADDR)
#define CODEC_ANA_CTRL0       REG32(CODEC_ANA_CTRL0_ADDR)
#define CODEC_AMIC1_CTRL      REG32(CODEC_AMIC1_CTRL_ADDR)
#define CODEC_AMIC01_PGA      REG32(CODEC_AMIC01_PGA_ADDR)
#define CODEC_AMIC2_CTRL      REG32(CODEC_AMIC2_CTRL_ADDR)
#define CODEC_ADC_MUX         REG32(CODEC_ADC_MUX_ADDR)
#define CODEC_ADC_CH_MUX      REG32(CODEC_ADC_CH_MUX_ADDR)
#define CODEC_ANA_MISC        REG32(CODEC_ANA_MISC_ADDR)
#define CODEC_GLB             REG32(CODEC_GLB_ADDR)
#define CODEC_DAC0_MUTE       REG32(CODEC_DAC0_MUTE_ADDR)
#define CODEC_DAC0_VOL        REG32(CODEC_DAC0_VOL_ADDR)
#define CODEC_DAC1_MUTE       REG32(CODEC_DAC1_MUTE_ADDR)
#define CODEC_DAC1_VOL        REG32(CODEC_DAC1_VOL_ADDR)
#define CODEC_DAC2_MUTE       REG32(CODEC_DAC2_MUTE_ADDR)
#define CODEC_DAC2_VOL        REG32(CODEC_DAC2_VOL_ADDR)
#define CODEC_ADC_DIG_CTRL    REG32(CODEC_ADC_DIG_CTRL_ADDR)
#define CODEC_ADC_FIFO_CTRL   REG32(CODEC_ADC_FIFO_CTRL_ADDR)
#define CODEC_ADC_RX_FIFO     REG32(CODEC_ADC_RX_FIFO_ADDR)
#define CODEC_ADC_CH_CTRL(ch) REG32(CODEC_ADC_CH_CTRL_ADDR(ch))
#define CODEC_ADC_CH_GAIN(ch) REG32(CODEC_ADC_CH_GAIN_ADDR(ch))

#define DMAC_CH_CTL(ch)       REG32(DMAC_CH_CTL_ADDR(ch))
#define DMAC_CH_SRC(ch)       REG32(DMAC_CH_SRC_ADDR(ch))
#define DMAC_CH_DST(ch)       REG32(DMAC_CH_DST_ADDR(ch))
#define DMAC_CH_XFER_CNT(ch)  REG32(DMAC_CH_XFER_CNT_ADDR(ch))
#define DMAC_CH_IRQ_EN(ch)    REG32(DMAC_CH_IRQ_EN_ADDR(ch))
#define DMAC_CH_IRQ_STAT(ch)  REG32(DMAC_CH_IRQ_STAT_ADDR(ch))
#endif

#define DMAC_REQ_AUDIO_ADC_RX 1u
#define DMAC_CTL_ADC_RX_BASE  0x002A0281u /* SRC=ADC_RX(1) fixed 32b b4 -> DST=MEM inc 32b b4 */
#define DMAC_CTL_DAC_TX_BASE  0x002812A0u /* SRC=MEM inc 32b b1 -> DST=DAC_TX(1) fixed 32b b1 */
#define DMAC_CTL_CH_EN        (1u << 30)
#define DMAC_CTL_RELOAD       (1u << 29)
#define DMAC_IRQ_WM           (1u << 0)
#define DMAC_IRQ_TC           (1u << 1)
#define DMAC_AUDIO_IRQ_EN     ((32u << 14) | DMAC_IRQ_TC)

/* ============================================================================
 * Mentor MUSB USB 2.0 OTG Controller (0x40040000) & USB PHY Wrapper
 * ============================================================================ */
#define MUSB_FADDR_ADDR     (MUSB_BASE + 0x00u)
#define MUSB_POWER_ADDR     (MUSB_BASE + 0x01u)
#define MUSB_INTRTX_ADDR    (MUSB_BASE + 0x02u) /* read-to-clear */
#define MUSB_INTRRX_ADDR    (MUSB_BASE + 0x04u) /* read-to-clear */
#define MUSB_INTRTXE_ADDR   (MUSB_BASE + 0x06u)
#define MUSB_INTRRXE_ADDR   (MUSB_BASE + 0x08u)
#define MUSB_INTRUSB_ADDR   (MUSB_BASE + 0x0Au)  /* read-to-clear */
#define MUSB_INTRUSBE_ADDR  (MUSB_BASE + 0x0Bu)
#define MUSB_FRAME_ADDR     (MUSB_BASE + 0x0Cu)
#define MUSB_INDEX_ADDR     (MUSB_BASE + 0x0Eu)
#define MUSB_TESTMODE_ADDR  (MUSB_BASE + 0x0Fu)
#define MUSB_TXMAXP_ADDR    (MUSB_BASE + 0x10u)
#define MUSB_CSR0_ADDR      (MUSB_BASE + 0x12u)
#define MUSB_TXCSR_ADDR     (MUSB_BASE + 0x12u)
#define MUSB_RXMAXP_ADDR    (MUSB_BASE + 0x14u)
#define MUSB_RXCSR_ADDR     (MUSB_BASE + 0x16u)
#define MUSB_COUNT0_ADDR    (MUSB_BASE + 0x18u)
#define MUSB_RXCOUNT_ADDR   (MUSB_BASE + 0x18u)
#define MUSB_DEVCTL_ADDR    (MUSB_BASE + 0x60u)
#define MUSB_FIFO_ADDR(ep)  (MUSB_BASE + 0x20u + 4u * (uint32_t)(ep))

#define USB_PHY_CTRL_ADDR       (MUSB_BASE + 0x300u) /* bit 12 = Device mode */
#define USB_PHY_INTE_ADDR       (MUSB_BASE + 0x304u) /* bits [1:0] = VBUS/plug IRQ EN */
#define USB_PHY_INT_STATUS_ADDR (MUSB_BASE + 0x308u) /* bit 0 = USB host attach (W1C) */

#ifndef OPENFW_HOST_MMIO
#define MUSB_FADDR          REG8(MUSB_FADDR_ADDR)
#define MUSB_POWER          REG8(MUSB_POWER_ADDR)
#define MUSB_INTRTX         REG16(MUSB_INTRTX_ADDR)
#define MUSB_INTRRX         REG16(MUSB_INTRRX_ADDR)
#define MUSB_INTRTXE        REG16(MUSB_INTRTXE_ADDR)
#define MUSB_INTRRXE        REG16(MUSB_INTRRXE_ADDR)
#define MUSB_INTRUSB        REG8(MUSB_INTRUSB_ADDR)
#define MUSB_INTRUSBE       REG8(MUSB_INTRUSBE_ADDR)
#define MUSB_FRAME          REG16(MUSB_FRAME_ADDR)
#define MUSB_INDEX          REG8(MUSB_INDEX_ADDR)
#define MUSB_TESTMODE       REG8(MUSB_TESTMODE_ADDR)
#define MUSB_TXMAXP         REG16(MUSB_TXMAXP_ADDR)
#define MUSB_CSR0           REG16(MUSB_CSR0_ADDR)
#define MUSB_TXCSR          REG16(MUSB_TXCSR_ADDR)
#define MUSB_RXMAXP         REG16(MUSB_RXMAXP_ADDR)
#define MUSB_RXCSR          REG16(MUSB_RXCSR_ADDR)
#define MUSB_COUNT0         REG16(MUSB_COUNT0_ADDR)
#define MUSB_RXCOUNT        REG16(MUSB_RXCOUNT_ADDR)
#define MUSB_DEVCTL         REG8(MUSB_DEVCTL_ADDR)
#define USB_PHY_CTRL        REG32(USB_PHY_CTRL_ADDR)
#define USB_PHY_INTE        REG32(USB_PHY_INTE_ADDR)
#define USB_PHY_INT_STATUS  REG32(USB_PHY_INT_STATUS_ADDR)
#endif

/* ============================================================================
 * eFuse / OTP (0x40099000) & BT RF / Modem (0x40010000 / 0x40011000)
 * ============================================================================ */
#define EFUSE_WORD_ADDR(n)  (EFUSE_BASE + 0x20u + 4u * (uint32_t)(n))
#define RF_BB_CTRL_ADDR     (RF_BB_BASE + 0x000u) /* bit 26 = RF config mode */
#define RF_LUT_CTRL_ADDR    (RF_ANA_BASE + 0x004u)
#define RF_LUT_DATA_ADDR    (RF_ANA_BASE + 0x008u)

#ifndef OPENFW_HOST_MMIO
#define EFUSE_WORD(n)       REG32(EFUSE_WORD_ADDR(n))
#define RF_BB_CTRL          REG32(RF_BB_CTRL_ADDR)
#define RF_LUT_CTRL         REG32(RF_LUT_CTRL_ADDR)
#define RF_LUT_DATA         REG32(RF_LUT_DATA_ADDR)
#endif

/* ============================================================================
 * RivieraWaves RW-BT Baseband Controller (0x40020000)
 * ============================================================================ */
#ifndef OPENFW_HOST_MMIO
#define RWBT_REG(off)       REG32(RWBT_BB_BASE + (uint32_t)(off))
#define RWBT_CNTL           REG32(RWBT_BB_BASE + 0x000u)
#define RWBT_INTCNTL        REG32(RWBT_BB_BASE + 0x00Cu)
#define RWBT_INTSTAT        REG32(RWBT_BB_BASE + 0x010u)
#define RWBT_INTACK         REG32(RWBT_BB_BASE + 0x018u)
#define RWBT_TIMING         REG32(RWBT_BB_BASE + 0x080u)

/* ============================================================================
 * IPC Mailbox, Spinlocks & CPU1 Boot Mailbox
 * ============================================================================ */

#define MBOX_CPU0_IRQ_STAT  REG32(IPC_MBOX_BASE + 0x24u)
#define MBOX_CPU1_IRQ_STAT  REG32(IPC_MBOX_BASE + 0x2Cu)
#define MBOX_CH_CNT(ch)     REG32(IPC_MBOX_BASE + 0x44u + (uint32_t)(ch) * 0x20u)
#define MBOX_CH_DATA(ch)    REG32(IPC_MBOX_BASE + 0x48u + (uint32_t)(ch) * 0x20u)

#define SPINLOCK_REG(id)    REG32(SPINLOCK_BASE + (uint32_t)(id) * 0x10u)

#define CPU1_BOOT_TRAMP     REG32(0x30000600u)
#define CPU1_BOOT_SP        REG32(0x30000604u)
#define CPU1_BOOT_ENTRY     REG32(0x30000608u)
#endif

#endif /* SL6801_REGS_H */
