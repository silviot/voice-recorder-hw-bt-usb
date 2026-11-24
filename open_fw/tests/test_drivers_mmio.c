/*
 * MMIO-Hooked Host Unit Tests for Cassini OpenFW Bare-Metal Drivers
 *
 * Compiles the real ARM driver sources natively under -DOPENFW_HOST_MMIO and
 * drives them against deterministic virtual SL6801 peripheral models
 * (PPB/US_TIMER/WDT, UART0+g_openfw_log, AON RTC, PMU RTWI, SARADC Keypad,
 * ST7735S LCDC+PWM3 framebuffer, 1-bit SDIO/microSD+exFAT, Mentor MUSB+SCSI).
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_exp.h"
#include "openfw_fs.h"
#include "openfw_usb_msc.h"

/* ============================================================================
 * Virtual SoC State & MMIO / ROM Hooks
 * ============================================================================ */
volatile uint32_t g_systick_ms = 0u;

static jmp_buf      g_wfi_jmp;
static volatile int g_wfi_armed = 0;

static struct {
    /* Core PPB & System */
    uint32_t systick_csr;
    uint32_t systick_rvr;
    uint32_t systick_cvr;
    uint32_t scb_aircr;
    uint32_t dsb_count;
    uint32_t wfi_count;
    uint64_t us_time;
    int      us_rollover_inject;
    uint32_t wdt_feeds;
    uint32_t last_wdt_key;
    uint32_t wdt_en;
    uint32_t wdt_mode;
    uint32_t wdt_cfg;
    uint32_t wdt_ctrl1;
    uint32_t aon_wdt_trim0;
    uint32_t aon_wdt_trim1;
    uint32_t aon_wdt_pmu_rst_cmd;
    uint32_t aon_pll_ctrl;

    /* UART0 */
    uint32_t uart0_ctrl;
    uint32_t uart0_ie;
    uint32_t uart0_reg18;
    uint32_t uart0_stat;
    char     uart0_tx[8192];
    uint32_t uart0_tx_len;

    /* AON RTC */
    uint32_t aon_clk_cfg;
    uint32_t aon_clk_en;
    uint32_t aon_pwr_ctl0;
    uint32_t aon_rtc_ctrl;
    uint32_t rtc_date;
    uint32_t rtc_time;
    uint32_t rtc_alarm0_val;
    uint32_t rtc_alarm0_ctrl;
    int      rtc_rollover_inject;
    uint32_t aon_backup[16];

    /* PMU RTWI */
    uint32_t rtwi_cmd;
    uint32_t rtwi_wdata;
    uint32_t rtwi_rdata;
    uint32_t rtwi_status;
    uint8_t  pmu_regs[256];

    /* GPIO / SARADC / CRU */
    uint32_t gpio_mux[4][4];
    uint32_t gpio_out[4];
    uint32_t gpio_in[4];
    uint32_t gpio_pad_dir[4];
    uint32_t gpio_lcd_pad_en;
    uint32_t cru_saradc_clk;
    uint32_t cru_saradc_rst;
    uint32_t saradc_ctrl;
    uint32_t saradc_start;
    uint32_t saradc_ch1_thr;
    uint32_t saradc_ch1_data;
    uint32_t rom_rst_released_mask;

    /* PWM3 + LCDC + Virtual ST7735S 80x160 RGB565 Panel */
    uint32_t pwm3_ctrl;
    uint32_t pwm3_period_duty;
    uint32_t lcdc_xbar0;
    uint32_t lcdc_xbar1;
    uint32_t lcdc_if_ctrl;
    uint32_t lcdc_status;
    uint32_t lcdc_xfer_ctrl;
    uint32_t lcdc_cmd_reg;
    uint32_t lcdc_data_len;
    uint32_t lcdc_tx_data;
    uint8_t  st_cur_cmd;
    uint8_t  st_param_idx;
    uint16_t st_x0, st_x1, st_y0, st_y1;
    uint16_t st_cx, st_cy;
    uint8_t  st_madctl;
    uint8_t  st_colmod;
    uint8_t  st_invon;
    uint8_t  st_disp_on;
    uint8_t  st_ram_half;
    uint8_t  st_hi_byte;
    uint16_t st_fb[160][160];

    /* SDIO Controller + Virtual 8 MiB SDHC Card */
    uint32_t sdio_ctrl;
    uint32_t sdio_cmd;
    uint32_t sdio_arg;
    uint32_t sdio_blksiz;
    uint32_t sdio_bytcnt;
    uint32_t sdio_rintsts;
    uint32_t sdio_resp[4];
    uint32_t sdio_fifoth;
    uint32_t sdio_tmout;
    uint32_t sdio_bmod;
    uint32_t sdio_intmask;
    uint32_t sdio_fifo[128];
    uint32_t sdio_fifo_pos;
    uint32_t sdio_fifo_len;
    uint32_t sdio_active_lba;
    uint32_t sdio_blocks_rem;
    int      sdio_app_cmd;
    int      sdio_acmd41_polls;
    int      sdio_selected;
    uint32_t sdio_last_cmd13;
    uint32_t sdio_cmd17_cnt;
    uint32_t sdio_cmd18_cnt;
    uint32_t sdio_cmd24_cnt;
    uint32_t sdio_cmd25_cnt;
    uint8_t  *sd_sectors;
    uint32_t sd_num_sectors;

    /* USB PHY + Mentor MUSB OTG Core */
    uint32_t musb_ep0_stalls;
    uint32_t cru_clk_src_gate;
    uint32_t cru_rst_set1;
    uint32_t cru_rst_stat1;
    uint32_t cru_clk_en_usb;
    uint32_t sysctl_usb_ctrl;
    uint32_t usb_phy_ctrl;
    uint32_t usb_phy_inte;
    uint32_t usb_phy_int_status;

    uint8_t  musb_faddr;
    uint8_t  musb_power;
    uint16_t musb_intrtx;
    uint16_t musb_intrrx;
    uint16_t musb_intrtxe;
    uint16_t musb_intrrxe;
    uint8_t  musb_intrusb;
    uint8_t  musb_intrusbe;
    uint16_t musb_frame;
    uint8_t  musb_index;
    uint8_t  musb_testmode;
    uint16_t musb_txmaxp[4];
    uint16_t musb_rxmaxp[4];
    uint16_t musb_txcsr[4]; /* ep0: CSR0 */
    uint16_t musb_rxcsr[4];
    uint16_t musb_rxcount[4]; /* ep0: COUNT0 */
    uint8_t  musb_rx_fifo[4][512];
    uint16_t musb_rx_pos[4];
    uint16_t musb_rx_len[4];
    uint8_t  musb_tx_fifo[4][512];
    uint16_t musb_tx_len[4];
} g_soc;

static void soc_reset(void)
{
    uint8_t *saved_sd = g_soc.sd_sectors;
    uint32_t saved_n  = g_soc.sd_num_sectors;
    memset(&g_soc, 0, sizeof(g_soc));
    g_soc.sd_sectors     = saved_sd;
    g_soc.sd_num_sectors = saved_n;
    g_soc.uart0_stat     = 16u;
    g_soc.aon_pwr_ctl0   = (1u << 28);
    g_soc.aon_rtc_ctrl   = (1u << 4) | (1u << 8);
    g_soc.saradc_ch1_data = 4095u;
    g_soc.gpio_in[1]     = (1u << 6) | (1u << 11); /* Rec switch & PB11 HP jack inactive */
    g_soc.st_x1          = LCD_WIDTH - 1u;
    g_soc.st_y1          = LCD_HEIGHT - 1u;
}

static void st7735s_on_byte(int is_data, uint8_t b)
{
    if (!is_data) {
        g_soc.st_cur_cmd   = b;
        g_soc.st_param_idx = 0u;
        if (b == 0x21u) g_soc.st_invon = 1u;
        if (b == 0x29u) g_soc.st_disp_on = 1u;
        if (b == 0x2Cu) {
            g_soc.st_cx       = g_soc.st_x0;
            g_soc.st_cy       = g_soc.st_y0;
            g_soc.st_ram_half = 0u;
        }
        return;
    }

    switch (g_soc.st_cur_cmd) {
    case 0x3Au:
        g_soc.st_colmod = b;
        break;
    case 0x36u:
        g_soc.st_madctl = b;
        break;
    case 0x2Au:
    {
        uint16_t x_off = (g_soc.st_madctl & 0x20u) ? 1u : 26u;
        if (g_soc.st_param_idx == 1u) g_soc.st_x0 = (uint16_t)(b - x_off);
        if (g_soc.st_param_idx == 3u) g_soc.st_x1 = (uint16_t)(b - x_off);
        g_soc.st_param_idx++;
        break;
    }
    case 0x2Bu:
    {
        uint16_t y_off = (g_soc.st_madctl & 0x20u) ? 26u : 1u;
        if (g_soc.st_param_idx == 1u) g_soc.st_y0 = (uint16_t)(b - y_off);
        if (g_soc.st_param_idx == 3u) g_soc.st_y1 = (uint16_t)(b - y_off);
        g_soc.st_param_idx++;
        break;
    }
    case 0x2Cu:
        if (g_soc.st_ram_half == 0u) {
            g_soc.st_hi_byte  = b;
            g_soc.st_ram_half = 1u;
        } else {
            uint16_t px = ((uint16_t)g_soc.st_hi_byte << 8) | b;
            if (g_soc.st_cx < 160u && g_soc.st_cy < 160u) {
                g_soc.st_fb[g_soc.st_cy][g_soc.st_cx] = px;
            }
            if (g_soc.st_cx < g_soc.st_x1) {
                g_soc.st_cx++;
            } else {
                g_soc.st_cx = g_soc.st_x0;
                if (g_soc.st_cy < g_soc.st_y1) {
                    g_soc.st_cy++;
                } else {
                    g_soc.st_cy = g_soc.st_y0;
                }
            }
            g_soc.st_ram_half = 0u;
        }
        break;
    default:
        g_soc.st_param_idx++;
        break;
    }
}

