/*
 * Cassini OpenFW Internal Driver Interfaces (Layer 1b)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_H
#define OPENFW_H

#include <stdint.h>
#include "audio_dsp.h"
#include "openfw_fs.h"

#ifndef OPENFW_UNATTENDED_DEV
#define OPENFW_UNATTENDED_DEV 0
#endif

/*
 * Privileged USB 0xE0 SRAM experiment & arbitrary memory read handlers
 * (EXP_WRITE 0x05, EXP_RUN 0x06, MEM_READ 0x07, EXP_STATUS 0x09).
 * Disabled by default in ordinary recorder builds; opt in explicitly via
 * `make -C open_fw DEV_EXP=1`.
 */
#ifndef OPENFW_ENABLE_USB_EXP
#define OPENFW_ENABLE_USB_EXP 0
#endif

#ifdef OPENFW_HOST_MMIO
#define OPENFW_RAMFUNC
#define OPENFW_FASTFUNC
#define OPENFW_SRAM_HI
#else
#define OPENFW_RAMFUNC  __attribute__((section(".ramtext")))
#define OPENFW_FASTFUNC __attribute__((section(".fast_ramtext")))
#define OPENFW_SRAM_HI  __attribute__((section(".bss_hi")))
#endif

/* SPI NOR XIP mode detected at boot (startup.c) */
#define OPENFW_XIP_MODE_1WIRE_24M 0u
#define OPENFW_XIP_MODE_DUAL_48M  1u
#define OPENFW_XIP_MODE_QUAD_48M  2u
extern volatile uint8_t g_openfw_xip_mode;

/* Dual-core CPU1 dedicated LVGL/LCD UI engine & latency telemetry (cpu1_ui.c) */
extern volatile uint32_t g_cpu1_alive;
extern volatile uint32_t g_cpu1_faulted;
extern volatile uint32_t g_cpu1_fault_pc;
extern volatile uint32_t g_cpu1_fault_cfsr;
extern volatile uint32_t g_cpu1_wfe_ok;
extern volatile uint32_t g_cpu1_wfe_wakes;
extern volatile uint32_t g_ui_last_render_us;
extern volatile uint32_t g_ui_max_render_us;
extern volatile uint32_t g_ui_frames;
extern volatile uint32_t g_ui_last_load_us;
extern volatile uint32_t g_ui_last_draw_us;
extern volatile uint32_t g_ui_last_blit_us;
extern volatile uint32_t g_ui_heap_used;

int  cpu1_ui_start(void);
void cpu1_ui_stop(void);
int  cpu1_ui_is_active(void);
int  cpu1_ui_wfe_active(void);
void cpu1_ui_sync(uint32_t timeout_ms);

#define OPENFW_EXP_TIMEOUT_MS    15000u
#define OPENFW_FAULT_MAGIC_MASK  0xFFFFF000u
#define OPENFW_FAULT_MAGIC_BASE  0xFA017000u
#define OPENFW_FAULT_EXP_TIMEOUT 0xFA017E00u
#define OPENFW_BOOT_STRIKE_MAGIC 0xB0070000u
#define OPENFW_BOOT_MAX_STRIKES  3u

/* Watchdog, Timers & System Reset (wdt_timer.c) */
extern volatile uint32_t g_exp_running;
extern volatile uint32_t g_exp_deadline_ms;
extern uint32_t          g_prev_fault_code;

void     wdt_start_reset(uint32_t timeout_sec, uint8_t pmu_reg20_cur);
void     wdt_stop(void);
void     wdt_feed(void);
void     system_reboot(void);
uint64_t us_timer_now(void);
void     udelay(uint32_t us);
void     mdelay(uint32_t ms);
void     systick_init(uint32_t cpu_hz);
uint32_t millis(void);

/* AON Hardware RTC (rtc.c) */
typedef struct {
    uint16_t year;    /* Full year e.g. 2026 (2000..2127) */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint8_t  weekday; /* 0 = Mon .. 6 = Sun */
    uint8_t  hour;    /* 0..23 */
    uint8_t  minute;  /* 0..59 */
    uint8_t  second;  /* 0..59 */
} rtc_datetime_t;

