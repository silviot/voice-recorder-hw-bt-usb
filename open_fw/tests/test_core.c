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

    /* SHA-256 FIPS 180-4 2-block vector + multi-block direct-block fast path (aligned vs unaligned vs chunked) */
    static const char vec2[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t exp_vec2[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
    };
    uint8_t d1[32], d2[32], d3[32];
    openfw_sha256((const uint8_t *)vec2, (uint32_t)(sizeof(vec2) - 1u), d1);
    assert(openfw_ct_memeq(d1, exp_vec2, 32u) == 1);

    uint8_t big[200] __attribute__((aligned(4)));
    for (uint32_t i = 0u; i < sizeof(big); i++) {
        big[i] = (uint8_t)((i * 37u + 13u) & 0xFFu);
    }
    openfw_sha256(big + 1u, 195u, d1); /* unaligned multi-block */
    openfw_sha256_ctx_t sctx;
    openfw_sha256_init(&sctx);
    openfw_sha256_update(&sctx, big + 1u, 13u);
    openfw_sha256_update(&sctx, big + 14u, 128u);
    openfw_sha256_update(&sctx, big + 142u, 54u);
    openfw_sha256_final(&sctx, d2);
    assert(openfw_ct_memeq(d1, d2, 32u) == 1);

    uint8_t aligned_copy[196] __attribute__((aligned(4)));
    memcpy(aligned_copy, big + 1u, 195u);
    openfw_sha256(aligned_copy, 195u, d3); /* aligned multi-block */
    assert(openfw_ct_memeq(d1, d3, 32u) == 1);

    printf("  [PASS] crypto (CRC-16, SHA-256 direct-block, HMAC-SHA256 vector, ct_memeq)\n");
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

    /* Test deterministic transcript line paginator (zero-reflow wrapping, turn breaks, clamping) */
    {
        static uint8_t cwx2_img[2048];
        memset(cwx2_img, 0, sizeof(cwx2_img));
        const char *wtexts[] = {
            "Systems,", "ACS", "one,", "we", "just", "had", "transition", "to",
            "high", "rate", "mode,", "and,", "uh,", "we", "are", "in", "the", "atmosphere.",
            "We", "have", "loss", "of", "signal", "at", "station.",
            "Go", "ahead.",
        };
        const uint8_t wspk[] = {
            0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
            1,1,1,1,1,1,1,
            2,2,
        };
        uint32_t nw = (uint32_t)(sizeof(wtexts) / sizeof(wtexts[0]));
        uint32_t s_off = CWX_HEADER_SIZE;
        uint32_t k_off = s_off + 3u * CWX_SPEAKER_SIZE;
        uint32_t w_off = k_off + 12u * CWX_SEEK_SIZE;
        uint32_t wp    = w_off;
        uint32_t w_rel[32], w_end[32];
        for (uint32_t i = 0u; i < nw; i++) {
            uint32_t st = 200u + i * 350u + (wspk[i] == 1u ? 1500u : (wspk[i] == 2u ? 2500u : 0u));
            uint32_t en = st + 260u;
            uint8_t flg = (i == 0u || wspk[i] != wspk[i - 1u]) ? CWX_FLAG_TURN_START : 0u;
            w_rel[i] = wp - w_off;
            w_end[i] = en;
            append_cwx_word(cwx2_img, &wp, st, en, wspk[i], flg, wtexts[i]);
        }
        memcpy(&cwx2_img[0], "CWX1", 4u);
        put_le16(&cwx2_img[4], 1u);
        put_le16(&cwx2_img[6], 3u);
        put_le32(&cwx2_img[8], 12000u);
        put_le32(&cwx2_img[12], nw);
        put_le32(&cwx2_img[16], s_off);
        put_le32(&cwx2_img[20], k_off);
        put_le32(&cwx2_img[24], 12u);
        put_le32(&cwx2_img[28], w_off);
        memcpy(&cwx2_img[32], "Pager Test", 10u);
        put_le16(&cwx2_img[s_off + 0u], 0x465Fu);  memcpy(&cwx2_img[s_off + 4u], "ACS 1", 5u);
        put_le16(&cwx2_img[s_off + 24u], 0xFD84u); memcpy(&cwx2_img[s_off + 28u], "Control", 7u);
        put_le16(&cwx2_img[s_off + 48u], 0xFBD9u); memcpy(&cwx2_img[s_off + 52u], "Earl", 4u);
        uint32_t scan = 0u;
        for (uint32_t sec = 0u; sec < 12u; sec++) {
            while (scan < nw && w_end[scan] < sec * 1000u) scan++;
            put_le32(&cwx2_img[k_off + sec * 8u + 0u], (scan < nw) ? w_rel[scan] : (wp - w_off));
            put_le32(&cwx2_img[k_off + sec * 8u + 4u], scan);
        }

        mem_cwx_src_t src2 = { cwx2_img, wp };
        cwx_stream_t cwx2;
        assert(cwx_open(&cwx2, mem_cwx_read, &src2, wp) == 0);
        cwx_tr_pager_t pager;
        cwx_tr_pager_init(&pager, &cwx2, CWX_TR_MAX_LINE_W);
        cwx_tr_view_t v;

        /* Before first word (t=50ms): clamped at top, 4 visible rows populated, no word pill */
        assert(cwx_tr_pager_update(&pager, 50u, 0, &v) == 1);
        assert(v.top_line_id == 0 && v.active_slot == 1 && v.spoken_len == 0u && v.word_len == 0u);
        assert(strcmp(v.speaker_name, "ACS 1") == 0);
        for (int s = 1; s <= 4; s++) {
            assert(v.lines[s].line_id >= 0 && v.lines[s].text[0] != '\0');
            assert(cwx_measure_md_px(v.lines[s].text, (uint32_t)strlen(v.lines[s].text)) <= CWX_TR_MAX_LINE_W);
        }

        /* First word active (t=250ms): "Systems," highlighted on line 0 */
        assert(cwx_tr_pager_update(&pager, 250u, 0, &v) == 1);
        assert(v.active_slot == 1 && v.word_start == 0u && v.word_len == 8u);
        char line0_saved[CWX_TR_LINE_CAP];
        strcpy(line0_saved, v.lines[1].text);

        /* Advance word-by-word across whole transcript: verify zero reflow & monotonic scroll */
        int32_t prev_top = v.top_line_id;
        for (uint32_t t = 250u; t <= 12000u; t += 100u) {
            assert(cwx_tr_pager_update(&pager, t, 0, &v) == 1);
            assert(v.top_line_id >= prev_top);
            assert(v.top_line_id - prev_top <= 1); /* smooth single-line increments */
            prev_top = v.top_line_id;
            for (int s = 1; s <= 4; s++) {
                if (v.lines[s].line_id == 0) {
                    assert(strcmp(v.lines[s].text, line0_saved) == 0);
                }
                if (v.lines[s].line_id >= 0) {
                    assert(cwx_measure_md_px(v.lines[s].text, (uint32_t)strlen(v.lines[s].text)) <= CWX_TR_MAX_LINE_W);
                }
            }
        }

        /* At EOF (t=12600ms, >320ms past last word end 12060ms):
         * all 4 visible rows remain filled, active_slot clamped to bottom row */
        assert(cwx_tr_pager_update(&pager, 12600u, 0, &v) == 1);
        assert(v.active_slot == 4);
        assert(strcmp(v.speaker_name, "Earl") == 0);
        assert(v.lines[4].turn_start == 1u && strcmp(v.lines[4].text, "Go ahead.") == 0);
        /* Silence >320ms after last word hides pill while playing, keeps it while paused */
        assert(v.word_len == 0u);
        assert(cwx_tr_pager_update(&pager, 12600u, 1, &v) == 1);
        assert(v.word_len > 0u);

        /* Rewind to start works cleanly */
        assert(cwx_tr_pager_update(&pager, 250u, 0, &v) == 1);
        assert(v.active_slot == 1 && strcmp(v.lines[1].text, line0_saved) == 0);
    }

    printf("  [PASS] cwx_engine (header, speakers, seek table, next_word, read_window, tr_pager)\n");
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
        snprintf(lc->first_name, sizeof(lc->first_name), "%s", e->name);
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

    exfat_set_timestamp(&vol, 0x5D748420u); /* Next day */
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
    assert(wav_ent.created_timestamp == 0x5D738420u);
    assert(seventh.created_timestamp == 0x5D748420u);
    assert(seventh.modified_timestamp == 0x5D748420u);
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

typedef struct {
    uint8_t *buf;
    uint32_t sectors;
    int      fail_write_at; /* <0 = never fail; >=0 = fail when write_count == fail_write_at */
    int      write_count;
} fault_disk_t;

static int fault_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    fault_disk_t *d = (fault_disk_t *)ctx;
    if (lba + count > d->sectors) return -1;
    memcpy(buf, &d->buf[lba * 512u], count * 512u);
    return 0;
}

