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

#include "openfw_bt_spp.h"
#include "openfw_crypto.h"
#include "openfw_cwx.h"
#include "openfw_fs.h"
#include "openfw_ui.h"
#include "openfw_usb_msc.h"
#include "openfw_wav.h"
#include "openfw_xrec.h"

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

static void test_wav_stream_and_ima_adpcm(void)
{
    /* 1. PCM 16 kHz mono with an extra LIST chunk before data */
    uint8_t pcm_wav[64];
    memset(pcm_wav, 0, sizeof(pcm_wav));
    memcpy(&pcm_wav[0], "RIFF", 4u);
    put_le32(&pcm_wav[4], 56u);
    memcpy(&pcm_wav[8], "WAVEfmt ", 8u);
    put_le32(&pcm_wav[16], 16u);
    put_le16(&pcm_wav[20], WAV_FMT_PCM);
    put_le16(&pcm_wav[22], 1u);
    put_le32(&pcm_wav[24], 16000u);
    put_le32(&pcm_wav[28], 32000u);
    put_le16(&pcm_wav[32], 2u);
    put_le16(&pcm_wav[34], 16u);
    memcpy(&pcm_wav[36], "LIST", 4u);
    put_le32(&pcm_wav[40], 4u);
    memcpy(&pcm_wav[48], "data", 4u);
    put_le32(&pcm_wav[52], 32000u);

    wav_stream_info_t ws;
    assert(wav_parse_stream(pcm_wav, sizeof(pcm_wav), &ws) == 0);
    assert(ws.format_tag == WAV_FMT_PCM && ws.channels == 1u);
    assert(ws.sample_rate == 16000u && ws.data_offset == 56u && ws.data_bytes == 32000u);
    assert(ws.total_samples == 16000u && ws.duration_ms == 1000u);

    /* 2. Mono Microsoft IMA-ADPCM (16 kHz, block_align=256, 505 spb, fact chunk) */
    uint8_t ima_hdr[72];
    memset(ima_hdr, 0, sizeof(ima_hdr));
    memcpy(&ima_hdr[0], "RIFF", 4u);
    put_le32(&ima_hdr[4], 512u);
    memcpy(&ima_hdr[8], "WAVEfmt ", 8u);
    put_le32(&ima_hdr[16], 20u);
    put_le16(&ima_hdr[20], WAV_FMT_IMA_ADPCM);
    put_le16(&ima_hdr[22], 1u);
    put_le32(&ima_hdr[24], 16000u);
    put_le32(&ima_hdr[28], 8110u);
    put_le16(&ima_hdr[32], 256u);
    put_le16(&ima_hdr[34], 4u);
    put_le16(&ima_hdr[36], 2u);
    put_le16(&ima_hdr[38], 505u);
    memcpy(&ima_hdr[40], "fact", 4u);
    put_le32(&ima_hdr[44], 4u);
    put_le32(&ima_hdr[48], 32000u); /* 2000 ms */
    memcpy(&ima_hdr[52], "data", 4u);
    put_le32(&ima_hdr[56], 256u * 63u);

    assert(wav_parse_stream(ima_hdr, sizeof(ima_hdr), &ws) == 0);
    assert(ws.format_tag == WAV_FMT_IMA_ADPCM && ws.channels == 1u);
    assert(ws.block_align == 256u && ws.samples_per_block == 505u);
    assert(ws.data_offset == 60u && ws.duration_ms == 2000u);

    uint8_t mono_blk[256];
    memset(mono_blk, 0x22u, sizeof(mono_blk));
    put_le16(&mono_blk[0], (uint16_t)1000);
    mono_blk[2] = 20u;
    mono_blk[3] = 0u;
    int16_t mono_pcm[512];
    int nf = wav_decode_ima_adpcm_block(mono_blk, 256u, 1u, mono_pcm, 512u);
    assert(nf == 505 && mono_pcm[0] == 1000 && mono_pcm[1] == 1031 && mono_pcm[2] > mono_pcm[1]);

    /* 3. Stereo Microsoft IMA-ADPCM (block_align=512, 505 stereo frames) */
    put_le16(&ima_hdr[22], 2u);
    put_le16(&ima_hdr[32], 512u);
    assert(wav_parse_stream(ima_hdr, sizeof(ima_hdr), &ws) == 0);
    assert(ws.channels == 2u && ws.block_align == 512u && ws.samples_per_block == 505u);

    uint8_t st_blk[512];
    memset(st_blk, 0x11u, sizeof(st_blk));
    put_le16(&st_blk[0], (uint16_t)500);
    st_blk[2] = 15u;
    st_blk[3] = 0u;
    put_le16(&st_blk[4], (uint16_t)(uint16_t)-400);
    st_blk[6] = 15u;
    st_blk[7] = 0u;
    int16_t st_pcm[1024];
    nf = wav_decode_ima_adpcm_block(st_blk, 512u, 2u, st_pcm, 512u);
    assert(nf == 505 && st_pcm[0] == 500 && st_pcm[1] == -400);
    assert(st_pcm[2] > 500 && st_pcm[3] != st_pcm[2]);

    printf("  [PASS] wav_stream + IMA-ADPCM (PCM/LIST, mono & stereo block decode)\n");
}

typedef struct {
    const uint8_t *buf;
    uint32_t       len;
} mem_cwx_src_t;

static int mem_cwx_read(void *user, uint32_t offset, void *dst, uint32_t len, uint32_t *out_read)
{
    const mem_cwx_src_t *src = (const mem_cwx_src_t *)user;
    if (offset >= src->len) {
        *out_read = 0u;
        return 0;
    }
    uint32_t avail = src->len - offset;
    uint32_t take  = (len < avail) ? len : avail;
    memcpy(dst, src->buf + offset, take);
    *out_read = take;
    return 0;
}

static void append_cwx_word(uint8_t *buf, uint32_t *pos, uint32_t start_ms, uint32_t end_ms,
                            uint8_t spk, uint8_t flags, const char *text)
{
    uint32_t tlen = (uint32_t)strlen(text);
    put_le32(&buf[*pos + 0u], start_ms);
    put_le32(&buf[*pos + 4u], end_ms);
    buf[*pos + 8u]  = spk;
    buf[*pos + 9u]  = flags;
    buf[*pos + 10u] = (uint8_t)tlen;
    memcpy(&buf[*pos + 11u], text, tlen);
    *pos += 11u + tlen;
}

