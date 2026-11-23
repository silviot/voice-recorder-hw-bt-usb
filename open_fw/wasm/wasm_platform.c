/*
 * Smartlink SL6801 Virtual SoC & WebAssembly Platform for Browser Emulation
 *
 * Compiles the real standalone Cassini OpenFW C firmware + LVGL v9.2.2 + EEZ
 * Studio UI + exFAT + Audio DSP/Recorder/Player/CWX directly to wasm32 with
 * zero libc/WASI dependencies.
 *
 * Runs inside a Web Worker with SharedArrayBuffer + Atomics.wait so openfw_main()
 * executes unmodified with <1 ms button response, live 16 kHz stereo Web Audio
 * playback, live browser microphone (or synthetic voice) recording, and a real
 * in-memory exFAT microSD card.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_cwx.h"
#include "openfw_exp.h"
#include "openfw_fs.h"
#include "openfw_wav.h"

extern int  openfw_main(void);
extern void DMAC_CH0_IRQHandler(void);
extern void DMAC_CH1_IRQHandler(void);
extern void keys_scan_tick(uint32_t now_ms);

/* ============================================================================
 * Host Imports (provided by web_emu/worker.js)
 * ============================================================================ */
__attribute__((import_module("env"), import_name("host_yield")))
extern uint32_t host_yield(uint32_t wait_ms);

__attribute__((import_module("env"), import_name("host_uart_putc")))
extern void host_uart_putc(uint32_t ch);

__attribute__((import_module("env"), import_name("host_dac_push")))
extern void host_dac_push(const int16_t *pcm, uint32_t num_stereo_frames, uint32_t sample_rate);

__attribute__((import_module("env"), import_name("host_adc_pull")))
extern uint32_t host_adc_pull(int16_t *dst, uint32_t num_frames, uint32_t channels);

__attribute__((import_module("env"), import_name("host_on_poweroff")))
extern void host_on_poweroff(uint32_t mode);

__attribute__((import_module("env"), import_name("host_get_wall_clock")))
extern void host_get_wall_clock(uint32_t *out_ymd_hms);