static int fault_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    fault_disk_t *d = (fault_disk_t *)ctx;
    if (lba + count > d->sectors) return -1;
    if (d->fail_write_at >= 0 && d->write_count >= d->fail_write_at) {
        return -1;
    }
    d->write_count++;
    memcpy(&d->buf[lba * 512u], buf, count * 512u);
    return 0;
}

static void init_tiny_exfat(uint8_t *disk, uint32_t sectors)
{
    memset(disk, 0, sectors * 512u);
    uint8_t *vbr = &disk[0];
    memcpy(vbr + 3, "EXFAT   ", 8u);
    put_le64(vbr + 0x48, sectors);
    put_le32(vbr + 0x50, 16u); /* fat_offset */
    put_le32(vbr + 0x54, 4u);  /* fat_length */
    put_le32(vbr + 0x58, 20u); /* cluster_heap_offset */
    put_le32(vbr + 0x5C, sectors - 20u); /* cluster_count */
    put_le32(vbr + 0x60, 2u);  /* root_cluster */
    vbr[0x6C] = 9u;            /* 512 B sectors */
    vbr[0x6D] = 0u;            /* 512 B clusters */
    vbr[510]  = 0x55u;
    vbr[511]  = 0xAAu;

    uint8_t *fat = &disk[16u * 512u];
    put_le32(fat + 0u * 4u, 0xFFFFFFF8u);
    put_le32(fat + 1u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 2u * 4u, 0xFFFFFFFFu); /* root */
    put_le32(fat + 3u * 4u, 0xFFFFFFFFu); /* bitmap */

    uint8_t *bmp_ent = &disk[20u * 512u];
    bmp_ent[0] = 0x81u;
    bmp_ent[1] = 0x00u;
    put_le32(bmp_ent + 20u, 3u);
    put_le64(bmp_ent + 24u, 8u);
    disk[21u * 512u] = 0x03u;
}

static void test_exfat_remount_cache_invalidation(void)
{
    enum { SECS = 64u };
    uint8_t disk_a[SECS * 512u];
    uint8_t disk_b[SECS * 512u];
    init_tiny_exfat(disk_a, SECS);
    init_tiny_exfat(disk_b, SECS);

    fault_disk_t fa = { disk_a, SECS, -1, 0 };
    fault_disk_t fb = { disk_b, SECS, -1, 0 };
    blkdev_t deva = { fault_read_sectors, fault_write_sectors, SECS, &fa };
    blkdev_t devb = { fault_read_sectors, fault_write_sectors, SECS, &fb };

    exfat_vol_t vol;
    /* Disk A: /FILE.bin uses clusters 4 -> 5 (bytes 0x11, 0x22) */
    assert(exfat_mount(&vol, &deva) == 0);
    exfat_writer_t w;
    uint8_t blk[512];
    assert(exfat_create_file(&vol, "/", "FILE.bin", &w) == 0);
    memset(blk, 0x11, sizeof(blk));
    assert(exfat_write(&w, blk, sizeof(blk)) == 0);
    memset(blk, 0x22, sizeof(blk));
    assert(exfat_write(&w, blk, sizeof(blk)) == 0);
    assert(exfat_close_file(&w) == 0);

    /* Disk B: /FILE.bin starts at cluster 4, then skips cluster 5 (held by /OTHER.bin) to cluster 6 */
    assert(exfat_mount(&vol, &devb) == 0);
    exfat_writer_t wb, wo;
    assert(exfat_create_file(&vol, "/", "FILE.bin", &wb) == 0);
    memset(blk, 0x33, sizeof(blk));
    assert(exfat_write(&wb, blk, sizeof(blk)) == 0); /* allocates cluster 4 */
    assert(exfat_create_file(&vol, "/", "OTHER.bin", &wo) == 0);
    memset(blk, 0xEE, sizeof(blk));
    assert(exfat_write(&wo, blk, sizeof(blk)) == 0); /* allocates cluster 5 */
    assert(exfat_close_file(&wo) == 0);
    memset(blk, 0x66, sizeof(blk));
    assert(exfat_write(&wb, blk, sizeof(blk)) == 0); /* allocates cluster 6 */
    assert(exfat_close_file(&wb) == 0);

    /* Mount disk A into `vol` and read offset 512 to populate vol.rd_cache (first=4, idx=1 -> cluster 5) */
    assert(exfat_mount(&vol, &deva) == 0);
    exfat_entry_t ea;
    assert(exfat_find_path(&vol, "/FILE.bin", &ea) == 0);
    assert(ea.first_cluster == 4u);
    uint8_t byte_a = 0u;
    uint32_t nr = 0u;
    assert(exfat_read_file(&vol, &ea, 512u, &byte_a, 1u, &nr) == 0 && nr == 1u && byte_a == 0x22u);

    /* Remount disk B into the SAME `vol` struct: cache MUST be cleared so offset 512 reads cluster 6 (0x66), not cluster 5 (0xEE) */
    assert(exfat_mount(&vol, &devb) == 0);
    exfat_entry_t eb;
    assert(exfat_find_path(&vol, "/FILE.bin", &eb) == 0);
    assert(eb.first_cluster == 4u);
    uint8_t byte_b = 0u;
    assert(exfat_read_file(&vol, &eb, 512u, &byte_b, 1u, &nr) == 0 && nr == 1u);
    assert(byte_b == 0x66u);

    printf("  [PASS] exfat remount cache invalidation (same first_cluster across two volumes)\n");
}

