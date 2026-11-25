/*
 * Cassini OpenFW — Headset Button Probe (hardware side, modal Settings entry)
 *
 * See include/openfw_hsprobe.h for the experiment design. This file only
 * touches MMIO; every register it changes is saved first, restored at the
 * end, and compared in a self-check written to the log.
 *
 * SARADC/KADC layout (verified from the stock FIRM KADC init at 0x00D510B8):
 *   +0x00 CTRL   bit n = channel n enable (n = 0..9), 0x80180000 base config
 *   +0x04 START  0xA800 = continuous scan
 *   +0x0C IE, +0x10 per-channel nibbles ch0..7, +0x18 nibbles ch8..9
 *   +0x20 + 0x10*n  THR of channel n   (ch1 = +0x30, matches keys.c)
 *   +0x24 + 0x10*n  DATA of channel n  (ch1 = +0x34, matches keys.c)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "openfw.h"
#include "openfw_fs.h"
#include "board_config.h"
#include "openfw_hsprobe.h"

#define HSP_SARADC_THR_ADDR(n)   (SARADC_BASE + 0x20u + 0x10u * (uint32_t)(n))
#define HSP_SARADC_DATA_ADDR(n)  (SARADC_BASE + 0x24u + 0x10u * (uint32_t)(n))
#define HSP_SARADC_START_SCAN    0x0000A800u
#define HSP_GPIOB_MUX_HI_ADDR    (GPIOB_BASE + 0x04u) /* PB8..PB15 mux nibbles */
#define HSP_GPIOB_PULL_ADDR      (GPIOB_BASE + 0x24u)
#define HSP_GPIOB_2C_ADDR        (GPIOB_BASE + 0x2Cu)
#define HSP_CODEC_GAIN           20u
#define HSP_CODEC_CH2_MUX_SHIFT  16u
#define HSP_LOG_NAME             "HSPROBE.LOG"

#ifndef HSP_ENABLE_CODEC
#define HSP_ENABLE_CODEC 1
#endif

typedef struct {
    uint32_t    addr;
    const char *name;
} hsp_reg_t;

