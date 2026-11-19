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
#define f_close               FW_FN(0x00D4C750, int,      (void *fp))

/* Boot ROM libc & FIRM sprintf */
#define rom_memset            FW_FN(0x00000BAA, void *,   (void *dst, int val, size_t n))
#define rom_memcpy            FW_FN(0x00000B2A, void *,   (void *dst, const void *src, size_t n))
#define rom_strlen            FW_FN(0x00000AA2, size_t,   (const char *s))
#define fw_sprintf            FW_FN(0x00D73428, int,      (char *buf, const char *fmt, ...))

/* Struct sizes (generously over-allocated via oal_malloc for safety):
 *   DIR     <= 0x80 bytes (firmware stack uses 0x58)
 *   FIL     <= 0x300 bytes (contains 512-byte sector buf at +0x54)
 *   FILINFO <= 0x160 bytes (fsize u64 @ +0x00, fattrib u8 @ +0x0C, fname[256] @ +0x1A)
 */
#define DIR_ALLOC_SIZE     0x100u
#define FIL_ALLOC_SIZE     0x300u
#define FILINFO_ALLOC_SIZE 0x160u
#define MAX_CHUNK_LEN      256u
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

static void handle_get(void *hf, const uint8_t *args, uint16_t args_len)
{
    const uint8_t *p = args;
    const uint8_t *end = args + args_len;

    /* Format: <offset>,<len>,<filename>[\r\n] */
    uint32_t offset = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,ARG1\r\n");
        return;
    }
    p++;

    uint32_t req_len = parse_u32(p, end, &p);
    if (p >= end || *p != ',') {
        send_str(hf, "+XREC:ERR,ARG2\r\n");
        return;
    }
    p++;

    if (req_len == 0) {
        req_len = 1;
    }
    if (req_len > MAX_CHUNK_LEN) {
        req_len = MAX_CHUNK_LEN;
    }

    /* Extract filename up to CR/LF/NUL, rejecting path traversal */
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

    const char *dir_prefix = "0:\\record\\";
    uint32_t pi = 0;
    while (dir_prefix[pi] != '\0') {
        path[pi] = dir_prefix[pi];
        pi++;
    }
    uint32_t name_len = 0;
    while (p < end && *p != '\r' && *p != '\n' && *p != '\0' && name_len < 240u) {
        if (*p == '/' || *p == '\\') {
            name_len = 0;
            break;
        }
        path[pi++] = (char)*p++;
        name_len++;
    }
    path[pi] = '\0';

    if (name_len == 0) {
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

    uint32_t br = 0;
    rc = f_read(fp, pkt + 12u, req_len, &br);
    f_close(fp);

    if (rc != 0) {
        fw_sprintf((char *)pkt, "+XREC:ERR,READ,%d\r\n", rc);
        send_str(hf, (const char *)pkt);
        goto cleanup;
    }

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

cleanup:
    oal_free(pkt);
    oal_free(fp);
    oal_free(path);
}

/*
 * Entry point placed at exact VMA 0x00D74000 via section ".text.entry".
 */
__attribute__((section(".text.entry"), used))
uint32_t bt_filesync_hook(void *hf, const uint8_t *data, uint16_t len)
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

    p += 6;
    rem -= 6;

    if (starts_with(p, rem, "PING")) {
        send_str(hf, "+XREC:PONG,pen133_2.0.65\r\n");
    } else if (starts_with(p, rem, "LS")) {
        handle_ls(hf);
    } else if (starts_with(p, rem, "GET,")) {
        handle_get(hf, p + 4, (uint16_t)(rem - 4));
    } else {
        send_str(hf, "+XREC:ERR,CMD\r\n");
    }

    /* Return 0 -> _hfp_hf_slc_open_status jump table[0] exits cleanly to 0xD624E8 */
    return 0;
}
