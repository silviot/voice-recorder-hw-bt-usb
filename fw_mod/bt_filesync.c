/*
 * Smartlink SL6801 (pen133_2.0.65) Bluetooth RFCOMM File Transfer XIP Extension
 *
 * Linked to run in-place (XIP) from unused SPI NOR flash at 0x00D74000
 * (flash offset 0x174000, well past FIRM end 0x173E8C and before PSMP 0x1FC000).
 *
 * Hook site:
 *   0x00D625E2 (flash 0x1625E2) in _hfp_hf_slc_open_status:
 *     Original: bl 0x00D62180  (hfp_parse_at_cmd(hf_ctx, data, len))
 *     Patched:  bl 0x00D74000  (bt_filesync_hook(hf_ctx, data, len))
 *
 * Behavior:
 *   - If incoming RFCOMM Channel 1 payload does NOT start with "+XREC:",
 *     tail-calls original hfp_parse_at_cmd(hf_ctx, data, len) untouched.
 *   - If payload starts with "+XREC:", handles:
 *       +XREC:PING\r
 *         -> "+XREC:PONG,pen133_2.0.65\r\n"
 *       +XREC:LS\r
 *         -> "+XREC:F,<size_dec>,<filename>\r\n" (one per file in 0:\record)
 *         -> "+XREC:END\r\n"
 *       +XREC:GET,<offset_dec>,<len_dec>,<filename>\r
 *         -> 12-byte binary header:
 *              [0..3]  "XREC"
 *              [4..7]  uint32_le offset
 *              [8..9]  uint16_le bytes_read (0..256)
 *              [10..11] uint16_le crc16_ccitt_false(payload)
 *            followed by <bytes_read> raw bytes of 0:\record\<filename>
 *         -> on error, "+XREC:ERR,<fatfs_rc>\r\n"
 *       +XREC:GETS,<offset_dec>,<total_len_dec>,<chunk_len_dec>,<filename>\r
 *         -> Streams up to <total_len_dec> bytes (clamped to 16384 B per burst)
 *            as consecutive 12-byte "XREC" binary frames of <= <chunk_len_dec>
 *            bytes (1..256) with a single f_open/f_lseek/f_close.
 *       +XREC:DEL,<filename>\r
 *         -> Deletes 0:\record\<filename> via f_unlink -> "+XREC:OK,DEL\r\n"
 *       +XREC:RTC\r
 *         -> Reads hardware RTC via HAL_rtc_get -> "+XREC:RTC,YYYY-MM-DD HH:MM:SS,W\r\n"
 *       +XREC:RTC,<YYYY>,<MM>,<DD>,<hh>,<mm>,<ss>,<wd>\r
 *         -> Sets hardware RTC via HAL_rtc_set -> "+XREC:OK,RTC\r\n"
 *       +XREC:RECSTART[,<filename>]\r
 *         -> Starts a local stereo WAV recording (default name from RTC) while keeping
 *            BT HFP connected -> "+XREC:OK,RECSTART,<filename>\r\n"
 *       +XREC:RECSTOP\r
 *         -> Stops active local stereo recording cleanly -> "+XREC:OK,RECSTOP\r\n"
 *       +XREC:STAT\r
 *         -> Returns live battery %, voltage_mv, charge_status, vbus, media_status,
 *            rec_sec, free_mb, total_mb:
 *            "+XREC:STAT,<bat%>,<mv>,<chg>,<vbus>,<mstat>,<rec_s>,<free_mb>,<tot_mb>\r\n"
 *     and returns 0 (cleanly exits _hfp_hf_slc_open_status via jump table[0] -> 0xD624E8).
 *
 * Rules:
 *   - No writable globals (.data/.bss forbidden in XIP flash).
 *   - All scratch structs (DIR, FIL, FILINFO, I/O buffer) allocated via oal_malloc()
 *     and freed before return to avoid overflowing the BT task stack.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>

/* Firmware & Boot ROM function pointers (Thumb bit = | 1) for pen133_2.0.65 */
#define FW_FN(addr, ret, args) ((ret (*) args)((uintptr_t)(addr) | 1u))

/* HFP / RFCOMM */
#define orig_hfp_parse_at_cmd FW_FN(0x00D62180, uint32_t, (void *hf, const uint8_t *data, uint16_t len))
#define hfp_rfcomm_send       FW_FN(0x00D620E6, void,     (void *hf, const void *buf, uint16_t len))

/* OAL / FreeRTOS Heap */
#define oal_malloc            FW_FN(0x00D3BBC8, void *,   (uint32_t size))
#define oal_free              FW_FN(0x00D3BBD4, void,     (void *ptr))

/* FatFs (exFAT enabled, 64-bit FSIZE_t) */
#define f_opendir             FW_FN(0x00D4BA76, int,      (void *dp, const char *path))
#define f_readdir             FW_FN(0x00D4BB80, int,      (void *dp, void *fno))
#define f_closedir            FW_FN(0x00D4BB52, int,      (void *dp))
#define f_open                FW_FN(0x00D4C280, int,      (void *fp, const char *path, uint8_t mode))
#define f_lseek               FW_FN(0x00D4B6BE, int,      (void *fp, uint64_t ofs))
#define f_read                FW_FN(0x00D4B308, int,      (void *fp, void *buff, uint32_t btr, uint32_t *br))
#define f_write               FW_FN(0x00D4B4C6, int,      (void *fp, const void *buff, uint32_t btw, uint32_t *bw))
#define f_close               FW_FN(0x00D4C750, int,      (void *fp))
#define f_unlink              FW_FN(0x00D4BDDA, int,      (const char *path))
#define f_getfree             FW_FN(0x00D4BCBC, int,      (const char *path, uint32_t *nclst, void **fatfs))

/* Timer, PMU, Reboot & Post-Welcome BT Autoboot (from 01_autoboot_ota_idle.md) */
#define fw_timer_reset        FW_FN(0x00D1BCD4, int,      (const char *name))
#define bt_open_stack         FW_FN(0x00D66E14, void,     (void))
#define HAL_pmu_sd_upd_set    FW_FN(0x00CE84B0, void,     (int en))
#define do_reboot_mode        FW_FN(0x00CEE05C, void,     (void))
#define STR_IDLESHUT          ((const char *)0x00C4B944u) /* "idleshut" */
#define STR_SCREEN            ((const char *)0x00CB0EFCu) /* "screen"   */