static void test_exfat_checkpoint_fault_injection(void)
{
    enum { SECS = 96u };
    uint8_t base_disk[SECS * 512u];
    uint8_t work_disk[SECS * 512u];
    init_tiny_exfat(base_disk, SECS);

    fault_disk_t fd = { base_disk, SECS, -1, 0 };
    blkdev_t dev = { fault_read_sectors, fault_write_sectors, SECS, &fd };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    assert(exfat_mkdir(&vol, "record") == 0);

    exfat_writer_t w;
    assert(exfat_create_file(&vol, "/record", "REC_0001.wav", &w) == 0);

    uint8_t hdr[44];
    assert(wav_build_header(hdr, 16000u, 1u, 16u, 0u) == 0);
    assert(exfat_write(&w, hdr, 44u) == 0);

    uint8_t pcm[8148];
    for (uint32_t i = 0u; i < sizeof(pcm); i++) {
        pcm[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }
    /* First 4052 bytes -> total 4096 bytes, checkpoint #1 succeeds */
    assert(exfat_write(&w, pcm, 4052u) == 0);
    assert(wav_build_header(hdr, 16000u, 1u, 16u, 4052u) == 0);
    assert(exfat_checkpoint_file(&w, hdr, 44u) == 0);

    /* Next 4096 bytes -> total 8192 bytes (data_bytes = 8148) */
    assert(exfat_write(&w, pcm + 4052u, 4096u) == 0);
    uint8_t hdr2[44];
    assert(wav_build_header(hdr2, 16000u, 1u, 16u, 8148u) == 0);

    /* Interrupt checkpoint #2 at every write index 0..3 */
    for (int fail_at = 0; fail_at <= 3; fail_at++) {
        memcpy(work_disk, base_disk, sizeof(work_disk));
        fault_disk_t fwork = { work_disk, SECS, fail_at, 0 };
        blkdev_t dwork = { fault_read_sectors, fault_write_sectors, SECS, &fwork };
        exfat_vol_t vwork = vol;
        vwork.dev = &dwork;
        exfat_writer_t wcopy = w;
        wcopy.vol = &vwork;

        (void)exfat_checkpoint_file(&wcopy, hdr2, 44u);

        /* Remount fresh and verify WAV header NEVER exceeds exFAT file size */
        fault_disk_t fchk = { work_disk, SECS, -1, 0 };
        blkdev_t dchk = { fault_read_sectors, fault_write_sectors, SECS, &fchk };
        exfat_vol_t vchk;
        assert(exfat_mount(&vchk, &dchk) == 0);
        exfat_entry_t ent;
        assert(exfat_find_path(&vchk, "/record/REC_0001.wav", &ent) == 0);
        assert(ent.size >= 4096u);

        uint8_t rb[8192];
        uint32_t nr = 0u;
        assert(exfat_read_file(&vchk, &ent, 0u, rb, (uint32_t)ent.size, &nr) == 0);
        assert(nr == (uint32_t)ent.size);

        wav_info_t wi;
        assert(wav_parse_header(rb, &wi) == 0);
        assert(wi.data_bytes + 44u <= ent.size);
        assert(wi.data_bytes == 4052u || wi.data_bytes == 8148u);
        assert(memcmp(rb + 44u, pcm, wi.data_bytes) == 0);
    }

    printf("  [PASS] exfat checkpoint fault injection (WAV header never exceeds file size)\n");
}

static void test_exfat_replace_fault_injection(void)
{
    enum { SECS = 96u };
    uint8_t base_disk[SECS * 512u];
    uint8_t work_disk[SECS * 512u];
    init_tiny_exfat(base_disk, SECS);

    fault_disk_t fd = { base_disk, SECS, -1, 0 };
    blkdev_t dev = { fault_read_sectors, fault_write_sectors, SECS, &fd };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    assert(exfat_mkdir(&vol, "cassini") == 0);

    uint8_t old_data[1024];
    uint8_t new_data[1024];
    memset(old_data, 0x33, sizeof(old_data));
    memset(new_data, 0x77, sizeof(new_data));

    exfat_writer_t w;
    assert(exfat_create_file(&vol, "/cassini", "DEST.bin", &w) == 0);
    assert(exfat_write(&w, old_data, sizeof(old_data)) == 0);
    assert(exfat_close_file(&w) == 0);

    assert(exfat_create_file(&vol, "/cassini", ".tmp.bin", &w) == 0);
    assert(exfat_write(&w, new_data, sizeof(new_data)) == 0);
    assert(exfat_close_file(&w) == 0);

    int saw_old = 0, saw_new = 0;
    for (int fail_at = 0; fail_at <= 6; fail_at++) {
        memcpy(work_disk, base_disk, sizeof(work_disk));
        fault_disk_t fwork = { work_disk, SECS, fail_at, 0 };
        blkdev_t dwork = { fault_read_sectors, fault_write_sectors, SECS, &fwork };
        exfat_vol_t vwork;
        assert(exfat_mount(&vwork, &dwork) == 0);
        (void)exfat_replace_file(&vwork, "/cassini", ".tmp.bin", "DEST.bin");

        fault_disk_t fchk = { work_disk, SECS, -1, 0 };
        blkdev_t dchk = { fault_read_sectors, fault_write_sectors, SECS, &fchk };
        exfat_vol_t vchk;
        assert(exfat_mount(&vchk, &dchk) == 0);

        exfat_entry_t ent;
        assert(exfat_find_path(&vchk, "/cassini/DEST.bin", &ent) == 0);
        assert(ent.size == sizeof(old_data));
        uint8_t rb[1024];
        uint32_t nr = 0u;
        assert(exfat_read_file(&vchk, &ent, 0u, rb, sizeof(rb), &nr) == 0 && nr == sizeof(rb));
        if (memcmp(rb, old_data, sizeof(old_data)) == 0) {
            saw_old = 1;
        } else if (memcmp(rb, new_data, sizeof(new_data)) == 0) {
            saw_new = 1;
            /* If .tmp.bin is still present sharing DEST.bin's clusters, deleting .tmp.bin
             * MUST unlink it without freeing DEST.bin's live clusters. */
            exfat_entry_t tmp_e;
            if (exfat_find_path(&vchk, "/cassini/.tmp.bin", &tmp_e) == 0) {
                assert(exfat_delete_file(&vchk, "/cassini", ".tmp.bin") == 0);
                assert(exfat_find_path(&vchk, "/cassini/DEST.bin", &ent) == 0);
                assert(exfat_read_file(&vchk, &ent, 0u, rb, sizeof(rb), &nr) == 0 && nr == sizeof(rb));
                assert(memcmp(rb, new_data, sizeof(new_data)) == 0);
            }
        } else {
            assert(0 && "DEST.bin corrupted during replace fault injection");
        }
    }
    assert(saw_old && saw_new);

    /* Corrupted FAT cycle on root directory must fail cleanly without infinite loop */
    uint8_t loop_disk[SECS * 512u];
    init_tiny_exfat(loop_disk, SECS);
    put_le32(&loop_disk[16u * 512u + 2u * 4u], 2u); /* FAT[2] -> 2 self-loop */
    for (uint32_t i = 0u; i < 16u; i++) {
        loop_disk[20u * 512u + i * 32u] = 0x05u;    /* Fill cluster 2 with deleted entries so scan follows FAT[2] */
    }
    fault_disk_t floop = { loop_disk, SECS, -1, 0 };
    blkdev_t dloop = { fault_read_sectors, fault_write_sectors, SECS, &floop };
    exfat_vol_t vloop;
    exfat_entry_t lent;
    assert(exfat_mount(&vloop, &dloop) == 0);
    assert(exfat_find_path(&vloop, "/NOPE", &lent) != 0);

    printf("  [PASS] exfat replace fault injection (shared-cluster delete guard + FAT-cycle bound)\n");
}

typedef struct {
    uint8_t *buf;
    uint32_t sectors;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t max_read_cnt;
    uint32_t max_write_cnt;
    uint32_t fail_lba; /* 0xFFFFFFFF = disabled */
    int      fail_active;
} stats_disk_t;

static int stats_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    stats_disk_t *d = (stats_disk_t *)ctx;
    if ((uint64_t)lba + (uint64_t)count > (uint64_t)d->sectors) return -1;
    d->read_calls++;
    if (count > d->max_read_cnt) d->max_read_cnt = count;
    memcpy(buf, &d->buf[lba * 512u], count * 512u);
    return 0;
}

static int stats_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    stats_disk_t *d = (stats_disk_t *)ctx;
    if ((uint64_t)lba + (uint64_t)count > (uint64_t)d->sectors) return -1;
    if (d->fail_active && d->fail_lba >= lba && d->fail_lba < lba + count) {
        return -1;
    }
    d->write_calls++;
    if (count > d->max_write_cnt) d->max_write_cnt = count;
    memcpy(&d->buf[lba * 512u], buf, count * 512u);
    return 0;
}

static uint16_t test_entry_checksum(const uint8_t *ent, uint32_t total_ents)
{
    uint16_t sum = 0u;
    for (uint32_t i = 0u; i < total_ents * 32u; i++) {
        if (i == 2u || i == 3u) continue;
        sum = (uint16_t)(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + ent[i]);
    }
    return sum;
}