static void sdio_exec_cmd(uint32_t val)
{
    uint32_t cmd = val & 0x3Fu;
    g_soc.sdio_cmd = val & ~0x80000000u; /* controller clears START */
    if (val & 0x00200000u) {
        return; /* update clock only */
    }
    g_soc.sdio_rintsts |= SDIO_INT_CMD_DONE;

    switch (cmd) {
    case 0u: /* CMD0 GO_IDLE */
        g_soc.sdio_selected     = 0;
        g_soc.sdio_app_cmd      = 0;
        g_soc.sdio_acmd41_polls = 0;
        break;
    case 8u: /* CMD8 IF_COND */
        g_soc.sdio_resp[0] = g_soc.sdio_arg;
        break;
    case 55u: /* CMD55 APP_CMD */
        g_soc.sdio_app_cmd = 1;
        g_soc.sdio_resp[0] = 0x00000120u;
        break;
    case 41u: /* ACMD41 */
        g_soc.sdio_app_cmd = 0;
        g_soc.sdio_acmd41_polls++;
        g_soc.sdio_resp[0] = (g_soc.sdio_acmd41_polls >= 2) ? 0xC0FF8000u : 0x00FF8000u;
        break;
    case 2u: /* CMD2 ALL_SEND_CID */
        g_soc.sdio_resp[0] = 0x11223344u;
        g_soc.sdio_resp[1] = 0x55667788u;
        g_soc.sdio_resp[2] = 0x99AABBCBu;
        g_soc.sdio_resp[3] = 0x00534400u;
        break;
    case 3u: /* CMD3 SEND_REL_ADDR */
        g_soc.sdio_resp[0] = 0x12340000u;
        break;
    case 9u: /* CMD9 SEND_CSD (SDv2: c_size = sectors/1024 - 1) */
    {
        uint32_t c_size = (g_soc.sd_num_sectors / 1024u) - 1u;
        g_soc.sdio_resp[3] = (1u << 30);
        g_soc.sdio_resp[2] = (c_size >> 16) & 0x3Fu;
        g_soc.sdio_resp[1] = (c_size & 0xFFFFu) << 16;
        g_soc.sdio_resp[0] = 0u;
        break;
    }
    case 7u: /* CMD7 SELECT_CARD */
        g_soc.sdio_selected = 1;
        g_soc.sdio_resp[0] = 0x00000700u;
        break;
    case 12u: /* CMD12 STOP_TRANSMISSION */
        g_soc.sdio_blocks_rem = 0u;
        g_soc.sdio_resp[0] = 0x00000900u;
        break;
    case 13u: /* CMD13 SEND_STATUS */
        g_soc.sdio_last_cmd13 = val;
        g_soc.sdio_resp[0] = 0x00000900u; /* READY_FOR_DATA | TRAN */
        break;
    case 17u: /* CMD17 READ_SINGLE_BLOCK */
    case 18u: /* CMD18 READ_MULTIPLE_BLOCK (finite or BYTCNT=0 open-ended) */
    {
        if (cmd == 17u) g_soc.sdio_cmd17_cnt++; else g_soc.sdio_cmd18_cnt++;
        uint32_t lba = g_soc.sdio_arg;
        uint32_t blks = (cmd == 18u)
            ? ((g_soc.sdio_bytcnt == 0u) ? 0xFFFFFFFFu : (g_soc.sdio_bytcnt / 512u))
            : 1u;
        assert(g_soc.sdio_selected);
        assert(lba < g_soc.sd_num_sectors);
        if (blks != 0xFFFFFFFFu) {
            assert(blks >= 1u && lba + blks <= g_soc.sd_num_sectors);
        }
        const uint8_t *src = &g_soc.sd_sectors[lba * 512u];
        for (uint32_t w = 0u; w < 128u; w++) {
            g_soc.sdio_fifo[w] = (uint32_t)src[w * 4u + 0u]
                               | ((uint32_t)src[w * 4u + 1u] << 8)
                               | ((uint32_t)src[w * 4u + 2u] << 16)
                               | ((uint32_t)src[w * 4u + 3u] << 24);
        }
        g_soc.sdio_active_lba = lba;
        g_soc.sdio_blocks_rem = (blks == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (blks - 1u);
        g_soc.sdio_fifo_pos   = 0u;
        g_soc.sdio_fifo_len   = 128u;
        if (g_soc.sdio_blocks_rem == 0u) {
            g_soc.sdio_rintsts |= SDIO_INT_DTO;
        }
        break;
    }
    case 24u: /* CMD24 WRITE_BLOCK */
    case 25u: /* CMD25 WRITE_MULTIPLE_BLOCK */
    {
        if (cmd == 24u) g_soc.sdio_cmd24_cnt++; else g_soc.sdio_cmd25_cnt++;
        uint32_t blks = (cmd == 25u) ? (g_soc.sdio_bytcnt / 512u) : 1u;
        assert(g_soc.sdio_selected);
        assert(blks >= 1u && g_soc.sdio_arg + blks <= g_soc.sd_num_sectors);
        g_soc.sdio_active_lba = g_soc.sdio_arg;
        g_soc.sdio_blocks_rem = blks;
        g_soc.sdio_fifo_pos   = 0u;
        g_soc.sdio_fifo_len   = 128u;
        break;
    }
    default:
        break;
    }
}

uint32_t openfw_mmio_rd32(uint32_t addr)
{
    if (addr == SYSTICK_CSR_ADDR) return g_soc.systick_csr;
    if (addr == SYSTICK_RVR_ADDR) return g_soc.systick_rvr;
    if (addr == SYSTICK_CVR_ADDR) return g_soc.systick_cvr;

    if (addr == US_TIMER_HI_ADDR) {
        if (g_soc.us_rollover_inject == 1) {
            g_soc.us_rollover_inject = 2;
            return (uint32_t)(g_soc.us_time >> 32);
        }
        if (g_soc.us_rollover_inject == 2) {
            g_soc.us_rollover_inject = 0;
            g_soc.us_time += 0x100000000ULL;
            g_soc.us_time &= 0xFFFFFFFF00000000ULL;
        }
        return (uint32_t)(g_soc.us_time >> 32);
    }
    if (addr == US_TIMER_LO_ADDR) {
        return (uint32_t)(g_soc.us_time & 0xFFFFFFFFu);
    }

    if (addr == UART0_CTRL_ADDR) return g_soc.uart0_ctrl;
    if (addr == UART0_IE_ADDR)   return g_soc.uart0_ie;
    if (addr == UART0_REG18_ADDR) return g_soc.uart0_reg18;
    if (addr == UART0_STAT_ADDR) return g_soc.uart0_stat;

    if (addr == WDT_EN_ADDR)       return g_soc.wdt_en;
    if (addr == WDT_MODE_ADDR)     return g_soc.wdt_mode;
    if (addr == WDT_CFG_ADDR)      return g_soc.wdt_cfg;
    if (addr == WDT_CTRL1_ADDR)    return g_soc.wdt_ctrl1;
    if (addr == AON_WDT_TRIM0_ADDR) return g_soc.aon_wdt_trim0;
    if (addr == AON_WDT_TRIM1_ADDR) return g_soc.aon_wdt_trim1;
    if (addr == AON_WDT_PMU_RST_CMD_ADDR) return g_soc.aon_wdt_pmu_rst_cmd;
    if (addr == AON_PLL_CTRL_ADDR) return g_soc.aon_pll_ctrl;

    if (addr == AON_CLK_CFG_ADDR)  return g_soc.aon_clk_cfg;
    if (addr == AON_CLK_STAT_ADDR) return 0u;
    if (addr == AON_CLK_EN_ADDR)   return g_soc.aon_clk_en;
    if (addr == AON_PWR_CTL0_ADDR) return g_soc.aon_pwr_ctl0;
    if (addr == AON_RTC_CTRL_ADDR) return g_soc.aon_rtc_ctrl;
    if (addr >= AON_BACKUP_ADDR(0u) && addr <= AON_BACKUP_ADDR(15u)) {
        return g_soc.aon_backup[(addr - AON_BACKUP_ADDR(0u)) >> 2];
    }
    if (addr == RTC_DATE_ADDR) {
        if (g_soc.rtc_rollover_inject == 1) {
            g_soc.rtc_rollover_inject = 2;
            return g_soc.rtc_date;
        }
        if (g_soc.rtc_rollover_inject == 2) {
            g_soc.rtc_rollover_inject = 0;
            g_soc.rtc_date++;
            g_soc.rtc_time = 0u;
        }
        return g_soc.rtc_date;
    }
    if (addr == RTC_TIME_ADDR) return g_soc.rtc_time;

    if (addr == RTWI_CMD_ADDR)    return g_soc.rtwi_cmd;
    if (addr == RTWI_RDATA_ADDR)  return g_soc.rtwi_rdata;
    if (addr == RTWI_STATUS_ADDR) return g_soc.rtwi_status;

    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u) {
        uint32_t port = (addr - GPIO_BASE) >> 6;
        uint32_t off  = (addr - GPIO_BASE) & 0x3Fu;
        if (off < 0x10u)  return g_soc.gpio_mux[port][off >> 2];
        if (off == 0x10u) return g_soc.gpio_in[port];
        if (off == 0x24u) return g_soc.gpio_pad_dir[port];
    }
    if (addr == GPIO_LCD_PAD_EN_ADDR) return g_soc.gpio_lcd_pad_en;

    if (addr == CRU_SARADC_CLK_ADDR)  return g_soc.cru_saradc_clk;
    if (addr == CRU_SARADC_RST_ADDR)  return g_soc.cru_saradc_rst;
    if (addr == SARADC_CH1_DATA_ADDR) return g_soc.saradc_ch1_data;

    if (addr == PWM3_CTRL_ADDR)       return g_soc.pwm3_ctrl;
    if (addr == PWM3_PERIOD_DUTY_ADDR) return g_soc.pwm3_period_duty;
    if (addr == LCDC_STATUS_ADDR)     return g_soc.lcdc_status;
    if (addr == LCDC_XFER_CTRL_ADDR)  return g_soc.lcdc_xfer_ctrl;
    if (addr == LCDC_IF_CTRL_ADDR)    return g_soc.lcdc_if_ctrl;

    if (addr == SDIO_CTRL_ADDR)    return g_soc.sdio_ctrl;
    if (addr == SDIO_CMD_ADDR)     return g_soc.sdio_cmd;
    if (addr == SDIO_RINTSTS_ADDR) return g_soc.sdio_rintsts;
    if (addr == SDIO_RESP0_ADDR)   return g_soc.sdio_resp[0];
    if (addr == SDIO_RESP1_ADDR)   return g_soc.sdio_resp[1];
    if (addr == SDIO_RESP2_ADDR)   return g_soc.sdio_resp[2];
    if (addr == SDIO_RESP3_ADDR)   return g_soc.sdio_resp[3];
    if (addr == SDIO_BMOD_ADDR)    return g_soc.sdio_bmod;
    if (addr == SDIO_STATUS_ADDR) {
        return (g_soc.sdio_fifo_pos >= g_soc.sdio_fifo_len) ? SDIO_STAT_FIFO_EMP : 0u;
    }
    if (addr == SDIO_FIFO_ADDR) {
        assert(g_soc.sdio_fifo_pos < g_soc.sdio_fifo_len);
        uint32_t word = g_soc.sdio_fifo[g_soc.sdio_fifo_pos++];
        if (g_soc.sdio_fifo_pos == g_soc.sdio_fifo_len && g_soc.sdio_blocks_rem > 0u) {
            g_soc.sdio_active_lba++;
            if (g_soc.sdio_blocks_rem != 0xFFFFFFFFu) {
                g_soc.sdio_blocks_rem--;
            }
            if (g_soc.sdio_active_lba < g_soc.sd_num_sectors) {
                const uint8_t *src = &g_soc.sd_sectors[g_soc.sdio_active_lba * 512u];
                for (uint32_t w = 0u; w < 128u; w++) {
                    g_soc.sdio_fifo[w] = (uint32_t)src[w * 4u + 0u]
                                       | ((uint32_t)src[w * 4u + 1u] << 8)
                                       | ((uint32_t)src[w * 4u + 2u] << 16)
                                       | ((uint32_t)src[w * 4u + 3u] << 24);
                }
                g_soc.sdio_fifo_pos = 0u;
                g_soc.sdio_fifo_len = 128u;
            }
            if (g_soc.sdio_blocks_rem == 0u) {
                g_soc.sdio_rintsts |= SDIO_INT_DTO;
            }
        }
        return word;
    }

    if (addr == CRU_CLK_SRC_GATE_ADDR) return g_soc.cru_clk_src_gate;
    if (addr == CRU_RST_SET1_ADDR)     return g_soc.cru_rst_set1;
    if (addr == CRU_RST_STAT1_ADDR)    return g_soc.cru_rst_stat1;
    if (addr == CRU_CLK_EN_USB_ADDR)   return g_soc.cru_clk_en_usb;
    if (addr == SYSCTL_USB_CTRL_ADDR)  return g_soc.sysctl_usb_ctrl;
    if (addr == USB_PHY_CTRL_ADDR)     return g_soc.usb_phy_ctrl;
    if (addr == USB_PHY_INTE_ADDR)     return g_soc.usb_phy_inte;
    if (addr == USB_PHY_INT_STATUS_ADDR) return g_soc.usb_phy_int_status;

    if (addr >= MUSB_FIFO_ADDR(0) && addr <= MUSB_FIFO_ADDR(3)) {
        uint8_t ep = (uint8_t)((addr - MUSB_FIFO_ADDR(0)) >> 2);
        uint32_t w = 0u;
        for (uint32_t i = 0u; i < 4u; i++) {
            uint8_t b = (g_soc.musb_rx_pos[ep] < g_soc.musb_rx_len[ep])
                      ? g_soc.musb_rx_fifo[ep][g_soc.musb_rx_pos[ep]++] : 0u;
            w |= ((uint32_t)b << (i * 8u));
        }
        return w;
    }
    return 0u;
}

