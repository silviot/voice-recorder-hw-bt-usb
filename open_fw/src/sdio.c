/*
 * Smartlink SL6801 Bare-Metal 1-Bit SDIO / microSD Driver (0x40003000)
 *
 * Uses raw polling PIO on Port D (PD5=CLK, PD4=CMD, PD6=D0) with single-block
 * CMD17/CMD24 transfers so BootROM SRAM (0x00804A04) is untouched and the
 * factory firmware's CMD18/CMD12 multi-block stall is completely avoided.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_bench_meas.h"   /* SD error counters: bench images only (OPENFW_BENCH_MEAS), compiled out otherwise */

#define SDIO_CMD_START      0x80000000u
#define SDIO_CMD_UPD_CLK    0x80200000u

#define SD_CMD0_GO_IDLE     0xA400u
#define SD_CMD2_ALL_CID     0x25C2u
#define SD_CMD3_REL_ADDR    0x2543u
#define SD_CMD7_SELECT      0x2547u
#define SD_CMD8_IF_COND     0x2548u
#define SD_CMD9_SEND_CSD    0x25C9u
#define SD_CMD12_STOP       0x454Cu
#define SD_CMD13_STATUS     0x254Du
#define SD_CMD16_SETLEN     0x2550u
#define SD_CMD17_READ1       0x2351u
#define SD_CMD18_READ_MULTI  0x2352u
#define SD_CMD24_WRITE1      0x2758u
#define SD_CMD25_WRITE_MULTI 0x2759u
#define SD_ACMD41_OPCOND     0x2569u
#define SD_CMD55_APPCMD      0x2577u

/* Fatal write-programming R1 bits: WP_VIOLATION | CARD_ECC_FAILED | CC_ERROR | ERROR.
 * Avoids aborting on sticky historical status bits (e.g. OUT_OF_RANGE from prior probes). */
#define SD_R1_WRITE_ERR_MASK 0x04380000u

#define SDIO_CMD_TIMEOUT_US  100000u
#define SDIO_DAT_TIMEOUT_US  500000u
#define SDIO_MAX_MULTI_BLKS  64u

/* R1 CURRENT_STATE (bits 12:9) and READY_FOR_DATA (bit 8) */
#define SD_R1_STATE(r1)      (((r1) >> 9) & 0xFu)
#define SD_R1_READY          (1u << 8)
#define SD_STATE_TRAN        4u
#define SD_STATE_DATA        5u
#define SD_STATE_RCV         6u
#define SD_STATE_PRG         7u

/* RINTSTS bit 13 is SBE (start-bit error) on reads but BCI (busy clear interrupt) on writes: sd_init() sets bit 1 of
 * +0x100 (dw_mmc CARDTHRCTL.BsyClrIntEn; the register is named SDIO_BMOD here after the factory firmware), so the
 * controller raises bit 13 every time the card releases DAT0 busy after a written block. That is completion, not an
 * error. A CMD25 always shows it at DTO (the busy between blocks ends before the last block), and a CMD24 shows it
 * whenever the CPU reads RINTSTS after the card finished programming - i.e. whenever the (priority 1) app task was
 * preempted for longer than the card's programming time between DTO and the status read, which BT traffic does
 * (silicon 2026-11-26: SDW_MDTO a=0x2018 at the first CMD25 of every recording; SDW_1_FAIL during XREC GETS).
 * Write data phases check this mask; real write failures are DCRC (CRC status negative), EBE (no CRC status),
 * DRTO/HTO-class timeouts and the R1 error bits of the CMD13 that follows. */
#define SDIO_INT_WR_DATA_ERRS (SDIO_INT_DATA_ERRS & ~SDIO_INT_SBE)

/* Sector write retries (sd_write_blocks): SDIO_WR_SOFT_TRIES attempts with a card-state recovery (FIFO reset, CMD13,
 * CMD12 if the card is stuck in data/rcv, busy wait) before each retry, then one in-place re-initialisation and
 * SDIO_WR_REINIT_TRIES more. A retry rewrites the same sector with the same data at the same LBA: idempotent. */
#define SDIO_WR_SOFT_TRIES   3u
#define SDIO_WR_REINIT_TRIES 2u
#define SDIO_RD_TRIES        2u

static void sdio_log_err(const char *tag, uint32_t a, uint32_t b)
{
#if !defined(OPENFW_HOST_MMIO) || defined(OPENFW_WASM)
#if OPENFW_BENCH_MEAS
    bench_meas_sd_err(&g_bench_meas, tag);   /* every error, also past the 8-line log cap */
#endif
    static uint8_t s_err_logs = 0u;
    if (s_err_logs >= 8u) {
        return;
    }
    s_err_logs++;
    openfw_log_puts(tag);
    openfw_log_puts(" a=");
    openfw_log_hex(a);
    openfw_log_puts(" b=");
    openfw_log_hex(b);
    openfw_log_putc('\n');
#else
    (void)tag;
    (void)a;
    (void)b;
#endif
}

