/* SBC encoder wrapper for headphones mode (docs/BT_A2DP_DESIGN.md §4.2). One static SBC_ENC_PARAMS over the vendored
 * Bluedroid encoder (third_party/bluedroid/host/bluedroid/external/sbc/encoder), configured from the negotiated A2DP
 * codec configuration. Pure C; hp_sbc.c is built with the Bluedroid flags and bt_port_prefix.h so the encoder struct
 * layout matches the vendored objects. Single instance, single caller (the BTC tick, or a host test).
 * SPDX-License-Identifier: MIT */
#ifndef HP_SBC_H
#define HP_SBC_H
#include <stdint.h>

/* Channel modes and allocation methods use the encoder's values (SBC_MONO .. SBC_JOINT_STEREO, SBC_LOUDNESS/SNR). */
#define HP_SBC_MONO         0u
#define HP_SBC_DUAL         1u
#define HP_SBC_STEREO       2u
#define HP_SBC_JOINT_STEREO 3u
#define HP_SBC_LOUDNESS     0u
#define HP_SBC_SNR          1u
#define HP_SBC_MAX_BITPOOL  53u   /* highest bitpool this firmware ever advertises (A2DP high-quality profile) */

typedef struct {
    uint32_t rate;        /* 16000, 32000, 44100 or 48000 */
    uint8_t  ch_mode;     /* HP_SBC_MONO .. HP_SBC_JOINT_STEREO */
    uint8_t  blocks;      /* 4, 8, 12, 16 */
    uint8_t  subbands;    /* 4, 8 */
    uint8_t  alloc;       /* HP_SBC_LOUDNESS or HP_SBC_SNR */
    uint8_t  min_bp;      /* negotiated bitpool range */
    uint8_t  max_bp;
} hp_sbc_cfg_t;

/* Parse an A2DP SBC codec information element (4 bytes as on the air and in esp_a2d_cie_sbc_t: freq|ch_mode,
 * blocks|subbands|alloc, min bitpool, max bitpool) holding ONE configured value per field. 0 = ok, -1 = invalid. */
int      hp_sbc_cfg_from_cie(const uint8_t cie[4], hp_sbc_cfg_t *out);
/* 0 = ok, -1 = invalid configuration. Resets the analysis filter history. */
int      hp_sbc_init(const hp_sbc_cfg_t *cfg);
/* Frame length in bytes at a given bitpool (A2DP spec 12.9), 0 if invalid. */
uint32_t hp_sbc_frame_len(const hp_sbc_cfg_t *cfg, uint8_t bitpool);
/* PCM frames (samples per channel) per SBC frame: blocks * subbands. */
uint32_t hp_sbc_pcm_per_frame(const hp_sbc_cfg_t *cfg);
/* Number of input channels the encoder takes (1 for mono, else 2). */
uint8_t  hp_sbc_channels(const hp_sbc_cfg_t *cfg);
/* The encoder's PCM input for the next frame: hp_sbc_pcm_per_frame() frames, interleaved, hp_sbc_channels() wide.
 * Fill it, then call hp_sbc_encode(). */
int16_t *hp_sbc_pcm_buf(void);
/* Encode the PCM in hp_sbc_pcm_buf() into out (>= hp_sbc_frame_len(cfg, bitpool) bytes) at the given bitpool, which
 * is clamped to the configured [min_bp, max_bp]. Returns the bytes written, 0 if not initialised. */
uint32_t hp_sbc_encode(uint8_t bitpool, uint8_t *out);
#endif