void openfw_mmio_wr32(uint32_t addr, uint32_t val)
{
    if (addr == SYSTICK_CSR_ADDR) { g_soc.systick_csr = val; return; }
    if (addr == SYSTICK_RVR_ADDR) { g_soc.systick_rvr = val; return; }
    if (addr == SYSTICK_CVR_ADDR) { g_soc.systick_cvr = val; return; }
    if (addr == SCB_AIRCR_ADDR)   { g_soc.scb_aircr = val; return; }
    if (addr == WDT_EN_ADDR)      { g_soc.wdt_en = val; return; }
    if (addr == WDT_FEED_ADDR)    { g_soc.wdt_feeds++; g_soc.last_wdt_key = val; return; }
    if (addr == WDT_MODE_ADDR)    { g_soc.wdt_mode = val; return; }
    if (addr == WDT_CFG_ADDR)     { g_soc.wdt_cfg = val; return; }
    if (addr == WDT_CTRL1_ADDR)   { g_soc.wdt_ctrl1 = val; return; }
    if (addr == AON_WDT_PMU_RST_CMD_ADDR) { g_soc.aon_wdt_pmu_rst_cmd = val; return; }
    if (addr == AON_PLL_CTRL_ADDR) { g_soc.aon_pll_ctrl = val; return; }

    if (addr == UART0_CTRL_ADDR)  { g_soc.uart0_ctrl = val; return; }
    if (addr == UART0_IE_ADDR)    { g_soc.uart0_ie = val; return; }
    if (addr == UART0_REG18_ADDR) { g_soc.uart0_reg18 = val; return; }
    if (addr == UART0_DATA_ADDR) {
        if (g_soc.uart0_tx_len + 1u < sizeof(g_soc.uart0_tx)) {
            g_soc.uart0_tx[g_soc.uart0_tx_len++] = (char)(val & 0xFFu);
            g_soc.uart0_tx[g_soc.uart0_tx_len] = '\0';
        }
        return;
    }

    if (addr == AON_CLK_CFG_ADDR)     { g_soc.aon_clk_cfg = val & 0x7FFFFFFFu; return; }
    if (addr == AON_CLK_EN_ADDR)      { g_soc.aon_clk_en = val; return; }
    if (addr == AON_PWR_CTL0_ADDR)    { g_soc.aon_pwr_ctl0 = val; return; }
    if (addr == AON_RTC_CTRL_ADDR)    { g_soc.aon_rtc_ctrl = val; return; }
    if (addr == RTC_DATE_ADDR)        { g_soc.rtc_date = val; return; }
    if (addr == RTC_TIME_ADDR)        { g_soc.rtc_time = val; return; }
    if (addr == RTC_ALARM0_VAL_ADDR)  { g_soc.rtc_alarm0_val = val; return; }
    if (addr == RTC_ALARM0_CTRL_ADDR) { g_soc.rtc_alarm0_ctrl = val; return; }
    if (addr >= AON_BACKUP_ADDR(0u) && addr <= AON_BACKUP_ADDR(15u)) {
        g_soc.aon_backup[(addr - AON_BACKUP_ADDR(0u)) >> 2] = val;
        return;
    }

    if (addr == RTWI_WDATA_ADDR) { g_soc.rtwi_wdata = val; return; }
    if (addr == RTWI_CMD_ADDR) {
        g_soc.rtwi_cmd = val;
        if (val & (1u << 31)) {
            uint8_t reg = (uint8_t)((val >> 8) & 0xFFu);
            uint8_t op  = (uint8_t)(val & 0xFFu);
            if (op == 0x60u) {
                if (reg == 0x30u || reg == 0x31u || reg == 0x4Au) {
                    g_soc.pmu_regs[reg] &= (uint8_t)~(g_soc.rtwi_wdata & 0xFFu); /* W1C IRQ regs */
                } else {
                    g_soc.pmu_regs[reg] = (uint8_t)(g_soc.rtwi_wdata & 0xFFu);
                }
            } else if (op == 0x61u) {
                g_soc.rtwi_rdata = (uint32_t)g_soc.pmu_regs[reg];
            }
            g_soc.rtwi_cmd &= ~(1u << 31);
        }
        return;
    }

    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u) {
        uint32_t port = (addr - GPIO_BASE) >> 6;
        uint32_t off  = (addr - GPIO_BASE) & 0x3Fu;
        if (off < 0x10u)  { g_soc.gpio_mux[port][off >> 2] = val; return; }
        if (off == 0x24u) { g_soc.gpio_pad_dir[port] = val; return; }
        if (off == 0x34u) { g_soc.gpio_out[port] |= val; return; }
        if (off == 0x38u) { g_soc.gpio_out[port] &= ~val; return; }
    }
    if (addr == GPIO_LCD_PAD_EN_ADDR) { g_soc.gpio_lcd_pad_en = val; return; }

    if (addr == CRU_SARADC_CLK_ADDR)  { g_soc.cru_saradc_clk = val; return; }
    if (addr == CRU_SARADC_RST_ADDR)  { g_soc.cru_saradc_rst = val; return; }
    if (addr == SARADC_CTRL_ADDR)     { g_soc.saradc_ctrl = val; return; }
    if (addr == SARADC_START_ADDR)    { g_soc.saradc_start = val; return; }
    if (addr == SARADC_CH1_THR_ADDR)  { g_soc.saradc_ch1_thr = val; return; }

    if (addr == PWM3_CTRL_ADDR)       { g_soc.pwm3_ctrl = val & ~(1u << 28); return; }
    if (addr == PWM3_PERIOD_DUTY_ADDR) { g_soc.pwm3_period_duty = val; return; }
    if (addr == LCDC_XBAR0_ADDR)      { g_soc.lcdc_xbar0 = val; return; }
    if (addr == LCDC_XBAR1_ADDR)      { g_soc.lcdc_xbar1 = val; return; }
    if (addr == LCDC_IF_CTRL_ADDR)    { g_soc.lcdc_if_ctrl = val; return; }
    if (addr == LCDC_CMD_REG_ADDR)    { g_soc.lcdc_cmd_reg = val; return; }
    if (addr == LCDC_DATA_LEN_ADDR)   { g_soc.lcdc_data_len = val; return; }
    if (addr == LCDC_TX_DATA_ADDR)    { g_soc.lcdc_tx_data = val; return; }
    if (addr == LCDC_STATUS_ADDR)     { g_soc.lcdc_status &= ~val; return; }
    if (addr == LCDC_XFER_CTRL_ADDR) {
        g_soc.lcdc_xfer_ctrl = val;
        if ((val & 0x0Fu) == 0x07u) {
            st7735s_on_byte(0, (uint8_t)g_soc.lcdc_cmd_reg);
            g_soc.lcdc_status |= 0x80000000u;
        } else if ((val & 0x0Fu) == 0x0Bu) {
            uint32_t n = (g_soc.lcdc_data_len & 0x3u) + 1u;
            for (uint32_t i = 0u; i < n; i++) {
                st7735s_on_byte(1, (uint8_t)(g_soc.lcdc_tx_data >> (i * 8u)));
            }
            g_soc.lcdc_status |= 0x80000000u;
        }
        return;
    }

    if (addr == SDIO_CTRL_ADDR)    { g_soc.sdio_ctrl = val & ~0x7u; return; }
    if (addr == SDIO_ARG_ADDR)     { g_soc.sdio_arg = val; return; }
    if (addr == SDIO_BLKSIZ_ADDR)  { g_soc.sdio_blksiz = val; return; }
    if (addr == SDIO_BYTCNT_ADDR)  { g_soc.sdio_bytcnt = val; return; }
    if (addr == SDIO_INTMASK_ADDR) { g_soc.sdio_intmask = val; return; }
    if (addr == SDIO_FIFOTH_ADDR)  { g_soc.sdio_fifoth = val; return; }
    if (addr == SDIO_TMOUT_ADDR)   { g_soc.sdio_tmout = val; return; }
    if (addr == SDIO_BMOD_ADDR)    { g_soc.sdio_bmod = val; return; }
    if (addr == SDIO_RINTSTS_ADDR) { g_soc.sdio_rintsts &= ~val; return; }
    if (addr == SDIO_CMD_ADDR)     { if (val & 0x80000000u) sdio_exec_cmd(val); return; }
    if (addr == SDIO_FIFO_ADDR) {
        assert(g_soc.sdio_fifo_pos < 128u);
        g_soc.sdio_fifo[g_soc.sdio_fifo_pos++] = val;
        if (g_soc.sdio_fifo_pos == 128u) {
            uint8_t *dst = &g_soc.sd_sectors[g_soc.sdio_active_lba * 512u];
            for (uint32_t w = 0u; w < 128u; w++) {
                uint32_t word = g_soc.sdio_fifo[w];
                dst[w * 4u + 0u] = (uint8_t)(word);
                dst[w * 4u + 1u] = (uint8_t)(word >> 8);
                dst[w * 4u + 2u] = (uint8_t)(word >> 16);
                dst[w * 4u + 3u] = (uint8_t)(word >> 24);
            }
            g_soc.sdio_active_lba++;
            if (g_soc.sdio_blocks_rem > 0u) {
                g_soc.sdio_blocks_rem--;
            }
            g_soc.sdio_fifo_pos = 0u;
            if (g_soc.sdio_blocks_rem == 0u) {
                g_soc.sdio_rintsts |= SDIO_INT_DTO;
            }
        }
        return;
    }

    if (addr == CRU_CLK_SRC_GATE_ADDR) { g_soc.cru_clk_src_gate = val; return; }
    if (addr == CRU_RST_SET1_ADDR)     { g_soc.cru_rst_set1 = val; return; }
    if (addr == CRU_RST_STAT1_ADDR)    { g_soc.cru_rst_stat1 = val; return; }
    if (addr == CRU_CLK_EN_USB_ADDR)   { g_soc.cru_clk_en_usb = val; return; }
    if (addr == SYSCTL_USB_CTRL_ADDR)  { g_soc.sysctl_usb_ctrl = val; return; }
    if (addr == USB_PHY_CTRL_ADDR)     { g_soc.usb_phy_ctrl = val; return; }
    if (addr == USB_PHY_INTE_ADDR)     { g_soc.usb_phy_inte = val; return; }
    if (addr == USB_PHY_INT_STATUS_ADDR) { g_soc.usb_phy_int_status &= ~val; return; }

    if (addr >= MUSB_FIFO_ADDR(0) && addr <= MUSB_FIFO_ADDR(3)) {
        uint8_t ep = (uint8_t)((addr - MUSB_FIFO_ADDR(0)) >> 2);
        for (uint32_t i = 0u; i < 4u; i++) {
            assert(g_soc.musb_tx_len[ep] < sizeof(g_soc.musb_tx_fifo[ep]));
            g_soc.musb_tx_fifo[ep][g_soc.musb_tx_len[ep]++] = (uint8_t)(val >> (i * 8u));
        }
    }
}

uint16_t openfw_mmio_rd16(uint32_t addr)
{
    if (addr == MUSB_INTRTX_ADDR)  { uint16_t v = g_soc.musb_intrtx; g_soc.musb_intrtx = 0u; return v; }
    if (addr == MUSB_INTRRX_ADDR)  { uint16_t v = g_soc.musb_intrrx; g_soc.musb_intrrx = 0u; return v; }
    if (addr == MUSB_FRAME_ADDR)   return g_soc.musb_frame;
    if (addr == MUSB_CSR0_ADDR)    return g_soc.musb_txcsr[g_soc.musb_index];
    if (addr == MUSB_RXCSR_ADDR)   return g_soc.musb_rxcsr[g_soc.musb_index];
    if (addr == MUSB_COUNT0_ADDR)  return g_soc.musb_rxcount[g_soc.musb_index];
    return 0u;
}

void openfw_mmio_wr16(uint32_t addr, uint16_t val)
{
    uint8_t ep = g_soc.musb_index & 3u;
    if (addr == MUSB_INTRTXE_ADDR) { g_soc.musb_intrtxe = val; return; }
    if (addr == MUSB_INTRRXE_ADDR) { g_soc.musb_intrrxe = val; return; }
    if (addr == MUSB_TXMAXP_ADDR)  { g_soc.musb_txmaxp[ep] = val; return; }
    if (addr == MUSB_RXMAXP_ADDR)  { g_soc.musb_rxmaxp[ep] = val; return; }
    if (addr == MUSB_CSR0_ADDR) {
        if (ep == 0u) {
            if (val & 0x0020u) {
                g_soc.musb_ep0_stalls++;
            }
            if (val & 0x0040u) {
                g_soc.musb_txcsr[0] &= ~0x0001u;
                g_soc.musb_rx_pos[0] = 0u;
                g_soc.musb_rx_len[0] = 0u;
            }
            if (val & 0x0002u) {
                g_soc.musb_txcsr[0] &= ~0x0002u; /* host instantly consumes EP0 IN packet */
            }
            if (val & 0x0100u) {
                g_soc.musb_tx_len[0] = 0u;
                g_soc.musb_rx_len[0] = 0u;
            }
        } else {
            g_soc.musb_txcsr[ep] = (uint16_t)(val & ~0x0048u);
            if (val & 0x0008u) g_soc.musb_tx_len[ep] = 0u;
        }
        return;
    }
    if (addr == MUSB_RXCSR_ADDR) {
        g_soc.musb_rxcsr[ep] = (uint16_t)(val & ~0x0090u);
        if (val & 0x0010u) {
            g_soc.musb_rx_pos[ep] = 0u;
            g_soc.musb_rx_len[ep] = 0u;
        }
    }
}

uint8_t openfw_mmio_rd8(uint32_t addr)
{
    if (addr == MUSB_POWER_ADDR)    return g_soc.musb_power;
    if (addr == MUSB_TESTMODE_ADDR) return g_soc.musb_testmode;
    if (addr == MUSB_INTRUSB_ADDR)  { uint8_t v = g_soc.musb_intrusb; g_soc.musb_intrusb = 0u; return v; }
    if (addr >= MUSB_FIFO_ADDR(0) && addr <= MUSB_FIFO_ADDR(3)) {
        uint8_t ep = (uint8_t)((addr - MUSB_FIFO_ADDR(0)) >> 2);
        return (g_soc.musb_rx_pos[ep] < g_soc.musb_rx_len[ep])
             ? g_soc.musb_rx_fifo[ep][g_soc.musb_rx_pos[ep]++] : 0u;
    }
    return 0u;
}