static const hsp_reg_t k_regs[] = {
    /* SARADC (keys.c configuration) */
    { SARADC_CTRL_ADDR,           "ADC_CTRL" },
    { SARADC_START_ADDR,          "ADC_START" },
    { SARADC_IE_ADDR,             "ADC_IE" },
    { SARADC_REG10_ADDR,          "ADC_R10" },
    { SARADC_REG18_ADDR,          "ADC_R18" },
    { HSP_SARADC_THR_ADDR(0),     "ADC_THR0" },
    { HSP_SARADC_THR_ADDR(1),     "ADC_THR1" },
    { HSP_SARADC_THR_ADDR(2),     "ADC_THR2" },
    { HSP_SARADC_THR_ADDR(3),     "ADC_THR3" },
    { HSP_SARADC_THR_ADDR(4),     "ADC_THR4" },
    { HSP_SARADC_THR_ADDR(5),     "ADC_THR5" },
    { HSP_SARADC_THR_ADDR(6),     "ADC_THR6" },
    { HSP_SARADC_THR_ADDR(7),     "ADC_THR7" },
    { HSP_SARADC_THR_ADDR(8),     "ADC_THR8" },
    { HSP_SARADC_THR_ADDR(9),     "ADC_THR9" },
    /* GPIO port B (PB11 jack detect, PB12..15 AMIC pads) */
    { HSP_GPIOB_MUX_HI_ADDR,      "GPIOB_MUX1" },
    { HSP_GPIOB_PULL_ADDR,        "GPIOB_PULL" },
    { HSP_GPIOB_2C_ADDR,          "GPIOB_2C" },
    /* Codec (touched by audio_capture_start_ex / audio_capture_stop) */
    { CODEC_ANA_CTRL0_ADDR,       "ANA_CTRL0" },
    { CODEC_AMIC01_PGA_ADDR,      "AMIC01_PGA" },
    { CODEC_AMIC2_CTRL_ADDR,      "AMIC2_CTRL" },
    { CODEC_ADC_CH_MUX_ADDR,      "ADC_CH_MUX" },
    { CODEC_ANA_MISC_ADDR,        "ANA_MISC" },
    { CODEC_ADC_DIG_CTRL_ADDR,    "ADC_DIG" },
    { CODEC_ADC_FIFO_CTRL_ADDR,   "ADC_FIFO" },
    { CODEC_ADC_CH_CTRL_ADDR(0u), "ADC_CH0" },
    { CODEC_ADC_CH_CTRL_ADDR(1u), "ADC_CH1" },
    { CODEC_ADC_CH_CTRL_ADDR(2u), "ADC_CH2" },
    { CODEC_ADC_CH_CTRL_ADDR(3u), "ADC_CH3" },
    { CODEC_ADC_CH_GAIN_ADDR(0u), "ADC_G0" },
    { CODEC_ADC_CH_GAIN_ADDR(1u), "ADC_G1" },
    { CODEC_ADC_CH_GAIN_ADDR(2u), "ADC_G2" },
    { CODEC_ADC_CH_GAIN_ADDR(3u), "ADC_G3" },
    { CODEC_DAC0_VOL_ADDR,        "DAC0_VOL" },
    { CODEC_DAC1_VOL_ADDR,        "DAC1_VOL" },
    { CODEC_DAC2_VOL_ADDR,        "DAC2_VOL" },
    { CODEC_DAC0_MUTE_ADDR,       "DAC0_MUTE" },
    { CODEC_DAC1_MUTE_ADDR,       "DAC1_MUTE" },
    { CODEC_DAC2_MUTE_ADDR,       "DAC2_MUTE" },
    { CODEC_DAC_ANA_CFG0_ADDR,    "DAC_ANA0" },
    { CODEC_GLB_ADDR,             "CODEC_GLB" },
};
#define HSP_NREGS ((uint32_t)(sizeof(k_regs) / sizeof(k_regs[0])))
#define HSP_IDX_ADC_START  1u
#define HSP_IDX_GPIOB_FIRST 15u
#define HSP_IDX_CODEC_FIRST 18u

#if defined(OPENFW_BT_BLUEDROID) && OPENFW_BT_BLUEDROID && !defined(OPENFW_HOST_MMIO)
/* BT=1: .bss_hi has no room for 16 KiB (BT bench / link-view buffers) and the FreeRTOS heap must keep its floor for
 * the BT stack, so the log borrows the experiment output buffer (.exp_out, exp_runner.c, 16 KiB at 0x00858000).
 * It is idle outside a USB experiment run (Settings and USB mode never overlap); a probe run overwrites the
 * output of an experiment that was not dumped yet. The default image keeps its own .bss_hi buffer. */
#include "openfw_exp.h"
extern uint8_t g_exp_out[OPENFW_EXP_OUT_SIZE];
_Static_assert(HSP_LOG_CAP <= OPENFW_EXP_OUT_SIZE, "HSPROBE log larger than the experiment output buffer");
#define HSP_LOG_BUF ((char *)g_exp_out)
#else
char g_hsp_log_buf[HSP_LOG_CAP] OPENFW_SRAM_HI;
#define HSP_LOG_BUF g_hsp_log_buf
#endif
static hsp_state_t s_st;
static uint32_t    s_pre[HSP_NREGS];
static uint32_t    s_last_len;

uint32_t hsprobe_last_log_len(void)
{
    return s_last_len;
}

static void reg_line(hsp_line_t *l, const char *prefix, uint32_t first, uint32_t last,
                     const uint32_t *vals)
{
    hsp_line_reset(l);
    hsp_line_str(l, prefix);
    for (uint32_t i = first; i < last; i++) {
        hsp_line_chr(l, ' ');
        hsp_line_str(l, k_regs[i].name);
        hsp_line_chr(l, '=');
        hsp_line_hex_n(l, vals[i], 8u);
    }
}

