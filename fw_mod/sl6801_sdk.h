/*
 * Smartlink SL6801 (pen133_2.0.65) Clean-Room XIP C SDK Header
 *
 * Verified Thumb-2 function pointers, hardware MMIO registers, and SRAM
 * structures for writing standalone C modules & LVGL v8.3 UI replacements
 * that execute in-place (XIP) from unused SPI NOR flash (0x00D74000..0x00DFC000).
 *
 * Rules for XIP C modules:
 *   - No writable globals (.data/.bss forbidden in XIP flash).
 *   - Use the verified 21 KB unused SRAM gap (0x0086AC08..0x0086FFFF) or
 *     oal_malloc() / oal_free() for runtime state.
 *   - LVGL v8.3 functions (lv_*) are SAFE ONLY from ViewTask (CPU1).
 *   - FatFs, PSMP nv_*, and BT/Media queue helpers are thread-safe from CPU0.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL6801_SDK_H
#define SL6801_SDK_H

#include <stdint.h>
#include <stddef.h>

#define FW_FN(addr, ret, args) ((ret (*) args)((uintptr_t)(addr) | 1u))

/* ---- Boot ROM libc & FIRM formatting ---- */
#define rom_memset                FW_FN(0x00000BAA, void *,   (void *dst, int val, size_t n))
#define rom_memcpy                FW_FN(0x00000B2A, void *,   (void *dst, const void *src, size_t n))
#define rom_strlen                FW_FN(0x00000AA2, size_t,   (const char *s))
#define rom_printf                FW_FN(0x000002B0, int,      (const char *fmt, ...)) /* UART0 PD0 115200 */
#define fw_sprintf                FW_FN(0x00D73428, int,      (char *buf, const char *fmt, ...))

/* ---- OAL / FreeRTOS / Timing ---- */
#define oal_malloc                FW_FN(0x00D3BBC8, void *,   (uint32_t size))
#define oal_free                  FW_FN(0x00D3BBD4, void,     (void *ptr))
#define OAL_get_tick              FW_FN(0x00CE9106, uint32_t, (void)) /* 1000 Hz = 1 ms/tick */
#define fw_timer_reset            FW_FN(0x00D1BCD4, int,      (const char *name))
#define SYSTICK_VAL               (*(const volatile uint32_t *)0xE000E018u)

/* ---- PSMP NVRAM (thread-safe, wear-leveled, CRC-32, survives OTA) ---- */
#define nv_get_val                FW_FN(0x00D405CC, int,      (const char *key, void *buf, uint32_t max_len))
#define nv_set_val                FW_FN(0x00D405E0, int,      (const char *key, const void *buf, uint32_t len))
#define nv_del_val                FW_FN(0x00D405F4, int,      (const char *key))

/* ---- FatFs R0.14 (exFAT enabled, 64-bit FSIZE_t) ---- */
#define FA_READ                   0x01u
#define FA_WRITE                  0x02u
#define FA_CREATE_ALWAYS          0x08u
#define FA_OPEN_ALWAYS            0x10u
#define f_opendir                 FW_FN(0x00D4BA76, int,      (void *dp, const char *path))
#define f_readdir                 FW_FN(0x00D4BB80, int,      (void *dp, void *fno))
#define f_closedir                FW_FN(0x00D4BB52, int,      (void *dp))
#define f_open                    FW_FN(0x00D4C280, int,      (void *fp, const char *path, uint8_t mode))
#define f_lseek                   FW_FN(0x00D4B6BE, int,      (void *fp, uint64_t ofs))
#define f_read                    FW_FN(0x00D4B308, int,      (void *fp, void *buff, uint32_t btr, uint32_t *br))
#define f_write                   FW_FN(0x00D4B4C6, int,      (void *fp, const void *buff, uint32_t btw, uint32_t *bw))
#define f_close                   FW_FN(0x00D4C750, int,      (void *fp))
#define f_unlink                  FW_FN(0x00D4BDDA, int,      (const char *path))
#define f_getfree                 FW_FN(0x00D4BCBC, int,      (const char *path, uint32_t *nclst, void **fatfs))

/* ---- PMU, Reboot, RTC & Backlight ---- */
typedef struct __attribute__((packed)) {
    uint16_t year;    /* years since 2000 (0..99) */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint16_t weekday; /* 0..6 */
    uint8_t  hour;    /* 0..23 */
    uint8_t  minute;  /* 0..59 */
    uint8_t  second;  /* 0..59 */
    uint8_t  pad;
} sl_rtc_time_t;