static int sdio_read_one_sector(sd_card_t *card, uint32_t lba, uint8_t *dst);

/* RINTSTS of the last sdio_send_cmd() (0xFFFFFFFF: never accepted / no CMD_DONE): for the failure logs only. */
static uint32_t s_last_cmd_st;

static void sdio_reset_fifo(void)
{
    openfw_mmio_set32(SDIO_CTRL_ADDR, 0x2u);
    uint64_t t0 = us_timer_now();
    while (openfw_mmio_rd32(SDIO_CTRL_ADDR) & 0x2u) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            break;
        }
    }
}

static int sdio_wait_cmd_accept(void)
{
    uint64_t t0 = us_timer_now();
    while (openfw_mmio_rd32(SDIO_CMD_ADDR) & SDIO_CMD_START) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            return -1;
        }
    }
    return 0;
}

static int sdio_update_clk(void)
{
    if (sdio_wait_cmd_accept() != 0) {
        return -1;
    }
    openfw_mmio_wr32(SDIO_CMD_ADDR, SDIO_CMD_UPD_CLK);
    return sdio_wait_cmd_accept();
}

static void sdio_set_clk_div(uint32_t div)
{
    rom_clk_disable(CRU_CLK_SDIO);
    rom_clk_div(CRU_CLK_SDIO, div);
    rom_us_delay(100u);
    rom_clk_enable(CRU_CLK_SDIO);
}

static int sdio_send_cmd(uint32_t cmd_word, uint32_t arg, int ignore_rcrc)
{
    s_last_cmd_st = 0xFFFFFFFFu;
    if (sdio_wait_cmd_accept() != 0) {
        return -1;
    }

    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_ARG_ADDR,     arg);
    openfw_mmio_wr32(SDIO_CMD_ADDR,     SDIO_CMD_START | (cmd_word & 0xFFFFu));

    uint64_t t0 = us_timer_now();
    while (!(openfw_mmio_rd32(SDIO_RINTSTS_ADDR) & SDIO_INT_CMD_DONE)) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            return -1;
        }
    }

    uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0x00000344u);
    s_last_cmd_st = st;

    if (st & (SDIO_INT_RTO | SDIO_INT_DRTO)) {
        return -1;
    }
    if (!ignore_rcrc && (st & SDIO_INT_RCRC)) {
        return -1;
    }
    return 0;
}

static int sdio_send_acmd41(uint32_t arg)
{
    if (sdio_send_cmd(SD_CMD55_APPCMD, 0u, 0) != 0) {
        return -1;
    }
    return sdio_send_cmd(SD_ACMD41_OPCOND, arg, 1);
}

static uint32_t parse_csd_sectors(void)
{
    /* Standard DesignWare dw_mmc R2 packing: RESP3=[127:96], RESP2=[95:64], RESP1=[63:32] */
    uint32_t csd3 = openfw_mmio_rd32(SDIO_RESP3_ADDR);
    uint32_t csd2 = openfw_mmio_rd32(SDIO_RESP2_ADDR);
    uint32_t csd1 = openfw_mmio_rd32(SDIO_RESP1_ADDR);
    uint32_t csd_ver = (csd3 >> 30) & 0x3u;
    if (csd_ver == 1u) {
        /* CSD v2.0 (SDHC/SDXC): C_SIZE = [69:48], capacity = (C_SIZE + 1) * 512 KiB */
        uint32_t c_size = ((csd2 & 0x3Fu) << 16) | ((csd1 >> 16) & 0xFFFFu);
        uint64_t secs64 = ((uint64_t)c_size + 1u) * 1024u;
        return (secs64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)secs64;
    }
    if (csd_ver == 0u) {
        /* CSD v1.0 (SDSC): READ_BL_LEN=[83:80], C_SIZE=[73:62], C_SIZE_MULT=[49:47] */
        uint32_t read_bl_len = (csd2 >> 16) & 0xFu;
        uint32_t c_size      = ((csd2 & 0x3FFu) << 2) | ((csd1 >> 30) & 0x3u);
        uint32_t c_size_mult = (csd1 >> 15) & 0x7u;
        if (read_bl_len < 9u || read_bl_len > 11u) {
            return 0u;
        }
        uint32_t shift  = c_size_mult + 2u + read_bl_len - 9u;
        uint64_t secs64 = ((uint64_t)c_size + 1u) << shift;
        return (secs64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)secs64;
    }
    return 0u;
}

