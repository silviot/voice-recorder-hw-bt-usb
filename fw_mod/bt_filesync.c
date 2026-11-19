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

    p += 6;
    rem -= 6;

    if (starts_with(p, rem, "PING")) {
        send_str(hf, "+XREC:PONG,pen133_2.0.65\r\n");
    } else if (starts_with(p, rem, "LS")) {
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

/*
 * Fixed-address entry trampolines:
 *   0x00D74000: bt_filesync_hook        (hooked from 0x00D625E2)
 *   0x00D74010: bt_autoboot_post_welcome (hooked from 0x00CFBD8A, strictly after "welcome" is dismissed)
 */
__attribute__((section(".text.entry"), used, naked))
void bt_filesync_hook(void)
{
    __asm__ volatile ("b.w bt_filesync_dispatch");
}

__attribute__((section(".text.autoboot"), used))
int bt_autoboot_post_welcome(const char *timer_name)
{
    int rc = fw_timer_reset(timer_name);
    bt_open_stack();
    return rc;
}
