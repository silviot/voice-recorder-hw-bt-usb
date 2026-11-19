/*
 * Host unit tests for hardware-independent open_fw core libraries
 * (crypto.c, wav.c, exfat.c, usb_scsi.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openfw_crypto.h"
#include "openfw_fs.h"
#include "openfw_usb_msc.h"
#include "openfw_wav.h"

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void test_crypto(void)
{
    static const uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    static const uint8_t nonce[16] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    };
    static const uint8_t expected_hmac[32] = {
        0xb1, 0x48, 0x97, 0x27, 0xb2, 0x60, 0x2a, 0x0e,
        0x04, 0x7c, 0xb2, 0x37, 0x58, 0x0a, 0x4c, 0x79,
        0x5a, 0xaf, 0x90, 0x23, 0xc5, 0xbc, 0x6b, 0x0b,
        0x86, 0x5c, 0x7f, 0xa1, 0x7e, 0x0a, 0x23, 0x67,
    };

    assert(openfw_crc16_ccitt_false((const uint8_t *)"123456789", 9u) == 0x29B1u);
    assert(openfw_crc16_ccitt_false((const uint8_t *)"", 0u) == 0xFFFFu);

    uint8_t mac[32];
    openfw_hmac_sha256(key, sizeof(key), nonce, sizeof(nonce), mac);
    assert(openfw_ct_memeq(mac, expected_hmac, 32u) == 1);

    uint8_t mac_bad[32];
    memcpy(mac_bad, mac, 32u);
    mac_bad[31] ^= 0x01u;
    assert(openfw_ct_memeq(mac, mac_bad, 32u) == 0);
    printf("  [PASS] crypto (CRC-16, SHA-256, HMAC-SHA256 vector, ct_memeq)\n");
}

static void test_wav(void)
{
    uint8_t hdr[44];
    wav_info_t info;

    assert(wav_build_header(hdr, 16000u, 1u, 16u, 32000u) == 0);
    assert(wav_parse_header(hdr, &info) == 0);
    assert(info.sample_rate == 16000u);
    assert(info.channels == 1u);
    assert(info.bits_per_sample == 16u);
    assert(info.block_align == 2u);
    assert(info.byte_rate == 32000u);
    assert(info.data_bytes == 32000u);

    assert(wav_build_header(hdr, 16000u, 2u, 16u, 64000u) == 0);
    assert(wav_parse_header(hdr, &info) == 0);
    assert(info.block_align == 4u);
    assert(info.byte_rate == 64000u);
    printf("  [PASS] wav (mono blockAlign=2 & stereo blockAlign=4 build/parse)\n");
}

#define MEM_DISK_SECTORS 128u
static uint8_t g_disk[MEM_DISK_SECTORS * 512u];

static int mem_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > MEM_DISK_SECTORS) {
        return -1;
    }
    memcpy(buf, &g_disk[lba * 512u], count * 512u);
    return 0;
}

static int mem_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    if (lba + count > MEM_DISK_SECTORS) {
        return -1;
    }
    memcpy(&g_disk[lba * 512u], buf, count * 512u);
    return 0;
}

static void write_exfat_entry_set(uint8_t *slot_base, const char *name,
                                  uint16_t attr, uint8_t no_fat_chain,
                                  uint32_t first_cluster, uint64_t size)
{
    size_t nlen = strlen(name);
    assert(nlen <= 15u);

    uint8_t *f = slot_base;
    f[0] = 0x85u;
    f[1] = 2u;
    put_le16(f + 4u, attr);

    uint8_t *s = slot_base + 32u;
    s[0] = 0xC0u;
    s[1] = (uint8_t)(0x01u | (no_fat_chain ? 0x02u : 0x00u));
    s[3] = (uint8_t)nlen;
    put_le32(s + 20u, first_cluster);
    put_le64(s + 8u, size);
    put_le64(s + 24u, size);

    uint8_t *fn = slot_base + 64u;
    fn[0] = 0xC1u;
    for (size_t i = 0u; i < nlen; i++) {
        put_le16(fn + 2u + i * 2u, (uint8_t)name[i]);
    }
}

typedef struct {
    int  count;
    char first_name[64];
} list_ctx_t;

static int test_list_cb(const exfat_entry_t *e, void *user)
{
    list_ctx_t *lc = (list_ctx_t *)user;
    if (lc->count == 0) {
        strncpy(lc->first_name, e->name, sizeof(lc->first_name) - 1u);
        lc->first_name[sizeof(lc->first_name) - 1u] = '\0';
    }
    lc->count++;
    return 0;
}

static void test_exfat(void)
{
    memset(g_disk, 0, sizeof(g_disk));

    g_disk[510] = 0x55u;
    g_disk[511] = 0xAAu;
    g_disk[0x1BEu + 4u] = 0x07u;
    put_le32(&g_disk[0x1BEu + 8u], 4u);
    put_le32(&g_disk[0x1BEu + 12u], MEM_DISK_SECTORS - 4u);

    uint8_t *vbr = &g_disk[4u * 512u];
    memcpy(&vbr[3], "EXFAT   ", 8u);
    put_le64(&vbr[0x40], 4u);
    put_le64(&vbr[0x48], MEM_DISK_SECTORS - 4u);
    put_le32(&vbr[0x50], 12u); /* FatOffset -> LBA 16 */
    put_le32(&vbr[0x54], 4u);  /* FatLength */
    put_le32(&vbr[0x58], 16u); /* ClusterHeapOffset -> LBA 20 */
    put_le32(&vbr[0x5C], 44u); /* ClusterCount */
    put_le32(&vbr[0x60], 2u);  /* RootDirCluster */
    vbr[0x6C] = 9u;            /* 512 B/sector */
    vbr[0x6D] = 0u;            /* 1 sector/cluster */
    vbr[510]  = 0x55u;
    vbr[511]  = 0xAAu;

    uint8_t *fat = &g_disk[16u * 512u];
    put_le32(fat + 0u * 4u, 0xFFFFFFF8u);
    put_le32(fat + 1u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 2u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 3u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 4u * 4u, 5u);
    put_le32(fat + 5u * 4u, 0xFFFFFFFFu);

    write_exfat_entry_set(&g_disk[20u * 512u], "record", EXFAT_ATTR_DIR, 1u, 3u, 512u);
    write_exfat_entry_set(&g_disk[21u * 512u], "test.wav", 0x0020u, 0u, 4u, 700u);

    uint8_t expected_wav[700];
    assert(wav_build_header(expected_wav, 16000u, 1u, 16u, 700u - 44u) == 0);
    for (uint32_t i = 44u; i < sizeof(expected_wav); i++) {
        expected_wav[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }
    memcpy(&g_disk[22u * 512u], expected_wav, sizeof(expected_wav));

    blkdev_t dev = { mem_read_sectors, NULL, MEM_DISK_SECTORS, NULL };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);

    exfat_entry_t dir_ent;
    assert(exfat_find_path(&vol, "/RECORD", &dir_ent) == 0);
    assert(dir_ent.is_dir == 1u);

    list_ctx_t lc = { 0, "" };
    assert(exfat_list_dir(&vol, dir_ent.first_cluster, dir_ent.no_fat_chain, test_list_cb, &lc) == 0);
    assert(lc.count == 1);
    assert(strcmp(lc.first_name, "test.wav") == 0);

    exfat_entry_t wav_ent;
    assert(exfat_find_path(&vol, "/record/test.wav", &wav_ent) == 0);
    assert(wav_ent.size == 700u);

    uint8_t read_buf[700];
    uint32_t nread = 0u;
    assert(exfat_read_file(&vol, &wav_ent, 0u, read_buf, sizeof(read_buf), &nread) == 0);
    assert(nread == 700u);
    assert(memcmp(read_buf, expected_wav, sizeof(expected_wav)) == 0);

    uint8_t slice_buf[200];
    assert(exfat_read_file(&vol, &wav_ent, 400u, slice_buf, sizeof(slice_buf), &nread) == 0);
    assert(nread == 200u);
    assert(memcmp(slice_buf, &expected_wav[400], sizeof(slice_buf)) == 0);

    wav_info_t winfo;
    assert(wav_parse_header(read_buf, &winfo) == 0);
    assert(winfo.block_align == 2u && winfo.data_bytes == 700u - 44u);
    printf("  [PASS] exfat (MBR + VBR mount, path lookup, FAT-chained cross-cluster read)\n");
}