int sd_init(sd_card_t *card)
{
    if (!card) {
        return -1;
    }
    card->initialized  = 0;
    card->is_sdhc      = 0;
    card->rca          = 0u;
    card->sector_count = 0u;
    card->clk_div      = 0u;
    card->async_active = 0u;
    card->async_last_desc = 0u;
    card->no_multi_write = 0u;
    card->async_t0_us  = 0u;
    card->need_status  = 0u;
    card->reinits      = 0u;
    card->wr_retries   = 0u;
    card->wr_bci       = 0u;

    /* 1-bit SDIO pins on Port D: PD5=CLK, PD4=CMD (PU), PD6=D0 (PU) */
    rom_gpio_config(0x00032D90u);
    rom_gpio_config(0x0003259Bu);
    rom_gpio_config(0x0003359Bu);

    rom_clk_set_src(CRU_CLK_SDIO, CRU_CLK_SRC_HOSC24M);
    rom_rst_hold(CRU_RST_SDIO);
    mdelay(5u);
    rom_rst_release(CRU_RST_SDIO);

    /* Initial 375 kHz identification clock (24 MHz / 64) */
    sdio_set_clk_div(64u);

    /* Soft reset controller + FIFO + DMA, then lock 1-bit polling mode */
    openfw_mmio_set32(SDIO_CTRL_ADDR, 0x7u);
    {
        uint64_t t0 = us_timer_now();
        while (openfw_mmio_rd32(SDIO_CTRL_ADDR) & 0x7u) {
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
                return -1;
            }
        }
    }
    openfw_mmio_rmw32(SDIO_CTRL_ADDR, 0xC0000008u, 0x40000000u);
    if (sdio_update_clk() != 0) {
        return -1;
    }

    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_FIFOTH_ADDR,  0x20070008u);
    openfw_mmio_wr32(SDIO_TMOUT_ADDR,   0x00FFFFFFu);
    openfw_mmio_set32(SDIO_BMOD_ADDR,   0x2u);
    openfw_mmio_wr32(SDIO_INTMASK_ADDR, 0xFFFFFF40u);

    /* Card identification: CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 */
    if (sdio_send_cmd(SD_CMD0_GO_IDLE, 0u, 1) != 0) {
        return -1;
    }
    mdelay(2u);

    uint32_t acmd41_arg = 0x00300000u; /* 3.2V - 3.4V window */
    if (sdio_send_cmd(SD_CMD8_IF_COND, 0x000001AAu, 0) == 0 &&
        (openfw_mmio_rd32(SDIO_RESP0_ADDR) & 0xFFFu) == 0x1AAu) {
        acmd41_arg = 0x40300000u; /* SD v2.0 HCS=1 */
    }

    int ready = 0;
    uint32_t ocr = 0u;
    for (int tries = 0; tries < 250; tries++) {
        wdt_feed();
        if (sdio_send_acmd41(acmd41_arg) == 0) {
            ocr = openfw_mmio_rd32(SDIO_RESP0_ADDR);
            if (ocr & (1u << 31)) {
                ready = 1;
                break;
            }
        }
        mdelay(4u);
    }
    if (!ready) {
        return -1;
    }

    int is_sdhc = (ocr & (1u << 30)) ? 1 : 0;

    if (sdio_send_cmd(SD_CMD2_ALL_CID, 0u, 1) != 0) {
        return -1;
    }
    if (sdio_send_cmd(SD_CMD3_REL_ADDR, 0x00010000u, 0) != 0) {
        return -1;
    }
    uint16_t rca = (uint16_t)(openfw_mmio_rd32(SDIO_RESP0_ADDR) >> 16);
    if (rca == 0u) {
        return -1;
    }

    uint32_t sectors = 0u;
    if (sdio_send_cmd(SD_CMD9_SEND_CSD, (uint32_t)rca << 16, 1) == 0) {
        sectors = parse_csd_sectors();
    }

    if (sdio_send_cmd(SD_CMD7_SELECT, (uint32_t)rca << 16, 0) != 0) {
        return -1;
    }
    if (!is_sdhc) {
        if (sdio_send_cmd(SD_CMD16_SETLEN, 512u, 0) != 0) {
            return -1;
        }
    }

    card->initialized  = 1;
    card->is_sdhc      = is_sdhc;
    card->rca          = rca;
    card->sector_count = sectors;

    /* Auto-negotiate highest reliable 1-bit SDIO clock: 24 MHz -> 12 MHz -> 6 MHz */
    static const uint32_t k_divs[3] = { 1u, 2u, 4u };
    uint8_t probe_sec[512];
    int clk_ok = 0;
    for (uint32_t di = 0u; di < 3u; di++) {
        sdio_set_clk_div(k_divs[di]);
        if (sdio_update_clk() != 0) {
            continue;
        }
        if (sdio_read_one_sector(card, 0u, probe_sec) == 0) {
            card->clk_div = (uint8_t)k_divs[di];
            clk_ok = 1;
            break;
        }
        sdio_reset_fifo();
    }
    if (!clk_ok) {
        card->initialized = 0;
        return -1;
    }
    return 0;
}