static void test_cwx_engine(void)
{
    uint8_t cwx_img[512];
    memset(cwx_img, 0, sizeof(cwx_img));

    uint32_t spk_off   = CWX_HEADER_SIZE;
    uint32_t seek_off  = spk_off + 2u * CWX_SPEAKER_SIZE;
    uint32_t words_off = seek_off + 3u * CWX_SEEK_SIZE;

    memcpy(&cwx_img[0], "CWX1", 4u);
    put_le16(&cwx_img[4], 1u);
    put_le16(&cwx_img[6], 2u);
    put_le32(&cwx_img[8], 2500u);
    put_le32(&cwx_img[12], 6u);
    put_le32(&cwx_img[16], spk_off);
    put_le32(&cwx_img[20], seek_off);
    put_le32(&cwx_img[24], 3u);
    put_le32(&cwx_img[28], words_off);
    memcpy(&cwx_img[32], "Cassini Test", 12u);

    put_le16(&cwx_img[spk_off + 0u], 0x07FFu);
    memcpy(&cwx_img[spk_off + 4u], "Alice", 5u);
    put_le16(&cwx_img[spk_off + CWX_SPEAKER_SIZE + 0u], 0xFFE0u);
    memcpy(&cwx_img[spk_off + CWX_SPEAKER_SIZE + 4u], "Bob", 3u);

    uint32_t wpos = words_off;
    uint32_t rel0 = wpos - words_off;
    append_cwx_word(cwx_img, &wpos, 100u, 400u, 0u, CWX_FLAG_TURN_START, "Hello");
    append_cwx_word(cwx_img, &wpos, 420u, 800u, 0u, 0u, "world");
    append_cwx_word(cwx_img, &wpos, 850u, 1150u, 0u, 0u, "here");
    uint32_t rel3 = wpos - words_off;
    append_cwx_word(cwx_img, &wpos, 1200u, 1500u, 1u, CWX_FLAG_TURN_START, "Yes");
    append_cwx_word(cwx_img, &wpos, 1550u, 1950u, 1u, 0u, "indeed");
    uint32_t rel5 = wpos - words_off;
    append_cwx_word(cwx_img, &wpos, 2000u, 2400u, 1u, 0u, "done!");

    put_le32(&cwx_img[seek_off + 0u], rel0); put_le32(&cwx_img[seek_off + 4u], 0u);
    put_le32(&cwx_img[seek_off + 8u], rel3); put_le32(&cwx_img[seek_off + 12u], 3u);
    put_le32(&cwx_img[seek_off + 16u], rel5); put_le32(&cwx_img[seek_off + 20u], 5u);

    mem_cwx_src_t src = { cwx_img, wpos };
    cwx_stream_t cwx;
    assert(cwx_open(&cwx, mem_cwx_read, &src, wpos) == 0);
    assert(cwx.speaker_count == 2u && cwx.word_count == 6u && cwx.duration_ms == 2500u);
    assert(strcmp(cwx.title, "Cassini Test") == 0);
    assert(strcmp(cwx_speaker_label(&cwx, 0u), "Alice") == 0);
    assert(strcmp(cwx_speaker_label(&cwx, 1u), "Bob") == 0);
    assert(cwx_speaker_color(&cwx, 0u) == 0x07FFu && cwx_speaker_color(&cwx, 1u) == 0xFFE0u);

    assert(cwx_seek_ms(&cwx, 1200u) == 0);
    cwx_word_t wd;
    assert(cwx_next_word(&cwx, &wd) == 1);
    assert(wd.word_idx == 3u && wd.speaker_idx == 1u &&
           (wd.flags & CWX_FLAG_TURN_START) != 0u && strcmp(wd.text, "Yes") == 0);

    cwx_word_t win[4];
    int active = -1;
    int n = cwx_read_window(&cwx, 1300u, win, 4u, &active);
    assert(n > 0 && active >= 0 && active < n && strcmp(win[active].text, "Yes") == 0);

    n = cwx_read_window(&cwx, 2100u, win, 4u, &active);
    assert(n > 0 && active >= 0 && active < n && strcmp(win[active].text, "done!") == 0);
    assert(win[active].speaker_idx == 1u);

    printf("  [PASS] cwx_engine (header, speakers, seek table, next_word, read_window)\n");
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

    uint32_t free_mb = 99u, total_mb = 99u;
    assert(exfat_stat_fs(&vol, &free_mb, &total_mb) == 0);
    assert(free_mb <= total_mb);

    /* Delete safety: reject directories/missing files, delete multi-cluster file & reuse slot/clusters */
    assert(exfat_delete_file(&vol, "/", "record") != 0);
    assert(exfat_delete_file(&vol, "/record", "NOPE.wav") != 0);
    assert(exfat_create_file(&vol, "/record", "REC_TEMP.wav", &w) == 0);
    assert(exfat_write(&w, pcm, 1024u) == 0);
    assert(exfat_close_file(&w) == 0);
    exfat_entry_t tmp_ent;
    assert(exfat_find_path(&vol, "/record/REC_TEMP.wav", &tmp_ent) == 0);
    assert(exfat_delete_file(&vol, "/record", "REC_TEMP.wav") == 0);
    assert(exfat_find_path(&vol, "/record/REC_TEMP.wav", &tmp_ent) != 0);
    assert(exfat_create_file(&vol, "/record", "REC_REUSE.wav", &w) == 0);
    assert(exfat_write(&w, "OK", 2u) == 0);
    assert(exfat_close_file(&w) == 0);

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

typedef struct {
    int      calls;
    int      fail_next;
    int      last_is_out;
    uint16_t resp_len;
    uint16_t last_io_len;
    uint8_t  last_cdb[16];
    uint8_t  last_out[64];
} test_vendor_ctx_t;

static int test_vendor_cb(void *user, const uint8_t cdb[16],
                          uint8_t *io_buf, uint16_t *io_len, int is_out)
{
    test_vendor_ctx_t *ctx = (test_vendor_ctx_t *)user;
    ctx->calls++;
    ctx->last_is_out = is_out;
    ctx->last_io_len = *io_len;
    memcpy(ctx->last_cdb, cdb, 16u);
    if (ctx->fail_next) {
        ctx->fail_next = 0;
        return -1;
    }
    if (is_out) {
        if (*io_len > 0u) {
            memcpy(ctx->last_out, io_buf, *io_len);
        }
        return 0;
    }
    uint16_t n = (ctx->resp_len < *io_len) ? ctx->resp_len : *io_len;
    for (uint16_t i = 0u; i < n; i++) {
        io_buf[i] = (uint8_t)((cdb[1] * 17u + i) & 0xFFu);
    }
    *io_len = n;
    return 0;
}

static void test_usb_msc_vendor_e0(void)
{
    memset(g_disk, 0, sizeof(g_disk));
    blkdev_t dev = { mem_read_sectors, mem_write_sectors, MEM_DISK_SECTORS, NULL };
    usb_msc_state_t st;
    usb_msc_init_state(&st, &dev);

    uint8_t  status = 0u;
    uint32_t res = 0u, got = 0u;
    uint8_t  buf[128];
    uint8_t  req_cdb[6] = { 0x03u, 0u, 0u, 0u, 18u, 0u };

    /* 1. Unregistered 0xE0 -> Illegal Request 0x05/0x20/0x00 */
    uint8_t v_in_cdb[16] = { 0xE0u, 0x01u, 0x11u, 0x22u };
    scsi_xfer(&st, 100u, 0x80u, v_in_cdb, 16u, NULL, buf, 8u, &status, &res, &got);
    assert(status == 1u && res == 8u && got == 0u);
    scsi_xfer(&st, 101u, 0x80u, req_cdb, 6u, NULL, buf, 18u, &status, &res, &got);
    assert(status == 0u && buf[2] == 0x05u && buf[12] == 0x20u && buf[13] == 0x00u);

    /* 2. Register callback -> IN exact (32 B) & short (20/64 B) & zero-len */
    test_vendor_ctx_t vctx;
    memset(&vctx, 0, sizeof(vctx));
    usb_msc_set_vendor_cb(&st, test_vendor_cb, &vctx);

    vctx.resp_len = 32u;
    scsi_xfer(&st, 102u, 0x80u, v_in_cdb, 16u, NULL, buf, 32u, &status, &res, &got);
    assert(status == 0u && res == 0u && got == 32u && vctx.calls == 1 && vctx.last_is_out == 0);
    for (uint32_t i = 0u; i < 32u; i++) {
        assert(buf[i] == (uint8_t)((0x01u * 17u + i) & 0xFFu));
    }

    vctx.resp_len = 20u;
    scsi_xfer(&st, 103u, 0x80u, v_in_cdb, 16u, NULL, buf, 64u, &status, &res, &got);
    assert(status == 0u && res == 44u && got == 20u);

    uint8_t v_zero_cdb[16] = { 0xE0u, 0x05u };
    scsi_xfer(&st, 104u, 0x00u, v_zero_cdb, 16u, NULL, NULL, 0u, &status, &res, NULL);
    assert(status == 0u && res == 0u && vctx.last_is_out == 0 && vctx.last_io_len == 0u);

    /* 3. OUT single-packet (8 B) and multi-chunk (24 + 24 = 48 B) */
    uint8_t v_out_cdb[16] = { 0xE0u, 0x04u, 0xAAu, 0xBBu };
    uint8_t out8[8] = { 0xEAu, 0x07u, 11u, 15u, 10u, 20u, 30u, 4u };
    scsi_xfer(&st, 105u, 0x00u, v_out_cdb, 16u, out8, NULL, 8u, &status, &res, NULL);
    assert(status == 0u && res == 0u && vctx.last_is_out == 1 && vctx.last_io_len == 8u);
    assert(memcmp(vctx.last_out, out8, 8u) == 0);
    assert(memcmp(vctx.last_cdb, v_out_cdb, 16u) == 0);

    uint8_t out48[48];
    for (uint32_t i = 0u; i < sizeof(out48); i++) {
        out48[i] = (uint8_t)(0xC0u ^ i);
    }
    uint8_t cbw[31];
    build_cbw(cbw, 106u, 48u, 0x00u, v_out_cdb, 16u);
    usb_msc_on_rx_packet(&st, cbw, 31u);
    int calls_before = vctx.calls;
    usb_msc_on_rx_packet(&st, out48, 24u);
    assert(vctx.calls == calls_before);
    usb_msc_on_rx_packet(&st, out48 + 24u, 24u);
    assert(vctx.calls == calls_before + 1 && vctx.last_io_len == 48u);
    assert(memcmp(vctx.last_out, out48, 48u) == 0);
    uint8_t csw_pkt[64];
    assert(usb_msc_need_tx(&st) == 1);
    assert(usb_msc_get_tx_packet(&st, csw_pkt) == 13u && csw_pkt[12] == 0u && get_le32(&csw_pkt[8]) == 0u);
    assert(usb_msc_need_tx(&st) == 0);

    /* 4. Failure sense 0x05/0x24/0x00 on IN, OUT callback fail, and OUT > 64 B */
    vctx.fail_next = 1;
    scsi_xfer(&st, 107u, 0x80u, v_in_cdb, 16u, NULL, buf, 32u, &status, &res, &got);
    assert(status == 1u && res == 32u && got == 0u);
    scsi_xfer(&st, 108u, 0x80u, req_cdb, 6u, NULL, buf, 18u, &status, &res, &got);
    assert(status == 0u && buf[2] == 0x05u && buf[12] == 0x24u && buf[13] == 0x00u);

    vctx.fail_next = 1;
    scsi_xfer(&st, 109u, 0x00u, v_out_cdb, 16u, out8, NULL, 8u, &status, &res, NULL);
    assert(status == 1u && res == 0u);
    scsi_xfer(&st, 110u, 0x80u, req_cdb, 6u, NULL, buf, 18u, &status, &res, &got);
    assert(status == 0u && buf[2] == 0x05u && buf[12] == 0x24u && buf[13] == 0x00u);

    uint8_t out65[65];
    memset(out65, 0x5Au, sizeof(out65));
    calls_before = vctx.calls;
    scsi_xfer(&st, 111u, 0x00u, v_out_cdb, 16u, out65, NULL, 65u, &status, &res, NULL);
    assert(vctx.calls == calls_before && status == 1u && res == 65u);
    scsi_xfer(&st, 112u, 0x80u, req_cdb, 6u, NULL, buf, 18u, &status, &res, &got);
    assert(status == 0u && buf[2] == 0x05u && buf[12] == 0x24u && buf[13] == 0x00u);

    /* 5. Callback & user pointer preserved across usb_msc_reset() */
    usb_msc_reset(&st);
    assert(st.vendor_cb == test_vendor_cb && st.vendor_user == &vctx);
    vctx.resp_len = 16u;
    v_in_cdb[1] = 0x02u;
    scsi_xfer(&st, 113u, 0x80u, v_in_cdb, 16u, NULL, buf, 16u, &status, &res, &got);
    assert(status == 0u && res == 0u && got == 16u);
    for (uint32_t i = 0u; i < 16u; i++) {
        assert(buf[i] == (uint8_t)((0x02u * 17u + i) & 0xFFu));
    }
    scsi_xfer(&st, 114u, 0x00u, v_out_cdb, 16u, out8, NULL, 8u, &status, &res, NULL);
    assert(status == 0u && res == 0u && memcmp(vctx.last_out, out8, 8u) == 0);

    printf("  [PASS] usb_msc vendor 0xE0 (IN, OUT, multi-pkt, fail sense, reset preserve)\n");
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

    uint32_t free_before = 0u, free_mid = 0u, free_after = 0u, total_mb = 0u;
    assert(exfat_stat_fs(&vol, &free_before, &total_mb) == 0);
    assert(total_mb >= 1u);

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

    /* Create & delete a 2-cluster temp file so fsck.exfat verifies deleted slots + freed bitmap/FAT */
    exfat_writer_t wt;
    assert(exfat_create_file(&vol, "/record", "REC_TEMP.wav", &wt) == 0);
    assert(exfat_write(&wt, pcm, 8192u) == 0);
    assert(exfat_close_file(&wt) == 0);
    assert(exfat_stat_fs(&vol, &free_mid, NULL) == 0);
    assert(free_mid <= free_before);
    assert(exfat_delete_file(&vol, "/record", "REC_TEMP.wav") == 0);
    exfat_entry_t deleted_ent;
    assert(exfat_find_path(&vol, "/record/REC_TEMP.wav", &deleted_ent) != 0);
    assert(exfat_stat_fs(&vol, &free_after, NULL) == 0);
    assert(free_after >= free_mid);

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

    /* Persistent settings (/cassini/settings.bin) round-trip, clamping, and CRC-16 rejection */
    ui_settings_t cfg_def = {
        .reserved0 = 0u, .mic_gain = 20u, .sound_mode = 0u, .channels = 2u,
        .vor_enabled = 0u, .brightness_idx = 3u, .screen_off_idx = UI_SCR_OFF_30S,
        .skip_mode = UI_SKIP_5_10, .hp_vol = 18u, .spk_vol = 18u
    };
    ui_settings_t cfg_load = cfg_def;
    char     last_name[32] = "UNCHANGED";
    uint32_t last_resume   = 999u;
    int      has_last      = 0;

    /* Missing file -> -1, leaving defaults intact */
    assert(ui_persist_load(&vol2, &cfg_load, last_name, &last_resume, &has_last) == -1);
    assert(memcmp(&cfg_load, &cfg_def, sizeof(cfg_def)) == 0);
    assert(strcmp(last_name, "UNCHANGED") == 0 && last_resume == 999u);

    /* Save custom settings & last-played state, remount, and verify round-trip */
    ui_settings_t cfg_save = {
        .reserved0 = 0u, .mic_gain = 27u, .sound_mode = 1u, .channels = 1u,
        .vor_enabled = 1u, .brightness_idx = 4u, .screen_off_idx = UI_SCR_OFF_5M,
        .skip_mode = UI_SKIP_10_30, .hp_vol = 22u, .spk_vol = 14u
    };
    assert(ui_persist_save(&vol2, &cfg_save, "REC_0001.wav", 42u, 1) == 0);

    exfat_vol_t vol3;
    assert(exfat_mount(&vol3, &dev) == 0);
    assert(ui_persist_load(&vol3, &cfg_load, last_name, &last_resume, &has_last) == 0);
    assert(memcmp(&cfg_load, &cfg_save, sizeof(cfg_save)) == 0);
    assert(has_last == 1 && last_resume == 42u && strcmp(last_name, "REC_0001.wav") == 0);

    /* Overwrite with out-of-range values -> verify clamping on save/load */
    ui_settings_t cfg_oob = {
        .reserved0 = 5u, .mic_gain = 99u, .sound_mode = 7u, .channels = 9u,
        .vor_enabled = 3u, .brightness_idx = 9u, .screen_off_idx = 9u,
        .skip_mode = 9u, .hp_vol = 99u, .spk_vol = 99u
    };
    assert(ui_persist_save(&vol3, &cfg_oob, "", 123u, 0) == 0);
    assert(ui_persist_load(&vol3, &cfg_load, last_name, &last_resume, &has_last) == 0);
    assert(cfg_load.reserved0 == 0u && cfg_load.mic_gain == 31u && cfg_load.sound_mode == 0u);
    assert(cfg_load.channels == 2u && cfg_load.vor_enabled == 1u && cfg_load.brightness_idx == 3u);
    assert(cfg_load.screen_off_idx == UI_SCR_OFF_30S && cfg_load.skip_mode == UI_SKIP_5_10);
    assert(cfg_load.hp_vol == 31u && cfg_load.spk_vol == 31u);
    assert(has_last == 0 && last_resume == 0u && last_name[0] == '\0');

    /* Corrupt a byte inside /cassini/settings.bin -> CRC-16 must reject & keep defaults */
    exfat_entry_t set_ent;
    assert(exfat_find_path(&vol3, "/cassini/settings.bin", &set_ent) == 0);
    assert(set_ent.size == sizeof(openfw_persist_t));
    uint8_t raw_set[sizeof(openfw_persist_t)];
    uint32_t raw_got = 0u;
    assert(exfat_read_file(&vol3, &set_ent, 0u, raw_set, (uint32_t)sizeof(raw_set), &raw_got) == 0);
    assert(raw_got == sizeof(raw_set));
    raw_set[10] ^= 0xFFu;
    assert(exfat_delete_file(&vol3, "/cassini", "settings.bin") == 0);
    exfat_writer_t sw;
    assert(exfat_create_file(&vol3, "/cassini", "settings.bin", &sw) == 0);
    assert(exfat_write(&sw, raw_set, (uint32_t)sizeof(raw_set)) == 0);
    assert(exfat_close_file(&sw) == 0);

    cfg_load = cfg_def;
    assert(ui_persist_load(&vol3, &cfg_load, last_name, &last_resume, &has_last) == -1);
    assert(memcmp(&cfg_load, &cfg_def, sizeof(cfg_def)) == 0);

    /* Restore a valid settings.bin before fsck.exfat */
    assert(ui_persist_save(&vol3, &cfg_save, "REC_0001.wav", 42u, 1) == 0);

    fclose(fp);
    printf("  [PASS] exfat-img (%s: 16,428 B multi-cluster WAV + settings.bin persist/CRC-16 verified)\n", path);
}

static uint8_t test_rfcomm_fcs(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0xFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint8_t)((crc >> 1) ^ 0xE0u) : (uint8_t)(crc >> 1);
        }
    }
    return (uint8_t)(crc ^ 0xFFu);
}

