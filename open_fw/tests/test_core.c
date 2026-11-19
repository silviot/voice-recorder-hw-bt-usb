/*
 * Host unit tests for hardware-independent open_fw core libraries
 * (crypto.c, wav.c, exfat.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "openfw_crypto.h"
#include "openfw_fs.h"
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
    /* Verified test vector from docs/XREC_PROTOCOL.md */
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

#define MEM_DISK_SECTORS 64u
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

static void write_exfat_entry_set(uint8_t *slot_base, const char *name,
                                  uint16_t attr, uint8_t no_fat_chain,
                                  uint32_t first_cluster, uint64_t size)
{
    size_t nlen = strlen(name);
    assert(nlen <= 15u);

    uint8_t *f = slot_base;
    f[0] = 0x85u; /* File */
    f[1] = 2u;    /* SecondaryCount = 2 (Stream + Name) */
    put_le16(f + 4u, attr);

    uint8_t *s = slot_base + 32u;
    s[0] = 0xC0u; /* Stream Extension */
    s[1] = (uint8_t)(0x01u | (no_fat_chain ? 0x02u : 0x00u));
    s[3] = (uint8_t)nlen;
    put_le32(s + 20u, first_cluster);
    put_le64(s + 8u, size);
    put_le64(s + 24u, size);

    uint8_t *fn = slot_base + 64u;
    fn[0] = 0xC1u; /* File Name */
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

    /* LBA 0: MBR with partition 0 (type 0x07 exFAT) starting at LBA 4 */
    g_disk[510] = 0x55u;
    g_disk[511] = 0xAAu;
    g_disk[0x1BEu + 4u] = 0x07u;
    put_le32(&g_disk[0x1BEu + 8u], 4u);
    put_le32(&g_disk[0x1BEu + 12u], 60u);

    /* LBA 4: exFAT VBR (spc=1 sector/cluster = 512B clusters; FAT@+12=16; Heap@+16=20) */
    uint8_t *vbr = &g_disk[4u * 512u];
    memcpy(&vbr[3], "EXFAT   ", 8u);
    put_le64(&vbr[0x40], 4u);
    put_le64(&vbr[0x48], 60u);
    put_le32(&vbr[0x50], 12u); /* FatOffset -> LBA 16 */
    put_le32(&vbr[0x54], 4u);  /* FatLength */
    put_le32(&vbr[0x58], 16u); /* ClusterHeapOffset -> LBA 20 */
    put_le32(&vbr[0x5C], 44u); /* ClusterCount */
    put_le32(&vbr[0x60], 2u);  /* RootDirCluster */
    vbr[0x6C] = 9u;            /* 512 bytes/sector */
    vbr[0x6D] = 0u;            /* 1 sector/cluster */
    vbr[510]  = 0x55u;
    vbr[511]  = 0xAAu;

    /* LBA 16: FAT (cluster 4 -> cluster 5 -> EOF for fragmented file test) */
    uint8_t *fat = &g_disk[16u * 512u];
    put_le32(fat + 0u * 4u, 0xFFFFFFF8u);
    put_le32(fat + 1u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 2u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 3u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 4u * 4u, 5u);
    put_le32(fat + 5u * 4u, 0xFFFFFFFFu);

    /* Cluster 2 (LBA 20): Root dir with "record" subdir (cluster 3, NoFatChain=1) */
    write_exfat_entry_set(&g_disk[20u * 512u], "record", EXFAT_ATTR_DIR, 1u, 3u, 512u);

    /* Cluster 3 (LBA 21): /record with "test.wav" (cluster 4->5, NoFatChain=0, 700 B) */
    write_exfat_entry_set(&g_disk[21u * 512u], "test.wav", 0x0020u, 0u, 4u, 700u);

    /* Clusters 4 & 5 (LBA 22 & 23): valid 700-byte mono WAV file */
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

int main(void)
{
    printf("Running open_fw host unit tests...\n");
    test_crypto();
    test_wav();
    test_exfat();
    printf("All open_fw host unit tests PASSED!\n");
    return 0;
}