static int sdio_wait_card_ready(sd_card_t *card)
{
    /* Wait for card programming completion (R1 bit 8 = READY_FOR_DATA, state != PRG) */
    uint64_t p0 = us_timer_now();
    uint32_t fast_polls = 0u;
    uint32_t last_r1 = 0u;
    while (1) {
        wdt_feed();
        if (sdio_send_cmd(SD_CMD13_STATUS, (uint32_t)card->rca << 16, 0) == 0) {
            uint32_t r1 = openfw_mmio_rd32(SDIO_RESP0_ADDR);
            last_r1 = r1;
            if (r1 & SD_R1_WRITE_ERR_MASK) {
                sdio_log_err("SD_R1_ERR", r1, openfw_mmio_rd32(SDIO_RINTSTS_ADDR));
                return -1;
            }
            if ((r1 & SD_R1_READY) && (SD_R1_STATE(r1) != SD_STATE_PRG)) {
                card->need_status = 0u;
                return 0;
            }
        }
        if ((uint32_t)(us_timer_now() - p0) >= SDIO_DAT_TIMEOUT_US) {
            sdio_log_err("SD_RDY_TMO", last_r1, openfw_mmio_rd32(SDIO_RINTSTS_ADDR));
            return -1;
        }
        if (fast_polls < 16u) {
            fast_polls++;
        } else {
            rom_us_delay(50u);
        }
    }
}

static int sdio_fifo_read_words(uint8_t *dst, uint32_t words, uint64_t t0, uint32_t timeout_us)
{
    int aligned = (((uintptr_t)dst & 3u) == 0u);
    uint32_t *dst32 = (uint32_t *)(void *)dst;
    for (uint32_t w = 0u; w < words; w++) {
        uint32_t polls = 0u;
        while (openfw_mmio_rd32(SDIO_STATUS_ADDR) & SDIO_STAT_FIFO_EMP) {
            if ((polls & 31u) == 31u) {
                uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
                if (st & SDIO_INT_DATA_ERRS) {
                    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                    return -1;
                }
                if ((uint32_t)(us_timer_now() - t0) >= timeout_us) {
                    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                    return -1;
                }
            }
            polls++;
        }
        uint32_t word = openfw_mmio_rd32(SDIO_FIFO_ADDR);
        if (aligned) {
            dst32[w] = word;
        } else {
            uint32_t off  = w * 4u;
            dst[off + 0u] = (uint8_t)(word);
            dst[off + 1u] = (uint8_t)(word >> 8);
            dst[off + 2u] = (uint8_t)(word >> 16);
            dst[off + 3u] = (uint8_t)(word >> 24);
        }
    }
    return 0;
}

static int sdio_fifo_write_words(const uint8_t *src, uint32_t words, uint64_t t0, uint32_t timeout_us)
{
    int aligned = (((uintptr_t)src & 3u) == 0u);
    const uint32_t *src32 = (const uint32_t *)(const void *)src;
    for (uint32_t w = 0u; w < words; w++) {
        uint32_t polls = 0u;
        while (openfw_mmio_rd32(SDIO_STATUS_ADDR) & SDIO_STAT_FIFO_FULL) {
            if ((polls & 31u) == 31u) {
                uint32_t st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
                if (st & SDIO_INT_WR_DATA_ERRS) {   /* bit 13 here = busy clear between CMD25 blocks */
                    sdio_log_err("SDW_FIFO_ERR", st, w);
                    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                    return -1;
                }
                if ((uint32_t)(us_timer_now() - t0) >= timeout_us) {
                    sdio_log_err("SDW_FIFO_TMO", st, w);
                    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
                    return -1;
                }
            }
            polls++;
        }
        uint32_t word;
        if (aligned) {
            word = src32[w];
        } else {
            uint32_t off = w * 4u;
            word = (uint32_t)src[off + 0u]
                 | ((uint32_t)src[off + 1u] << 8)
                 | ((uint32_t)src[off + 2u] << 16)
                 | ((uint32_t)src[off + 3u] << 24);
        }
        openfw_mmio_wr32(SDIO_FIFO_ADDR, word);
    }
    return 0;
}

static int sdio_wait_dto(uint64_t t0, uint32_t timeout_us, uint32_t *out_st)
{
    uint32_t st;
    while (!((st = openfw_mmio_rd32(SDIO_RINTSTS_ADDR)) & SDIO_INT_DTO)) {
        if ((uint32_t)(us_timer_now() - t0) >= timeout_us) {
            *out_st = st;
            openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
            return -1;
        }
    }
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    *out_st = st;
    return 0;
}

/* Bring the card back to tran/ready before a data command: CMD13; data/rcv (an open read or write whose CMD12 was
 * lost) -> CMD12; prg -> poll until programming ends. Bounded by SDIO_DAT_TIMEOUT_US. Clears card->need_status. */