void openfw_mmio_wr8(uint32_t addr, uint8_t val)
{
    if (addr == MUSB_FADDR_ADDR)    { g_soc.musb_faddr = val; return; }
    if (addr == MUSB_POWER_ADDR)    { g_soc.musb_power = val; return; }
    if (addr == MUSB_INTRUSBE_ADDR) { g_soc.musb_intrusbe = val; return; }
    if (addr == MUSB_INDEX_ADDR)    { g_soc.musb_index = val & 0x0Fu; return; }
    if (addr == MUSB_TESTMODE_ADDR) { g_soc.musb_testmode = val; return; }
    if (addr >= MUSB_FIFO_ADDR(0) && addr <= MUSB_FIFO_ADDR(3)) {
        uint8_t ep = (uint8_t)((addr - MUSB_FIFO_ADDR(0)) >> 2);
        assert(g_soc.musb_tx_len[ep] < sizeof(g_soc.musb_tx_fifo[ep]));
        g_soc.musb_tx_fifo[ep][g_soc.musb_tx_len[ep]++] = val;
    }
}

void openfw_mmio_nop(void) { g_soc.us_time += 10u; }
void openfw_mmio_dsb(void) { g_soc.dsb_count++; }
void openfw_mmio_wfi(void)
{
    g_soc.wfi_count++;
    if (g_wfi_armed) {
        g_wfi_armed = 0;
        longjmp(g_wfi_jmp, 1);
    }
}

/* BootROM hooks required by drivers under test */
int      rom_printf(const char *fmt, ...) { (void)fmt; return 0; }
void     rom_gpio_config(uint32_t cfg) { (void)cfg; }
int      rom_gpio_read(uint32_t cfg) { (void)cfg; return 0; }
void     rom_gpio_write(uint32_t cfg, int val) { (void)cfg; (void)val; }
size_t   rom_strlen(const char *s) { return strlen(s); }
void    *rom_memcpy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }
void    *rom_memset(void *dst, int val, size_t n) { return memset(dst, val, n); }
int      rom_rst_is_released(uint32_t rst_id) { return (g_soc.rom_rst_released_mask >> (rst_id & 31u)) & 1u; }
void     rom_rst_release(uint32_t rst_id) { g_soc.rom_rst_released_mask |= (1u << (rst_id & 31u)); }
void     rom_rst_hold(uint32_t rst_id) { g_soc.rom_rst_released_mask &= ~(1u << (rst_id & 31u)); }
void     rom_clk_enable(uint32_t clk_id) { (void)clk_id; }
void     rom_clk_disable(uint32_t clk_id) { (void)clk_id; }
void     rom_clk_div(uint32_t clk_id, uint32_t div) { (void)clk_id; (void)div; }
void     rom_clk_set_src(uint32_t clk_id, uint32_t src) { (void)clk_id; (void)src; }
void     rom_clk_set_rate(uint32_t clk_id, uint32_t hz) { (void)clk_id; (void)hz; }
uint64_t rom_us_timer_get(void) { return g_soc.us_time; }
void     rom_us_delay(uint32_t us) { g_soc.us_time += us; }
void     rom_wdt_reset(uint32_t timeout) { (void)timeout; }

/* ============================================================================
 * Per-Driver Test Cases
 * ============================================================================ */
static void test_wdt_timer(void)
{
    soc_reset();
    wdt_feed();
    assert(g_soc.wdt_feeds == 1u && g_soc.last_wdt_key == WDT_FEED_KEY);

    g_systick_ms = 500u;
    g_exp_running = 1u;
    g_exp_deadline_ms = 1000u;
    wdt_feed();
    assert(g_exp_deadline_ms == 500u + OPENFW_EXP_TIMEOUT_MS);
    g_exp_running = 0u;

    systick_init(192000000u);
    assert(g_soc.systick_rvr == 191999u);
    assert(g_soc.systick_csr == 0x7u);
    systick_init(0u);
    assert(g_soc.systick_rvr == 1u);
    systick_init(500u);
    assert(g_soc.systick_rvr == 1u);
    systick_init(192000000u);
    g_systick_ms = 4242u;
    assert(millis() == 4242u);

    g_soc.us_time = 0x00000001FFFFFFFAULL;
    g_soc.us_rollover_inject = 1; /* hi1=1, lo=0xFFFFFFFA, then hi2=2 -> retries cleanly */
    assert(us_timer_now() == 0x0000000200000000ULL);

    uint64_t before = g_soc.us_time;
    mdelay(2u);
    assert(g_soc.us_time - before >= 2000u);

    g_soc.aon_wdt_trim0 = 0x00001234u;
    g_soc.aon_wdt_trim1 = 0x0000005Au;
    g_soc.aon_wdt_pmu_rst_cmd = 0u;
    wdt_start_reset(16u, 0x2Fu);
    assert(g_soc.aon_wdt_pmu_rst_cmd == 0xAF20u);
    assert(g_soc.wdt_en == 1u);
    assert(g_soc.wdt_mode == 0x2001u);
    assert(g_soc.wdt_cfg == 0x71u);
    assert(g_soc.wdt_ctrl1 == (((0x1234u << 8) & 0x01FFFF00u) | 0x5Au));
    wdt_start_reset(16u, 0u);
    assert(g_soc.aon_wdt_pmu_rst_cmd == 0u);
    wdt_stop();
    assert(g_soc.wdt_cfg == 0u);

    g_soc.aon_wdt_pmu_rst_cmd = 0xAF20u;
    g_soc.aon_pll_ctrl = 0x0Fu;
    g_soc.wdt_en = 0u;
    g_soc.wdt_mode = 0u;
    g_soc.wdt_cfg = 0xF1u;
    uint32_t feeds_before = g_soc.wdt_feeds;
    g_wfi_armed = 1;
    if (setjmp(g_wfi_jmp) == 0) {
        system_reboot();
    }
    assert(g_soc.dsb_count >= 1u);
    assert(g_soc.aon_wdt_pmu_rst_cmd == 0u);
    assert((g_soc.aon_pll_ctrl & 0x02u) == 0u);
    assert(g_soc.wdt_en == 1u);
    assert(g_soc.wdt_mode == 0x2001u);
    assert(g_soc.wdt_cfg == 0x01u);
    assert(g_soc.wdt_feeds == feeds_before + 1u && g_soc.last_wdt_key == WDT_FEED_KEY);
    assert(g_soc.scb_aircr == (SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET));
    assert(g_soc.wfi_count == 1u);
    printf("[OK] wdt_timer + PMU reset MMIO\n");
}

static void test_uart_and_log_ring(void)
{
    soc_reset();
    openfw_log_reset();
    uart0_init_115200();
    assert((g_soc.gpio_mux[3][0] & 0xFu) == 12u); /* PD0 mux=12 */
    assert(g_soc.uart0_ctrl == ((13u << 16) | (13u << 4) | 0x7u));

    uart0_puts("BOOT\n");
    uart0_put_u32(2026u);
    uart0_putc(' ');
    uart0_put_hex(0x00804C01u);
    assert(strcmp(g_soc.uart0_tx, "BOOT\r\n2026 0x00804C01") == 0);
    assert(g_openfw_log.magic == OPENFW_LOG_MAGIC);
    assert(g_openfw_log.size == OPENFW_LOG_SIZE);
    assert(g_openfw_log.write_idx == 20u);
    assert(g_openfw_log.dropped_bytes == 0u);
    assert(strncmp(g_openfw_log.buf, "BOOT\n2026 0x00804C01", 20) == 0);

    /* TX FIFO stall must bound-out after 50,000 polls and still preserve SRAM log */
    g_soc.uart0_stat = 0u;
    uart0_puts("STALL");
    assert(g_openfw_log.dropped_bytes == 5u);
    assert(g_openfw_log.write_idx == 25u);
    assert(strncmp(&g_openfw_log.buf[20], "STALL", 5) == 0);

    /* PD0-safe openfw_log_puts appends to SRAM ring without touching UART0 MMIO */
    uint32_t tx_before   = g_soc.uart0_tx_len;
    uint32_t drop_before = g_openfw_log.dropped_bytes;
    uart0_puts(NULL);
    assert(g_soc.uart0_tx_len == tx_before);
    openfw_log_puts("SRAM_ONLY");
    assert(g_soc.uart0_tx_len == tx_before);
    assert(g_openfw_log.dropped_bytes == drop_before);
    assert(g_openfw_log.write_idx == 34u);
    assert(strncmp(&g_openfw_log.buf[25], "SRAM_ONLY", 9) == 0);
    printf("[OK] uart + g_openfw_log MMIO\n");
}

static void test_rtc_driver(void)
{
    soc_reset();
    rtc_ensure_running();
    assert((g_soc.aon_clk_en & (1u << 2)) != 0u);
    assert((g_soc.aon_pwr_ctl0 & (1u << 28)) == 0u);
    assert((g_soc.aon_rtc_ctrl & ((1u << 4) | (1u << 8))) == 0u);

    /* 2024-02-29 is leap day (bit 24 = 1), Thursday (wday=3) */
    rtc_datetime_t set_leap = { 2024u, 2u, 29u, 0u, 23u, 59u, 58u };
    rtc_set(&set_leap);
    assert((g_soc.rtc_date & (1u << 24)) != 0u);
    rtc_datetime_t got = {0};
    rtc_get(&got);
    assert(got.year == 2024u && got.month == 2u && got.day == 29u);
    assert(got.weekday == 3u && got.hour == 23u && got.minute == 59u && got.second == 58u);

    /* Midnight rollover retry: first RTC_DATE read returns old day, second returns new day */
    g_soc.rtc_rollover_inject = 1;
    rtc_get(&got);
    assert(got.day == 30u && got.hour == 0u && got.minute == 0u && got.second == 0u);

    /* Non-leap 2026-11-19 Thursday (wday=3) + DOS timestamp + Alarm0 */
    rtc_datetime_t set_2026 = { 2026u, 11u, 19u, 0u, 14u, 30u, 44u };
    rtc_set(&set_2026);
    assert((g_soc.rtc_date & (1u << 24)) == 0u);
    rtc_get(&got);
    assert(got.weekday == 3u);
    uint32_t dos = rtc_to_dos_timestamp(&got);
    assert(((dos >> 25) + 1980u) == 2026u && ((dos >> 21) & 0xFu) == 11u && ((dos >> 16) & 0x1Fu) == 19u);

    rtc_datetime_t file_dt = rtc_from_dos_timestamp(dos);
    assert(file_dt.year == got.year && file_dt.month == got.month && file_dt.day == got.day);
    assert(file_dt.hour == got.hour && file_dt.minute == got.minute && file_dt.second == got.second);
    assert(file_dt.weekday == got.weekday);
    assert(rtc_from_dos_timestamp(0).year == 0);
    assert(rtc_from_dos_timestamp((46u << 25) | (2u << 21) | (30u << 16)).year == 0);

    /* rtc_to_dos_timestamp year clamp (>2107 clamps to 127 = 2107 instead of wrapping) */
    rtc_datetime_t future = { 2120u, 6u, 15u, 0u, 12u, 0u, 0u };
    uint32_t dos_hi = rtc_to_dos_timestamp(&future);
    assert((dos_hi >> 25) == 127u && ((dos_hi >> 21) & 0xFu) == 6u && ((dos_hi >> 16) & 0x1Fu) == 15u);

    /* rtc_set year=0 clamps safely to 2000 (no uint32 underflow) and releases HALT bit */
    rtc_datetime_t zero_yr = { 0u, 1u, 1u, 0u, 0u, 0u, 0u };
    rtc_set(&zero_yr);
    assert((g_soc.aon_rtc_ctrl & (1u << 8)) == 0u);
    rtc_get(&got);
    assert(got.year == 2000u && got.month == 1u && got.day == 1u && got.weekday == 5u);

    rtc_set_alarm0_seconds(10u);
    assert(g_soc.rtc_alarm0_val == 9u && g_soc.rtc_alarm0_ctrl == 1u);
    printf("[OK] rtc MMIO\n");
}