static void test_exfat_write(void)
{
    memset(g_disk, 0, sizeof(g_disk));

    g_disk[510] = 0x55u;
    g_disk[511] = 0xAAu;
    g_disk[0x1BEu + 4u] = 0x07u;
    put_le32(&g_disk[0x1BEu + 8u], 4u);
    put_le32(&g_disk[0x1BEu + 12u], MEM_DISK_SECTORS - 4u);

    uint8_t *vbr = &g_disk[4u * 512u];
    memcpy(&vbr[3], "EXFAT   ", 8u);
    put_le64(&vbr[0x40], 4u);
    put_le64(&vbr[0x48], MEM_DISK_SECTORS - 4u);
    put_le32(&vbr[0x50], 12u); /* FatOffset -> LBA 16 */
    put_le32(&vbr[0x54], 4u);  /* FatLength */
    put_le32(&vbr[0x58], 16u); /* Heap -> LBA 20 */
    put_le32(&vbr[0x5C], 64u); /* 64 clusters x 512 B = 32 KiB */
    put_le32(&vbr[0x60], 2u);  /* Root dir at cluster 2 */
    vbr[0x6C] = 9u;
    vbr[0x6D] = 0u;            /* 512 B clusters to stress multi-cluster FAT & sector wrap */
    vbr[510]  = 0x55u;
    vbr[511]  = 0xAAu;

    uint8_t *fat = &g_disk[16u * 512u];
    put_le32(fat + 0u * 4u, 0xFFFFFFF8u);
    put_le32(fat + 1u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 2u * 4u, 0xFFFFFFFFu); /* root */
    put_le32(fat + 3u * 4u, 0xFFFFFFFFu); /* bitmap */

    /* Root Dir @ cluster 2 (LBA 20): Allocation Bitmap (0x81) at cluster 3 */
    uint8_t *bmp_ent = &g_disk[20u * 512u];
    bmp_ent[0] = 0x81u;
    bmp_ent[1] = 0x00u;
    put_le32(bmp_ent + 20u, 3u);
    put_le64(bmp_ent + 24u, 8u);

    /* Cluster 3 (LBA 21): Allocation Bitmap marking clusters 2 & 3 used (bits 0..1) */
    g_disk[21u * 512u] = 0x03u;

    blkdev_t dev = { mem_read_sectors, mem_write_sectors, MEM_DISK_SECTORS, NULL };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    assert(vol.bitmap_cluster == 3u && vol.bitmap_bytes == 8u);

    assert(exfat_mkdir(&vol, "record") == 0);
    assert(exfat_mkdir(&vol, "record") == 0); /* Idempotent when already present */

    exfat_set_timestamp(&vol, 0x5D738420u); /* 2026-11-19 16:33:00 */
    exfat_writer_t w;
    assert(exfat_create_file(&vol, "/record", "REC_0001.wav", &w) == 0);

    uint8_t hdr[44];
    assert(wav_build_header(hdr, 16000u, 1u, 16u, 0u) == 0);
    assert(exfat_write(&w, hdr, 44u) == 0);

    uint8_t pcm[4096];
    for (uint32_t i = 0u; i < sizeof(pcm); i++) {
        pcm[i] = (uint8_t)((i * 73u + 19u) & 0xFFu);
    }
    assert(exfat_write(&w, pcm, sizeof(pcm)) == 0);

    assert(wav_build_header(hdr, 16000u, 1u, 16u, sizeof(pcm)) == 0);
    assert(exfat_patch_header(&w, hdr, 44u) == 0);
    assert(exfat_close_file(&w) == 0);

    /* Also create 6 more files to force 0x05 sector-boundary wrapping and cluster expansion */
    for (int fidx = 2; fidx <= 7; fidx++) {
        char fname[16];
        snprintf(fname, sizeof(fname), "REC_%04d.wav", fidx);
        assert(exfat_create_file(&vol, "/record", fname, &w) == 0);
        uint8_t small_msg[12];
        snprintf((char *)small_msg, sizeof(small_msg), "FILE_%04d!!", fidx);
        assert(exfat_write(&w, small_msg, 10u) == 0);
        assert(exfat_close_file(&w) == 0);
    }

    exfat_vol_t vol2;
    assert(exfat_mount(&vol2, &dev) == 0);

    exfat_entry_t wav_ent;
    assert(exfat_find_path(&vol2, "/record/REC_0001.wav", &wav_ent) == 0);
    assert(wav_ent.size == 44u + sizeof(pcm));

    uint8_t read_back[44 + 4096];
    uint32_t nread = 0u;
    assert(exfat_read_file(&vol2, &wav_ent, 0u, read_back, sizeof(read_back), &nread) == 0);
    assert(nread == sizeof(read_back));

    wav_info_t winfo;
    assert(wav_parse_header(read_back, &winfo) == 0);
    assert(winfo.sample_rate == 16000u && winfo.data_bytes == sizeof(pcm));
    assert(memcmp(read_back + 44u, pcm, sizeof(pcm)) == 0);

    exfat_entry_t seventh;
    assert(exfat_find_path(&vol2, "/record/REC_0007.wav", &seventh) == 0);
    uint8_t buf7[16];
    assert(exfat_read_file(&vol2, &seventh, 0u, buf7, 10u, &nread) == 0);
    assert(nread == 10u && memcmp(buf7, "FILE_0007!", 10u) == 0);

    exfat_entry_t rec_dir;
    assert(exfat_find_path(&vol2, "/record", &rec_dir) == 0);
    uint8_t dir_sec[512];
    assert(mem_read_sectors(NULL, vol2.heap_lba + (rec_dir.first_cluster - 2u), 1u, dir_sec) == 0);
    assert(dir_sec[0] == 0x85u);
    assert(get_le32(&dir_sec[0x08]) == 0x5D738420u);
    assert(get_le32(&dir_sec[0x0C]) == 0x5D738420u);

    printf("  [PASS] exfat write (mkdir, multi-cluster write, header patch, timestamp, sector wrap)\n");
}