static int sdio_recover_state(sd_card_t *card)
{
    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    uint64_t t0 = us_timer_now();
    uint32_t stops = 0u;
    uint32_t last_r1 = 0xFFFFFFFFu;
    while (1) {
        wdt_feed();
        if (sdio_send_cmd(SD_CMD13_STATUS, (uint32_t)card->rca << 16, 0) == 0) {
            uint32_t r1 = openfw_mmio_rd32(SDIO_RESP0_ADDR);
            uint32_t state = SD_R1_STATE(r1);
            last_r1 = r1;
            if (state == SD_STATE_TRAN && (r1 & SD_R1_READY)) {
                card->need_status = 0u;
                return 0;
            }
            if ((state == SD_STATE_DATA || state == SD_STATE_RCV) && stops < 3u) {
                stops++;
                (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
                sdio_reset_fifo();
                continue;
            }
        } else if (stops < 3u) {
            /* No answer to CMD13: a card still streaming an open-ended read can garble it; stop it and ask again. */
            stops++;
            (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
            sdio_reset_fifo();
        }
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_DAT_TIMEOUT_US) {
            sdio_log_err("SD_RECOVER", last_r1, stops);
            return -1;
        }
        rom_us_delay(50u);
    }
}

static int sdio_ensure_ready(sd_card_t *card)
{
    return card->need_status ? sdio_recover_state(card) : 0;
}

static int sdio_read_one_sector(sd_card_t *card, uint32_t lba, uint8_t *dst)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD17_READ1, arg, 0) != 0) {
        return -1;
    }

    uint64_t t0 = us_timer_now();
    if (sdio_fifo_read_words(dst, 128u, t0, SDIO_DAT_TIMEOUT_US) != 0) {
        sdio_reset_fifo();
        return -1;
    }

    uint32_t st = 0u;
    if (sdio_wait_dto(us_timer_now(), SDIO_DAT_TIMEOUT_US, &st) != 0 || (st & SDIO_INT_DATA_ERRS)) {
        sdio_reset_fifo();
        return -1;
    }
    return 0;
}

typedef struct {
    volatile uint32_t des0;
    uint32_t          des1;
    uint32_t          des2;
    uint32_t          des3;
} sdio_idmac_desc_t;

#define IDMAC_DES0_OWN (1u << 31)
#define IDMAC_DES0_CH  (1u << 4)
#define IDMAC_DES0_FS  (1u << 3)
#define IDMAC_DES0_LD  (1u << 2)
#define IDMAC_DES0_DIC (1u << 1)

#ifndef OPENFW_HOST_MMIO
static sdio_idmac_desc_t g_idmac_desc[2] __attribute__((aligned(16)));
#endif

static void sdio_reset_dma_and_fifo(void)
{
    openfw_mmio_set32(SDIO_CTRL_ADDR, 0x6u); /* FIFO_RESET | DMA_RESET */
    uint64_t t0 = us_timer_now();
    while (openfw_mmio_rd32(SDIO_CTRL_ADDR) & 0x6u) {
        if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
            break;
        }
    }
}

void sd_read_async_abort(sd_card_t *card)
{
    if (!card || !card->async_active) {
        return;
    }
    card->async_active = 0u;
    card->need_status  = 1u;
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
    openfw_mmio_clr32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
    (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
    sdio_reset_dma_and_fifo();
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, SDIO_IDMAC_BMOD_SWR);
    {
        uint64_t t0 = us_timer_now();
        while (openfw_mmio_rd32(SDIO_IDMAC_BMOD_ADDR) & SDIO_IDMAC_BMOD_SWR) {
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
                break;
            }
        }
    }
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_IDSTS_ADDR,   0xFFFFFFFFu);
    OPENFW_DSB();
}

