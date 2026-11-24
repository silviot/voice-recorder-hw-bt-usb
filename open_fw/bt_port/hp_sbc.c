/* SBC encoder wrapper (docs/BT_A2DP_DESIGN.md §4.2). Built with the Bluedroid flags and -include bt_port_prefix.h
 * (bt/Makefile.bluedroid, tests), so SBC_ENC_PARAMS has the layout the vendored encoder objects were built with.
 * SPDX-License-Identifier: MIT */
#include "hp_sbc.h"
#include <string.h>
#include "common/bt_target.h"
#include "sbc_encoder.h"

#if !(defined(SBC_ENC_INCLUDED) && SBC_ENC_INCLUDED == TRUE)
#error "hp_sbc.c needs the SBC encoder (SBC_ENC_INCLUDED, bt_port_prefix.h)"
#endif
/* One frame per SBC_Encoder() call and the PCM buffer inside the params (what hp_sbc_pcm_buf() hands out). */
_Static_assert(SBC_MAX_NUM_FRAME == 1, "hp_sbc encodes one frame per call");
_Static_assert(SBC_NO_PCM_CPY_OPTION == FALSE, "hp_sbc fills as16PcmBuffer");
_Static_assert(SBC_JOINT_STE_INCLUDED == TRUE, "joint stereo is the default A2DP configuration");

static SBC_ENC_PARAMS s_enc;   /* ~1.7 KB, never on the BTC stack */
static hp_sbc_cfg_t   s_cfg;
static int            s_ready;

static int rate_code(uint32_t rate)
{
    switch (rate) {
    case 16000: return SBC_sf16000;
    case 32000: return SBC_sf32000;
    case 44100: return SBC_sf44100;
    case 48000: return SBC_sf48000;
    default:    return -1;
    }
}

static int cfg_valid(const hp_sbc_cfg_t *c)
{
    if (c == NULL || rate_code(c->rate) < 0 || c->ch_mode > HP_SBC_JOINT_STEREO || c->alloc > HP_SBC_SNR) {
        return 0;
    }
    if (!(c->blocks == 4 || c->blocks == 8 || c->blocks == 12 || c->blocks == 16) ||
        !(c->subbands == 4 || c->subbands == 8)) {
        return 0;
    }
    /* A2DP: 2 <= bitpool <= 250; mono/dual <= 16 * subbands, stereo/joint <= 32 * subbands. */
    uint32_t bp_cap = (c->ch_mode <= HP_SBC_DUAL ? 16u : 32u) * c->subbands;
    return c->min_bp >= 2 && c->min_bp <= c->max_bp && c->max_bp <= bp_cap;
}

int hp_sbc_cfg_from_cie(const uint8_t cie[4], hp_sbc_cfg_t *out)
{
    if (cie == NULL || out == NULL) {
        return -1;
    }
    hp_sbc_cfg_t c;
    memset(&c, 0, sizeof(c));
    switch (cie[0] & 0xF0u) {
    case 0x80: c.rate = 16000; break;
    case 0x40: c.rate = 32000; break;
    case 0x20: c.rate = 44100; break;
    case 0x10: c.rate = 48000; break;
    default: return -1;
    }
    switch (cie[0] & 0x0Fu) {
    case 0x08: c.ch_mode = HP_SBC_MONO; break;
    case 0x04: c.ch_mode = HP_SBC_DUAL; break;
    case 0x02: c.ch_mode = HP_SBC_STEREO; break;
    case 0x01: c.ch_mode = HP_SBC_JOINT_STEREO; break;
    default: return -1;
    }
    switch (cie[1] & 0xF0u) {
    case 0x80: c.blocks = 4; break;
    case 0x40: c.blocks = 8; break;
    case 0x20: c.blocks = 12; break;
    case 0x10: c.blocks = 16; break;
    default: return -1;
    }
    switch (cie[1] & 0x0Cu) {
    case 0x08: c.subbands = 4; break;
    case 0x04: c.subbands = 8; break;
    default: return -1;
    }
    switch (cie[1] & 0x03u) {
    case 0x02: c.alloc = HP_SBC_SNR; break;
    case 0x01: c.alloc = HP_SBC_LOUDNESS; break;
    default: return -1;
    }
    c.min_bp = cie[2];
    c.max_bp = cie[3];
    if (!cfg_valid(&c)) {
        return -1;
    }
    *out = c;
    return 0;
}

uint32_t hp_sbc_frame_len(const hp_sbc_cfg_t *c, uint8_t bp)
{
    if (c == NULL || !(c->subbands == 4 || c->subbands == 8) || c->blocks == 0) {
        return 0;
    }
    uint32_t nch = (c->ch_mode == HP_SBC_MONO) ? 1u : 2u;
    uint32_t len = 4u + (4u * c->subbands * nch) / 8u;
    uint32_t bits;
    switch (c->ch_mode) {
    case HP_SBC_MONO:
    case HP_SBC_DUAL:   bits = (uint32_t)c->blocks * nch * bp; break;
    case HP_SBC_STEREO: bits = (uint32_t)c->blocks * bp; break;
    default:            bits = c->subbands + (uint32_t)c->blocks * bp; break;
    }
    return len + (bits + 7u) / 8u;
}

uint32_t hp_sbc_pcm_per_frame(const hp_sbc_cfg_t *c)
{
    return c ? (uint32_t)c->blocks * c->subbands : 0u;
}

uint8_t hp_sbc_channels(const hp_sbc_cfg_t *c)
{
    return (c && c->ch_mode == HP_SBC_MONO) ? 1u : 2u;
}

int hp_sbc_init(const hp_sbc_cfg_t *c)
{
    s_ready = 0;
    if (!cfg_valid(c)) {
        return -1;
    }
    memset(&s_enc, 0, sizeof(s_enc));
    s_enc.s16SamplingFreq     = (SINT16)rate_code(c->rate);
    s_enc.s16ChannelMode      = (SINT16)c->ch_mode;
    s_enc.s16NumOfSubBands    = (SINT16)c->subbands;
    s_enc.s16NumOfBlocks      = (SINT16)c->blocks;
    s_enc.s16AllocationMethod = (SINT16)c->alloc;
    s_enc.sbc_mode            = SBC_MODE_STD;
    s_enc.u16BitRate          = 229;   /* only feeds Init's bitpool guess; overwritten per frame below */
    SBC_Encoder_Init(&s_enc);
    s_enc.s16BitPool = (SINT16)c->max_bp;
    s_cfg = *c;
    s_ready = 1;
    return 0;
}

int16_t *hp_sbc_pcm_buf(void)
{
    return s_enc.as16PcmBuffer;
}

uint32_t hp_sbc_encode(uint8_t bp, uint8_t *out)
{
    if (!s_ready || out == NULL) {
        return 0;
    }
    if (bp < s_cfg.min_bp) {
        bp = s_cfg.min_bp;
    }
    if (bp > s_cfg.max_bp) {
        bp = s_cfg.max_bp;
    }
    /* SBC_Encoder reads s16BitPool for every frame (sbc_packing.c, sbc_enc_bit_alloc_*.c): no re-init needed. */
    s_enc.s16BitPool = (SINT16)bp;
    s_enc.pu8Packet = out;
    s_enc.u8NumPacketToEncode = 1;
    SBC_Encoder(&s_enc);
    return s_enc.u16PacketLength;
}