static void read_saradc(uint16_t v[HSP_NCH])
{
    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        v[c] = (uint16_t)(openfw_mmio_rd32(HSP_SARADC_DATA_ADDR(c)) & 0x0FFFu);
    }
}

static void read_gpio(uint32_t g[HSP_NPORT])
{
    for (uint32_t p = 0u; p < HSP_NPORT; p++) {
        g[p] = openfw_mmio_rd32(gpio_base(p) + 0x10u);
    }
}

static void saradc_restart_scan(void)
{
    openfw_mmio_wr32(SARADC_START_ADDR, 0u);
    mdelay(5u);
    openfw_mmio_wr32(SARADC_START_ADDR, HSP_SARADC_START_SCAN);
    mdelay(5u);
}

static void saradc_enable_all(void)
{
    uint32_t thr1 = s_pre[6]; /* ADC_THR1 = key ladder window used by keys.c */
    for (uint32_t c = 0u; c < HSP_NCH; c++) {
        if (c != 1u) {
            openfw_mmio_wr32(HSP_SARADC_THR_ADDR(c), thr1);
        }
    }
    openfw_mmio_wr32(SARADC_CTRL_ADDR, s_pre[0] | 0x3FFu);
    saradc_restart_scan();
}

static void restore_range(uint32_t first, uint32_t last)
{
    for (uint32_t i = first; i < last; i++) {
        if (i == HSP_IDX_ADC_START) {
            continue; /* START is re-issued by saradc_restart_scan() */
        }
        openfw_mmio_wr32(k_regs[i].addr, s_pre[i]);
    }
}

static void log_raw(const char *s, int reserve)
{
    hsp_line_t l;
    hsp_line_reset(&l);
    hsp_line_str(&l, s);
    (void)hsp_text_line(&s_st.txt, &l, reserve);
}

static void write_header(int codec_on)
{
    hsp_line_t l;
    rtc_datetime_t dt;
    rtc_get(&dt);

    log_raw("# HSPROBE v1 - Cassini OpenFW headset button probe", 0);
    hsp_line_reset(&l);
    hsp_line_str(&l, "# fw=" BOARD_FW_VERSION_STR);
#ifndef OPENFW_HOST_MMIO
    /* FIRM partition CRC-16 (partition table @ flash 0xD010+0x1C) & FIRM header timestamp */
    hsp_line_str(&l, " firm_crc=0x");
    hsp_line_hex_n(&l, (uint32_t)(*(volatile const uint16_t *)(uintptr_t)0x00C0D02Cu), 4u);
    hsp_line_str(&l, " firm_ts=");
    hsp_line_hex(&l, *(volatile const uint32_t *)(uintptr_t)0x00C0E004u);
#endif
    hsp_line_str(&l, " rtc=");
    hsp_line_u32(&l, dt.year);
    hsp_line_chr(&l, '-');
    const uint8_t parts[5] = { dt.month, dt.day, dt.hour, dt.minute, dt.second };
    const char    seps[5]  = { '-', '-', 'T', ':', ':' };
    for (uint32_t i = 0u; i < 5u; i++) {
        hsp_line_chr(&l, seps[i]);
        hsp_line_chr(&l, (char)('0' + (parts[i] / 10u) % 10u));
        hsp_line_chr(&l, (char)('0' + parts[i] % 10u));
    }
    (void)hsp_text_line(&s_st.txt, &l, 0);

    log_raw("# phases: A 0-10s codec idle (MICBIAS off) PB11=GPIO | B 10-20s codec capture "
            "(MICBIAS on, ch2 mux cycles 0..15 / 1.25s) PB11=GPIO | C 20-30s as B + PB11=analog mux15", 0);
    log_raw("# prompts (user asked to press the headset button) at 5,12,16,23,27 s for 1.5 s", 0);
    log_raw("# t_ms K<ch> <adc12>      SARADC ch change > 40 LSB vs last logged value", 0);
    log_raw("# t_ms D|U<ch> <v> ref=<baseline>  large drop / rise (>= 300 LSB) start", 0);
    log_raw("# t_ms E<ch> <v> ref= len=<ms>     drop/rise end  (R = re-baselined after 3 s)", 0);
    log_raw("# t_ms P <0|1>            PB11 raw level change (0 = jack inserted when GPIO)", 0);
    log_raw("# t_ms S <pb11> v0..v9    1 Hz snapshot of all SARADC channels", 0);
    log_raw("# t_ms G<port> <in> x=<changed>  GPIO input reg change (GM = bits masked as noisy)", 0);
    log_raw("# t_ms A mux=<m> n= c1=min/max/mean c2=... p2p=  codec ADC window (c1=AMIC1 ref, c2=mux m)", 0);
    log_raw("# t_ms AE ...             codec 128 ms block with large p2p or mean step", 0);
    log_raw("# SARADC: ch n THR=0x20+0x10n DATA=0x24+0x10n, CTRL bit n = enable (all 10 enabled)", 0);

    for (uint32_t i = 0u; i < HSP_NREGS; i += 6u) {
        uint32_t end = (i + 6u < HSP_NREGS) ? i + 6u : HSP_NREGS;
        reg_line(&l, "# pre:", i, end, s_pre);
        (void)hsp_text_line(&s_st.txt, &l, 0);
    }

    hsp_line_reset(&l);
    hsp_line_str(&l, "# state: hp_detect=");
    hsp_line_u32(&l, (uint32_t)keys_read_hp_detect());
    hsp_line_str(&l, " rec_sw=");
    hsp_line_u32(&l, (uint32_t)keys_read_rec_switch());
    hsp_line_str(&l, " codec_phase=");
    hsp_line_u32(&l, (uint32_t)codec_on);
    (void)hsp_text_line(&s_st.txt, &l, 0);
}

