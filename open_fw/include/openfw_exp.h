/*
 * Cassini OpenFW Open Experiment Upload & Execution ABI
 *
 * Allows dynamic compilation and upload of small Thumb-2 hardware experiment
 * payloads into a fixed 32 KiB SRAM arena (0x00850000..0x00857FFF) over USB
 * vendor SCSI (0xE0) or Bluetooth SPP (+XREC), with structured telemetry,
 * PD0-safe SRAM ring logging, optional 16 KiB binary output capture
 * (0x00858000..0x0085BFFF), and exFAT SD file output.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_EXP_H
#define OPENFW_EXP_H

#include <stdint.h>
#include "openfw.h"

#define OPENFW_EXP_ABI_MAGIC   0x45585031u /* "EXP1" */
#define OPENFW_EXP_RES_MAGIC   0x31525845u /* "EXR1" LE */
#define OPENFW_EXP_ARENA_ADDR  0x00850000u
#define OPENFW_EXP_ARENA_SIZE  32768u
#define OPENFW_EXP_OUT_ADDR    0x00858000u
#define OPENFW_EXP_OUT_SIZE    16384u

typedef struct {
    uint32_t magic;              /* OPENFW_EXP_RES_MAGIC */
    uint32_t rc;                 /* Return value from exp_main(api, arg) */
    uint32_t elapsed_ms;         /* Execution duration in ms */
    uint32_t out_len;            /* Bytes written into out_buf (0x00858000) */
    uint32_t log_widx;           /* g_openfw_log.write_idx after run */
    uint32_t run_count;          /* Monotonic experiment execution counter */
    uint32_t arena_crc_and_size; /* [31:16] CRC-16, [15:0] loaded bytes */
    uint32_t arena_base;         /* OPENFW_EXP_ARENA_ADDR on target */
} openfw_exp_result_t;

typedef struct {
    uint32_t  magic;
    uint8_t  *arena_base;
    uint32_t  arena_size;
    uint8_t  *out_buf;
    uint32_t  out_size;
    uint32_t *out_len_ptr;

    /* Watchdog & Timing */
    void     (*wdt_feed)(void);
    uint32_t (*millis)(void);
    uint64_t (*us_timer_now)(void);
    void     (*udelay)(uint32_t us);
    void     (*mdelay)(uint32_t ms);

    /* PD0-safe SRAM Ring Log (safe after keys_init() muxes PD0 to SARADC) */
    void     (*log_reset)(void);
    void     (*log_putc)(char c);
    void     (*log_puts)(const char *s);
    void     (*log_u32)(uint32_t v);
    void     (*log_hex)(uint32_t v);

    /* PMU & Power Button */
    uint8_t  (*pmu_read_reg)(uint8_t reg);
    int      (*pmu_write_reg)(uint8_t reg, uint8_t val);
    uint32_t (*pmu_read_vbat_mv)(void);
    int      (*pmu_is_vbus_present)(void);
    void     (*key_onoff_init)(void);
    int      (*key_onoff_pressed)(int *out_long_press);

    /* Keys & Switch GPIO */
    uint16_t (*keys_read_adc)(void);
    key_id_t (*keys_decode)(uint16_t adc);
    int      (*keys_read_rec_switch)(void);
    int      (*keys_read_hp_detect)(void);

    /* LCD Display */
    void     (*lcd_set_rotation)(int landscape);
    uint16_t (*lcd_get_width)(void);
    uint16_t (*lcd_get_height)(void);
    void     (*lcd_fill_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
    void     (*lcd_draw_str)(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);
    void     (*lcd_backlight_set)(uint8_t percent);

    /* exFAT File Logging */
    int      (*sd_write_file)(const char *dir, const char *name, const void *data, uint32_t len);
    int      (*sd_dump_log)(const char *name);

    /* Audio ADC Capture & DMAC CH0 (IRQ 60) Hook */
    int            (*audio_capture_start)(uint8_t gain_0_31);
    void           (*audio_capture_stop)(void);
    const int16_t *(*audio_pop_frame)(uint32_t *out_bytes);
    uint8_t        (*audio_get_peak_pct)(void);
    uint32_t       (*audio_get_frames)(void);
    uint32_t       (*audio_get_overruns)(void);
    void           (*set_dmac_ch0_irq)(void (*cb)(void));

    /* Classic Bluetooth & RF (bt_xrec_*: NULL in images without phone mode, BT=0 and HCI probe) */
    uint32_t (*bt_rf_init_calibrated)(uint32_t freq_drift, uint32_t clk6_hz);
    int      (*bt_xrec_start)(exfat_vol_t *vol, int sd_mounted);
    void     (*bt_xrec_poll)(void);
    void     (*bt_xrec_stop)(void);
    void     (*bt_xrec_open_pairing)(uint32_t duration_ms);
    void     (*bt_xrec_get_status)(bt_status_t *out);
} openfw_exp_api_t;

void openfw_exp_init_api(const openfw_exp_api_t *api);
void openfw_exp_bind_fs(exfat_vol_t *vol, int *sd_mounted_ptr);
int  openfw_exp_write(uint32_t off, const uint8_t *data, uint32_t len);
int  openfw_exp_run(uint32_t entry_off, uint32_t arg, openfw_exp_result_t *out);
int  openfw_exp_mem_read(uint32_t addr, uint8_t *dst, uint32_t len);
void openfw_exp_get_last_result(openfw_exp_result_t *out);
int  openfw_exp_sd_write_file(const char *dir, const char *name, const void *data, uint32_t len);
int  openfw_exp_sd_dump_log(const char *name);
void openfw_exp_set_dmac_ch0_irq(void (*cb)(void));

#ifdef OPENFW_HOST_MMIO
typedef uint32_t (*openfw_host_exp_fn_t)(const openfw_exp_api_t *api, uint32_t entry_off, uint32_t arg);
void openfw_exp_set_host_runner(openfw_host_exp_fn_t fn);
#endif

#endif /* OPENFW_EXP_H */