static void send_l2cap_client(bt_spp_t *spp, uint16_t dcid, const uint8_t *sdu, uint16_t len)
{
    uint8_t pdu[700];
    put_le16(&pdu[0], len);
    put_le16(&pdu[2], dcid);
    memcpy(&pdu[4], sdu, len);
    bt_spp_on_acl_rx(spp, 0x0080u, 0x02u, pdu, (uint16_t)(4u + len));
}

static void send_rfcomm_client_ctrl(bt_spp_t *spp, uint16_t rfcomm_dcid, uint8_t dlci, uint8_t ctrl)
{
    uint8_t f[4];
    f[0] = (uint8_t)((dlci << 2) | 0x03u); /* Initiator CR=1, EA=1 */
    f[1] = ctrl;
    f[2] = 0x01u;
    f[3] = test_rfcomm_fcs(f, 3u);
    send_l2cap_client(spp, rfcomm_dcid, f, 4u);
}

static void send_rfcomm_client_uih(bt_spp_t *spp, uint16_t rfcomm_dcid, uint8_t dlci,
                                   const uint8_t *info, uint16_t len)
{
    uint8_t f[700];
    uint16_t h = 0u;
    f[h++] = (uint8_t)((dlci << 2) | 0x03u);
    f[h++] = 0xEFu;
    if (len <= 127u) {
        f[h++] = (uint8_t)((len << 1) | 0x01u);
    } else {
        f[h++] = (uint8_t)((len & 0x7Fu) << 1);
        f[h++] = (uint8_t)(len >> 7);
    }
    memcpy(&f[h], info, len);
    f[h + len] = test_rfcomm_fcs(f, 2u);
    send_l2cap_client(spp, rfcomm_dcid, f, (uint16_t)(h + len + 1u));
}