static void build_cbw(uint8_t cbw[31], uint32_t tag, uint32_t data_len,
                      uint8_t flags, const uint8_t *cdb, uint8_t cdb_len)
{
    memset(cbw, 0, 31u);
    put_le32(&cbw[0], 0x43425355u);
    put_le32(&cbw[4], tag);
    put_le32(&cbw[8], data_len);
    cbw[12] = flags;
    cbw[13] = 0u;
    cbw[14] = cdb_len;
    memcpy(&cbw[15], cdb, cdb_len);
}

static void scsi_xfer(usb_msc_state_t *st, uint32_t tag, uint8_t flags,
                      const uint8_t *cdb, uint8_t cdb_len,
                      const uint8_t *out_data, uint8_t *in_data, uint32_t data_len,
                      uint8_t *out_status, uint32_t *out_residue, uint32_t *out_in_bytes)
{
    uint8_t cbw[31];
    build_cbw(cbw, tag, data_len, flags, cdb, cdb_len);
    usb_msc_on_rx_packet(st, cbw, 31u);

    if ((flags & 0x80u) == 0u && data_len > 0u) {
        uint32_t sent = 0u;
        while (sent < data_len) {
            uint8_t chunk = ((data_len - sent) > 64u) ? 64u : (uint8_t)(data_len - sent);
            usb_msc_on_rx_packet(st, out_data + sent, chunk);
            sent += chunk;
        }
    }

    uint32_t got = 0u;
    uint8_t csw[64];
    int got_csw = 0;
    while (usb_msc_need_tx(st)) {
        uint8_t pkt[64];
        uint8_t n = usb_msc_get_tx_packet(st, pkt);
        if (!usb_msc_need_tx(st)) {
            /* Last packet emitted was CSW */
            assert(n == 13u);
            memcpy(csw, pkt, 13u);
            got_csw = 1;
            break;
        }
        if (in_data && n > 0u && got + n <= data_len) {
            memcpy(in_data + got, pkt, n);
        }
        got += n;
    }

    assert(got_csw == 1);
    assert(get_le32(&csw[0]) == 0x53425355u);
    assert(get_le32(&csw[4]) == tag);
    *out_residue = get_le32(&csw[8]);
    *out_status  = csw[12];
    if (out_in_bytes) {
        *out_in_bytes = got;
    }
}

