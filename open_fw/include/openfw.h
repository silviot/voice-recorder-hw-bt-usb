/*
 * Cassini OpenFW Internal Driver Interfaces (Layer 1b)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_H
#define OPENFW_H

#include <stdint.h>
#include "openfw_fs.h"

/* Watchdog, Timers & System Reset (wdt_timer.c) */
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
uint32_t rtc_to_dos_timestamp(const rtc_datetime_t *dt);
void     rtc_set_alarm0_seconds(uint32_t seconds);

/* UART0 Debug Console (uart.c) — NOTE: PD0 is shared with SARADC kadc_ch1 */
void     uart0_init_115200(void);
void     uart0_putc(char c);
void     uart0_puts(const char *s);
void     uart0_put_u32(uint32_t v);
void     uart0_put_hex(uint32_t v);

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
void            pmu_reboot_to_usb_bootloader(void) __attribute__((noreturn));
void            pmu_reboot_to_sd_ota(void) __attribute__((noreturn));

/* SARADC Keypad & Record Switch (keys.c) */
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

void        keys_init(void);
uint16_t    keys_read_adc(void);
int         keys_read_rec_switch(void);
key_id_t    keys_decode(uint16_t adc);
const char *keys_name(key_id_t key);

/* ST7735S 80x160 8080 Parallel LCD + PWM3 Backlight (lcd_st7735s.c) */
#define LCD_WIDTH   80u
#define LCD_HEIGHT  160u

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

void lcd_backlight_set(uint8_t percent);
void lcd_init(void);
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_blit_rgb565(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
void lcd_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
void lcd_draw_u32(uint16_t x, uint16_t y, uint32_t v, uint16_t fg, uint16_t bg);
void lcd_draw_hex(uint16_t x, uint16_t y, uint32_t v, uint8_t digits, uint16_t fg, uint16_t bg);

/* 1-Bit SDIO / microSD Block Driver (sdio.c) */
typedef struct {
    int      initialized;
    int      is_sdhc;
    uint16_t rca;
    uint32_t sector_count;
} sd_card_t;

int      sd_init(sd_card_t *card);
int      sd_read_blocks(sd_card_t *card, uint32_t lba, uint32_t count, void *buf);
int      sd_write_blocks(sd_card_t *card, uint32_t lba, uint32_t count, const void *buf);
blkdev_t sd_as_blkdev(sd_card_t *card);

/* Audio ADC AMIC1 + DMAC CH1 4-Frame Ring Capture (audio_dma.c) */
int            audio_capture_start(uint8_t gain_0_31);
void           audio_capture_stop(void);
uint8_t        audio_get_peak_pct(void);
uint32_t       audio_get_frames(void);
const int16_t *audio_pop_frame(uint32_t *out_bytes);
uint32_t       audio_get_overruns(void);

/* Real-Time WAV Recorder (recorder.c) */
int         recorder_start(exfat_vol_t *vol, int sd_mounted);
void        recorder_poll(void);
int         recorder_stop(void);
int         recorder_is_saving(void);
uint32_t    recorder_get_seconds(void);
const char *recorder_get_filename(void);

/* USB Mass Storage Full-Speed MUSB & PHY Plug Detect (usb_musb.c) */
void usb_phy_det_arm(void);
int  usb_phy_det_poll_host(void);
uint16_t usb_musb_frame(void);
int  usb_msc_start(blkdev_t *dev);
void usb_msc_stop(void);
void usb_msc_poll(void);
int  usb_msc_is_active(void);
void usb_msc_get_stats(uint32_t *rx_pkts, uint32_t *tx_pkts);

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
    uint8_t  bd_addr[6];
    char     dev_name[16];
} bt_status_t;

int  bt_xrec_start(exfat_vol_t *vol, int sd_mounted);
void bt_xrec_poll(void);
void bt_xrec_stop(void);
void bt_xrec_open_pairing(uint32_t duration_ms);
void bt_xrec_get_status(bt_status_t *out);

#endif /* OPENFW_H */