static void send_rfcomm_client_credits(bt_spp_t *spp, uint16_t rfcomm_dcid, uint8_t dlci, uint8_t credits)
{
    uint8_t f[5];
    f[0] = (uint8_t)((dlci << 2) | 0x03u);
    f[1] = 0xFFu;
    f[2] = 0x01u; /* len=0, EA=1 */
    f[3] = credits;
    f[4] = test_rfcomm_fcs(f, 2u);
    send_l2cap_client(spp, rfcomm_dcid, f, 5u);
}

typedef struct {
    bt_spp_t   *spp;
    xrec_rtc_t  rtc;
    uint8_t     bound_key[32];
    int         bound_called;
    int         ota_called;
    int         dfu_called;
    uint8_t     exp_buf[1024];
    uint32_t    exp_len;
    int         exp_runs;
    uint32_t    last_exp_arg;
    char        log_buf[256];
    uint32_t    log_widx;
    int         log_cleared;
    uint8_t     rx_stream[4096];
    uint32_t    rx_len;
    uint32_t    rx_acl_pkts;
    uint8_t     dsp_profile;
    uint8_t     mic_gain;
    uint8_t     channels;
    uint8_t     vor_enabled;
} sim_phone_t;

static void send_xrec_put_target(bt_spp_t *spp, uint32_t offset, const char *target,
                                 const uint8_t *chunk, uint16_t chunk_len, uint16_t crc)
{
    uint8_t pkt[96u + XREC_MAX_CHUNK];
    int hlen = snprintf((char *)pkt, sizeof(pkt), "+XREC:PUT,%u,%u,%u,%s:",
                        (unsigned)offset, (unsigned)chunk_len, (unsigned)crc, target);
    assert(hlen > 0 && hlen + chunk_len <= (int)sizeof(pkt));
    memcpy(&pkt[hlen], chunk, chunk_len);
    send_rfcomm_client_uih(spp, 0x0041u, 2u, pkt, (uint16_t)(hlen + chunk_len));
}