static void test_exfat_hardening_and_batching(void)
{
    /* 1. MBR multi-partition scan: partition 0 is 0x07 non-exFAT, partition 1 is 0x07 exFAT */
    enum { SECS = 1024u };
    uint8_t *disk = (uint8_t *)calloc(SECS, 512u);
    assert(disk != NULL);
    disk[510] = 0x55u;
    disk[511] = 0xAAu;
    /* Partition 0: NTFS type 0x07 at LBA 4 (not exFAT) */
    disk[0x1BEu + 4u] = 0x07u;
    put_le32(&disk[0x1BEu + 8u], 4u);
    put_le32(&disk[0x1BEu + 12u], 8u);
    memcpy(&disk[4u * 512u + 3u], "NTFS    ", 8u);
    disk[4u * 512u + 510u] = 0x55u;
    disk[4u * 512u + 511u] = 0xAAu;
    /* Partition 1: exFAT type 0x07 at LBA 16, 4 KiB clusters (spc_shift = 3, 8 sectors/cluster) */
    disk[0x1CEu + 4u] = 0x07u;
    put_le32(&disk[0x1CEu + 8u], 16u);
    put_le32(&disk[0x1CEu + 12u], SECS - 16u);
    uint8_t *vbr = &disk[16u * 512u];
    memcpy(vbr + 3u, "EXFAT   ", 8u);
    put_le64(vbr + 0x40u, 16u);
    put_le64(vbr + 0x48u, SECS - 16u);
    put_le32(vbr + 0x50u, 16u); /* FAT @ LBA 32 */
    put_le32(vbr + 0x54u, 8u);  /* FatLength = 8 sectors */
    put_le32(vbr + 0x58u, 24u); /* Heap @ LBA 40 */
    put_le32(vbr + 0x5Cu, 120u); /* 120 clusters x 4 KiB = 480 KiB */
    put_le32(vbr + 0x60u, 2u);  /* Root dir cluster 2 */
    vbr[0x6Cu] = 9u;
    vbr[0x6Du] = 3u;            /* 8 sectors/cluster = 4 KiB */
    vbr[0x6Eu] = 1u;            /* NumberOfFats = 1 */
    vbr[510]   = 0x55u;
    vbr[511]   = 0xAAu;

    uint8_t *fat = &disk[32u * 512u];
    put_le32(fat + 0u * 4u, 0xFFFFFFF8u);
    put_le32(fat + 1u * 4u, 0xFFFFFFFFu);
    put_le32(fat + 2u * 4u, 0xFFFFFFFFu); /* root */
    put_le32(fat + 3u * 4u, 0xFFFFFFFFu); /* bitmap */
    /* Cluster 4: PC-created NoFatChain=1 directory at slot 15 of root sector 0! */
    /* Cluster 5: ValidDataLength test file (1024 B size, 300 B valid_data_len) */
    put_le32(fat + 5u * 4u, 0xFFFFFFFFu);

    uint8_t *root0 = &disk[40u * 512u];
    root0[0] = 0x81u; /* Allocation Bitmap at cluster 3 */
    put_le32(root0 + 20u, 3u);
    put_le64(root0 + 24u, 16u);

    /* Fill slots 1..14 with deleted markers (0x05) so PCDIR starts at slot 15 (spanning sectors 0 & 1) */
    for (uint32_t sl = 1u; sl < 15u; sl++) {
        root0[sl * 32u] = 0x05u;
    }
    uint8_t pcdir_raw[96];
    memset(pcdir_raw, 0, sizeof(pcdir_raw));
    write_exfat_entry_set(pcdir_raw, "PCDIR", EXFAT_ATTR_DIR, 1u, 4u, 4096u);
    put_le16(pcdir_raw + 2u, test_entry_checksum(pcdir_raw, 3u));
    memcpy(&root0[15u * 32u], pcdir_raw, 32u);        /* 0x85 at sector 0 slot 15 */
    memcpy(&disk[41u * 512u], pcdir_raw + 32u, 64u);  /* 0xC0 + 0xC1 at sector 1 slots 0..1 */

    /* ValidDataLength file at sector 1 slots 2..4 */
    uint8_t vdl_raw[96];
    memset(vdl_raw, 0, sizeof(vdl_raw));
    write_exfat_entry_set(vdl_raw, "VDL.BIN", 0x0020u, 0u, 5u, 1024u);
    put_le64(vdl_raw + 32u + 8u, 300u); /* ValidDataLength = 300 < DataLength = 1024 */
    put_le16(vdl_raw + 2u, test_entry_checksum(vdl_raw, 3u));
    memcpy(&disk[41u * 512u + 2u * 32u], vdl_raw, 96u);

    /* Fill cluster 5 (LBA 40 + 3*8 = 64) with 0x5A for [0..300) and 0xAA for [300..1024) */
    memset(&disk[64u * 512u], 0x5Au, 300u);
    memset(&disk[64u * 512u + 300u], 0xAAu, 724u);

    /* Bitmap at cluster 3 (LBA 48): clusters 2, 3, 4, 5 used -> bits 0..3 = 0x0F */
    disk[48u * 512u] = 0x0Fu;

    stats_disk_t sd = { disk, SECS, 0u, 0u, 0u, 0u, 0xFFFFFFFFu, 0 };
    blkdev_t dev = { stats_read_sectors, stats_write_sectors, SECS, &sd };
    exfat_vol_t vol;
    assert(exfat_mount(&vol, &dev) == 0);
    assert(vol.part_lba == 16u && vol.sectors_per_cluster == 8u);

    /* 2. ValidDataLength zero-fill verification */
    exfat_entry_t vdl_ent;
    assert(exfat_find_path(&vol, "/VDL.BIN", &vdl_ent) == 0);
    assert(vdl_ent.size == 1024u && vdl_ent.valid_data_len == 300u);
    uint8_t vdl_buf[1024];
    memset(vdl_buf, 0xFFu, sizeof(vdl_buf));
    uint32_t nr = 0u;
    assert(exfat_read_file(&vol, &vdl_ent, 0u, vdl_buf, sizeof(vdl_buf), &nr) == 0 && nr == 1024u);
    for (uint32_t i = 0u; i < 300u; i++) assert(vdl_buf[i] == 0x5Au);
    for (uint32_t i = 300u; i < 1024u; i++) assert(vdl_buf[i] == 0x00u);
    memset(vdl_buf, 0xFFu, 200u);
    assert(exfat_read_file(&vol, &vdl_ent, 400u, vdl_buf, 200u, &nr) == 0 && nr == 200u);
    for (uint32_t i = 0u; i < 200u; i++) assert(vdl_buf[i] == 0x00u);

    /* 3. Illegal exFAT filename rejection */
    exfat_writer_t w;
    assert(exfat_mkdir(&vol, ".") != 0);
    assert(exfat_mkdir(&vol, "..") != 0);
    assert(exfat_create_file(&vol, "/", "bad:name.wav", &w) != 0);
    assert(exfat_create_file(&vol, "/", "a*b.wav", &w) != 0);
    assert(exfat_create_file(&vol, "/", "\x01ctrl.wav", &w) != 0);

    /* 4. Multi-sector batching in exfat_write() & exfat_read_file() + cached free_file_clusters() */
    uint8_t big_wr[32u * 4096u]; /* 32 clusters = 128 KiB */
    for (uint32_t i = 0u; i < sizeof(big_wr); i++) {
        big_wr[i] = (uint8_t)((i * 19u + 3u) & 0xFFu);
    }
    assert(exfat_create_file(&vol, "/", "BATCH.BIN", &w) == 0);
    sd.max_write_cnt = 0u;
    assert(exfat_write(&w, big_wr, sizeof(big_wr)) == 0);
    assert(exfat_close_file(&w) == 0);
    assert(sd.max_write_cnt == 8u); /* Full 8-sector cluster written in ONE call! */

    exfat_entry_t batch_ent;
    assert(exfat_find_path(&vol, "/BATCH.BIN", &batch_ent) == 0 && batch_ent.size == sizeof(big_wr));
    uint8_t big_rd[32u * 4096u];
    sd.max_read_cnt = 0u;
    assert(exfat_read_file(&vol, &batch_ent, 0u, big_rd, sizeof(big_rd), &nr) == 0 && nr == sizeof(big_rd));
    assert(sd.max_read_cnt == 8u); /* Full 8-sector cluster read in ONE call! */
    assert(memcmp(big_rd, big_wr, sizeof(big_wr)) == 0);

    sd.write_calls = 0u;
    assert(exfat_delete_file(&vol, "/", "BATCH.BIN") == 0);
    /* Deleting 32 clusters should write dir sector + 1 bitmap sector + 1 FAT sector (3 writes total vs 65 uncached) */
    assert(sd.write_calls <= 6u);

    /* 5. Zero-byte write-error cleanup & mid-file cluster-boundary write-error rollback */
    uint8_t bmp_before = disk[48u * 512u];
    assert(exfat_create_file(&vol, "/", "ZEROERR.BIN", &w) == 0);
    /* Next free cluster is 6 -> LBA 40 + (6-2)*8 = 72 */
    sd.fail_lba    = 72u;
    sd.fail_active = 1;
    uint8_t sec_pat[512];
    memset(sec_pat, 0xCCu, sizeof(sec_pat));
    assert(exfat_write(&w, sec_pat, 512u) != 0);
    assert(w.bytes_written == 0u && w.first_cluster == 0u && w.sec_buf_used == 0u);
    sd.fail_active = 0;
    assert(exfat_close_file(&w) == 0);
    exfat_entry_t zent;
    assert(exfat_find_path(&vol, "/ZEROERR.BIN", &zent) == 0);
    assert(zent.size == 0u && zent.first_cluster == 0u);
    assert(disk[48u * 512u] == bmp_before); /* No leaked cluster bit */
    assert(get_le32(fat + 6u * 4u) == 0u);  /* FAT[6] cleaned up */

    /* Mid-file cluster-boundary failure + partial flush error */
    assert(exfat_create_file(&vol, "/", "MIDERR.BIN", &w) == 0);
    uint8_t clus_buf[4096];
    memset(clus_buf, 0x77u, sizeof(clus_buf));
    assert(exfat_write(&w, clus_buf, 4096u) == 0); /* Writes cluster 6 cleanly */
    assert(exfat_write(&w, clus_buf, 100u) == 0);  /* Buffers 100 B in sec_buf */
    /* Fail sector 0 of cluster 7 (LBA 40 + (7-2)*8 = 80) when partial flush completes 512 B */
    sd.fail_lba    = 80u;
    sd.fail_active = 1;
    assert(exfat_write(&w, clus_buf, 412u) != 0);
    assert(w.sec_buf_used == 100u && w.bytes_written == 4196u);
    assert(get_le32(fat + 6u * 4u) == 0xFFFFFFFFu); /* Cluster 6 EOF restored! */
    assert(get_le32(fat + 7u * 4u) == 0u);          /* Cluster 7 freed! */
    assert((disk[48u * 512u] & (1u << (7u - 2u))) == 0u);
    /* Close while cluster 7 still fails -> drops 100 B tail, commits 4096 B cleanly */
    assert(exfat_close_file(&w) != 0);
    sd.fail_active = 0;
    exfat_entry_t ment;
    assert(exfat_find_path(&vol, "/MIDERR.BIN", &ment) == 0);
    assert(ment.size == 4096u && ment.first_cluster == 6u);
    assert(get_le32(fat + 6u * 4u) == 0xFFFFFFFFu && get_le32(fat + 7u * 4u) == 0u);

    /* 6. Grow PC-created NoFatChain=1 directory whose parent entry starts at slot 15! */
    for (int i = 0; i < 45; i++) {
        char fn[20];
        snprintf(fn, sizeof(fn), "F_%03d.TXT", i);
        assert(exfat_create_file(&vol, "/PCDIR", fn, &w) == 0);
        assert(exfat_write(&w, fn, (uint32_t)strlen(fn)) == 0);
        assert(exfat_close_file(&w) == 0);
    }
    exfat_vol_t vol2;
    assert(exfat_mount(&vol2, &dev) == 0);
    exfat_entry_t pcdir_after;
    assert(exfat_find_path(&vol2, "/PCDIR", &pcdir_after) == 0);
    assert(pcdir_after.is_dir == 1u && pcdir_after.no_fat_chain == 0u && pcdir_after.size == 8192u);
    exfat_entry_t f44;
    assert(exfat_find_path(&vol2, "/PCDIR/F_044.TXT", &f44) == 0);
    char fb[16] = { 0 };
    assert(exfat_read_file(&vol2, &f44, 0u, fb, 16u, &nr) == 0 && nr == 9u);
    assert(memcmp(fb, "F_044.TXT", 9u) == 0);

    free(disk);
    printf("  [PASS] exfat hardening (MBR multi-part, ValidDataLength, batching, write-err rollback, slot-15 NoFatChain grow, cached delete)\n");
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
        uint16_t mps  = (st->max_pkt >= 64u && st->max_pkt <= 512u) ? st->max_pkt : 64u;
        uint32_t sent = 0u;
        while (sent < data_len) {
            uint16_t chunk = ((data_len - sent) > mps) ? mps : (uint16_t)(data_len - sent);
            usb_msc_on_rx_packet(st, out_data + sent, chunk);
            sent += chunk;
        }
    }

    uint32_t got = 0u;
    uint8_t csw[64];
    int got_csw = 0;
    while (usb_msc_need_tx(st)) {
        uint8_t pkt[512];
        uint16_t n = usb_msc_get_tx_packet(st, pkt);
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

    /* 9. READ(10) short 64B-multiple IN (blocks=1 -> 512 B, CBW data_len=1024 B): MUST emit trailing ZLP */
    {
        uint8_t cbw[31];
        uint8_t rd1_cdb[10] = { 0x28u, 0u };
        put_be32(&rd1_cdb[2], 10u);
        put_be16(&rd1_cdb[7], 1u);
        build_cbw(cbw, 10u, 1024u, 0x80u, rd1_cdb, 10u);
        usb_msc_on_rx_packet(&st, cbw, 31u);
        uint32_t data_bytes = 0u;
        int saw_zlp = 0;
        uint8_t pkt[512];
        while (usb_msc_need_tx(&st)) {
            uint16_t n = usb_msc_get_tx_packet(&st, pkt);
            if (!usb_msc_need_tx(&st)) {
                assert(n == 13u && pkt[12] == 0u && get_le32(&pkt[8]) == 512u);
                break;
            }
            if (n == 0u) {
                assert(data_bytes == 512u);
                saw_zlp++;
            } else {
                data_bytes += n;
            }
        }
        assert(data_bytes == 512u && saw_zlp == 1);
    }

    /* 9b. High-Speed (512 B max_pkt) READ(10) + WRITE(10) + 512 B short-transfer ZLP */
    {
        usb_msc_set_max_packet(&st, 512u);
        uint8_t hs_w[1024], hs_r[1024];
        for (uint32_t i = 0u; i < sizeof(hs_w); i++) hs_w[i] = (uint8_t)(0x5Au ^ i);
        uint8_t wr_cdb_hs[10] = { 0x2Au, 0u };
        put_be32(&wr_cdb_hs[2], 12u);
        put_be16(&wr_cdb_hs[7], 2u);
        scsi_xfer(&st, 101u, 0x00u, wr_cdb_hs, 10u, hs_w, NULL, sizeof(hs_w), &status, &res, NULL);
        assert(status == 0u && res == 0u);
        uint8_t rd_cdb_hs[10] = { 0x28u, 0u };
        put_be32(&rd_cdb_hs[2], 12u);
        put_be16(&rd_cdb_hs[7], 2u);
        scsi_xfer(&st, 102u, 0x80u, rd_cdb_hs, 10u, NULL, hs_r, sizeof(hs_r), &status, &res, &got);
        assert(status == 0u && res == 0u && got == sizeof(hs_r));
        assert(memcmp(hs_r, hs_w, sizeof(hs_w)) == 0);
        /* Short 512 B IN on 1024 B CBW must emit a single 512 B packet + ZLP + CSW */
        uint8_t cbw[31], p[512];
        put_be16(&rd_cdb_hs[7], 1u);
        build_cbw(cbw, 103u, 1024u, 0x80u, rd_cdb_hs, 10u);
        usb_msc_on_rx_packet(&st, cbw, 31u);
        assert(usb_msc_get_tx_packet(&st, p) == 512u && usb_msc_need_tx(&st) == 1);
        assert(usb_msc_get_tx_packet(&st, p) == 0u && usb_msc_need_tx(&st) == 1); /* ZLP */
        assert(usb_msc_get_tx_packet(&st, p) == 13u && p[12] == 0u && get_le32(&p[8]) == 512u);
        usb_msc_set_max_packet(&st, 64u);
    }

    /* 10. WRITE(10) Ho > Do (blocks=1 -> 512 B, CBW data_len=1024 B): drains excess 512 B without deadlock */
    {
        uint8_t wr1_cdb[10] = { 0x2Au, 0u };
        put_be32(&wr1_cdb[2], 14u);
        put_be16(&wr1_cdb[7], 1u);
        memset(&g_disk[15u * 512u], 0xEEu, 512u);
        scsi_xfer(&st, 11u, 0x00u, wr1_cdb, 10u, wpat, NULL, 1024u, &status, &res, NULL);
        assert(status == 0u && res == 512u);
        assert(memcmp(&g_disk[14u * 512u], wpat, 512u) == 0);
        assert(g_disk[15u * 512u] == 0xEEu); /* Second sector untouched */
    }

    /* 11. Zero-block READ(10)/WRITE(10) and TUR with data_len > 0 */
    {
        uint8_t rd0_cdb[10] = { 0x28u, 0u };
        scsi_xfer(&st, 12u, 0x80u, rd0_cdb, 10u, NULL, buf, 64u, &status, &res, &got);
        assert(status == 0u && res == 64u && got == 0u);
        uint8_t wr0_cdb[10] = { 0x2Au, 0u };
        scsi_xfer(&st, 13u, 0x00u, wr0_cdb, 10u, wpat, NULL, 64u, &status, &res, NULL);
        assert(status == 0u && res == 64u);
        scsi_xfer(&st, 14u, 0x80u, tur_cdb, 6u, NULL, buf, 8u, &status, &res, &got);
        assert(status == 0u && res == 8u && got == 0u);
    }

    /* 12. Direction mismatch -> CSW Phase Error (2), short cb_len -> status 1 */
    scsi_xfer(&st, 15u, 0x00u, rd_cdb, 10u, wpat, NULL, 512u, &status, &res, NULL);
    assert(status == 2u);
    scsi_xfer(&st, 16u, 0x80u, wr_cdb, 10u, NULL, buf, 512u, &status, &res, &got);
    assert(status == 2u);
    scsi_xfer(&st, 17u, 0x80u, rd_cdb, 6u, NULL, buf, 512u, &status, &res, &got);
    assert(status == 1u && res == 512u);

    printf("  [PASS] usb_msc (SCSI INQUIRY, TUR, READCAP, WRITE10, READ10, ZLP, Ho>Do, PhaseErr, SENSE)\n");
}