#define HAL_rtc_get               FW_FN(0x0080D4E4, int,      (sl_rtc_time_t *out))
#define HAL_rtc_set               FW_FN(0x00CE88D4, int,      (const sl_rtc_time_t *in))
#define HAL_rtc_set_alarm         FW_FN(0x00CE88E0, int,      (int type, const void *cfg))
#define HAL_pmu_sd_upd_set        FW_FN(0x00CE84B0, void,     (int en))
#define do_reboot_mode            FW_FN(0x00CEE05C, void,     (void))
#define lcd_set_backlight         FW_FN(0x00D16560, void,     (uint8_t percent)) /* 0..100 */
#define PMU_STATE_ADDR            ((const volatile uint8_t *)0x00821222u)
#define PMU_VOLTAGE_MV_ADDR       ((const volatile uint16_t *)0x0082122Au)

/* ---- Audio & Recording ---- */
#define HAL_audio_ioctl           FW_FN(0x00D3C186, int,      (int dev, uint32_t cmd, uint32_t arg))
#define media_get_status          FW_FN(0x00D1AB1C, uint8_t,  (void))
#define pstr_audio_get_rec_time   FW_FN(0x00CF04DC, uint32_t, (void))
#define pstr_audio_start_record   FW_FN(0x00CF07B0, void,     (const char *basename))
#define media_stop_record_if_act  FW_FN(0x00D1AAD4, void,     (void))
#define pstr_audio_rec_nv_write   FW_FN(0x00CF0964, int,      (void))
#define RECORD_CFG_RAM            ((volatile uint8_t *)0x0081B9C8u) /* +0:kbps +1:vad +2:vad_lvl +3:seg +4:mon +0x14:denoise */

/* ---- Bluetooth Stack (CPU0 bt_host_task) ---- */
#define bt_open_stack             FW_FN(0x00D66E14, void,     (void))
#define bt_close_stack            FW_FN(0x00D66E3C, void,     (void))
#define bt_send_msg               FW_FN(0x00D66C38, int,      (uint8_t dst_mod, const void *msg, uint16_t len))
#define hfp_rfcomm_send           FW_FN(0x00D620E6, void,     (void *hf, const void *buf, uint16_t len))
#define BT_HOST_STATE             (*(const volatile uint8_t *)0x0081DF1Cu) /* 0x0C == OPEN */

/* ---- LVGL v8.3 UI API (CPU1 ViewTask ONLY; ST7735S 80x160 RGB565) ---- */
#define LV_ALIGN_TOP_MID          2u
#define LV_ALIGN_BOTTOM_MID       5u
#define LV_ALIGN_CENTER           9u

#define lv_disp_get_scr_act       FW_FN(0x00D1CCA0, void *,   (void *disp))
#define lv_label_create           FW_FN(0x00D33F80, void *,   (void *parent))
#define lv_label_set_text         FW_FN(0x00D34A2C, void,     (void *label, const char *text))
#define lv_label_set_text_fmt     FW_FN(0x00D34AEC, void,     (void *label, const char *fmt, ...))
#define lv_obj_align              FW_FN(0x00D2018E, void,     (void *obj, uint8_t align, int16_t x_ofs, int16_t y_ofs))
#define lv_obj_set_style_bg_color FW_FN(0x00D22E7E, void,     (void *obj, uint16_t rgb565, uint32_t sel))
#define lv_obj_set_style_bg_opa   FW_FN(0x00D22EA0, void,     (void *obj, uint8_t opa, uint32_t sel))
#define lv_obj_set_style_txt_clr  FW_FN(0x00D22EEC, void,     (void *obj, uint16_t rgb565, uint32_t sel))

/* Embedded stock LVGL image descriptor VMAs (extract/replace via tools/assets.py) */
#define IMG_WELCOME_VMA           0x00C55CB8u /* 80x40 cf=4 RGB565 */
#define IMG_ICON_BT_VMA           0x00C94A08u /* 56x58 cf=6 */
#define IMG_ICON_FILES_VMA        0x00C96580u /* 56x58 cf=6 */
#define IMG_ICON_MUSIC_VMA        0x00C97EECu /* 56x58 cf=6 */
#define IMG_ICON_RECORD_VMA       0x00C99858u /* 56x58 cf=6 */
#define IMG_ICON_SETTINGS_VMA     0x00C9B1C4u /* 56x58 cf=6 */

/* RGB565 helper (little-endian host -> ST7735S RGB565) */
static inline uint16_t sl_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

#endif /* SL6801_SDK_H */