static void send_xrec_put_chunk(bt_spp_t *spp, uint32_t offset, const uint8_t *chunk,
                                uint16_t chunk_len, uint16_t crc)
{
    send_xrec_put_target(spp, offset, "OTA", chunk, chunk_len, crc);
}

static void mock_spp_data_cb(void *user, const uint8_t *data, uint16_t len)
{
    xrec_ctx_t *xrec = (xrec_ctx_t *)user;
    xrec_on_rx(xrec, data, len);
}

static void mock_spp_disc_cb(void *user)
{
    xrec_ctx_t *xrec = (xrec_ctx_t *)user;
    xrec_on_disconnect(xrec);
}

static int mock_xrec_send(void *user, const uint8_t *data, uint16_t len)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    return bt_spp_send(sp->spp, data, len);
}

static void mock_xrec_get_stat(void *user, xrec_stat_t *out)
{
    (void)user;
    out->bat_pct       = 87u;
    out->voltage_mv    = 4095u;
    out->charge_status = 1u;
    out->vbus          = 1u;
    out->media_status  = 0x10u;
    out->rec_sec       = 0u;
    out->free_mb       = 30100u;
    out->total_mb      = 30500u;
}

static void mock_xrec_get_rtc(void *user, xrec_rtc_t *out)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    *out = sp->rtc;
}

static int mock_xrec_set_rtc(void *user, const xrec_rtc_t *dt)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    sp->rtc = *dt;
    return 0;
}

static void mock_xrec_entropy(void *user, uint8_t out[16])
{
    (void)user;
    for (uint32_t i = 0u; i < 16u; i++) {
        out[i] = (uint8_t)(0xA0u + i);
    }
}

static void mock_xrec_bound(void *user, const uint8_t key[32])
{
    sim_phone_t *sp = (sim_phone_t *)user;
    memcpy(sp->bound_key, key, 32u);
    sp->bound_called = 1;
}

static void mock_xrec_on_ota(void *user)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    sp->ota_called++;
}

static int mock_xrec_exp_write(void *user, uint32_t off, const uint8_t *data, uint16_t len)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    if (off + len > sizeof(sp->exp_buf)) {
        return -1;
    }
    memcpy(&sp->exp_buf[off], data, len);
    if (off + len > sp->exp_len) {
        sp->exp_len = off + len;
    }
    return 0;
}

static int mock_xrec_exp_run(void *user, uint32_t entry_off, uint32_t arg,
                             uint32_t *rc, uint32_t *elapsed_ms,
                             uint32_t *out_len, uint32_t *log_widx)
{
    (void)entry_off;
    sim_phone_t *sp = (sim_phone_t *)user;
    sp->exp_runs++;
    sp->last_exp_arg = arg;
    const char *msg = "EXP_OK_LOG\n";
    uint32_t mlen = (uint32_t)strlen(msg);
    memcpy(sp->log_buf, msg, mlen);
    sp->log_widx = mlen;
    *rc         = arg ^ 0x55u;
    *elapsed_ms = 12u;
    *out_len    = 4u;
    *log_widx   = sp->log_widx;
    return 0;
}

static uint32_t mock_xrec_read_log(void *user, uint32_t off, uint8_t *dst,
                                   uint32_t max_len, uint32_t *out_widx)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    *out_widx = sp->log_widx;
    if (off >= sp->log_widx) {
        return 0u;
    }
    uint32_t take = sp->log_widx - off;
    if (take > max_len) {
        take = max_len;
    }
    memcpy(dst, &sp->log_buf[off], take);
    return take;
}

static void mock_xrec_reset_log(void *user)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    sp->log_widx = 0u;
    sp->log_cleared++;
}

static void mock_xrec_on_dfu(void *user)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    sp->dfu_called++;
}

static int mock_xrec_audio_cfg(void *user, int set, uint8_t *dsp_profile,
                               uint8_t *gain, uint8_t *channels, uint8_t *vor)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    if (set) {
        if (dsp_profile) sp->dsp_profile = *dsp_profile;
        if (gain)        sp->mic_gain    = *gain;
        if (channels)    sp->channels    = *channels;
        if (vor)         sp->vor_enabled = *vor;
    }
    if (dsp_profile) *dsp_profile = sp->dsp_profile;
    if (gain)        *gain        = sp->mic_gain;
    if (channels)    *channels    = sp->channels;
    if (vor)         *vor         = sp->vor_enabled;
    return 0;
}

static void drain_acl_to_phone(bt_spp_t *spp, sim_phone_t *sp)
{
    uint16_t hdl = 0u;
    uint8_t  pdu[700];
    uint16_t plen;
    while ((plen = bt_spp_pop_acl_tx(spp, &hdl, pdu, sizeof(pdu))) > 0u) {
        assert(hdl == 0x0080u && plen >= 4u);
        sp->rx_acl_pkts++;
        uint16_t l2cid = (uint16_t)pdu[2] | ((uint16_t)pdu[3] << 8);
        if (l2cid == 0x0102u && plen > 8u) {
            /* RFCOMM frame from server -> extract UIH DLCI 2 user data */
            const uint8_t *rf = &pdu[4];
            uint16_t rf_len   = (uint16_t)(plen - 4u);
            uint8_t  dlci     = (uint8_t)(rf[0] >> 2);
            uint8_t  ctrl     = rf[1];
            if (dlci == 2u && (ctrl & 0xEFu) == 0xEFu) {
                uint16_t hdr  = 3u;
                uint16_t ulen;
                if (rf[2] & 0x01u) {
                    ulen = (uint16_t)(rf[2] >> 1);
                } else {
                    ulen = (uint16_t)((rf[2] >> 1) | ((uint16_t)rf[3] << 7));
                    hdr  = 4u;
                }
                if (ctrl & 0x10u) {
                    hdr++;
                }
                assert(hdr + ulen + 1u == rf_len);
                if (ulen > 0u) {
                    assert(sp->rx_len + ulen <= sizeof(sp->rx_stream));
                    memcpy(&sp->rx_stream[sp->rx_len], &rf[hdr], ulen);
                    sp->rx_len += ulen;
                }
            }
        }
    }
}