void     rtc_read_raw(uint32_t *date_reg, uint32_t *time_reg);
void     rtc_ensure_running(void);
void     rtc_get(rtc_datetime_t *dt);
void     rtc_set(const rtc_datetime_t *dt);
rtc_datetime_t rtc_from_dos_timestamp(uint32_t stamp);
uint32_t rtc_to_dos_timestamp(const rtc_datetime_t *dt);
void     rtc_set_alarm0_seconds(uint32_t seconds);

/* UART0 Debug Console (uart.c) — NOTE: PD0 is shared with SARADC kadc_ch1 */
void     uart0_init_115200(void);
void     uart0_putc(char c);
void     uart0_puts(const char *s);
void     uart0_put_u32(uint32_t v);
void     uart0_put_hex(uint32_t v);

/* Zero-wire SRAM debug log ring (mirrored by uart0_putc in uart.c) */
#define OPENFW_LOG_MAGIC 0x474C464Fu /* 'OFLG' little-endian */
#define OPENFW_LOG_SIZE  4096u

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t write_idx;
    uint32_t dropped_bytes;
    char     buf[OPENFW_LOG_SIZE];
} openfw_log_t;

extern openfw_log_t g_openfw_log;
void     openfw_log_reset(void);
void     openfw_log_putc(char c);
void     openfw_log_puts(const char *s);
void     openfw_log_u32(uint32_t v);
void     openfw_log_hex(uint32_t v);

/* PMU RTWI (pmu.c) */
typedef enum {
    PMU_CHG_DISCHARGING = 0,
    PMU_CHG_CHARGING    = 1,
    PMU_CHG_FULL        = 2,
} pmu_chg_state_t;

int             pmu_write_reg(uint8_t reg, uint8_t val);
uint8_t         pmu_read_reg(uint8_t reg);
uint32_t        pmu_read_vbat_mv(void);
pmu_chg_state_t pmu_get_charge_state(void);
int             pmu_is_vbus_present(void);
#define KEY_ONOFF_PRESS   0x01u
#define KEY_ONOFF_LONG    0x02u
#define KEY_ONOFF_RELEASE 0x04u

void            key_onoff_init(void);
int             key_onoff_pressed(int *out_long_press);
uint8_t         key_onoff_poll(void);
typedef enum {
    POWER_BUTTON_IDLE, POWER_BUTTON_WAKE, POWER_BUTTON_SLEEP,
    POWER_BUTTON_PROGRESS, POWER_BUTTON_CANCEL, POWER_BUTTON_SHUTDOWN
} power_button_action_t;

power_button_action_t power_button_update(uint8_t events, uint32_t now, int screen_off,
                                          uint8_t *progress);
void            pmu_soft_poweroff(void) __attribute__((noreturn));
void            pmu_unattended_init(void);
void            pmu_poll_charge_manage(void);
void            pmu_reboot_to_usb_bootloader(void) __attribute__((noreturn));
void            pmu_reboot_to_sd_ota(void) __attribute__((noreturn));

/* SARADC Keypad, Record Switch (PB6) & Earphone Jack Detect (PB11) (keys.c) */
typedef enum {
    KEY_NONE = 0,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_VOL_UP,
    KEY_VOL_DOWN,
    KEY_BACK,
    KEY_ENTER,
    KEY_UNKNOWN,
} key_id_t;

typedef struct {
    uint32_t t_ms;
    key_id_t key;
    uint8_t  pressed;
} key_event_t;