int sd_read_async_start(sd_card_t *card, uint32_t lba, uint32_t count, void *buf)
{
#ifdef OPENFW_HOST_MMIO
    (void)card; (void)lba; (void)count; (void)buf;
    return -2; /* 64-bit host pointer width cannot feed 32-bit IDMAC */
#else
    if (!card || !card->initialized || !buf) {
        return -1;
    }
    if (card->async_active) {
        sd_read_async_abort(card);
    }
    if (count < 2u || count > 16u || (((uintptr_t)buf & 3u) != 0u)) {
        return -2; /* Caller falls back to synchronous PIO */
    }
    if (sdio_ensure_ready(card) != 0) {
        return -1;
    }
    if (card->sector_count > 0u &&
        ((uint64_t)lba + (uint64_t)count) > (uint64_t)card->sector_count) {
        return -1;
    }
    if (card->sector_count > 3u &&
        ((uint64_t)lba + (uint64_t)count) > (uint64_t)(card->sector_count - 3u)) {
        return -2; /* Near end-of-disk: use single-block CMD17 */
    }

    uint32_t total_bytes = count * 512u;
    uint32_t buf_addr    = (uint32_t)(uintptr_t)buf;
    if (total_bytes <= 4096u) {
        g_idmac_desc[0].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_FS | IDMAC_DES0_LD;
        g_idmac_desc[0].des1 = total_bytes;
        g_idmac_desc[0].des2 = buf_addr;
        g_idmac_desc[0].des3 = (uint32_t)(uintptr_t)&g_idmac_desc[0];
        card->async_last_desc = 0u;
    } else {
        g_idmac_desc[0].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_FS | IDMAC_DES0_DIC;
        g_idmac_desc[0].des1 = 4096u;
        g_idmac_desc[0].des2 = buf_addr;
        g_idmac_desc[0].des3 = (uint32_t)(uintptr_t)&g_idmac_desc[1];
        g_idmac_desc[1].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_LD;
        g_idmac_desc[1].des1 = total_bytes - 4096u;
        g_idmac_desc[1].des2 = buf_addr + 4096u;
        g_idmac_desc[1].des3 = (uint32_t)(uintptr_t)&g_idmac_desc[1];
        card->async_last_desc = 1u;
    }
    OPENFW_DSB();

    sdio_reset_dma_and_fifo();
    openfw_mmio_set32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, SDIO_IDMAC_BMOD_SWR);
    {
        uint64_t t0 = us_timer_now();
        while (openfw_mmio_rd32(SDIO_IDMAC_BMOD_ADDR) & SDIO_IDMAC_BMOD_SWR) {
            if ((uint32_t)(us_timer_now() - t0) >= SDIO_CMD_TIMEOUT_US) {
                openfw_mmio_clr32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
                return -1;
            }
        }
    }
    openfw_mmio_wr32(SDIO_IDSTS_ADDR,      0xFFFFFFFFu);
    openfw_mmio_wr32(SDIO_IDINTEN_ADDR,    SDIO_IDSTS_RI | SDIO_IDSTS_TI);
    openfw_mmio_wr32(SDIO_DBADDR_ADDR,     (uint32_t)(uintptr_t)&g_idmac_desc[0]);
    openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, SDIO_IDMAC_BMOD_DE | SDIO_IDMAC_BMOD_FB);

    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  total_bytes);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    uint32_t arg = card->is_sdhc ? lba : (lba << 9);
    if (sdio_send_cmd(SD_CMD18_READ_MULTI, arg, 0) != 0) {
        openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
        openfw_mmio_clr32(SDIO_CTRL_ADDR, SDIO_CTRL_USE_IDMAC);
        sdio_reset_dma_and_fifo();
        return -1;
    }
    openfw_mmio_wr32(SDIO_PLDMND_ADDR, 1u);

    card->async_active = 1u;
    card->async_t0_us  = (uint32_t)us_timer_now();
    return 0;
#endif
}

int sd_read_async_poll(sd_card_t *card)
{
#ifdef OPENFW_HOST_MMIO
    (void)card;
    return -1;
#else
    if (!card || !card->async_active) {
        return 1;
    }
    uint32_t st    = openfw_mmio_rd32(SDIO_RINTSTS_ADDR);
    uint32_t idsts = openfw_mmio_rd32(SDIO_IDSTS_ADDR);
    if ((st & SDIO_INT_DATA_ERRS) ||
        (idsts & (SDIO_IDSTS_FBE | SDIO_IDSTS_DU | SDIO_IDSTS_CES))) {
        sd_read_async_abort(card);
        return -1;
    }
    if ((st & SDIO_INT_DTO) &&
        ((g_idmac_desc[card->async_last_desc].des0 & IDMAC_DES0_OWN) == 0u)) {
        card->async_active = 0u;
        openfw_mmio_wr32(SDIO_RINTSTS_ADDR,    0xFFFFFFFFu);
        openfw_mmio_wr32(SDIO_IDSTS_ADDR,      0xFFFFFFFFu);
        openfw_mmio_wr32(SDIO_IDMAC_BMOD_ADDR, 0u);
        openfw_mmio_clr32(SDIO_CTRL_ADDR,      SDIO_CTRL_USE_IDMAC);
        int stop_rc = sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
        sdio_reset_fifo();
        card->need_status = 1u;
        OPENFW_DSB();
        return (stop_rc == 0) ? 1 : -1;
    }
    if ((uint32_t)((uint32_t)us_timer_now() - card->async_t0_us) >= SDIO_DAT_TIMEOUT_US) {
        sd_read_async_abort(card);
        return -1;
    }
    return 0;
#endif
}

int sd_read_async_wait(sd_card_t *card)
{
    if (!card || !card->async_active) {
        return 0;
    }
    while (1) {
        wdt_feed();
        int rc = sd_read_async_poll(card);
        if (rc > 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
    }
}

int sd_flush(sd_card_t *card)
{
    if (card && card->async_active) {
        sd_read_async_abort(card);
    }
    return 0;
}

static int sdio_read_multi(sd_card_t *card, uint32_t lba, uint32_t count, uint8_t *dst)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  count * 512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD18_READ_MULTI, arg, 0) != 0) {
        return -1;
    }

    uint32_t xfer_timeout = SDIO_DAT_TIMEOUT_US + (count * 50000u);
    uint64_t t0 = us_timer_now();
    if (sdio_fifo_read_words(dst, count * 128u, t0, xfer_timeout) != 0) {
        (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
        sdio_reset_fifo();
        card->need_status = 1u;
        return -1;
    }

    uint32_t st = 0u;
    int dto_rc = sdio_wait_dto(us_timer_now(), SDIO_DAT_TIMEOUT_US, &st);
    int stop_rc = sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
    sdio_reset_fifo();
    /* CMD12 is R1b and its response can be lost while the card streams: the state is checked (CMD13, cheap) before
     * the next data command instead of assuming tran. */
    card->need_status = 1u;
    if (dto_rc != 0 || stop_rc != 0 || (st & SDIO_INT_DATA_ERRS)) {
        return -1;
    }
    return 0;
}