/* ============================================================================
 * Freestanding Memory Primitives (Clang wasm32 intrinsics)
 * ============================================================================ */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *memset(void *dst, int val, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t b = (uint8_t)val;
    for (size_t i = 0; i < n; i++) {
        d[i] = b;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

/* ============================================================================
 * Firmware Shared Globals (normally defined in ARM startup.c)
 * ============================================================================ */
volatile uint32_t g_systick_ms                 = 0u;
volatile uint8_t  g_openfw_boot_strike_counted = 0u;
volatile uint8_t  g_openfw_xip_mode            = OPENFW_XIP_MODE_QUAD_48M;

/* ============================================================================
 * Virtual 16 MiB microSD Card (32768 x 512 B sectors — zero-init BSS in WASM)
 * ============================================================================ */
#define WASM_SD_SECTORS  32768u
#define WASM_SD_BYTES    (WASM_SD_SECTORS * 512u)

static uint8_t g_sd_img[WASM_SD_BYTES];
static int     g_sd_formatted = 0;

/* Scratch transfer buffer for JS file listing / WAV download */
static uint8_t g_xfer_buf[65536];

/* ============================================================================
 * Virtual SL6801 SoC Register State
 * ============================================================================ */
static struct {
    uint64_t us_time;
    uint32_t last_tick_ms;
    uint32_t rtc_last_sec_ms;

    uint32_t systick_csr;
    uint32_t systick_rvr;
    uint32_t systick_cvr;
    uint32_t wdt_en;
    uint32_t wdt_mode;
    uint32_t wdt_cfg;
    uint32_t wdt_ctrl1;
    uint32_t aon_wdt_pmu_rst_cmd;

    uint32_t aon_clk_cfg;
    uint32_t aon_clk_en;
    uint32_t aon_pwr_ctl0;
    uint32_t aon_rtc_ctrl;
    uint32_t aon_backup[16];
    uint32_t rtc_date;
    uint32_t rtc_time;

    /* PMU RTWI */
    uint32_t rtwi_cmd;
    uint32_t rtwi_wdata;
    uint32_t rtwi_rdata;
    uint8_t  pmu_regs[256];
    uint8_t  vbus_present;
    uint16_t vbat_mv;

    /* GPIO / SARADC / CRU / Audio */
    uint32_t gpio_mux[4][4];
    uint32_t gpio_out[4];
    uint32_t gpio_in[4];
    uint32_t gpio_pad_dir[4];
    uint32_t cru_saradc_clk;
    uint32_t cru_saradc_rst;
    uint32_t saradc_ch1_data;
    uint32_t rom_rst_mask;
    uint32_t speaker_pa;

    /* Audio Codec + DMAC CH0 (DAC TX) & CH1 (ADC RX) */
    uint32_t cru_apll_ctrl;
    uint32_t cru_apll_frac;
    uint32_t codec_regs[256];
    uint32_t dmac_regs[8][16];
    void    *dmac_buf[8];
    uint32_t dmac_len[8];
    uint8_t  dmac_ch0_active;
    uint64_t dmac_ch0_done_us;
    uint8_t  dmac_ch1_active;
    uint64_t dmac_ch1_done_us;

    /* PWM3 + ST7735S 160x80 RGB565 Display */
    uint32_t pwm3_ctrl;
    uint32_t pwm3_period_duty;
    uint32_t lcdc_if_ctrl;
    uint32_t lcdc_xfer_ctrl;
    uint32_t lcdc_cmd_reg;
    uint32_t lcdc_data_len;
    uint32_t lcdc_tx_data;
    uint8_t  st_cur_cmd;
    uint8_t  st_param_idx;
    uint16_t st_x0, st_x1, st_y0, st_y1;
    uint16_t st_cx, st_cy;
    uint8_t  st_madctl;
    uint8_t  st_disp_on;
    uint8_t  st_ram_half;
    uint8_t  st_hi_byte;
    uint32_t fb_dirty;
    uint16_t fb[80][160];

    /* SDIO Controller */
    uint32_t sdio_ctrl;
    uint32_t sdio_cmd;
    uint32_t sdio_arg;
    uint32_t sdio_blksiz;
    uint32_t sdio_bytcnt;
    uint32_t sdio_rintsts;
    uint32_t sdio_resp[4];
    uint32_t sdio_bmod;
    uint32_t sdio_fifo[128];
    uint32_t sdio_fifo_pos;
    uint32_t sdio_fifo_len;
    uint32_t sdio_active_lba;
    uint32_t sdio_blocks_rem;
    int      sdio_acmd41_polls;
    int      sdio_selected;

    /* USB MUSB minimal registers */
    uint32_t sysctl_usb_ctrl;
    uint32_t usb_phy_ctrl;
    uint8_t  musb_power;
    uint8_t  musb_index;
    uint16_t musb_frame;

    /* Power state */
    uint8_t  powered_off;
} g_soc;

/* ============================================================================
 * Helpers: Little-Endian & RTC Advancement
 * ============================================================================ */
static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint8_t days_in_month(uint32_t y, uint32_t m)
{
    static const uint8_t k_dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2u) {
        int leap = ((y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u));
        return leap ? 29u : 28u;
    }
    return (m >= 1u && m <= 12u) ? k_dim[m - 1u] : 30u;
}

static void soc_rtc_tick_1s(void)
{
    rtc_datetime_t dt;
    rtc_get(&dt);
    if (dt.year < 2000u || dt.month < 1u || dt.month > 12u || dt.day < 1u) {
        return;
    }
    dt.second++;
    if (dt.second >= 60u) {
        dt.second = 0u;
        dt.minute++;
        if (dt.minute >= 60u) {
            dt.minute = 0u;
            dt.hour++;
            if (dt.hour >= 24u) {
                dt.hour = 0u;
                dt.day++;
                if (dt.day > days_in_month(dt.year, dt.month)) {
                    dt.day = 1u;
                    dt.month++;
                    if (dt.month > 12u) {
                        dt.month = 1u;
                        dt.year++;
                    }
                }
            }
        }
    }
    rtc_set(&dt);
}

/* ============================================================================
 * Virtual ST7735S Display Controller (160x80 Landscape RGB565)
 * ============================================================================ */
static void st7735s_on_byte(int is_data, uint8_t b)
{
    if (!is_data) {
        g_soc.st_cur_cmd   = b;
        g_soc.st_param_idx = 0u;
        if (b == 0x29u) g_soc.st_disp_on = 1u;
        if (b == 0x28u) g_soc.st_disp_on = 0u;
        if (b == 0x2Cu) {
            g_soc.st_cx       = g_soc.st_x0;
            g_soc.st_cy       = g_soc.st_y0;
            g_soc.st_ram_half = 0u;
        }
        return;
    }

    switch (g_soc.st_cur_cmd) {
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
            uint16_t px = (uint16_t)(((uint16_t)g_soc.st_hi_byte << 8) | b);
            if (g_soc.st_cx < 160u && g_soc.st_cy < 80u) {
                g_soc.fb[g_soc.st_cy][g_soc.st_cx] = px;
                g_soc.fb_dirty = 1u;
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

/* ============================================================================
 * Virtual SDIO Controller (1-bit PIO over g_sd_img[])
 * ============================================================================ */
static void sdio_load_sector_to_fifo(uint32_t lba)
{
    if (lba >= WASM_SD_SECTORS) {
        memset(g_soc.sdio_fifo, 0, sizeof(g_soc.sdio_fifo));
        return;
    }
    const uint8_t *src = &g_sd_img[lba * 512u];
    for (uint32_t w = 0u; w < 128u; w++) {
        g_soc.sdio_fifo[w] = (uint32_t)src[w * 4u + 0u]
                           | ((uint32_t)src[w * 4u + 1u] << 8)
                           | ((uint32_t)src[w * 4u + 2u] << 16)
                           | ((uint32_t)src[w * 4u + 3u] << 24);
    }
}

static void sdio_exec_cmd(uint32_t val)
{
    uint32_t cmd = val & 0x3Fu;
    g_soc.sdio_cmd = val & ~0x80000000u;
    if (val & 0x00200000u) {
        return; /* update clock only */
    }
    g_soc.sdio_rintsts |= SDIO_INT_CMD_DONE;

    switch (cmd) {
    case 0u:
        g_soc.sdio_selected     = 0;
        g_soc.sdio_acmd41_polls = 0;
        break;
    case 8u:
        g_soc.sdio_resp[0] = g_soc.sdio_arg;
        break;
    case 55u:
        g_soc.sdio_resp[0] = 0x00000120u;
        break;
    case 41u:
        g_soc.sdio_acmd41_polls++;
        g_soc.sdio_resp[0] = (g_soc.sdio_acmd41_polls >= 2) ? 0xC0FF8000u : 0x00FF8000u;
        break;
    case 2u:
        g_soc.sdio_resp[0] = 0x11223344u;
        g_soc.sdio_resp[1] = 0x55667788u;
        g_soc.sdio_resp[2] = 0x99AABBCBu;
        g_soc.sdio_resp[3] = 0x00534400u;
        break;
    case 3u:
        g_soc.sdio_resp[0] = 0x12340000u;
        break;
    case 9u:
    {
        uint32_t c_size = (WASM_SD_SECTORS / 1024u) - 1u;
        g_soc.sdio_resp[3] = (1u << 30);
        g_soc.sdio_resp[2] = (c_size >> 16) & 0x3Fu;
        g_soc.sdio_resp[1] = (c_size & 0xFFFFu) << 16;
        g_soc.sdio_resp[0] = 0u;
        break;
    }
    case 7u:
        g_soc.sdio_selected = 1;
        g_soc.sdio_resp[0]  = 0x00000700u;
        break;
    case 12u:
        g_soc.sdio_blocks_rem = 0u;
        g_soc.sdio_resp[0]    = 0x00000900u;
        break;
    case 13u:
        g_soc.sdio_resp[0] = 0x00000900u;
        break;
    case 17u:
    case 18u:
    {
        uint32_t lba  = g_soc.sdio_arg;
        uint32_t blks = (cmd == 18u)
            ? ((g_soc.sdio_bytcnt == 0u) ? 0xFFFFFFFFu : (g_soc.sdio_bytcnt / 512u))
            : 1u;
        sdio_load_sector_to_fifo(lba);
        g_soc.sdio_active_lba = lba;
        g_soc.sdio_blocks_rem = (blks == 0xFFFFFFFFu) ? 0xFFFFFFFFu : (blks - 1u);
        g_soc.sdio_fifo_pos   = 0u;
        g_soc.sdio_fifo_len   = 128u;
        if (g_soc.sdio_blocks_rem == 0u) {
            g_soc.sdio_rintsts |= SDIO_INT_DTO;
        }
        break;
    }
    case 24u:
    case 25u:
    {
        uint32_t blks = (cmd == 25u) ? (g_soc.sdio_bytcnt / 512u) : 1u;
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

/* ============================================================================
 * Audio DMAC Scheduling (CH0 DAC TX @ 16 kHz stereo, CH1 ADC RX @ 16 kHz)
 * ============================================================================ */
void openfw_host_dmac_register_ptr(uint32_t ch, void *buf, uint32_t len)
{
    if (ch < 8u) {
        g_soc.dmac_buf[ch] = buf;
        g_soc.dmac_len[ch] = len;
    }
}

static uint32_t current_dac_sample_rate(void)
{
    uint32_t dig = g_soc.codec_regs[(CODEC_DAC_DIG_CFG_ADDR - AUDIO_CODEC_BASE) >> 2];
    uint32_t sel = (dig >> 8) & 0xFu;
    if (sel == 7u) return 48000u;
    if (sel == 8u) return 44100u;
    if (sel == 5u) return 24000u;
    return 16000u;
}

static void pump_audio_dmac(void)
{
    /* CH0: DAC playback completion */
    if (g_soc.dmac_ch0_active && g_soc.us_time >= g_soc.dmac_ch0_done_us) {
        g_soc.dmac_ch0_active = 0u;
        g_soc.dmac_regs[0][0x24u >> 2] |= DMAC_IRQ_TC;
        DMAC_CH0_IRQHandler();
    }

    /* CH1: ADC microphone capture completion (2048 frames = 128 ms @ 16 kHz) */
    if (g_soc.dmac_ch1_active && g_soc.us_time >= g_soc.dmac_ch1_done_us) {
        g_soc.dmac_ch1_active = 0u;
        int16_t *dst = (int16_t *)g_soc.dmac_buf[1];
        uint32_t len_bytes = g_soc.dmac_len[1];
        if (dst && len_bytes >= 4096u) {
            uint32_t channels = (len_bytes >= 8192u) ? 2u : 1u;
            uint32_t frames   = len_bytes / (channels * 2u);
            (void)host_adc_pull(dst, frames, channels);
        }
        g_soc.dmac_regs[1][0x24u >> 2] |= DMAC_IRQ_TC;
        DMAC_CH1_IRQHandler();
    }
}

/* ============================================================================
 * Virtual Clock / Tick Synchronization
 * ============================================================================ */
static void sync_time_ms(uint32_t now_ms)
{
    if (now_ms < g_soc.last_tick_ms) {
        now_ms = g_soc.last_tick_ms;
    }
    uint32_t delta = now_ms - g_soc.last_tick_ms;
    if (delta > 250u) {
        /* Cap large tab-background jumps so timers don't spin millions of ticks */
        delta = 250u;
    }
    for (uint32_t i = 1u; i <= delta; i++) {
        g_systick_ms = g_soc.last_tick_ms + i;
        g_soc.us_time = (uint64_t)g_systick_ms * 1000ULL;
        keys_scan_tick(g_systick_ms);
        pump_audio_dmac();
        if ((uint32_t)(g_systick_ms - g_soc.rtc_last_sec_ms) >= 1000u) {
            g_soc.rtc_last_sec_ms += 1000u;
            soc_rtc_tick_1s();
        }
    }
    g_soc.last_tick_ms = now_ms;
    g_systick_ms       = now_ms;
    uint64_t wall_us   = (uint64_t)now_ms * 1000ULL;
    if (wall_us > g_soc.us_time) {
        g_soc.us_time = wall_us;
    }
    g_soc.musb_frame   = (uint16_t)(now_ms & 0x7FFu);
    pump_audio_dmac();
}

/* ============================================================================
 * MMIO Hooks (OPENFW_HOST_MMIO)
 * ============================================================================ */
uint32_t openfw_mmio_rd32(uint32_t addr)
{
    if (addr == SYSCTL_CPU_ID_ADDR) return 0u;
    if (addr == SYSTICK_CSR_ADDR)   return g_soc.systick_csr;
    if (addr == SYSTICK_RVR_ADDR)   return g_soc.systick_rvr;
    if (addr == SYSTICK_CVR_ADDR)   return g_soc.systick_cvr;

    if (addr == US_TIMER_HI_ADDR)   return (uint32_t)(g_soc.us_time >> 32);
    if (addr == US_TIMER_LO_ADDR) {
        g_soc.us_time += 25ULL;
        return (uint32_t)(g_soc.us_time & 0xFFFFFFFFu);
    }

    if (addr == UART0_STAT_ADDR)    return 16u; /* TX FIFO always has space */
    if (addr == WDT_EN_ADDR)        return g_soc.wdt_en;
    if (addr == WDT_MODE_ADDR)      return g_soc.wdt_mode;
    if (addr == WDT_CFG_ADDR)       return g_soc.wdt_cfg;
    if (addr == WDT_CTRL1_ADDR)     return g_soc.wdt_ctrl1;
    if (addr == AON_WDT_PMU_RST_CMD_ADDR) return g_soc.aon_wdt_pmu_rst_cmd;

    if (addr == AON_CLK_CFG_ADDR)   return g_soc.aon_clk_cfg;
    if (addr == AON_CLK_STAT_ADDR)  return 0u;
    if (addr == AON_CLK_EN_ADDR)    return g_soc.aon_clk_en;
    if (addr == AON_PWR_CTL0_ADDR)  return g_soc.aon_pwr_ctl0;
    if (addr == AON_RTC_CTRL_ADDR)  return g_soc.aon_rtc_ctrl;
    if (addr >= AON_BACKUP_ADDR(0u) && addr <= AON_BACKUP_ADDR(15u)) {
        return g_soc.aon_backup[(addr - AON_BACKUP_ADDR(0u)) >> 2];
    }
    if (addr == RTC_DATE_ADDR)      return g_soc.rtc_date;
    if (addr == RTC_TIME_ADDR)      return g_soc.rtc_time;

    if (addr == RTWI_CMD_ADDR)      return g_soc.rtwi_cmd;
    if (addr == RTWI_RDATA_ADDR)    return g_soc.rtwi_rdata;
    if (addr == RTWI_STATUS_ADDR)   return 0u;

    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u) {
        uint32_t port = (addr - GPIO_BASE) >> 6;
        uint32_t off  = (addr - GPIO_BASE) & 0x3Fu;
        if (off < 0x10u)  return g_soc.gpio_mux[port][off >> 2];
        if (off == 0x10u) return g_soc.gpio_in[port];
        if (off == 0x24u) return g_soc.gpio_pad_dir[port];
    }

    if (addr == CRU_SARADC_CLK_ADDR)  return g_soc.cru_saradc_clk;
    if (addr == CRU_SARADC_RST_ADDR)  return g_soc.cru_saradc_rst;
    if (addr == SARADC_CH1_DATA_ADDR) return g_soc.saradc_ch1_data;
    if (addr == CRU_APLL_CTRL_ADDR)   return g_soc.cru_apll_ctrl | (1u << 28);
    if (addr == CRU_APLL_FRAC_ADDR)   return g_soc.cru_apll_frac;

    if (addr == PWM3_CTRL_ADDR)        return g_soc.pwm3_ctrl;
    if (addr == PWM3_PERIOD_DUTY_ADDR) return g_soc.pwm3_period_duty;
    if (addr == LCDC_STATUS_ADDR)      return 0x80000000u; /* DONE=1 */
    if (addr == LCDC_XFER_CTRL_ADDR)   return g_soc.lcdc_xfer_ctrl;
    if (addr == LCDC_IF_CTRL_ADDR)     return g_soc.lcdc_if_ctrl;

    if (addr >= AUDIO_CODEC_BASE && addr < AUDIO_CODEC_BASE + 0x400u) {
        return g_soc.codec_regs[(addr - AUDIO_CODEC_BASE) >> 2];
    }
    if (addr >= DMAC_BASE && addr < DMAC_BASE + 8u * 0x40u) {
        uint32_t ch  = (addr - DMAC_BASE) >> 6;
        uint32_t off = (addr - DMAC_BASE) & 0x3Fu;
        return g_soc.dmac_regs[ch][off >> 2];
    }

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
        uint32_t word = (g_soc.sdio_fifo_pos < g_soc.sdio_fifo_len)
            ? g_soc.sdio_fifo[g_soc.sdio_fifo_pos++] : 0u;
        if (g_soc.sdio_fifo_pos == g_soc.sdio_fifo_len && g_soc.sdio_blocks_rem > 0u) {
            g_soc.sdio_active_lba++;
            if (g_soc.sdio_blocks_rem != 0xFFFFFFFFu) {
                g_soc.sdio_blocks_rem--;
            }
            sdio_load_sector_to_fifo(g_soc.sdio_active_lba);
            g_soc.sdio_fifo_pos = 0u;
            g_soc.sdio_fifo_len = 128u;
            if (g_soc.sdio_blocks_rem == 0u) {
                g_soc.sdio_rintsts |= SDIO_INT_DTO;
            }
        }
        return word;
    }

    if (addr == SYSCTL_USB_CTRL_ADDR) return g_soc.sysctl_usb_ctrl;
    if (addr == USB_PHY_CTRL_ADDR)    return g_soc.usb_phy_ctrl;
    return 0u;
}

void openfw_mmio_wr32(uint32_t addr, uint32_t val)
{
    if (addr == SYSTICK_CSR_ADDR) { g_soc.systick_csr = val; return; }
    if (addr == SYSTICK_RVR_ADDR) { g_soc.systick_rvr = val; return; }
    if (addr == SYSTICK_CVR_ADDR) { g_soc.systick_cvr = val; return; }
    if (addr == SCB_AIRCR_ADDR) {
        if ((val & 0xFFFF0004u) == (SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESET)) {
            host_on_poweroff(1u);
        }
        return;
    }
    if (addr == UART0_DATA_ADDR) {
        host_uart_putc(val & 0xFFu);
        return;
    }
    if (addr == WDT_EN_ADDR)   { g_soc.wdt_en = val; return; }
    if (addr == WDT_MODE_ADDR) { g_soc.wdt_mode = val; return; }
    if (addr == WDT_CFG_ADDR)  { g_soc.wdt_cfg = val; return; }
    if (addr == WDT_CTRL1_ADDR){ g_soc.wdt_ctrl1 = val; return; }
    if (addr == AON_WDT_PMU_RST_CMD_ADDR) { g_soc.aon_wdt_pmu_rst_cmd = val; return; }

    if (addr == AON_CLK_CFG_ADDR)  { g_soc.aon_clk_cfg = val; return; }
    if (addr == AON_CLK_EN_ADDR)   { g_soc.aon_clk_en = val; return; }
    if (addr == AON_PWR_CTL0_ADDR) { g_soc.aon_pwr_ctl0 = val; return; }
    if (addr == AON_RTC_CTRL_ADDR) { g_soc.aon_rtc_ctrl = val; return; }
    if (addr == (AON_RTC_BASE + 0xD0u) && (val & 0x01u)) {
        g_soc.powered_off = 1u;
        host_on_poweroff(0u);
        return;
    }
    if (addr >= AON_BACKUP_ADDR(0u) && addr <= AON_BACKUP_ADDR(15u)) {
        g_soc.aon_backup[(addr - AON_BACKUP_ADDR(0u)) >> 2] = val;
        return;
    }
    if (addr == RTC_DATE_ADDR) { g_soc.rtc_date = val; return; }
    if (addr == RTC_TIME_ADDR) { g_soc.rtc_time = val; return; }

    if (addr == RTWI_WDATA_ADDR) { g_soc.rtwi_wdata = val; return; }
    if (addr == RTWI_CMD_ADDR) {
        g_soc.rtwi_cmd = val & ~0x80000000u;
        if (val & 0x80000000u) {
            uint8_t reg = (uint8_t)((val >> 8) & 0xFFu);
            if (val & 1u) {
                g_soc.rtwi_rdata = g_soc.pmu_regs[reg];
            } else {
                uint8_t wv = (uint8_t)(g_soc.rtwi_wdata & 0xFFu);
                if (reg == 0x30u || reg == 0x31u || reg == 0x4Au) {
                    g_soc.pmu_regs[reg] &= (uint8_t)~wv; /* W1C */
                } else {
                    g_soc.pmu_regs[reg] = wv;
                }
            }
        }
        return;
    }

    if (addr >= GPIO_BASE && addr < GPIO_BASE + 0x100u) {
        uint32_t port = (addr - GPIO_BASE) >> 6;
        uint32_t off  = (addr - GPIO_BASE) & 0x3Fu;
        if (off < 0x10u)  { g_soc.gpio_mux[port][off >> 2] = val; return; }
        if (off == 0x14u) { g_soc.gpio_out[port] = val; return; }
        if (off == 0x24u) { g_soc.gpio_pad_dir[port] = val; return; }
    }

    if (addr == CRU_SARADC_CLK_ADDR) { g_soc.cru_saradc_clk = val; return; }
    if (addr == CRU_SARADC_RST_ADDR) { g_soc.cru_saradc_rst = val; return; }
    if (addr == CRU_APLL_CTRL_ADDR)  { g_soc.cru_apll_ctrl = val; return; }
    if (addr == CRU_APLL_FRAC_ADDR)  { g_soc.cru_apll_frac = val; return; }

    if (addr == PWM3_CTRL_ADDR)        { g_soc.pwm3_ctrl = val & ~(1u << 28); return; }
    if (addr == PWM3_PERIOD_DUTY_ADDR) { g_soc.pwm3_period_duty = val; return; }
    if (addr == LCDC_IF_CTRL_ADDR)     { g_soc.lcdc_if_ctrl = val; return; }
    if (addr == LCDC_CMD_REG_ADDR)     { g_soc.lcdc_cmd_reg = val; return; }
    if (addr == LCDC_DATA_LEN_ADDR)    { g_soc.lcdc_data_len = val; return; }
    if (addr == LCDC_TX_DATA_ADDR)     { g_soc.lcdc_tx_data = val; return; }
    if (addr == LCDC_XFER_CTRL_ADDR) {
        g_soc.lcdc_xfer_ctrl = val;
        uint32_t mode = val & 0x0Fu;
        if (mode == 0x07u) {
            st7735s_on_byte(0, (uint8_t)(g_soc.lcdc_cmd_reg & 0xFFu));
        } else if (mode == 0x0Bu) {
            uint32_t n = (g_soc.lcdc_data_len & 0x3u) + 1u;
            uint32_t d = g_soc.lcdc_tx_data;
            for (uint32_t i = 0u; i < n; i++) {
                st7735s_on_byte(1, (uint8_t)((d >> (i * 8u)) & 0xFFu));
            }
        }
        return;
    }

    if (addr >= AUDIO_CODEC_BASE && addr < AUDIO_CODEC_BASE + 0x400u) {
        g_soc.codec_regs[(addr - AUDIO_CODEC_BASE) >> 2] = val;
        return;
    }

    if (addr >= DMAC_BASE && addr < DMAC_BASE + 8u * 0x40u) {
        uint32_t ch  = (addr - DMAC_BASE) >> 6;
        uint32_t off = (addr - DMAC_BASE) & 0x3Fu;
        if (off == 0x24u) {
            g_soc.dmac_regs[ch][off >> 2] &= ~val; /* W1C IRQ stat */
            return;
        }
        uint32_t prev = g_soc.dmac_regs[ch][off >> 2];
        g_soc.dmac_regs[ch][off >> 2] = val;
        if (off == 0x00u) {
            if ((val & DMAC_CTL_CH_EN) && !(prev & DMAC_CTL_CH_EN)) {
                if (ch == 0u) {
                    uint32_t len_b  = g_soc.dmac_len[0];
                    uint32_t frames = len_b / 4u; /* 16-bit stereo */
                    uint32_t rate   = current_dac_sample_rate();
                    if (frames > 0u && g_soc.dmac_buf[0]) {
                        host_dac_push((const int16_t *)g_soc.dmac_buf[0], frames, rate);
                        g_soc.dmac_ch0_active  = 1u;
                        g_soc.dmac_ch0_done_us = g_soc.us_time + ((uint64_t)frames * 1000000ULL) / rate;
                    }
                } else if (ch == 1u) {
                    uint32_t len_b    = g_soc.dmac_len[1];
                    uint32_t channels = (len_b >= 8192u) ? 2u : 1u;
                    uint32_t frames   = (len_b > 0u) ? (len_b / (channels * 2u)) : 2048u;
                    g_soc.dmac_ch1_active  = 1u;
                    g_soc.dmac_ch1_done_us = g_soc.us_time + ((uint64_t)frames * 1000000ULL) / 16000ULL;
                }
            } else if (!(val & DMAC_CTL_CH_EN)) {
                if (ch == 0u) {
                    if (g_soc.dmac_ch0_active) {
                        host_dac_push((const int16_t *)0, 0u, 0u); /* flush browser audio queue */
                    }
                    g_soc.dmac_ch0_active = 0u;
                }
                if (ch == 1u) g_soc.dmac_ch1_active = 0u;
            }
        }
        return;
    }

    if (addr == SDIO_CTRL_ADDR)   { g_soc.sdio_ctrl = val & ~0x7u; return; }
    if (addr == SDIO_ARG_ADDR)    { g_soc.sdio_arg = val; return; }
    if (addr == SDIO_BLKSIZ_ADDR) { g_soc.sdio_blksiz = val; return; }
    if (addr == SDIO_BYTCNT_ADDR) { g_soc.sdio_bytcnt = val; return; }
    if (addr == SDIO_BMOD_ADDR)   { g_soc.sdio_bmod = val; return; }
    if (addr == SDIO_RINTSTS_ADDR){ g_soc.sdio_rintsts &= ~val; return; }
    if (addr == SDIO_CMD_ADDR)    { if (val & 0x80000000u) sdio_exec_cmd(val); return; }
    if (addr == SDIO_FIFO_ADDR) {
        if (g_soc.sdio_fifo_pos < 128u) {
            g_soc.sdio_fifo[g_soc.sdio_fifo_pos++] = val;
        }
        if (g_soc.sdio_fifo_pos == 128u && g_soc.sdio_blocks_rem > 0u) {
            if (g_soc.sdio_active_lba < WASM_SD_SECTORS) {
                uint8_t *dst = &g_sd_img[g_soc.sdio_active_lba * 512u];
                for (uint32_t w = 0u; w < 128u; w++) {
                    put_le32(&dst[w * 4u], g_soc.sdio_fifo[w]);
                }
            }
            g_soc.sdio_active_lba++;
            g_soc.sdio_blocks_rem--;
            g_soc.sdio_fifo_pos = 0u;
            if (g_soc.sdio_blocks_rem == 0u) {
                g_soc.sdio_rintsts |= SDIO_INT_DTO;
            }
        }
        return;
    }

    if (addr == SYSCTL_USB_CTRL_ADDR) { g_soc.sysctl_usb_ctrl = val; return; }
    if (addr == USB_PHY_CTRL_ADDR)    { g_soc.usb_phy_ctrl = val; return; }
}

uint16_t openfw_mmio_rd16(uint32_t addr)
{
    if (addr == MUSB_FRAME_ADDR) return g_soc.musb_frame;
    return 0u;
}

void openfw_mmio_wr16(uint32_t addr, uint16_t val)
{
    (void)addr;
    (void)val;
}

uint8_t openfw_mmio_rd8(uint32_t addr)
{
    if (addr == MUSB_POWER_ADDR) return g_soc.musb_power;
    if (addr == MUSB_INDEX_ADDR) return g_soc.musb_index;
    return 0u;
}

void openfw_mmio_wr8(uint32_t addr, uint8_t val)
{
    if (addr == MUSB_POWER_ADDR) { g_soc.musb_power = val; return; }
    if (addr == MUSB_INDEX_ADDR) { g_soc.musb_index = val; return; }
}

void openfw_mmio_nop(void)
{
    g_soc.us_time += 250ULL;
    if (g_soc.us_time >= (uint64_t)(g_soc.last_tick_ms + 4u) * 1000ULL) {
        uint32_t now_ms = host_yield(1u);
        sync_time_ms(now_ms);
    }
}
void openfw_mmio_dsb(void) { }

void openfw_mmio_wfi(void)
{
    uint32_t wait_ms = g_soc.powered_off ? 100u : 4u;
    uint32_t now_ms  = host_yield(wait_ms);
    sync_time_ms(now_ms);
}

/* ============================================================================
 * Mask ROM Helpers (OPENFW_HOST_MMIO)
 * ============================================================================ */
int rom_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

void rom_gpio_config(uint32_t cfg)
{
    /* PB30 speaker PA control: 0x0001F008 = pull-up ON, 0x0001F000 = Hi-Z OFF */
    if ((cfg & 0xFFFFF800u) == 0x0001F000u) {
        g_soc.speaker_pa = ((cfg & 0xFu) != 0u) ? 1u : 0u;
    }
}

int rom_gpio_read(uint32_t cfg)
{
    uint32_t port = (cfg >> 16) & 3u;
    uint32_t pin  = (cfg >> 11) & 31u;
    return (int)((g_soc.gpio_in[port] >> pin) & 1u);
}

void rom_gpio_write(uint32_t cfg, int val)
{
    uint32_t port = (cfg >> 16) & 3u;
    uint32_t pin  = (cfg >> 11) & 31u;
    if (val) g_soc.gpio_out[port] |= (1u << pin);
    else     g_soc.gpio_out[port] &= ~(1u << pin);
}

size_t rom_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

void *rom_memcpy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }
void *rom_memset(void *dst, int val, size_t n)         { return memset(dst, val, n); }

int  rom_rst_is_released(uint32_t rst_id) { return (int)((g_soc.rom_rst_mask >> (rst_id & 31u)) & 1u); }
void rom_rst_release(uint32_t rst_id)     { g_soc.rom_rst_mask |= (1u << (rst_id & 31u)); }
void rom_rst_hold(uint32_t rst_id)        { g_soc.rom_rst_mask &= ~(1u << (rst_id & 31u)); }
void rom_clk_enable(uint32_t clk_id)      { (void)clk_id; }
void rom_clk_disable(uint32_t clk_id)     { (void)clk_id; }
void rom_clk_div(uint32_t clk_id, uint32_t div) { (void)clk_id; (void)div; }
void rom_clk_set_src(uint32_t clk_id, uint32_t src) { (void)clk_id; (void)src; }
void rom_clk_set_rate(uint32_t clk_id, uint32_t hz) { (void)clk_id; (void)hz; }
uint64_t rom_us_timer_get(void)           { return g_soc.us_time; }
void rom_wdt_reset(uint32_t timeout)      { (void)timeout; }

void rom_us_delay(uint32_t us)
{
    if (us >= 1000u) {
        uint32_t target = g_systick_ms + (us / 1000u);
        while ((int32_t)(g_systick_ms - target) < 0) {
            uint32_t rem = target - g_systick_ms;
            if (rem > 16u) rem = 16u;
            uint32_t now_ms = host_yield(rem);
            sync_time_ms(now_ms);
        }
    } else {
        g_soc.us_time += (uint64_t)us;
    }
}

/* ============================================================================
 * Virtual Bluetooth +XREC Stub (displays pairing PIN in BT Sync screen)
 * ============================================================================ */
static bt_status_t g_wasm_bt;

int bt_xrec_start(exfat_vol_t *vol, int sd_mounted)
{
    (void)vol;
    (void)sd_mounted;
    memset(&g_wasm_bt, 0, sizeof(g_wasm_bt));
    g_wasm_bt.active      = 1u;
    g_wasm_bt.hci_ready   = 1u;
    g_wasm_bt.pair_active = 1u;
    memcpy(g_wasm_bt.pair_pin, "482915", 7u);
    memcpy(g_wasm_bt.dev_name, "HW_76", 6u);
    return 0;
}

void bt_xrec_poll(void) { }
void bt_xrec_stop(void) { g_wasm_bt.active = 0u; g_wasm_bt.pair_active = 0u; }
void bt_xrec_open_pairing(uint32_t duration_ms)
{
    (void)duration_ms;
    g_wasm_bt.pair_active = 1u;
    memcpy(g_wasm_bt.pair_pin, "482915", 7u);
}

void bt_xrec_get_status(bt_status_t *out)
{
    if (out) {
        *out = g_wasm_bt;
    }
}

/* ============================================================================
 * Minimal Spec-Compliant exFAT Formatter + Demo WAV/CWX Seeder
 * ============================================================================ */
static uint32_t exfat_boot_checksum(const uint8_t *sectors_0_to_10)
{
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < 11u * 512u; i++) {
        if (i == 106u || i == 107u || i == 112u) continue;
        sum = ((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + (uint32_t)sectors_0_to_10[i];
    }
    return sum;
}

static uint32_t exfat_table_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < len; i++) {
        sum = ((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + (uint32_t)data[i];
    }
    return sum;
}

static void wasm_format_exfat(void)
{
    memset(g_sd_img, 0, sizeof(g_sd_img));

    const uint32_t part_lba    = 64u;
    const uint32_t part_secs   = WASM_SD_SECTORS - part_lba; /* 32704 */
    const uint32_t fat_off     = 24u;
    const uint32_t fat_len     = 40u;  /* 20 KiB = 5120 FAT entries */
    const uint32_t heap_off    = 64u;
    const uint32_t spc         = 8u;   /* 4096 B cluster */
    const uint32_t clus_cnt    = (part_secs - heap_off) / spc; /* 4080 */

    /* 1. MBR (LBA 0) */
    uint8_t *mbr = &g_sd_img[0];
    mbr[446 + 4] = 0x07u; /* exFAT */
    put_le32(&mbr[446 + 8], part_lba);
    put_le32(&mbr[446 + 12], part_secs);
    mbr[510] = 0x55u;
    mbr[511] = 0xAAu;

    /* 2. exFAT Main VBR (sectors 0..11 of partition) */
    uint8_t *vbr = &g_sd_img[part_lba * 512u];
    vbr[0] = 0xEBu; vbr[1] = 0x76u; vbr[2] = 0x90u;
    memcpy(&vbr[3], "EXFAT   ", 8u);
    put_le64(&vbr[64], (uint64_t)part_lba);
    put_le64(&vbr[72], (uint64_t)part_secs);
    put_le32(&vbr[80], fat_off);
    put_le32(&vbr[84], fat_len);
    put_le32(&vbr[88], heap_off);
    put_le32(&vbr[92], clus_cnt);
    put_le32(&vbr[96], 4u); /* Root directory first cluster = 4 */
    put_le32(&vbr[100], 0x43415331u); /* Serial */
    put_le16(&vbr[104], 0x0100u);     /* Revision 1.00 */
    vbr[108] = 9u; /* 512 B/sector */
    vbr[109] = 3u; /* 8 sectors/cluster (4 KiB) */
    vbr[110] = 1u; /* NumberOfFats */
    vbr[111] = 0x80u;
    vbr[112] = 0xFFu;
    vbr[510] = 0x55u;
    vbr[511] = 0xAAu;

    for (uint32_t s = 1u; s <= 8u; s++) {
        vbr[s * 512u + 510u] = 0x55u;
        vbr[s * 512u + 511u] = 0xAAu;
    }

    uint32_t csum = exfat_boot_checksum(vbr);
    uint8_t *cs_sec = &vbr[11u * 512u];
    for (uint32_t i = 0u; i < 128u; i++) {
        put_le32(&cs_sec[i * 4u], csum);
    }

    /* Backup VBR (sectors 12..23) */
    memcpy(&vbr[12u * 512u], vbr, 12u * 512u);

    /* 3. FAT */
    uint8_t *fat = &g_sd_img[(part_lba + fat_off) * 512u];
    put_le32(&fat[0 * 4], 0xFFFFFFF8u);
    put_le32(&fat[1 * 4], 0xFFFFFFFFu);
    put_le32(&fat[2 * 4], 0xFFFFFFFFu); /* Cluster 2: Allocation Bitmap */
    put_le32(&fat[3 * 4], 0xFFFFFFFFu); /* Cluster 3: Up-Case Table */
    put_le32(&fat[4 * 4], 0xFFFFFFFFu); /* Cluster 4: Root Directory */

    /* 4. Cluster 2: Allocation Bitmap (clusters 2, 3, 4 used = bits 0..2 = 0x07) */
    uint32_t heap_lba = part_lba + heap_off;
    uint8_t *bm = &g_sd_img[heap_lba * 512u];
    bm[0] = 0x07u;
    uint32_t bm_bytes = (clus_cnt + 7u) / 8u;

    /* 5. Cluster 3: Up-Case Table (ASCII 0..127 -> 256 bytes) */
    uint8_t *uc = &g_sd_img[(heap_lba + spc) * 512u];
    for (uint32_t c = 0u; c < 128u; c++) {
        uint16_t u = (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32u) : (uint16_t)c;
        put_le16(&uc[c * 2u], u);
    }
    uint32_t uc_csum = exfat_table_checksum(uc, 256u);

    /* 6. Cluster 4: Root Directory */
    uint8_t *root = &g_sd_img[(heap_lba + 2u * spc) * 512u];

    /* Entry 0: Volume Label (0x83) */
    static const char k_label[] = "OPENFW_EMU";
    root[0] = 0x83u;
    root[1] = 10u;
    for (uint32_t i = 0u; i < 10u; i++) {
        put_le16(&root[2u + i * 2u], (uint16_t)k_label[i]);
    }

    /* Entry 1: Allocation Bitmap (0x81) */
    uint8_t *e_bm = &root[32u];
    e_bm[0] = 0x81u;
    put_le32(&e_bm[20], 2u);
    put_le64(&e_bm[24], (uint64_t)bm_bytes);

    /* Entry 2: Up-Case Table (0x82) */
    uint8_t *e_uc = &root[64u];
    e_uc[0] = 0x82u;
    put_le32(&e_uc[4], uc_csum);
    put_le32(&e_uc[20], 3u);
    put_le64(&e_uc[24], 256u);
}

static int ram_sd_read(void *ctx, uint32_t lba, uint32_t count, void *dst)
{
    (void)ctx;
    if (lba + count > WASM_SD_SECTORS) return -1;
    memcpy(dst, &g_sd_img[lba * 512u], count * 512u);
    return 0;
}

static int ram_sd_write(void *ctx, uint32_t lba, uint32_t count, const void *src)
{
    (void)ctx;
    if (lba + count > WASM_SD_SECTORS) return -1;
    memcpy(&g_sd_img[lba * 512u], src, count * 512u);
    return 0;
}

/* Quarter-wave Q15 sine lookup (0..255 -> 0..2*pi) for crisp demo audio */
static int16_t fast_sin_q15(uint32_t phase)
{
    static const int16_t q[65] = {
            0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
         6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
        12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
        18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
        23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
        27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
        30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
        32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
        32767
    };
    uint32_t p = phase & 255u;
    if (p <= 64u)  return q[p];
    if (p < 128u)  return q[128u - p];
    if (p <= 192u) return (int16_t)(-q[p - 128u]);
    return (int16_t)(-q[256u - p]);
}

static void wasm_seed_demo_files(void)
{
    blkdev_t bd;
    bd.read_sectors  = ram_sd_read;
    bd.write_sectors = ram_sd_write;
    bd.total_sectors = WASM_SD_SECTORS;
    bd.ctx           = (void *)0;

    exfat_vol_t vol;
    if (exfat_mount(&vol, &bd) != 0) {
        return;
    }

    rtc_datetime_t now;
    rtc_get(&now);
    uint32_t ts_now = rtc_to_dos_timestamp(&now);

    rtc_datetime_t earlier = now;
    if (earlier.minute >= 15u) {
        earlier.minute -= 15u;
    } else if (earlier.hour > 0u) {
        earlier.hour -= 1u;
        earlier.minute = (uint8_t)(earlier.minute + 45u);
    }
    uint32_t ts_earlier = rtc_to_dos_timestamp(&earlier);

    exfat_set_timestamp(&vol, ts_earlier);
    (void)exfat_mkdir(&vol, "/record");

    /* 1. Demo Recording 1 (earlier): 5-second stereo chime WAV (no transcript) */
    {
        const uint32_t rate    = 16000u;
        const uint32_t secs    = 5u;
        const uint32_t samples = rate * secs;
        const uint32_t data_b  = samples * 4u; /* 16-bit stereo */
        exfat_writer_t f;
        if (exfat_create_file(&vol, "/record", "REC_0001.wav", &f) == 0) {
            uint8_t hdr[44];
            wav_build_header(hdr, rate, 2u, 16u, data_b);
            (void)exfat_write(&f, hdr, 44u);

            int16_t chunk[512]; /* 256 stereo frames */
            uint32_t phL = 0u, phR = 0u;
            uint32_t done = 0u;
            while (done < samples) {
                uint32_t n = (samples - done > 256u) ? 256u : (samples - done);
                for (uint32_t i = 0u; i < n; i++) {
                    uint32_t idx = done + i;
                    uint32_t fL  = (idx < rate * 2u) ? 440u : 554u;
                    uint32_t fR  = (idx < rate * 2u) ? 659u : 880u;
                    phL += (fL * 65536u) / rate;
                    phR += (fR * 65536u) / rate;
                    chunk[i * 2u + 0u] = (int16_t)(fast_sin_q15(phL >> 8) >> 2);
                    chunk[i * 2u + 1u] = (int16_t)(fast_sin_q15(phR >> 8) >> 2);
                }
                (void)exfat_write(&f, chunk, n * 4u);
                done += n;
            }
            (void)exfat_close_file(&f);
        }
    }

    /* 2. Demo Recording 2 (newest, appears first in Files): 12s WAV + synced CWX1 transcript */
    exfat_set_timestamp(&vol, ts_now);
    {
        const uint32_t rate    = 16000u;
        const uint32_t secs    = 12u;
        const uint32_t samples = rate * secs;
        const uint32_t data_b  = samples * 2u; /* 16-bit mono */
        exfat_writer_t f;
        if (exfat_create_file(&vol, "/record", "REC_0002.wav", &f) == 0) {
            uint8_t hdr[44];
            wav_build_header(hdr, rate, 1u, 16u, data_b);
            (void)exfat_write(&f, hdr, 44u);

            static const uint16_t notes[12] = {
                262, 330, 392, 523, 440, 392, 349, 330, 294, 330, 392, 523
            };
            int16_t chunk[512];
            uint32_t ph1 = 0u, ph2 = 0u;
            uint32_t done = 0u;
            while (done < samples) {
                uint32_t n = (samples - done > 512u) ? 512u : (samples - done);
                for (uint32_t i = 0u; i < n; i++) {
                    uint32_t idx  = done + i;
                    uint32_t sec  = idx / rate;
                    uint32_t frac = idx % rate;
                    uint32_t freq = notes[sec % 12u];
                    ph1 += (freq * 65536u) / rate;
                    ph2 += (freq * 2u * 65536u) / rate;
                    int32_t env = 1024;
                    if (frac < 1200u) env = (int32_t)((frac * 1024u) / 1200u);
                    else if (frac > 13000u) env = (int32_t)(((16000u - frac) * 1024u) / 3000u);
                    int32_t s1 = fast_sin_q15(ph1 >> 8);
                    int32_t s2 = fast_sin_q15(ph2 >> 8) >> 2;
                    int32_t v  = (((s1 + s2) * env) >> 12);
                    chunk[i]   = (int16_t)v;
                }
                (void)exfat_write(&f, chunk, n * 2u);
                done += n;
            }
            (void)exfat_close_file(&f);
        }

        typedef struct { uint32_t s; uint32_t e; uint8_t spk; uint8_t flg; const char *w; } dw_t;
        static const dw_t words[] = {
            {   200,   650, 0, CWX_FLAG_TURN_START, "Welcome" },
            {   680,   920, 0, 0,                   "to" },
            {   950,  1450, 0, 0,                   "Cassini" },
            {  1480,  2100, 0, 0,                   "OpenFW," },
            {  2150,  2650, 0, 0,                   "running" },
            {  2680,  3100, 0, 0,                   "live" },
            {  3140,  3400, 0, 0,                   "in" },
            {  3440,  3750, 0, 0,                   "your" },
            {  3780,  4450, 0, 0,                   "browser." },
            {  4700,  5150, 1, CWX_FLAG_TURN_START, "Slide" },
            {  5180,  5450, 1, 0,                   "the" },
            {  5480,  5950, 1, 0,                   "record" },
            {  5980,  6450, 1, 0,                   "switch" },
            {  6480,  6750, 1, 0,                   "to" },
            {  6780,  7350, 1, 0,                   "capture" },
            {  7380,  7850, 1, 0,                   "stereo" },
            {  7880,  8450, 1, 0,                   "audio," },
            {  8650,  9050, 0, CWX_FLAG_TURN_START, "or" },
            {  9080,  9500, 0, 0,                   "hold" },
            {  9540,  9950, 0, 0,                   "Next" },
            {  9980, 10250, 0, 0,                   "to" },
            { 10280, 10850, 0, 0,                   "scrub" },
            { 10880, 11750, 0, 0,                   "audibly!" },
        };
        const uint32_t nw = (uint32_t)(sizeof(words) / sizeof(words[0]));
        uint8_t cwx[2048];
        memset(cwx, 0, sizeof(cwx));
        uint32_t spk_off   = CWX_HEADER_SIZE;
        uint32_t seek_off  = spk_off + 2u * CWX_SPEAKER_SIZE;
        uint32_t seek_cnt  = secs;
        uint32_t words_off = seek_off + seek_cnt * CWX_SEEK_SIZE;

        memcpy(&cwx[0], "CWX1", 4u);
        put_le16(&cwx[4], 1u);
        put_le16(&cwx[6], 2u);
        put_le32(&cwx[8], secs * 1000u);
        put_le32(&cwx[12], nw);
        put_le32(&cwx[16], spk_off);
        put_le32(&cwx[20], seek_off);
        put_le32(&cwx[24], seek_cnt);
        put_le32(&cwx[28], words_off);
        memcpy(&cwx[32], "Browser Demo", 12u);

        put_le16(&cwx[spk_off + 0u], 0x465Fu);
        memcpy(&cwx[spk_off + 4u], "Host", 4u);
        put_le16(&cwx[spk_off + CWX_SPEAKER_SIZE + 0u], 0xFD84u);
        memcpy(&cwx[spk_off + CWX_SPEAKER_SIZE + 4u], "Guest", 5u);

        uint32_t wpos = words_off;
        uint32_t word_rel[32];
        for (uint32_t i = 0u; i < nw; i++) {
            word_rel[i] = wpos - words_off;
            uint32_t tlen = (uint32_t)rom_strlen(words[i].w);
            put_le32(&cwx[wpos + 0u], words[i].s);
            put_le32(&cwx[wpos + 4u], words[i].e);
            cwx[wpos + 8u]  = words[i].spk;
            cwx[wpos + 9u]  = words[i].flg;
            cwx[wpos + 10u] = (uint8_t)tlen;
            memcpy(&cwx[wpos + 11u], words[i].w, tlen);
            wpos += 11u + tlen;
        }
        for (uint32_t s = 0u; s < seek_cnt; s++) {
            uint32_t wi = 0u;
            while (wi + 1u < nw && words[wi].e <= s * 1000u) {
                wi++;
            }
            put_le32(&cwx[seek_off + s * CWX_SEEK_SIZE + 0u], word_rel[wi]);
            put_le32(&cwx[seek_off + s * CWX_SEEK_SIZE + 4u], wi);
        }

        if (exfat_create_file(&vol, "/record", "REC_0002.cwx", &f) == 0) {
            (void)exfat_write(&f, cwx, wpos);
            (void)exfat_close_file(&f);
        }
    }
}

/* ============================================================================
 * Exports for web_emu/worker.js
 * ============================================================================ */
__attribute__((export_name("wasm_fb_ptr")))
uint16_t *wasm_fb_ptr(void)
{
    return &g_soc.fb[0][0];
}

__attribute__((export_name("wasm_fb_take_dirty")))
uint32_t wasm_fb_take_dirty(void)
{
    uint32_t d = g_soc.fb_dirty;
    g_soc.fb_dirty = 0u;
    return d;
}

__attribute__((export_name("wasm_get_backlight")))
uint32_t wasm_get_backlight(void)
{
    if (g_soc.powered_off) return 0u;
    uint32_t pb17_mux = (g_soc.gpio_mux[1][2] >> 4) & 0xFu;
    if (pb17_mux != 5u || (g_soc.pwm3_ctrl & 0x3Fu) == 0u) {
        return 0u;
    }
    uint32_t duty = g_soc.pwm3_period_duty & 0xFFFFu;
    uint32_t pct  = (duty + 240u) / 480u;
    return (pct > 100u) ? 100u : pct;
}

__attribute__((export_name("wasm_get_speaker_pa")))
uint32_t wasm_get_speaker_pa(void)
{
    return g_soc.speaker_pa;
}

__attribute__((export_name("wasm_xfer_ptr")))
uint8_t *wasm_xfer_ptr(void)
{
    return g_xfer_buf;
}

__attribute__((export_name("wasm_set_key_mask")))
void wasm_set_key_mask(uint32_t mask)
{
    /* Bit mapping: 0=LEFT, 1=RIGHT, 2=VOL+, 3=VOL-, 4=BACK, 5=ENTER */
    if      (mask & (1u << 0)) g_soc.saradc_ch1_data = 150u;
    else if (mask & (1u << 1)) g_soc.saradc_ch1_data = 590u;
    else if (mask & (1u << 2)) g_soc.saradc_ch1_data = 1135u;
    else if (mask & (1u << 3)) g_soc.saradc_ch1_data = 2000u;
    else if (mask & (1u << 4)) g_soc.saradc_ch1_data = 2368u;
    else if (mask & (1u << 5)) g_soc.saradc_ch1_data = 2760u;
    else                       g_soc.saradc_ch1_data = 4095u;
}

__attribute__((export_name("wasm_set_rec_switch")))
void wasm_set_rec_switch(uint32_t on)
{
    /* PB6 is active-low: 0 = Record ON, 1 = Stop */
    if (on) g_soc.gpio_in[1] &= ~(1u << 6);
    else    g_soc.gpio_in[1] |= (1u << 6);
}

__attribute__((export_name("wasm_set_hp_plugged")))
void wasm_set_hp_plugged(uint32_t plugged)
{
    /* PB11 is active-low: 0 = headphones inserted */
    if (plugged) g_soc.gpio_in[1] &= ~(1u << 11);
    else         g_soc.gpio_in[1] |= (1u << 11);
}

__attribute__((export_name("wasm_set_vbus")))
void wasm_set_vbus(uint32_t present)
{
    g_soc.vbus_present = present ? 1u : 0u;
    if (!present) {
        g_soc.pmu_regs[0x2Au] = 0x00u;
    } else if (g_soc.vbat_mv >= 4150u) {
        g_soc.pmu_regs[0x2Au] = 0x40u; /* Full */
    } else {
        g_soc.pmu_regs[0x2Au] = 0x04u; /* Charging */
    }
}

__attribute__((export_name("wasm_set_vbat_mv")))
void wasm_set_vbat_mv(uint32_t mv)
{
    if (mv < 3000u) mv = 3000u;
    if (mv > 4250u) mv = 4250u;
    g_soc.vbat_mv = (uint16_t)mv;
    uint32_t raw = (mv * 1000u - 2800000u) / 14173u;
    if (raw > 127u) raw = 127u;
    g_soc.pmu_regs[0x2Eu] = (uint8_t)raw;
    wasm_set_vbus(g_soc.vbus_present);
}

__attribute__((export_name("wasm_power_button")))
void wasm_power_button(uint32_t down)
{
    if (down) g_soc.pmu_regs[0x31u] |= 0x40u; /* KEY_ONOFF_PRESS */
    else      g_soc.pmu_regs[0x31u] |= 0x10u; /* KEY_ONOFF_RELEASE */
}

typedef struct {
    uint8_t *buf;
    uint32_t pos;
    uint32_t cap;
} list_ctx_t;

static int list_rec_cb(const exfat_entry_t *ent, void *user)
{
    list_ctx_t *c = (list_ctx_t *)user;
    if (ent->is_dir) return 0;
    uint32_t nlen = (uint32_t)rom_strlen(ent->name);
    char szbuf[16];
    uint32_t v = (uint32_t)ent->size;
    int sp = 0;
    if (v == 0u) {
        szbuf[sp++] = '0';
    } else {
        char tmp[16];
        int tp = 0;
        while (v > 0u && tp < 15) { tmp[tp++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (tp > 0) szbuf[sp++] = tmp[--tp];
    }
    if (c->pos + nlen + (uint32_t)sp + 2u >= c->cap) return 1;
    memcpy(&c->buf[c->pos], ent->name, nlen);
    c->pos += nlen;
    c->buf[c->pos++] = '\t';
    memcpy(&c->buf[c->pos], szbuf, (size_t)sp);
    c->pos += (uint32_t)sp;
    c->buf[c->pos++] = '\n';
    return 0;
}

__attribute__((export_name("wasm_list_records")))
uint32_t wasm_list_records(void)
{
    blkdev_t bd = { ram_sd_read, ram_sd_write, WASM_SD_SECTORS, (void *)0 };
    exfat_vol_t vol;
    exfat_entry_t dir;
    if (exfat_mount(&vol, &bd) != 0) return 0u;
    if (exfat_find_path(&vol, "/record", &dir) != 0 || !dir.is_dir) return 0u;
    list_ctx_t ctx = { g_xfer_buf, 0u, (uint32_t)sizeof(g_xfer_buf) - 1u };
    (void)exfat_list_dir_entry(&vol, &dir, list_rec_cb, &ctx);
    g_xfer_buf[ctx.pos] = '\0';
    return ctx.pos;
}

__attribute__((export_name("wasm_read_record_chunk")))
uint32_t wasm_read_record_chunk(uint32_t name_len, uint32_t offset, uint32_t max_len)
{
    if (name_len == 0u || name_len > 64u) return 0u;
    char path[96];
    memcpy(path, "/record/", 8u);
    memcpy(path + 8u, g_xfer_buf, name_len);
    path[8u + name_len] = '\0';

    blkdev_t bd = { ram_sd_read, ram_sd_write, WASM_SD_SECTORS, (void *)0 };
    exfat_vol_t vol;
    if (exfat_mount(&vol, &bd) != 0) return 0u;
    exfat_entry_t ent;
    if (exfat_find_path(&vol, path, &ent) != 0 || ent.is_dir) return 0u;
    if (max_len > sizeof(g_xfer_buf)) max_len = (uint32_t)sizeof(g_xfer_buf);
    uint32_t got = 0u;
    if (exfat_read_file(&vol, &ent, (uint64_t)offset, g_xfer_buf, max_len, &got) != 0) return 0u;
    return got;
}

__attribute__((export_name("wasm_sd_ptr")))
uint8_t *wasm_sd_ptr(void)
{
    return g_sd_img;
}

__attribute__((export_name("wasm_sd_size")))
uint32_t wasm_sd_size(void)
{
    return WASM_SD_BYTES;
}

__attribute__((export_name("wasm_boot")))
void wasm_boot(uint32_t keep_sd)
{
    memset(&g_soc, 0, sizeof(g_soc));
    g_soc.aon_pwr_ctl0    = (1u << 28);
    g_soc.aon_rtc_ctrl    = (1u << 4) | (1u << 8);
    g_soc.saradc_ch1_data = 4095u;
    g_soc.gpio_in[1]      = (1u << 6) | (1u << 11); /* REC switch off, HP unplugged */
    g_soc.st_x1           = 159u;
    g_soc.st_y1           = 79u;
    g_soc.pmu_regs[0x00u] = 0x88u;
    g_soc.pmu_regs[0x24u] = 0x04u;
    wasm_set_vbat_mv(4020u);
    wasm_set_vbus(0u);

    /* Initialize RTC from browser wall clock */
    uint32_t ymd_hms[6] = { 2026u, 11u, 23u, 12u, 0u, 0u };
    host_get_wall_clock(ymd_hms);
    rtc_datetime_t dt;
    dt.year    = (uint16_t)ymd_hms[0];
    dt.month   = (uint8_t)ymd_hms[1];
    dt.day     = (uint8_t)ymd_hms[2];
    dt.hour    = (uint8_t)ymd_hms[3];
    dt.minute  = (uint8_t)ymd_hms[4];
    dt.second  = (uint8_t)ymd_hms[5];
    dt.weekday = 0u;
    rtc_set(&dt);

    if (!keep_sd || !g_sd_formatted) {
        wasm_format_exfat();
        wasm_seed_demo_files();
        g_sd_formatted = 1;
    }

    (void)openfw_main();
}