/* v5 Casual-User Pairing & Security (from 02_pairing_security_hooks.md) */
#define nv_get_val            FW_FN(0x00D405CC, int,      (const char *key, void *buf, uint32_t max_len))
#define nv_set_val            FW_FN(0x00D405E0, int,      (const char *key, const void *buf, uint32_t len))
#define OAL_get_tick          FW_FN(0x00CE9106, uint32_t, (void))
#define page_bt_menu_create   FW_FN(0x00D02D64, int,      (void))
#define bt_send_msg           FW_FN(0x00D66C38, int,      (uint8_t dst_mod, const void *msg, uint16_t len))
#define BT_HOST_STATE         (*(const volatile uint8_t *)0x0081DF1Cu) /* 0x0C == OPEN */
#define SYSTICK_VAL           (*(const volatile uint32_t *)0xE000E018u)

/* LVGL v8.3 helpers (SAFE ONLY from ViewTask, e.g. inside page_bt_menu_create hook) */
#define lv_disp_get_scr_act       FW_FN(0x00D1CCA0, void *, (void *disp))
#define lv_label_create           FW_FN(0x00D33F80, void *, (void *parent))
#define lv_label_set_text_fmt     FW_FN(0x00D34AEC, void,   (void *label, const char *fmt, ...))
#define lv_obj_align              FW_FN(0x00D2018E, void,   (void *obj, uint8_t align, int16_t x_ofs, int16_t y_ofs))
#define lv_obj_set_style_bg_color FW_FN(0x00D22E7E, void,   (void *obj, uint16_t rgb565, uint32_t sel))
#define lv_obj_set_style_bg_opa   FW_FN(0x00D22EA0, void,   (void *obj, uint8_t opa, uint32_t sel))
#define lv_obj_set_style_txt_clr  FW_FN(0x00D22EEC, void,   (void *obj, uint16_t rgb565, uint32_t sel))

/* Verified unused 512-byte SRAM gap (between MSP top 0x0086AC08 and heap 0x0086B200) */
#define XREC_RT_SRAM_ADDR     0x0086B000u
#define XREC_RT_MAGIC         0x58525335u /* "XRS5" */
#define XREC_NV_MAGIC         0x58524B31u /* "XRK1" */
#define XREC_PAIR_WINDOW_MS   60000u
#define XREC_MAX_PAIR_FAILS   5u

#define FA_WRITE              0x02u
#define FA_CREATE_ALWAYS      0x08u
#define FA_OPEN_ALWAYS        0x10u

/* Media / Local Stereo Recorder (thread-safe xQueueSend to Media Task ID 8) */
#define media_get_status          FW_FN(0x00D1AB1C, uint8_t,  (void))
#define pstr_audio_get_rec_time   FW_FN(0x00CF04DC, uint32_t, (void))
#define pstr_audio_start_record   FW_FN(0x00CF07B0, void,     (const char *basename))
#define media_stop_record_if_act  FW_FN(0x00D1AAD4, void,     (void))

/* Lock-free live PMU battery/charging state in SRAM (0x00821222) */
#define PMU_STATE_ADDR            ((const volatile uint8_t *)0x00821222u)
#define PMU_VOLTAGE_MV_ADDR       ((const volatile uint16_t *)0x0082122Au)

/* Hardware RTC HAL (lock-free MMIO) */
typedef struct __attribute__((packed)) {
    uint16_t year;    /* years since 2000 (0..99) */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint16_t weekday; /* 0..6 */
    uint8_t  hour;    /* 0..23 */
    uint8_t  minute;  /* 0..59 */
    uint8_t  second;  /* 0..59 */
    uint8_t  pad;
} rtc_time_t;

#define HAL_rtc_get           FW_FN(0x0080D4E4, int,      (rtc_time_t *out))
#define HAL_rtc_set           FW_FN(0x00CE88D4, int,      (const rtc_time_t *in))

/* Boot ROM libc & FIRM sprintf */
#define rom_memset            FW_FN(0x00000BAA, void *,   (void *dst, int val, size_t n))
#define rom_memcpy            FW_FN(0x00000B2A, void *,   (void *dst, const void *src, size_t n))
#define rom_strlen            FW_FN(0x00000AA2, size_t,   (const char *s))
#define fw_sprintf            FW_FN(0x00D73428, int,      (char *buf, const char *fmt, ...))

/* Struct sizes (generously over-allocated via oal_malloc for safety):
 *   DIR     <= 0x80 bytes (firmware stack uses 0x58)
 *   FIL     <= 0x300 bytes (contains 512-byte sector buf at +0x54)
 *   FILINFO <= 0x160 bytes (fsize u64 @ +0x00, fattrib u8 @ +0x0C, fname[256] @ +0x1A)
 *
 * RFCOMM MTU & Credit Limits (verified in 0xD636E0 / 0xD63962 / 0xD6429A):
 *   - Negotiated dlci->mtu = 512 B (0x200); L2CAP MTU = 672 B; HCI ACL cap = 674 B.
 *     12-byte XREC header + 496 B payload = 508 B <= 512 B (safe per hfp_rfcomm_send).
 *   - RFCOMM uses credit-based flow control (K=7 initial credits) with NO TX queueing:
 *     packets sent when remote_credits == 0 are silently dropped! Because this hook runs
 *     inside the BT task, cap each +XREC:GETS burst to <= 4 frames so credits never run dry.
 */
#define DIR_ALLOC_SIZE     0x100u
#define FIL_ALLOC_SIZE     0x300u
#define FILINFO_ALLOC_SIZE 0x160u
#define MAX_CHUNK_LEN      496u
#define MAX_BURST_FRAMES   4u
#define MAX_BURST_LEN      (MAX_CHUNK_LEN * MAX_BURST_FRAMES)
#define FA_READ            0x01u