static void test_bt_spp_and_xrec(void)
{
    /* Prepare clean exFAT volume in g_disk with /record/REC_0001.wav + EXACT960.wav + 8 extra files */
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
    put_le32(&vbr[0x50], 12u);
    put_le32(&vbr[0x54], 4u);
    put_le32(&vbr[0x58], 16u);
    put_le32(&vbr[0x5C], 64u);
    put_le32(&vbr[0x60], 2u);
    vbr[0x6C] = 9u;
    vbr[0x6D] = 0u;
    vbr[510]  = 0x55u;
    vbr[511]  = 0xAAu;

    uint8_t *fat = &g_disk[16u * 512u];
    put_le32(fat + 0u * 4u, 0xFFFFFFF8u);
    put_le32(fat + 1u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 2u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 3u * 4u, 0xFFFFFFFFu);
    uint8_t *bmp_ent = &g_disk[20u * 512u];
    bmp_ent[0] = 0x81u;
    put_le32(bmp_ent + 20u, 3u);
    put_le64(bmp_ent + 24u, 8u);
    g_disk[21u * 512u] = 0x03u;

    blkdev_t dev = { mem_read_sectors, mem_write_sectors, MEM_DISK_SECTORS, NULL };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    assert(exfat_mkdir(&vol, "record") == 0);

    uint8_t wav_file[1024];
    assert(wav_build_header(wav_file, 16000u, 1u, 16u, sizeof(wav_file) - 44u) == 0);
    for (uint32_t i = 44u; i < sizeof(wav_file); i++) {
        wav_file[i] = (uint8_t)((i * 41u + 3u) & 0xFFu);
    }
    exfat_writer_t w;
    assert(exfat_create_file(&vol, "/record", "REC_0001.wav", &w) == 0);
    assert(exfat_write(&w, wav_file, sizeof(wav_file)) == 0);
    assert(exfat_close_file(&w) == 0);

    uint8_t exact960[960];
    assert(wav_build_header(exact960, 16000u, 1u, 16u, sizeof(exact960) - 44u) == 0);
    for (uint32_t i = 44u; i < sizeof(exact960); i++) {
        exact960[i] = (uint8_t)((i * 59u + 13u) & 0xFFu);
    }
    assert(exfat_create_file(&vol, "/record", "EXACT960.wav", &w) == 0);
    assert(exfat_write(&w, exact960, sizeof(exact960)) == 0);
    assert(exfat_close_file(&w) == 0);

    for (int idx = 2; idx <= 9; idx++) {
        char fname[20];
        snprintf(fname, sizeof(fname), "REC_%04d.wav", idx);
        assert(exfat_create_file(&vol, "/record", fname, &w) == 0);
        assert(exfat_write(&w, wav_file, 64u) == 0);
        assert(exfat_close_file(&w) == 0);
    }

    bt_spp_t    spp;
    sim_phone_t sp;
    xrec_ctx_t  xrec;
    memset(&sp, 0, sizeof(sp));
    sp.spp         = &spp;
    sp.rtc.year    = 2026u;
    sp.rtc.month   = 11u;
    sp.rtc.day     = 19u;
    sp.dsp_profile = 0u;
    sp.mic_gain    = 20u;
    sp.channels    = 1u;
    sp.vor_enabled = 0u;

    xrec_ops_t ops = {
        mock_xrec_send, mock_xrec_get_stat, mock_xrec_get_rtc,
        mock_xrec_set_rtc, mock_xrec_entropy, mock_xrec_bound, NULL,
        NULL, NULL, mock_xrec_on_ota,
        mock_xrec_exp_write, mock_xrec_exp_run, mock_xrec_read_log,
        mock_xrec_reset_log, mock_xrec_on_dfu, mock_xrec_audio_cfg,
    };
    xrec_init(&xrec, &vol, &ops, &sp);
    bt_spp_init(&spp, mock_spp_data_cb, &xrec);
    bt_spp_set_disconnect_cb(&spp, mock_spp_disc_cb, &xrec);

    /* 1. L2CAP Info + SDP connect + HFP negative match + SPP positive match */
    uint8_t info_req[6] = { 0x0Au, 1u, 2u, 0u, 2u, 0u };
    send_l2cap_client(&spp, 0x0001u, info_req, 6u);
    uint8_t conn_sdp[8] = { 0x02u, 2u, 4u, 0u, 0x01u, 0x00u, 0x01u, 0x01u };
    send_l2cap_client(&spp, 0x0001u, conn_sdp, 8u);
    uint8_t cfg_sdp[8]  = { 0x04u, 3u, 4u, 0u, 0x40u, 0x00u, 0u, 0u };
    send_l2cap_client(&spp, 0x0001u, cfg_sdp, 8u);
    drain_acl_to_phone(&spp, &sp);

    /* HFP (0x111E) + L2CAP (0x0100) must NOT match our SPP record */
    uint8_t sdp_hfp[18] = {
        0x06u, 0x00u, 0x01u, 0x00u, 13u,
        0x35u, 0x06u, 0x19u, 0x11u, 0x1Eu, 0x19u, 0x01u, 0x00u,
        0x00u, 0xFFu, 0x35u, 0x01u, 0x00u
    };
    send_l2cap_client(&spp, 0x0040u, sdp_hfp, sizeof(sdp_hfp));
    uint16_t hdl = 0u;
    uint8_t sdp_out[256];
    uint16_t sdp_len = bt_spp_pop_acl_tx(&spp, &hdl, sdp_out, sizeof(sdp_out));
    assert(sdp_len == 14u && sdp_out[4] == 0x07u && sdp_out[11] == 0x35u && sdp_out[12] == 0x00u);

    uint8_t sdp_req[15] = { 0x06u, 0x00u, 0x02u, 0x00u, 10u, 0x35u, 0x03u, 0x19u, 0x11u, 0x01u, 0x00u, 0xFFu, 0x35u, 0x05u, 0x0Au };
    send_l2cap_client(&spp, 0x0040u, sdp_req, sizeof(sdp_req));
    sdp_len = bt_spp_pop_acl_tx(&spp, &hdl, sdp_out, sizeof(sdp_out));
    assert(sdp_len > 20u && sdp_out[4] == 0x07u);
    int found_name = 0;
    for (uint16_t i = 0u; i + 7u <= sdp_len; i++) {
        if (memcmp(&sdp_out[i], "Cassini", 7u) == 0) {
            found_name = 1;
            break;
        }
    }
    assert(found_name == 1);

    /* 2. L2CAP RFCOMM connect + Config with Android-style MTU=1691 -> RFCOMM PN -> SABM */
    uint8_t conn_rf[8] = { 0x02u, 4u, 4u, 0u, 0x03u, 0x00u, 0x02u, 0x01u };
    send_l2cap_client(&spp, 0x0001u, conn_rf, 8u);
    uint8_t cfg_rf[12] = { 0x04u, 5u, 8u, 0u, 0x41u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x9Bu, 0x06u };
    send_l2cap_client(&spp, 0x0001u, cfg_rf, 12u);

    int saw_echo_1691 = 0;
    uint16_t plen;
    while ((plen = bt_spp_pop_acl_tx(&spp, &hdl, sdp_out, sizeof(sdp_out))) > 0u) {
        if (plen >= 18u && sdp_out[2] == 0x01u && sdp_out[4] == 0x05u && sdp_out[14] == 0x01u) {
            uint16_t echoed = (uint16_t)sdp_out[16] | ((uint16_t)sdp_out[17] << 8);
            if (echoed == 1691u) {
                saw_echo_1691 = 1;
            }
        }
    }
    assert(saw_echo_1691 == 1);

    send_rfcomm_client_ctrl(&spp, 0x0041u, 0u, 0x3Fu); /* SABM DLCI 0 */
    uint8_t pn_cmd[10] = { 0x83u, 0x11u, 0x02u, 0xF0u, 0x00u, 0x00u, 0x94u, 0x02u, 0x00u, 0x07u }; /* MTU 660, k=7 */
    send_rfcomm_client_uih(&spp, 0x0041u, 0u, pn_cmd, sizeof(pn_cmd));
    send_rfcomm_client_ctrl(&spp, 0x0041u, 2u, 0x3Fu); /* SABM DLCI 2 */
    drain_acl_to_phone(&spp, &sp);
    assert(bt_spp_is_open(&spp) == 1);

    /* 3. +XREC:PING, +XREC:STAT (locked), +XREC:PAIR, +XREC:CHAL -> +XREC:AUTH */
    const char *c_ping = "+XREC:PING\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ping, (uint16_t)strlen(c_ping));
    const char *c_lock = "+XREC:STAT\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_lock, (uint16_t)strlen(c_lock));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:PONG,openfw_v5,v5,bound=0,auth=0,pair=0") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,LOCKED") != NULL);

    sp.rx_len = 0u;
    xrec_open_pairing(&xrec, "482910");
    const char *c_pair = "+XREC:PAIR,482910,000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_pair, (uint16_t)strlen(c_pair));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(sp.bound_called == 1 && strstr((const char *)sp.rx_stream, "+XREC:OK,PAIR") != NULL);

    /* Disconnect callback check: RFCOMM DISC DLCI 2 must drop auth */
    send_rfcomm_client_ctrl(&spp, 0x0041u, 2u, 0x53u); /* DISC */
    drain_acl_to_phone(&spp, &sp);
    assert(bt_spp_is_open(&spp) == 0);

    uint8_t pn_k7[10] = { 0x83u, 0x11u, 0x02u, 0xF0u, 0x00u, 0x00u, 0x94u, 0x02u, 0x00u, 0x07u };
    send_rfcomm_client_uih(&spp, 0x0041u, 0u, pn_k7, sizeof(pn_k7));
    send_rfcomm_client_ctrl(&spp, 0x0041u, 2u, 0x3Fu); /* re-SABM DLCI 2 */
    drain_acl_to_phone(&spp, &sp);
    assert(bt_spp_is_open(&spp) == 1);

    sp.rx_len = 0u;
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_lock, (uint16_t)strlen(c_lock));
    const uint8_t unauth_bin[16] = {
        '\r', '\n', ':', 0x00u, 'X', 0xFFu, 0x01u, '\r',
        '\n', '+', 'X', 'R', 'E', 'C', ':', 0x00u
    };
    send_xrec_put_chunk(&spp, 0u, unauth_bin, 16u,
                        openfw_crc16_ccitt_false(unauth_bin, 16u));
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ping, (uint16_t)strlen(c_ping));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,LOCKED") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:PONG,openfw_v5") != NULL);
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 2u);

    /* Authenticate using docs/XREC_PROTOCOL.md §5 HMAC vector */
    sp.rx_len = 0u;
    const char *c_chal = "+XREC:CHAL\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_chal, (uint16_t)strlen(c_chal));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:CHAL,a0a1a2a3a4a5a6a7a8a9aaabacadaeaf") != NULL);

    sp.rx_len = 0u;
    const char *c_auth = "+XREC:AUTH,b1489727b2602a0e047cb237580a4c795aaf9023c5bc6b0b865c7fa17e0a2367\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_auth, (uint16_t)strlen(c_auth));
    const char *c_rtc_bad = "+XREC:RTC,2026,13,19,17,05,30,3\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_rtc_bad, (uint16_t)strlen(c_rtc_bad));
    const char *c_rtc  = "+XREC:RTC,2026,11,19,17,05,30,3\r+XREC:RTC\r+XREC:STAT\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_rtc, (uint16_t)strlen(c_rtc));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,AUTH") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,RTCRNG") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,RTC") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:RTC,2026-11-19 17:05:30,3") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:STAT,87,4095,1,1,16,0,30100,30500") != NULL);

    /* Replenish credits & test coalesced 10-file +XREC:LS + +XREC:DEL + path traversal block */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    sp.rx_len      = 0u;
    sp.rx_acl_pkts = 0u;
    const char *c_ls = "+XREC:LS\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ls, (uint16_t)strlen(c_ls));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(sp.rx_acl_pkts <= 2u);
    assert(strstr((const char *)sp.rx_stream, "+XREC:F,1024,REC_0001.wav") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:F,960,EXACT960.wav") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:F,64,REC_0009.wav") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:END") != NULL);

    sp.rx_len = 0u;
    const char *c_del_trav = "+XREC:DEL,../evil.wav\r+XREC:GETS,0,16,16,../evil.wav\r+XREC:DEL,REC_0009.wav\r+XREC:LS\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_del_trav, (uint16_t)strlen(c_del_trav));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,NAME") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,DEL") != NULL);
    assert(strstr((const char *)sp.rx_stream, "REC_0009.wav") == NULL);

    /* 4. Single-frame +XREC:GET */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 6u);
    sp.rx_len = 0u;
    const char *c_get = "+XREC:GET,0,300,REC_0001.wav\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_get, (uint16_t)strlen(c_get));
    drain_acl_to_phone(&spp, &sp);
    assert(sp.rx_len == 12u + 300u && memcmp(sp.rx_stream, "XREC", 4u) == 0);
    assert(memcmp(sp.rx_stream + 12u, wav_file, 300u) == 0);

    /* 5. +XREC:GETS exact-multiple EOF (0-byte XREC frame) + credit stall + xrec_poll resume */
    while (spp.peer_credits > 1u) {
        send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ping, (uint16_t)strlen(c_ping));
        drain_acl_to_phone(&spp, &sp);
    }
    assert(spp.peer_credits == 1u);

    sp.rx_len = 0u;
    const char *c_gets = "+XREC:GETS,0,1920,480,EXACT960.wav\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_gets, (uint16_t)strlen(c_gets));
    drain_acl_to_phone(&spp, &sp);
    assert(sp.rx_len == 12u + 480u); /* Stalled cleanly after 1st frame because peer_credits hit 0 */
    assert(xrec.gets_active == 1u);

    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 4u);
    xrec_poll(&xrec);
    drain_acl_to_phone(&spp, &sp);
    assert(xrec.gets_active == 0u);

    uint8_t downloaded[1024];
    uint32_t dpos = 0u, ppos = 0u, frames_seen = 0u;
    int saw_eof_zero_frame = 0;
    while (ppos + 12u <= sp.rx_len) {
        assert(memcmp(&sp.rx_stream[ppos], "XREC", 4u) == 0);
        uint32_t foff = get_le32(&sp.rx_stream[ppos + 4u]);
        uint16_t flen = (uint16_t)sp.rx_stream[ppos + 8u] | ((uint16_t)sp.rx_stream[ppos + 9u] << 8);
        uint16_t fcrc = (uint16_t)sp.rx_stream[ppos + 10u] | ((uint16_t)sp.rx_stream[ppos + 11u] << 8);
        assert(foff == dpos);
        assert(ppos + 12u + flen <= sp.rx_len);
        assert(openfw_crc16_ccitt_false(&sp.rx_stream[ppos + 12u], flen) == fcrc);
        if (flen > 0u) {
            memcpy(&downloaded[dpos], &sp.rx_stream[ppos + 12u], flen);
            dpos += flen;
        } else {
            saw_eof_zero_frame = 1;
            assert(fcrc == 0xFFFFu);
        }
        frames_seen++;
        ppos += 12u + flen;
    }
    assert(ppos == sp.rx_len && frames_seen == 3u && saw_eof_zero_frame == 1);
    assert(dpos == sizeof(exact960) && memcmp(downloaded, exact960, sizeof(exact960)) == 0);

    /* 6. +XREC:PUT (chunks 560+560+80 with bad CRC + idempotent retry) -> +XREC:OTA */
    uint8_t ota_img[1200];
    for (uint32_t i = 0u; i < (uint32_t)sizeof(ota_img); i++) {
        ota_img[i] = (uint8_t)((i * 37u) ^ 0x5Au);
    }
    ota_img[0]   = '\r';
    ota_img[1]   = '\n';
    ota_img[2]   = '\0';
    ota_img[3]   = ':';
    ota_img[560] = '\r';
    ota_img[561] = ':';

    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    sp.rx_len = 0u;
    uint16_t crc0 = openfw_crc16_ccitt_false(&ota_img[0], 560u);
    send_xrec_put_chunk(&spp, 0u, &ota_img[0], 560u, crc0);
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strcmp((const char *)sp.rx_stream, "+XREC:OK,PUT,0,560\r\n") == 0);

    /* Bad CRC at offset 560 -> +XREC:ERR,CRC, leaving file offset at 560 */
    sp.rx_len = 0u;
    uint16_t crc1 = openfw_crc16_ccitt_false(&ota_img[560], 560u);
    send_xrec_put_chunk(&spp, 560u, &ota_img[560], 560u, (uint16_t)(crc1 ^ 0x0001u));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strcmp((const char *)sp.rx_stream, "+XREC:ERR,CRC\r\n") == 0);

    /* Retry offset 560 with good CRC, then idempotent duplicate resend of offset 560 */
    sp.rx_len = 0u;
    send_xrec_put_chunk(&spp, 560u, &ota_img[560], 560u, crc1);
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strcmp((const char *)sp.rx_stream, "+XREC:OK,PUT,560,560\r\n") == 0);

    sp.rx_len = 0u;
    send_xrec_put_chunk(&spp, 560u, &ota_img[560], 560u, crc1);
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strcmp((const char *)sp.rx_stream, "+XREC:OK,PUT,560,560\r\n") == 0);

    /* Final 80-byte chunk at offset 1120 + +XREC:OTA trigger */
    sp.rx_len = 0u;
    uint16_t crc2 = openfw_crc16_ccitt_false(&ota_img[1120], 80u);
    send_xrec_put_chunk(&spp, 1120u, &ota_img[1120], 80u, crc2);
    const char *c_ota = "+XREC:OTA\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ota, (uint16_t)strlen(c_ota));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strcmp((const char *)sp.rx_stream, "+XREC:OK,PUT,1120,80\r\n+XREC:OK,OTA\r\n") == 0);
    assert(sp.ota_called == 1);

    exfat_entry_t ota_ent;
    assert(exfat_find_path(&vol, "/update.up", &ota_ent) == 0 && ota_ent.size == sizeof(ota_img));
    uint8_t ota_readback[1200];
    uint32_t ota_nread = 0u;
    assert(exfat_read_file(&vol, &ota_ent, 0u, ota_readback, sizeof(ota_readback), &ota_nread) == 0);
    assert(ota_nread == sizeof(ota_img) && memcmp(ota_readback, ota_img, sizeof(ota_img)) == 0);

    /* 7. +XREC EXP upload/run/log/clear, CAS,<filename>+PUTEND, and DFU */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    uint8_t exp_code[32];
    for (uint32_t i = 0u; i < sizeof(exp_code); i++) {
        exp_code[i] = (uint8_t)(0xE0u + i);
    }
    uint16_t exp_crc = openfw_crc16_ccitt_false(exp_code, sizeof(exp_code));
    sp.rx_len = 0u;
    send_xrec_put_target(&spp, 0u, "EXP", exp_code, sizeof(exp_code), (uint16_t)(exp_crc ^ 1u));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strcmp((const char *)sp.rx_stream, "+XREC:ERR,CRC\r\n") == 0);

    sp.rx_len = 0u;
    send_xrec_put_target(&spp, 0u, "EXP", exp_code, sizeof(exp_code), exp_crc);
    const char *c_run = "+XREC:RUN,0,123\r+XREC:LOG,0,64\r+XREC:LOG,CLEAR\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_run, (uint16_t)strlen(c_run));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(sp.exp_len == 32u && memcmp(sp.exp_buf, exp_code, 32u) == 0);
    assert(sp.exp_runs == 1 && sp.last_exp_arg == 123u && sp.log_cleared == 1);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,PUT,0,32") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,RUN,46,12,4,11") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:LOG,0,11,11:EXP_OK_LOG\n") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,LOG,CLEAR") != NULL);

    uint8_t cas_data[48];
    for (uint32_t i = 0u; i < sizeof(cas_data); i++) {
        cas_data[i] = (uint8_t)(i * 7u + 1u);
    }
    uint16_t cas_crc = openfw_crc16_ccitt_false(cas_data, sizeof(cas_data));
    sp.rx_len = 0u;
    send_xrec_put_target(&spp, 0u, "CAS,demo.cwx", cas_data, sizeof(cas_data), cas_crc);
    const char *c_put_end = "+XREC:PUTEND\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_put_end, (uint16_t)strlen(c_put_end));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,PUT,0,48") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,PUTEND,48") != NULL);

    sp.rx_len = 0u;
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    const char *c_cfg_end = "+XREC:DSP\r+XREC:DSP=1\r+XREC:GAIN=25\r+XREC:CH=2\r+XREC:VOR=1\r+XREC:DSP=9\r+XREC:DFU\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_cfg_end, (uint16_t)strlen(c_cfg_end));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,DSP,0") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,DSP,1") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,GAIN,25") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,CH,2") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,VOR,1") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,ARGS") != NULL);
    assert(sp.dsp_profile == 1u && sp.mic_gain == 25u && sp.channels == 2u && sp.vor_enabled == 1u);
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,DFU") != NULL);
    assert(sp.dfu_called == 1);

    exfat_entry_t cas_ent;
    assert(exfat_find_path(&vol, "/cassini/demo.cwx", &cas_ent) == 0 && cas_ent.size == 48u);
    uint8_t cas_rb[48];
    uint32_t cas_nread = 0u;
    assert(exfat_read_file(&vol, &cas_ent, 0u, cas_rb, sizeof(cas_rb), &cas_nread) == 0);
    assert(cas_nread == 48u && memcmp(cas_rb, cas_data, 48u) == 0);

    printf("  [PASS] bt_spp + xrec (L2CAP, SDP, RFCOMM credits/DISC, +XREC auth/LS/DEL/GET/GETS+EOF/PUT+OTA/EXP/RUN/LOG/CAS/DFU)\n");
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
    test_wav_stream_and_ima_adpcm();
    test_cwx_engine();
    test_exfat();
    test_exfat_write();
    test_usb_msc();
    test_usb_msc_vendor_e0();
    test_bt_spp_and_xrec();
    printf("All open_fw host unit tests PASSED!\n");
    return 0;
}