static int sdio_write_one_sector(sd_card_t *card, uint32_t lba, const uint8_t *src)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD24_WRITE1, arg, 0) != 0) {
        sdio_log_err("SDW_1_CMD", lba, s_last_cmd_st);
        return -1;
    }

    uint64_t t0 = us_timer_now();
    if (sdio_fifo_write_words(src, 128u, t0, SDIO_DAT_TIMEOUT_US) != 0) {
        sdio_reset_fifo();
        return -1;
    }

    uint32_t st = 0u;
    int dto_rc = sdio_wait_dto(us_timer_now(), SDIO_DAT_TIMEOUT_US, &st);
    if (dto_rc != 0 || (st & SDIO_INT_WR_DATA_ERRS)) {
        sdio_log_err("SDW_1_DTO", lba, dto_rc != 0 ? (st | 0x80000000u) : st);
        sdio_reset_fifo();
        return -1;
    }
    if (st & SDIO_INT_SBE) {
        card->wr_bci++;
    }
    return sdio_wait_card_ready(card);
}

static int sdio_write_multi(sd_card_t *card, uint32_t lba, uint32_t count, const uint8_t *src)
{
    uint32_t arg = card->is_sdhc ? lba : (lba << 9);

    sdio_reset_fifo();
    openfw_mmio_wr32(SDIO_BLKSIZ_ADDR,  512u);
    openfw_mmio_wr32(SDIO_BYTCNT_ADDR,  count * 512u);
    openfw_mmio_wr32(SDIO_RINTSTS_ADDR, 0xFFFFFFFFu);

    if (sdio_send_cmd(SD_CMD25_WRITE_MULTI, arg, 0) != 0) {
        sdio_log_err("SDW_CMD25", lba, count);
        return -1;
    }

    uint32_t xfer_timeout = SDIO_DAT_TIMEOUT_US + (count * 50000u);
    uint64_t t0 = us_timer_now();
    if (sdio_fifo_write_words(src, count * 128u, t0, xfer_timeout) != 0) {
        (void)sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
        sdio_reset_fifo();
        card->need_status = 1u;
        return -1;
    }

    uint32_t st = 0u;
    int dto_rc = sdio_wait_dto(us_timer_now(), SDIO_DAT_TIMEOUT_US, &st);
    int stop_rc = sdio_send_cmd(SD_CMD12_STOP, 0u, 1);
    if (dto_rc != 0 || stop_rc != 0 || (st & SDIO_INT_WR_DATA_ERRS)) {
        sdio_log_err("SDW_MDTO", st, ((uint32_t)(dto_rc != 0) << 16) | (uint32_t)(stop_rc != 0));
        sdio_reset_fifo();
        return -1;
    }
    if (st & SDIO_INT_SBE) {
        card->wr_bci++;
    }
    return sdio_wait_card_ready(card);
}

/* Full in-place re-initialisation (controller reset, CMD0 .. CMD7, clock probe) for the write retry path. Keeps the
 * latched single-block mode: a card that stalled on CMD25 once keeps using CMD24. */
static int sdio_reinit(sd_card_t *card)
{
    uint8_t  nmw     = card->no_multi_write;
    uint8_t  reinits = card->reinits;
    uint16_t retries = card->wr_retries;
    uint32_t bci     = card->wr_bci;
    int rc = sd_init(card);
    card->no_multi_write = nmw;
    card->reinits        = (uint8_t)(reinits < 255u ? reinits + 1u : 255u);
    card->wr_retries     = retries;
    card->wr_bci         = bci;
    sdio_log_err("SD_REINIT", (uint32_t)rc, card->reinits);
    return rc;
}

/* One sector, retried: a failed attempt marks the card state unknown, the next attempt first brings the card back
 * to tran (sdio_recover_state); after SDIO_WR_SOFT_TRIES the card is re-initialised. Rewriting the same LBA with the
 * same data is idempotent, so a write that did reach the card before its status was lost costs nothing. */
static int sdio_write_one_retry(sd_card_t *card, uint32_t lba, const uint8_t *src)
{
    const uint32_t total = SDIO_WR_SOFT_TRIES + SDIO_WR_REINIT_TRIES;
    for (uint32_t attempt = 0u; attempt < total; attempt++) {
        wdt_feed();
        if (attempt > 0u) {
            card->need_status = 1u;
            if (card->wr_retries < 0xFFFFu) {
                card->wr_retries++;
            }
            sdio_log_err("SDW_RETRY", lba, attempt);
        }
        if (attempt == SDIO_WR_SOFT_TRIES && sdio_reinit(card) != 0) {
            break;
        }
        if (sdio_ensure_ready(card) != 0) {
            continue;
        }
        if (sdio_write_one_sector(card, lba, src) == 0) {
            return 0;
        }
        card->need_status = 1u;
    }
    sdio_log_err("SDW_1_FAIL", lba, 1u);
    return -1;
}