static uint16_t crc16_ccitt_false(const uint8_t *buf, uint32_t len)
{
    uint32_t c = 0xFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t t = ((c >> 8) ^ buf[i]) & 0xFFu;
        t ^= (t >> 4);
        c = ((c << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFFu;
    }
    return (uint16_t)c;
}

static int starts_with(const uint8_t *data, uint16_t len, const char *prefix)
{
    uint16_t i = 0;
    while (prefix[i] != '\0') {
        if (i >= len || data[i] != (uint8_t)prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static uint32_t parse_u32(const uint8_t *p, const uint8_t *end, const uint8_t **out_next)
{
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
    }
    if (out_next) {
        *out_next = p;
    }
    return v;
}

static void send_str(void *hf, const char *s)
{
    uint16_t n = (uint16_t)rom_strlen(s);
    if (n > 0) {
        hfp_rfcomm_send(hf, s, n);
    }
}

static void handle_ls(void *hf)
{
    uint8_t *dp = (uint8_t *)oal_malloc(DIR_ALLOC_SIZE);
    uint8_t *fno = (uint8_t *)oal_malloc(FILINFO_ALLOC_SIZE);
    char *line = (char *)oal_malloc(320);
    if (!dp || !fno || !line) {
        if (dp) oal_free(dp);
        if (fno) oal_free(fno);
        if (line) oal_free(line);
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    rom_memset(dp, 0, DIR_ALLOC_SIZE);
    rom_memset(fno, 0, FILINFO_ALLOC_SIZE);

    int rc = f_opendir(dp, "0:\\record");
    if (rc != 0) {
        fw_sprintf(line, "+XREC:ERR,%d\r\n", rc);
        send_str(hf, line);
    } else {
        for (int limit = 0; limit < 512; limit++) {
            rom_memset(fno, 0, FILINFO_ALLOC_SIZE);
            rc = f_readdir(dp, fno);
            if (rc != 0) {
                break;
            }
            const char *fname = (const char *)(fno + 0x1A);
            if (fname[0] == '\0') {
                break;
            }
            uint8_t attrib = fno[0x0C];
            if (attrib & 0x10u) {
                continue; /* skip subdirectories */
            }
            uint32_t fsize_lo = *(const uint32_t *)(fno + 0x00);
            fw_sprintf(line, "+XREC:F,%u,%s\r\n", fsize_lo, fname);
            send_str(hf, line);
        }
        f_closedir(dp);
        send_str(hf, "+XREC:END\r\n");
    }

    oal_free(line);
    oal_free(fno);
    oal_free(dp);
}

static uint32_t build_record_path(char *path, const uint8_t *p, const uint8_t *end)
{
    const char *dir_prefix = "0:\\record\\";
    uint32_t pi = 0;
    while (dir_prefix[pi] != '\0') {
        path[pi] = dir_prefix[pi];
        pi++;
    }
    uint32_t name_len = 0;
    while (p < end && *p != '\r' && *p != '\n' && *p != '\0' && name_len < 240u) {
        if (*p == '/' || *p == '\\') {
            return 0;
        }
        path[pi++] = (char)*p++;
        name_len++;
    }
    path[pi] = '\0';
    return name_len;
}

static void send_xrec_frame(void *hf, uint8_t *pkt, uint32_t offset, uint32_t br)
{
    uint16_t crc = crc16_ccitt_false(pkt + 12u, br);
    pkt[0] = 'X';
    pkt[1] = 'R';
    pkt[2] = 'E';
    pkt[3] = 'C';
    pkt[4] = (uint8_t)(offset & 0xFFu);
    pkt[5] = (uint8_t)((offset >> 8) & 0xFFu);
    pkt[6] = (uint8_t)((offset >> 16) & 0xFFu);
    pkt[7] = (uint8_t)((offset >> 24) & 0xFFu);
    pkt[8] = (uint8_t)(br & 0xFFu);
    pkt[9] = (uint8_t)((br >> 8) & 0xFFu);
    pkt[10] = (uint8_t)(crc & 0xFFu);
    pkt[11] = (uint8_t)((crc >> 8) & 0xFFu);

    hfp_rfcomm_send(hf, pkt, (uint16_t)(12u + br));
}

static void handle_get_stream(void *hf, const uint8_t *args, uint16_t args_len, int is_stream)
{
    const uint8_t *p = args;
    const uint8_t *end = args + args_len;

    /* GET:  <offset>,<len>,<filename>[\r\n]
     * GETS: <offset>,<total_len>,<chunk_len>,<filename>[\r\n]
     */
    uint32_t offset = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,ARG1\r\n");
        return;
    }
    p++;

    uint32_t total_len = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,ARG2\r\n");
        return;
    }
    p++;

    uint32_t chunk_len = total_len;
    if (is_stream) {
        chunk_len = parse_u32(p, end, &p);
        if (p >= end || *p != ',') {
            send_str(hf, "+XREC:ERR,ARG3\r\n");
            return;
        }
        p++;
        if (total_len == 0) {
            total_len = 1;
        }
        if (total_len > MAX_BURST_LEN) {
            total_len = MAX_BURST_LEN;
        }
    } else {
        if (total_len == 0) {
            total_len = 1;
        }
        if (total_len > MAX_CHUNK_LEN) {
            total_len = MAX_CHUNK_LEN;
        }
        chunk_len = total_len;
    }

    if (chunk_len == 0) {
        chunk_len = 1;
    }
    if (chunk_len > MAX_CHUNK_LEN) {
        chunk_len = MAX_CHUNK_LEN;
    }

    char *path = (char *)oal_malloc(300);
    uint8_t *fp = (uint8_t *)oal_malloc(FIL_ALLOC_SIZE);
    uint8_t *pkt = (uint8_t *)oal_malloc(12u + MAX_CHUNK_LEN);
    if (!path || !fp || !pkt) {
        if (path) oal_free(path);
        if (fp) oal_free(fp);
        if (pkt) oal_free(pkt);
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    if (build_record_path(path, p, end) == 0) {
        send_str(hf, "+XREC:ERR,NAME\r\n");
        goto cleanup;
    }

    rom_memset(fp, 0, FIL_ALLOC_SIZE);
    int rc = f_open(fp, path, FA_READ);
    if (rc != 0) {
        fw_sprintf((char *)pkt, "+XREC:ERR,OPEN,%d\r\n", rc);
        send_str(hf, (const char *)pkt);
        goto cleanup;
    }

    if (offset > 0) {
        rc = f_lseek(fp, (uint64_t)offset);
        if (rc != 0) {
            f_close(fp);
            fw_sprintf((char *)pkt, "+XREC:ERR,SEEK,%d\r\n", rc);
            send_str(hf, (const char *)pkt);
            goto cleanup;
        }
    }

    uint32_t sent = 0;
    uint32_t frames = 0;
    uint32_t max_frames = is_stream ? MAX_BURST_FRAMES : 1u;
    while (sent < total_len && frames < max_frames) {
        uint32_t want = total_len - sent;
        if (want > chunk_len) {
            want = chunk_len;
        }
        uint32_t br = 0;
        rc = f_read(fp, pkt + 12u, want, &br);
        if (rc != 0) {
            fw_sprintf((char *)pkt, "+XREC:ERR,READ,%d\r\n", rc);
            send_str(hf, (const char *)pkt);
            break;
        }
        send_xrec_frame(hf, pkt, offset + sent, br);
        sent += br;
        frames++;
        if (br < want) {
            break; /* EOF reached */
        }
    }
    f_close(fp);

cleanup:
    oal_free(pkt);
    oal_free(fp);
    oal_free(path);
}

static void handle_del(void *hf, const uint8_t *args, uint16_t args_len)
{
    char *path = (char *)oal_malloc(300);
    char *resp = (char *)oal_malloc(64);
    if (!path || !resp) {
        if (path) oal_free(path);
        if (resp) oal_free(resp);
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    if (build_record_path(path, args, args + args_len) == 0) {
        send_str(hf, "+XREC:ERR,NAME\r\n");
    } else {
        int rc = f_unlink(path);
        if (rc == 0) {
            send_str(hf, "+XREC:OK,DEL\r\n");
        } else {
            fw_sprintf(resp, "+XREC:ERR,DEL,%d\r\n", rc);
            send_str(hf, resp);
        }
    }

    oal_free(resp);
    oal_free(path);
}

static void handle_rtc(void *hf, const uint8_t *args, uint16_t args_len)
{
    char *resp = (char *)oal_malloc(64);
    if (!resp) {
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    rtc_time_t t;
    rom_memset(&t, 0, sizeof(t));

    if (args_len == 0 || args[0] == '\r' || args[0] == '\n') {
        HAL_rtc_get(&t);
        fw_sprintf(
            resp,
            "+XREC:RTC,%04u-%02u-%02u %02u:%02u:%02u,%u\r\n",
            (unsigned)(2000u + t.year),
            (unsigned)t.month,
            (unsigned)t.day,
            (unsigned)t.hour,
            (unsigned)t.minute,
            (unsigned)t.second,
            (unsigned)t.weekday
        );
        send_str(hf, resp);
        oal_free(resp);
        return;
    }

    /* Set format: ,<YYYY>,<MM>,<DD>,<hh>,<mm>,<ss>,<wd> */
    const uint8_t *p = args;
    const uint8_t *end = args + args_len;
    if (*p != ',') {
        send_str(hf, "+XREC:ERR,RTCARG\r\n");
        oal_free(resp);
        return;
    }
    p++;

    uint32_t vals[7];
    for (int i = 0; i < 7; i++) {
        vals[i] = parse_u32(p, end, &p);
        if (i < 6) {
            if (p >= end || *p != ',') {
                send_str(hf, "+XREC:ERR,RTCARG\r\n");
                oal_free(resp);
                return;
            }
            p++;
        }
    }

    if (vals[0] < 2000u || vals[0] > 2099u || vals[1] < 1u || vals[1] > 12u ||
        vals[2] < 1u || vals[2] > 31u || vals[3] > 23u || vals[4] > 59u ||
        vals[5] > 59u || vals[6] > 6u) {
        send_str(hf, "+XREC:ERR,RTCRNG\r\n");
        oal_free(resp);
        return;
    }

    t.year    = (uint16_t)(vals[0] - 2000u);
    t.month   = (uint8_t)vals[1];
    t.day     = (uint8_t)vals[2];
    t.hour    = (uint8_t)vals[3];
    t.minute  = (uint8_t)vals[4];
    t.second  = (uint8_t)vals[5];
    t.weekday = (uint16_t)vals[6];

    HAL_rtc_set(&t);
    send_str(hf, "+XREC:OK,RTC\r\n");
    oal_free(resp);
}

static void handle_recstart(void *hf, const uint8_t *args, uint16_t args_len)
{
    uint8_t st = media_get_status();
    if (st == 0x21u || st == 0x22u) {
        send_str(hf, "+XREC:ERR,RECACTIVE\r\n");
        return;
    }
    if (st != 0x10u && st != 0x00u && st != 0x20u) {
        send_str(hf, "+XREC:ERR,BUSY\r\n");
        return;
    }

    char *fname = (char *)oal_malloc(128);
    char *resp = (char *)oal_malloc(160);
    if (!fname || !resp) {
        if (fname) oal_free(fname);
        if (resp) oal_free(resp);
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    if (args_len > 1 && args[0] == ',') {
        const uint8_t *p = args + 1;
        const uint8_t *end = args + args_len;
        uint32_t n = 0;
        while (p < end && *p != '\r' && *p != '\n' && *p != '\0' && n < 120u) {
            if (*p == '/' || *p == '\\') {
                n = 0;
                break;
            }
            fname[n++] = (char)*p++;
        }
        fname[n] = '\0';
        if (n == 0) {
            send_str(hf, "+XREC:ERR,NAME\r\n");
            oal_free(resp);
            oal_free(fname);
            return;
        }
    } else {
        rtc_time_t t;
        rom_memset(&t, 0, sizeof(t));
        HAL_rtc_get(&t);
        fw_sprintf(
            fname,
            "20%02u%02u%02u-%02u%02u%02u.wav",
            (unsigned)t.year,
            (unsigned)t.month,
            (unsigned)t.day,
            (unsigned)t.hour,
            (unsigned)t.minute,
            (unsigned)t.second
        );
    }

    pstr_audio_start_record(fname);
    fw_sprintf(resp, "+XREC:OK,RECSTART,%s\r\n", fname);
    send_str(hf, resp);

    oal_free(resp);
    oal_free(fname);
}

static void handle_recstop(void *hf)
{
    uint8_t st = media_get_status();
    if (st != 0x21u && st != 0x22u) {
        send_str(hf, "+XREC:ERR,NOTREC\r\n");
        return;
    }
    media_stop_record_if_act();
    send_str(hf, "+XREC:OK,RECSTOP\r\n");
}

static void handle_stat(void *hf)
{
    char *resp = (char *)oal_malloc(128);
    if (!resp) {
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    uint32_t bat_pct = PMU_STATE_ADDR[3];
    uint32_t chg_st  = PMU_STATE_ADDR[1];
    uint32_t vbus    = PMU_STATE_ADDR[2];
    uint32_t mv      = *PMU_VOLTAGE_MV_ADDR;
    uint32_t mstat   = media_get_status();
    uint32_t rec_s   = (mstat == 0x21u || mstat == 0x22u) ? pstr_audio_get_rec_time() : 0u;

    uint32_t free_mb = 0;
    uint32_t total_mb = 0;
    uint32_t free_clst = 0;
    void *fs = NULL;
    if (f_getfree("0:", &free_clst, &fs) == 0 && fs != NULL) {
        uint32_t csize = *(const uint16_t *)((const uint8_t *)fs + 0x0A);
        uint32_t n_fatent = *(const uint32_t *)((const uint8_t *)fs + 0x20);
        uint32_t total_clst = (n_fatent > 2u) ? (n_fatent - 2u) : 0u;
        /* 512 B/sector -> (clst * csize) / 2048 == MB; split shift to avoid 64-bit helper */
        free_mb  = ((free_clst >> 11) * csize) + (((free_clst & 0x7FFu) * csize) >> 11);
        total_mb = ((total_clst >> 11) * csize) + (((total_clst & 0x7FFu) * csize) >> 11);
    }

    fw_sprintf(
        resp,
        "+XREC:STAT,%u,%u,%u,%u,%u,%u,%u,%u\r\n",
        bat_pct, mv, chg_st, vbus, mstat, rec_s, free_mb, total_mb
    );
    send_str(hf, resp);
    oal_free(resp);
}

static void handle_put(void *hf, const uint8_t *args, uint16_t args_len)
{
    /* Binary upload frame for 0:\update.up (OTA) or 0:\record\<file>:
     *   +XREC:PUT,<offset_dec>,<len_dec>,<crc16_dec>,<dest_tag>:<raw_bytes>
     *   dest_tag: "OTA" -> "0:\update.up"
     */
    const uint8_t *p = args;
    const uint8_t *end = args + args_len;

    uint32_t offset = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,PUTARG1\r\n");
        return;
    }
    p++;

    uint32_t data_len = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,PUTARG2\r\n");
        return;
    }
    p++;

    uint32_t exp_crc = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,PUTARG3\r\n");
        return;
    }
    p++;

    const uint8_t *colon = p;
    while (colon < end && *colon != ':') {
        colon++;
    }
    if (colon >= end || (uint32_t)(end - (colon + 1)) < data_len || data_len == 0 || data_len > 460u) {
        send_str(hf, "+XREC:ERR,PUTLEN\r\n");
        return;
    }

    const uint8_t *payload = colon + 1;
    uint16_t calc_crc = crc16_ccitt_false(payload, data_len);
    if (calc_crc != (uint16_t)exp_crc) {
        send_str(hf, "+XREC:ERR,PUTCRC\r\n");
        return;
    }

    if (!starts_with(p, (uint16_t)(colon - p), "OTA")) {
        send_str(hf, "+XREC:ERR,PUTDST\r\n");
        return;
    }

    uint8_t *fp = (uint8_t *)oal_malloc(FIL_ALLOC_SIZE);
    char *resp = (char *)oal_malloc(64);
    if (!fp || !resp) {
        if (fp) oal_free(fp);
        if (resp) oal_free(resp);
        send_str(hf, "+XREC:ERR,ENOMEM\r\n");
        return;
    }

    rom_memset(fp, 0, FIL_ALLOC_SIZE);
    uint8_t mode = (offset == 0) ? (uint8_t)(FA_WRITE | FA_CREATE_ALWAYS) : (uint8_t)(FA_WRITE | FA_OPEN_ALWAYS);
    int rc = f_open(fp, "0:\\update.up", mode);
    if (rc != 0) {
        fw_sprintf(resp, "+XREC:ERR,PUTOPEN,%d\r\n", rc);
        send_str(hf, resp);
        goto cleanup;
    }

    if (offset > 0) {
        rc = f_lseek(fp, (uint64_t)offset);
        if (rc != 0) {
            f_close(fp);
            fw_sprintf(resp, "+XREC:ERR,PUTSEEK,%d\r\n", rc);
            send_str(hf, resp);
            goto cleanup;
        }
    }

    uint32_t bw = 0;
    rc = f_write(fp, payload, data_len, &bw);
    f_close(fp);

    if (rc != 0 || bw != data_len) {
        fw_sprintf(resp, "+XREC:ERR,PUTWR,%d,%u\r\n", rc, bw);
        send_str(hf, resp);
        goto cleanup;
    }

    fw_sprintf(resp, "+XREC:OK,PUT,%u,%u\r\n", offset, bw);
    send_str(hf, resp);

cleanup:
    oal_free(resp);
    oal_free(fp);
}

/* ============================================================================
 * v5 Casual-User Pairing & Security (SHA-256, HMAC-SHA256, PSMP, LVGL PIN)
 * ============================================================================ */

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

typedef struct {
    uint32_t h[8];
    uint8_t  buf[64];
    uint32_t buf_len;
    uint32_t total_len;
} sha256_ctx_t;

static void sha256_compress(sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[16];
    for (uint32_t i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24)
             | ((uint32_t)block[i * 4u + 1u] << 16)
             | ((uint32_t)block[i * 4u + 2u] << 8)
             | ((uint32_t)block[i * 4u + 3u]);
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t wi;
        if (i < 16u) {
            wi = w[i];
        } else {
            uint32_t w15 = w[(i - 15u) & 15u];
            uint32_t w2  = w[(i - 2u) & 15u];
            uint32_t s0  = rotr32(w15, 7u) ^ rotr32(w15, 18u) ^ (w15 >> 3);
            uint32_t s1  = rotr32(w2, 17u) ^ rotr32(w2, 19u) ^ (w2 >> 10);
            wi = w[(i - 16u) & 15u] + s0 + w[(i - 7u) & 15u] + s1;
            w[i & 15u] = wi;
        }
        uint32_t S1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + wi;
        uint32_t S0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->buf_len = 0;
    ctx->total_len = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    ctx->total_len += len;
    while (len > 0) {
        uint32_t take = 64u - ctx->buf_len;
        if (take > len) take = len;
        rom_memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == 64u) {
            sha256_compress(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    uint32_t bit_len_lo = ctx->total_len << 3;
    uint32_t bit_len_hi = ctx->total_len >> 29;
    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > 56u) {
        while (ctx->buf_len < 64u) ctx->buf[ctx->buf_len++] = 0;
        sha256_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56u) ctx->buf[ctx->buf_len++] = 0;
    ctx->buf[56] = (uint8_t)(bit_len_hi >> 24);
    ctx->buf[57] = (uint8_t)(bit_len_hi >> 16);
    ctx->buf[58] = (uint8_t)(bit_len_hi >> 8);
    ctx->buf[59] = (uint8_t)(bit_len_hi);
    ctx->buf[60] = (uint8_t)(bit_len_lo >> 24);
    ctx->buf[61] = (uint8_t)(bit_len_lo >> 16);
    ctx->buf[62] = (uint8_t)(bit_len_lo >> 8);
    ctx->buf[63] = (uint8_t)(bit_len_lo);
    sha256_compress(ctx, ctx->buf);
    for (uint32_t i = 0; i < 8u; i++) {
        out[i * 4u]      = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)(ctx->h[i]);
    }
}

static void hmac_sha256(const uint8_t key[32], const uint8_t *msg, uint32_t msg_len, uint8_t out[32])
{
    uint8_t pad[64];
    uint8_t inner[32];
    sha256_ctx_t ctx;

    for (uint32_t i = 0; i < 64u; i++) {
        pad[i] = (uint8_t)((i < 32u ? key[i] : 0u) ^ 0x36u);
    }
    sha256_init(&ctx);
    sha256_update(&ctx, pad, 64u);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    for (uint32_t i = 0; i < 64u; i++) {
        pad[i] = (uint8_t)((i < 32u ? key[i] : 0u) ^ 0x5cu);
    }
    sha256_init(&ctx);
    sha256_update(&ctx, pad, 64u);
    sha256_update(&ctx, inner, 32u);
    sha256_final(&ctx, out);
}

static int ct_memeq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t diff = 0;
    for (uint32_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int hex_nibble(uint8_t c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int parse_hex(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t out_bytes)
{
    if (src_len < out_bytes * 2u) return 0;
    for (uint32_t i = 0; i < out_bytes; i++) {
        int hi = hex_nibble(src[i * 2u]);
        int lo = hex_nibble(src[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return 0;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static void format_hex(const uint8_t *src, uint32_t n, char *dst)
{
    static const char HEX[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        dst[i * 2u]      = HEX[(src[i] >> 4) & 0xFu];
        dst[i * 2u + 1u] = HEX[src[i] & 0xFu];
    }
    dst[n * 2u] = '\0';
}

/* Persisted in PSMP under key "xrec_key" (44 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;      /* XREC_NV_MAGIC ("XRK1") */
    uint8_t  key[32];    /* 256-bit bound device secret */
    uint8_t  peer[6];    /* bound peer BD_ADDR (info/reference) */
    uint16_t crc16;      /* CRC-16/CCITT-FALSE over first 42 bytes */
} xrec_nv_key_t;

/* Runtime state in verified unused SRAM at 0x0086B000 */
typedef struct {
    uint32_t      init_magic;
    uint32_t      rng_counter;
    uint8_t       rng_pool[32];
    uint8_t       nv_loaded;
    uint8_t       nv_bound;
    uint8_t       pair_active;
    uint8_t       pair_fails;
    uint32_t      pair_start_ms;
    uint32_t      pair_pin;
    uint8_t       session_authed;
    uint8_t       nonce_valid;
    uint8_t       session_peer[6];
    void         *session_hf;
    uint32_t      last_auth_ms;
    uint8_t       session_nonce[16];
    xrec_nv_key_t nv;
} xrec_rt_t;

static inline xrec_rt_t *xrec_rt(void)
{
    xrec_rt_t *rt = (xrec_rt_t *)XREC_RT_SRAM_ADDR;
    if (rt->init_magic != XREC_RT_MAGIC) {
        rom_memset(rt, 0, sizeof(*rt));
        rt->init_magic = XREC_RT_MAGIC;
    }
    return rt;
}

static void xrec_rng_bytes(xrec_rt_t *rt, uint8_t *out, uint32_t n)
{
    uint32_t seed[8];
    rtc_time_t t;
    rom_memset(&t, 0, sizeof(t));
    HAL_rtc_get(&t);

    seed[0] = OAL_get_tick();
    seed[1] = SYSTICK_VAL;
    seed[2] = ++rt->rng_counter;
    seed[3] = (uint32_t)(*PMU_VOLTAGE_MV_ADDR);
    seed[4] = ((uint32_t)t.year << 16) | ((uint32_t)t.month << 8) | t.day;
    seed[5] = ((uint32_t)t.hour << 16) | ((uint32_t)t.minute << 8) | t.second;
    seed[6] = *(const volatile uint32_t *)0x0081DF1Cu;
    seed[7] = (uint32_t)(uintptr_t)out;

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, rt->rng_pool, 32u);
    sha256_update(&ctx, (const uint8_t *)seed, sizeof(seed));
    sha256_final(&ctx, rt->rng_pool);

    if (n > 32u) n = 32u;
    rom_memcpy(out, rt->rng_pool, n);
}

static int bt_set_discoverable(uint8_t en)
{
    if (BT_HOST_STATE != 0x0Cu) {
        return -1;
    }
    struct __attribute__((packed)) {
        uint16_t mod;   /* 0x0101 */
        uint16_t evt;   /* 0x8004 (USER_EVT_SET_DISCOVERABLE) */
        uint16_t plen;  /* 0x0001 */
        uint16_t pad;   /* 0x0000 */
        uint8_t  disc;  /* 0 = Page Scan only (0x02), 1 = Inquiry+Page (0x03) */
    } msg = { 0x0101u, 0x8004u, 1u, 0u, (uint8_t)(en ? 1u : 0u) };
    return bt_send_msg(4u, &msg, (uint16_t)sizeof(msg));
}

static void xrec_ensure_nv_loaded(xrec_rt_t *rt)
{
    if (rt->nv_loaded) return;
    rt->nv_loaded = 1;
    rt->nv_bound = 0;

    xrec_nv_key_t tmp;
    rom_memset(&tmp, 0, sizeof(tmp));
    int rc = nv_get_val("xrec_key", &tmp, (uint32_t)sizeof(tmp));
    if (rc == (int)sizeof(tmp) && tmp.magic == XREC_NV_MAGIC) {
        uint16_t calc = crc16_ccitt_false((const uint8_t *)&tmp, (uint32_t)(sizeof(tmp) - 2u));
        if (calc == tmp.crc16) {
            rom_memcpy(&rt->nv, &tmp, sizeof(tmp));
            rt->nv_bound = 1;
        }
    }
}

static int xrec_pair_window_open(xrec_rt_t *rt)
{
    if (!rt->pair_active) return 0;
    if ((OAL_get_tick() - rt->pair_start_ms) >= XREC_PAIR_WINDOW_MS) {
        rt->pair_active = 0;
        bt_set_discoverable(0u);
        return 0;
    }
    return 1;
}

static int xrec_is_authed(xrec_rt_t *rt, void *hf)
{
    if (!rt->session_authed) return 0;
    if (rt->session_hf != hf) return 0;
    const uint8_t *peer = (const uint8_t *)hf + 6u;
    if (!ct_memeq(rt->session_peer, peer, 6u)) return 0;
    if ((OAL_get_tick() - rt->last_auth_ms) > 600000u) {
        rt->session_authed = 0;
        return 0;
    }
    rt->last_auth_ms = OAL_get_tick();
    return 1;
}

static void handle_chal(void *hf, xrec_rt_t *rt)
{
    rt->session_authed = 0;
    rt->session_hf = hf;
    rom_memcpy(rt->session_peer, (const uint8_t *)hf + 6u, 6u);
    xrec_rng_bytes(rt, rt->session_nonce, 16u);
    rt->nonce_valid = 1;

    char hex[33];
    format_hex(rt->session_nonce, 16u, hex);
    char resp[64];
    fw_sprintf(resp, "+XREC:CHAL,%s\r\n", hex);
    send_str(hf, resp);
}

static void handle_pair(void *hf, xrec_rt_t *rt, const uint8_t *args, uint16_t args_len)
{
    if (!xrec_pair_window_open(rt)) {
        send_str(hf, "+XREC:ERR,PAIRCLOSED\r\n");
        return;
    }
    /* Expected: <6-digit-PIN>,<64-hex-key> (min 71 chars) */
    const uint8_t *p = args;
    const uint8_t *end = args + args_len;
    while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) {
        end--;
    }
    if ((uint32_t)(end - p) < 71u || p[6] != ',') {
        send_str(hf, "+XREC:ERR,PAIRARG\r\n");
        return;
    }

    char expected_pin[8];
    fw_sprintf(expected_pin, "%06u", rt->pair_pin);
    if (!ct_memeq(p, (const uint8_t *)expected_pin, 6u)) {
        rt->pair_fails++;
        if (rt->pair_fails >= XREC_MAX_PAIR_FAILS) {
            rt->pair_active = 0;
            bt_set_discoverable(0u);
        }
        send_str(hf, "+XREC:ERR,PIN\r\n");
        return;
    }

    xrec_nv_key_t nv;
    rom_memset(&nv, 0, sizeof(nv));
    if (!parse_hex(p + 7, (uint32_t)(end - (p + 7)), nv.key, 32u)) {
        send_str(hf, "+XREC:ERR,PAIRKEY\r\n");
        return;
    }

    nv.magic = XREC_NV_MAGIC;
    rom_memcpy(nv.peer, (const uint8_t *)hf + 6u, 6u);
    nv.crc16 = crc16_ccitt_false((const uint8_t *)&nv, (uint32_t)(sizeof(nv) - 2u));

    int rc = nv_set_val("xrec_key", &nv, (uint32_t)sizeof(nv));
    if (rc != 0) {
        char resp[48];
        fw_sprintf(resp, "+XREC:ERR,NV,%d\r\n", rc);
        send_str(hf, resp);
        return;
    }

    rom_memcpy(&rt->nv, &nv, sizeof(nv));
    rt->nv_loaded = 1;
    rt->nv_bound = 1;
    rt->pair_active = 0;
    bt_set_discoverable(0u);

    rt->session_authed = 1;
    rt->session_hf = hf;
    rom_memcpy(rt->session_peer, nv.peer, 6u);
    rt->last_auth_ms = OAL_get_tick();
    rt->nonce_valid = 0;

    send_str(hf, "+XREC:OK,PAIR\r\n");
}

static void handle_auth(void *hf, xrec_rt_t *rt, const uint8_t *args, uint16_t args_len)
{
    xrec_ensure_nv_loaded(rt);
    if (!rt->nv_bound) {
        send_str(hf, "+XREC:ERR,UNBOUND\r\n");
        return;
    }
    if (!rt->nonce_valid || rt->session_hf != hf ||
        !ct_memeq(rt->session_peer, (const uint8_t *)hf + 6u, 6u)) {
        send_str(hf, "+XREC:ERR,NOCHAL\r\n");
        return;
    }

    /* One-time nonce: consume immediately to prevent replay/brute-force */
    rt->nonce_valid = 0;

    const uint8_t *p = args;
    const uint8_t *end = args + args_len;
    while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) {
        end--;
    }

    uint8_t got[32];
    if (!parse_hex(p, (uint32_t)(end - p), got, 32u)) {
        send_str(hf, "+XREC:ERR,AUTH\r\n");
        return;
    }

    uint8_t expected[32];
    hmac_sha256(rt->nv.key, rt->session_nonce, 16u, expected);
    if (!ct_memeq(got, expected, 32u)) {
        rt->session_authed = 0;
        send_str(hf, "+XREC:ERR,AUTH\r\n");
        return;
    }

    rt->session_authed = 1;
    rt->last_auth_ms = OAL_get_tick();
    send_str(hf, "+XREC:OK,AUTH\r\n");
}

__attribute__((used))
uint32_t bt_filesync_dispatch(void *hf, const uint8_t *data, uint16_t len)
{
    /* Fast path: skip leading CR/LF/spaces */
    const uint8_t *p = data;
    uint16_t rem = len;
    while (rem > 0 && (*p == '\r' || *p == '\n' || *p == ' ')) {
        p++;
        rem--;
    }

    if (!starts_with(p, rem, "+XREC:")) {
        return orig_hfp_parse_at_cmd(hf, data, len);
    }

    /* Reset idle auto-power-off and screen-off timers on every +XREC packet */
    fw_timer_reset(STR_IDLESHUT);
    fw_timer_reset(STR_SCREEN);

    xrec_rt_t *rt = xrec_rt();
    xrec_ensure_nv_loaded(rt);
    int pair_open = xrec_pair_window_open(rt);

    p += 6;
    rem -= 6;

    /* Unauthenticated discovery / pairing / auth commands */
    if (starts_with(p, rem, "PING")) {
        char resp[96];
        int authed = xrec_is_authed(rt, hf);
        fw_sprintf(resp, "+XREC:PONG,pen133_2.0.65,v5,bound=%d,auth=%d,pair=%d\r\n",
                   (int)rt->nv_bound, authed, pair_open);
        send_str(hf, resp);
        return 0;
    } else if (starts_with(p, rem, "CHAL")) {
        handle_chal(hf, rt);
        return 0;
    } else if (starts_with(p, rem, "PAIR,")) {
        handle_pair(hf, rt, p + 5, (uint16_t)(rem - 5));
        return 0;
    } else if (starts_with(p, rem, "AUTH,")) {
        handle_auth(hf, rt, p + 5, (uint16_t)(rem - 5));
        return 0;
    }

    /* All privileged commands require an authenticated session */
    if (!xrec_is_authed(rt, hf)) {
        send_str(hf, "+XREC:ERR,LOCKED\r\n");
        return 0;
    }

    if (starts_with(p, rem, "LS")) {
        handle_ls(hf);
    } else if (starts_with(p, rem, "GETS,")) {
        handle_get_stream(hf, p + 5, (uint16_t)(rem - 5), 1);
    } else if (starts_with(p, rem, "GET,")) {
        handle_get_stream(hf, p + 4, (uint16_t)(rem - 4), 0);
    } else if (starts_with(p, rem, "DEL,")) {
        handle_del(hf, p + 4, (uint16_t)(rem - 4));
    } else if (starts_with(p, rem, "RTC")) {
        handle_rtc(hf, p + 3, (uint16_t)(rem - 3));
    } else if (starts_with(p, rem, "RECSTART")) {
        handle_recstart(hf, p + 8, (uint16_t)(rem - 8));
    } else if (starts_with(p, rem, "RECSTOP")) {
        handle_recstop(hf);
    } else if (starts_with(p, rem, "STAT")) {
        handle_stat(hf);
    } else if (starts_with(p, rem, "PUT,")) {
        handle_put(hf, p + 4, (uint16_t)(rem - 4));
    } else if (starts_with(p, rem, "OTA")) {
        send_str(hf, "+XREC:OK,OTA\r\n");
        HAL_pmu_sd_upd_set(1);
        do_reboot_mode();
    } else if (starts_with(p, rem, "REBOOT")) {
        send_str(hf, "+XREC:OK,REBOOT\r\n");
        do_reboot_mode();
    } else {
        send_str(hf, "+XREC:ERR,CMD\r\n");
    }

    /* Return 0 -> _hfp_hf_slc_open_status jump table[0] exits cleanly to 0xD624E8 */
    return 0;
}

__attribute__((used))
int bt_autoboot_impl(const char *timer_name)
{
    /* Zero-initialize runtime state cleanly on every boot */
    xrec_rt_t *rt = (xrec_rt_t *)XREC_RT_SRAM_ADDR;
    rom_memset(rt, 0, sizeof(*rt));
    rt->init_magic = XREC_RT_MAGIC;

    int rc = fw_timer_reset(timer_name);
    bt_open_stack();
    return rc;
}

__attribute__((used))
int bt_pair_menu_impl(void)
{
    int ret = page_bt_menu_create();
    if (ret == 0) {
        xrec_rt_t *rt = xrec_rt();
        uint8_t rnd[4];
        xrec_rng_bytes(rt, rnd, 4u);
        uint32_t v = ((uint32_t)rnd[0] << 24) | ((uint32_t)rnd[1] << 16)
                   | ((uint32_t)rnd[2] << 8)  | ((uint32_t)rnd[3]);
        rt->pair_pin = v % 1000000u;
        rt->pair_start_ms = OAL_get_tick();
        rt->pair_fails = 0;
        rt->pair_active = 1;

        bt_set_discoverable(1u);

        void *scr = lv_disp_get_scr_act((void *)0);
        if (scr) {
            void *lbl = lv_label_create(scr);
            if (lbl) {
                lv_label_set_text_fmt(lbl, " PIN: %06u ", rt->pair_pin);
                lv_obj_set_style_bg_color(lbl, 0x0000u, 0u);  /* black bg */
                lv_obj_set_style_bg_opa(lbl, 0xFFu, 0u);      /* opaque */
                lv_obj_set_style_txt_clr(lbl, 0xFFE0u, 0u);   /* yellow text */
                lv_obj_align(lbl, 5u, 0, -4);                 /* LV_ALIGN_BOTTOM_MID */
            }
        }
    }
    return ret;
}

/*
 * Fixed-address entry trampolines:
 *   0x00D74000: bt_filesync_hook         (hooked from 0x00D625E2)
 *   0x00D74010: bt_autoboot_post_welcome (hooked from 0x00CFBD8A, strictly after "welcome" is dismissed)
 *   0x00D74020: bt_pair_menu_hook        (hooked from 0x00CFFA56, ViewTask page_bt_menu_create)
 */
__attribute__((section(".text.entry"), used, naked))
void bt_filesync_hook(void)
{
    __asm__ volatile ("b.w bt_filesync_dispatch");
}

__attribute__((section(".text.autoboot"), used, naked))
void bt_autoboot_post_welcome(void)
{
    __asm__ volatile ("b.w bt_autoboot_impl");
}

__attribute__((section(".text.pairmenu"), used, naked))
void bt_pair_menu_hook(void)
{
    __asm__ volatile ("b.w bt_pair_menu_impl");
}