static void self_check(void)
{
    hsp_line_t l;
    uint32_t ok = 0u, diff = 0u;
    log_raw("# ---- self-check (pre vs post restore) ----", 1);
    for (uint32_t i = 0u; i < HSP_NREGS; i++) {
        uint32_t post = openfw_mmio_rd32(k_regs[i].addr);
        if (post == s_pre[i]) {
            ok++;
            continue;
        }
        diff++;
        hsp_line_reset(&l);
        hsp_line_str(&l, "CHK DIFF ");
        hsp_line_str(&l, k_regs[i].name);
        hsp_line_str(&l, " pre=");
        hsp_line_hex(&l, s_pre[i]);
        hsp_line_str(&l, " post=");
        hsp_line_hex(&l, post);
        (void)hsp_text_line(&s_st.txt, &l, 1);
    }
    uint16_t ladder = (uint16_t)(openfw_mmio_rd32(SARADC_CH1_DATA_ADDR) & 0x0FFFu);
    uint32_t mux11  = (openfw_mmio_rd32(HSP_GPIOB_MUX_HI_ADDR) >> 12) & 0xFu;
    hsp_line_reset(&l);
    hsp_line_str(&l, "CHK ok=");
    hsp_line_u32(&l, ok);
    hsp_line_str(&l, " diff=");
    hsp_line_u32(&l, diff);
    hsp_line_str(&l, " ladder_ch1=");
    hsp_line_u32(&l, ladder);
    hsp_line_str(&l, " key=");
    hsp_line_str(&l, keys_name(keys_decode(ladder)));
    hsp_line_str(&l, " pb11_mux=");
    hsp_line_u32(&l, mux11);
    hsp_line_str(&l, " hp_detect=");
    hsp_line_u32(&l, (uint32_t)keys_read_hp_detect());
    hsp_line_str(&l, (diff == 0u) ? " RESULT=PASS" : " RESULT=DIFF");
    (void)hsp_text_line(&s_st.txt, &l, 1);
}