int sd_read_blocks(sd_card_t *card, uint32_t lba, uint32_t count, void *buf)
{
    if (!card || !card->initialized || (!buf && count > 0u)) {
        return -1;
    }
    if (card->sector_count > 0u &&
        ((uint64_t)lba + (uint64_t)count) > (uint64_t)card->sector_count) {
        return -1;
    }
    if (card->async_active) {
        sd_read_async_abort(card);
    }
    uint32_t safe_limit = (card->sector_count > 3u) ? (card->sector_count - 3u) : 0u;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0u;
    while (done < count) {
        wdt_feed();
        uint32_t rem = count - done;
        uint32_t cur_lba = lba + done;
        uint32_t max_multi = rem;
        if (card->sector_count > 3u) {
            if (cur_lba >= safe_limit) {
                max_multi = 0u;
            } else if ((uint64_t)cur_lba + (uint64_t)max_multi > (uint64_t)safe_limit) {
                max_multi = safe_limit - cur_lba;
            }
        }
        uint32_t chunk = 1u;
        if (max_multi >= 2u) {
            chunk = (max_multi > SDIO_MAX_MULTI_BLKS) ? SDIO_MAX_MULTI_BLKS : max_multi;
        }
        int rc = -1;
        for (uint32_t t = 0u; t < SDIO_RD_TRIES && rc != 0; t++) {
            if (t > 0u) {
                card->need_status = 1u;
            }
            if (sdio_ensure_ready(card) != 0) {
                continue;
            }
            rc = (chunk >= 2u) ? sdio_read_multi(card, cur_lba, chunk, dst + (done * 512u))
                               : sdio_read_one_sector(card, cur_lba, dst + (done * 512u));
            if (rc != 0) {
                card->need_status = 1u;
            }
        }
        if (rc != 0) {
            return -1;
        }
        done += chunk;
    }
    return 0;
}

int sd_write_blocks(sd_card_t *card, uint32_t lba, uint32_t count, const void *buf)
{
    if (!card || !card->initialized || (!buf && count > 0u)) {
        return -1;
    }
    if (card->sector_count > 0u &&
        ((uint64_t)lba + (uint64_t)count) > (uint64_t)card->sector_count) {
        return -1;
    }
    (void)sd_flush(card);
    uint32_t safe_limit = (card->sector_count > 3u) ? (card->sector_count - 3u) : 0u;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0u;
    while (done < count) {
        wdt_feed();
        if (!card->initialized) {
            return -1;   /* a failed re-initialisation in the retry path */
        }
        uint32_t rem = count - done;
        uint32_t cur_lba = lba + done;
        uint32_t max_multi = rem;
        if (card->sector_count > 3u) {
            if (cur_lba >= safe_limit) {
                max_multi = 0u;
            } else if ((uint64_t)cur_lba + (uint64_t)max_multi > (uint64_t)safe_limit) {
                max_multi = safe_limit - cur_lba;
            }
        }
        if (!card->no_multi_write && max_multi >= 2u) {
            uint32_t chunk = (max_multi > SDIO_MAX_MULTI_BLKS) ? SDIO_MAX_MULTI_BLKS : max_multi;
            if (sdio_ensure_ready(card) != 0 ||
                sdio_write_multi(card, cur_lba, chunk, src + (done * 512u)) != 0) {
                /* Some cards / SL6801 dw_mmc revisions stall on CMD25+CMD12;
                 * log once, latch single-block CMD24 mode, and rewrite the chunk sector by sector. */
                sdio_log_err("SDW_MFAIL", cur_lba, chunk);
                card->no_multi_write = 1u;
                card->need_status = 1u;
                for (uint32_t k = 0u; k < chunk; k++) {
                    if (sdio_write_one_retry(card, cur_lba + k, src + ((done + k) * 512u)) != 0) {
                        sdio_log_err("SDW_FB_FAIL", cur_lba + k, chunk);
                        return -1;
                    }
                }
            }
            done += chunk;
        } else {
            if (sdio_write_one_retry(card, cur_lba, src + (done * 512u)) != 0) {
                return -1;
            }
            done++;
        }
    }
    return 0;
}

static int sd_blk_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return sd_read_blocks((sd_card_t *)ctx, lba, count, buf);
}

static int sd_blk_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    return sd_write_blocks((sd_card_t *)ctx, lba, count, buf);
}

blkdev_t sd_as_blkdev(sd_card_t *card)
{
    blkdev_t dev;
    dev.read_sectors  = sd_blk_read;
    dev.write_sectors = sd_blk_write;
    dev.total_sectors = card ? card->sector_count : 0u;
    dev.ctx           = card;
    return dev;
}