static void test_pmu_driver(void)
{
    soc_reset();
    g_soc.pmu_regs[0x2Eu] = 90u;  /* ~4.075 V */
    g_soc.pmu_regs[0x2Au] = 0x04u; /* VBUS / charging */
    assert(pmu_read_vbat_mv() == 4075u);
    assert(pmu_get_charge_state() == PMU_CHG_CHARGING);
    assert(pmu_is_vbus_present() == 1);

    g_soc.pmu_regs[0x2Au] = 0x40u;
    assert(pmu_get_charge_state() == PMU_CHG_FULL);
    assert(pmu_is_vbus_present() == 1);

    g_soc.pmu_regs[0x2Au] = 0x00u;
    assert(pmu_get_charge_state() == PMU_CHG_DISCHARGING);
    assert(pmu_is_vbus_present() == 0);

    g_wfi_armed = 1;
    if (setjmp(g_wfi_jmp) == 0) {
        pmu_reboot_to_usb_bootloader();
    }
    assert((g_soc.pmu_regs[0x23u] & 0xC0u) == 0xC0u);
    assert((g_soc.pmu_regs[0x00u] & 0x01u) == 0x01u);

    g_wfi_armed = 1;
    if (setjmp(g_wfi_jmp) == 0) {
        pmu_reboot_to_sd_ota();
    }
    assert((g_soc.pmu_regs[0x00u] & 0x01u) == 0x00u);

    /* KEY_ONOFF (PMU reg 0x31 W1C + reg 0x1A unmask) */
    g_soc.pmu_regs[0x1Au] = 0xFFu;
    g_soc.pmu_regs[0x31u] = 0x70u;
    key_onoff_init();
    assert((g_soc.pmu_regs[0x1Au] & 0x70u) == 0x00u);
    assert(g_soc.pmu_regs[0x31u] == 0x00u);

    int lp = -1;
    g_soc.pmu_regs[0x31u] = 0x40u; /* Press */
    assert(key_onoff_pressed(&lp) == 1 && lp == 0 && g_soc.pmu_regs[0x31u] == 0x00u);
    g_soc.pmu_regs[0x31u] = 0x20u; /* Long press while down */
    assert(key_onoff_pressed(&lp) == 1 && lp == 1 && g_soc.pmu_regs[0x31u] == 0x00u);
    g_soc.pmu_regs[0x31u] = 0x10u; /* Release */
    assert(key_onoff_pressed(&lp) == 0 && lp == 0 && g_soc.pmu_regs[0x31u] == 0x00u);

    /* key_onoff_poll: brief press+release latched in a single read is preserved */
    g_soc.pmu_regs[0x31u] = 0x50u; /* Press (0x40) | Release (0x10) */
    assert(key_onoff_poll() == (KEY_ONOFF_PRESS | KEY_ONOFF_RELEASE));
    assert(g_soc.pmu_regs[0x31u] == 0x00u);
    assert(key_onoff_poll() == 0u);
    g_soc.pmu_regs[0x31u] = 0x40u;
    assert(key_onoff_poll() == KEY_ONOFF_PRESS);
    g_soc.pmu_regs[0x31u] = 0x20u;
    assert(key_onoff_poll() == KEY_ONOFF_LONG);
    g_soc.pmu_regs[0x31u] = 0x10u;
    assert(key_onoff_poll() == KEY_ONOFF_RELEASE);

    uint8_t progress = 0u;
    /* A tap can arrive as one read. A tap that woke the display stays awake. */
    uint8_t tap = KEY_ONOFF_PRESS | KEY_ONOFF_RELEASE;
    assert(power_button_update(tap, 0, 0, &progress) == POWER_BUTTON_SLEEP);
    assert(power_button_update(tap, 1, 1, &progress) == POWER_BUTTON_WAKE);

    /* Holding Power from screen-off wakes the display and never shuts back down. */
    uint32_t start = UINT32_MAX - 100u;
    assert(power_button_update(KEY_ONOFF_PRESS, start, 1, &progress) == POWER_BUTTON_WAKE);
    assert(power_button_update(0, start + 300u, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(KEY_ONOFF_LONG, start + 2000u, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(0, start + 2100u, 0, &progress) == POWER_BUTTON_IDLE);

    /* Start awake, cross the millisecond counter rollover, then release early. */
    assert(power_button_update(KEY_ONOFF_PRESS, start, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(0, start + 300u, 0, &progress) == POWER_BUTTON_PROGRESS);
    assert(progress == 15u);
    assert(power_button_update(KEY_ONOFF_RELEASE, start + 1999u, 0, &progress) == POWER_BUTTON_CANCEL);
    assert(power_button_update(0, start + 2000u, 0, &progress) == POWER_BUTTON_IDLE);

    /* Shutdown wins when the threshold or the PMU long event coincides with release. */
    assert(power_button_update(KEY_ONOFF_PRESS, 2000u, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(0, 4000u, 0, &progress) == POWER_BUTTON_PROGRESS && progress == 100u);
    assert(power_button_update(KEY_ONOFF_RELEASE, 4016u, 0, &progress) == POWER_BUTTON_SHUTDOWN);
    assert(power_button_update(KEY_ONOFF_PRESS, 4500u, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(KEY_ONOFF_LONG, 4550u, 0, &progress) == POWER_BUTTON_PROGRESS);
    assert(power_button_update(0, 4600u, 0, &progress) == POWER_BUTTON_PROGRESS && progress == 100u);
    assert(power_button_update(KEY_ONOFF_RELEASE, 4650u, 0, &progress) == POWER_BUTTON_SHUTDOWN);
    assert(power_button_update(tap | KEY_ONOFF_LONG, 5000u, 0, &progress) == POWER_BUTTON_SHUTDOWN);

    /* A missing release after the full progress screen still completes shutdown (400 ms grace). */
    assert(power_button_update(KEY_ONOFF_PRESS, 6000u, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(0, 8000u, 0, &progress) == POWER_BUTTON_PROGRESS);
    assert(power_button_update(0, 8400u, 0, &progress) == POWER_BUTTON_SHUTDOWN);

    /* The PMU can report a long press before we observe any press edge. */
    assert(power_button_update(KEY_ONOFF_LONG, 10000u, 0, &progress) == POWER_BUTTON_PROGRESS);
    assert(power_button_update(KEY_ONOFF_PRESS, 10200u, 0, &progress) == POWER_BUTTON_PROGRESS);
    assert(power_button_update(0, 10400u, 0, &progress) == POWER_BUTTON_SHUTDOWN);

    /* Delayed poll after button release (held >= 2000 ms without KEY_ONOFF_LONG) must NOT shutdown */
    assert(power_button_update(KEY_ONOFF_PRESS, 12000u, 0, &progress) == POWER_BUTTON_IDLE);
    assert(power_button_update(KEY_ONOFF_RELEASE, 14500u, 0, &progress) == POWER_BUTTON_CANCEL);
    assert(power_button_update(0u, 14600u, 0, &progress) == POWER_BUTTON_IDLE);

    /* NULL progress pointer must not dereference */
    assert(power_button_update(KEY_ONOFF_PRESS, 20000u, 0, NULL) == POWER_BUTTON_IDLE);
    assert(power_button_update(0u, 20500u, 0, NULL) == POWER_BUTTON_PROGRESS);
    assert(power_button_update(0u, 22100u, 0, NULL) == POWER_BUTTON_PROGRESS);
    assert(power_button_update(KEY_ONOFF_RELEASE, 22200u, 0, NULL) == POWER_BUTTON_SHUTDOWN);

    /* KEY_ONOFF_LONG when !down still arms shutdown and finishes cleanly on release */
    progress = 0u;
    assert(power_button_update(KEY_ONOFF_LONG, 30000u, 0, &progress) == POWER_BUTTON_PROGRESS);
    assert(progress == 100u);
    assert(power_button_update(KEY_ONOFF_RELEASE, 30100u, 0, &progress) == POWER_BUTTON_SHUTDOWN);

    /* Unattended always-on PMU init + charge management */
    g_soc.pmu_regs[0x00u] = 0xCFu;
    g_soc.pmu_regs[0x17u] = 0x01u;
    g_soc.pmu_regs[0x20u] = 0x80u;
    g_soc.pmu_regs[0x22u] = 0x55u;
    g_soc.pmu_regs[0x23u] = 0xC3u;
    g_soc.pmu_regs[0x2Au] = 0x04u; /* VBUS present */
    g_soc.pmu_regs[0x30u] = 0xAAu;
    pmu_unattended_init();
    assert(g_soc.pmu_regs[0x00u] == 0xC9u); /* [2:1] cleared, bit 3 (0x08), [7:6] & [0] preserved */
    assert(g_soc.pmu_regs[0x20u] == 0x2Fu);
    assert(g_soc.pmu_regs[0x22u] == 0x00u);
    assert(g_soc.pmu_regs[0x23u] == 0x03u);
    assert(g_soc.pmu_regs[0x30u] == 0x00u);
    assert((g_soc.pmu_regs[0x17u] & 0x05u) == 0x04u);

    /* If soft power-off cleared PMU[0x00][7:6], boot init must re-enable 8s rescue (0x80) */
    g_soc.pmu_regs[0x00u] = 0x0Eu;
    pmu_unattended_init();
    assert(g_soc.pmu_regs[0x00u] == 0x88u);

    g_soc.pmu_regs[0x17u] = 0x00u;
    pmu_poll_charge_manage();
    assert((g_soc.pmu_regs[0x17u] & 0x04u) == 0x04u); /* VBUS attach enables charge */
    g_soc.pmu_regs[0x30u] = 0x02u; /* PMU_CHG_FULL IRQ */
    pmu_poll_charge_manage();
    assert(g_soc.pmu_regs[0x30u] == 0x00u);
    assert((g_soc.pmu_regs[0x17u] & 0x04u) == 0x00u); /* Full stops charging without dropping VBUS */
    printf("[OK] pmu RTWI MMIO\n");
}

static void test_keys_driver(void)
{
    soc_reset();
    keys_init();
    assert((g_soc.gpio_mux[3][0] & 0xFu) == 15u); /* PD0 analog */
    assert((g_soc.cru_saradc_clk & (1u << 20)) != 0u);
    assert(g_soc.saradc_start == 0x0000A800u);

    g_soc.saradc_ch1_data = 50u;
    assert(keys_decode(keys_read_adc()) == KEY_LEFT);
    g_soc.saradc_ch1_data = 300u;
    assert(keys_decode(keys_read_adc()) == KEY_LEFT);
    g_soc.saradc_ch1_data = 500u;
    assert(keys_decode(keys_read_adc()) == KEY_RIGHT);
    g_soc.saradc_ch1_data = 750u;
    assert(keys_decode(keys_read_adc()) == KEY_RIGHT);
    g_soc.saradc_ch1_data = 1350u;
    assert(keys_decode(keys_read_adc()) == KEY_VOL_UP);
    g_soc.saradc_ch1_data = 1600u;
    assert(keys_decode(keys_read_adc()) == KEY_NONE);
    g_soc.saradc_ch1_data = 2150u;
    assert(keys_decode(keys_read_adc()) == KEY_VOL_DOWN);
    g_soc.saradc_ch1_data = 2550u;
    assert(keys_decode(keys_read_adc()) == KEY_BACK);
    g_soc.saradc_ch1_data = 2850u;
    assert(keys_decode(keys_read_adc()) == KEY_ENTER);
    g_soc.saradc_ch1_data = 4095u;
    assert(keys_decode(keys_read_adc()) == KEY_NONE);

    assert(keys_read_rec_switch() == 0);
    g_soc.gpio_in[1] &= ~(1u << 6); /* PB6 low = Rec ON */
    assert(keys_read_rec_switch() == 1);

    assert(keys_read_hp_detect() == 0);
    g_soc.gpio_in[1] &= ~(1u << 11); /* PB11 low = Earphones inserted */
    assert(keys_read_hp_detect() == 1);

    /* 1 kHz keys_scan_tick: 8-sample debounce & brief 16 ms tap */
    g_soc.saradc_ch1_data = 4095u;
    keys_init();
    assert(keys_state(NULL) == KEY_NONE);
    assert(keys_event_pending() == 0);

    uint32_t t = 100u;
    g_soc.saradc_ch1_data = 2850u; /* KEY_ENTER */
    for (int i = 0; i < 7; i++) {
        keys_scan_tick(t++);
    }
    assert(keys_event_pending() == 0);
    assert(keys_state(NULL) == KEY_NONE);
    keys_scan_tick(t++); /* 8th sample at t=107 -> press accepted */
    assert(keys_event_pending() == 1);
    uint32_t since = 0u;
    assert(keys_state(&since) == KEY_ENTER && since == 107u);
    for (int i = 0; i < 8; i++) {
        keys_scan_tick(t++); /* hold to 16 ms total */
    }
    g_soc.saradc_ch1_data = 4095u; /* release */
    for (int i = 0; i < 8; i++) {
        keys_scan_tick(t++); /* accepted at t=123 */
    }
    key_event_t ev;
    assert(keys_peek_event(&ev) == 1 && ev.key == KEY_ENTER && ev.pressed == 1u && ev.t_ms == 107u);
    assert(keys_pop_event(&ev) == 1 && ev.key == KEY_ENTER && ev.pressed == 1u && ev.t_ms == 107u);
    assert(keys_pop_event(&ev) == 1 && ev.key == KEY_ENTER && ev.pressed == 0u && ev.t_ms == 123u);
    assert(keys_pop_event(&ev) == 0);
    assert(keys_state(NULL) == KEY_NONE);

    /* Resistor-ladder sweep glitch rejection (ENTER->BACK->VOL_DOWN 2 ms each -> LEFT 10 ms) */
    const uint16_t sweep_adc[] = { 2850u, 2850u, 2550u, 2550u, 2150u, 2150u };
    for (unsigned i = 0u; i < sizeof(sweep_adc) / sizeof(sweep_adc[0]); i++) {
        g_soc.saradc_ch1_data = sweep_adc[i];
        keys_scan_tick(t++);
    }
    assert(keys_event_pending() == 0);
    g_soc.saradc_ch1_data = 100u; /* KEY_LEFT */
    for (int i = 0; i < 10; i++) {
        keys_scan_tick(t++);
    }
    assert(keys_pop_event(&ev) == 1 && ev.key == KEY_LEFT && ev.pressed == 1u);
    assert(keys_pop_event(&ev) == 0);

    /* Direct key-to-key transition: LEFT -> RIGHT emits release(LEFT) + press(RIGHT) */
    g_soc.saradc_ch1_data = 600u; /* KEY_RIGHT */
    for (int i = 0; i < 8; i++) {
        keys_scan_tick(t++);
    }
    key_event_t ev1, ev2;
    assert(keys_pop_event(&ev1) == 1 && ev1.key == KEY_LEFT && ev1.pressed == 0u);
    assert(keys_pop_event(&ev2) == 1 && ev2.key == KEY_RIGHT && ev2.pressed == 1u);
    assert(ev1.t_ms == ev2.t_ms);
    assert(keys_pop_event(&ev) == 0);

    /* Release RIGHT before testing g_exp_running pause */
    g_soc.saradc_ch1_data = 4095u;
    for (int i = 0; i < 8; i++) {
        keys_scan_tick(t++);
    }
    keys_flush_events();

    /* g_exp_running != 0 skips scanning and flushes/resyncs cleanly on exit */
    g_exp_running = 1u;
    g_soc.saradc_ch1_data = 2850u;
    for (int i = 0; i < 12; i++) {
        keys_scan_tick(t++);
    }
    assert(keys_event_pending() == 0);
    g_exp_running = 0u;
    g_soc.saradc_ch1_data = 4095u;
    keys_scan_tick(t++);
    assert(keys_event_pending() == 0);
    assert(keys_state(NULL) == KEY_NONE);

    /* SPSC overflow resync: >16 events queued, ending with KEY_BACK held -> flush & ignore until release */
    for (int rep = 0; rep < 10; rep++) {
        g_soc.saradc_ch1_data = 2550u; /* KEY_BACK */
        for (int i = 0; i < 8; i++) keys_scan_tick(t++);
        if (rep < 9) {
            g_soc.saradc_ch1_data = 4095u;
            for (int i = 0; i < 8; i++) keys_scan_tick(t++);
        }
    }
    assert(keys_pop_event(&ev) == 0);       /* overflow flushed queue */
    assert(keys_state(NULL) == KEY_NONE);   /* held KEY_BACK ignored until released */
    g_soc.saradc_ch1_data = 4095u;
    for (int i = 0; i < 8; i++) keys_scan_tick(t++); /* release clears resync_ignore */
    assert(keys_pop_event(&ev) == 0);       /* no spurious release */
    g_soc.saradc_ch1_data = 2850u;          /* fresh KEY_ENTER press works */
    for (int i = 0; i < 8; i++) keys_scan_tick(t++);
    assert(keys_pop_event(&ev) == 1 && ev.key == KEY_ENTER && ev.pressed == 1u);
    assert(keys_flush_events() == KEY_ENTER);
    g_soc.saradc_ch1_data = 4095u;
    for (int i = 0; i < 8; i++) keys_scan_tick(t++);
    assert(keys_pop_event(&ev) && ev.key == KEY_ENTER && !ev.pressed);

    /* Mode exit hides the held key during cleanup. A direct switch to
     * another key and its release remain queued for the returning menu. */
    g_soc.saradc_ch1_data = 2550u;
    for (int i = 0; i < 8; i++) keys_scan_tick(t++);
    keys_ignore_held();
    assert(keys_state(NULL) == KEY_NONE && !keys_event_pending());
    g_soc.saradc_ch1_data = 2850u;
    for (int i = 0; i < 8; i++) keys_scan_tick(t++);
    g_soc.saradc_ch1_data = 4095u;
    for (int i = 0; i < 8; i++) keys_scan_tick(t++);
    assert(keys_pop_event(&ev) && ev.key == KEY_ENTER && ev.pressed);
    assert(keys_pop_event(&ev) && ev.key == KEY_ENTER && !ev.pressed);
    assert(!keys_event_pending());

    printf("[OK] keys SARADC + PB6/PB11 + 1kHz scanner/debounce/SPSC MMIO\n");
}

static int exp_test_blk_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *img = (uint8_t *)ctx;
    if (lba + count > 4096u) return -1;
    memcpy(buf, img + (size_t)lba * 512u, (size_t)count * 512u);
    return 0;
}

static int exp_test_blk_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    uint8_t *img = (uint8_t *)ctx;
    if (lba + count > 4096u) return -1;
    memcpy(img + (size_t)lba * 512u, buf, (size_t)count * 512u);
    return 0;
}

static void test_exp_runner_guards(const char *exfat_img_path)
{
    soc_reset();

    /* Reject openfw_exp_run() before any payload is loaded */
    openfw_exp_result_t res;
    assert(openfw_exp_run(0u, 0u, &res) == -1);

    /* Reject address+len 32-bit wraparound in openfw_exp_mem_read() */
    uint8_t mbuf[8];
    assert(openfw_exp_mem_read(0xFFFFFFFEu, mbuf, 4u) == -1);
    assert(openfw_exp_mem_read(0xFFFFFFFFu, mbuf, 1u) == -1);

    /* Disabling DMAC CH0 IRQ emits DSB barrier */
    uint32_t dsb_before = g_soc.dsb_count;
    openfw_exp_set_dmac_ch0_irq(NULL);
    assert(g_soc.dsb_count > dsb_before);

    /* Directory collision rejection & wrapped ring log dump (zero 4 KiB stack copy) */
    uint8_t *img = (uint8_t *)calloc(4096u, 512u);
    assert(img != NULL);
    FILE *f = fopen(exfat_img_path, "rb");
    assert(f != NULL);
    assert(fread(img, 512u, 4096u, f) == 4096u);
    fclose(f);

    blkdev_t dev = { exp_test_blk_read, exp_test_blk_write, 4096u, img };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    int mounted = 1;
    openfw_exp_bind_fs(&vol, &mounted);

    assert(exfat_mkdir(&vol, "COLLIDE") == 0);
    assert(openfw_exp_sd_write_file("/", "COLLIDE", "abc", 3u) == -1);
    assert(openfw_exp_sd_write_file("/", "exp_ok.txt", "xyz", 3u) == 0);

    openfw_log_reset();
    for (uint32_t i = 0u; i < OPENFW_LOG_SIZE + 123u; i++) {
        openfw_log_putc((char)('A' + (i % 26u)));
    }
    assert(openfw_exp_sd_dump_log("wrapped.log") == 0);

    exfat_entry_t lent;
    assert(exfat_find_path(&vol, "/wrapped.log", &lent) == 0 && lent.size == OPENFW_LOG_SIZE);
    uint8_t log_rb[OPENFW_LOG_SIZE];
    uint32_t got = 0u;
    assert(exfat_read_file(&vol, &lent, 0u, log_rb, OPENFW_LOG_SIZE, &got) == 0 && got == OPENFW_LOG_SIZE);
    for (uint32_t i = 0u; i < OPENFW_LOG_SIZE; i++) {
        assert(log_rb[i] == (uint8_t)('A' + ((123u + i) % 26u)));
    }

    openfw_exp_bind_fs(NULL, NULL);
    free(img);
    printf("[OK] exp_runner guards + wrapped log dump MMIO\n");
}

/* Run the 1 ms fade tick until the ramp ends; check monotonic duty steps. */
static uint32_t run_fade(int dir, uint32_t *ticks)
{
    uint32_t prev = g_soc.pwm3_period_duty & 0xFFFFu, steps = 0u;
    *ticks = 0u;
    while (lcd_backlight_fading() && *ticks < 1000u) {
        lcd_backlight_fade_tick();
        (*ticks)++;
        uint32_t d = g_soc.pwm3_period_duty & 0xFFFFu;
        assert((g_soc.pwm3_period_duty >> 16) == 48000u);
        if (d != prev) {
            assert(dir > 0 ? d > prev : d < prev);
            assert((*ticks % 10u) == 0u);
            steps++;
        }
        prev = d;
    }
    return steps;
}

static void test_backlight_fade(void)
{
    uint32_t ticks;
    /* Fade-out 80% -> 0 over 200 ms: 20 steps, gamma-2 first step is small */
    lcd_backlight_set(80u);
    lcd_backlight_fade(80u, 0u, 200u);
    assert(lcd_backlight_fading());
    assert((g_soc.pwm3_period_duty & 0xFFFFu) == 80u * 480u);
    for (int i = 0; i < 10; i++) lcd_backlight_fade_tick();
    uint32_t d1 = g_soc.pwm3_period_duty & 0xFFFFu; /* L 894 -> 849: 34600 */
    assert(d1 < 80u * 480u && d1 > 34000u);
    assert(run_fade(-1, &ticks) == 19u && ticks == 190u);
    assert((g_soc.pwm3_period_duty & 0xFFFFu) == 0u && !lcd_backlight_fading());
    lcd_backlight_set(0u);
    assert(((g_soc.gpio_mux[GPIO_PORT_B][2] >> 4) & 0xFu) == 1u);

    /* Fade-in 0 -> 20% over 100 ms from a disconnected pad: PWM connects */
    lcd_backlight_fade(0u, 20u, 100u);
    assert(((g_soc.gpio_mux[GPIO_PORT_B][2] >> 4) & 0xFu) == 5u);
    assert((g_soc.pwm3_period_duty & 0xFFFFu) == 0u);
    assert(run_fade(+1, &ticks) == 10u && ticks == 100u);
    assert(g_soc.pwm3_period_duty == ((48000u << 16) | (20u * 480u)));

    /* Reversal mid-ramp continues from the current level (no jump) */
    lcd_backlight_set(0u);
    lcd_backlight_fade(0u, 100u, 100u);
    for (int i = 0; i < 50; i++) lcd_backlight_fade_tick();
    uint32_t mid = g_soc.pwm3_period_duty & 0xFFFFu;   /* L 500 -> 12000 */
    assert(mid == 12000u);
    lcd_backlight_fade(100u, 0u, 200u);
    assert((g_soc.pwm3_period_duty & 0xFFFFu) == mid);
    assert(run_fade(-1, &ticks) == 20u);

    /* lcd_backlight_set() cancels a running ramp */
    lcd_backlight_fade(0u, 60u, 100u);
    lcd_backlight_fade_tick();
    lcd_backlight_set(35u);
    assert(!lcd_backlight_fading());
    for (int i = 0; i < 50; i++) lcd_backlight_fade_tick();
    assert(g_soc.pwm3_period_duty == ((48000u << 16) | (35u * 480u)));
    printf("[OK] backlight fade ramp\n");
}

static void test_lcd_driver(void)
{
    soc_reset();
    gpio_write(GPIO_PORT_B, 17u, 1);
    lcd_backlight_set(75u);
    assert(g_soc.pwm3_period_duty == ((48000u << 16) | (75u * 480u)));

    lcd_backlight_set(0u);
    assert((g_soc.gpio_out[GPIO_PORT_B] & (1u << 17)) == 0u);
    assert(((g_soc.gpio_mux[GPIO_PORT_B][2] >> 4) & 0xFu) == 1u);
    assert((g_soc.pwm3_ctrl & 0x3Fu) == 0u);

    lcd_backlight_set(35u);
    assert(((g_soc.gpio_mux[GPIO_PORT_B][2] >> 4) & 0xFu) == 5u);
    assert((g_soc.pwm3_ctrl & 0x3Fu) == 0x3Fu);
    assert(g_soc.pwm3_period_duty == ((48000u << 16) | (35u * 480u)));
    test_backlight_fade();

    lcd_init();
    assert((g_soc.gpio_lcd_pad_en & 0x0000FFD0u) == 0x0000FFD0u);
    assert(g_soc.st_invon == 1u && g_soc.st_disp_on == 1u);
    assert(g_soc.st_colmod == 0x05u && g_soc.st_madctl == 0xC8u);

    uint16_t navy = rgb565(0u, 0u, 128u);
    uint16_t gold = rgb565(255u, 220u, 0u);
    lcd_fill_rect(0u, 0u, LCD_WIDTH, LCD_HEIGHT, navy);
    assert(g_soc.st_fb[0][0] == navy && g_soc.st_fb[159][79] == navy);

    lcd_draw_str(8u, 16u, "OK", gold, navy);
    uint32_t fg_pixels = 0u;
    for (uint16_t y = 16u; y < 23u; y++) {
        for (uint16_t x = 8u; x < 20u; x++) {
            if (g_soc.st_fb[y][x] == gold) {
                fg_pixels++;
            }
        }
    }
    assert(fg_pixels > 10u);

    uint16_t patch[4] = { 0x1111u, 0x2222u, 0x3333u, 0x4444u };
    lcd_blit_rgb565(4u, 4u, 2u, 2u, patch);
    assert(g_soc.st_fb[4][4] == 0x1111u && g_soc.st_fb[5][5] == 0x4444u);

    /* Landscape 160x80 rotation check & restore portrait */
    lcd_set_rotation(1);
    assert(g_soc.st_madctl == 0xA8u && lcd_get_width() == 160u && lcd_get_height() == 80u);
    lcd_fill_rect(120u, 10u, 4u, 4u, gold);
    assert(g_soc.st_fb[10][120] == gold && g_soc.st_fb[13][123] == gold);
    lcd_set_rotation(0);
    assert(g_soc.st_madctl == 0xC8u && lcd_get_width() == 80u && lcd_get_height() == 160u);
    lcd_shutdown();
    assert(g_soc.lcdc_cmd_reg == 0x10u);
    printf("[OK] lcd_st7735s + PWM3 + rotation MMIO (%u fg px)\n", fg_pixels);
}

static void host_feed_ep0_setup(const uint8_t setup[8])
{
    memcpy(g_soc.musb_rx_fifo[0], setup, 8u);
    g_soc.musb_rx_pos[0]   = 0u;
    g_soc.musb_rx_len[0]   = 8u;
    g_soc.musb_rxcount[0]  = 8u;
    g_soc.musb_txcsr[0]   |= 0x0001u; /* RxPktRdy */
    g_soc.musb_intrtx     |= 0x0001u;
    g_soc.musb_tx_len[0]   = 0u;
    usb_msc_poll();
}

static void host_feed_ep_out(const uint8_t *data, uint16_t len)
{
    assert(len <= sizeof(g_soc.musb_rx_fifo[2]));
    memcpy(g_soc.musb_rx_fifo[2], data, len);
    g_soc.musb_rx_pos[2]  = 0u;
    g_soc.musb_rx_len[2]  = len;
    g_soc.musb_rxcount[2] = len;
    g_soc.musb_rxcsr[2]  |= 0x0001u; /* RxPktRdy */
    g_soc.musb_intrrx    |= (1u << 2);
    usb_msc_poll();
}

static uint16_t host_drain_ep_in(uint8_t *out, uint16_t max_len)
{
    uint16_t total = 0u;
    for (int iter = 0; iter < 64; iter++) {
        if (g_soc.musb_tx_len[2] > 0u) {
            assert(total + g_soc.musb_tx_len[2] <= max_len);
            memcpy(out + total, g_soc.musb_tx_fifo[2], g_soc.musb_tx_len[2]);
            total = (uint16_t)(total + g_soc.musb_tx_len[2]);
            g_soc.musb_tx_len[2] = 0u;
            g_soc.musb_txcsr[2] &= ~0x0001u;
        }
        usb_msc_poll();
        if (g_soc.musb_tx_len[2] == 0u) {
            break;
        }
    }
    return total;
}

static void host_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t host_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint8_t host_vendor_e0(uint32_t tag, uint8_t subcmd, uint32_t arg1, uint32_t arg2,
                              int is_in, const uint8_t *out_data, uint8_t *in_data, uint16_t len)
{
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    memcpy(cbw, "USBC", 4u);
    host_put_le32(&cbw[4], tag);
    host_put_le32(&cbw[8], len);
    cbw[12] = is_in ? 0x80u : 0x00u;
    cbw[14] = 16u;
    cbw[15] = 0xE0u;
    cbw[16] = subcmd;
    host_put_le32(&cbw[17], arg1);
    host_put_le32(&cbw[21], arg2);
    host_feed_ep_out(cbw, 31u);
    if (!is_in && len > 0u && out_data) {
        host_feed_ep_out(out_data, len);
    }
    uint8_t rx[128];
    uint16_t got = host_drain_ep_in(rx, sizeof(rx));
    uint16_t exp_data = (is_in && len > 0u) ? len : 0u;
    assert(got == (uint16_t)(exp_data + 13u));
    if (exp_data > 0u && in_data) {
        memcpy(in_data, rx, exp_data);
    }
    assert(memcmp(&rx[exp_data], "USBS", 4u) == 0);
    return rx[exp_data + 12u];
}

static uint8_t  g_mock_audio_cfg[4] = { 0u, 20u, 1u, 0u };
static int16_t  g_mock_rms_x10      = -185;
static uint16_t g_mock_clips        = 3u;

static int mock_usb_audio_cfg_cb(int set, uint8_t cfg[4], int16_t *rms_x10, uint16_t *clips)
{
    if (set) {
        for (int i = 0; i < 4; i++) {
            if (cfg[i] != 0xFFu) {
                g_mock_audio_cfg[i] = cfg[i];
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        cfg[i] = g_mock_audio_cfg[i];
    }
    if (rms_x10) *rms_x10 = g_mock_rms_x10;
    if (clips)   *clips   = g_mock_clips;
    return 0;
}

static uint32_t host_mock_exp_fn(const openfw_exp_api_t *api, uint32_t entry_off, uint32_t arg)
{
    assert(api->magic == OPENFW_EXP_ABI_MAGIC);
    api->log_puts("HOST_EXP_OK\n");
    api->out_buf[0] = 0x11u;
    api->out_buf[1] = 0x22u;
    api->out_buf[2] = 0x33u;
    api->out_buf[3] = 0x44u;
    *api->out_len_ptr = 4u;
    return arg + entry_off + 100u;
}

static void test_sdio_exfat_and_usb_msc(const char *exfat_img_path)
{
    soc_reset();
    g_soc.sd_num_sectors = 16384u; /* 8 MiB SDHC */
    g_soc.sd_sectors = (uint8_t *)calloc(g_soc.sd_num_sectors, 512u);
    assert(g_soc.sd_sectors != NULL);

    FILE *f = fopen(exfat_img_path, "rb");
    assert(f != NULL);
    size_t got_bytes = fread(g_soc.sd_sectors, 512u, 4096u, f);
    fclose(f);
    assert(got_bytes == 4096u);

    sd_card_t card;
    assert(sd_init(&card) == 0);
    assert(card.initialized == 1 && card.is_sdhc == 1);
    assert(card.rca == 0x1234u && card.sector_count == 16384u);

    blkdev_t blk = sd_as_blkdev(&card);
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &blk) == 0);
    exfat_set_timestamp(&vol, 0x5C210000u);

    exfat_writer_t wf;
    assert(exfat_mkdir(&vol, "MMIO") == 0);
    assert(exfat_create_file(&vol, "MMIO", "MMIO_TEST.TXT", &wf) == 0);
    static const char msg[] = "Hello via SL6801 SDIO!";
    assert(exfat_write(&wf, msg, sizeof(msg) - 1u) == 0);
    assert(exfat_close_file(&wf) == 0);

    exfat_entry_t ent;
    char rb[64];
    uint32_t got_n = 0u;
    assert(exfat_find_path(&vol, "MMIO/MMIO_TEST.TXT", &ent) == 0);
    assert(ent.size == sizeof(msg) - 1u);
    assert(exfat_read_file(&vol, &ent, 0u, rb, sizeof(rb), &got_n) == 0);
    assert(got_n == sizeof(msg) - 1u && memcmp(rb, msg, got_n) == 0);

    uint8_t sector_buf[512];
    assert(sd_read_blocks(&card, 0u, 1u, sector_buf) == 0);
    assert(memcmp(sector_buf + 3, "EXFAT   ", 8) == 0);

    /* Multi-block CMD25 write + CMD18 readback verification */
    static uint8_t mb_wr[8 * 512];
    static uint8_t mb_rd[8 * 512];
    for (uint32_t i = 0u; i < sizeof(mb_wr); i++) {
        mb_wr[i] = (uint8_t)((i * 29u + 7u) & 0xFFu);
    }
    assert(sd_write_blocks(&card, 8000u, 8u, mb_wr) == 0);
    assert(sd_read_blocks(&card, 8000u, 8u, mb_rd) == 0);
    assert(memcmp(mb_rd, mb_wr, sizeof(mb_wr)) == 0);
    /* Verify CMD13 uses silicon-proven command word (0x254D) */
    assert((g_soc.sdio_last_cmd13 & 0xFFFFu) == 0x254Du);

    /* Verify near_end clamps multi-block chunk before last 3 sectors instead of downgrading all */
    static uint8_t tail_wr[14 * 512];
    static uint8_t tail_rd[14 * 512];
    for (uint32_t i = 0u; i < sizeof(tail_wr); i++) {
        tail_wr[i] = (uint8_t)((i * 13u + 5u) & 0xFFu);
    }
    g_soc.sdio_cmd24_cnt = 0u;
    g_soc.sdio_cmd25_cnt = 0u;
    assert(sd_write_blocks(&card, 16370u, 14u, tail_wr) == 0);
    assert(g_soc.sdio_cmd25_cnt == 1u && g_soc.sdio_cmd24_cnt == 3u);
    g_soc.sdio_cmd17_cnt = 0u;
    g_soc.sdio_cmd18_cnt = 0u;
    assert(sd_read_blocks(&card, 16370u, 14u, tail_rd) == 0);
    assert(g_soc.sdio_cmd18_cnt == 1u && g_soc.sdio_cmd17_cnt == 3u);
    assert(memcmp(tail_rd, tail_wr, sizeof(tail_wr)) == 0);
    assert(sd_read_blocks(&card, 16380u, 5u, tail_rd) != 0); /* Out of bounds rejected */
    assert(sd_flush(&card) == 0);

    /* Now exercise full USB PHY + MUSB enumeration + SCSI BOT R/W over SDIO */
    usb_phy_det_arm();
    assert(usb_phy_det_poll_host() == 0);
    g_soc.usb_phy_int_status = 0x1u;
    assert(usb_phy_det_poll_host() == 1);

    assert(usb_msc_start(&blk) == 0);
    assert((g_soc.musb_power & 0x60u) == 0x60u); /* SoftConn | HS_Enab */
    assert((g_soc.musb_testmode & 0x20u) == 0u); /* Force_FS cleared */

    /* Simulate USB 2.0 High-Speed Bus Reset (MUSB_POWER.HS_Mode = 0x10) */
    g_soc.musb_power  |= 0x10u;
    g_soc.musb_intrusb = 0x04u;
    usb_msc_poll();
    assert(g_soc.musb_txmaxp[2] == 512u && g_soc.musb_rxmaxp[2] == 512u);

    static const uint8_t get_dev[8] = { 0x80u, 0x06u, 0x00u, 0x01u, 0x00u, 0x00u, 18u, 0x00u };
    host_feed_ep0_setup(get_dev);
    assert(g_soc.musb_tx_len[0] == 18u);
    assert(g_soc.musb_tx_fifo[0][8] == 0x09u && g_soc.musb_tx_fifo[0][9] == 0x12u);

    static const uint8_t get_cfg_desc[8] = { 0x80u, 0x06u, 0x00u, 0x02u, 0x00u, 0x00u, 32u, 0x00u };
    host_feed_ep0_setup(get_cfg_desc);
    assert(g_soc.musb_tx_len[0] == 32u);
    assert(g_soc.musb_tx_fifo[0][20] == 0x82u && g_soc.musb_tx_fifo[0][22] == 0x00u && g_soc.musb_tx_fifo[0][23] == 0x02u);

    static const uint8_t get_qual[8] = { 0x80u, 0x06u, 0x00u, 0x06u, 0x00u, 0x00u, 10u, 0x00u };
    host_feed_ep0_setup(get_qual);
    assert(g_soc.musb_tx_len[0] == 10u && g_soc.musb_tx_fifo[0][1] == 0x06u);

    static const uint8_t get_other[8] = { 0x80u, 0x06u, 0x00u, 0x07u, 0x00u, 0x00u, 32u, 0x00u };
    host_feed_ep0_setup(get_other);
    assert(g_soc.musb_tx_len[0] == 32u && g_soc.musb_tx_fifo[0][1] == 0x07u && g_soc.musb_tx_fifo[0][22] == 64u);

    static const uint8_t set_addr[8] = { 0x00u, 0x05u, 5u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
    host_feed_ep0_setup(set_addr);
    usb_msc_poll(); /* applies deferred address on next ep0_service pass if triggered, or next SETUP */
    static const uint8_t get_cfg[8] = { 0x80u, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 1u, 0x00u };
    host_feed_ep0_setup(get_cfg);
    assert(g_soc.musb_faddr == 5u);
    assert(g_soc.musb_tx_fifo[0][0] == 0u);

    static const uint8_t set_cfg[8] = { 0x00u, 0x09u, 1u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
    host_feed_ep0_setup(set_cfg);
    /* Invalid SET_CONFIGURATION(2) and unsupported CLEAR_FEATURE must STALL EP0 */
    uint32_t stalls_before = g_soc.musb_ep0_stalls;
    static const uint8_t bad_cfg[8] = { 0x00u, 0x09u, 2u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
    host_feed_ep0_setup(bad_cfg);
    assert(g_soc.musb_ep0_stalls == stalls_before + 1u);
    static const uint8_t bad_clr[8] = { 0x02u, 0x01u, 0x01u, 0x00u, 0x82u, 0x00u, 0x00u, 0x00u };
    host_feed_ep0_setup(bad_clr);
    assert(g_soc.musb_ep0_stalls == stalls_before + 2u);
    static const uint8_t get_lun[8] = { 0xA1u, 0xFEu, 0x00u, 0x00u, 0x00u, 0x00u, 1u, 0x00u };
    host_feed_ep0_setup(get_lun);
    assert(g_soc.musb_tx_len[0] == 1u && g_soc.musb_tx_fifo[0][0] == 0u);

    /* SCSI INQUIRY CBW */
    uint8_t cbw[31] = {
        0x55, 0x53, 0x42, 0x43,  0x01, 0x00, 0x00, 0x00,
        36u, 0u, 0u, 0u,         0x80u, 0u, 6u,
        0x12u, 0u, 0u, 0u, 36u, 0u,
    };
    host_feed_ep_out(cbw, 31u);
    uint8_t in_buf[600];
    uint16_t n_in = host_drain_ep_in(in_buf, sizeof(in_buf));
    assert(n_in == 36u + 13u);
    assert(memcmp(in_buf + 8, "Cassini ", 8) == 0);
    assert(in_buf[36 + 12] == 0u);

    /* SCSI READ(10) LBA 0 (1 sector = single 512 B HS packet + 13 B CSW) */
    memset(cbw, 0, sizeof(cbw));
    memcpy(cbw, "USBC\x02\x00\x00\x00", 8);
    cbw[8]  = 0x00u; cbw[9] = 0x02u; /* 512 B */
    cbw[12] = 0x80u; cbw[14] = 10u;
    cbw[15] = 0x28u; cbw[23] = 1u;   /* LBA 0, 1 blk */
    host_feed_ep_out(cbw, 31u);
    n_in = host_drain_ep_in(in_buf, sizeof(in_buf));
    assert(n_in == 512u + 13u);
    assert(memcmp(in_buf, sector_buf, 512u) == 0);
    assert(in_buf[512 + 12] == 0u);

    /* SCSI WRITE(10) LBA 5000 (single 512 B HS packet) -> verifies real SD write */
    uint8_t wbuf[512];
    for (uint32_t i = 0u; i < 512u; i++) {
        wbuf[i] = (uint8_t)(0xA5u ^ i);
    }
    memset(cbw, 0, sizeof(cbw));
    memcpy(cbw, "USBC\x03\x00\x00\x00", 8);
    cbw[8]  = 0x00u; cbw[9] = 0x02u;
    cbw[12] = 0x00u; cbw[14] = 10u;
    cbw[15] = 0x2Au;
    cbw[17] = (uint8_t)(5000u >> 24);
    cbw[18] = (uint8_t)(5000u >> 16);
    cbw[19] = (uint8_t)(5000u >> 8);
    cbw[20] = (uint8_t)(5000u);
    cbw[23] = 1u;
    host_feed_ep_out(cbw, 31u);
    host_feed_ep_out(wbuf, 512u);
    n_in = host_drain_ep_in(in_buf, sizeof(in_buf));
    assert(n_in == 13u && in_buf[12] == 0u);
    assert(memcmp(&g_soc.sd_sectors[5000u * 512u], wbuf, 512u) == 0);

    /* Verify clean Full-Speed fallback on Bus Reset without HS_Mode (0x10)
     * and that DEVICE_QUALIFIER stalls in FS mode per USB 2.0 9.6.2 */
    g_soc.musb_power  &= ~0x10u;
    g_soc.musb_intrusb = 0x04u;
    usb_msc_poll();
    assert(g_soc.musb_txmaxp[2] == 64u && g_soc.musb_rxmaxp[2] == 64u);
    host_feed_ep0_setup(get_cfg_desc);
    assert(g_soc.musb_tx_len[0] == 32u && g_soc.musb_tx_fifo[0][22] == 64u);
    uint32_t fs_stalls = g_soc.musb_ep0_stalls;
    host_feed_ep0_setup(get_qual);
    assert(g_soc.musb_ep0_stalls == fs_stalls + 1u);
    host_feed_ep0_setup(set_cfg);

    uint32_t rx_p = 0u, tx_p = 0u;
    usb_msc_get_stats(&rx_p, &tx_p);
    assert(rx_p > 0u && tx_p > 0u);
    uint64_t rx_b = 0u, tx_b = 0u;
    usb_msc_get_bytes(&rx_b, &tx_b);
    assert(rx_b >= 512u && tx_b >= 512u);

    /* OpenFW 0xE0 experiment & memory commands (0x0A, 0x05, 0x07, 0x06, 0x09, 0x02, 0x08) */
    assert(host_vendor_e0(10u, 0x0Au, 0u, 0u, 0, NULL, NULL, 0u) == 0u); /* LOG_RESET */
    assert(g_openfw_log.write_idx == 0u);

    uint8_t exp_bin[32];
    for (uint32_t i = 0u; i < 32u; i++) {
        exp_bin[i] = (uint8_t)(0x30u + i);
    }
    assert(host_vendor_e0(11u, 0x05u, 0u, 32u, 0, exp_bin, NULL, 32u) == 0u); /* EXP_WRITE */

    uint8_t rb_arena[32];
    assert(host_vendor_e0(12u, 0x07u, OPENFW_EXP_ARENA_ADDR, 32u, 1, NULL, rb_arena, 32u) == 0u); /* MEM_READ */
    assert(memcmp(rb_arena, exp_bin, 32u) == 0);

    openfw_exp_set_host_runner(host_mock_exp_fn);
    openfw_exp_init_api(NULL);
    uint8_t exp_res[32];
    assert(host_vendor_e0(13u, 0x06u, 0u, 77u, 1, NULL, exp_res, 32u) == 0u); /* EXP_RUN */
    assert(host_get_le32(&exp_res[0]) == OPENFW_EXP_RES_MAGIC);
    assert(host_get_le32(&exp_res[4]) == 177u);
    assert(host_get_le32(&exp_res[12]) == 4u);
    assert(host_get_le32(&exp_res[16]) == 12u);
    assert(host_get_le32(&exp_res[20]) == 1u);

    uint8_t rb_stat[32];
    assert(host_vendor_e0(14u, 0x09u, 0u, 0u, 1, NULL, rb_stat, 32u) == 0u); /* EXP_STATUS */
    assert(memcmp(rb_stat, exp_res, 32u) == 0);

    uint8_t rb_out[4];
    assert(host_vendor_e0(15u, 0x07u, OPENFW_EXP_OUT_ADDR, 4u, 1, NULL, rb_out, 4u) == 0u); /* MEM_READ */
    assert(rb_out[0] == 0x11u && rb_out[1] == 0x22u && rb_out[2] == 0x33u && rb_out[3] == 0x44u);

    uint8_t rb_log[12];
    assert(host_vendor_e0(16u, 0x02u, 0u, 12u, 1, NULL, rb_log, 12u) == 0u); /* READ_LOG */
    assert(memcmp(rb_log, "HOST_EXP_OK\n", 12u) == 0);

    /* Verify GET_INFO[28..31] exposes g_prev_fault_code and 0xE0 REBOOT mode=3 is refused in unattended mode */
    g_prev_fault_code = 0xFA017006u;
    uint8_t info_buf[32];
    assert(host_vendor_e0(165u, 0x01u, 0u, 0u, 1, NULL, info_buf, 32u) == 0u);
    assert(host_get_le32(&info_buf[28]) == 0xFA017006u);
    g_prev_fault_code = 0u;
    assert(host_vendor_e0(166u, 0x08u, 3u, 0u, 0, NULL, NULL, 0u) == 1u);

    /* 0xE0 0x0B AUDIO_CFG GET/SET + range check */
    g_mock_audio_cfg[0] = 0u;
    g_mock_audio_cfg[1] = 20u;
    g_mock_audio_cfg[2] = 1u;
    g_mock_audio_cfg[3] = 0u;
    usb_msc_set_audio_cfg_cb(mock_usb_audio_cfg_cb);
    uint8_t acfg_out[4] = { 1u, 0xFFu, 2u, 1u };
    assert(host_vendor_e0(167u, 0x0Bu, 0u, 0u, 0, acfg_out, NULL, 4u) == 0u);
    uint8_t acfg_in[8];
    assert(host_vendor_e0(168u, 0x0Bu, 0u, 0u, 1, NULL, acfg_in, 8u) == 0u);
    assert(acfg_in[0] == 1u && acfg_in[1] == 20u && acfg_in[2] == 2u && acfg_in[3] == 1u);
    assert((int16_t)(acfg_in[4] | (acfg_in[5] << 8)) == -185);
    assert((uint16_t)(acfg_in[6] | (acfg_in[7] << 8)) == 3u);
    uint8_t acfg_bad[4] = { 9u, 20u, 1u, 0u };
    assert(host_vendor_e0(169u, 0x0Bu, 0u, 0u, 0, acfg_bad, NULL, 4u) == 1u);
    usb_msc_set_audio_cfg_cb(NULL);

    /* 0xE0 0x04 SET_RTC validation: reject invalid weekday=7 & non-leap Feb 29, accept leap Feb 29 */
    uint8_t rtc_bad_wd[8]  = { 0xEAu, 0x07u, 11u, 19u, 12u, 0u, 0u, 7u };  /* 2026-11-19 wday=7 */
    uint8_t rtc_bad_feb[8] = { 0xEAu, 0x07u, 2u, 29u, 12u, 0u, 0u, 6u };   /* 2026-02-29 invalid */
    uint8_t rtc_ok_leap[8] = { 0xE8u, 0x07u, 2u, 29u, 12u, 0u, 0u, 3u };   /* 2024-02-29 valid */
    assert(host_vendor_e0(170u, 0x04u, 0u, 0u, 0, rtc_bad_wd, NULL, 8u) == 1u);
    assert(host_vendor_e0(171u, 0x04u, 0u, 0u, 0, rtc_bad_feb, NULL, 8u) == 1u);
    assert(host_vendor_e0(172u, 0x04u, 0u, 0u, 0, rtc_ok_leap, NULL, 8u) == 0u);

    /* Deferred 0x08 REBOOT (mode=1 DFU): CSW PASS arrives before reboot triggers */
    assert(host_vendor_e0(17u, 0x08u, 1u, 0u, 0, NULL, NULL, 0u) == 0u);
    assert(usb_msc_is_active() == 1);
    g_systick_ms += 300u;
    g_wfi_armed = 1;
    if (setjmp(g_wfi_jmp) == 0) {
        usb_msc_poll();
    }
    assert(usb_msc_is_active() == 0);
    assert((g_soc.pmu_regs[0x23u] & 0xC0u) == 0xC0u);
    assert((g_soc.pmu_regs[0x00u] & 0x01u) == 0x01u);
    openfw_exp_set_host_runner(NULL);

    free(g_soc.sd_sectors);
    g_soc.sd_sectors = NULL;
    printf("[OK] sdio + exfat + usb_musb + usb_scsi + 0xE0 E2E MMIO (rx=%u tx=%u)\n", rx_p, tx_p);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test_exfat.img>\n", argv[0]);
        return 2;
    }
    test_wdt_timer();
    test_uart_and_log_ring();
    test_rtc_driver();
    test_pmu_driver();
    test_keys_driver();
    test_exp_runner_guards(argv[1]);
    test_lcd_driver();
    test_sdio_exfat_and_usb_msc(argv[1]);
    printf("ALL 8 MMIO DRIVER SUITES PASSED\n");
    return 0;
}