typedef struct {
    blkdev_t *dev;
    int       start_calls;
    int       wait_calls;
    int       abort_calls;
    int       fail_wait_on_call;
    int       active;
    uint32_t  lba;
    uint32_t  count;
    void     *buf;
} mock_async_rd_t;

static int mock_async_start(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    mock_async_rd_t *m = (mock_async_rd_t *)ctx;
    m->start_calls++;
    m->active = 1;
    m->lba    = lba;
    m->count  = count;
    m->buf    = buf;
    return 0;
}

static int mock_async_wait(void *ctx)
{
    mock_async_rd_t *m = (mock_async_rd_t *)ctx;
    m->wait_calls++;
    assert(m->active);
    m->active = 0;
    if (m->fail_wait_on_call > 0 && m->wait_calls == m->fail_wait_on_call) {
        return -1;
    }
    return m->dev->read_sectors(m->dev->ctx, m->lba, m->count, m->buf);
}

static void mock_async_abort(void *ctx)
{
    mock_async_rd_t *m = (mock_async_rd_t *)ctx;
    m->abort_calls++;
    m->active = 0;
}

static void test_usb_msc_async_pingpong(void)
{
    blkdev_t dev = { mem_read_sectors, mem_write_sectors, MEM_DISK_SECTORS, NULL };
    /* Fill 40 sectors starting at LBA 20 with known pattern */
    static uint8_t expected[40u * 512u];
    static uint8_t actual[40u * 512u];
    for (uint32_t i = 0u; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)((i * 53u + 17u) & 0xFFu);
    }
    memcpy(&g_disk[20u * 512u], expected, sizeof(expected));

    usb_msc_state_t st;
    usb_msc_init_state(&st, &dev);
    mock_async_rd_t m = { &dev, 0, 0, 0, 0, 0, 0u, 0u, NULL };
    usb_msc_async_read_t ops = { mock_async_start, mock_async_wait, mock_async_abort, &m };
    usb_msc_set_async_read(&st, &ops);

    /* 1. Normal 40-sector READ(10) -> chunk0(16) sync + prefetch(16) + prefetch(8) */
    uint8_t rd_cdb[10] = { 0x28u, 0u };
    put_be32(&rd_cdb[2], 20u);
    put_be16(&rd_cdb[7], 40u);
    uint8_t status = 99u;
    uint32_t res = 99u, got = 0u;
    scsi_xfer(&st, 100u, 0x80u, rd_cdb, 10u, NULL, actual, sizeof(actual), &status, &res, &got);
    assert(status == 0u && res == 0u && got == sizeof(actual));
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
    assert(m.start_calls == 2 && m.wait_calls == 2 && m.abort_calls == 0);

    /* 2. Inject async IDMAC error on 1st prefetch -> must fall back to sync PIO cleanly */
    memset(actual, 0, sizeof(actual));
    m.start_calls = 0; m.wait_calls = 0; m.abort_calls = 0; m.fail_wait_on_call = 1;
    scsi_xfer(&st, 101u, 0x80u, rd_cdb, 10u, NULL, actual, sizeof(actual), &status, &res, &got);
    assert(status == 0u && res == 0u && got == sizeof(actual));
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
    assert(m.start_calls == 2 && m.wait_calls == 2);

    /* 3. Mid-transfer usb_msc_reset() must abort in-flight prefetch */
    m.fail_wait_on_call = 0;
    uint8_t cbw[31];
    build_cbw(cbw, 102u, sizeof(actual), 0x80u, rd_cdb, 10u);
    usb_msc_on_rx_packet(&st, cbw, 31u);
    uint8_t pkt[64];
    assert(usb_msc_get_tx_packet(&st, pkt) == 64u);
    assert(m.active == 1);
    usb_msc_reset(&st);
    assert(m.abort_calls == 1 && m.active == 0);

    /* 4. Short CBW data_len <= buf_len (8192 B) must NOT launch wasted async prefetch */
    m.start_calls = 0; m.wait_calls = 0; m.abort_calls = 0;
    scsi_xfer(&st, 103u, 0x80u, rd_cdb, 10u, NULL, actual, 8192u, &status, &res, &got);
    assert(status == 0u && res == 0u && got == 8192u);
    assert(m.start_calls == 0);

    printf("  [PASS] usb_msc async ping-pong (40-sector overlap, PIO fallback, reset abort, no wasted prefetch)\n");
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

    /* Inline 64 B-multiple short IN (resp_len=64, cbw_data_len=128) MUST emit trailing ZLP */
    vctx.resp_len = 64u;
    {
        uint8_t zcbw[31];
        build_cbw(zcbw, 1035u, 128u, 0x80u, v_in_cdb, 16u);
        usb_msc_on_rx_packet(&st, zcbw, 31u);
        uint8_t p[64];
        assert(usb_msc_get_tx_packet(&st, p) == 64u && usb_msc_need_tx(&st) == 1);
        assert(usb_msc_get_tx_packet(&st, p) == 0u && usb_msc_need_tx(&st) == 1); /* ZLP */
        assert(usb_msc_get_tx_packet(&st, p) == 13u && p[12] == 0u && get_le32(&p[8]) == 64u);
        assert(usb_msc_need_tx(&st) == 0);
    }

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
        .reserved0 = 0u, .mic_gain = 27u, .sound_mode = AUDIO_DSP_ASR_RAW, .channels = 1u,
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
    assert(cfg_load.reserved0 == 0u && cfg_load.mic_gain == 31u && cfg_load.sound_mode == AUDIO_DSP_VOICE_CLARITY);
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

    /* Crash-safe checkpoint test: stream 16 KiB, checkpoint WITHOUT closing, remount & verify */
    exfat_writer_t cw;
    assert(exfat_create_file(&vol3, "/record", "CKPT_TEST.wav", &cw) == 0);
    uint8_t ckpt_hdr[44];
    assert(wav_build_header(ckpt_hdr, 16000u, 1u, 16u, 0u) == 0);
    assert(exfat_write(&cw, ckpt_hdr, 44u) == 0);
    assert(exfat_write(&cw, pcm, (uint32_t)sizeof(pcm)) == 0);
    uint64_t flushed = cw.bytes_written - (uint64_t)cw.sec_buf_used;
    assert(flushed > 44u);
    assert(wav_build_header(ckpt_hdr, 16000u, 1u, 16u, (uint32_t)(flushed - 44u)) == 0);
    assert(exfat_checkpoint_file(&cw, ckpt_hdr, 44u) == 0);
    /* Simulate power loss: do NOT call exfat_close_file(&cw); remount fresh volume */
    exfat_vol_t vol4;
    assert(exfat_mount(&vol4, &dev) == 0);
    exfat_entry_t ck_ent;
    assert(exfat_find_path(&vol4, "/record/CKPT_TEST.wav", &ck_ent) == 0);
    assert(ck_ent.first_cluster >= 2u && ck_ent.size == flushed);
    uint8_t ck_hdr_rb[44];
    uint32_t ck_got = 0u;
    assert(exfat_read_file(&vol4, &ck_ent, 0u, ck_hdr_rb, 44u, &ck_got) == 0 && ck_got == 44u);
    wav_info_t ck_wi;
    assert(wav_parse_header(ck_hdr_rb, &ck_wi) == 0 && ck_wi.data_bytes == (uint32_t)(flushed - 44u));

    /* Atomic replace test: stage temp file and replace CKPT_TEST.wav */
    exfat_writer_t tw;
    assert(exfat_create_file(&vol4, "/record", "STAGED.tmp", &tw) == 0);
    const char *staged_msg = "ATOMIC_REPLACE_OK";
    assert(exfat_write(&tw, staged_msg, 17u) == 0);
    assert(exfat_close_file(&tw) == 0);
    assert(exfat_replace_file(&vol4, "/record", "STAGED.tmp", "CKPT_TEST.wav") == 0);
    exfat_entry_t rep_ent;
    assert(exfat_find_path(&vol4, "/record/STAGED.tmp", &rep_ent) != 0);
    assert(exfat_find_path(&vol4, "/record/CKPT_TEST.wav", &rep_ent) == 0);
    assert(rep_ent.size == 17u);
    char rep_buf[20] = { 0 };
    assert(exfat_read_file(&vol4, &rep_ent, 0u, rep_buf, 17u, &ck_got) == 0 && ck_got == 17u);
    assert(memcmp(rep_buf, staged_msg, 17u) == 0);
    assert(exfat_delete_file(&vol4, "/record", "CKPT_TEST.wav") == 0);

    /* Exercise cached multi-cluster deletion (16 clusters = 64 KiB) for fsck.exfat verification */
    {
        exfat_writer_t bw;
        assert(exfat_create_file(&vol4, "/record", "BIGDEL.BIN", &bw) == 0);
        for (int k = 0; k < 4; k++) {
            assert(exfat_write(&bw, pcm, (uint32_t)sizeof(pcm)) == 0);
        }
        assert(exfat_close_file(&bw) == 0);
        assert(exfat_delete_file(&vol4, "/record", "BIGDEL.BIN") == 0);
    }

    /* Simulate PC-created NoFatChain=1 directory (/pcdir) growing past 1 cluster -> fsck.exfat verified! */
    {
        assert(exfat_mkdir(&vol4, "pcdir") == 0);
        exfat_entry_t pcdir_ent;
        assert(exfat_find_path(&vol4, "/pcdir", &pcdir_ent) == 0 && pcdir_ent.is_dir);
        /* Flip pcdir's root directory entry to NoFatChain=1 and clear its FAT entry (like Windows/macOS) */
        uint8_t rsec[512];
        uint32_t root_lba = vol4.heap_lba + ((vol4.root_dir_cluster - 2u) << vol4.spc_shift);
        int patched = 0;
        for (uint32_t s = 0u; s < vol4.sectors_per_cluster && !patched; s++) {
            assert(file_read_sectors(fp, root_lba + s, 1u, rsec) == 0);
            for (uint32_t sl = 0u; sl + 2u < 16u; sl++) {
                uint8_t *e = &rsec[sl * 32u];
                if (e[0] == 0x85u && e[32u] == 0xC0u && get_le32(e + 32u + 20u) == pcdir_ent.first_cluster) {
                    e[32u + 1u] = 0x03u; /* AllocationPossible=1, NoFatChain=1 */
                    put_le16(e + 2u, test_entry_checksum(e, 1u + (uint32_t)e[1]));
                    assert(file_write_sectors(fp, root_lba + s, 1u, rsec) == 0);
                    patched = 1;
                    break;
                }
            }
        }
        assert(patched == 1);
        uint32_t fsec_lba = vol4.fat_lba + (pcdir_ent.first_cluster >> 7);
        assert(file_read_sectors(fp, fsec_lba, 1u, rsec) == 0);
        put_le32(rsec + ((pcdir_ent.first_cluster & 127u) << 2), 0u);
        assert(file_write_sectors(fp, fsec_lba, 1u, rsec) == 0);

        /* Create 45 files inside /pcdir (> 42-file 1-cluster ceiling) */
        for (int i = 0; i < 45; i++) {
            char fn[20];
            snprintf(fn, sizeof(fn), "PC_%03d.TXT", i);
            exfat_writer_t pw;
            assert(exfat_create_file(&vol4, "/pcdir", fn, &pw) == 0);
            assert(exfat_write(&pw, fn, (uint32_t)strlen(fn)) == 0);
            assert(exfat_close_file(&pw) == 0);
        }
        /* Clean up the 45 files to keep directory/file count tidy while leaving expanded /pcdir for fsck */
        for (int i = 0; i < 45; i++) {
            char fn[20];
            snprintf(fn, sizeof(fn), "PC_%03d.TXT", i);
            assert(exfat_delete_file(&vol4, "/pcdir", fn) == 0);
        }
        /* Also test zero-byte file close (FirstCluster == 0, DataLength == 0) */
        exfat_writer_t zw;
        assert(exfat_create_file(&vol4, "/pcdir", "EMPTY.TXT", &zw) == 0);
        assert(exfat_close_file(&zw) == 0);
        assert(exfat_delete_file(&vol4, "/pcdir", "EMPTY.TXT") == 0);
    }

    fclose(fp);
    printf("  [PASS] exfat-img (%s: 16,428 B multi-cluster WAV + checkpoint + atomic replace + NoFatChain grow + settings.bin verified)\n", path);
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