static int save_log(const hsprobe_ctx_t *ctx)
{
    if (!ctx->sd_mounted || !ctx->vol) {
        return HSP_RC_NO_SD;
    }
    exfat_vol_t *vol = (exfat_vol_t *)ctx->vol;
    rtc_datetime_t dt;
    rtc_get(&dt);
    exfat_set_timestamp(vol, rtc_to_dos_timestamp(&dt));
    (void)exfat_delete_file(vol, "/", HSP_LOG_NAME);
    exfat_writer_t w;
    if (exfat_create_file(vol, "/", HSP_LOG_NAME, &w) != 0) {
        return HSP_RC_SAVE_FAIL;
    }
    wdt_feed();
    if (exfat_write(&w, HSP_LOG_BUF, s_last_len) != 0) {
        (void)exfat_close_file(&w);
        return HSP_RC_SAVE_FAIL;
    }
    if (exfat_close_file(&w) != 0) {
        return HSP_RC_SAVE_FAIL;
    }
    wdt_feed();
    return HSP_RC_SAVED;
}

int hsprobe_run(const hsprobe_ctx_t *ctx)
{
    if (!ctx || keys_read_rec_switch() || recorder_is_saving() || audio_dac_is_busy()) {
        return HSP_RC_BUSY;
    }

    for (uint32_t i = 0u; i < HSP_NREGS; i++) {
        s_pre[i] = openfw_mmio_rd32(k_regs[i].addr);
    }

    /* GPIO auto-mask seeds: drop pins known to toggle (LCD bus, backlight PWM, SD, KADC)
     * and PB11 (logged separately as P). Everything else is watched. */
    static const uint32_t k_gpio_mask[HSP_NPORT] = {
        0xFFFFFFFFu,
        ~(0x3Fu | (1u << 11) | (1u << 17) | (1u << 18)),
        0xFFFFFFFFu,
        ~(0x7Fu | (1u << 10)),
    };
    hsp_init(&s_st, HSP_LOG_BUF, HSP_LOG_CAP, k_gpio_mask);
    write_header(HSP_ENABLE_CODEC);

    keys_pause_scan(1);
    saradc_enable_all();

    uint16_t    v[HSP_NCH];
    uint32_t    gp[HSP_NPORT];
    const char *reason      = "complete";
    uint32_t    t0          = millis();
    uint32_t    next_sample = 0u;
    uint32_t    next_snap   = 0u;
    uint32_t    next_ui     = 0u;
    uint8_t     phase       = 0u;
    uint8_t     prompt      = 0u;
    int         codec_on    = 0;
    uint32_t    mux_idx     = 0u;
    uint32_t    mux_next    = 0u;
    uint8_t     back_cnt    = 0u;
    int         back_armed  = 0;
    int8_t      plug        = (int8_t)keys_read_hp_detect();
    uint32_t    t           = 0u;

    hsp_set_phase(&s_st, 0u, 0u, "codec_idle pb11=gpio");

    for (;;) {
        wdt_feed();
        uint32_t now = millis();
        t = now - t0;
        if (t >= HSP_T_END) {
            break;
        }
        if (ctx->poll) {
            (void)ctx->poll(now);
        }
        if (keys_read_rec_switch()) {
            reason = "rec_switch";
            break;
        }

        /* Phase transitions */
        uint8_t ph = hsp_phase_at(t);
        if (ph != phase) {
            phase = ph;
            if (phase == 1u) {
                hsp_set_phase(&s_st, t, 1u, HSP_ENABLE_CODEC ? "codec_capture micbias=on pb11=gpio"
                                                             : "codec_disabled pb11=gpio");
#if HSP_ENABLE_CODEC
                if (audio_capture_start_ex(HSP_CODEC_GAIN, 2u) == 0) {
                    codec_on = 1;
                    mux_idx  = 0u;
                    openfw_mmio_rmw32(CODEC_ADC_CH_MUX_ADDR, 0xFu << HSP_CODEC_CH2_MUX_SHIFT,
                                      mux_idx << HSP_CODEC_CH2_MUX_SHIFT);
                    t        = millis() - t0;
                    mux_next = t + HSP_MUX_WINDOW_MS;
                    hsp_mark(&s_st, t, "CODEC_ON gain", HSP_CODEC_GAIN);
                    hsp_codec_window_begin(&s_st, t, (uint8_t)mux_idx);
                } else {
                    hsp_mark(&s_st, t, "CODEC_FAIL", 0u);
                }
#endif
            } else {
                hsp_set_phase(&s_st, t, 2u, "pb11=analog_mux15");
                gpio_set_mux(GPIO_PORT_B, 11u, 15u);
                plug = -1;
            }
            next_sample = t; /* resync after blocking init */
        }

        /* Codec: drain finished 128 ms slots, cycle ch2 mux */
        if (codec_on) {
            uint32_t bytes = 0u;
            const int16_t *f;
            while ((f = audio_pop_frame(&bytes)) != (const int16_t *)0) {
                hsp_codec_block(&s_st, t, f, bytes / 4u, 2u);
            }
            if (t >= mux_next && mux_idx + 1u < HSP_NMUX) {
                hsp_codec_window_end(&s_st, t);
                mux_idx++;
                openfw_mmio_rmw32(CODEC_ADC_CH_MUX_ADDR, 0xFu << HSP_CODEC_CH2_MUX_SHIFT,
                                  mux_idx << HSP_CODEC_CH2_MUX_SHIFT);
                (void)audio_pop_frame(&bytes); /* drop a slot that straddles the switch */
                hsp_codec_window_begin(&s_st, t, (uint8_t)mux_idx);
                mux_next += HSP_MUX_WINDOW_MS;
            }
        }

        /* Prompt windows */
        uint8_t pr = hsp_prompt_at(t);
        if (pr != prompt) {
            prompt = pr;
            if (pr) {
                hsp_mark(&s_st, t, "PROMPT", pr);
            }
            next_ui = t;
        }

        /* 10 ms sample */
        if (t >= next_sample) {
            if (t > next_sample + 50u) {
                hsp_mark(&s_st, t, "LAG", t - next_sample);
            }
            next_sample = t + HSP_SAMPLE_MS;
            read_saradc(v);
            read_gpio(gp);
            int pb11 = (int)((gp[GPIO_PORT_B] >> 11) & 1u);
            hsp_sample(&s_st, t, v, pb11, gp);
            if (t >= next_snap) {
                next_snap += 1000u;
                hsp_snapshot(&s_st, t, v, pb11);
            }
            if (phase < 2u) {
                plug = (int8_t)(pb11 == 0);
            }
            /* BACK on the key ladder (ch1), debounced 3 samples; must see it released first */
            key_id_t k = keys_decode(v[1]);
            if (k != KEY_BACK) {
                back_armed = 1;
                back_cnt   = 0u;
            } else if (back_armed && ++back_cnt >= 3u) {
                reason = "back";
                break;
            }
        }

        if (t >= next_ui && ctx->render) {
            next_ui = t + 250u;
            uint32_t left = (HSP_T_END - t + 999u) / 1000u;
            ctx->render((uint8_t)left, (uint8_t)(phase + 1u), prompt, plug);
        }

        OPENFW_TASK_WAIT();   /* BT=1 (FreeRTOS): yield to the BT tasks; WFI otherwise */
    }

    /* ---- Restore (PB11 / codec / SARADC), then self-check ---- */
    if (codec_on) {
        hsp_codec_window_end(&s_st, t);
        audio_capture_stop();
        hsp_mark(&s_st, t, "CODEC_OFF", 0u);
    }
    restore_range(HSP_IDX_CODEC_FIRST, HSP_NREGS);
    restore_range(HSP_IDX_GPIOB_FIRST, HSP_IDX_CODEC_FIRST);
    restore_range(0u, HSP_IDX_GPIOB_FIRST);
    saradc_restart_scan();
    mdelay(15u);
    keys_pause_scan(0);
    wdt_feed();

    self_check();
    hsp_summary(&s_st, t, reason);
    s_last_len = s_st.txt.len;
    return save_log(ctx);
}