void        keys_init(void);
uint16_t    keys_read_adc(void);
int         keys_read_rec_switch(void);
int         keys_read_hp_detect(void);
key_id_t    keys_decode(uint16_t adc);
const char *keys_name(key_id_t key);
void        keys_scan_tick(uint32_t now_ms);
int         keys_peek_event(key_event_t *out);
int         keys_pop_event(key_event_t *out);
int         keys_event_pending(void);
key_id_t    keys_flush_events(void); /* held key at the queue boundary */
void        keys_ignore_held(void); /* start a screen transition, suppress the exiting press */
key_id_t    keys_state(uint32_t *since_ms);

/* ST7735S 80x160 / 160x80 8080 Parallel LCD + PWM3 Backlight (lcd_st7735s.c) */
#define LCD_WIDTH   80u
#define LCD_HEIGHT  160u

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

void     lcd_backlight_set(uint8_t percent);
void     lcd_init(void);
uint64_t lcd_init_begin(void);
void     lcd_init_finish(uint64_t t_slpout_us);
void     lcd_shutdown(void);
void     lcd_set_rotation(int landscape);
uint16_t lcd_get_width(void);
uint16_t lcd_get_height(void);
void     lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void     lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void     lcd_blit_rgb565_stride(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                const uint16_t *pixels, uint16_t stride);
void     lcd_blit_rgb565(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
void     lcd_blit_rgb565_alpha(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               const uint16_t *pixels, uint8_t alpha);
void     lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
void     lcd_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void     lcd_draw_char_2x(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
void     lcd_draw_str_2x(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void     lcd_draw_u32(uint16_t x, uint16_t y, uint32_t v, uint16_t fg, uint16_t bg);
void     lcd_draw_hex(uint16_t x, uint16_t y, uint32_t v, uint8_t digits, uint16_t fg, uint16_t bg);
/* Landscape hardware scroll ring: 160 visible columns + 2 hidden GRAM lines. */
#define LCD_HSCROLL_LINES 162u
void     lcd_hscroll_set(uint16_t offset);

/* 1-Bit SDIO / microSD Block Driver (sdio.c) */
typedef struct {
    int      initialized;
    int      is_sdhc;
    uint16_t rca;
    uint32_t sector_count;
    uint8_t  clk_div;
    uint8_t  async_active;
    uint8_t  async_last_desc;
    uint8_t  no_multi_write;
    uint32_t async_t0_us;
} sd_card_t;

int      sd_init(sd_card_t *card);
int      sd_flush(sd_card_t *card);
int      sd_read_blocks(sd_card_t *card, uint32_t lba, uint32_t count, void *buf);
int      sd_write_blocks(sd_card_t *card, uint32_t lba, uint32_t count, const void *buf);
int      sd_read_async_start(sd_card_t *card, uint32_t lba, uint32_t count, void *buf);
int      sd_read_async_poll(sd_card_t *card);
int      sd_read_async_wait(sd_card_t *card);
void     sd_read_async_abort(sd_card_t *card);
blkdev_t sd_as_blkdev(sd_card_t *card);

/* Audio ADC AMIC1/AMIC2 + DMAC CH1 4-Frame Ring Capture (audio_dma.c) */
int            audio_capture_start(uint8_t gain_0_31);
int            audio_capture_start_ex(uint8_t gain_0_31, uint8_t channels);
void           audio_capture_set_gain(uint8_t gain_0_31);
void           audio_capture_stop(void);
uint8_t        audio_get_channels(void);
uint8_t        audio_get_peak_pct(void);
void           audio_get_peak_lr_pct(uint8_t *out_l, uint8_t *out_r);
uint32_t       audio_dma_peek_pcm(int16_t *dst, uint32_t max_frames, uint8_t *out_channels);
uint32_t       audio_get_frames(void);
const int16_t *audio_pop_frame(uint32_t *out_bytes);
int            audio_pop_frame_copy(int16_t *dst, uint32_t max_bytes, uint32_t *out_bytes);
uint32_t       audio_get_overruns(void);

/* Audio DAC0+DAC1 Stereo Headphone + Built-in Speaker PA (PB30) + DMAC CH0 TX (audio_dac.c) */
void     audio_pll_set(uint32_t sample_rate);
void     audio_pa_set(int enable);
int      audio_dac_start(uint32_t sample_rate, uint8_t ana_vol_0_31);
void     audio_dac_set_volume(uint8_t ana_vol_0_31, uint16_t dig_vol_0_511);
void     audio_dac_set_mute(int muted);
#define AUDIO_DAC_QUEUE_SLOTS 4u
int      audio_dac_write_stereo(const int16_t *interleaved_lr, uint32_t num_frames);
int      audio_dac_is_busy(void);
void     audio_dac_flush(void);
uint32_t audio_dac_queued_frames(void);
void     audio_dac_stop(void);

/* Real-Time WAV Recorder + Clean-Room DSP (recorder.c) */
void                recorder_set_gain(uint8_t gain_0_31);
uint8_t             recorder_get_gain(void);
void                recorder_set_dsp_profile(audio_dsp_profile_t profile);
audio_dsp_profile_t recorder_get_dsp_profile(void);
void                recorder_set_channels(uint8_t channels);
uint8_t             recorder_get_channels(void);
void                recorder_set_vor(int enabled);
int                 recorder_get_vor(void);
uint32_t            recorder_get_clip_events(void);
int16_t             recorder_get_rms_dbfs_x10(void);
int                 recorder_start(exfat_vol_t *vol, int sd_mounted);
void                recorder_poll(void);
int                 recorder_stop(void);
int                 recorder_is_saving(void);
int                 recorder_has_error(void);
int                 recorder_is_vor_waiting(void);
uint32_t            recorder_get_seconds(void);
uint32_t            recorder_get_committed_bytes(void);
uint32_t            recorder_get_created_timestamp(void);
const char         *recorder_get_filename(void);

/* USB Mass Storage Full-Speed MUSB & PHY Plug Detect (usb_musb.c) */
typedef int (*usb_audio_cfg_cb_t)(int set, uint8_t cfg[4], int16_t *rms_x10, uint16_t *clips);

void usb_phy_det_arm(void);
int  usb_phy_det_poll_host(void);
uint16_t usb_musb_frame(void);
void usb_msc_set_diag_info(uint8_t sd_mounted, uint8_t rec_active, uint8_t mic_gain);
void usb_msc_set_audio_cfg_cb(usb_audio_cfg_cb_t cb);
int  usb_msc_start(blkdev_t *dev);
void usb_msc_enable_sd_async(sd_card_t *card);
void usb_msc_stop(void);
void usb_msc_poll(void);
int  usb_msc_is_active(void);
void usb_msc_get_stats(uint32_t *rx_pkts, uint32_t *tx_pkts);
void usb_msc_get_bytes(uint64_t *rx_bytes, uint64_t *tx_bytes);

/* BT RF & Modem Hardware Bring-Up (bt_rf.c) */
uint32_t bt_rf_efuse_trim(void);
void     bt_rf_init(uint32_t freq_drift, uint32_t vco_cal_10a4);
void     bt_modem_init(uint32_t clk6_hz);
uint32_t bt_rf_init_calibrated(uint32_t freq_drift, uint32_t clk6_hz);

/* Classic BR/EDR Bluetooth Controller + SPP + +XREC v5 Service (bt_hci.c) */
typedef struct {
    uint8_t  active;
    uint8_t  hci_ready;
    uint8_t  acl_connected;
    uint8_t  spp_open;
    uint8_t  xrec_bound;
    uint8_t  xrec_auth;
    uint8_t  pair_active;
    char     pair_pin[8];
    uint32_t rx_acl_pkts;
    uint32_t tx_acl_pkts;
    uint32_t irq_count;
    uint8_t  bd_addr[6];
    char     dev_name[16];
} bt_status_t;

int  bt_xrec_start(exfat_vol_t *vol, int sd_mounted);
void bt_xrec_poll(void);
void bt_xrec_stop(void);
void bt_xrec_open_pairing(uint32_t duration_ms);
void bt_xrec_get_status(bt_status_t *out);

#endif /* OPENFW_H */