static void test_usb_msc(void)
{
    memset(g_disk, 0, sizeof(g_disk));
    blkdev_t dev = { mem_read_sectors, mem_write_sectors, MEM_DISK_SECTORS, NULL };
    usb_msc_state_t st;
    usb_msc_init_state(&st, &dev);

    uint8_t  status = 0u;
    uint32_t res = 0u, got = 0u;
    uint8_t  buf[1024];

    /* 1. INQUIRY */
    uint8_t inq_cdb[6] = { 0x12u, 0u, 0u, 0u, 36u, 0u };
    scsi_xfer(&st, 1u, 0x80u, inq_cdb, 6u, NULL, buf, 36u, &status, &res, &got);
    assert(status == 0u && res == 0u && got == 36u);
    assert(memcmp(&buf[8], "Cassini OpenFW Recorder 1.0 ", 28u) == 0);

    /* 2. TEST UNIT READY */
    uint8_t tur_cdb[6] = { 0x00u, 0u, 0u, 0u, 0u, 0u };
    scsi_xfer(&st, 2u, 0x00u, tur_cdb, 6u, NULL, NULL, 0u, &status, &res, NULL);
    assert(status == 0u && res == 0u);

    /* 3. READ CAPACITY (10) */
    uint8_t rcap_cdb[10] = { 0x25u, 0u };
    scsi_xfer(&st, 3u, 0x80u, rcap_cdb, 10u, NULL, buf, 8u, &status, &res, &got);
    assert(status == 0u && got == 8u);
    assert(get_be32(&buf[0]) == MEM_DISK_SECTORS - 1u);
    assert(get_be32(&buf[4]) == 512u);

    /* 4. READ FORMAT CAPACITIES */
    uint8_t rfc_cdb[10] = { 0x23u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 12u, 0u };
    scsi_xfer(&st, 4u, 0x80u, rfc_cdb, 10u, NULL, buf, 12u, &status, &res, &got);
    assert(status == 0u && got == 12u);
    assert(get_be32(&buf[4]) == MEM_DISK_SECTORS);

    /* 5. MODE SENSE (6) */
    uint8_t ms_cdb[6] = { 0x1Au, 0u, 0x3Fu, 0u, 4u, 0u };
    scsi_xfer(&st, 5u, 0x80u, ms_cdb, 6u, NULL, buf, 4u, &status, &res, &got);
    assert(status == 0u && got == 4u && buf[2] == 0x00u);

    /* 6. WRITE (10) 2 sectors at LBA 10 */
    uint8_t wpat[1024];
    for (uint32_t i = 0u; i < sizeof(wpat); i++) {
        wpat[i] = (uint8_t)((i * 53u + 7u) & 0xFFu);
    }
    uint8_t wr_cdb[10] = { 0x2Au, 0u };
    put_be32(&wr_cdb[2], 10u);
    put_be16(&wr_cdb[7], 2u);
    scsi_xfer(&st, 6u, 0x00u, wr_cdb, 10u, wpat, NULL, sizeof(wpat), &status, &res, NULL);
    assert(status == 0u && res == 0u);
    assert(memcmp(&g_disk[10u * 512u], wpat, sizeof(wpat)) == 0);

    /* 7. READ (10) 2 sectors at LBA 10 */
    uint8_t rd_cdb[10] = { 0x28u, 0u };
    put_be32(&rd_cdb[2], 10u);
    put_be16(&rd_cdb[7], 2u);
    scsi_xfer(&st, 7u, 0x80u, rd_cdb, 10u, NULL, buf, sizeof(wpat), &status, &res, &got);
    assert(status == 0u && res == 0u && got == sizeof(wpat));
    assert(memcmp(buf, wpat, sizeof(wpat)) == 0);

    /* 8. Unsupported command -> ZLP + CSW status 1 + REQUEST SENSE 0x05/0x20 */
    uint8_t bad_cdb[6] = { 0xFFu, 0u };
    scsi_xfer(&st, 8u, 0x80u, bad_cdb, 6u, NULL, buf, 8u, &status, &res, &got);
    assert(status == 1u && res == 8u && got == 0u);

    uint8_t req_cdb[6] = { 0x03u, 0u, 0u, 0u, 18u, 0u };
    scsi_xfer(&st, 9u, 0x80u, req_cdb, 6u, NULL, buf, 18u, &status, &res, &got);
    assert(status == 0u && got == 18u && buf[2] == 0x05u && buf[12] == 0x20u);

    printf("  [PASS] usb_msc (SCSI INQUIRY, TUR, READCAP, WRITE10, READ10, SENSE)\n");
}