static uint16_t mock_xrec_get_mtu(void *user)
{
    sim_phone_t *sp = (sim_phone_t *)user;
    return bt_spp_get_mtu(sp->spp);
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

typedef struct {
    uint32_t read_calls;
    int      fail_writes;
} xrec_disk_stats_t;

static int xrec_count_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    xrec_disk_stats_t *st = (xrec_disk_stats_t *)ctx;
    st->read_calls++;
    return mem_read_sectors(NULL, lba, count, buf);
}

static int xrec_count_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    xrec_disk_stats_t *st = (xrec_disk_stats_t *)ctx;
    if (st->fail_writes) {
        return -1;
    }
    return mem_write_sectors(NULL, lba, count, buf);
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

    xrec_disk_stats_t dstats = { 0u, 0 };
    blkdev_t dev = { xrec_count_read_sectors, xrec_count_write_sectors, MEM_DISK_SECTORS, &dstats };
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
        mock_xrec_get_mtu,
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
    /* Spoofed ACL handle (0x0042 != 0x0041) MUST be silently dropped */
    const char *c_spoof = "+XREC:DEL,REC_0001.wav\r";
    send_rfcomm_client_uih(&spp, 0x0042u, 2u, (const uint8_t *)c_spoof, (uint16_t)strlen(c_spoof));
    drain_acl_to_phone(&spp, &sp);
    assert(sp.rx_len == 0u);

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

    /* 8. Edge-case hardening tests: overflow, short/odd pair/hex, calendar RTC, bad names, overlong line */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 20u);
    sp.rx_len = 0u;
    const char *c_ovf = "+XREC:GETS,4294967296,16,16,REC_0001.wav\r"
                        "+XREC:RTC,2026,2,29,12,0,0,0\r"
                        "+XREC:RTC,2026,4,31,12,0,0,0\r"
                        "+XREC:RTC,2024,2,29,12,0,0,3\r"
                        "+XREC:DEL,bad:name.wav\r"
                        "+XREC:DEL,trail.\r"
                        "+XREC:DEL,trail \r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ovf, (uint16_t)strlen(c_ovf));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,ARGS\r\n") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,RTCRNG\r\n+XREC:ERR,RTCRNG\r\n+XREC:OK,RTC\r\n") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,NAME\r\n+XREC:ERR,NAME\r\n+XREC:ERR,NAME\r\n") != NULL);

    /* Short/odd PAIR & AUTH inputs (ASan checks no OOB reads past NUL) + PIN zeroed on close */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 20u);
    sp.rx_len = 0u;
    xrec_open_pairing(&xrec, "123456");
    const char *c_short_pair = "+XREC:PAIR,12\r+XREC:PAIR,123456,\r+XREC:PAIR,123456,abc\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_short_pair, (uint16_t)strlen(c_short_pair));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,PIN\r\n+XREC:ERR,BADKEY\r\n+XREC:ERR,BADKEY\r\n") != NULL);
    xrec_close_pairing(&xrec);
    assert(xrec.pair_active == 0u && xrec.pair_pin[0] == '\0');

    sp.rx_len = 0u;
    const char *c_short_auth = "+XREC:CHAL\r+XREC:AUTH,abc\r+XREC:CHAL\r+XREC:AUTH,\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_short_auth, (uint16_t)strlen(c_short_auth));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,AUTH\r\n") != NULL);

    /* Overlong rx_line (>= 192 chars) rejects with +XREC:ERR,LINE and recovers cleanly */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    sp.rx_len = 0u;
    char long_line[256];
    memcpy(long_line, "+XREC:PING,", 11u);
    memset(long_line + 11u, 'A', 200u);
    memcpy(long_line + 211u, ":+XREC:PUT,0,1,0,OTA:\r+XREC:PING\r", 33u);
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)long_line, 244u);
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,LINE\r\n") != NULL);
    assert(strstr((const char *)sp.rx_stream, "+XREC:PONG,openfw_v5") != NULL);
    assert(xrec.put_active == 0u);

    /* 9. GETS busy-retry caches frame (zero extra SD reads), concurrent LS cancels GETS, PUT SDIO fail aborts */
    while (spp.peer_credits > 1u) {
        send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ping, (uint16_t)strlen(c_ping));
        drain_acl_to_phone(&spp, &sp);
    }
    assert(spp.peer_credits == 1u);
    sp.rx_len = 0u;
    const char *c_gets_cache = "+XREC:GETS,0,1024,256,REC_0001.wav\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_gets_cache, (uint16_t)strlen(c_gets_cache));
    drain_acl_to_phone(&spp, &sp);
    assert(sp.rx_len == 12u + 256u && xrec.gets_active == 1u && xrec.gets_staged == 1u);

    /* Calling xrec_poll() 5 times while stalled at 0 credits must perform ZERO extra SD reads */
    uint32_t reads_while_stalled = dstats.read_calls;
    for (int retry = 0; retry < 5; retry++) {
        xrec_poll(&xrec);
    }
    assert(dstats.read_calls == reads_while_stalled);

    /* Grant 1 credit -> sends the cached 2nd frame without re-reading SD first */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 1u);
    xrec_poll(&xrec);
    drain_acl_to_phone(&spp, &sp);
    assert(sp.rx_len == 2u * (12u + 256u) && xrec.gets_active == 1u);
    assert(memcmp(&sp.rx_stream[12u + 256u + 12u], &wav_file[256], 256u) == 0);

    /* Concurrent LS while GETS is still active stops GETS cleanly without interleaving */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    sp.rx_len = 0u;
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ls, (uint16_t)strlen(c_ls));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(xrec.gets_active == 0u && xrec.gets_staged == 0u && xrec.ls_active == 0u);
    assert(strstr((const char *)sp.rx_stream, "+XREC:END\r\n") != NULL);
    assert(memcmp(sp.rx_stream, "+XREC:F,", 8u) == 0);

    /* PUT exfat_write() failure aborts staged upload so subsequent PUTEND fails with NOPUT */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    sp.rx_len = 0u;
    uint8_t big_chunk[600];
    memset(big_chunk, 0xABu, sizeof(big_chunk));
    uint16_t big_crc = openfw_crc16_ccitt_false(big_chunk, sizeof(big_chunk));
    send_xrec_put_target(&spp, 0u, "CAS,fail.cwx", big_chunk, sizeof(big_chunk), big_crc);
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(strstr((const char *)sp.rx_stream, "+XREC:OK,PUT,0,600") != NULL);
    assert(xrec.put_stream_mode == XREC_PUT_CAS);

    sp.rx_len = 0u;
    dstats.fail_writes = 1;
    send_xrec_put_target(&spp, 600u, "CAS,fail.cwx", big_chunk, sizeof(big_chunk), big_crc);
    dstats.fail_writes = 0;
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_put_end, (uint16_t)strlen(c_put_end));
    drain_acl_to_phone(&spp, &sp);
    sp.rx_stream[sp.rx_len] = '\0';
    assert(xrec.put_stream_mode == XREC_PUT_NONE);
    assert(strstr((const char *)sp.rx_stream, "+XREC:ERR,SDIO\r\n+XREC:ERR,NOPUT\r\n") != NULL);

    /* 10. bt_spp_peek_acl_tx / consume, control-slot reserve, RFCOMM credit deadlock recovery, DM on unknown DLCI */
    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 30u);
    drain_acl_to_phone(&spp, &sp);
    assert(spp.tx_count == 0u);
    assert(bt_spp_peek_acl_tx(&spp, NULL, NULL) == NULL);

    /* Unknown DLCI SABM (e.g. DLCI 6) -> must reply DM (0x1F) */
    send_rfcomm_client_ctrl(&spp, 0x0041u, 6u, 0x3Fu);
    uint16_t peek_hdl = 0u, peek_len = 0u;
    const uint8_t *peek_p = bt_spp_peek_acl_tx(&spp, &peek_hdl, &peek_len);
    assert(peek_p != NULL && peek_hdl == 0x0080u && peek_len == 8u);
    assert(peek_p[4] == ((6u << 2) | 0x03u) && peek_p[5] == 0x1Fu); /* DM */
    /* Second peek returns identical pointer without consuming */
    assert(bt_spp_peek_acl_tx(&spp, NULL, NULL) == peek_p);
    bt_spp_consume_acl_tx(&spp);
    assert(spp.tx_count == 0u);

    /* Fill data queue up to BT_SPP_TX_Q_CAP - BT_SPP_TX_CTRL_RESERVE -> data rejects, control succeeds */
    const uint8_t tiny[4] = { 'T', 'E', 'S', 'T' };
    for (uint8_t k = 0u; k < (BT_SPP_TX_Q_CAP - BT_SPP_TX_CTRL_RESERVE); k++) {
        assert(bt_spp_send(&spp, tiny, 4u) == 0);
    }
    assert(spp.tx_count == (BT_SPP_TX_Q_CAP - BT_SPP_TX_CTRL_RESERVE));
    assert(bt_spp_send(&spp, tiny, 4u) == -1); /* Data slots full; 2 control slots reserved */

    /* Fill remaining 2 control slots with L2CAP Echo Requests so queue is 100% full (BT_SPP_TX_Q_CAP) */
    uint8_t echo_req[4] = { 0x08u, 9u, 0u, 0u };
    for (uint8_t k = 0u; k < BT_SPP_TX_CTRL_RESERVE; k++) {
        echo_req[1] = (uint8_t)(10u + k);
        send_l2cap_client(&spp, 0x0001u, echo_req, 4u);
    }
    assert(spp.tx_count == BT_SPP_TX_Q_CAP);

    /* Drain local_credits to <= 2 while TX queue is 100% full: credit grant MUST defer (no false bump to 7) */
    while (spp.local_credits > 3u) {
        send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)"\n", 1u);
    }
    assert(spp.local_credits == 3u && spp.local_credit_pending == 0u);
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)"\n", 1u);
    assert(spp.local_credits == 2u && spp.local_credit_pending == 1u); /* Deferred, NOT stuck at 7! */

    /* Consume 1 slot -> pending credit replenishment MUST enqueue immediately and restore local_credits = 7 */
    bt_spp_consume_acl_tx(&spp);
    assert(spp.local_credits == 7u && spp.local_credit_pending == 0u);
    assert(spp.tx_count == BT_SPP_TX_Q_CAP);
    drain_acl_to_phone(&spp, &sp);

    /* 11. Small-MTU (48 B) GETS via L2CAP RFCOMM reconnect -> clamps payload to 36 B without deadlock */
    sp.rx_len = 0u;
    uint8_t conn_rf2[8] = { 0x02u, 20u, 4u, 0u, 0x03u, 0x00u, 0x02u, 0x01u };
    send_l2cap_client(&spp, 0x0001u, conn_rf2, 8u);
    assert(xrec.auth == 0u); /* Reconnect notified disconnect */
    assert(spp.rfcomm.cfg_rx_done == 0u && spp.rfcomm.cfg_tx_done == 0u);
    uint8_t cfg_rf_small[12] = { 0x04u, 21u, 8u, 0u, 0x41u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x35u, 0x00u }; /* L2CAP MTU 53 -> rfcomm_mtu=48 */
    send_l2cap_client(&spp, 0x0001u, cfg_rf_small, 12u);
    send_rfcomm_client_ctrl(&spp, 0x0041u, 0u, 0x3Fu); /* SABM DLCI 0 */
    uint8_t pn_small[10] = { 0x83u, 0x11u, 0x02u, 0xF0u, 0x00u, 0x00u, 0x30u, 0x00u, 0x00u, 0x07u }; /* MTU 48, k=7 */
    send_rfcomm_client_uih(&spp, 0x0041u, 0u, pn_small, sizeof(pn_small));
    /* Pre-SABM UIH on DLCI 2 must be ignored because ch1_open == 0 */
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_ping, (uint16_t)strlen(c_ping));
    drain_acl_to_phone(&spp, &sp);
    assert(bt_spp_is_open(&spp) == 0 && sp.rx_len == 0u);
    assert(bt_spp_get_mtu(&spp) == 48u);

    send_rfcomm_client_ctrl(&spp, 0x0041u, 2u, 0x3Fu); /* SABM DLCI 2 */
    drain_acl_to_phone(&spp, &sp);
    assert(bt_spp_is_open(&spp) == 1);

    /* Re-authenticate & stream 120 B of REC_0001.wav requesting 480 B chunks over 48 B MTU */
    sp.rx_len = 0u;
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_chal, (uint16_t)strlen(c_chal));
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_auth, (uint16_t)strlen(c_auth));
    drain_acl_to_phone(&spp, &sp);
    assert(xrec.auth == 1u);

    send_rfcomm_client_credits(&spp, 0x0041u, 2u, 10u);
    sp.rx_len = 0u;
    const char *c_gets_small = "+XREC:GETS,0,120,480,REC_0001.wav\r";
    send_rfcomm_client_uih(&spp, 0x0041u, 2u, (const uint8_t *)c_gets_small, (uint16_t)strlen(c_gets_small));
    drain_acl_to_phone(&spp, &sp);
    assert(xrec.gets_active == 0u);
    assert(xrec.gets_chunk_max == 36u); /* 48 - 12 = 36 B payload per frame */
    uint8_t sm_dl[120];
    uint32_t sm_dpos = 0u, sm_ppos = 0u, sm_frames = 0u;
    while (sm_ppos + 12u <= sp.rx_len) {
        assert(memcmp(&sp.rx_stream[sm_ppos], "XREC", 4u) == 0);
        uint16_t flen = (uint16_t)sp.rx_stream[sm_ppos + 8u] | ((uint16_t)sp.rx_stream[sm_ppos + 9u] << 8);
        uint16_t fcrc = (uint16_t)sp.rx_stream[sm_ppos + 10u] | ((uint16_t)sp.rx_stream[sm_ppos + 11u] << 8);
        assert(flen <= 36u);
        assert(openfw_crc16_ccitt_false(&sp.rx_stream[sm_ppos + 12u], flen) == fcrc);
        memcpy(&sm_dl[sm_dpos], &sp.rx_stream[sm_ppos + 12u], flen);
        sm_dpos += flen;
        sm_ppos += 12u + flen;
        sm_frames++;
    }
    assert(sm_ppos == sp.rx_len && sm_dpos == 120u && sm_frames == 4u);
    assert(memcmp(sm_dl, wav_file, 120u) == 0);

    printf("  [PASS] bt_spp + xrec (L2CAP, SDP, RFCOMM credits/DISC/DM/peek, +XREC auth/LS/DEL/GET/GETS/small-MTU/PUT/OTA/EXP/LOG/CAS/DFU)\n");
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
    test_exfat_remount_cache_invalidation();
    test_exfat_checkpoint_fault_injection();
    test_exfat_replace_fault_injection();
    test_exfat_hardening_and_batching();
    test_usb_msc();
    test_usb_msc_async_pingpong();
    test_usb_msc_vendor_e0();
    test_bt_spp_and_xrec();
    printf("All open_fw host unit tests PASSED!\n");
    return 0;
}