static int file_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    FILE *fp = (FILE *)ctx;
    if (fseek(fp, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    return (fread(buf, 512u, count, fp) == count) ? 0 : -1;
}

static int file_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    FILE *fp = (FILE *)ctx;
    if (fseek(fp, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buf, 512u, count, fp) != count) {
        return -1;
    }
    return fflush(fp) == 0 ? 0 : -1;
}

static void run_exfat_img_test(const char *path)
{
    FILE *fp = fopen(path, "r+b");
    assert(fp != NULL);
    assert(fseek(fp, 0L, SEEK_END) == 0);
    long sz = ftell(fp);
    assert(sz >= 512L);

    blkdev_t dev = { file_read_sectors, file_write_sectors, (uint32_t)(sz / 512L), fp };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    assert(vol.bitmap_cluster >= 2u);

    assert(exfat_mkdir(&vol, "record") == 0);
    exfat_set_timestamp(&vol, 0x5D738420u); /* 2026-11-19 16:33:00 */

    exfat_writer_t w;
    assert(exfat_create_file(&vol, "/record", "REC_0001.wav", &w) == 0);

    uint8_t hdr[44];
    assert(wav_build_header(hdr, 16000u, 1u, 16u, 0u) == 0);
    assert(exfat_write(&w, hdr, 44u) == 0);

    static uint8_t pcm[16384];
    for (uint32_t i = 0u; i < sizeof(pcm); i++) {
        pcm[i] = (uint8_t)((i * 29u + 5u) & 0xFFu);
    }
    assert(exfat_write(&w, pcm, sizeof(pcm)) == 0);

    assert(wav_build_header(hdr, 16000u, 1u, 16u, sizeof(pcm)) == 0);
    assert(exfat_patch_header(&w, hdr, 44u) == 0);
    assert(exfat_close_file(&w) == 0);

    exfat_vol_t vol2;
    assert(exfat_mount(&vol2, &dev) == 0);

    exfat_entry_t wav_ent;
    assert(exfat_find_path(&vol2, "/record/REC_0001.wav", &wav_ent) == 0);
    assert(wav_ent.size == 44u + sizeof(pcm));

    static uint8_t verify_buf[44u + 16384u];
    uint32_t nread = 0u;
    assert(exfat_read_file(&vol2, &wav_ent, 0u, verify_buf, sizeof(verify_buf), &nread) == 0);
    assert(nread == sizeof(verify_buf));

    wav_info_t winfo;
    assert(wav_parse_header(verify_buf, &winfo) == 0);
    assert(winfo.sample_rate == 16000u && winfo.data_bytes == sizeof(pcm));
    assert(memcmp(verify_buf + 44u, pcm, sizeof(pcm)) == 0);

    fclose(fp);
    printf("  [PASS] exfat-img (%s: 16,428 B multi-cluster WAV written & verified)\n", path);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--exfat-img") == 0) {
        run_exfat_img_test(argv[2]);
        return 0;
    }

    printf("Running open_fw host unit tests...\n");
    test_crypto();
    test_wav();
    test_exfat();
    test_exfat_write();
    test_usb_msc();
    printf("All open_fw host unit tests PASSED!\n");
    return 0;
}
